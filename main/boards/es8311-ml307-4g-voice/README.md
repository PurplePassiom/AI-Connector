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
| CAN (TWAI) | IO39 (TX) / IO40 (RX) / IO41 (STB)（固件暂未使用） |

## 注意

- ⚠️ **两根 I2S 数据脚在 PCB 上是交叉的**：ES8311 的 `DSDIN`（DAC 数据输入，播放用）接在 **IO5**，`ASDOUT`（ADC 数据输出，录音用）接在 **IO7**。因此 `config.h` 里必须写
  `AUDIO_I2S_GPIO_DOUT = IO5`、`AUDIO_I2S_GPIO_DIN = IO7`。
  若按「常规」写成 IO7=DOUT / IO5=DIN，codec 的 `DSDIN` 会落到一个被配置成输入脚的 IO5 上，
  DAC 收不到播放数据、只能一直转换全零，`OUTP/OUTN` 恒为 1.65V 直流无变化 —— 现象就是
  **完全无声**（本板实测踩过这个坑）。ESP32 任何 GPIO 都能经 GPIO matrix 做 I2S 信号，
  所以在固件里交换这两个脚即可，不需要改硬件。
- **`CONFIG_SPIRAM` 必须保持 `n`**：本板 PA_EN 用了 IO37，而 Octal(八线) PSRAM 工作在
  IO33~IO37 这组管脚上。若改成 `CONFIG_SPIRAM=y` + `CONFIG_SPIRAM_MODE_OCT=y`，MSPI 会接管
  这组脚，PA 使能（以及 IO38 等邻近脚）会失控。`config.json` 里已经写死 `CONFIG_SPIRAM=n`。
  （注：`CONFIG_SPIRAM=n` 只是不去使用 PSRAM，并不能“释放”这些脚 —— 这点与引脚能否正常当
  GPIO 使用无关；本板实测 IO37 作为普通 GPIO 驱动功放是正常的。）
- 控制台已切到 USB-Serial/JTAG（`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`），因为 UART0 的
  默认引脚 IO43/IO44 被 4G 模组占用。
- 4G 串口的 TX/RX 顺序经实测确定（发 `AT` 看有没有 `OK`）：**TX=IO44 / RX=IO43**。
  互换后完全无应答，不要凭模组网名去改。
- 4G 模组实测**不会自行启动**（冷启动 STATE=0），必须靠 PWRKEY 开机：拉低 2.5s 后 STATE 变高
  并锁存。上电行为由 `config.h` 的 `ML307_POWERKEY_MODE` 控制：
  - `2`（当前默认）＝ PWRKEY 一直拉低，有电即工作；不会额外等待，模组与 ESP 并行启动，
    由 `AtModem::Detect` 的重试去等它就绪。
  - `1` ＝ 条件脉冲（更保守）：仅当 STATE 为低时才拉低 2.5s 后释放，不会对已运行的模组
    误发一次「按键」；代价是构造函数里会阻塞约 5.5s。
  - `0` ＝ 不干预。
  若置 `2` 后发现模组被「长按」逻辑关机（STATE 反复 1→0），请改用 `1`。
- 音频若「I2C 寄存器正常、I2S 有波形但完全没声音」，先把 `config.h` 里的
  `AUDIO_CODEC_USE_MCLK` 改成 `0` 再试：那说明 MCLK（IO4）走线/pad 有问题，
  改用 BCLK 当 codec 内部主时钟可以绕过（驱动会置 REG01 bit7=1、pre_multi=×8，
  BCLK 768kHz × 8 = 6.144MHz = 256fs）。
- 功放使能默认高电平有效。若硬件为低电平使能，把 `GetAudioCodec()` 里 `Es8311AudioCodec`
  最后一个参数 `pa_inverted` 改为 `true`。
- 本板不带 SIM 自愈逻辑：ML307 驱动会自行重试检测。若实测遇到「模组能应答 AT、但 SIM 一直
  报 `+CME ERROR: 13/14`」，那是模组 SIM 接口锁死且自己不会重试，可加一次
  `AT+CFUN=0` → 等 3s → `AT+CFUN=1` 的重建流程（参考 `es8311-ml307-s3` 板的 `SimHealTask`）。

