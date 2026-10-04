/**
 * @file hui_mcp.cc
 * @brief HUI 命令手册 → MCP 工具。设计说明见 hui_mcp.h。
 *
 * ⚠️ 实现须知 (踩过的坑, 不要改回去):
 *   1. 只在 PrepareBeforeConnect() 里**同步**跑一次, 注册完不再碰 tools_。
 *      后端连上后约 190ms 就问 tools/list, 所以不能像早期版本那样丢给后台任务 ——
 *      那样第一次 tools/list 里就没有 self.hui.*, 而后端不理会
 *      notifications/tools/list_changed, AI 就永远看不到这些工具。
 *   2. 工具回调跑在主任务里 (McpServer::DoToolCall 走 Application::Schedule)。
 *      HUI 事务要等 CAN 应答, 按本工程既有做法 (self.camera.take_photo) 用
 *      TaskPriorityReset 把当前任务优先级降下来, 别让等待卡住别的实时任务。
 *   3. 工具回调只读**已加载**的手册 (HuiManual_Cached), 绝不在这里联网 ——
 *      否则一次 tools/call 会阻塞主任务好几秒。
 *   4. 描述总量压在 kMaxDescriptionBytes 以内: tools/list 单页上限是 8000 字节,
 *      一个工具的描述撑爆了会导致整页返回失败, 那就是 MCP 全瘫。
 */

#include "hui_mcp.h"

#include "hui.h"
#include "hui_manual.h"
#include "hui_rpc.h"

#include "application.h"
#include "cjson_utils.h"
#include "mcp_server.h"

#include <esp_log.h>

#include <cstdint>
#include <cstdio>
#include <optional>
#include <set>
#include <string>
#include <vector>

#define TAG "HuiMcp"

/* 工具调用 (tools/call) 等 CAN 应答的超时。注意手册加载那次 read 1000 用的是
 * hui_manual.cc 里的 CONFIG_HUI_MANUAL_READ_TIMEOUT_MS, 两者是不同的场景。 */
#ifndef CONFIG_HUI_RPC_TIMEOUT_MS
#define CONFIG_HUI_RPC_TIMEOUT_MS 1500
#endif

#define HUI_MCP_MAX_DESC_BYTES 5500u /* 单个工具描述上限, 见文件头第 4 条 */
#define HUI_MCP_WAIT_PRIORITY   1    /* 等 CAN 应答期间降到的优先级 */
#define HUI_MCP_TOOL_PREFIX     "self.hui."
/* 具体工具的数量预算: tools/list 单页 8000 字节, 一条工具(带 schema)约 150~350 字节,
 * 再扣掉工程原有的公共工具 (~1~2 KB), 24 条是安全值。
 * 调大它必须确认后端真的会跟 nextCursor 翻页, 否则第 2 页的工具 AI 根本看不到。 */
#define HUI_MCP_MAX_TOOLS 24u

static bool s_loaded = false;

/* ============================== 描述拼装 ============================== */

static std::string Hex4(uint16_t id)
{
    char text[8];
    snprintf(text, sizeof(text), "%04X", id);
    return std::string(text);
}

/** @brief 工具名只允许 [A-Za-z0-9_], 其它字符换成 '_'。 */
static std::string SanitizeSegment(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    for (unsigned char c : text)
    {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                        (c >= 'A' && c <= 'Z') || c == '_';
        out.push_back(ok ? static_cast<char>(c) : '_');
    }
    return out;
}

/** @brief 工具命名空间: 手册的 alias, 没写就用 host_<设备ID>。 */
static std::string HostNamespace(const HuiManual& manual)
{
    if (!manual.alias.empty())
    {
        return manual.alias;
    }
    return manual.device_id.empty() ? std::string("host") : ("host_" + manual.device_id);
}

/* ============================== 参数 → Schema ============================== */

/** @brief 在 PropertyList 里按名字找参数; 找不到返回 nullptr。
 *  (不用 PropertyList::operator[] —— 它找不到会直接 esp_system_abort) */
static const Property* FindProperty(const PropertyList& properties, const std::string& name)
{
    for (const auto& property : properties)
    {
        if (property.name() == name)
        {
            return &property;
        }
    }
    return nullptr;
}

static HuiManualArgValue ToArgValue(const Property& property)
{
    HuiManualArgValue value;
    switch (property.type())
    {
        case kPropertyTypeBoolean:
            value.is_bool = true;
            value.boolean = property.value<bool>();
            break;
        case kPropertyTypeInteger:
            value.is_int  = true;
            value.integer = property.value<int>();
            break;
        case kPropertyTypeString:
            value.is_text = true;
            value.text    = property.value<std::string>();
            break;
    }
    return value;
}

