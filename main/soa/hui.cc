/**
 * @file hui.cc
 * @brief HUI 节点实现。设计说明与职责见 hui.h。
 *
 * ⚠️ 实现须知 (踩过的坑, 不要改回去):
 *   1. 总线上与本节点有关的只有三件事: 发 SlaveHeartbeat、收 HostHeartbeat、
 *      发 HuiRead/Write/Ioctl 请求并处理它们的应答。
 *   2. 所有 SOA 回调都跑在**传输层的任务**里 (走 CAN 时是 soa_oncan 的 CAN 任务,
 *      走 ESP-NOW 时是 soa_espnow 的分发任务), 不是主任务。
 *      本文件只碰总线上自己的状态, 不碰 Application/音频。
 *   3. 协同休眠 (SleepMgr) 已从本工程移除, 所以 SlaveHeartbeat 的字段目前由
 *      本文件自己维护 (见下面的 s_hb_*)。等休眠协调回来时改成从 SleepMgr 取即可。
 *   4. **SlaveHeartbeat 必须 serialize_ 后再发** (统一走 HuiNode_SendHeartbeat):
 *      线上是 7 字节打包格式, 而 sizeof(struct) 是 8 —— 4 个 1 字节枚举 + session_id
 *      后面有 1 字节对齐填充, count 在结构体里位于偏移 6、在线上位于偏移 5。
 *      直接发结构体会把填充字节当成 count 发出去。其他 DSDL 消息同理。
 */

#include "hui.h"

#include "hui_rpc.h"
#include "soa.h"
#include "soa_onespnow.h"
#include "soa_types.h"

#include "Health_1_0.h"
#include "HostHeartbeat_1_0.h"
#include "HuiIoctl_1_0.h"
#include "HuiRead_1_0.h"
#include "HuiWrite_1_0.h"
#include "PowerMode_1_0.h"
#include "SlaveHeartbeat_1_0.h"
#include "SleepReason_1_0.h"
#include "WakeReason_1_0.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_log.h>

#include <string.h>

#define TAG "HuiNode"

#define HUI_HEARTBEAT_MS        1000U   /* SlaveHeartbeat 周期 (整车约定 1 Hz) */
#define HUI_STACK_SIZE          3072U   /* 要跑序列化 + ESP_LOG */
#define HUI_TASK_PRIO           4

/* SlaveHeartbeat 的字段来源。
 * 协同休眠已移除, 这些字段先由本文件自己维护:
 *   mode         固定 WORKING (本节点上电即工作)
 *   health       可由应用层覆盖 (HuiNode_SetHealth)
 *   sleep/wake   暂固定 NONE (休眠握手回来后再接)
 *   session_id   固定 1, count 每帧自增 */
static uint8_t  s_hb_health     = dsdl_Common_Health_1_0_NOMINAL;
static uint8_t  s_hb_session_id = 1u;
static uint16_t s_hb_count      = 0u;

/* 主机心跳变化检测 (1 Hz 不能每帧打日志) */
static uint8_t  s_host_mode_last    = 0xFFu;
static uint8_t  s_host_session_last = 0xFFu;

static TaskHandle_t s_task = nullptr;
static bool s_initialized = false;

/* HuiRead/HuiWrite 的共享状态码 (两个服务各自编号, 但 0 都是 NO_ERROR) */
#define HUI_READ_ERR_UNKNOWN_SUB_SERVICE     1u
#define HUI_READ_ERR_NOT_READABLE            2u
#define HUI_READ_ERR_BAD_LENGTH              3u
#define HUI_READ_ERR_INTERNAL                4u
#define HUI_WRITE_ERR_UNKNOWN_SUB_SERVICE    1u
#define HUI_WRITE_ERR_READ_ONLY              2u
#define HUI_WRITE_ERR_BAD_LENGTH             3u
#define HUI_WRITE_ERR_VALUE_REJECTED         4u
#define HUI_WRITE_ERR_INTERNAL               5u
#define HUI_IOCTL_ERR_UNSUPPORTED_REQUEST    1u
#define HUI_IOCTL_ERR_BAD_LENGTH             2u
#define HUI_IOCTL_ERR_VALUE_REJECTED         3u
#define HUI_IOCTL_ERR_INTERNAL               4u

