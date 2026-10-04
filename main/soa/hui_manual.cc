/**
 * @file hui_manual.cc
 * @brief HUI 主机命令手册: JSON 解析 + 带缓存的拉取。设计说明见 hui_manual.h。
 *
 * ⚠️ 实现须知 (踩过的坑, 不要改回去):
 *   1. 手册**只缓存在 RAM**, 不落 NVS (按需求)。HuiManual_Invalidate() 置一下
 *      失效位, 下一次 EnsureLoaded() 就会重新 read 1000 + HTTP 拉一次。
 *   2. 拉取失败后有一条 COOLDOWN 冷却: MCP 工具可能被 AI 连着调好几次, 没有冷却
 *      的话每次都会卡一次 HTTP 超时。冷却期内直接返回错误, 不联网。
 *   3. LoadLocked() 在锁里做 HTTP (会阻塞几百 ms ~ 几秒)。调用方是 MCP 工具回调
 *      (主任务) 或后台任务, 都是任务上下文; 但绝不能在 CAN 任务里调。
 *   4. 手册对象用 shared_ptr 交出去, 重载时旧对象仍被调用方持有 —— 不会
 *      use-after-free。
 */

#include "hui_manual.h"

#include "hui_rpc.h"

#include "assets.h"
#include "board.h"
#include "cjson_utils.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <esp_log.h>
#include <esp_timer.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>

#define TAG "HuiManual"

/* 都来自 main/Kconfig.projbuild → menu "SOA (Service-Oriented Architecture)"。
 * 这里给一份兜底默认值: 目标不支持 TWAI 时这些 CONFIG_ 不会被定义。 */
#ifndef CONFIG_HUI_MANUAL_URL
#define CONFIG_HUI_MANUAL_URL ""
#endif
#ifndef CONFIG_HUI_RPC_TIMEOUT_MS
#define CONFIG_HUI_RPC_TIMEOUT_MS 1500
#endif
/* read 1000 (取设备 ID) 的超时。与工具调用的 CONFIG_HUI_RPC_TIMEOUT_MS 分开:
 * 手册加载发生在启动路径上的一次性阻塞调用, 而车机上电顺序可能让中央 ECU 慢一拍,
 * 所以给 2s 余量 (实测应答 7~500ms)。 */
#ifndef CONFIG_HUI_MANUAL_READ_TIMEOUT_MS
#define CONFIG_HUI_MANUAL_READ_TIMEOUT_MS 2000
#endif
#ifndef CONFIG_HUI_MANUAL_TIMEOUT_MS
#define CONFIG_HUI_MANUAL_TIMEOUT_MS 3000
#endif
#ifndef CONFIG_HUI_MANUAL_MAX_BYTES
#define CONFIG_HUI_MANUAL_MAX_BYTES 8192
#endif
/* 手册来源 (见 main/Kconfig.projbuild 的 HUI_MANUAL_SOURCE choice)。
 * 没定义说明选了 FLASH_FIRST (默认)。 */
#ifndef CONFIG_HUI_MANUAL_SOURCE_FLASH_ONLY
#define CONFIG_HUI_MANUAL_SOURCE_FLASH_ONLY 0
#endif
#ifndef CONFIG_HUI_MANUAL_SOURCE_HTTP_ONLY
#define CONFIG_HUI_MANUAL_SOURCE_HTTP_ONLY 0
#endif

/* 拉取失败后的冷却时间, 避免 AI 连续调用时反复卡在 HTTP 超时上 */
#define HUI_MANUAL_COOLDOWN_MS 3000

/* 是否读 flash / 是否禁止联网 */
static constexpr bool kUseFlash  = (CONFIG_HUI_MANUAL_SOURCE_HTTP_ONLY == 0);
static constexpr bool kFlashOnly = (CONFIG_HUI_MANUAL_SOURCE_FLASH_ONLY == 1);

/* 手册文件在 assets 镜像里的名字长度上限 —— build_default_assets.py 的
 * pack_assets_simple() 用固定宽度字段存名字, 超了会被截断 (名字就变了)。 */
#define HUI_MANUAL_ASSET_NAME_MAX 32u

static std::shared_ptr<const HuiManual> s_manual; /* RAM 缓存, 不落 NVS */
static std::string                      s_device_id;
static bool                             s_valid = false;
static int64_t                          s_last_try_us = 0;

static SemaphoreHandle_t Lock()
{
    static SemaphoreHandle_t lock = xSemaphoreCreateMutex();
    return lock;
}

