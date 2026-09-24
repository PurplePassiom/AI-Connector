#include "ml307_net_heal.h"

#include "config.h"

#include <at_uart.h>

#include <esp_log.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#define TAG "NetHeal"

// ===========================================================================
// ML307 掉网 / SIM 锁死自愈
//
// 用到的 AT 命令都很基础，不依赖厂商 GNSS 手册：
//   AT+CEREG?           查 EPS 注册状态 -> +CEREG: <n>,<stat>[,"<tac>","<ci>",<AcT>]
//                       stat: 0 未注册且不搜网 / 1 已注册(本地) / 2 正在搜网 /
//                             3 注册被拒 / 4 未知 / 5 已注册(漫游)
//   AT+CSQ              查信号质量 -> +CSQ: <rssi>,<ber>   rssi: 0-31 有效, 99 读不到
//   AT+CFUN=0           最小功能：关射频、关 SIM 接口
//   AT+CFUN=1           全功能：重新初始化射频与 SIM 接口（本板实测可解开锁死状态）
//   AT+MCCID            读 ICCID（ML307 专用命令，不能写成 AT+ICCID）
//   AT+CPIN?            SIM 状态 -> +CPIN: READY
//
// ⚠️ 判定「异常」时只用**查询成功但结果异常**的情况（见 NetHealTask），
//    命令本身超时不算异常 —— 否则音频流正好占着 AT 通道时会误判，
//    而误判的代价是一次 CFUN=0（会把当前 4G 业务打断），必须避免。
// ===========================================================================

#if NET_HEAL_ENABLE

namespace {

constexpr char kCmdCereg[] = "AT+CEREG?";
constexpr char kCmdCsq[] = "AT+CSQ";
constexpr char kCmdCfunOff[] = "AT+CFUN=0";
constexpr char kCmdCfunOn[] = "AT+CFUN=1";
constexpr char kCmdIccid[] = "AT+MCCID";
constexpr char kCmdCpin[] = "AT+CPIN?";

// URC 回调（EventTask）写、看门狗任务读，用一把锁保护。
// 用 +CEREG / +CSQ 这两个 URC 自己抓最新的真实值：驱动的 GetCsq()/GetRegistrationState()
// 返回的是缓存，命令失败时会拿到过期值，不能用来判断「模组是不是真的卡死了」。
struct NetSignal {
    std::mutex mtx;
    int cereg_stat = -1;
    uint32_t cereg_seq = 0;
    int csq = -1;
    uint32_t csq_seq = 0;

    void OnUrc(const std::string& command, const std::vector<AtArgumentValue>& args) {
        if (command == "CEREG" && !args.empty()) {
            std::lock_guard<std::mutex> lock(mtx);
            // 两种格式：+CEREG: <stat>  或  +CEREG: <n>,<stat>[,"<tac>","<ci>",<AcT>]
            size_t index = (args.size() >= 2 && args[1].type == AtArgumentValue::Type::Int) ? 1 : 0;
            if (index < args.size()) {
                cereg_stat = args[index].int_value;
                cereg_seq++;
            }
        } else if (command == "CSQ" && !args.empty()) {
            std::lock_guard<std::mutex> lock(mtx);
            csq = args[0].int_value;
            csq_seq++;
        }
    }

