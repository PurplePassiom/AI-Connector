#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

// ============================================================================
// ES8311 + ML307C 4G 纯语音板 (ESP32-S3)
// 硬件只有「语音 + 4G」两部分：ES8311 单编解码 + ML307C Cat.1 模组，
// 无显示屏、无触摸、无摄像头。
// ============================================================================

// ===== 音频编解码 (ES8311) =====
#define AUDIO_INPUT_SAMPLE_RATE  24000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

// I2S 引脚（ES8311 单编解码，收发共用一组 I2S 时钟）
#define AUDIO_I2S_GPIO_MCLK GPIO_NUM_4    // MCLK
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_15   // BCLK / SCLK
#define AUDIO_I2S_GPIO_WS   GPIO_NUM_6    // LRCK / WS

// ⚠️⚠️ 两根数据脚是「交叉」的，不要按常规写法改回去！
// 实测 PCB 走线（万用表确认）：
//     ES8311 的 DSDIN（DAC 数据输入，播放用） ——接的是—— ESP32 的 IO5
//     ES8311 的 ASDOUT（ADC 数据输出，录音用）——接的是—— ESP32 的 IO7
// 所以这里必须按【实际走线】写，即 ESP 的 I2S 信号这样分配：
//     DOUT（播放，ESP 输出）-> 用 IO5（因为 codec 的 DSDIN 在 IO5）
//     DIN （录音，ESP 输入）-> 用 IO7（因为 codec 的 ASDOUT 在 IO7）
//
// 如果按「常规」写成 IO7=DOUT / IO5=DIN，后果是：codec 的 DSDIN 被接到一个配置为
// 输入脚的 IO5 上 -> DAC 收不到任何播放数据 -> 只能一直转换全零 ->
// OUTP/OUTN 恒为 1.65V 直流无变化 -> 表现就是「完全无声」（已踩过这个坑）。
// ESP32 任何 GPIO 都能经 GPIO matrix 做 I2S 信号，走线交叉不影响功能。
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_5    // 播放: ESP32 -> ES8311 DSDIN
#define AUDIO_I2S_GPIO_DIN  GPIO_NUM_7    // 录音: ES8311 ASDOUT -> ESP32

// ES8311 是 Slave 模式，必须有主时钟。
//   1 = 由 ESP32 在 AUDIO_I2S_GPIO_MCLK 上输出 MCLK（本板实测正确）
//   0 = 不输出 MCLK，改用 BCLK(SCLK) 作为 codec 的内部主时钟
//       （驱动会置 REG01 bit7=1、pre_multi=×8：BCLK 768kHz × 8 = 6.144MHz = 256fs）
// 实测本板 IO4 上 MCLK = 6.144MHz 正常，保持 1 即可。
#define AUDIO_CODEC_USE_MCLK 1

// 功放使能（NS4150B CTRL，高电平使能，播放时拉高）
// 已实测：IO37 -> NS4150B CTRL 走线正常（1Hz 方波能传到功放 CTRL 脚），
// 所以八线 PSRAM 那个“IO33~IO37 未引出”的担心在本模组上不成立，可以放心用。
#define AUDIO_CODEC_PA_PIN GPIO_NUM_37

// ES8311 寄存器配置用 I2C
#define AUDIO_CODEC_I2C_SDA_PIN  GPIO_NUM_17
#define AUDIO_CODEC_I2C_SCL_PIN  GPIO_NUM_18
#define AUDIO_CODEC_ES8311_ADDR  ES8311_CODEC_DEFAULT_ADDR

// ===== 按键 =====
#define BOOT_BUTTON_GPIO GPIO_NUM_0

// ===== 指示灯 (WS2812 单总线全彩灯) =====
#define BUILTIN_LED_GPIO GPIO_NUM_38

// ===== ML307C 4G 模组 =====
// 串口经 TXB0104 电平转换器。实测（发 AT 看有无 OK）确定的对应关系：
//     ESP TX(GPIO44) -> 模组 RXD
//     ESP RX(GPIO43) <- 模组 TXD
// 写成 43/44 就完全无应答。
#define ML307_TX_PIN GPIO_NUM_44          // ESP32 TX -> B1 -> A1 -> 模组 RXD
#define ML307_RX_PIN GPIO_NUM_43          // ESP32 RX <- B3 <- A3 <- 模组 TXD

