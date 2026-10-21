#ifndef TBOX_LINK_H
#define TBOX_LINK_H

/* ============================================================================
 * TBOX 专用链路 (方案 B) —— 设备另开一条**独立**连接连到 TBOX 服务器。
 *
 * 为什么要独立: AI 云 (OTA 下发的 mqtt/websocket 配置) 是第三方 (如
 * api.tenclass.net), 我们既改不了它、也没法让 TBOX 服务器借用那条会话。所以
 * "人在服务器端选命令执行" 这条路径自己开一条连接:
 *
 *   AI 通道   : 云端 LLM 的 tools/list / tools/call      <- OTA 下发的配置
 *   TBOX 链路 : 人的选择 -> type:"command" / 标准 JSON-RPC <- 本模块 (NVS/Kconfig)
 *
 * 两边互不影响: AI 云断了照样能控车, TBOX 服务器也拿不到 AI 那条会话。
 *
 * 传输: **独立 TCP (可选 TLS) + 行分隔 JSON (NDJSON)**，一个 JSON 一行。
 * 故意不用第二个 MQTT 客户端 —— ML307 的 `MQTTSTATE` URC 不带客户端编号, 两个
 * Ml307Mqtt 实例会互相把 `connected_` 清成 false, 导致其中一个彻底发不出去
 * (2026-10-05 真机实测)。`Ml307Tcp` 的 URC 带 id, 多路复用安全。
 *
 * 契约与 AI 通道一致 (两条都收敛到 McpServer 同一张工具表, 因此参数校验、
 * 手册字节编码、工具内的安全门槛只有一份):
 *   设备->服务器: {"type":"hello","device_id":..,"client_id":..,"channel":..,"version":1}
 *                {"type":"ping"}
 *                {"type":"mcp","payload":{ ...JSON-RPC 应答... }}
 *   服务器->设备: {"type":"command","name":"self.hui.host1.set_headlight","arguments":{"on":1}}
 *                {"type":"mcp","payload":{ ...标准 JSON-RPC... }}
 *                裸 JSON-RPC (带 method 字段, 未套 type)
 *
 * 配置: NVS 命名空间 "tbox" (endpoint/channel/client_id/token/ping) 优先,
 *       缺省回落到 CONFIG_TBOX_LINK_* —— 出厂后可用 NVS 改地址, 不用重新编译。
 *
 * 关闭 CONFIG_TBOX_LINK_ENABLE 时本函数是空实现。
 * ========================================================================= */

/** 启动 TBOX 链路 (幂等)。需要网络已就绪。首次连接会阻塞到出结果, 调用方自行决定在哪个任务里调。 */
void TboxLink_Start(void);

#endif  /* TBOX_LINK_H */
