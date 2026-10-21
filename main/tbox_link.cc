/**
 * @file tbox_link.cc
 * @brief TBOX 专用链路 (方案 B) 的实现。设计说明见 tbox_link.h。
 *
 * 传输层用**一条独立的 TCP/TLS 连接 + 行分隔 JSON (NDJSON)**，不用第二个 MQTT 客户端。
 * 原因 (2026-10-05 在真机上实测出来的, 不要再改回 MQTT):
 *   `managed_components/78__esp-ml307/src/ml307/ml307_mqtt.cc` 里 `MQTTSTATE` 的 URC
 *   只有状态值、**不带客户端编号** (`+MQTTSTATE: 3`), 而每个 Ml307Mqtt 实例的 URC 回调
 *   都会收到它。于是第二个 MQTT 客户端做一次状态查询, 就会把第一个客户端的
 *   `connected_` 置成 false —— 之后第一个客户端的 `Publish()` 直接 `if (!connected_)
 *   return false`, 连 `AT+MQTTPUB` 都不发。谁后连谁把对方打掉, 顺序换过来也一样。
 *   而 `Ml307Tcp` 的 URC (`MIPOPEN`/`MIPCLOSE`/`MIPURC`/`MIPSTATE`) 全部比对
 *   `arguments[0] == tcp_id_`, 多路复用是安全的 —— 所以走 TCP。
 *
 * ⚠️ 另一条经验 (对 MQTT 版同样成立, 这里保留说明): 不要在 AT URC 回调里发 AT 命令。
 *   `Tcp::OnStream` 回调跑在模组的 AT 任务里, 所以**任何回写都不在那里做**, 而是
 *   `Application::Schedule` 到别的任务 (见 OnData)。
 */

#include "tbox_link.h"

#include <esp_log.h>

#if CONFIG_TBOX_LINK_ENABLE

#include "application.h"
#include "board.h"
#include "mcp_server.h"
#include "settings.h"

#include <esp_timer.h>
#include <cJSON.h>
#include <tcp.h>

#include <atomic>
#include <charconv>
#include <memory>
#include <string>

#define TAG "TboxLink"

/* TCP 连接 id —— 必须与全项目其它通道互不相同!
 *
 * ML307 上这个 id 直通 `AT+MIPOPEN=<id>,...`; 同一个 id 被第二次 Create* 时,
 * `Ml307Tcp/Udp::Connect()` 会先 `AT+MIPCLOSE=<id>` 把前一个 socket 关掉, 而且
 * **被抢占的一方收不到任何 URC** (disconn 被新对象消费了) -> 那条链路会「静默死亡」
 * (真机实测: 按 BOOT 唤醒 AI 时音频通道 CreateUdp(2) 抢走 id 2, TBOX 链路当场断掉
 *  却毫无日志, 直到 60s 后保活 ping 发送失败才暴露)。
 *
 * 当前占用: 0 = MQTT (mqtt_protocol.cc CreateMqtt(0)) 与 HTTP (ota/assets/notify/hui_manual),
 *           1 = WebSocket 协议 (websocket_protocol.cc CreateWebSocket(1)),
 *           2 = AI 音频 UDP (mqtt_protocol.cc CreateUdp(2)),
 *           3 = 摄像头 / 固件升级 HTTP (CreateHttp(3))。
 * 所以这里用 4 (留 5 备用)。改这里请先全项目搜 `CreateTcp\(|CreateUdp\(|CreateHttp\(|CreateSsl\(`。
 */
#define TBOX_LINK_CONNECT_ID 4

/* 断开后的重连间隔 (ms) */
#define TBOX_LINK_RECONNECT_MS 30000

/* 一行 JSON 的上限: 超了说明对端没按行分隔, 丢掉避免内存无限增长 */
#define TBOX_LINK_MAX_LINE_BYTES 16384

/* 链路协议版本, 放进 hello 里方便以后改协议 */
#define TBOX_LINK_PROTOCOL_VERSION 1