    void Snapshot(int* stat, uint32_t* stat_seq, int* rssi, uint32_t* rssi_seq) {
        std::lock_guard<std::mutex> lock(mtx);
        *stat = cereg_stat;
        *stat_seq = cereg_seq;
        *rssi = csq;
        *rssi_seq = csq_seq;
    }
};

const char* CeregName(int stat) {
    switch (stat) {
        case 0: return "未注册且不搜网";
        case 1: return "已注册(本地)";
        case 2: return "正在搜网";
        case 3: return "注册被拒绝";
        case 4: return "未知";
        case 5: return "已注册(漫游)";
        default: return "读取失败";
    }
}

bool SendAt(const std::shared_ptr<AtUart>& uart, const std::string& command, uint32_t timeout_ms) {
    ESP_LOGI(TAG, "AT >> %s", command.c_str());
    auto result = uart->SendCommand(command, timeout_ms);
    if (!result) {
        ESP_LOGW(TAG, "AT << %s 无响应: %s", command.c_str(), result.error().ToString().c_str());
        return false;
    }
    return true;
}

// AT+CFUN=0 -> 等 3s -> AT+CFUN=1 -> 等 10s：重新初始化射频与 SIM 接口
void ReinitializeModem(const std::shared_ptr<AtUart>& uart) {
    ESP_LOGW(TAG, "→ 重新初始化模组射频/SIM：AT+CFUN=0 → %dms → AT+CFUN=1", NET_HEAL_CFUN_OFF_MS);
    SendAt(uart, kCmdCfunOff, 5000);
    vTaskDelay(pdMS_TO_TICKS(NET_HEAL_CFUN_OFF_MS));
    SendAt(uart, kCmdCfunOn, 5000);
    vTaskDelay(pdMS_TO_TICKS(NET_HEAL_CFUN_ON_WAIT_MS));

    // 复检：SIM 是否回来、有没有重新注册
    if (SendAt(uart, kCmdCpin, 3000)) {
        ESP_LOGI(TAG, "恢复后 AT+CPIN? 正常（SIM 接口有响应）");
    }
    if (SendAt(uart, kCmdIccid, 3000)) {
        ESP_LOGI(TAG, "恢复后 AT+MCCID 成功（SIM 可读）");
    }
}

#if NET_HEAL_POWER_CYCLE
// ⚠️ 实验性：给模组真正断电重启（PWRKEY 低脉冲 = 开/关机切换，时序见 config.h）。
// 连续 NET_HEAL_MAX_CFUN_TRIES 次 CFUN 重初始化无效时才用；时序不对可能把模组关掉，
// 需要手动拔电恢复，所以默认关闭（NET_HEAL_POWER_CYCLE=0）。
void PowerCycleModule() {
    const int active = ML307_POWER_ACTIVE_LEVEL;
    const int inactive = active ? 0 : 1;
    ESP_LOGW(TAG, "→ 实验性：用 PWRKEY 给模组断电重启（关机 → 3s → 开机）");
    gpio_set_level(ML307_POWER_PIN, inactive);
    vTaskDelay(pdMS_TO_TICKS(500));
    gpio_set_level(ML307_POWER_PIN, active);  // 拉低 -> 关机
    vTaskDelay(pdMS_TO_TICKS(ML307_POWERKEY_PULSE_MS));
    gpio_set_level(ML307_POWER_PIN, inactive);
    vTaskDelay(pdMS_TO_TICKS(3000));
    gpio_set_level(ML307_POWER_PIN, active);  // 再拉低 -> 开机
    vTaskDelay(pdMS_TO_TICKS(ML307_POWERKEY_PULSE_MS));
#if ML307_POWERKEY_MODE == 2
    gpio_set_level(ML307_POWER_PIN, active);  // 保持拉低：有电即工作
#else
    gpio_set_level(ML307_POWER_PIN, inactive);
#endif
    vTaskDelay(pdMS_TO_TICKS(15000));  // 等模组启动 + 注册
}
#endif  // NET_HEAL_POWER_CYCLE

void NetHealTask(std::shared_ptr<AtUart> uart) {
    auto signal = std::make_shared<NetSignal>();
    // shared_ptr 捕获：URC 回调即使比本函数活得久也不会读到野指针
    uart->RegisterUrcCallback(
        [signal](const std::string& command, const std::vector<AtArgumentValue>& args) {
            signal->OnUrc(command, args);
        });

    ESP_LOGI(TAG, "=== 4G 网络/SIM 自愈看门狗启动：每 %dms 查一次 CEREG/CSQ，连续 %d 次异常就重初始化模组 ===",
             NET_HEAL_CHECK_INTERVAL_MS, NET_HEAL_FAIL_THRESHOLD);

#if NET_HEAL_SELF_TEST
    // 自检：主动把射频关掉模拟「掉网锁死」，验证看门狗能否自己发现并恢复。
    vTaskDelay(pdMS_TO_TICKS(NET_HEAL_SELF_TEST_DELAY_MS));
    ESP_LOGW(TAG, "[自检] 主动下发 AT+CFUN=0 模拟掉网，接下来看门狗应当自己发现并恢复");
    SendAt(uart, kCmdCfunOff, 5000);
#endif

    int fail_count = 0;
#if NET_HEAL_POWER_CYCLE
    int cfun_tries = 0;
#endif

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(NET_HEAL_CHECK_INTERVAL_MS));