/* ===================== 发送: SlaveHeartbeat ===================== */

/**
 * @brief 把一个 SlaveHeartbeat 序列化后发到中央 ECU。
 *
 * ⚠️ 必须走 dsdl_..._serialize_(), 不能把结构体指针 + sizeof() 直接丢给
 *    soa_request_send(): 线上是 **7 字节打包**格式, 而
 *    sizeof(dsdl_Common_SlaveHeartbeat_1_0) == 8, 且 count 的偏移也不同
 *    (结构体 6 / 线上 5) —— 直接发结构体会把对齐填充当成 count 发出去。
 */
static bool HuiNode_SendHeartbeat(const dsdl_Common_SlaveHeartbeat_1_0* hb)
{
    uint8_t buf[dsdl_Common_SlaveHeartbeat_1_0_SERIALIZATION_BUFFER_SIZE_BYTES_];
    size_t  size = sizeof(buf);
    if (dsdl_Common_SlaveHeartbeat_1_0_serialize_(hb, buf, &size) < 0)
    {
        ESP_LOGW(TAG, "SlaveHeartbeat 序列化失败");
        return false;
    }
    return soa_request_send(HUI_NODE_ID, MASTER_NODE_ID,
                            (uint16_t)dsdl_Common_SlaveHeartbeat_1_0_FIXED_PORT_ID_,
                            buf, size) == SOA_OK;
}

/** @brief 组一个本节点 SlaveHeartbeat 的骨架 (health / session / count 取共享状态)。 */
static void HuiNode_FillHeartbeat(dsdl_Common_SlaveHeartbeat_1_0* hb)
{
    memset(hb, 0, sizeof(*hb));
    hb->health.value = s_hb_health;
    hb->session_id   = s_hb_session_id;
    hb->count        = ++s_hb_count;
}
#if 0
static void HuiNode_PublishHeartbeat(void)
{
    dsdl_Common_SlaveHeartbeat_1_0 hb;
    HuiNode_FillHeartbeat(&hb);
    hb.mode.value         = dsdl_Common_PowerMode_1_0_WORKING;
    hb.sleep_reason.value = dsdl_Common_SleepReason_1_0_NONE;
    hb.wake_reason.value  = dsdl_Common_WakeReason_1_0_NONE;

    (void)HuiNode_SendHeartbeat(&hb);
}
#endif

bool HuiNode_RequestHostSleep(void)
{
    /* 主机不在 WORKING 时请求休眠没有意义: 要么它已经在睡, 要么状态还不知道。
       这种时候发出去是无效请求, 所以直接返回 false 让调用方如实告知用户, */
    if (s_host_mode_last != dsdl_Host_HostHeartbeat_1_0_HOST_MODE_WORKING)
    {
        ESP_LOGW(TAG, "忽略休眠请求: 主机不在 WORKING 模式 (当前 mode=%u)",
                 (unsigned)s_host_mode_last);
        return false;
    }

    dsdl_Common_SlaveHeartbeat_1_0 hb;
    HuiNode_FillHeartbeat(&hb);
    hb.mode.value         = dsdl_Common_PowerMode_1_0_SLEEP_REQUESTED;
    hb.sleep_reason.value = dsdl_Common_SleepReason_1_0_USER_REQUEST;
    hb.wake_reason.value  = dsdl_Common_WakeReason_1_0_NONE;

    const bool sent = HuiNode_SendHeartbeat(&hb);
    ESP_LOGI(TAG, "HUI 请求主机休眠: %s", sent ? "已发出" : "发送失败");
    return sent;
}

