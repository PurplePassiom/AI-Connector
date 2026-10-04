#ifndef HUI_MANUAL_H
#define HUI_MANUAL_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

/* ============================================================================
 * HUI 主机命令手册 (后端下发的 JSON) —— 模型 + 解析 + 带缓存的拉取。
 *
 * 手册解决的是"命令号无法自描述"的问题:
 *   HuiRead/Write/Ioctl 只能搬字节, 具体某条命令读什么、写什么、返回什么单位,
 *   只有主机的手册知道。流程是:
 *     read 1000 (通用命令) → 2 字节设备 ID → 拿 ID 去服务换手册
 *   → 手册转成 self.hui.read/write/ioctl 三个 MCP 工具的描述 → 喂给 AI。
 *
 * 缓存策略 (按需求): 只缓存在 RAM, **不写 NVS**; 失效之后下次再拉一次。
 *
 * ⚠️ 多字节数值一律**大端** (MSB first), 与 read 1000 的设备 ID 一致。
 * ========================================================================= */

/** @brief 通用命令: read 0x1000 → 设备 ID (2 字节大端, 例如 "0001") */
#define HUI_CMD_GET_DEVICE_ID 0x1000u

/** @brief 一条命令的参数, 或一个返回值字段的编码说明。 */
struct HuiManualField
{
    std::string name;
    std::string type;        /* u8 | i8 | u16 | i16 | u32 | i32 | bool | hex */
    std::string description; /* 人话描述, 也是给 AI 看的那份说明 */
    double      scale  = 1.0;
    double      offset = 0.0;
    std::string unit;
    int         bytes  = 0;  /* 仅 hex: 固定字节数, 0 = 用实际收到的长度 */
};

/** @brief 手册里的一条命令。 */
struct HuiManualCommand
{
    uint16_t    id = 0;   /* read/write = sub_service; ioctl = 2 字节命令码 */
    std::string kind;     /* read | write | ioctl */
    std::string name;     /* 语义名, 如 get_battery */
    std::string description;
    HuiManualField              value; /* read/ioctl 应答的编码方式 (可缺省) */
    std::vector<HuiManualField> args;  /* write/ioctl 的入参, 按 payload 顺序 */
};

/** @brief 一整本主机手册。 */
struct HuiManual
{
    std::string device_id; /* "0001", 与 read 1000 的结果一致 */
    std::string alias;     /* 工具名里的命名空间片段, 如 "host1"; 可缺省 */
    std::string name;
    std::string description;
    std::vector<HuiManualCommand> commands;

    /** @brief 按类型 + 命令号找一条命令; 找不到返回 nullptr。 */
    const HuiManualCommand* Find(const char* kind, uint16_t id) const;
};

/** @brief 给编码器用的"人给的值"。三选一, 按字段类型取用。 */
struct HuiManualArgValue
{
    bool        is_bool  = false;
    bool        boolean  = false;
    bool        is_int   = false;
    int64_t     integer  = 0;
    bool        is_text  = false;
    std::string text;
};

/** @brief 解码结果: numeric 为真时用 number, 否则用 text。 */
struct HuiManualDecoded
{
    bool        ok      = false;
    bool        numeric = false;
    double      number  = 0.0;
    std::string text;  /* hex / bool / 原始字节的可读形式 */
    std::string error;
};

/**
 * @brief 只从 flash 试一次 (不联网, 不读退避冷却)。
 *
 * 给启动早期的同步调用用: 后端连上来就会 tools/list, 而它不一定理会
 * notifications/tools/list_changed —— 手册在 flash 里的话就应该在第一次 tools/list
 * 之前就把工具注册好。
 */
std::shared_ptr<const HuiManual> HuiManual_TryLoadLocal(std::string* error);

/**
 * @brief 保证手册已加载。必要时会阻塞: read 1000 → flash 查找 → HTTP 拉取。
 * @param error 失败原因 (可为 nullptr)
 * @return 手册 (调用方持有 shared_ptr, 保证在多线程重载时不会被提前释放); 失败返回 nullptr
 */
std::shared_ptr<const HuiManual> HuiManual_EnsureLoaded(std::string* error);

/** @brief 只读当前缓存, 不联网。未加载时返回 nullptr 并把原因写进 error。 */
std::shared_ptr<const HuiManual> HuiManual_Cached(std::string* error);

/** @brief 标记失效: 下一次 EnsureLoaded (或后台任务) 会重新拉一次。 */
void HuiManual_Invalidate(void);

/** @brief 解析后端下发的 JSON 文档。单条坏命令会被跳过并告警, 不会整本失败。 */
bool HuiManual_ParseJson(const std::string& json, HuiManual* out, std::string* error);

/** @brief 按字段说明把原始字节解释成数值/文本。 */
HuiManualDecoded HuiManual_Decode(const HuiManualField& field, const uint8_t* data, size_t len);

/**
 * @brief 字段类型的数值范围 (同时决定了线上字节宽度: u8/i8=1, u16/i16=2, u32/i32=4)。
 * @return false = 类型不是数值型 (bool / hex / 未知)
 */
bool HuiManual_FieldRange(const std::string& type, int64_t* min_value, int64_t* max_value);

/**
 * @brief 把一个"人给的值"按字段说明编码成线上字节 (大端), 追加到 out。
 * @param max_total_bytes payload 总长上限 (write/ioctl 是 DSDL 的 8 字节)
 *
 * ⚠️ 参数的 scale/offset **不参与编码** —— 人工声明的小数换算交给大模型做不可靠,
 *    所以要求手册把量化写进 unit (如 "0.1 km/h"), 值本身按线上整数传。
 */
bool HuiManual_EncodeField(const HuiManualField& field, const HuiManualArgValue& value,
                           size_t max_total_bytes, std::vector<uint8_t>* out,
                           std::string* error);

/** @brief 拆一个命令在 payload 里占的字节数 (bool=1, 数值看宽度, hex 看 bytes)。 */
size_t HuiManual_FieldBytes(const HuiManualField& field);

/** @brief 把 1~4 位十六进制文本解析成命令号。 */
bool HuiManual_ParseCommandId(const std::string& text, uint16_t* out, std::string* error);

/** @brief 把十六进制文本解析成字节序列 (允许 "01 02" / "0x01,0x02" 这类写法)。 */
bool HuiManual_ParseHexBytes(const std::string& text, size_t max_bytes,
                             std::vector<uint8_t>* out, std::string* error);

/** @brief 字节序列 → 大写十六进制文本。 */
std::string HuiManual_ToHex(const uint8_t* data, size_t len);

#endif /* HUI_MANUAL_H */