        int stat = -1, rssi = -1;
        uint32_t stat_seq = 0, rssi_seq = 0;
        signal->Snapshot(&stat, &stat_seq, &rssi, &rssi_seq);

        // 两条查询都必须成功，只看「查到了但结果不对」的情况（避免抢通道导致误判）
        bool ok_cereg = SendAt(uart, kCmdCereg, 3000);
        bool ok_csq = SendAt(uart, kCmdCsq, 3000);
        if (!ok_cereg || !ok_csq) {
            ESP_LOGW(TAG, "本轮查询未拿到完整响应，跳过判定（不重初始化，避免误伤正在跑的 4G 业务）");
            continue;
        }

        int new_stat = -1, new_rssi = -1;
        uint32_t new_stat_seq = 0, new_rssi_seq = 0;
        signal->Snapshot(&new_stat, &new_stat_seq, &new_rssi, &new_rssi_seq);
        if (new_stat_seq == stat_seq || new_rssi_seq == rssi_seq) {
            ESP_LOGW(TAG, "查询成功但没收到 +CEREG/+CSQ 上报，跳过判定");
            continue;
        }

        bool registered = (new_stat == 1 || new_stat == 5);
        bool rf_ok = (new_rssi >= 1 && new_rssi <= 31);  // 99 = 读不到信号
        ESP_LOGI(TAG, "状态检查: CEREG=%d(%s) CSQ=%d -> %s", new_stat, CeregName(new_stat), new_rssi,
                 (registered && rf_ok) ? "正常" : "异常");

        if (registered && rf_ok) {
            if (fail_count > 0) {
                ESP_LOGI(TAG, "★ 网络已恢复正常");
            }
            fail_count = 0;
#if NET_HEAL_POWER_CYCLE
            cfun_tries = 0;
#endif
            continue;
        }

        fail_count++;
        if (fail_count < NET_HEAL_FAIL_THRESHOLD) {
            ESP_LOGW(TAG, "网络异常第 %d/%d 次，再观察一轮", fail_count, NET_HEAL_FAIL_THRESHOLD);
            continue;
        }

        fail_count = 0;
        ESP_LOGE(TAG,
                 "检测到模组卡死（掉网 + SIM 不可用，特征就是成对的 +CME ERROR: 30 与 13），"
                 "执行自愈流程");
        ReinitializeModem(uart);
#if NET_HEAL_POWER_CYCLE
        if (++cfun_tries >= NET_HEAL_MAX_CFUN_TRIES) {
            cfun_tries = 0;
            PowerCycleModule();
        }
#endif
    }
}

}  // namespace

void StartMl307NetHealTask(std::shared_ptr<AtUart> at_uart) {
    if (!at_uart) {
        ESP_LOGE(TAG, "AT 通道为空，自愈看门狗不启动");
        return;
    }
    auto* arg = new std::shared_ptr<AtUart>(std::move(at_uart));
    BaseType_t created = xTaskCreate(
        [](void* param) {
            auto* holder = static_cast<std::shared_ptr<AtUart>*>(param);
            auto uart = *holder;
            delete holder;
            NetHealTask(uart);
            vTaskDelete(nullptr);
        },
        "net_heal", 4096, arg, 3, nullptr);
    if (created != pdPASS) {
        delete arg;
        ESP_LOGE(TAG, "创建自愈看门狗任务失败");
    }
}

#else  // !NET_HEAL_ENABLE

void StartMl307NetHealTask(std::shared_ptr<AtUart> at_uart) {
    (void)at_uart;  // NET_HEAL_ENABLE=0：关闭，不产生任何额外 AT 命令
}

#endif  // NET_HEAL_ENABLE
