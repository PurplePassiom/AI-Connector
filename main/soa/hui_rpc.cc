/**
 * @file hui_rpc.cc
 * @brief HUI 三个客户端服务的同步调用封装。设计说明见 hui_rpc.h。
 *
 * ⚠️ 实现须知 (踩过的坑, 不要改回去):
 *   1. 应答里没有命令号 ⇒ 必须串行化。事务锁 s_tx_lock 一次只放一个请求过去,
 *      应答靠 s_pending/s_done 认领 (在 s_state_lock 保护下)。
 *   2. 复位顺序不能反: 先 s_done = false, **再**清事件位。反过来的话, 在
 *      "清位 → 复位" 之间落下来的应答会把 s_done 重新置 true 而事件位被清掉,
 *      于是这个应答既不被采纳又不会被等待到 (变成假的超时)。
 *   3. 超时之后**不能**简单地"丢掉下一个应答": Response 里没有命令号, 分不清那一帧
 *      是上一次迟到的应答, 还是本次自己的应答。一旦那帧迟到的应答永远不来, 就会把
 *      本次自己的应答也丢掉 ⇒ 从此每次事务都丢自己的应答, 变成永久性的
 *      "CAN 卡上明明有应答, 固件却次次报超时"。
 *      正确做法: 发下一个请求前先静默 HUI_RPC_DRAIN_MS, 把可能迟到的帧收干净
 *      (这段窗口里 s_pending == false, 应答自然被当成"无主的"忽略掉), 链路自愈。
 */

#include "hui_rpc.h"

#include "hui.h"

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <esp_log.h>

#include <string.h>

#define TAG "HuiRpc"

#ifndef CONFIG_HUI_RPC_TIMEOUT_MS
#define CONFIG_HUI_RPC_TIMEOUT_MS 1500
#endif
#ifndef CONFIG_HUI_RPC_DRAIN_MS
#define CONFIG_HUI_RPC_DRAIN_MS 200
#endif

#define HUI_RPC_BIT_DONE    BIT0
#define HUI_RPC_DEFAULT_MS  ((uint32_t)CONFIG_HUI_RPC_TIMEOUT_MS)

/* 事务锁: 保证同一时刻只有一个在途请求 */
static SemaphoreHandle_t s_tx_lock = nullptr;
/* 在途状态锁: 只保护下面这组变量, 每次只短暂持有 */
static SemaphoreHandle_t s_state_lock = nullptr;
static EventGroupHandle_t s_events = nullptr;

static bool         s_pending = false; /* 有事务在途, 应答可以落到 s_resp */
static bool         s_done = false;    /* s_resp 已就绪 */
static HuiRpcKind   s_pending_kind = HuiRpcKind_Read;
static HuiRpcResult s_resp = {};
static bool         s_timed_out = false; /* 上一次事务超时: 下次发请求前先静默收帧 */
static bool         s_inited = false;

static const char* KindName(HuiRpcKind kind)
{
    switch (kind)
    {
        case HuiRpcKind_Read:  return "read";
        case HuiRpcKind_Write: return "write";
        case HuiRpcKind_Ioctl: return "ioctl";
        default:               return "?";
    }
}

void HuiRpc_Init(void)
{
    if (s_inited)
    {
        return;
    }

    s_tx_lock    = xSemaphoreCreateMutex();
    s_state_lock = xSemaphoreCreateMutex();
    s_events     = xEventGroupCreate();
    if (s_tx_lock == nullptr || s_state_lock == nullptr || s_events == nullptr)
    {
        ESP_LOGE(TAG, "创建同步原语失败, HUI RPC 不可用");
        s_tx_lock    = nullptr;
        s_state_lock = nullptr;
        s_events     = nullptr;
        return;
    }

    s_inited = true;
}

/**
 * @brief 一个完整事务: 占锁 → 发请求 → 等应答 → 放锁。
 * @param send 真正发请求的可调用对象 (由调用方捕获参数)
 */