static void SetError(std::string* error, const std::string& message)
{
    if (error != nullptr)
    {
        *error = message;
    }
}

/* ============================== 基础工具 ============================== */

static int HexDigit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool HuiManual_ParseCommandId(const std::string& text, uint16_t* out, std::string* error)
{
    if (out == nullptr)
    {
        return false;
    }

    std::string digits = text;
    if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X'))
    {
        digits = digits.substr(2);
    }
    if (digits.empty() || digits.size() > 4)
    {
        SetError(error, "命令号必须是 1~4 位十六进制 (例如 \"ff01\"), 收到: \"" + text + "\"");
        return false;
    }

    uint32_t value = 0;
    for (char c : digits)
    {
        const int digit = HexDigit(c);
        if (digit < 0)
        {
            SetError(error, "命令号里有非十六进制字符: \"" + text + "\"");
            return false;
        }
        value = (value << 4) | static_cast<uint32_t>(digit);
    }
    *out = static_cast<uint16_t>(value);
    return true;
}

bool HuiManual_ParseHexBytes(const std::string& text, size_t max_bytes,
                             std::vector<uint8_t>* out, std::string* error)
{
    if (out == nullptr)
    {
        return false;
    }
    out->clear();

    std::string digits;
    digits.reserve(text.size());
    bool expect_prefix = true; /* 允许 "0x" 前缀与分隔符 */
    for (char c : text)
    {
        if (c == ' ' || c == ',' || c == '-' || c == ':' || c == '\t' || c == '\n')
        {
            expect_prefix = true;
            continue;
        }
        if (c == '0' && expect_prefix)
        {
            /* 可能吃掉 "0x" 的 0; 先记下来, 看下一个字符 */
            digits.push_back('0');
            continue;
        }
        if ((c == 'x' || c == 'X') && expect_prefix && !digits.empty() && digits.back() == '0')
        {
            digits.pop_back();
            expect_prefix = false;
            continue;
        }
        if (HexDigit(c) < 0)
        {
            SetError(error, std::string("数据里有非十六进制字符: '") + c + "'");
            out->clear();
            return false;
        }
        digits.push_back(c);
        expect_prefix = false;
    }

    if (digits.empty())
    {
        return true; /* 空 = 没有参数 */
    }
    if (digits.size() % 2 != 0)
    {
        digits.insert(digits.begin(), '0'); /* "1" → "01", 对用户更友好 */
    }

    const size_t count = digits.size() / 2;
    if (count > max_bytes)
    {
        SetError(error, "数据太长: " + std::to_string(count) + " 字节, 最多 " +
                            std::to_string(max_bytes) + " 字节");
        return false;
    }

    out->reserve(count);
    for (size_t i = 0; i < count; i++)
    {
        const int hi = HexDigit(digits[i * 2]);
        const int lo = HexDigit(digits[i * 2 + 1]);
        out->push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return true;
}

std::string HuiManual_ToHex(const uint8_t* data, size_t len)
{
    static const char kHex[] = "0123456789ABCDEF";
    std::string text;
    if (data == nullptr)
    {
        return text;
    }
    text.reserve(len * 2);
    for (size_t i = 0; i < len; i++)
    {
        text.push_back(kHex[data[i] >> 4]);
        text.push_back(kHex[data[i] & 0x0F]);
    }
    return text;
}

/* ============================== 解码 ============================== */

/** @brief 大端读 n 字节无符号数; 字节不够返回 false。 */
static bool ReadBigEndian(const uint8_t* data, size_t len, size_t n, uint64_t* out)
{
    if (data == nullptr || n == 0 || len < n || n > 8)
    {
        return false;
    }
    uint64_t value = 0;
    for (size_t i = 0; i < n; i++)
    {
        value = (value << 8) | data[i];
    }
    *out = value;
    return true;
}

bool HuiManual_FieldRange(const std::string& type, int64_t* min_value, int64_t* max_value)
{
    if (min_value == nullptr || max_value == nullptr)
    {
        return false;
    }
    if (type == "u8")       { *min_value = 0;    *max_value = 255; }
    else if (type == "i8")  { *min_value = -128; *max_value = 127; }
    else if (type == "u16") { *min_value = 0;    *max_value = 65535; }
    else if (type == "i16") { *min_value = -32768; *max_value = 32767; }
    else if (type == "u32") { *min_value = 0;    *max_value = 4294967295LL; }
    else if (type == "i32") { *min_value = -2147483648LL; *max_value = 2147483647LL; }
    else
    {
        return false;
    }
    return true;
}

/** @brief 数值类型在线上占的字节数。 */
static size_t FieldWidth(const std::string& type)
{
    if (type == "u8" || type == "i8")
    {
        return 1;
    }
    if (type == "u16" || type == "i16")
    {
        return 2;
    }
    if (type == "u32" || type == "i32")
    {
        return 4;
    }
    return 0;
}

size_t HuiManual_FieldBytes(const HuiManualField& field)
{
    if (field.type == "bool")
    {
        return 1;
    }
    if (field.type == "hex" || field.type.empty())
    {
        return (field.bytes > 0) ? static_cast<size_t>(field.bytes) : 1u;
    }
    return FieldWidth(field.type);
}

bool HuiManual_EncodeField(const HuiManualField& field, const HuiManualArgValue& value,
                           size_t max_total_bytes, std::vector<uint8_t>* out, std::string* error)
{
    if (out == nullptr)
    {
        SetError(error, "输出参数为空");
        return false;
    }

    const std::string label = field.name.empty() ? std::string("参数") : ("参数 " + field.name);

    if (field.type == "bool")
    {
        bool boolean = false;
        if (value.is_bool)
        {
            boolean = value.boolean;
        }
        else if (value.is_int)
        {
            boolean = (value.integer != 0);
        }
        else
        {
            SetError(error, label + " 需要布尔值 (true/false)");
            return false;
        }
        if (out->size() + 1 > max_total_bytes)
        {
            SetError(error, label + " 放不下: payload 上限 " + std::to_string(max_total_bytes) +
                                " 字节");
            return false;
        }
        out->push_back(boolean ? 1u : 0u);
        return true;
    }

    if (field.type == "hex" || field.type.empty())
    {
        if (!value.is_text)
        {
            SetError(error, label + " 需要十六进制字节串 (如 \"0102\")");
            return false;
        }
        std::vector<uint8_t> bytes;
        std::string          parse_error;
        const size_t remaining = (max_total_bytes > out->size()) ? (max_total_bytes - out->size()) : 0u;
        const size_t limit = (field.bytes > 0 && static_cast<size_t>(field.bytes) < remaining)
                                 ? static_cast<size_t>(field.bytes)
                                 : remaining;
        if (!HuiManual_ParseHexBytes(value.text, limit, &bytes, &parse_error))
        {
            SetError(error, label + ": " + parse_error);
            return false;
        }
        if (field.bytes > 0 && bytes.size() != static_cast<size_t>(field.bytes))
        {
            SetError(error, label + " 需要恰好 " + std::to_string(field.bytes) + " 字节, 收到 " +
                                std::to_string(bytes.size()) + " 字节");
            return false;
        }
        if (out->size() + bytes.size() > max_total_bytes)
        {
            SetError(error, label + " 放不下: payload 上限 " + std::to_string(max_total_bytes) +
                                " 字节");
            return false;
        }
        out->insert(out->end(), bytes.begin(), bytes.end());
        return true;
    }

    int64_t min_value = 0;
    int64_t max_value = 0;
    if (!HuiManual_FieldRange(field.type, &min_value, &max_value))
    {
        SetError(error, "手册里未知的字段类型: \"" + field.type + "\"");
        return false;
    }
    if (!value.is_int)
    {
        SetError(error, label + " 需要整数 (" + field.type + ")");
        return false;
    }
    if (value.integer < min_value || value.integer > max_value)
    {
        SetError(error, label + " 超出 " + field.type + " 范围 [" + std::to_string(min_value) +
                            ", " + std::to_string(max_value) + "], 收到 " +
                            std::to_string(value.integer));
        return false;
    }

    const size_t width = FieldWidth(field.type);
    if (width == 0 || out->size() + width > max_total_bytes)
    {
        SetError(error, label + " 放不下: payload 上限 " + std::to_string(max_total_bytes) +
                            " 字节");
        return false;
    }
    const uint64_t raw = static_cast<uint64_t>(value.integer); /* 补码, 负数也按大端写 */
    for (size_t i = 0; i < width; i++)
    {
        out->push_back(static_cast<uint8_t>((raw >> (8u * (width - 1u - i))) & 0xFFu));
    }
    return true;
}

/** @brief 带 scale/offset 的数值 → 文本 (整数就不打小数点)。 */
static std::string FormatScaled(double value)
{
    char buf[32];
    if (value == static_cast<double>(static_cast<long long>(value)))
    {
        snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(value));
    }
    else
    {
        snprintf(buf, sizeof(buf), "%.3f", value);
    }
    return std::string(buf);
}