/** @brief 一个入参 → 一个 JSON Schema 属性 (类型/范围/长度都按手册声明)。 */
static std::optional<Property> MakeProperty(const HuiManualCommand& command,
                                            const HuiManualField& arg, std::string* error)
{
    if (arg.name.empty())
    {
        *error = "命令 " + Hex4(command.id) + " 有入参没有 name, 无法生成工具";
        return std::nullopt;
    }

    if (arg.type == "bool")
    {
        return Property(arg.name, kPropertyTypeBoolean);
    }

    if (arg.type == "hex")
    {
        const size_t bytes = (arg.bytes > 0) ? static_cast<size_t>(arg.bytes) : HUI_RPC_DATA_MAX;
        Property property(arg.name, kPropertyTypeString);
        property.SetMaxLength(bytes * 2);
        return property;
    }

    int64_t min_value = 0;
    int64_t max_value = 0;
    if (!HuiManual_FieldRange(arg.type, &min_value, &max_value))
    {
        *error = "命令 " + Hex4(command.id) + " 的入参 \"" + arg.name + "\" 类型未知: \"" +
                 arg.type + "\"";
        return std::nullopt;
    }
    /* Property 只能存 int32: u32 的上半段表达不了, 收窄并告警 */
    if (min_value < INT32_MIN || max_value > INT32_MAX)
    {
        ESP_LOGW(TAG, "命令 %s 的入参 %s 是 %s, 超出工具 schema 能表达的 int32 范围, 已收窄",
                 Hex4(command.id).c_str(), arg.name.c_str(), arg.type.c_str());
    }
    const int min_int = static_cast<int>(std::max<int64_t>(min_value, INT32_MIN));
    const int max_int = static_cast<int>(std::min<int64_t>(max_value, INT32_MAX));
    return Property(arg.name, kPropertyTypeInteger, min_int, max_int);
}

/** @brief 把手册的 args 变成工具的 JSON Schema。 */
static bool BuildProperties(const HuiManualCommand& command, PropertyList* list,
                            std::string* error)
{
    for (const auto& arg : command.args)
    {
        std::string property_error;
        const std::optional<Property> property = MakeProperty(command, arg, &property_error);
        if (!property.has_value())
        {
            *error = property_error;
            return false;
        }
        list->AddProperty(*property);
    }
    return true;
}

/** @brief 按手册的 args 说明把参数编码成 payload 字节。 */
static bool EncodeArgs(const HuiManualCommand& command, const PropertyList& properties,
                       size_t max_total_bytes, std::vector<uint8_t>* out, std::string* error)
{
    for (const auto& arg : command.args)
    {
        const Property* property = FindProperty(properties, arg.name);
        if (property == nullptr)
        {
            *error = "内部错误: 参数 \"" + arg.name + "\" 没有声明";
            return false;
        }
        if (!HuiManual_EncodeField(arg, ToArgValue(*property), max_total_bytes, out, error))
        {
            return false;
        }
    }
    return true;
}

/** @brief 具体工具的 description: 命令语义 + 参数 + 返回值。
 *
 *  入参的语义只能写在这里 —— 原版 mcpserver 的 Property 带不了 description, 而
 *  AI 正是靠这段文字决定填什么值, 所以参数名/类型/单位一个都不能省。 */
static std::string BuildToolDescription(const HuiManual& manual, const HuiManualCommand& command)
{
    std::string text = command.description;
    text += "（" + manual.name + " / device_id=" + manual.device_id + " / 命令 " +
            Hex4(command.id) + "）";

    if (!command.args.empty())
    {
        text += "\n参数: ";
        for (size_t i = 0; i < command.args.size(); i++)
        {
            const HuiManualField& arg = command.args[i];
            if (i > 0)
            {
                text += "；";
            }
            text += arg.name + "=" + arg.type;
            if (!arg.description.empty())
            {
                text += "(" + arg.description + ")";
            }
            else if (!arg.unit.empty())
            {
                text += "(单位: " + arg.unit + ")";
            }
        }
    }
    if (command.kind != "write" && !command.value.type.empty())
    {
        text += "\n返回: " + command.value.type;
        if (!command.value.unit.empty())
        {
            text += "，单位 " + command.value.unit;
        }
    }

    if (text.size() > HUI_MCP_MAX_DESC_BYTES)
    {
        text.resize(HUI_MCP_MAX_DESC_BYTES);
        text += "…";
    }
    return text;
}

/* ============================== 工具实现 ============================== */

