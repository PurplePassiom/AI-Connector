#include "ml307_gnss_test.h"

#include "config.h"

#include <at_error.h>
#include <at_uart.h>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#define TAG "GnssTest"

// ===========================================================================
// ML307 GNSS AT 命令速查
// 出处：中移物联网《GNSS用户手册》V1.0.1（docs/GNSS用户手册_4G系列_primary.pdf），
//       适用型号 ML307C-GC-CN（也适用 ML307A / ML302A）。
//
// ── 本测试任务用到的命令 ───────────────────────────────────────────────
//   配置/控制
//     AT+MGNSSCFG="constellation/mask",<mask>   搜星系统掩码
//                                               bit0 GPS / bit1 BDS / bit2 QZSS /
//                                               bit3 GLONASS / bit4 Galileo
//                                               3 = GPS+北斗（手册 3.1 示例）
//     AT+MGNSSCFG="nmea/mask",<0-63>            NMEA 主动上报掩码
//                                               bit0 GGA/1 GSV/2 GSA/3 RMC/4 VTG/5 GLL
//     AT+MAGNSSEN=<0|1>                         AGNSS 辅助定位开关（须在开 GNSS 之前）
//     AT+MGNSSRST=<0|1|2>                       软重启接收机：0 冷启动/1 温启动/2 热启动
//     AT+MGNSS=<0|1|2>                          0 关闭 / 1 连续定位 / 2 单次定位
//     AT+MGNSS?                                 查询状态 -> +MGNSS: <status>（0/1/2 同上）
//
//   读取定位数据（本测试程序的核心）
//     AT+MGNSSNMEA="GGA"                        读一条 GGA 语句
//       -> +MGNSSNMEA: $GNGGA,<UTC>,<lat>,<N/S>,<lon>,<E/W>,<fix>,<nsat>,<hdop>,<alt>,...
//          <UTC>        hhmmss.sss        UTC 时间（本程序要的“UTC”）
//          <lat>        ddmm.mmmmm        纬度（度分）
//          <lon>        dddmm.mmmmm       经度（度分）
//          <fix>        0 未定位 / 1 单点 / 2 差分 …
//          <nsat>       参与定位的卫星数
//          <hdop>       水平精度因子
//     AT+MGNSSNMEA="RMC"|"VTG"|"GLL"|"GSA"|"GSV"  其它 NMEA 语句（日期在 RMC 里）
//     AT+MGNSSLOC                               读定位信息
//       -> +MGNSSLOC: <UTC>,<lat>,<lon>,<hdop>,<alt>,<fix>,<cog>,<spkm>,<spkn>,<date>,<nsat>,<dtype>
//     AT+MGNSSLOC=<0|1>                         1 = 定位成功后自动上报 +MGNSSLOC
//
//   异步上报（URC）
//     +MGNSSURC: "state",<0|1>                  GNSS 已关闭 / 已开启
//     +MGNSSURC: "error",<code>                 1 状态异常 2 存储错误 3 启动错误
//                                               4 AGNSS数据无效 5 AGNSS服务器连接异常
//                                               6 AGNSS数据获取异常 7 DGNSS服务器连接异常
//                                               8 DGNSS鉴权异常 9 DGNSS数据获取异常
//     +MGNSSLOC: ...                            AT+MGNSSLOC=1 时的自动上报
//     $GNGGA/$GPGGA ...                         AT+MGNSSCFG="nmea/mask",n 打开后的主动上报
//
//   其它（本测试未用，需要时按手册加）
//     AT+MAGNSSREFLOC=<lat>,<lon>[,2]           设置参考坐标（ML307C 只支持 mode=2）
//     AT+MAGNSSDATA                            触发下载 AGNSS 辅助数据（消耗流量）
//     AT+MDGNSSEN=<0|1>                         差分定位开关（ML307C 不支持）
//
// ── 常见 +CME ERROR（手册 5.1）─────────────────────────────────────────
//   950 未知错误 951 GNSS参数错误 952 GNSS已开启 953 GNSS未开启 954 GNSS状态忙
//   955 AGNSS状态忙 956 DGNSS状态忙 957 GNSS设备错误 958 GNSS操作内存失败
//   959 GNSS请求失败 960 AGNSS已开启 961 AGNSS未开启 962/963 DGNSS 已开/未开
//
// ── ⚠️ 为什么用 AT+MGNSSNMEA="GGA" 而不是 AT+MGNSSLOC ─────────────────
//   驱动 at_uart.cc 把所有 "+XXX: ..." 形式的响应当 URC 抛给回调，并按逗号切分参数、
//   数值字段转成 double/int。而 +MGNSSLOC 的纬度是「2937.1685N」这种带 N/S 后缀的字段，
//   一旦被 std::stod 转成 double，N/S、E/W 后缀就永久丢失，还原不出南/北纬、东/西经。
//   GGA 语句里 N/S、E/W 是独立字段（字符串），不会丢；时间/经纬度字段虽丢了前导 0，
//   但 NMEA 的字段宽度固定，可以按宽度零填充精确还原（见 FormatFixedWidth）。
// ===========================================================================