// PWRKEY：模组开/关机按键，低电平有效（拉低约 1.5~2.5s 切换开关机）
#define ML307_POWER_PIN GPIO_NUM_47
#define ML307_POWER_ACTIVE_LEVEL 0
// 上电时如何驱动 PWRKEY：
//   0 = 不干预（保持释放电平）
//   1 = 条件脉冲：仅当 STATE 为低时才拉低 ML307_POWERKEY_PULSE_MS 后释放
//       （模组锁存保持开机，且不会对已运行的模组误发一次“按键”）
//   2 = 一直拉低（有电即工作）
// 实测本板模组冷启动时 STATE=0（不会自启动），必须靠 PWRKEY 开机：
// 拉低 2.5s 后 STATE 变 1 并锁存（之后即使复位 ESP 也保持开机）。
#define ML307_POWERKEY_MODE 2
// 模式 1 的 PWRKEY 拉低时长（ms）
#define ML307_POWERKEY_PULSE_MS 2500

// STATE：模组运行状态检测（开机后输出高电平）
#define ML307_STATE_PIN GPIO_NUM_48

// ===== GNSS 定位测试（板级诊断开关）=====
// ML307C-GC-CN 内置 GNSS，AT 命令见《GNSS用户手册》（docs/GNSS用户手册_4G系列_primary.pdf）
//   0 = 关闭。固件行为与原来完全一致，不会产生任何额外 AT 命令。
//   1 = 打开。4G 注册成功后等 5s，然后开 GNSS 并每 GNSS_TEST_POLL_MS 轮询
//       AT+MGNSSNMEA="GGA"，串口打印 UTC 时间 / 经纬度 / 定位状态。
//       测试任务只发只读查询命令，应用（语音、对话）照常运行，不会破坏 4G 业务。
// 日常使用保持 0；要做 GNSS 测试（读 UTC/经纬度）时改成 1 再重编烧录。
#define GNSS_TEST 0

// 定位模式（手册 3.2 AT+MGNSS）：1 = 连续定位（推荐，测试用这个）
//                              2 = 单次定位（定位成功后模组自动关闭 GNSS）
// 注：手册 3.4 注明「单次定位模式下仅支持自动上报」，故用 2 时轮询不一定拿到数据，
//     本测试任务在 mode=2 下拿到第一次有效定位后就结束。
#define GNSS_TEST_MODE 1

// 轮询间隔(ms)。NMEA 数据刷新周期最快 1s，1000 即可，不要设得太小
#define GNSS_TEST_POLL_MS 1000

// 搜星系统掩码（0 = 不下发这条配置命令，沿用模组默认值）
//   bit0 GPS / bit1 BDS / bit2 QZSS / bit3 GLONASS / bit4 Galileo
//   3 = GPS + 北斗（国内推荐，手册 3.1 示例）
#define GNSS_TEST_CONSTELLATION_MASK 3

// 开 GNSS 之前是否先软重启接收机（手册 3.5 AT+MGNSSRST）：
//   -1 = 不重启（默认）
//    0 = 冷启动（清空星历，最慢，首次定位可能 30~60s+）
//    1 = 温启动
//    2 = 热启动（最快，需要星历仍有效）
#define GNSS_TEST_RST_MODE -1

// 是否开 AGNSS 辅助定位（手册 3.8，必须在开 GNSS 之前使能）：
//   1 = 开（走 4G 流量下载星历/参考位置，可大幅缩短首次定位时间）
//   0 = 关（纯 GNSS 冷启动，首次定位可能 30~90s）
#define GNSS_TEST_AGNSS 0