HuiManualDecoded HuiManual_Decode(const HuiManualField& field, const uint8_t* data, size_t len)
{
    HuiManualDecoded out;
    const std::string& type = field.type;

    if (data == nullptr || len == 0)
    {
        out.error = "主机没有返回数据";
        return out;
    }

    if (type == "hex" || type.empty())
    {
        const size_t n = (field.bytes > 0 && static_cast<size_t>(field.bytes) <= len)
                             ? static_cast<size_t>(field.bytes)
                             : len;
        out.ok      = true;
        out.numeric = false;
        out.text    = HuiManual_ToHex(data, n);
        return out;
    }

    if (type == "bool")
    {
        out.ok      = true;
        out.numeric = true;
        out.number  = (data[0] != 0) ? 1.0 : 0.0;
        out.text    = (data[0] != 0) ? "true" : "false";
        return out;
    }

    size_t width = 0;
    bool   sign = false;
    if (type == "u8")       { width = 1; }
    else if (type == "i8")  { width = 1; sign = true; }
    else if (type == "u16") { width = 2; }
    else if (type == "i16") { width = 2; sign = true; }
    else if (type == "u32") { width = 4; }
    else if (type == "i32") { width = 4; sign = true; }
    else
    {
        out.error = "手册里未知的字段类型: \"" + type + "\"";
        return out;
    }

    uint64_t raw = 0;
    if (!ReadBigEndian(data, len, width, &raw))
    {
        out.error = "数据长度不足: " + type + " 需要 " + std::to_string(width) + " 字节, 实际 " +
                    std::to_string(len);
        return out;
    }

    double value = static_cast<double>(raw);
    if (sign)
    {
        const uint64_t sign_bit = 1ull << (width * 8 - 1);
        if (raw & sign_bit)
        {
            value = static_cast<double>(raw) - static_cast<double>(sign_bit << 1);
        }
    }
    value = value * field.scale + field.offset;

    out.ok      = true;
    out.numeric = true;
    out.number  = value;
    out.text    = FormatScaled(value);
    return out;
}