/** @brief 执行一条手册命令。command 是**按值快照**传进来的。 */
static ToolResult RunCommand(const HuiManualCommand& command, const PropertyList& properties)
{
    const std::string id_text  = Hex4(command.id);
    const bool        is_ioctl = (command.kind == "ioctl");

    /* 1. 参数 → payload。ioctl 还要把 2 字节命令码放在最前面 */
    std::vector<uint8_t> payload;
    std::string          error;
    if (is_ioctl)
    {
        payload.push_back(static_cast<uint8_t>(command.id >> 8));
        payload.push_back(static_cast<uint8_t>(command.id & 0xFF));
    }
    if (!EncodeArgs(command, properties, HUI_RPC_DATA_MAX, &payload, &error))
    {
        return std::unexpected("命令 " + id_text + " 的参数不合法: " + error);
    }

    /* 2. 发下去等应答 (等的时候把优先级让出来) */
    TaskPriorityReset priority_reset(HUI_MCP_WAIT_PRIORITY);
    HuiRpcResult      result;
    HuiRpcKind        rpc_kind = HuiRpcKind_Read;
    if (command.kind == "read")
    {
        rpc_kind = HuiRpcKind_Read;
        result   = HuiRpc_Read(command.id, CONFIG_HUI_RPC_TIMEOUT_MS);
    }
    else if (is_ioctl)
    {
        rpc_kind = HuiRpcKind_Ioctl;
        result   = HuiRpc_Ioctl(payload.data(), payload.size(), CONFIG_HUI_RPC_TIMEOUT_MS);
    }
    else
    {
        rpc_kind = HuiRpcKind_Write;
        result   = HuiRpc_Write(command.id, payload.empty() ? nullptr : payload.data(),
                                payload.size(), CONFIG_HUI_RPC_TIMEOUT_MS);
    }

    if (!result.sent)
    {
        return std::unexpected("命令 " + id_text + " 没有发出去: HUI/SOA 还没就绪");
    }
    if (!result.replied)
    {
        return std::unexpected("命令 " + id_text + " 超时: 主机没有在超时时间内应答");
    }
    if (!result.ok)
    {
        return std::unexpected("主机拒绝命令 " + id_text + ": " +
                               HuiRpc_ErrorText(rpc_kind, result.error) + " (" +
                               std::to_string(result.error) + ")");
    }

    /* 3. 组返回值 */
    CJsonUniquePtr json(cJSON_CreateObject());
    if (json == nullptr)
    {
        return std::unexpected("内存不足, 无法构造应答");
    }
    cJSON_AddStringToObject(json.get(), "command", id_text.c_str());
    if (!command.name.empty())
    {
        cJSON_AddStringToObject(json.get(), "name", command.name.c_str());
    }
    cJSON_AddBoolToObject(json.get(), "ok", true);

    if (command.kind == "read" || result.len > 0)
    {
        cJSON_AddStringToObject(json.get(), "raw", HuiManual_ToHex(result.data, result.len).c_str());
    }
    /* 手册没声明 value 时就别再加一个和 raw 重复的 "value" (会被误当成有意义的量) */
    if (result.len > 0 && !command.value.type.empty())
    {
        const HuiManualDecoded decoded = HuiManual_Decode(command.value, result.data, result.len);
        if (decoded.ok)
        {
            if (decoded.numeric)
            {
                cJSON_AddNumberToObject(json.get(), "value", decoded.number);
            }
            else
            {
                cJSON_AddStringToObject(json.get(), "value", decoded.text.c_str());
            }
            if (!command.value.unit.empty())
            {
                cJSON_AddStringToObject(json.get(), "unit", command.value.unit.c_str());
            }
        }
        else if (!decoded.error.empty())
        {
            ESP_LOGW(TAG, "%s 解码失败: %s", id_text.c_str(), decoded.error.c_str());
            cJSON_AddStringToObject(json.get(), "decode_error", decoded.error.c_str());
        }
    }
    return json.release();
}

/* ============================== 注册 ============================== */

/** @brief 车辆级休眠/唤醒工具。
 *
 *  这两个不来自命令手册 (它们是 HUI 协议自带的整车休眠协调命令号), 所以
 *  **无条件注册** —— 手册没加载出来时它们也应该可用。 */