## GNSS 定位测试

模组（ML307C-GC-CN）内置 GNSS。AT 命令出处：中移物联网《GNSS用户手册》V1.0.1，
本地副本 `docs/GNSS用户手册_4G系列_primary.pdf`。固件里有一套板级测试开关，
打开后自动读 **UTC 时间 / 经纬度 / 定位状态**，不影响语音和对讲功能。

### 开关（`config.h`）

| 宏 | 当前值 | 说明 |
| --- | --- | --- |
| `GNSS_TEST` | `1` | `0` 关闭（完全不产生额外 AT 命令，行为与原来一致）；`1` 打开测试任务 |
| `GNSS_TEST_MODE` | `1` | `AT+MGNSS` 的 `<ctrl>`：1 连续定位；2 单次定位（定位成功模组自动关 GNSS，任务打印到第一次定位即结束） |
| `GNSS_TEST_POLL_MS` | `1000` | 轮询间隔，NMEA 刷新周期最快 1s |
| `GNSS_TEST_CONSTELLATION_MASK` | `3` | 搜星系统掩码 bit0 GPS / bit1 BDS / bit2 QZSS / bit3 GLONASS / bit4 Galileo；`0` = 不下发 |
| `GNSS_TEST_RST_MODE` | `-1` | `-1` 不重启 / `0` 冷启动 / `1` 温启动 / `2` 热启动 |
| `GNSS_TEST_AGNSS` | `0` | `1` = 先使能 AGNSS 辅助定位（走 4G 流量，可大幅缩短首次定位时间） |

时序：4G 注册成功（日志 `Network connected`）→ 等 5s（让驱动先发完版本/IMEI/ICCID 查询）
→ 开 GNSS → 每 `GNSS_TEST_POLL_MS` 打印一行。

### 串口输出

```
I (xxxxx) GnssTest: ==================== ML307 GNSS 定位测试 ====================
I (xxxxx) GnssTest: AT >> AT+MGNSS?
I (xxxxx) GnssTest: GNSS 当前状态: 0 (已关闭)
I (xxxxx) GnssTest: AT >> AT+MGNSSCFG="constellation/mask",3
I (xxxxx) GnssTest: AT >> AT+MGNSS=1
I (xxxxx) GnssTest: URC +MGNSSURC: GNSS 已开启 (state=1)
I (xxxxx) GnssTest: AT >> AT+MGNSSNMEA="GGA"
W (xxxxx) GnssTest: [未定位 4s] UTC -- | fix=0 定位用卫星=0 hdop=0.00 | 可见卫星 无（室内/遮挡处可能一直定不上，请到室外开阔处，首次定位一般 30~90s）
W (xxxxx) GnssTest: [未定位 66s] UTC 01:23:36.274 | fix=0 定位用卫星=0 hdop=0.00 | 可见卫星 GPS 4颗(最强30 dBHz)（……）
I (xxxxx) GnssTest: [已定位 12s] UTC 09:59:05.729 | 纬度 40.07901N (NMEA 4004.74005N) | 经度 116.23664E (NMEA 11614.19613E) | fix=1(单点定位) 定位用卫星=10 hdop=0.85 海拔=53.5m | 可见卫星 GPS 8颗(最强45 dBHz)
```

- 第 3 行如果直接是 `+CME ERROR: 957 (GNSS设备错误)` / `951` / `953`，说明该模组不带 GNSS
  或 GNSS 固件未使能 / 天线没接，测试任务会打印错误码后退出，**不影响固件其它功能**。
- 室内、金属壳内、天线没接时可能一直是「未定位」，请到室外开阔处再试；
  冷启动（`GNSS_TEST_RST_MODE=0`）首次定位最慢，热启动/AGNSS 会快很多。
- 连续定位会持续占用 GNSS 硬件与功率，长时间开着会明显增加功耗。

### 用到的 AT 命令