template <typename SendFn>
static HuiRpcResult Transaction(HuiRpcKind kind, uint32_t timeout_ms, SendFn&& send)
{
    HuiRpcResult result = {};

    if (!s_inited)
    {
        HuiRpc_Init();
        if (!s_inited)
        {
            ESP_LOGE(TAG, "%s: RPC 未初始化, 请求被丢弃", KindName(kind));
            return result;
        }
    }
    if (timeout_ms == 0)
    {
        timeout_ms = HUI_RPC_DEFAULT_MS;
    }

    /* 串行化: 等上一个事务彻底结束。等不到就放弃, 绝不并排发两个请求。 */
    if (xSemaphoreTake(s_tx_lock, pdMS_TO_TICKS(timeout_ms * 2u)) != pdTRUE)
    {
        ESP_LOGW(TAG, "%s: 上一个 HUI 事务还没结束, 本次请求放弃", KindName(kind));
        return result;
    }

    /*
     * 上一次事务超时了 ⇒ 先静默一小段, 把可能迟到的应答收干净, 再发新请求。
     * 此刻 s_pending 还是 false, 所以这段窗口里落下来的应答会被当成"无主的"忽略。
     * (见文件头第 3 条: 不能靠"丢下一个应答"来做, 那会永久毒化整条链路)
     */
    if (s_timed_out)
    {
        s_timed_out = false;
        if (CONFIG_HUI_RPC_DRAIN_MS > 0)
        {
            vTaskDelay(pdMS_TO_TICKS(CONFIG_HUI_RPC_DRAIN_MS));
        }
    }

    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    s_pending      = true;
    s_pending_kind = kind;
    s_done         = false;
    memset(&s_resp, 0, sizeof(s_resp));
    xSemaphoreGive(s_state_lock);

    /* ⚠️ 顺序: 复位 s_done 之后才清事件位 (见文件头第 2 条) */
    xEventGroupClearBits(s_events, HUI_RPC_BIT_DONE);

    result.sent = true;
    send();

    EventBits_t bits = xEventGroupWaitBits(s_events, HUI_RPC_BIT_DONE, pdTRUE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));

    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    if (s_done)
    {
        result.replied = true;
        result.error   = s_resp.error;
        result.len     = s_resp.len;
        result.ok      = (s_resp.error == 0);
        if (s_resp.len > 0)
        {
            memcpy(result.data, s_resp.data, s_resp.len);
        }
    }
    else if (bits & HUI_RPC_BIT_DONE)
    {
        /* 理论上到不了这里: 事件位是在 s_done 落库之后才置上的 */
        ESP_LOGE(TAG, "%s: 应答状态不一致, 本次丢弃", KindName(kind));
    }
    else
    {
        s_timed_out = true; /* 下一次事务开始前先静默收帧, 避免错配 */
        ESP_LOGW(TAG, "%s: 主机 %u ms 内没有应答", KindName(kind), (unsigned)timeout_ms);
    }
    s_pending = false;
    s_done    = false;
    xSemaphoreGive(s_state_lock);

    xSemaphoreGive(s_tx_lock);
    return result;
}

HuiRpcResult HuiRpc_Read(uint16_t sub_service, uint32_t timeout_ms)
{
    return Transaction(HuiRpcKind_Read, timeout_ms,
                       [sub_service]() { HuiNode_SendReadRequest(sub_service); });
}

HuiRpcResult HuiRpc_Write(uint16_t sub_service, const uint8_t* payload, size_t payload_len,
                          uint32_t timeout_ms)
{
    HuiRpcResult rejected = {};
    if (payload_len > HUI_RPC_DATA_MAX)
    {
        ESP_LOGE(TAG, "write: 数据过长 (%u > %u 字节)", (unsigned)payload_len,
                 (unsigned)HUI_RPC_DATA_MAX);
        return rejected;
    }
    return Transaction(HuiRpcKind_Write, timeout_ms, [sub_service, payload, payload_len]() {
        HuiNode_SendWriteRequest(sub_service, payload, payload_len);
    });
}