static void RegisterVehicleTools(void)
{
    auto& mcp = McpServer::GetInstance();

    mcp.AddTool("self.hui.vehicle.request_wake",
                "唤醒车辆主机 (整车休眠协调)。向中央 ECU 发一条 SlaveHeartbeat "
                "(mode=WAKING, wake_reason=USER_REQUEST), 由它决定何时真正唤醒。\n"
                "用户说“唤醒车辆 / 叫醒它 / 让它启动”时用这个。无条件发送。",
                PropertyList(), [](const PropertyList&) -> ToolResult {
                    if (!HuiNode_RequestHostWakeup())
                    {
                        return std::unexpected("唤醒请求没能发出去: 总线未就绪 (详见设备日志)");
                    }
                    return true;
                });

    mcp.AddTool("self.hui.vehicle.request_sleep",
                "请求车辆主机进入休眠 (整车休眠协调)。向中央 ECU 发一条 SlaveHeartbeat "
                "(mode=SLEEP_REQUESTED, sleep_reason=USER_REQUEST), 由它决定何时休眠。\n"
                "用户说“让车休息 / 我要走了 / 关掉车”时用这个。\n"
                "注意: 只有中央 ECU 当前处于 WORKING 模式时才会真正发出, 否则会失败。",
                PropertyList(), [](const PropertyList&) -> ToolResult {
                    if (!HuiNode_RequestHostSleep())
                    {
                        return std::unexpected(
                            "休眠请求未发出: 中央 ECU 当前不在 WORKING 模式, 或总线发送失败"
                            " (详见设备日志)");
                    }
                    return true;
                });

    ESP_LOGI(TAG, "已注册车辆休眠/唤醒工具: self.hui.vehicle.request_wake / request_sleep");
}

/** @brief 按手册把每条命令注册成一个 MCP 工具。只调一次。 */
static void RegisterManualTools(const HuiManual& manual)
{
    auto& mcp = McpServer::GetInstance();

    const std::string     prefix = std::string(HUI_MCP_TOOL_PREFIX) + HostNamespace(manual) + ".";
    std::set<std::string> used;
    size_t                registered = 0;
    size_t                skipped    = 0;

    for (size_t i = 0; i < manual.commands.size(); i++)
    {
        if (registered >= HUI_MCP_MAX_TOOLS)
        {
            skipped = manual.commands.size() - i;
            break;
        }

        const HuiManualCommand& command = manual.commands[i];

        std::string segment = SanitizeSegment(command.name);
        if (segment.empty())
        {
            segment = "cmd_" + Hex4(command.id);
        }

        std::string name = prefix + segment;
        if (!used.insert(name).second)
        {
            /* 手册里两条命令重名: 补命令号消歧 */
            name = prefix + segment + "_" + Hex4(command.id);
            if (!used.insert(name).second)
            {
                skipped++;
                continue;
            }
        }

        PropertyList properties;
        std::string  error;
        if (!BuildProperties(command, &properties, &error))
        {
            ESP_LOGW(TAG, "跳过命令 %s: %s", Hex4(command.id).c_str(), error.c_str());
            skipped++;
            continue;
        }

        /* 按值快照: 回调里不能留指向手册的指针/引用 (手册随时可能被重载替换) */
        HuiManualCommand snapshot    = command;
        std::string      description = BuildToolDescription(manual, command);
        mcp.AddTool(name, description, properties,
                    [snapshot](const PropertyList& args) -> ToolResult {
                        return RunCommand(snapshot, args);
                    });
        registered++;
    }

    ESP_LOGI(TAG, "已把 %s (device_id=%s) 的手册转成 %u 个 MCP 工具: %s*", manual.name.c_str(),
             manual.device_id.c_str(), (unsigned)registered, prefix.c_str());
    if (skipped > 0)
    {
        ESP_LOGW(TAG, "还有 %u 条命令没生成工具 (预算 %u 个)", (unsigned)skipped,
                 (unsigned)HUI_MCP_MAX_TOOLS);
    }
}

void HuiMcp_Init(void)
{
    if (s_loaded)
    {
        ESP_LOGW(TAG, "HuiMcp_Init() 重复调用, 忽略");
        return;
    }

    /* 车辆休眠/唤醒不依赖命令手册, 先注册 —— 手册读不到时它们也应该可用 */
    RegisterVehicleTools();

    /* read 1000 拿设备 ID → 从 flash 找 device_id 匹配的那份 JSON。
     * 用 TryLoadLocal 而不是 EnsureLoaded: 现在只走 flash, 不联网, 也不读退避冷却
     * (以后换成 HTTP 源时改回 EnsureLoaded 即可, 调用点不变)。 */
    std::string error;
    auto        manual = HuiManual_TryLoadLocal(&error);
    if (manual == nullptr)
    {
        /* 本次放弃 (按需求不重试): 后端第一次 tools/list 只会看到本机工具 */
        ESP_LOGW(TAG, "没拿到主机命令手册, 本次只注册本机工具: %s", error.c_str());
        s_loaded = true;
        return;
    }

    RegisterManualTools(*manual);
    s_loaded = true;
}