namespace {

/** 取值顺序: NVS("tbox") > Kconfig 默认。 */
std::string SettingOrDefault(Settings& settings, const char* key, const char* fallback) {
    std::string value = settings.GetString(key);
    return value.empty() ? std::string(fallback) : value;
}

class TboxLink {
public:
    static TboxLink& GetInstance() {
        static TboxLink instance;
        return instance;
    }

    void Start() {
        if (started_) {
            return;
        }
        started_ = true;

        Settings settings("tbox", false);
        endpoint_ = SettingOrDefault(settings, "endpoint", CONFIG_TBOX_LINK_ENDPOINT);
        channel_ = SettingOrDefault(settings, "channel", CONFIG_TBOX_LINK_CHANNEL);
        client_id_ = SettingOrDefault(settings, "client_id", CONFIG_TBOX_LINK_CLIENT_ID);
        token_ = SettingOrDefault(settings, "token", CONFIG_TBOX_LINK_TOKEN);
        ping_interval_s_ = settings.GetInt("ping", CONFIG_TBOX_LINK_PING_SECONDS);
        if (client_id_.empty()) {
            client_id_ = Board::GetInstance().GetUuid();
        }

        if (endpoint_.empty()) {
            ESP_LOGW(TAG, "未配置 endpoint, TBOX 链路不启动 (Kconfig 或 NVS \"tbox\")");
            return;
        }

        ESP_LOGI(TAG, "TBOX 链路: %s client_id=%s channel=%s", endpoint_.c_str(),
                 client_id_.c_str(), channel_.c_str());
        Connect();
    }

private:
    void Connect() {
        StopLink();

        auto network = Board::GetInstance().GetNetwork();
#if CONFIG_TBOX_LINK_TLS
        tcp_ = network->CreateSsl(TBOX_LINK_CONNECT_ID);
#else
        tcp_ = network->CreateTcp(TBOX_LINK_CONNECT_ID);
#endif
        if (tcp_ == nullptr) {
            ESP_LOGE(TAG, "创建 TCP 客户端失败, 稍后重试");
            ScheduleReconnect();
            return;
        }

        /* 回调跑在模组 AT 任务里: 只做缓冲区拼接, 真正处理调到别的任务 */
        tcp_->OnStream([this](const std::string& data) { OnData(data); });
        tcp_->OnDisconnected([this]() {
            ESP_LOGW(TAG, "TBOX 链路断开, %d s 后重连", TBOX_LINK_RECONNECT_MS / 1000);
            ScheduleReconnect();
        });

        std::string host = endpoint_;
        int port = 9000;
        size_t pos = endpoint_.find(':');
        if (pos != std::string::npos) {
            host = endpoint_.substr(0, pos);
            const std::string port_text = endpoint_.substr(pos + 1);
            int parsed_port = 0;
            auto [ptr, ec] =
                std::from_chars(port_text.data(), port_text.data() + port_text.size(), parsed_port);
            if (ec == std::errc() && parsed_port > 0 && parsed_port <= 65535) {
                port = parsed_port;
            } else {
                ESP_LOGW(TAG, "endpoint 端口不合法: %s (用 %d)", endpoint_.c_str(), port);
            }
        }

        if (auto connected = tcp_->Connect(host, port); !connected) {
            ESP_LOGE(TAG, "连接 %s 失败: %s, 稍后重试", endpoint_.c_str(),
                     connected.error().ToString().c_str());
            ScheduleReconnect();
            return;
        }

        ESP_LOGI(TAG, "已连上 TBOX 服务器 %s:%d", host.c_str(), port);
        /* 握手行: 让服务器知道这条连接是谁 (TCP 没有 client id 这种概念) */
        cJSON* hello = cJSON_CreateObject();
        if (hello != nullptr) {
            cJSON_AddStringToObject(hello, "type", "hello");
            cJSON_AddStringToObject(hello, "device_id", Board::GetInstance().GetUuid().c_str());
            cJSON_AddStringToObject(hello, "client_id", client_id_.c_str());
            cJSON_AddStringToObject(hello, "channel", channel_.c_str());
            cJSON_AddNumberToObject(hello, "version", TBOX_LINK_PROTOCOL_VERSION);
            if (!token_.empty()) {
                cJSON_AddStringToObject(hello, "token", token_.c_str());
            }
            SendJson(hello);
            cJSON_Delete(hello);
        }

        if (reconnect_timer_ != nullptr) {
            esp_timer_stop(reconnect_timer_);
        }
        StartPingTimer();
    }