HuiRpcResult HuiRpc_Ioctl(const uint8_t* payload, size_t payload_len, uint32_t timeout_ms)
{
    HuiRpcResult rejected = {};
    if (payload_len == 0 || payload_len > HUI_RPC_DATA_MAX)
    {
        ESP_LOGE(TAG, "ioctl: payload 长度非法 (%u 字节, 允许 1~%u)", (unsigned)payload_len,
                 (unsigned)HUI_RPC_DATA_MAX);
        return rejected;
    }
    return Transaction(HuiRpcKind_Ioctl, timeout_ms,
                       [payload, payload_len]() { HuiNode_SendIoctlRequest(payload, payload_len); });
}

void HuiRpc_OnResponse(HuiRpcKind kind, uint8_t error, const uint8_t* data, size_t len)
{
    if (!s_inited)
    {
        return;
    }
    if (len > HUI_RPC_DATA_MAX)
    {
        ESP_LOGW(TAG, "%s: 应答数据 %u 字节, 超过 %u, 已截断", KindName(kind), (unsigned)len,
                 (unsigned)HUI_RPC_DATA_MAX);
        len = HUI_RPC_DATA_MAX;
    }

    xSemaphoreTake(s_state_lock, portMAX_DELAY);

    if (!s_pending)
    {
        xSemaphoreGive(s_state_lock);
        /* 超时静默期与事务间隙里落下来的迟到帧走这里 (见文件头第 3 条) */
        ESP_LOGD(TAG, "忽略无主的 %s 应答 (当前没有在途请求)", KindName(kind));
        return;
    }
    if (kind != s_pending_kind)
    {
        xSemaphoreGive(s_state_lock);
        ESP_LOGE(TAG, "应答类型不匹配: 期待 %s, 收到 %s", KindName(s_pending_kind),
                 KindName(kind));
        return; /* 不认它, 交给超时处理 */
    }

    s_resp.error = error;
    s_resp.len   = len;
    if (data != nullptr && len > 0)
    {
        memcpy(s_resp.data, data, len);
    }
    s_done = true;
    ESP_LOGI(TAG, "%s 应答已收下: error=%u, 数据 %u 字节", KindName(kind), (unsigned)error,
             (unsigned)len);
    xSemaphoreGive(s_state_lock);

    /* 置位必须在数据落库之后 */
    xEventGroupSetBits(s_events, HUI_RPC_BIT_DONE);
}

const char* HuiRpc_ErrorText(HuiRpcKind kind, uint8_t error)
{
    if (error == 0)
    {
        return "NO_ERROR";
    }

    if (kind == HuiRpcKind_Read)
    {
        switch (error)
        {
            case 1: return "ERROR_UNKNOWN_SUB_SERVICE";
            case 2: return "ERROR_NOT_READABLE";
            case 3: return "ERROR_BAD_LENGTH";
            case 4: return "ERROR_INTERNAL";
            default: break;
        }
    }
    else if (kind == HuiRpcKind_Write)
    {
        switch (error)
        {
            case 1: return "ERROR_UNKNOWN_SUB_SERVICE";
            case 2: return "ERROR_READ_ONLY";
            case 3: return "ERROR_BAD_LENGTH";
            case 4: return "ERROR_VALUE_REJECTED";
            case 5: return "ERROR_INTERNAL";
            default: break;
        }
    }
    else
    {
        switch (error)
        {
            case 1: return "ERROR_UNSUPPORTED_REQUEST";
            case 2: return "ERROR_BAD_LENGTH";
            case 3: return "ERROR_VALUE_REJECTED";
            case 4: return "ERROR_INTERNAL";
            default: break;
        }
    }
    return "UNKNOWN_ERROR";
}
