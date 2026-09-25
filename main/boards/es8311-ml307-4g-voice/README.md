# ES8311 + ML307C 4G 纯语音定制板 (ESP32-S3)

单麦克风/单扬声器的 ESP32-S3 语音助手板，硬件只有两部分：

- **语音**：ES8311 音频编解码 + NS4150B 功放（I2S 全双工）
- **4G**：ML307C Cat.1 模组（UART AT 通道，PWRKEY 开机）

**无显示屏、无触摸、无摄像头。** 固件不 override `GetDisplay()` / `GetBacklight()`，
基类返回内置 `NoDisplay` 与 `nullptr`，上层调用点已有保护。

## 硬件资源

| 功能 | 引脚 |
| --- | --- |
| 按键 (BOOT) | IO0（上拉，按下拉低） |
| 指示灯 (WS2812) | IO38 |
| 功放使能 PA_EN（高电平开启） | IO37 |
| I2S MCLK | IO4 |
| I2S BCLK / SCLK | IO15 |
| I2S LRCK (WS) | IO6 |
| I2S DOUT（ESP32 → ES8311 `DSDIN`，播放） | **IO5** ⚠️ 见下 |
| I2S DIN（ES8311 `ASDOUT` → ESP32，录音） | **IO7** ⚠️ 见下 |
| Codec I2C SDA / SCL | IO17 / IO18（ES8311 7 位地址 0x18） |
| 4G 模组 UART | ESP TX = IO44 / ESP RX = IO43 |
| 4G 模组 PWRKEY | IO47（低电平有效） |
| 4G 模组 STATE | IO48（开机后应为高） |
| USB-Serial/JTAG 控制台 | IO19 (D-) / IO20 (D+)（必须保留，不可挪用） |
| 输入电压 ADC | IO1（固件暂未使用） |
| CAN (TWAI) | IO39 (TX) / IO40 (RX) / IO41 (STB)，500 kbps ✅ 已启用 |

## CAN / SOA / HUI 节点（从 MIO 整车工程移植）

本板挂到整车 CAN 总线上，作为 SOA（Service-Oriented Architecture）框架里的 **HUI 节点**，
这也是这块板和中央 ECU（Central）对接的唯一节点。相关代码在 `main/soa/`
（框架本体 + `canard`/Cyphal + `hui` + 本板适配层），DSDL 头文件在 `main/dsdl/`。