#if GNSS_TEST

namespace {

// ---------------- AT 命令字符串（集中管理，避免魔法字符串散落） ----------------
constexpr char kCmdGnssQuery[] = "AT+MGNSS?";
constexpr char kCmdGnssOff[] = "AT+MGNSS=0";
constexpr char kCmdGnssNmeaGga[] = "AT+MGNSSNMEA=\"GGA\"";
constexpr char kCmdGnssNmeaGsv[] = "AT+MGNSSNMEA=\"GSV\"";

std::string CmdGnssOn(int mode) {
    return "AT+MGNSS=" + std::to_string(mode);
}
[[maybe_unused]] std::string CmdGnssReset(int mode) {
    return "AT+MGNSSRST=" + std::to_string(mode);
}
[[maybe_unused]] std::string CmdGnssConstellation(int mask) {
    return "AT+MGNSSCFG=\"constellation/mask\"," + std::to_string(mask);
}
[[maybe_unused]] std::string CmdAgnssEnable(int enable) {
    return "AT+MAGNSSEN=" + std::to_string(enable);
}

// ---------------- 错误/状态文字（手册 5.1、3.2、3.3） ----------------
const char* CmeHint(int cme) {
    switch (cme) {
        case 950: return "未知错误";
        case 951: return "GNSS参数错误";
        case 952: return "GNSS已开启";
        case 953: return "GNSS未开启";
        case 954: return "GNSS状态忙";
        case 955: return "AGNSS状态忙";
        case 956: return "DGNSS状态忙";
        case 957: return "GNSS设备错误";
        case 958: return "GNSS操作内存失败";
        case 959: return "GNSS请求失败";
        case 960: return "AGNSS已开启";
        case 961: return "AGNSS未开启";
        case 962: return "DGNSS已开启";
        case 963: return "DGNSS未开启";
        default: return "未知错误码";
    }
}

const char* GnssStatusName(int status) {
    switch (status) {
        case 0: return "已关闭";
        case 1: return "连续定位中";
        case 2: return "单次定位中";
        default: return "状态未知（没收到 +MGNSS 响应）";
    }
}

// GGA 的定位质量字段（NMEA0183 标准）
const char* FixQualityName(int quality) {
    switch (quality) {
        case 0: return "未定位";
        case 1: return "单点定位";
        case 2: return "差分定位";
        case 4: return "RTK固定解";
        case 5: return "RTK浮动解";
        default: return "其它";
    }
}

// ---------------- 一次定位结果（可安全拷贝出来打印） ----------------
struct GgaFix {
    // ⚠️ 实测 ML307C：还没定位时也会回 GGA，形如
    //    $GNGGA,011646.742,0000.00000,N,00000.00000,E,0,00,0.000,0.0,M,0.0,M,,*6D
    //    即 UTC 可能有值（已从卫星取到时间），但经纬度是 0000.00000、fix=0、定位用卫星=0。
    //    所以「是否定位成功」必须以 fix_quality >= 1 为准，
    //    不能拿 UTC 是否为空、也不能拿经纬度字段来判断。
    bool valid = false;     // 定位成功（fix_quality >= 1）
    bool has_time = false;  // 已从卫星取到 UTC 时间（未定位时也可能为 true）
    uint32_t seq = 0;       // 收到的第几条 GGA，用来判断本轮轮询有没有拿到新数据
    std::string utc;        // hhmmss.sss
    std::string lat_field;  // ddmm.mmmmm
    std::string lon_field;  // dddmm.mmmmm
    char ns = 'N';
    char ew = 'E';
    double lat_deg = 0.0;   // 十进制度（南纬/西经为负）
    double lon_deg = 0.0;
    int fix_quality = 0;
    int satellites = 0;
    double hdop = 0.0;
    double altitude = 0.0;
};

// GSV（可见卫星）统计：判断天线是否接好、信号强弱的依据
struct GsvStat {
    int in_view = 0;    // 可见卫星数
    int best_cn0 = -1;  // 最强载噪比 dBHz
};

struct GsvInfo {
    GsvStat gps;
    GsvStat bds;
    GsvStat glonass;
    GsvStat galileo;
};

struct GnssSnapshot {
    GgaFix fix;
    GsvInfo gsv;
};

// URC 回调（EventTask）写、测试任务读，用一把锁保护
struct GnssState {
    std::mutex mtx;
    bool has_status = false;
    int status = -1;
    GgaFix fix;
    GsvInfo gsv;