// ===== 4G 掉网 / SIM 锁死自愈看门狗 =====
// 背景：ML307C 会偶发进入「掉网 + SIM 不可用」的锁死状态，日志特征是同时出现
//   +CEREG: 0（模组主动注销，不是“正在搜网”）  +CSQ: 99,99（射频读不到信号）
//   +CME ERROR: 30（无网络服务） 与 +CME ERROR: 13（SIM 失败）成对出现
// 之后上层每次重试（取版本 → 读 COPS/CSQ/MCCID）就会反复打印这对错误；
// 而**模组自己永远不会重试**，只能重新初始化射频与 SIM 接口（AT+CFUN=0 → AT+CFUN=1）
// 或真正断电。这个看门狗就是干这件事的。
//   0 = 关；1 = 开（每 NET_HEAL_CHECK_INTERVAL_MS 查一次，连续异常就自愈）
#define NET_HEAL_ENABLE 1
// 检查间隔(ms)。查询很轻（就两条 AT），20s 足够
#define NET_HEAL_CHECK_INTERVAL_MS 20000
// 连续几次「查到了但状态异常」才动手（× 检查间隔）。只统计“查询成功但结果不对”，
// 命令超时不算，所以不会因为音频流占着 AT 通道而误伤正在跑的 4G 业务
#define NET_HEAL_FAIL_THRESHOLD 2
// AT+CFUN=0（关射频/SIM）后等多久再发 AT+CFUN=1
#define NET_HEAL_CFUN_OFF_MS 3000
// AT+CFUN=1 后等多久再复检（等重新注册）
#define NET_HEAL_CFUN_ON_WAIT_MS 10000
// 自检开关：上电先正常跑 NET_HEAL_SELF_TEST_DELAY_MS，然后主动 AT+CFUN=0 模拟掉网，
// 用来验证看门狗能否自己发现并恢复（会短暂失去 4G）。验证完请改回 0
#define NET_HEAL_SELF_TEST 0
#define NET_HEAL_SELF_TEST_DELAY_MS 20000
// 实验性：CFUN 重初始化连续 NET_HEAL_MAX_CFUN_TRIES 次无效时，用 PWRKEY 给模组真正断电重启
// （相当于拔插 USB）。时序不对可能把模组关掉且需手动拔电恢复，默认关闭
#define NET_HEAL_POWER_CYCLE 0
#define NET_HEAL_MAX_CFUN_TRIES 3

// ===== CAN (TWAI) 整车总线 =====
// 这三个脚以前只是「引出来了但固件没用」，现在正式使能（SOA 框架走这条总线）。
//   收发器 TXD <- ESP 的 CAN_TX
//   收发器 RXD -> ESP 的 CAN_RX
//   收发器 STB <- ESP 的 CAN_STB（待机控制，高电平=待机，低电平=正常通信）
// 引脚与 MIO 工程 (整车 CAN) 对齐，波特率也是整车统一的 500 kbps。
#define SOA_CAN_ENABLE          1

#define CAN_TX_PIN              GPIO_NUM_39
#define CAN_RX_PIN              GPIO_NUM_40
#define CAN_STB_PIN             GPIO_NUM_41
// STB 引脚在「待机」时的电平。绝大多数收发器（TJA1051/1054、SIT1042 等）是
// 高电平待机；若本板收发器相反，把它改成 0 即可。
#define CAN_STB_STANDBY_LEVEL   1
#define CAN_BITRATE             500000

// 上电就拉起 SOA 框架（router + CAN/IPC/UART 传输层）并让 HUI 节点上线
//   0 = 只初始化 CAN 驱动（收发 CAN 帧可用，但不建 HUI 节点、不发心跳）
//   1 = 额外调用 soa_init() + HuiNode_Init()
// 按主机命令手册生成的 MCP 工具不在这里注册，而是走 Board::PrepareBeforeConnect()
// （连协议之前），详见 main/soa/hui_mcp.h。
// HUI 节点号见 main/soa/soa_types.h 的 HUI_NODE_ID（当前 6）。
#define SOA_INIT_ON_BOOT        1

// ===== HUI 命令手册来源 =====
// 默认走 flash（main/soa/manuals/*.json 打进 assets 分区，无网也能用）。
// 以后有了自己的服务器，把 Kconfig 的 HUI_MANUAL_SOURCE 切成 HTTP_ONLY/FLASH_FIRST，
// 并在目录级 config.json 的 sdkconfig_append 里加一行（%s = 设备 ID）：
//   "CONFIG_HUI_MANUAL_URL=\"http://<服务器>/api/hui/manual?device_id=%s\""
// 只用 flash 时 CONFIG_HUI_MANUAL_URL 留空即可，不需要配。
// 手册（参考数据）见 main/soa/manuals/ 与 docs/hui-command-manual_zh.md。

// ===== 已引出但固件暂未使用，仅作引脚记录 =====
// USB 控制台 (USB-Serial/JTAG)：D- = GPIO19 / D+ = GPIO20（必须保留，不可挪用）
// 输入电压 ADC：GPIO1

#endif // _BOARD_CONFIG_H_
