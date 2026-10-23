# 测试用 TSP 服务器

`scripts/tsp_server.py` 是一个**只用 Python 标准库**的测试服务器, 用来端到端联调
"人在服务器端选择执行" 这条路径 (TBOX 侧), 对应固件的
[`type: "command"`](mcp-protocol_zh.md#扩展-服务器下发的单条命令-type-command) 扩展。

它一个进程里提供三件事:

| 组件 | 默认端口 | 作用 |
| --- | --- | --- |
| OTA 端点 | 8000 (与网页同端口) | `GET/POST /xiaozhi/ota/` 返回 `mqtt` 配置, 把设备引到本进程的 broker |
| MQTT broker | 1883 | 接收设备连接与上行; 向设备下发命令 |
| 网页控制台 | 8000 | 列出设备 `tools/list` 回来的工具, 点一下就把命令发下去 |

## 1. 设备侧准备

先分清两个角色 —— 这是本方案的关键:

| 通道 | 谁发起 | 由什么配置决定 | 传输 |
| --- | --- | --- | --- |
| **AI 通道** | 云端 LLM 的 `tools/list` / `tools/call` | `CONFIG_OTA_URL` 下发的 `mqtt`/`websocket` 段 | MQTT (固件从不 Subscribe, 靠服务端直投) |
| **TBOX 链路** | 人在服务器端选命令 | `CONFIG_TBOX_LINK_*` / NVS `tbox` | 设备主动连的 **TCP + 行分隔 JSON** |

两条各自一个服务器，互不影响：AI 云挂了照样能控车，TBOX 服务器也拿不到 AI 那条
会话。两条都收敛到 `McpServer` 同一张工具表，所以参数校验 / 手册字节编码 /
工具内的安全门槛只有一份。

> **为什么 TBOX 链路不用第二个 MQTT 客户端？**（2026-10-05 真机实测，别再改回去）
> ML307 的 `MQTTSTATE` URC 不带客户端编号（`+MQTTSTATE: 3`），而每个 `Ml307Mqtt`
> 实例的 URC 回调都会收到它 —— 第二个客户端做一次状态查询就把第一个客户端的
> `connected_` 清成 false，之后它的 `Publish()` 直接 `return false`，连 `AT+MQTTPUB`
> 都不发（现场现象：AI 通道报 `Failed to publish message`，工具回复全部丢）。
> 而 `Ml307Tcp` 的 URC（`MIPOPEN`/`MIPCLOSE`/`MIPURC`/`MIPSTATE`）全部带 id，
> 多路复用是安全的，所以走 TCP。

板级配置在 `main/boards/es8311-ml307-4g-voice/config.json` 的 `sdkconfig_append`
(已填好测试服务器地址):

```json
"CONFIG_OTA_URL=\"http://134.175.25.107:8000/xiaozhi/ota/\"",
"CONFIG_TBOX_LINK_ENABLE=y",
"CONFIG_TBOX_LINK_ENDPOINT=\"134.175.25.107:9000\"",
"CONFIG_TBOX_LINK_CHANNEL=\"tsp/dev/tbox-test\"",
"CONFIG_TBOX_LINK_CLIENT_ID=\"tbox-test\"",
"CONFIG_TBOX_LINK_TOKEN=\"tsp\""
```

这里为了台架方便，两条都指向同一个测试 TSP，于是控制台会看到两条连接。
生产上把 `CONFIG_OTA_URL` 换回真实 AI 云、`TBOX_LINK_ENDPOINT` 换成你们自己的
TBOX 服务器即可。

要点:

- `CONFIG_TBOX_LINK_*` 可以被 NVS 命名空间 `tbox` 覆盖
  (`endpoint` / `channel` / `client_id` / `token` / `ping`)，出厂后换地址不需要
  重新编译。`client_id` 留空 = 设备 UUID。
- `CONFIG_TBOX_LINK_TLS=y` 时走模组的 SSL 通道 (通常配 443/8443)。台架验证的是
  明文 TCP，上真实 TSP 前请先确认证书策略。
- 链路是全双工 NDJSON：设备连上先发一行 `hello`（带 device_id/client_id/channel），
  之后每行一个 JSON；设备每 `CONFIG_TBOX_LINK_PING_SECONDS` 秒发一行 `{"type":"ping"}`。
- 测完要回到真实云端时，把 `CONFIG_OTA_URL` 那行删掉/改回即可。注意 OTA 下的
  `mqtt` 配置是**写进 NVS 持久化**的，重启即被新配置覆盖；要彻底清干净需
  `idf.py erase-flash`。
- 服务器侧要先放通 **TCP 8000、1883 和 9000 的入站** (云主机的安全组 / 防火墙);
  9000 是 TBOX 链路端口，漏了的话设备日志会停在 `连接 ... 失败`，
  并每 30 秒重试一次。
- `scripts/build.py` 每次都会把旧 `sdkconfig` 挪成 `sdkconfig.old` 并按下
  `sdkconfig_append` 重新生成，所以改完直接重新构建即可:

  ```sh
  . <idf>/export.sh
  python3 scripts/build.py es8311-ml307-4g-voice --name es8311-ml307-4g-voice
  idf.py -p <COM> flash
  ```

  想临时改而不动配置文件时, 也可以直接 `idf.py menuconfig`
  → `Xiaozhi Assistant` → `Default OTA URL`。

## 2. 服务器侧运行

把 `scripts/tsp_server.py` 拷到有公网 IP 的服务器 (或者在能连通的机器上跑):

```sh
# 默认已带鉴权: --token tsp --mqtt-user tsp --mqtt-password tsp
python3 scripts/tsp_server.py --host 0.0.0.0 --http-port 8000 --mqtt-port 1883 --tcp-port 9000
```

只依赖 Python 3.8+, 没有第三方包。常用参数:

- `--token` : TBOX 链路 hello 必须带的 token (默认 `tsp`, 对应固件
  `CONFIG_TBOX_LINK_TOKEN` 或 NVS `tbox`/`token`)。空字符串 = 不校验。
- `--mqtt-user` / `--mqtt-password` : MQTT 凭据 (默认 `tsp`/`tsp`), 会随 OTA 响应
  下发给设备。
- `--allow-anonymous` : ❗ 关掉 MQTT 鉴权。**只在监听 127.0.0.1 时用** —— 监听
  0.0.0.0 又不开鉴权, 几小时内就会被互联网扫描器连上 (见 [5. 已知限制](#5-已知限制))。
- `--public-host` : 下发给设备的 broker 主机名。默认用 OTA 请求的 `Host` 头推导,
  所以 NAT/端口映射之外的情况**不用手工填 IP**。
- `--publish-topic` : 强制指定主题; 默认按设备的 `Client-Id` 推导 `tsp/dev/<client_id>`。
- `--quiet` : 只在网页看日志。

用当前测试服务器 (`134.175.25.107`) 时, 先把 8000/1883 放通, 再临时验一下:

```sh
curl http://134.175.25.107:8000/healthz      # 期望 {"ok": true}
```

## 3. 怎么用

设备上电后会自动走: OTA 拿到 mqtt 配置 → 连 broker → TSP 自动发一次
`initialize` + `tools/list`。然后打开浏览器:

```
http://134.175.25.107:8000/
```

- 顶部：当前选中的设备 + 强制直投次数。
- **设备连接**：每条连上来的 MQTT 连接一行（AI 通道 / TBOX 链路），
  显示 `topic`、工具数、回执数，以及 **已订阅 / 未订阅(直投)**。
  圆点选哪条，命令就发到哪条。
- **工具表**取自当前选中设备的 `tools/list`；点 "填入" 会把一条预填好的命令
  （带 `client_id`）写进文本框，再点 "下发命令" 即发送。
- 也可以直接改文本框，发任意消息 (没有 `type` 字段时会被当作 MCP payload 自动包一层)。

命令行等价物:

```sh
# 看有哪几条连接 (devices 数组)
curl http://134.175.25.107:8000/api/state

# 选定一条 (不传 client_id 时用当前选中的)
curl -X POST http://134.175.25.107:8000/api/select \
  -H 'Content-Type: application/json' -d '{"client_id":"tbox-test"}'

# 重新探测工具列表
curl -X POST http://134.175.25.107:8000/api/probe \
  -H 'Content-Type: application/json' -d '{"client_id":"tbox-test"}'

# 下发一条命令 (人的选择) —— 等价于控制台点一下
curl -X POST http://134.175.25.107:8000/api/call \
  -H 'Content-Type: application/json' \
  -d '{"client_id":"tbox-test","name":"self.hui.host1.set_headlight","arguments":{"on":1}}'

# 发任意协议消息 / MCP payload (client_id 会被取出当作目标)
curl -X POST http://134.175.25.107:8000/api/send \
  -H 'Content-Type: application/json' \
  -d '{"client_id":"tbox-test","jsonrpc":"2.0","id":7,"method":"tools/list","params":{"withUserTools":true}}'
```

## 4. 日志怎么看

关键几行:

- `OTA 请求 Host=... Client-Id=... -> mqtt <host>:1883 topic=...`
  —— 确认 AI 那条通道真的连到了你。
- `发现设备连接 'xxx', topic=...` / `CONNECT client_id=... -> CONNACK ok`
  —— 一条 MQTT 连接被认成了设备。
- `设备 'tbox-test' 订阅了 ['tsp/dev/tbox-test'] (会正常收到下行)`
  —— **TBOX 链路那一侧**。如果这条一直不出现，设备日志里应能对应看到
  `TboxLink: 连接 ... 失败`（端口没放通 / 账号密码不对 / endpoint 写错）。
- `强制投递生效: client_id=... 没有订阅 <topic>, 按指定设备直投 (它确实从未 Subscribe)`
  —— **这是有意保留的诊断**: AI 通道那一侧固件只发不收（`mqtt_protocol.cc` 里
  没有任何 `Subscribe()` 调用），所以靠 broker 主动直投；TBOX 链路那一侧则是正常订阅。
  两条的差别能直接在这里看出来 —— 这本身就是有价值的结论。
- `[client_id] 从设备拿到 N 个工具 (tools/list)`
  —— `N = 0` 说明工具没注册上, 对照设备日志里 `tools/list -> N 个工具` 那行排查
  (常见原因: 命令手册没加载到, 见 `main/soa/hui_mcp.h` 的说明)。
- `下发命令 <name> -> 'client_id' args={...} (投递 1 份)` / `PUBLISH -> 设备 ...`
  —— 命令确实发出去了，且只发给选中的那一条。

设备侧对应应该看到:

```
TboxLink: TBOX 链路: 134.175.25.107:9000 client_id=tbox-test channel=tsp/dev/tbox-test
TboxLink: 已连上 TBOX 服务器 134.175.25.107:9000
TboxLink: TBOX 下发命令: {"type":"command","name":"self.hui.host1.set_headlight",...}
MCP: CallTool: self.hui.host1.set_headlight (id=1)
HuiRpc: ... 应答已收到 ...
```

服务器侧对应应该看到:

- `TBOX 链路 (TCP/NDJSON) 监听 0.0.0.0:9000` —— 启动那一行。
- `TBOX 链路接入 <IP>:<port>` + `[tbox-test] hello device_id='...' channel='tsp/dev/tbox-test' version=1`
  —— 点对点链路接上了。
- `[tbox-test] 从设备拿到 N 个工具 (tools/list)` —— `N = 0` 说明工具没注册上,
  对照设备日志里 `tools/list -> N 个工具` 那行排查 (常见原因: 命令手册没加载到,
  见 `main/soa/hui_mcp.h` 的说明)。
- `下发命令 <name> -> 'tbox-test' (tcp) args={...} (投递 1 份)` —— 命令确实发出去了，
  且只发给选中的那一条链路。

## 5. 已知限制

- **鉴权默认已开启** (2026-10-05 加固): 监听公网时 MQTT 必须带
  `--mqtt-user`/`--mqtt-password` 给对 (默认 `tsp`/`tsp`, 由 OTA 响应的 `mqtt` 段下发给
  设备, 固件会用 `AT+MQTTCONN` 带上); TBOX 链路则校验 hello 里的 `token`
  (对应固件 `CONFIG_TBOX_LINK_TOKEN` / NVS `tbox`/`token`)。不符直接断开。
  只在本机调试才用 `--allow-anonymous` / `--token ""`。
  - **为什么必须开**: 公网无鉴权的 1883 一天内就会被互联网扫描器连上来 —— 实测见到
    Nmap 的 `mqtt-subscribe` 脚本 (`nmap` + 16 位随机字母, 订阅 `$SYS/#` 和 `#`)、
    `iw-xxxx` 扫描器, 以及 `go`/`test`/`MqttClient` 之类的通用脚本。
  - **更严重的不是列表难看**: broker 的 "指定设备直投" 会把**任何客户端**发到设备主题
    的消息投给设备, 而固件把 `{"type":"command",name,arguments}` 当服务器消息执行
    —— 等于公网可远程执行车控工具 (`calibrate_motor` / `set_drive_mode` 这类)。
    现在**客户端发起的 PUBLISH 只按订阅投递, 不再直投设备**; 直投只给 TSP 自己发的下行。
- 仍不做的: QoS 0 投递、不做 retain/遗嘱/持久会话, `$SYS` 不产生任何事件;
  没有任何扫描器/限流防护 (扫描器仍然能建立 TCP 连接, 只是被 CONNACK 5 拒绝)。
- broker 只实现到能跑通测试的程度: 保留位与 `+`/`#` 通配符支持, 只为测试。
- 网页控制台的数据来自设备 `tools/list`, 因此同样受
  `tools/list` 单页 8000 字节与 `HUI_MCP_MAX_TOOLS = 24` 的限制 (超出的命令
  在控制台里看不到)。要看全量命令请直接读命令手册
  (`main/soa/manuals/*.json`), 但那样列出、这样执行的前提是工具确实注册过。
- 两条通道的工具表是**各自注册的同一张表**（同一个 `McpServer`），所以不存在
  "AI 能用但人不能用" 的工具；要区分可见性用 `AddUserOnlyTool`（见
  [MCP 协议](mcp-protocol_zh.md#扩展-服务器下发的单条命令-type-command)）。

## 6. 自测

```sh
python3 -m unittest discover -s scripts/tests -p "test_tsp_server.py" -v
```

测试用裸 socket 扮设备, 走真实 MQTT 报文与 HTTP 请求: OTA 响应推导、未订阅也能收到
探测、`tools/list` 回填控制台、`type:"command"` 下发、`/api/send` 自动包装。