    uint32_t GgaSeq() {
        std::lock_guard<std::mutex> lock(mtx);
        return fix.seq;
    }
    GnssSnapshot Snapshot() {
        std::lock_guard<std::mutex> lock(mtx);
        return {fix, gsv};
    }
    int Status() {
        std::lock_guard<std::mutex> lock(mtx);
        return has_status ? status : -1;
    }
};

// ---------------- NMEA 字段还原/换算 ----------------
// 驱动把数值字段解析成 double 后前导 0 就没了（"095905.729" -> 95905.729）。
// NMEA 的字段宽度是固定的，按宽度零填充即可精确还原：
//   hhmmss.sss  -> %010.3f（6 位整数 + '.' + 3 位小数）
//   ddmm.mmmmm  -> %010.5f（纬度 2 位度）
//   dddmm.mmmmm -> %011.5f（经度 3 位度）
std::string FormatFixedWidth(double value, int total_width, int decimals) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%0*.*f", total_width, decimals, value);
    return buf;
}

// "095905.729" -> "09:59:05.729"
std::string FormatUtcHms(const std::string& utc) {
    if (utc.size() < 6) {
        return utc;
    }
    return utc.substr(0, 2) + ":" + utc.substr(2, 2) + ":" + utc.substr(4);
}

// ddmm.mmmmm + 半球 -> 十进制度
double NmeaFieldToDegrees(const std::string& ddmm, char hemisphere) {
    double raw = std::strtod(ddmm.c_str(), nullptr);
    int degrees = static_cast<int>(raw / 100.0);
    double minutes = raw - degrees * 100.0;
    double value = degrees + minutes / 60.0;
    return (hemisphere == 'S' || hemisphere == 'W') ? -value : value;
}

bool ArgIsEmpty(const AtArgumentValue& arg) {
    return arg.type == AtArgumentValue::Type::String && arg.string_value.empty();
}

double ArgAsDouble(const AtArgumentValue& arg) {
    switch (arg.type) {
        case AtArgumentValue::Type::Int: return arg.int_value;
        case AtArgumentValue::Type::Double: return arg.double_value;
        default: return std::strtod(arg.string_value.c_str(), nullptr);
    }
}

// 可见卫星汇总文本，如 "GPS 1颗(最强29 dBHz) BDS 2颗(最强31 dBHz)"；一颗都没搜到时返回 "无"
std::string FormatGsv(const GsvInfo& gsv) {
    std::string text;
    auto append = [&text](const char* name, const GsvStat& stat) {
        if (stat.in_view <= 0) {
            return;
        }
        if (!text.empty()) {
            text += " ";
        }
        char buf[48];
        if (stat.best_cn0 >= 0) {
            std::snprintf(buf, sizeof(buf), "%s %d颗(最强%d dBHz)", name, stat.in_view, stat.best_cn0);
        } else {
            std::snprintf(buf, sizeof(buf), "%s %d颗", name, stat.in_view);
        }
        text += buf;
    };
    append("GPS", gsv.gps);
    append("BDS", gsv.bds);
    append("GLONASS", gsv.glonass);
    append("Galileo", gsv.galileo);
    return text.empty() ? "无" : text;
}

// ---------------- 发一条 AT 命令并带上结果日志 ----------------
bool SendAt(const std::shared_ptr<AtUart>& uart, const std::string& command, uint32_t timeout_ms = 3000,
            bool log_error = true) {
    ESP_LOGI(TAG, "AT >> %s", command.c_str());
    auto result = uart->SendCommand(command, timeout_ms);
    if (!result) {
        if (log_error) {
            const AtError& error = result.error();
            if (error.code == AtErrc::CmeError) {
                ESP_LOGE(TAG, "AT << %s 失败: +CME ERROR: %d (%s)", command.c_str(), error.cme,
                         CmeHint(error.cme));
            } else {
                ESP_LOGE(TAG, "AT << %s 失败: %s", command.c_str(), error.ToString().c_str());
            }
        }
        return false;
    }
    return true;
}