/* ============================== JSON 解析 ============================== */

static std::string JsonString(const cJSON* object, const char* key)
{
    const cJSON* item = cJSON_GetObjectItem(object, key);
    return cJSON_IsString(item) ? std::string(item->valuestring) : std::string();
}

static double JsonNumber(const cJSON* object, const char* key, double fallback)
{
    const cJSON* item = cJSON_GetObjectItem(object, key);
    return cJSON_IsNumber(item) ? item->valuedouble : fallback;
}

static HuiManualField ParseField(const cJSON* item)
{
    HuiManualField field;
    field.name        = JsonString(item, "name");
    field.type        = JsonString(item, "type");
    field.description = JsonString(item, "description");
    field.unit        = JsonString(item, "unit");
    field.scale       = JsonNumber(item, "scale", 1.0);
    field.offset      = JsonNumber(item, "offset", 0.0);
    field.bytes       = static_cast<int>(JsonNumber(item, "bytes", 0.0));
    if (field.type.empty())
    {
        field.type = "hex"; /* 没写类型就当原始字节, 至少不会解错 */
    }
    return field;
}

static HuiManualField ParseArg(const cJSON* item)
{
    HuiManualField field = ParseField(item);
    /* 入参不做 scale/offset 换算 (见 HuiManual_EncodeField), 所以声明了也没用 ——
       直接告警, 免得手册作者以为写上去就生效了 */
    if (field.scale != 1.0 || field.offset != 0.0)
    {
        ESP_LOGW(TAG, "入参 \"%s\" 声明了 scale/offset, 但不参与编码; 请把量化写进 unit (如 \"0.1 km/h\")",
                 field.name.c_str());
    }
    return field;
}

/** @brief 工具名/命名空间只允许 [A-Za-z0-9_.-], 其它字符换成 '_'。 */
static std::string SanitizeAlias(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    for (unsigned char c : text)
    {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        c == '_';
        out.push_back(ok ? static_cast<char>(c) : '_');
    }
    return out;
}