| 命令 | 用途 |
| --- | --- |
| `AT+MGNSS?` | 查询状态 → `+MGNSS: <status>`（0 关闭 / 1 连续 / 2 单次） |
| `AT+MGNSS=<0\|1\|2>` | 关 / 连续定位 / 单次定位 |
| `AT+MGNSSNMEA="GGA"` | 读最新一条 GGA（本测试主用）→ `+MGNSSNMEA: $GNGGA,<UTC>,<lat>,<N/S>,<lon>,<E/W>,<fix>,<nsat>,<hdop>,<alt>,…` |
| `AT+MGNSSNMEA="RMC"`/`"VTG"`/`"GLL"`/`"GSA"`/`"GSV"` | 其它 NMEA 语句（日期在 RMC 里；GSV = 可见卫星与载噪比，本测试每 5 轮取一次） |
| `AT+MGNSSLOC` | 读定位信息 → `+MGNSSLOC: <UTC>,<lat>,<lon>,<hdop>,<alt>,<fix>,<cog>,<spkm>,<spkn>,<date>,<nsat>,<dtype>` |
| `AT+MGNSSLOC=1` | 定位成功后自动上报 `+MGNSSLOC`（URC） |
| `AT+MGNSSCFG="constellation/mask",<n>` | 搜星系统掩码（bit0 GPS/bit1 BDS/bit2 QZSS/bit3 GLONASS/bit4 Galileo） |
| `AT+MGNSSCFG="nmea/mask",<0-63>` | NMEA 主动上报掩码（bit0 GGA/1 GSV/2 GSA/3 RMC/4 VTG/5 GLL） |
| `AT+MGNSSCFG="nmea/cycle",<1-60>` | NMEA 输出周期（秒） |
| `AT+MGNSSRST=<0\|1\|2>` | 接收机软重启：冷 / 温 / 热启动 |
| `AT+MAGNSSEN=<0\|1>` | AGNSS 辅助定位开关（必须在开 GNSS **之前**使能） |
| `AT+MAGNSSDATA` | 触发下载 AGNSS 辅助数据（消耗流量），完成后 URC `+MAGNSSDATA:` |
| `AT+MAGNSSREFLOC=<lat>,<lon>[,2]` | 参考坐标，缩短首次定位时间（ML307C 只支持 mode=2） |
| URC `+MGNSSURC: "state",<0\|1>` | GNSS 关闭 / 开启 |
| URC `+MGNSSURC: "error",<code>` | 1 状态异常 2 存储错误 3 启动错误 4 AGNSS数据无效 5 AGNSS服务器连接异常 6 AGNSS数据获取异常 7 DGNSS服务器连接异常 8 DGNSS鉴权异常 9 DGNSS数据获取异常 |

常见 `+CME ERROR`（手册 5.1）：950 未知错误 / 951 GNSS参数错误 / 952 GNSS已开启 /
953 GNSS未开启 / 954 GNSS状态忙 / 955 AGNSS状态忙 / 957 GNSS设备错误 / 958 GNSS操作内存失败 /
959 GNSS请求失败。

ML307C 不支持 DGNSS（`AT+MDGNSSEN`、`dgnss/*` 配置），也不支持 `nmea/port`。

### 实现要点（踩坑记录）

- 驱动 `at_uart.cc` 把所有 `+XXX: …` 形式响应当 **URC** 抛给回调（不会进 `GetResponse()`），
  并按逗号切分参数、把纯数字/带小数点字段转成 `int`/`double`。所以 `+MGNSS:`、`+MGNSSNMEA:`
  这些响应必须用 `AtUart::RegisterUrcCallback` 去收。
- **不要用 `AT+MGNSSLOC` 取经纬度**：它的纬度字段是 `2937.1685N` 这种把 N/S 后缀贴在数值上的
  格式，驱动 `std::stod` 一转就把后缀丢了，分不出南/北纬、东/西经。
  用 GGA（`AT+MGNSSNMEA="GGA"`），N/S、E/W 是独立字段不会丢。
- ⚠️ **没定位时模块也会回「看着正常」的 GGA**，实测：
  `$GNGGA,011646.742,0000.00000,N,00000.00000,E,0,00,0.000,0.0,M,0.0,M,,*6D`
  即 UTC 有值（时间已从卫星取到，也可能是空）、经纬度是 `0000.00000`、`fix=0`、定位用卫星=0。
  所以「是否定位成功」只认 GGA 第 6 个字段 `fix_quality >= 1`，**不要拿 UTC 或经纬度字段来判断**
  （第一版用「UTC 非空」判断，结果室内一直误报「已定位 0.00000N」）。
