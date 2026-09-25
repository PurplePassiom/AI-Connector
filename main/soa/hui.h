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

#ifdef __cplusplus
}
#endif

#endif /* HUI_H */