static bool ParseCommand(const cJSON* item, HuiManualCommand* out, std::string* error)
{
    const std::string id_text = JsonString(item, "id");
    if (id_text.empty())
    {
        SetError(error, "命令缺少 id");
        return false;
    }
    if (!HuiManual_ParseCommandId(id_text, &out->id, error))
    {
        return false;
    }

    out->kind = JsonString(item, "kind");
    if (out->kind.empty())
    {
        out->kind = "read";
    }
    if (out->kind != "read" && out->kind != "write" && out->kind != "ioctl")
    {
        SetError(error, "命令 " + id_text + " 的 kind 非法: \"" + out->kind +
                            "\" (只支持 read/write/ioctl)");
        return false;
    }

    out->name        = JsonString(item, "name");
    out->description = JsonString(item, "description");
    if (out->description.empty())
    {
        out->description = out->name.empty() ? ("命令 " + id_text) : out->name;
    }

    const cJSON* value = cJSON_GetObjectItem(item, "value");
    if (cJSON_IsObject(value))
    {
        out->value = ParseField(value);
    }

    const cJSON* args = cJSON_GetObjectItem(item, "args");
    if (cJSON_IsArray(args))
    {
        const cJSON* arg = nullptr;
        cJSON_ArrayForEach(arg, args)
        {
            if (cJSON_IsObject(arg))
            {
                out->args.push_back(ParseArg(arg));
            }
        }
    }
    return true;
}

bool HuiManual_ParseJson(const std::string& json, HuiManual* out, std::string* error)
{
    if (out == nullptr)
    {
        SetError(error, "输出参数为空");
        return false;
    }

    CJsonUniquePtr root(cJSON_Parse(json.c_str()));
    if (root == nullptr)
    {
        SetError(error, "手册不是合法 JSON");
        return false;
    }

    const cJSON* commands = cJSON_GetObjectItem(root.get(), "commands");
    if (!cJSON_IsArray(commands))
    {
        SetError(error, "手册缺少 commands 数组");
        return false;
    }

    out->device_id   = JsonString(root.get(), "device_id");
    out->alias        = SanitizeAlias(JsonString(root.get(), "alias"));
    out->name        = JsonString(root.get(), "name");
    out->description = JsonString(root.get(), "description");
    if (out->name.empty())
    {
        out->name = "未知主机";
    }
    out->commands.clear();

    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, commands)
    {
        HuiManualCommand command;
        std::string      command_error;
        if (!cJSON_IsObject(item))
        {
            continue;
        }
        if (!ParseCommand(item, &command, &command_error))
        {
            /* 单条坏命令不该让整本手册作废 —— 跳过并告警 */
            ESP_LOGW(TAG, "跳过一条命令: %s", command_error.c_str());
            continue;
        }
        out->commands.push_back(std::move(command));
    }

    if (out->commands.empty())
    {
        SetError(error, "手册里没有一条可用命令");
        return false;
    }
    return true;
}

const HuiManualCommand* HuiManual::Find(const char* kind, uint16_t id) const
{
    for (const auto& command : commands)
    {
        if (command.id == id && (kind == nullptr || command.kind == kind))
        {
            return &command;
        }
    }
    return nullptr;
}

/* ============================== 拉取 ============================== */

/** @brief read 1000 → 2 字节大端设备 ID。ID 变化时把手册缓存作废。 */
static bool FetchDeviceId(std::string* error)
{
    HuiRpcResult result = HuiRpc_Read(HUI_CMD_GET_DEVICE_ID, CONFIG_HUI_MANUAL_READ_TIMEOUT_MS);
    if (!result.sent)
    {
        SetError(error, "读设备 ID 的请求没有发出去 (SOA/CAN 还没就绪?)");
        return false;
    }
    if (!result.replied)
    {
        SetError(error, "read 1000 超时: 中央 ECU 没有应答");
        return false;
    }
    if (result.error != 0)
    {
        SetError(error, std::string("read 1000 被拒绝: ") +
                            HuiRpc_ErrorText(HuiRpcKind_Read, result.error) + " (" +
                            std::to_string(result.error) + ")");
        return false;
    }
    if (result.len != 2)
    {
        SetError(error, "设备 ID 长度非法: 期待 2 字节, 实际 " + std::to_string(result.len) +
                            " 字节");
        return false;
    }

    char id_text[8];
    snprintf(id_text, sizeof(id_text), "%02X%02X", result.data[0], result.data[1]);
    if (s_device_id != id_text)
    {
        ESP_LOGI(TAG, "主机设备 ID: %s", id_text);
        s_device_id = id_text;
        s_valid     = false; /* 换了主机 ⇒ 旧手册作废 */
    }
    return true;
}

