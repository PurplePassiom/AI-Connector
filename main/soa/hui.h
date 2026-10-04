#ifndef HUI_H
#define HUI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void HuiNode_Init(void);

void HuiNode_SendReadRequest(uint16_t sub_service);

/** @brief 写一个 sub_service (HuiWrite, port 302) */
void HuiNode_SendWriteRequest(uint16_t sub_service, const uint8_t* payload, size_t payload_len);

/** @brief 设备私有控制 (HuiIoctl, port 303)。约定 payload[0] 是操作码 */
void HuiNode_SendIoctlRequest(const uint8_t* payload, size_t payload_len);

/**
 * @brief 请求中央 ECU 进入休眠 (发 SlaveHeartbeat, mode=SLEEP_REQUESTED)。
 *
 * 只在已知主机处于 HOST_MODE_WORKING 时才真正发送 —— 其余情况直接返回 false,
 * 调用方据此如实告知用户 (而不是“看起来发了其实没发”)。
 *
 * @return true = 已发出; false = 主机不在工作模式, 或总线发送失败
 */
bool HuiNode_RequestHostSleep(void);

/**
 * @brief 唤醒中央 ECU (发 SlaveHeartbeat, mode=WAKING, wake_reason=USER_REQUEST)。
 *
 * 与休眠不同, 这里**无条件**发送: 主机可能正在睡, 那正是要唤醒它的场景。
 *
 * @return true = 已发出; false = 总线发送失败
 */
bool HuiNode_RequestHostWakeup(void);

#ifdef __cplusplus
}
#endif

#endif /* HUI_H */
