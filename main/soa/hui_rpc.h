#ifndef HUI_RPC_H
#define HUI_RPC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * HUI 客户端服务 (HuiRead 301 / HuiWrite 302 / HuiIoctl 303) 的同步调用封装。
 *
 * 设计约束 (来自 DSDL, 别绕过去):
 *   1. 三个 Response 里**没有回显命令号** —— 收到一帧应答时无法判断它属于哪个
 *      请求。所以同一时刻只允许有一个在途事务, 本模块用互斥量串行化, 用事件组
 *      等待应答 (超时后可丢弃一帧迟到的应答, 避免错配给下一个事务)。
 *   2. 调用方是 MCP 工具回调 (主任务), 应答方是 SOA 的 CAN 任务;
 *      两边只在状态锁保护下交换数据。
 *   3. 本模块会阻塞。**绝不允许**从传输层回调 (CAN 任务) 里再调本模块的
 *      HuiRpc_Read/Write/Ioctl, 那会和等待应答的自己死锁。
 * ========================================================================= */

#define HUI_RPC_DATA_MAX 8u /* DSDL: uint8[<=8] data */

typedef enum {
    HuiRpcKind_Read  = 0,
    HuiRpcKind_Write = 1,
    HuiRpcKind_Ioctl = 2,
} HuiRpcKind;

typedef struct {
    bool    sent;    /* 请求是否真的发出去了 (false = 参数非法/未初始化, 没发) */
    bool    replied; /* 是否收到应答帧 (false = 超时) */
    bool    ok;      /* replied 且 error == 0 */
    uint8_t error;   /* 服务侧错误码, 0 = NO_ERROR */
    size_t  len;     /* data 有效字节数 */
    uint8_t data[HUI_RPC_DATA_MAX];
} HuiRpcResult;

/** @brief 幂等。HuiNode_Init() 会调用一次, 单独调用也安全。 */
void HuiRpc_Init(void);

/**
 * @brief 发 read 请求并等应答。
 * @param sub_service 命令号 (read 的 sub_service 字段)
 * @param timeout_ms  0 = 用默认值
 */
HuiRpcResult HuiRpc_Read(uint16_t sub_service, uint32_t timeout_ms);

/** @brief 发 write 请求并等应答。payload_len > 8 时直接返回 sent=false。 */
HuiRpcResult HuiRpc_Write(uint16_t sub_service, const uint8_t* payload, size_t payload_len,
                          uint32_t timeout_ms);

/**
 * @brief 发 ioctl 请求并等应答。
 * @param payload 完整 payload (约定 payload[0..1] 是命令码, 后面是参数), 最多 8 字节
 */
HuiRpcResult HuiRpc_Ioctl(const uint8_t* payload, size_t payload_len, uint32_t timeout_ms);

/** @brief 由 hui.cc 的三个应答回调调用。跑在传输层任务里 —— 只落数据 + 置事件位。 */
void HuiRpc_OnResponse(HuiRpcKind kind, uint8_t error, const uint8_t* data, size_t len);

/** @brief 服务侧错误码 → 人话 (read/write/ioctl 三套编号各不相同) */
const char* HuiRpc_ErrorText(HuiRpcKind kind, uint8_t error);

#ifdef __cplusplus
}
#endif

#endif /* HUI_RPC_H */