- 转 double 会丢前导 0（`095905.729` → `95905.729`），NMEA 字段宽度固定，按宽度零填充还原：
  时间 `%010.3f`、纬度 `%010.5f`、经度 `%011.5f`（见 `ml307_gnss_test.cc`）。
- 测试任务的每条命令都走驱动同一把命令锁（`AtUart::command_mutex_`），与语音/对话并行不会串包；
  但必须等 4G 注册成功后再开始，否则会和驱动的 `AT+CPIN?`/`AT+CEREG` 轮询抢同一条通道。
  板子用网络事件回调（`NetworkEvent::Connected`）来同步，而不是跨任务读 `modem_` 指针。
- `AT+MGNSSNMEA` 的响应在 `OK` 之前发出，所以 `SendCommand` 返回时数据通常已经到了；
  代码里仍保留最多 500ms 的兜底等待，并用「GGA 序号」判断本轮是否拿到新数据。

### 实测结果（2026-09-24，本板 COM19）

- ✅ AT 通路全部验证通过：`AT+MGNSS?` → `+MGNSS: 0`；`AT+MGNSSCFG="constellation/mask",3` → OK；
  `AT+MGNSS=1` → OK 并收到 `+MGNSSURC: "state",1`；`AT+MGNSSNMEA="GGA"/"GSV"` 均能读到数据。
  本模组（ML307C-GC-CN-MBRH0S00）**确实带 GNSS**。
- ✅ UTC 可读：室内已能读到 `UTC 01:23:36.274`（同一时刻本地 09:23，UTC+8 对得上，说明时间来自卫星）。
- ⚠️ 经纬度**尚未在真实定位下验证**：室内（无天空视野）只看到 GPS 4 颗、载噪比 30~35 dBHz，
  北斗 0 颗，始终 `fix=0` 定不上位。请把天线放到室外/窗边再抓一次日志。
- 📌 ML307C 默认就以 1 Hz 主动输出整组 NMEA（GGA/GSA/RMC/VTG/GLL/GSV，手册说 `nmea/mask`
  默认是 0，实测不成立），所以 AT 口日志比较吵；原始 `$G...` 行不影响解析，
  带 `+MGNSSNMEA:` 前缀的才是轮询响应。
- 若想更快定上：把 `GNSS_TEST_CONSTELLATION_MASK` 改大（如 `0x1F` 打开 GLONASS/Galileo），
  或开 `GNSS_TEST_AGNSS=1`（需要 4G 数据通，走流量下载星历）。

## 4G 掉网 / SIM 锁死自愈（NET_HEAL 看门狗）

### 现象

日志里按固定节奏（上层重试周期 10/20/40/80s）出现成对错误：

```
I (12826) AtUart: << +CME ERROR: 30      ← AT+COPS? 报「无网络服务」
I (12826) AtUart: << +CSQ: 99,99         ← 射频读不到任何信号
I (12826) AtUart: << +CME ERROR: 13      ← AT+MCCID 读 SIM 失败
```

同时 `W Ml307Board: Network disconnected`、云端连不上（`DNS resolution failed`）、
界面一直报「检查新版本失败」。

### 实测根因（COM19 抓包，本模组每次开机都会复现）

```
 1306  << +CEREG: 2,1,"3322","03A5E243",7   ← 注册成功，CSQ 18~22（信号良好）
 3326  << +CEREG: 0                         ← ⚠️ 模组自己注销（stat=0 = 直接不搜网）
 3326  Ml307Board: Network disconnected
```

掉网时刻紧跟在上层第一条 HTTPS 数据请求（`AT+MHTTPCREATE`/`AT+MHTTPREQUEST`）之后，
之后射频（`+CSQ: 99,99`）和 SIM（`+CME ERROR: 13`）同时不可用，而**模组自己永远不会重试**。
这就是那对 `30` / `13` 的真正来源：它们是上层重试时的 `AT+COPS?` / `AT+CSQ` / `AT+MCCID`
打在已经死掉的模组上，**不是这三个命令本身有问题**。