// ---------------- URC 回调（在驱动的 EventTask 里执行，必须快） ----------------
void HandleGnssUrc(const std::string& command, const std::vector<AtArgumentValue>& args, GnssState* state) {
    // 异步状态/错误上报（手册 3.12）：直接打印，不缓存
    if (command == "MGNSSURC" && args.size() >= 2) {
        if (args[0].string_value == "state") {
            ESP_LOGI(TAG, "URC +MGNSSURC: GNSS %s (state=%d)",
                     args[1].int_value == 1 ? "已开启" : "已关闭", args[1].int_value);
        } else if (args[0].string_value == "error") {
            static const char* kUrcErrorNames[] = {"",                     "GNSS状态异常",
                                                   "GNSS存储错误",         "GNSS启动错误",
                                                   "AGNSS辅助数据无效",    "AGNSS服务器连接异常",
                                                   "AGNSS辅助数据获取异常", "DGNSS服务器连接异常",
                                                   "DGNSS鉴权异常",        "DGNSS差分数据获取异常"};
            int code = args[1].int_value;
            const char* name = (code >= 1 && code <= 9) ? kUrcErrorNames[code] : "未知";
            ESP_LOGE(TAG, "URC +MGNSSURC: 错误码 %d (%s)", code, name);
        }
        return;
    }

    // AT+MGNSSLOC=1 时的自动上报（手册 3.10）。注意：数值字段已被驱动转成 double，
    // N/S、E/W 后缀在 +MGNSSLOC 里是贴在数值后面的（2937.1685N），已经被丢掉，
    // 所以这里只能打印数值本身，不能判断南北纬/东西经 —— 精确数据请用 GGA。
    if (command == "MGNSSLOC" && args.size() >= 11) {
        ESP_LOGI(TAG, "URC +MGNSSLOC: UTC=%.3f 纬度=%.5f 经度=%.5f hdop=%.1f 海拔=%.1f 卫星=%d",
                 ArgAsDouble(args[0]), ArgAsDouble(args[1]), ArgAsDouble(args[2]), ArgAsDouble(args[3]),
                 ArgAsDouble(args[4]), static_cast<int>(ArgAsDouble(args[10])));
        return;
    }

    std::lock_guard<std::mutex> lock(state->mtx);

    // AT+MGNSS? 的响应 -> +MGNSS: <status>
    if (command == "MGNSS" && !args.empty()) {
        state->has_status = true;
        state->status = args[0].int_value;
        return;
    }

    // NMEA 语句的响应 -> +MGNSSNMEA: $GNGGA,... / +MGNSSNMEA: $GPGSV,...
    if (command == "MGNSSNMEA" && args.size() >= 4) {
        const std::string& sentence = args[0].string_value;

        // --- GSV：可见卫星数与最强载噪比（判断天线是否接好、信号强弱）---
        if (sentence.size() >= 3 && sentence.compare(sentence.size() - 3, 3, "GSV") == 0) {
            GsvStat* stat = nullptr;
            if (sentence.compare(1, 2, "GP") == 0) {
                stat = &state->gsv.gps;
            } else if (sentence.compare(1, 2, "GB") == 0) {
                stat = &state->gsv.bds;
            } else if (sentence.compare(1, 2, "GL") == 0) {
                stat = &state->gsv.glonass;
            } else if (sentence.compare(1, 2, "GA") == 0) {
                stat = &state->gsv.galileo;
            }
            if (stat != nullptr) {
                // $GPGSV,<总条数>,<本条序号>,<可见卫星数>,<PRN,仰角,方位,C/N0> xN,...
                stat->in_view = static_cast<int>(ArgAsDouble(args[3]));
                int best_cn0 = -1;
                for (size_t i = 7; i < args.size(); i += 4) {  // 每颗星 4 个字段，第 4 个是 C/N0
                    int cn0 = static_cast<int>(ArgAsDouble(args[i]));
                    if (cn0 > best_cn0) {
                        best_cn0 = cn0;
                    }
                }
                stat->best_cn0 = best_cn0;
            }
            return;
        }

        // --- GGA：UTC 时间 / 经纬度 / 定位状态 ---
        // args[0] 是 $GNGGA / $GPGGA / $BDGGA…，只认 GGA
        if (sentence.find("GGA") == std::string::npos) {
            return;
        }
        GgaFix& fix = state->fix;
        fix.seq++;
        fix.fix_quality = ArgIsEmpty(args[6]) ? 0 : static_cast<int>(ArgAsDouble(args[6]));
        fix.satellites = ArgIsEmpty(args[7]) ? 0 : static_cast<int>(ArgAsDouble(args[7]));
        fix.hdop = ArgIsEmpty(args[8]) ? 0.0 : ArgAsDouble(args[8]);
        fix.altitude = (args.size() > 9 && !ArgIsEmpty(args[9])) ? ArgAsDouble(args[9]) : 0.0;

        // 未定位时模块可能已经给出 UTC（从卫星取到时间），但经纬度会填成
        // 0000.00000 / 00000.00000 —— 所以只看 fix_quality 判断是否真的定上了。
        fix.has_time = !ArgIsEmpty(args[1]);
        fix.valid = fix.fix_quality >= 1;
        if (fix.has_time) {
            fix.utc = FormatFixedWidth(ArgAsDouble(args[1]), 10, 3);  // hhmmss.sss
        } else {
            fix.utc.clear();
        }
        fix.lat_field.clear();
        fix.lon_field.clear();
        fix.lat_deg = 0.0;
        fix.lon_deg = 0.0;
        if (fix.valid) {
            fix.lat_field = FormatFixedWidth(ArgAsDouble(args[2]), 10, 5);
            fix.ns = ArgIsEmpty(args[3]) ? 'N' : args[3].string_value[0];
            fix.lon_field = FormatFixedWidth(ArgAsDouble(args[4]), 11, 5);
            fix.ew = ArgIsEmpty(args[5]) ? 'E' : args[5].string_value[0];
            fix.lat_deg = NmeaFieldToDegrees(fix.lat_field, fix.ns);
            fix.lon_deg = NmeaFieldToDegrees(fix.lon_field, fix.ew);
        }
        return;
    }
}