bool HuiNode_RequestHostWakeup(void)
{
    /* 唤醒**无条件**发: 主机可能正在睡 (mode != WORKING), 而那正是要唤醒它的场景 */
    dsdl_Common_SlaveHeartbeat_1_0 hb;
    HuiNode_FillHeartbeat(&hb);
    hb.mode.value         = dsdl_Common_PowerMode_1_0_WAKING;
    hb.sleep_reason.value = dsdl_Common_SleepReason_1_0_NONE;
    hb.wake_reason.value  = dsdl_Common_WakeReason_1_0_USER_REQUEST;

    const bool sent = HuiNode_SendHeartbeat(&hb);
    ESP_LOGI(TAG, "HUI 请求主机唤醒: %s", sent ? "已发出" : "发送失败");
    return sent;
}

/* ===================== 接收: HostHeartbeat ===================== */

static void HuiNode_OnHostHeartbeat(const uint8_t* data, size_t data_len)
{
    if (data == nullptr || data_len == 0)
    {
        return;
    }
    dsdl_Host_HostHeartbeat_1_0 hb;
    memset(&hb, 0, sizeof(hb));
    size_t size = data_len;
    if (dsdl_Host_HostHeartbeat_1_0_deserialize_(&hb, data, &size) < 0)
    {
        ESP_LOGW(TAG, "HostHeartbeat 反序列化失败 (%u 字节)", (unsigned)data_len);
        return;
    }

    /* 只在 mode / session 变化时打日志 —— 主机心跳是 1 Hz, 每帧打会把日志淡化 */
    if (hb.mode != s_host_mode_last || hb.session_id != s_host_session_last)
    {
        s_host_mode_last    = hb.mode;
        s_host_session_last = hb.session_id;
        ESP_LOGI(TAG, "Central 心跳: mode=%u health=%u session=%u counter=%u",
                 (unsigned)hb.mode, (unsigned)hb.health,
                 (unsigned)hb.session_id, (unsigned)hb.heartbeat_counter);
    }
}

/* ===================== 服务: HuiRead / HuiWrite / HuiIoctl ===================== */

void HuiNode_SendReadRequest(uint16_t sub_service)
{
    dsdl_CenterAndHui_HuiRead_Request_1_0 rqst;
    memset(&rqst, 0, sizeof(rqst));
    rqst.sub_service = sub_service;

    uint8_t buf[dsdl_CenterAndHui_HuiRead_Request_1_0_SERIALIZATION_BUFFER_SIZE_BYTES_];
    size_t  size = sizeof(buf);
    if (dsdl_CenterAndHui_HuiRead_Request_1_0_serialize_(&rqst, buf, &size) < 0)
    {
        return;
    }
    soa_request_send(HUI_NODE_ID, MASTER_NODE_ID,
                     (uint16_t)dsdl_CenterAndHui_HuiRead_1_0_FIXED_PORT_ID_, buf, size);
}

/**
 * 三个应答回调都跑在传输层任务里 (CAN 任务)。这里只做反序列化 + 把结果交给
 * HuiRpc (它在状态锁保护下落数据并唤醒等待中的事务), 不做任何重活。
 */
static void HuiNode_OnHuiReadResponse(const uint8_t* data, size_t data_len)
{
    dsdl_CenterAndHui_HuiRead_Response_1_0 resp;
    memset(&resp, 0, sizeof(resp));
    size_t size = data_len;
    if (dsdl_CenterAndHui_HuiRead_Response_1_0_deserialize_(&resp, data, &size) < 0)
    {
        ESP_LOGW(TAG, "HuiRead.Response 反序列化失败 (%u 字节)", (unsigned)data_len);
        return;
    }
    HuiRpc_OnResponse(HuiRpcKind_Read, resp._error, resp.data.elements, resp.data.count);
}

void HuiNode_SendWriteRequest(uint16_t sub_service, const uint8_t* payload, size_t payload_len)
{
    dsdl_CenterAndHui_HuiWrite_Request_1_0 rqst;
    memset(&rqst, 0, sizeof(rqst));
    rqst.sub_service = sub_service;

    uint8_t buf[dsdl_CenterAndHui_HuiWrite_Request_1_0_SERIALIZATION_BUFFER_SIZE_BYTES_];
    if (payload != nullptr && payload_len > 0)
    {
        if (payload_len > sizeof(rqst.data.elements))
        {
            payload_len = sizeof(rqst.data.elements);
        }
        memcpy(rqst.data.elements, payload, payload_len);
        rqst.data.count = payload_len;
    }
    size_t  size = sizeof(buf);
    if (dsdl_CenterAndHui_HuiWrite_Request_1_0_serialize_(&rqst, buf, &size) < 0)
    {
        return;
    }
    soa_request_send(HUI_NODE_ID, MASTER_NODE_ID,
                      (uint16_t)dsdl_CenterAndHui_HuiWrite_1_0_FIXED_PORT_ID_, buf, size);
}