    void StopLink() {
        if (ping_timer_ != nullptr) {
            esp_timer_stop(ping_timer_);
        }
        if (tcp_ != nullptr) {
            tcp_->Disconnect();
            tcp_.reset();
        }
        rx_buffer_.clear();
    }

    void StartPingTimer() {
        if (ping_interval_s_ <= 0) {
            return;
        }
        if (ping_timer_ == nullptr) {
            esp_timer_create_args_t args = {
                .callback = [](void* arg) { static_cast<TboxLink*>(arg)->SendPing(); },
                .arg = this,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "tbox_ping",
                .skip_unhandled_events = true,
            };
            if (esp_timer_create(&args, &ping_timer_) != ESP_OK) {
                ESP_LOGE(TAG, "创建保活定时器失败");
                return;
            }
        }
        esp_timer_stop(ping_timer_);
        esp_timer_start_periodic(ping_timer_, (int64_t)ping_interval_s_ * 1000000);
    }

    void SendPing() {
        cJSON* ping = cJSON_CreateObject();
        if (ping == nullptr) {
            return;
        }
        cJSON_AddStringToObject(ping, "type", "ping");
        SendJson(ping);
        cJSON_Delete(ping);
    }

    /** 发一行 JSON (带换行)。注意: 调用方不能是 AT URC 回调上下文。 */
    void SendJson(const cJSON* json) {
        if (tcp_ == nullptr) {
            return;
        }
        char* text = cJSON_PrintUnformatted(json);
        if (text == nullptr) {
            return;
        }
        std::string line(text);
        cJSON_free(text);
        line += "\n";
        if (tcp_->Send(line) < 0) {
            /* socket 可能已被对端/网络静默关闭 (收不到 MIPURC disconn), 这时只靠
             * OnDisconnected 永远等不到重连 —— 发送失败是最可靠的失效信号。
             * 这里只起定时器, 真正的 Disconnect/重连回到主任务里做。 */
            ESP_LOGE(TAG, "发送失败, 按链路失效处理, %d s 后重连", TBOX_LINK_RECONNECT_MS / 1000);
            ScheduleReconnect();
        }
    }

    /** AT 任务上下文: 只往缓冲区里拼, 拼出整行后交给别的任务处理。 */
    void OnData(const std::string& chunk) {
        rx_buffer_ += chunk;
        size_t start = 0;
        while (true) {
            size_t newline = rx_buffer_.find('\n', start);
            if (newline == std::string::npos) {
                break;
            }
            std::string line = rx_buffer_.substr(start, newline - start);
            start = newline + 1;
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.empty()) {
                continue;
            }
            auto alive = alive_;
            Application::GetInstance().Schedule([this, alive, line]() {
                if (*alive) {
                    HandleLine(line);
                }
            });
        }
        rx_buffer_.erase(0, start);
        if (rx_buffer_.size() > TBOX_LINK_MAX_LINE_BYTES) {
            ESP_LOGW(TAG, "收到的数据没有换行分隔 (累计 %u 字节), 丢弃", (unsigned)rx_buffer_.size());
            rx_buffer_.clear();
        }
    }