// ---------------- 测试主体 ----------------
void GnssTestTask(std::shared_ptr<AtUart> uart) {
    auto state = std::make_shared<GnssState>();
    // 用 shared_ptr 捕获：URC 回调即使比本函数活得久也不会读到野指针
    uart->RegisterUrcCallback([state](const std::string& command, const std::vector<AtArgumentValue>& args) {
        HandleGnssUrc(command, args, state.get());
    });

    ESP_LOGI(TAG, "==================== ML307 GNSS 定位测试 ====================");
    ESP_LOGI(TAG, "GNSS_TEST_MODE=%d  POLL=%dms  CONSTELLATION_MASK=0x%X  RST_MODE=%d  AGNSS=%d",
             GNSS_TEST_MODE, GNSS_TEST_POLL_MS, (unsigned)GNSS_TEST_CONSTELLATION_MASK, GNSS_TEST_RST_MODE,
             GNSS_TEST_AGNSS);
    ESP_LOGI(TAG, "轮询命令: %s （读 UTC 时间 / 经纬度 / 定位状态）", kCmdGnssNmeaGga);

    // 1) 先查当前状态（响应 +MGNSS: <status> 由 URC 回调收下）
    if (SendAt(uart, kCmdGnssQuery)) {
        int status = state->Status();
        ESP_LOGI(TAG, "GNSS 当前状态: %d (%s)", status, GnssStatusName(status));
    }

    // 2) 先关一次，保证从干净状态开始。模组本来就没开时会回 953 GNSS未开启，属正常，不报错
    SendAt(uart, kCmdGnssOff, 3000, false);

#if GNSS_TEST_AGNSS
    // AGNSS 必须在开 GNSS 之前使能（手册 3.8）
    SendAt(uart, CmdAgnssEnable(1));
#endif

#if GNSS_TEST_RST_MODE >= 0
    SendAt(uart, CmdGnssReset(GNSS_TEST_RST_MODE));
#endif

#if GNSS_TEST_CONSTELLATION_MASK > 0
    SendAt(uart, CmdGnssConstellation(GNSS_TEST_CONSTELLATION_MASK));
#endif

    // 3) 开 GNSS
    if (!SendAt(uart, CmdGnssOn(GNSS_TEST_MODE))) {
        ESP_LOGE(TAG, "开启 GNSS 失败：模组可能不带 GNSS 功能（或固件未使能 / 天线或硬件异常），测试结束");
        return;
    }

    // 4) 按固定间隔轮询 GGA，打印 UTC / 经纬度 / 定位状态
    uint32_t poll = 0;
    while (true) {
        uint32_t seq_before = state->GgaSeq();

        if (!SendAt(uart, kCmdGnssNmeaGga)) {
            ESP_LOGW(TAG, "读取 GGA 失败（GNSS 可能被关闭），下一轮重试");
        } else {
            // 手册里 +MGNSSNMEA 的响应在 OK 之前就已发出，正常情况下这里已经拿到数据；
            // 这里再兜底等 500ms，防止响应被别的响应挤到后面
            for (int waited = 0; waited < 500 && state->GgaSeq() == seq_before; waited += 20) {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            uint32_t elapsed_s = poll * GNSS_TEST_POLL_MS / 1000;
            if (state->GgaSeq() == seq_before) {
                ESP_LOGW(TAG, "没有收到 +MGNSSNMEA 响应（AT 通道可能被其它任务占用），下一轮重试");
            }
            GnssSnapshot snap = state->Snapshot();
            const GgaFix& fix = snap.fix;
            std::string gsv = FormatGsv(snap.gsv);
            if (!fix.valid) {
                ESP_LOGW(TAG,
                         "[未定位 %us] UTC %s | fix=%d 定位用卫星=%d hdop=%.2f | 可见卫星 %s"
                         "（室内/遮挡处可能一直定不上，请到室外开阔处，首次定位一般 30~90s）",
                         (unsigned)elapsed_s, fix.has_time ? FormatUtcHms(fix.utc).c_str() : "--",
                         fix.fix_quality, fix.satellites, fix.hdop, gsv.c_str());
            } else {
                ESP_LOGI(TAG,
                         "[已定位 %us] UTC %s | 纬度 %.5f%c (NMEA %s%c) | 经度 %.5f%c (NMEA %s%c) | "
                         "fix=%d(%s) 定位用卫星=%d hdop=%.2f 海拔=%.1fm | 可见卫星 %s",
                         (unsigned)elapsed_s, FormatUtcHms(fix.utc).c_str(), std::fabs(fix.lat_deg), fix.ns,
                         fix.lat_field.c_str(), fix.ns, std::fabs(fix.lon_deg), fix.ew, fix.lon_field.c_str(),
                         fix.ew, fix.fix_quality, FixQualityName(fix.fix_quality), fix.satellites, fix.hdop,
                         fix.altitude, gsv.c_str());
#if GNSS_TEST_MODE == 2
                // 单次定位：模组定位成功后会自动关闭 GNSS，测试目的已达到
                ESP_LOGI(TAG, "单次定位完成（GNSS 将被模组自动关闭），测试任务结束");
                return;
#endif
            }
        }

        // 每 5 轮额外取一次 GSV（可见卫星 / 载噪比），用来判断天线与信号质量
        if (poll % 5 == 0) {
            SendAt(uart, kCmdGnssNmeaGsv, 3000, false);
        }

        poll++;
        // 每 30 轮查一次状态，被异常关闭时重新打开
        if (poll % 30 == 0 && SendAt(uart, kCmdGnssQuery, 3000, false)) {
            if (state->Status() == 0) {
                ESP_LOGW(TAG, "GNSS 已处于关闭状态，重新下发 AT+MGNSS=%d", GNSS_TEST_MODE);
                SendAt(uart, CmdGnssOn(GNSS_TEST_MODE));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(GNSS_TEST_POLL_MS));
    }
}

}  // namespace

void StartMl307GnssTest(std::shared_ptr<AtUart> at_uart) {
    if (!at_uart) {
        ESP_LOGE(TAG, "AT 通道为空，GNSS 测试不启动");
        return;
    }
    // 用堆上的 shared_ptr 传参（xTaskCreate 只能带一个 void*），任务内先释放堆对象再跑
    auto* arg = new std::shared_ptr<AtUart>(std::move(at_uart));
    BaseType_t created = xTaskCreate(
        [](void* param) {
            auto* holder = static_cast<std::shared_ptr<AtUart>*>(param);
            auto uart = *holder;
            delete holder;
            GnssTestTask(uart);
            vTaskDelete(nullptr);
        },
        "gnss_test", 5120, arg, 4, nullptr);
    if (created != pdPASS) {
        delete arg;
        ESP_LOGE(TAG, "创建 GNSS 测试任务失败");
    }
}

#else  // !GNSS_TEST

void StartMl307GnssTest(std::shared_ptr<AtUart> at_uart) {
    (void)at_uart;  // GNSS_TEST=0：开关关闭，不产生任何 AT 命令
}

#endif  // GNSS_TEST