static void HuiNode_OnHuiWriteResponse(const uint8_t* data, size_t data_len)
{
    dsdl_CenterAndHui_HuiWrite_Response_1_0 resp;
    memset(&resp, 0, sizeof(resp));
    size_t size = data_len;
    if (dsdl_CenterAndHui_HuiWrite_Response_1_0_deserialize_(&resp, data, &size) < 0)
    {
        ESP_LOGW(TAG, "HuiWrite.Response 反序列化失败 (%u 字节)", (unsigned)data_len);
        return;
    }
    /* write 的应答只有错误码, 没有 data */
    HuiRpc_OnResponse(HuiRpcKind_Write, resp._error, nullptr, 0);
}

void HuiNode_SendIoctlRequest(const uint8_t* payload, size_t payload_len)
{
    dsdl_CenterAndHui_HuiIoctl_Request_1_0 rqst;
    memset(&rqst, 0, sizeof(rqst));
    if (payload != nullptr && payload_len > 0)
    {
        if (payload_len > sizeof(rqst.data.elements))
        {
            payload_len = sizeof(rqst.data.elements);
        }
        memcpy(rqst.data.elements, payload, payload_len);
        rqst.data.count = payload_len;
    }

    uint8_t buf[dsdl_CenterAndHui_HuiIoctl_Request_1_0_SERIALIZATION_BUFFER_SIZE_BYTES_];
    size_t  size = sizeof(buf);
    if (dsdl_CenterAndHui_HuiIoctl_Request_1_0_serialize_(&rqst, buf, &size) < 0)
    {
        return;
    }
    soa_request_send(HUI_NODE_ID, MASTER_NODE_ID,
                      (uint16_t)dsdl_CenterAndHui_HuiIoctl_1_0_FIXED_PORT_ID_, buf, size);
}

static void HuiNode_OnHuiIoctlResponse(const uint8_t* data, size_t data_len)
{
    dsdl_CenterAndHui_HuiIoctl_Response_1_0 resp;
    memset(&resp, 0, sizeof(resp));
    size_t size = data_len;
    if (dsdl_CenterAndHui_HuiIoctl_Response_1_0_deserialize_(&resp, data, &size) < 0)
    {
        ESP_LOGW(TAG, "HuiIoctl.Response 反序列化失败 (%u 字节)", (unsigned)data_len);
        return;
    }
    HuiRpc_OnResponse(HuiRpcKind_Ioctl, resp._error, resp.data.elements, resp.data.count);
}

/* ===================== 周期任务: 1 Hz SlaveHeartbeat ===================== */

static void HuiNode_Task(void* arg)
{
    (void)arg;

    for (;;)
    {
        // HuiNode_PublishHeartbeat();
        vTaskDelay(pdMS_TO_TICKS(HUI_HEARTBEAT_MS));
    }
}