/** @brief 组 URL: 模板里有 %s 就替换, 没有就追加 ?device_id=。 */
static std::string BuildUrl(const std::string& device_id)
{
    std::string url = CONFIG_HUI_MANUAL_URL;
    if (url.empty())
    {
        return {};
    }

    const std::string marker = "%s";
    const size_t      pos    = url.find(marker);
    if (pos != std::string::npos)
    {
        url.replace(pos, marker.size(), device_id);
    }
    else
    {
        url += (url.find('?') == std::string::npos) ? "?device_id=" : "&device_id=";
        url += device_id;
    }
    return url;
}

static bool HttpGet(const std::string& url, std::string* body, std::string* error)
{
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr)
    {
        SetError(error, "网络还没初始化");
        return false;
    }

    auto http = network->CreateHttp(0);
    if (http == nullptr)
    {
        SetError(error, "创建 HTTP 客户端失败");
        return false;
    }

    http->SetTimeout(CONFIG_HUI_MANUAL_TIMEOUT_MS);
    http->SetKeepAlive(false);
    http->SetHeader("Accept", "application/json");

    if (auto opened = http->Open("GET", url); !opened)
    {
        SetError(error, "打开 URL 失败: " + opened.error().ToString());
        return false;
    }

    auto status = http->GetStatusCode();
    if (!status)
    {
        SetError(error, "读 HTTP 状态码失败: " + status.error().ToString());
        http->Close();
        return false;
    }
    if (*status != 200)
    {
        SetError(error, "HTTP " + std::to_string(*status) +
                            " (手册服务返回非 200, 检查 URL / 路由)");
        http->Close();
        return false;
    }

    *body = http->ReadAll();
    http->Close();

    if (body->empty())
    {
        SetError(error, "手册服务返回了空响应");
        return false;
    }
    if (body->size() > CONFIG_HUI_MANUAL_MAX_BYTES)
    {
        SetError(error, "手册过大: " + std::to_string(body->size()) + " 字节 (上限 " +
                            std::to_string(CONFIG_HUI_MANUAL_MAX_BYTES) + ")");
        body->clear();
        return false;
    }
    return true;
}

/** @brief 设备 ID 归一化 (大写、去 0x、补足 4 位), 用于把手册里的 id 与主机上报的比对。 */
static std::string NormalizeDeviceId(const std::string& text)
{
    std::string digits = text;
    /* 去空白 */
    size_t begin = digits.find_first_not_of(" \t");
    if (begin == std::string::npos)
    {
        return std::string();
    }
    digits = digits.substr(begin, digits.find_last_not_of(" \t") - begin + 1);
    /* 大写 + 去 0x */
    for (char& c : digits)
    {
        c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    }
    if (digits.size() > 2 && digits[0] == '0' && digits[1] == 'X')
    {
        digits = digits.substr(2);
    }
    while (digits.size() < 4)
    {
        digits.insert(digits.begin(), '0');
    }
    return digits;
}

/* ========================== 来源 1: flash (assets 分区) ========================== */

/**
 * @brief 候选手册文件名: index.json 里的 extra_files + 约定的 <设备ID>.json。
 *
 * 打包链路: main/CMakeLists.txt 把 soa/manuals 交给 build_default_assets.py 的
 * --extra_files, 每个 json 变成 assets 镜像里的一个条目 (名字 = 文件名 basename),
 * 并把文件名列表写进 index.json 的 extra_files。这里两种都支持:
 *   - 用了 --extra_files → 能从 index.json 枚举到任何文件名
 *   - 没用 (例如手工塞进 assets 目录) → 按约定文件名 <设备ID>.json 找
 */
static std::vector<std::string> FlashManualCandidates(void)
{
    std::vector<std::string> names;

    void*  ptr  = nullptr;
    size_t size = 0;
    if (Assets::GetInstance().GetAssetData("index.json", ptr, size))
    {
        CJsonUniquePtr root(cJSON_ParseWithLength(static_cast<const char*>(ptr), size));
        if (root == nullptr)
        {
            ESP_LOGW(TAG, "assets 里的 index.json 解析失败");
        }
        else
        {
            const cJSON* extra = cJSON_GetObjectItem(root.get(), "extra_files");
            const cJSON* item  = nullptr;
            cJSON_ArrayForEach(item, extra)
            {
                if (cJSON_IsString(item) && item->valuestring[0] != '\0')
                {
                    names.emplace_back(item->valuestring);
                }
            }
        }
    }
    else
    {
        ESP_LOGD(TAG, "assets 里没有 index.json");
    }

    names.push_back(s_device_id + ".json");
    return names;
}

