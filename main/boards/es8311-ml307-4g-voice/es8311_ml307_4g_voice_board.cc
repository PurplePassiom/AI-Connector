#include "ml307_board.h"
#include "codecs/es8311_audio_codec.h"
#include "application.h"
#include "button.h"
#include "led/single_led.h"
#include "config.h"

#include <esp_log.h>
#include <driver/gpio.h>
#include <driver/i2c_master.h>

#include <utility>

#if SOA_CAN_ENABLE
// CAN 总线驱动 + HUI 节点（从 MIO 工程移植，见 main/soa/）
#include "can_bus.h"
#include "hui.h"
#include "hui_mcp.h"
#include "soa.h"
#endif

// 板级诊断 / 自愈任务（都要等模组就绪后再碰 AT 通道，所以共用一套网络事件同步）
#if GNSS_TEST || NET_HEAL_ENABLE
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#endif
#if GNSS_TEST
// GNSS 定位测试：轮询 AT+MGNSSNMEA="GGA" 读 UTC 时间 / 经纬度 / 定位状态，
// 详见 ml307_gnss_test.cc 顶部的 AT 命令速查
#include "ml307_gnss_test.h"
#endif
#if NET_HEAL_ENABLE
// 4G 掉网 / SIM 锁死自愈看门狗（AT+CFUN=0 → AT+CFUN=1），详见 ml307_net_heal.h
#include "ml307_net_heal.h"
#endif

#define TAG "Es8311Ml3074gVoiceBoard"

// 纯语音 + 4G 板：没有显示屏、没有触摸、没有摄像头。
// 因此不 override Board::GetDisplay() / GetBacklight() —— 基类会返回内置的
// NoDisplay 与 nullptr，上层已有空指针 / 高度判断保护。
class Es8311Ml3074gVoiceBoard : public Ml307Board {
private:
    i2c_master_bus_handle_t codec_i2c_bus_ = nullptr;
    Button boot_button_;

#if GNSS_TEST || NET_HEAL_ENABLE
    // ===== 诊断/自愈任务的公共同步设施 =====
    // 两个任务都必须等模组就绪后再碰 AT 通道，这里用板子的网络事件回调发信号，
    // 而不是跨任务去读 modem_ 指针（避免数据竞争）：
    //   BIT0 = NetworkEvent::Connected —— 4G 已注册联网（GNSS 测试要等这个）
    //   BIT1 = NetworkEvent::Connecting —— 此刻驱动已创建好 modem_（自愈看门狗要等这个，
    //          因为掉网卡死时永远等不到 Connected）
    EventGroupHandle_t net_event_group_ = nullptr;
    NetworkEventCallback user_network_event_callback_;  // 应用层注册的回调，原样透传
#endif

#if GNSS_TEST
    // ===== GNSS 定位测试（config.h: GNSS_TEST=1 时生效）=====
    void GnssTestTask() {
        ESP_LOGI(TAG, "[GNSS] 等 4G 注册成功后再开始测试...");
        auto bits = xEventGroupWaitBits(net_event_group_, BIT0, pdTRUE, pdFALSE, pdMS_TO_TICKS(300000));
        if (!(bits & BIT0)) {
            ESP_LOGE(TAG, "[GNSS] 等待 4G 网络超时（5 分钟），测试取消");
            return;
        }
        // 注册成功后驱动还会连发「版本/IMEI/ICCID」三条查询，等它们发完再碰 AT 通道
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (!modem_) {
            ESP_LOGE(TAG, "[GNSS] 模组未就绪，测试取消");
            return;
        }
        StartMl307GnssTest(modem_->GetAtUart());
    }

    static void GnssTestTaskEntry(void* arg) {
        static_cast<Es8311Ml3074gVoiceBoard*>(arg)->GnssTestTask();
        vTaskDelete(nullptr);
    }
#endif

#if NET_HEAL_ENABLE
    // ===== 4G 掉网 / SIM 锁死自愈看门狗 =====
    void NetHealWatchdogTask() {
        // 等 Connecting：它保证发生在 modem_ 赋值之后（见 Ml307Board::NetworkTask），
        // 而且掉网卡死时也会发出，不像 Connected 那样可能永远等不到。
        ESP_LOGI(TAG, "[NET-HEAL] 等模组检测完成...");
        auto bits = xEventGroupWaitBits(net_event_group_, BIT1, pdTRUE, pdFALSE, pdMS_TO_TICKS(300000));
        if (!(bits & BIT1)) {
            ESP_LOGE(TAG, "[NET-HEAL] 等待模组检测超时（5 分钟），自愈看门狗不启动");
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(3000));  // 让驱动的首轮 CPIN/CEREG 查询先发完
        if (!modem_) {
            ESP_LOGE(TAG, "[NET-HEAL] 模组未就绪，自愈看门狗不启动");
            return;
        }
        StartMl307NetHealTask(modem_->GetAtUart());
    }