void HuiNode_Init(void)
{
    if (s_initialized)
    {
        ESP_LOGW(TAG, "HuiNode_Init() 重复调用, 忽略");
        return;
    }

    /* 0. 同步 RPC 层 (read/write/ioctl 请求-应答配对) 必须在收到任何应答之前就绪 */
    HuiRpc_Init();

    /* 1. 在 SOA 总线上建立本节点 (本板走 ESP-NOW; IPC / UART 不参与) */
    soa_create_node(HUI_NODE_ID, SOA_NODE_ESPNOW);

    /* 1b. 登记路由。⚠️ 这一步不能省:
           soa_request_send() / soa_subscribe() 都要先在路由表里查到路径才肯干活,
           查不到就直接返回 SOA_NOT_OK (只有 subscribe_broadcast 不走路由表)。
           写法与 MIO 各节点 Init 一致 —— MIO 的 AI agent 走 UART, 本板改走 ESP-NOW。 */
    soa_create_route_path(HUI_NODE_ID, MASTER_NODE_ID, SOA_PATH_ESPNOW);

    /* 2. 订阅中央 ECU 的整车心跳 */
    if (soa_subscribe_broadcast(HUI_NODE_ID, SoaBroadcast,
                               (uint16_t)dsdl_Host_HostHeartbeat_1_0_FIXED_PORT_ID_,
                               (uint16_t)dsdl_Host_HostHeartbeat_1_0_EXTENT_BYTES_,
                               HuiNode_OnHostHeartbeat) != SOA_OK)
    {
        ESP_LOGE(TAG, "订阅 HostHeartbeat 失败");
    }

    /* 3. 订阅三个 HUI 服务的**应答** (本节点是 client, Central 是 server) */
    if (soa_subscribe(HUI_NODE_ID, MASTER_NODE_ID, SoaResponse,
                      (uint16_t)dsdl_CenterAndHui_HuiRead_1_0_FIXED_PORT_ID_,
                      (uint16_t)dsdl_CenterAndHui_HuiRead_Response_1_0_EXTENT_BYTES_,
                      HuiNode_OnHuiReadResponse) != SOA_OK)
    {
        ESP_LOGE(TAG, "订阅 HuiRead 失败");
    }
    if (soa_subscribe(HUI_NODE_ID, MASTER_NODE_ID, SoaResponse,
                      (uint16_t)dsdl_CenterAndHui_HuiWrite_1_0_FIXED_PORT_ID_,
                      (uint16_t)dsdl_CenterAndHui_HuiWrite_Response_1_0_EXTENT_BYTES_,
                      HuiNode_OnHuiWriteResponse) != SOA_OK)
    {
        ESP_LOGE(TAG, "订阅 HuiWrite 失败");
    }
    if (soa_subscribe(HUI_NODE_ID, MASTER_NODE_ID, SoaResponse,
                      (uint16_t)dsdl_CenterAndHui_HuiIoctl_1_0_FIXED_PORT_ID_,
                      (uint16_t)dsdl_CenterAndHui_HuiIoctl_Response_1_0_EXTENT_BYTES_,
                      HuiNode_OnHuiIoctlResponse) != SOA_OK)
    {
        ESP_LOGE(TAG, "订阅 HuiIoctl 失败");
    }

    /* 3b. 拉起 WiFi (ESP-NOW 的射频依赖)。
           ⚠️ 必须放在建节点/订阅之后: 射频一开就可能收到帧, 而分发靠“实例 + 订阅”
           两张表, 表还没建好时收到的帧会被直接丢弃。
           本板没有别的 WiFi 使用者 (上网走 ML307 的 4G), 所以这里直接把 STA 拉起来;
           若将来改用 WifiBoard 连 AP, 删掉这一句即可 —— ESP-NOW 会跟着那块 WiFi 起来。 */
    if (soa_espnow_wifi_start_sta() != SOA_OK)
    {
        ESP_LOGE(TAG, "ESP-NOW/WiFi 启动失败: HUI 将收不到主机心跳");
    }

    /* 4. 周期任务: 1 Hz 发 SlaveHeartbeat */
    if (xTaskCreate(HuiNode_Task, "hui_node", HUI_STACK_SIZE, nullptr,
                    HUI_TASK_PRIO, &s_task) != pdPASS)
    {
        ESP_LOGE(TAG, "创建 HUI 节点任务失败");
        s_task = nullptr;
        return;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "HUI 节点已上线: node_id=%u, HostHeartbeat=%u, SlaveHeartbeat=%u",
             (unsigned)HUI_NODE_ID,
             (unsigned)dsdl_Host_HostHeartbeat_1_0_FIXED_PORT_ID_,
             (unsigned)dsdl_Common_SlaveHeartbeat_1_0_FIXED_PORT_ID_);
}