/**
 * @brief 读一个 assets 条目并解析成手册。
 * @return false 时: error 非空 = 这个文件坏了; error 空 = 根本没这个条目
 *
 * ⚠️ Assets::GetAssetData() 返回的是 mmap 指针, **不是 NUL 结尾的字符串** ——
 *    必须带上长度 (这里先拷成 std::string), 不能当 const char* 用。
 */
static bool LoadFlashManualByName(const std::string& name, HuiManual* out, std::string* error)
{
    if (name.size() > HUI_MANUAL_ASSET_NAME_MAX)
    {
        SetError(error, name + ": 文件名超过 " + std::to_string(HUI_MANUAL_ASSET_NAME_MAX) +
                            " 字节, 打包时会被截断 (换个短名字)");
        return false;
    }

    void*  ptr  = nullptr;
    size_t size = 0;
    if (!Assets::GetInstance().GetAssetData(name, ptr, size) || ptr == nullptr || size == 0)
    {
        return false; /* 没这个条目, 不是错误 */
    }
    if (size > CONFIG_HUI_MANUAL_MAX_BYTES)
    {
        SetError(error, name + ": " + std::to_string(size) + " 字节, 超过上限 " +
                            std::to_string(CONFIG_HUI_MANUAL_MAX_BYTES));
        return false;
    }

    const std::string json(static_cast<const char*>(ptr), size);
    std::string       parse_error;
    if (!HuiManual_ParseJson(json, out, &parse_error))
    {
        SetError(error, name + ": " + parse_error);
        return false;
    }
    return true;
}

/** @brief 按设备 ID 在 flash 里找手册。 */
static bool LoadFromFlash(std::string* error)
{
    if (!Assets::GetInstance().partition_valid())
    {
        SetError(error, "assets 分区不可用 (没烧录 / 校验和错误)");
        return false;
    }

    const std::string wanted = NormalizeDeviceId(s_device_id);
    bool              saw_manual = false;

    for (const auto& name : FlashManualCandidates())
    {
        std::string parse_error;
        HuiManual   manual;
        if (!LoadFlashManualByName(name, &manual, &parse_error))
        {
            if (!parse_error.empty())
            {
                ESP_LOGW(TAG, "flash 里的 %s 不能用: %s", name.c_str(), parse_error.c_str());
                saw_manual = true; /* 有这个文件, 只是坏了 */
            }
            continue;
        }

        if (NormalizeDeviceId(manual.device_id) != wanted)
        {
            ESP_LOGD(TAG, "flash 里的 %s 是 device_id=%s, 不是本机 (%s)", name.c_str(),
                     manual.device_id.c_str(), s_device_id.c_str());
            saw_manual = true;
            continue;
        }

        manual.device_id = s_device_id;
        ESP_LOGI(TAG, "命令手册取自 flash: %s (device_id=%s), %u 条命令, 文件 %s",
                 manual.name.c_str(), manual.device_id.c_str(), (unsigned)manual.commands.size(),
                 name.c_str());
        s_manual = std::make_shared<HuiManual>(std::move(manual));
        s_valid  = true;
        return true;
    }

    SetError(error, saw_manual ? ("flash 里有手册, 但没有 device_id=" + s_device_id + " 的那份")
                               : "flash 里没有打包任何命令手册 (main/soa/manuals)");
    return false;
}