    static void NetHealTaskEntry(void* arg) {
        static_cast<Es8311Ml3074gVoiceBoard*>(arg)->NetHealWatchdogTask();
        vTaskDelete(nullptr);
    }
#endif

    void InitializeCodecI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &codec_i2c_bus_));

        // ⚠️ AUDIO_CODEC_ES8311_ADDR = ES8311_CODEC_DEFAULT_ADDR = 0x30 是「8 位形式」。
        //    IDF 的 i2c_master_probe() 要 7 位地址，必须 >>1 后传 0x18 ——
        //    否则一定 NAK，然后误报「codec 不存在」。
        //    （esp_codec_dev 内部也是 dev_addr_length=7 且 device_address = addr >> 1）
        const uint8_t addr_7bit = AUDIO_CODEC_ES8311_ADDR >> 1;
        esp_err_t probe = i2c_master_probe(codec_i2c_bus_, addr_7bit, 1000);
        if (probe == ESP_OK) {
            ESP_LOGI(TAG, "ES8311 found on I2C bus at 7-bit addr 0x%02X", addr_7bit);
        } else {
            // 只在探测失败时提示，且不视为致命错误：
            // 本探测跑在 codec 上电稳定之前时可能偶发 NAK，
            // 真正的失败会在 esp_codec_dev 初始化时暴露出来。
            ESP_LOGW(TAG, "ES8311 not answering on I2C (addr 0x%02X): %s",
                     addr_7bit, esp_err_to_name(probe));
        }
    }

    void Initialize4GModule() {
        // PWRKEY：输出，先放到「释放」电平
        gpio_config_t pwr_conf = {
            .pin_bit_mask = 1ULL << ML307_POWER_PIN,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&pwr_conf));
        gpio_set_level(ML307_POWER_PIN, ML307_POWER_ACTIVE_LEVEL ? 0 : 1);

        // STATE：输入检测模组运行状态（开机后输出高电平）
        gpio_config_t state_conf = {
            .pin_bit_mask = 1ULL << ML307_STATE_PIN,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_ENABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&state_conf));

        vTaskDelay(pdMS_TO_TICKS(50));
        const int state_before = gpio_get_level(ML307_STATE_PIN);
        ESP_LOGI(TAG, "[4G] PWRKEY=GPIO%d(active=%d) STATE=GPIO%d=%d mode=%d",
                 ML307_POWER_PIN, ML307_POWER_ACTIVE_LEVEL, ML307_STATE_PIN, state_before,
                 ML307_POWERKEY_MODE);

#if ML307_POWERKEY_MODE == 0
        ESP_LOGI(TAG, "[4G] mode 0: PWRKEY 不干预，保持释放");
#elif ML307_POWERKEY_MODE == 1
        if (state_before == 0) {
            ESP_LOGI(TAG, "[4G] mode 1: STATE==0 -> PWRKEY 拉低 %d ms 后释放",
                     ML307_POWERKEY_PULSE_MS);
            gpio_set_level(ML307_POWER_PIN, ML307_POWER_ACTIVE_LEVEL);
            vTaskDelay(pdMS_TO_TICKS(ML307_POWERKEY_PULSE_MS));
            gpio_set_level(ML307_POWER_PIN, ML307_POWER_ACTIVE_LEVEL ? 0 : 1);
            vTaskDelay(pdMS_TO_TICKS(3000));
            ESP_LOGI(TAG, "[4G] mode 1: 脉冲后 STATE=%d", gpio_get_level(ML307_STATE_PIN));
        } else {
            ESP_LOGI(TAG, "[4G] mode 1: STATE==1 -> 模组已运行，跳过 PWRKEY");
        }
#elif ML307_POWERKEY_MODE == 2
        // 一直拉低即「有电就开机」，模组会并行启动；
        // 这里不再额外等待，交给 AtModem::Detect 的重试去等模组就绪。
        ESP_LOGI(TAG, "[4G] mode 2: PWRKEY 一直拉低（有电即工作）");
        gpio_set_level(ML307_POWER_PIN, ML307_POWER_ACTIVE_LEVEL);
#else
#error "ML307_POWERKEY_MODE must be 0, 1 or 2"
#endif
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            Application::GetInstance().ToggleChatState();
        });
    }