两个容易踩的点：
- `ML307_POWERKEY_MODE=2` 让模组一直带电，所以**复位/重烧 ESP32 不会重置模组**，
  这个坏状态会跨 ESP 复位一直保留（观测上就是“报错赖着不走”）。
- 驱动 `Ml307Board::NetworkTask` 在注册阶段会阻塞在 `WaitForNetworkReady()`
  （`timeout_ms=-1` → portMAX_DELAY），所以掉网后固件自己不会重新注册，必须外部把它拉回来。

### 修复：`NET_HEAL_ENABLE`（默认开）

每 20s 查一次 `AT+CEREG?` + `AT+CSQ`，**连续 2 次「查询成功但结果异常」**（即 40s）
就执行 `AT+CFUN=0` → 3s → `AT+CFUN=1`，重新初始化射频与 SIM 接口。本板实测（COM19）：

```
 24296 NetHeal: 状态检查: CEREG=0(未注册且不搜网) CSQ=99 -> 异常
 24296 NetHeal: 网络异常第 1/2 次，再观察一轮
 44296 NetHeal: 检测到模组卡死（掉网 + SIM 不可用）…执行自愈流程
 44296 NetHeal: → 重新初始化模组射频/SIM：AT+CFUN=0 → 3000ms → AT+CFUN=1
 49356 AtUart: << +CEREG: 1,"3322","03A5E243",7   ★ 自动重新注册
 49356 Ml307Board: Network connected / Application: Network connected
 58286 NetHeal: 恢复后 AT+CPIN? 正常（SIM 接口有响应）
 58286 NetHeal: 恢复后 AT+MCCID 成功（SIM 可读）
 78286 NetHeal: 状态检查: CEREG=1(已注册(本地)) CSQ=19 -> 正常
```

自愈后应用层也自动恢复（`MQTT: Connected to endpoint`、`Activation done`、MQTT 正常收发），
全程不用断电。可调项都在 `config.h`：

| 宏 | 默认 | 说明 |
| --- | --- | --- |
| `NET_HEAL_ENABLE` | 1 | 总开关（0 时一句额外 AT 都不发） |
| `NET_HEAL_CHECK_INTERVAL_MS` | 20000 | 检查间隔 |
| `NET_HEAL_FAIL_THRESHOLD` | 2 | 连续几次异常才动手 |
| `NET_HEAL_CFUN_OFF_MS` / `_ON_WAIT_MS` | 3000 / 10000 | CFUN=0 到 CFUN=1 的间隔 / 之后等多久复检 |
| `NET_HEAL_SELF_TEST` | 0 | 上电后主动 `AT+CFUN=0` 模拟掉网，验证看门狗能否自己救回来 |
| `NET_HEAL_POWER_CYCLE` | 0 | 实验性：CFUN 连续 3 次无效时用 PWRKEY 给模组真正断电重启 |

- 判定只认「查询成功但状态异常」，**命令超时不计入** → 音频流占着 AT 通道时不会误伤正常业务。
- 手动兜底：串口发 `AT+CFUN=0` → 等 3s → `AT+CFUN=1`；或直接拔插 USB（模组 VBAT 断电，最彻底）。

## 构建

```sh
source /path/to/esp-idf/export.sh
python3 scripts/build.py es8311-ml307-4g-voice --name es8311-ml307-4g-voice
```

### 构建环境的坑（Windows）

`scripts/build.py` 的 assets 生成步骤是用 `cmd.exe` 调**裸 `python`** 的。
如果那个子进程的 PATH 里没有 python（例如在 IDE 终端里构建），会看到：

```
'python' is not recognized as an internal or external command
FAILED: generated_assets.bin ...
```

两种绕过方式：

1. 在 `python` 能正常解析的终端里构建（或把 python 所在目录加进 PATH）；
2. assets 没改动时，只重编 app 即可（需先在同一 shell 里 source `export.ps1`）：

```sh
ninja -C build gen_xiaozhi_binary    # 产出 build/xiaozhi.bin，跳过 assets 步骤
```