    void ScheduleReconnect() {
        if (reconnect_timer_ == nullptr) {
            esp_timer_create_args_t args = {
                .callback =
                    [](void* arg) {
                        auto* link = static_cast<TboxLink*>(arg);
                        auto alive = link->alive_;
                        Application::GetInstance().Schedule([link, alive]() {
                            if (*alive) {
                                link->Connect();
                            }
                        });
                    },
                .arg = this,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "tbox_reconnect",
                .skip_unhandled_events = true,
            };
            if (esp_timer_create(&args, &reconnect_timer_) != ESP_OK) {
                ESP_LOGE(TAG, "创建重连定时器失败");
                return;
            }
        }
        esp_timer_stop(reconnect_timer_);
        esp_timer_start_once(reconnect_timer_, (int64_t)TBOX_LINK_RECONNECT_MS * 1000);
    }

    /** 回执封装成 {"type":"mcp","payload":<JSON-RPC>}，与 AI 通道同构。 */
    McpServer::ResponseSender MakeReplySender() {
        return [this](const std::string& rpc_payload) {
            cJSON* message = cJSON_CreateObject();
            if (message == nullptr) {
                return;
            }
            cJSON_AddStringToObject(message, "type", "mcp");
            cJSON_AddRawToObject(message, "payload", rpc_payload.c_str());
            SendJson(message);
            cJSON_Delete(message);
        };
    }

    /** 在主任务里处理一行 (可能来自服务器)。 */
    void HandleLine(const std::string& line) {
        cJSON* root = cJSON_Parse(line.c_str());
        if (root == nullptr) {
            ESP_LOGW(TAG, "收到非 JSON 行, 忽略: %.120s", line.c_str());
            return;
        }

        cJSON* type = cJSON_GetObjectItem(root, "type");
        const char* type_text = cJSON_IsString(type) ? type->valuestring : "";
        if (strcmp(type_text, "ping") == 0) {
            /* 服务器保活, 无需处理 */
        } else if (strcmp(type_text, "mcp") == 0) {
            cJSON* rpc = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(rpc)) {
                ESP_LOGI(TAG, "TBOX 下发 MCP: %.160s", line.c_str());
                McpServer::GetInstance().ParseMessage(rpc, MakeReplySender());
            } else {
                ESP_LOGW(TAG, "type=mcp 但 payload 不是对象");
            }
        } else if (cJSON_GetObjectItem(root, "method") != nullptr) {
            /* 裸 JSON-RPC (没套 type) —— 宽容一点, 直接转发 */
            ESP_LOGI(TAG, "TBOX 下发 JSON-RPC: %.160s", line.c_str());
            McpServer::GetInstance().ParseMessage(root, MakeReplySender());
        } else if (cJSON_GetObjectItem(root, "name") != nullptr) {
            /* 便捷形式: {"type":"command","name":..,"arguments":{..}} */
            ESP_LOGI(TAG, "TBOX 下发命令: %.160s", line.c_str());
            McpServer::GetInstance().CallTool(
                cJSON_GetObjectItem(root, "name")->valuestring,
                cJSON_GetObjectItem(root, "arguments"), MakeReplySender());
        } else {
            ESP_LOGW(TAG, "无法识别的消息 (既没有 method/payload, 也没有 name)");
        }
        cJSON_Delete(root);
    }

    bool started_ = false;
    std::string endpoint_;
    std::string channel_;
    std::string client_id_;
    std::string token_;
    int ping_interval_s_ = 60;
    std::string rx_buffer_;
    std::unique_ptr<Tcp> tcp_;
    esp_timer_handle_t reconnect_timer_ = nullptr;
    esp_timer_handle_t ping_timer_ = nullptr;
    /* 让已经排进主任务的回调在对象失效后不再访问 (与 mqtt_protocol.cc 同一套做法) */
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
};

}  // namespace

void TboxLink_Start(void) { TboxLink::GetInstance().Start(); }

#else

void TboxLink_Start(void) {}

#endif  /* CONFIG_TBOX_LINK_ENABLE */