#if SOA_CAN_ENABLE
    // ===== CAN (TWAI) 整车总线 =====
    // 引脚/波特率/STB 极性全部来自板级 config.h，核心代码 (main/soa/Sop/esp32_can/can_bus.c)
    // 不认识任何板子。
    void InitializeCan() {
        const can_bus_config_t can_cfg = {
            .tx_gpio = CAN_TX_PIN,
            .rx_gpio = CAN_RX_PIN,
            .stb_gpio = CAN_STB_PIN,
            .stb_standby_level = CAN_STB_STANDBY_LEVEL,
            .bitrate = CAN_BITRATE,
        };

        esp_err_t err = can_init(&can_cfg);
        if (err != ESP_OK) {
            // CAN 起不来表示总线侧不可用，但语音功能不应该因此瘫痪
            ESP_LOGE(TAG, "[CAN] 初始化失败: %s", esp_err_to_name(err));
            return;
        }
        ESP_LOGI(TAG, "[CAN] 就绪: TX=GPIO%d RX=GPIO%d STB=GPIO%d @%d bps",
                 CAN_TX_PIN, CAN_RX_PIN, CAN_STB_PIN, CAN_BITRATE);

#if SOA_INIT_ON_BOOT
        // 拉起 SOA 框架 (router + CAN/IPC/UART 传输层)，然后让 HUI 节点上线：
        // 发 SlaveHeartbeat / 收 HostHeartbeat / 向 Central 发 HuiRead|Write|Ioctl。
        // 详见 main/soa/hui.h。
        soa_init();
        HuiNode_Init();
#endif
    }
#endif

public:
    Es8311Ml3074gVoiceBoard()
        : Ml307Board(ML307_TX_PIN, ML307_RX_PIN), boot_button_(BOOT_BUTTON_GPIO) {
        InitializeCodecI2c();
#if SOA_CAN_ENABLE
        InitializeCan();
#endif
        Initialize4GModule();
        InitializeButtons();
#if GNSS_TEST || NET_HEAL_ENABLE
        // 先装上「网络事件」通知钩子；应用稍后会用真正的回调再调一次，行为不变
        net_event_group_ = xEventGroupCreate();
        SetNetworkEventCallback(nullptr);
#endif
    }

    // 协议连接前的准备：按 HUI 主机命令手册生成 MCP 工具。
    //
    // 为什么必须在这里：后端在协议连上后约 190ms 就会发 tools/list，而它不理会
    // notifications/tools/list_changed —— 晚于第一次 tools/list 注册的工具在 AI
    // 眼里等于不存在。此刻 4G 已通、总线上电也已稳定，只是还没连协议。
    // 内部会 read 1000（最多阻塞 2s，读不到就本次放弃，只注册本机工具）。
    virtual void PrepareBeforeConnect() override {
#if SOA_INIT_ON_BOOT
        HuiMcp_Init();
#endif
    }

    virtual void SetNetworkEventCallback(NetworkEventCallback callback) override {
#if GNSS_TEST || NET_HEAL_ENABLE
        user_network_event_callback_ = std::move(callback);
        Ml307Board::SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
            if (net_event_group_ != nullptr) {
                if (event == NetworkEvent::Connected) {
                    xEventGroupSetBits(net_event_group_, BIT0);
                } else if (event == NetworkEvent::Connecting) {
                    xEventGroupSetBits(net_event_group_, BIT1);
                }
            }
            if (user_network_event_callback_) {
                user_network_event_callback_(event, data);
            }
        });
#else
        Ml307Board::SetNetworkEventCallback(std::move(callback));
#endif
    }

    virtual void StartNetwork() override {
        Ml307Board::StartNetwork();
#if GNSS_TEST
        // GNSS 测试任务：等 NetworkEvent::Connected 之后开始轮询 GGA
        if (xTaskCreate(GnssTestTaskEntry, "gnss_test_start", 3072, this, 4, nullptr) != pdPASS) {
            ESP_LOGE(TAG, "[GNSS] 创建测试任务失败");
        }
#endif
#if NET_HEAL_ENABLE
        // 自愈看门狗：等模组检测完成（Connecting）后开始定期检查网络状态
        if (xTaskCreate(NetHealTaskEntry, "net_heal_start", 3072, this, 4, nullptr) != pdPASS) {
            ESP_LOGE(TAG, "[NET-HEAL] 创建自愈任务失败");
        }
#endif
    }

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(
            codec_i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN, AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR,
            AUDIO_CODEC_USE_MCLK != 0 /* use_mclk */, false /* pa_inverted */);
        return &audio_codec;
    }
};

DECLARE_BOARD(Es8311Ml3074gVoiceBoard);
