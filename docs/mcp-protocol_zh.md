# MCP (Model Context Protocol) 交互流程

NOTICE: AI 辅助生成, 在实现后台服务时, 请参照代码确认细节!!

本项目中的 MCP 协议用于后台 API（MCP 客户端）与 ESP32 设备（MCP 服务器）之间的通信，以便后台能够发现和调用设备提供的功能（工具）。

## 协议格式

根据代码 (`main/protocols/protocol.cc`, `main/mcp_server.cc`)，MCP 消息是封装在基础通信协议（如 WebSocket 或 MQTT）的消息体中的。其内部结构遵循 [JSON-RPC 2.0](https://www.jsonrpc.org/specification) 规范。

整体消息结构示例：

```json
{
  "session_id": "...", // 会话 ID
  "type": "mcp",       // 消息类型，固定为 "mcp"
  "payload": {         // JSON-RPC 2.0 负载
    "jsonrpc": "2.0",
    "method": "...",   // 方法名 (如 "initialize", "tools/list", "tools/call")
    "params": { ... }, // 方法参数 (对于 request)
    "id": ...,         // 请求 ID (对于 request 和 response)
    "result": { ... }, // 方法执行结果 (对于 success response)
    "error": { ... }   // 错误信息 (对于 error response)
  }
}
```

其中，`payload` 部分是标准的 JSON-RPC 2.0 消息：

- `jsonrpc`: 固定的字符串 "2.0"。
- `method`: 要调用的方法名称 (对于 Request)。
- `params`: 方法的参数，一个结构化值，通常为对象 (对于 Request)。
- `id`: 请求的标识符，客户端发送请求时提供，服务器响应时原样返回。用于匹配请求和响应。
- `result`: 方法成功执行时的结果 (对于 Success Response)。
- `error`: 方法执行失败时的错误信息 (对于 Error Response)。

## 交互流程及发送时机

MCP 的交互主要围绕客户端（后台 API）发现和调用设备上的“工具”（Tool）进行。

1.  **连接建立与能力通告**

    - **时机：** 设备启动并成功连接到后台 API 后。
    - **发送方：** 设备。
    - **消息：** 设备发送基础协议的 "hello" 消息给后台 API，消息中包含设备支持的能力列表，例如通过支持 MCP 协议 (`"mcp": true`)。
    - **示例 (非 MCP 负载，而是基础协议消息):**
      ```json
      {
        "type": "hello",
        "version": ...,
        "features": {
          "mcp": true,
          ...
        },
        "transport": "websocket", // 或 "mqtt"
        "audio_params": { ... },
        "session_id": "..." // 设备收到服务器hello后可能设置
      }
      ```

2.  **初始化 MCP 会话**

    - **时机：** 后台 API 收到设备 "hello" 消息，确认设备支持 MCP 后，通常作为 MCP 会话的第一个请求发送。
    - **发送方：** 后台 API (客户端)。
    - **方法：** `initialize`
    - **消息 (MCP payload):**

      ```json
      {
        "jsonrpc": "2.0",
        "method": "initialize",
        "params": {
          "capabilities": {
            // 客户端能力，可选

            // 摄像头视觉相关
            "vision": {
              "url": "...", //摄像头: 图片处理地址(必须是http地址, 不是websocket地址)
              "token": "..." // url token
            }

            // ... 其他客户端能力
          }
        },
        "id": 1 // 请求 ID
      }
      ```

    - **设备响应时机：** 设备收到 `initialize` 请求并处理后。
    - **设备响应消息 (MCP payload):**
      ```json
      {
        "jsonrpc": "2.0",
        "id": 1, // 匹配请求 ID
        "result": {
          "protocolVersion": "2024-11-05",
          "capabilities": {
            "tools": {} // 这里的 tools 似乎不列出详细信息，需要 tools/list
          },
          "serverInfo": {
            "name": "...", // 设备名称 (BOARD_NAME)
            "version": "..." // 设备固件版本
          }
        }
      }
      ```

3.  **发现设备工具列表**

    - **时机：** 后台 API 需要获取设备当前支持的具体功能（工具）列表及其调用方式时。
    - **发送方：** 后台 API (客户端)。
    - **方法：** `tools/list`
    - **消息 (MCP payload):**
      ```json
      {
        "jsonrpc": "2.0",
        "method": "tools/list",
        "params": {
          "cursor": "" // 用于分页，首次请求为空字符串
        },
        "id": 2 // 请求 ID
      }
      ```
    - **设备响应时机：** 设备收到 `tools/list` 请求并生成工具列表后。
    - **设备响应消息 (MCP payload):**
      ```json
      {
        "jsonrpc": "2.0",
        "id": 2, // 匹配请求 ID
        "result": {
          "tools": [ // 工具对象列表
            {
              "name": "self.get_device_status",
              "description": "...",
              "inputSchema": { ... } // 参数 schema
            },
            {
              "name": "self.audio_speaker.set_volume",
              "description": "...",
              "inputSchema": { ... } // 参数 schema
            }
            // ... 更多工具
          ],
          "nextCursor": "..." // 如果列表很大需要分页，这里会包含下一个请求的 cursor 值
        }
      }
      ```
    - **分页处理：** 如果 `nextCursor` 字段非空，客户端需要再次发送 `tools/list` 请求，并在 `params` 中带上这个 `cursor` 值以获取下一页工具。

4.  **调用设备工具**

    - **时机：** 后台 API 需要执行设备上的某个具体功能时。
    - **发送方：** 后台 API (客户端)。
    - **方法：** `tools/call`
    - **消息 (MCP payload):**
      ```json
      {
        "jsonrpc": "2.0",
        "method": "tools/call",
        "params": {
          "name": "self.audio_speaker.set_volume", // 要调用的工具名称
          "arguments": {
            // 工具参数，对象格式
            "volume": 50 // 参数名及其值
          }
        },
        "id": 3 // 请求 ID
      }
      ```
    - **设备响应时机：** 设备收到 `tools/call` 请求，执行相应的工具函数后。
    - **设备成功响应消息 (MCP payload):**
      ```json
      {
        "jsonrpc": "2.0",
        "id": 3, // 匹配请求 ID
        "result": {
          "content": [
            // 工具执行结果内容
            { "type": "text", "text": "true" } // 示例：set_volume 返回 bool
          ],
          "isError": false // 表示成功
        }
      }
      ```
    - **设备失败响应消息 (MCP payload):**
      ```json
      {
        "jsonrpc": "2.0",
        "id": 3, // 匹配请求 ID
        "error": {
          "code": -32601, // JSON-RPC 错误码，例如 Method not found (-32601)
          "message": "Unknown tool: self.non_existent_tool" // 错误描述
        }
      }
      ```

5.  **设备主动发送消息 (Notifications)**
    - **时机：** 设备内部发生需要通知后台 API 的事件时（例如，状态变化，虽然代码示例中没有明确的工具发送此类消息，但 `Application::SendMcpMessage` 的存在暗示了设备可能主动发送 MCP 消息）。
    - **发送方：** 设备 (服务器)。
    - **方法：** 可能是以 `notifications/` 开头的方法名，或者其他自定义方法。
    - **消息 (MCP payload):** 遵循 JSON-RPC Notification 格式，没有 `id` 字段。
      ```json
      {
        "jsonrpc": "2.0",
        "method": "notifications/state_changed", // 示例方法名
        "params": {
          "newState": "idle",
          "oldState": "connecting"
        }
        // 没有 id 字段
      }
      ```
    - **后台 API 处理：** 接收到 Notification 后，后台 API 进行相应的处理，但不回复。

## 交互图

下面是一个简化的交互序列图，展示了主要的 MCP 消息流程：

```mermaid
sequenceDiagram
    participant Device as ESP32 Device
    participant BackendAPI as 后台 API (Client)

    Note over Device, BackendAPI: 建立 WebSocket / MQTT 连接

    Device->>BackendAPI: Hello Message (包含 "mcp": true)

    BackendAPI->>Device: MCP Initialize Request
    Note over BackendAPI: method: initialize
    Note over BackendAPI: params: { capabilities: ... }

    Device->>BackendAPI: MCP Initialize Response
    Note over Device: result: { protocolVersion: ..., serverInfo: ... }

    BackendAPI->>Device: MCP Get Tools List Request
    Note over BackendAPI: method: tools/list
    Note over BackendAPI: params: { cursor: "" }

    Device->>BackendAPI: MCP Get Tools List Response
    Note over Device: result: { tools: [...], nextCursor: ... }

    loop Optional Pagination
        BackendAPI->>Device: MCP Get Tools List Request
        Note over BackendAPI: method: tools/list
        Note over BackendAPI: params: { cursor: "..." }
        Device->>BackendAPI: MCP Get Tools List Response
        Note over Device: result: { tools: [...], nextCursor: "" }
    end

    BackendAPI->>Device: MCP Call Tool Request
    Note over BackendAPI: method: tools/call
    Note over BackendAPI: params: { name: "...", arguments: { ... } }

    alt Tool Call Successful
        Device->>BackendAPI: MCP Tool Call Success Response
        Note over Device: result: { content: [...], isError: false }
    else Tool Call Failed
        Device->>BackendAPI: MCP Tool Call Error Response
        Note over Device: error: { code: ..., message: ... }
    end

    opt Device Notification
        Device->>BackendAPI: MCP Notification
        Note over Device: method: notifications/...
        Note over Device: params: { ... }
    end
```

这份文档概述了该项目中 MCP 协议的主要交互流程。具体的参数细节和工具功能需要参考 `main/mcp_server.cc` 中 `McpServer::AddCommonTools` 以及各个工具的实现。

## 扩展: 服务器下发的单条命令 (`type: "command"`)

> 只有 TBOX + AIBOX 二合一产品需要, 见 `main/Kconfig.projbuild`。
> 联调用测试服务器见 [测试用 TSP 服务器](tsp-test-server_zh.md)。

除了 AI 自主调用, 还有一条 "人在服务器端选好命令再下发" 的路径 (TBOX 侧)。
它复用同一张工具表, 只是把 "选哪条命令" 从模型换成了人。消息不是 JSON-RPC,
而是一条基础协议消息:

有**两条**可选的到达方式, 由两个独立开关控制:

| 开关 | 通道 | 用途 |
| --- | --- | --- |
| `CONFIG_TBOX_COMMAND_MESSAGE` | 复用 **AI 那条连接** | 台架/应急: 只多一个消息类型, 不改连接拓扑; 但依赖 AI 云在线 |
| `CONFIG_TBOX_LINK_ENABLE` | 设备**另开一条独立连接**连到 TBOX 服务器 (TCP/可选 TLS, 行分隔 JSON) | 生产: 与 AI 云完全解耦 (AI 云不可用不影响控车) |

两条都收敛到 `McpServer::CallTool()` → `ParseMessage()`, 也就是同一张工具表。

```json
{
  "type": "command",
  "name": "self.hui.host1.set_headlight",
  "arguments": { "on": 1 }
}
```

固件收到后只做一件事: 把它翻译成标准 MCP 请求并交给 `McpServer` —— 走 AI 通道的
分支在 `main/application.cc`, 独立链路的在 `main/tbox_link.cc`, 两边都调
`McpServer::CallTool()`:

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "tools/call",
  "params": { "name": "self.hui.host1.set_headlight", "arguments": { "on": 1 } }
}
```

因此它与 AI 的 `tools/call` 走**完全相同**的一条路径: 同一张工具表、同一份
`Property` 范围/长度校验 (`-32602`)、同一份手册字节编码、以及工具内部的安全门槛
(例如 `HuiNode_RequestHostSleep()` 只在中央 ECU 处于 WORKING 时才真正发出)。
不存在 "绕过校验直接写总线" 的第二条路径。

回执按普通 MCP 应答发出 (`type: "mcp"` + `payload.result` / `payload.error`),
格式与 AI 调用时一致。

- `name` 必填且必须是字符串; 工具名由 `tools/list` 给出 (或按命令手册推导, 见
  [HUI 命令手册](hui-command-manual_zh.md))。
- `arguments` 可选; 缺省或类型不对都由 `McpServer::ParseMessage` 按 MCP 规则处理
  (`-32602`)。`id` 由固件生成, 服务器按 `name`/时间自行对应即可。
- 只想给人用、不想被 AI 误触发的命令, 用 `AddUserOnlyTool()` 注册: 控制台侧
  `tools/list` 带 `"withUserTools": true` 能看到, AI 侧不带就看不到。