/** @brief 调用方必须已经拿到 Lock()。allow_http=false 时只读 flash（启动早期用）。 */
static bool LoadLocked(std::string* error, bool allow_http)
{
    if (s_valid && s_manual != nullptr)
    {
        return true;
    }

    /* 1. 先问清楚对面是谁 —— 两个来源都要用设备 ID 定位 */
    if (!FetchDeviceId(error))
    {
        /* 带上这一句, 否则串口上只能看到一个光秃的 read 1000 超时, 分不清是 CAN 没通
         * 还是主机没实现这条通用命令 */
        ESP_LOGW(TAG, "读设备 ID (read 1000) 失败: %s",
                 error != nullptr ? error->c_str() : "");
        return false;
    }

    /* 2. flash 里的手册 (assets 分区, 不联网) */
    std::string flash_error;
    if (kUseFlash)
    {
        if (LoadFromFlash(&flash_error))
        {
            return true;
        }
        ESP_LOGI(TAG, "flash 里没有可用手册: %s", flash_error.c_str());
    }

    if (kFlashOnly || !allow_http)
    {
        SetError(error, flash_error.empty()
                            ? std::string("只允许用 flash 里的手册")
                            : (flash_error +
                               (kFlashOnly ? "" : " (本次只试 flash, 不联网)")));
        return false;
    }

    /* 3. HTTP 拉取 (带冷却: MCP 工具可能被 AI 连着调, 没冷却会反复卡超时) */
    const int64_t now_us = esp_timer_get_time();
    if (s_last_try_us != 0 &&
        (now_us - s_last_try_us) < static_cast<int64_t>(HUI_MANUAL_COOLDOWN_MS) * 1000)
    {
        SetError(error, "手册服务刚刚失败过, 冷却中" +
                            (flash_error.empty() ? std::string() : (" (" + flash_error + ")")));
        return false;
    }
    s_last_try_us = now_us;

    const std::string url = BuildUrl(s_device_id);
    if (url.empty())
    {
        SetError(error, flash_error.empty()
                            ? std::string("没有配置手册服务地址 (CONFIG_HUI_MANUAL_URL 为空)")
                            : (flash_error + ", 而且没有配置手册服务地址"));
        return false;
    }

    std::string body;
    ESP_LOGI(TAG, "拉取命令手册: %s", url.c_str());
    if (!HttpGet(url, &body, error))
    {
        ESP_LOGW(TAG, "拉取命令手册失败: %s", error != nullptr ? error->c_str() : "");
        return false;
    }

    /* 4. 解析 */
    auto manual = std::make_shared<HuiManual>();
    if (!HuiManual_ParseJson(body, manual.get(), error))
    {
        return false;
    }
    if (!manual->device_id.empty() && manual->device_id != s_device_id)
    {
        ESP_LOGW(TAG, "手册里的 device_id(%s) 与主机上报的(%s) 不一致, 以上报的为准",
                 manual->device_id.c_str(), s_device_id.c_str());
    }
    manual->device_id = s_device_id;

    ESP_LOGI(TAG, "命令手册已从服务端加载: %s (device_id=%s), %u 条命令, %u 字节",
             manual->name.c_str(), manual->device_id.c_str(), (unsigned)manual->commands.size(),
             (unsigned)body.size());

    s_manual = manual;
    s_valid  = true;
    return true;
}

/* ============================== 对外接口 ============================== */

std::shared_ptr<const HuiManual> HuiManual_EnsureLoaded(std::string* error)
{
    if (error != nullptr)
    {
        error->clear();
    }

    SemaphoreHandle_t lock = Lock();
    if (lock == nullptr)
    {
        SetError(error, "创建互斥量失败");
        return nullptr;
    }

    xSemaphoreTake(lock, portMAX_DELAY);
    const bool  ok     = LoadLocked(error, true);
    std::shared_ptr<const HuiManual> manual = ok ? s_manual : nullptr;
    xSemaphoreGive(lock);
    return manual;
}

std::shared_ptr<const HuiManual> HuiManual_TryLoadLocal(std::string* error)
{
    if (error != nullptr)
    {
        error->clear();
    }

    SemaphoreHandle_t lock = Lock();
    if (lock == nullptr)
    {
        SetError(error, "创建互斥量失败");
        return nullptr;
    }

    xSemaphoreTake(lock, portMAX_DELAY);
    const bool  ok     = LoadLocked(error, false);
    std::shared_ptr<const HuiManual> manual = ok ? s_manual : nullptr;
    xSemaphoreGive(lock);
    return manual;
}

std::shared_ptr<const HuiManual> HuiManual_Cached(std::string* error)
{
    if (error != nullptr)
    {
        error->clear();
    }

    SemaphoreHandle_t lock = Lock();
    if (lock == nullptr)
    {
        SetError(error, "创建互斥量失败");
        return nullptr;
    }

    xSemaphoreTake(lock, portMAX_DELAY);
    std::shared_ptr<const HuiManual> manual = (s_valid && s_manual != nullptr) ? s_manual : nullptr;
    xSemaphoreGive(lock);

    if (manual == nullptr)
    {
        SetError(error, "命令手册还没加载 (设备 ID 未知或手册服务不可达)");
    }
    return manual;
}

void HuiManual_Invalidate(void)
{
    SemaphoreHandle_t lock = Lock();
    if (lock == nullptr)
    {
        return;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    s_valid       = false;
    s_last_try_us = 0; /* 失效是显式动作, 不走冷却 */
    xSemaphoreGive(lock);
    ESP_LOGI(TAG, "命令手册已标记失效, 下次会重新拉取");
}
