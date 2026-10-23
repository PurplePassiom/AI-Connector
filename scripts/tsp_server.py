#!/usr/bin/env python3
"""测试用 TSP: 联调 "人在服务器端选择执行" 这条路径 (TBOX + AIBOX 二合一产品)。

一个进程里提供三件事, 只用 Python 标准库, 不需要任何第三方依赖:

  1. OTA 端点    GET/POST /xiaozhi/ota/  -> 返回 mqtt 配置, 把设备引到本进程的
     MQTT broker 上。broker 地址直接从请求的 Host 头推导, 不用手工填 IP。
  2. MQTT broker  (默认 1883, 明文; 设备端只有端口 == 8883 才启用 SSL)
  3. 网页控制台   http://<host>:<http-port>/  —— 列出设备 tools/list 回来的工具,
     点一下就把 {"type":"command","name":...,"arguments":{...}} 发给设备。

设备侧前置条件:
  * 固件打开 CONFIG_TBOX_COMMAND_MESSAGE  (main/Kconfig.projbuild)
  * CONFIG_OTA_URL 指向 http://<本机IP>:<http-port>/xiaozhi/ota/

用法:
    python scripts/tsp_server.py --host 0.0.0.0 --http-port 8000 --mqtt-port 1883
"""

import argparse
import json
import socket
import sys
import threading
import time
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse

# ---------------------------------------------------------------- MQTT 常量

CONNECT, CONNACK, PUBLISH, PUBACK = 1, 2, 3, 4
SUBSCRIBE, SUBACK, UNSUBSCRIBE, UNSUBACK = 8, 9, 10, 11
PINGREQ, PINGRESP, DISCONNECT = 12, 13, 14

MQTT_PACKET_NAMES = {
    CONNECT: "CONNECT", CONNACK: "CONNACK", PUBLISH: "PUBLISH", PUBACK: "PUBACK",
    SUBSCRIBE: "SUBSCRIBE", SUBACK: "SUBACK", UNSUBSCRIBE: "UNSUBSCRIBE",
    UNSUBACK: "UNSUBACK", PINGREQ: "PINGREQ", PINGRESP: "PINGRESP",
    DISCONNECT: "DISCONNECT",
}

DEFAULT_OTA_PATH = "/xiaozhi/ota/"


# ---------------------------------------------------------------- 编解码


def encode_remaining_length(length: int) -> bytes:
    """MQTT 变长 "Remaining Length" 字段。"""
    if length < 0 or length > 268435455:
        raise ValueError(f"remaining length out of range: {length}")
    out = bytearray()
    while True:
        digit = length % 128
        length //= 128
        if length:
            digit |= 0x80
        out.append(digit)
        if not length:
            return bytes(out)


def encode_packet(packet_type: int, flags: int, body: bytes) -> bytes:
    return bytes([(packet_type << 4) | (flags & 0x0F)]) + encode_remaining_length(
        len(body)
    ) + body


def encode_utf8(text: str) -> bytes:
    raw = text.encode("utf-8")
    return len(raw).to_bytes(2, "big") + raw


def _read_exact(sock: socket.socket, count: int):
    buf = b""
    while len(buf) < count:
        try:
            chunk = sock.recv(count - len(buf))
        except OSError:
            return None
        if not chunk:
            return None
        buf += chunk
    return buf


def read_packet(sock: socket.socket):
    """读一个完整的 MQTT 报文; 对端关闭时返回 None。"""
    first = _read_exact(sock, 1)
    if first is None:
        return None
    multiplier = 1
    length = 0
    while True:
        digit = _read_exact(sock, 1)
        if digit is None:
            return None
        length += (digit[0] & 0x7F) * multiplier
        if not digit[0] & 0x80:
            break
        multiplier *= 128
        if multiplier > 128 ** 3:
            raise ValueError("malformed remaining length")
    body = _read_exact(sock, length) if length else b""
    if body is None:
        return None
    return first[0] >> 4, first[0] & 0x0F, body


def parse_connect(body: bytes) -> dict:
    pos = 0

    def read_utf8() -> str:
        nonlocal pos
        size = int.from_bytes(body[pos : pos + 2], "big")
        pos += 2
        text = body[pos : pos + size].decode("utf-8", "replace")
        pos += size
        return text

    protocol_name = read_utf8()
    protocol_level = body[pos]
    pos += 1
    connect_flags = body[pos]
    pos += 1
    keepalive = int.from_bytes(body[pos : pos + 2], "big")
    pos += 2
    client_id = read_utf8()
    username = ""
    password = ""
    # MQTT 3.1.1 CONNECT flags: bit7=0x80 username, bit6=0x40 password,
    # bit5=0x20 will retain, bit2=0x04 will flag (后面跟 will topic + will message)。
    # ⚠️ 这三位以前写错位了 (0x80 当成 will、0x40 当 username), 结果带用户名/密码
    #    的设备会把 password 读成 username —— 一做鉴权就会误拒真设备。
    if connect_flags & 0x04:  # will flag
        read_utf8()  # will topic
        read_utf8()  # will message
    if connect_flags & 0x80:  # username
        username = read_utf8()
    if connect_flags & 0x40:  # password
        password = read_utf8()
    return {
        "protocol_name": protocol_name,
        "protocol_level": protocol_level,
        "keepalive": keepalive,
        "client_id": client_id,
        "username": username,
        "password": password,
    }


def parse_publish(flags: int, body: bytes):
    qos = (flags >> 1) & 0x03
    topic_len = int.from_bytes(body[0:2], "big")
    topic = body[2 : 2 + topic_len].decode("utf-8", "replace")
    pos = 2 + topic_len
    packet_id = None
    if qos > 0:
        packet_id = int.from_bytes(body[pos : pos + 2], "big")
        pos += 2
    return topic, packet_id, body[pos:]


def parse_subscribe(body: bytes):
    packet_id = int.from_bytes(body[0:2], "big")
    pos = 2
    filters = []
    while pos + 2 <= len(body):
        size = int.from_bytes(body[pos : pos + 2], "big")
        pos += 2
        topic_filter = body[pos : pos + size].decode("utf-8", "replace")
        pos += size
        if pos >= len(body):
            break
        qos = body[pos]
        pos += 1
        filters.append((topic_filter, qos))
    return packet_id, filters


def topic_matches(topic_filter: str, topic: str) -> bool:
    """MQTT 通配符匹配 (+ 单层, # 多层)。"""
    filter_parts = topic_filter.split("/")
    topic_parts = topic.split("/")
    for index, part in enumerate(filter_parts):
        if part == "#":
            return True
        if index >= len(topic_parts):
            return False
        if part != "+" and part != topic_parts[index]:
            return False
    return len(filter_parts) == len(topic_parts)


# ---------------------------------------------------------------- 日志


class LogRing:
    """内存里的日志环 + 同时打到 stdout, 供网页控制台轮询。"""

    def __init__(self, capacity: int = 400, echo: bool = True):
        self._lines = deque(maxlen=capacity)
        self._lock = threading.Lock()
        self._echo = echo

    def __call__(self, message: str) -> None:
        line = f"{time.strftime('%H:%M:%S')} {message}"
        with self._lock:
            self._lines.append(line)
        if self._echo:
            print(line, flush=True)

    def tail(self) -> list:
        with self._lock:
            return list(self._lines)


# ---------------------------------------------------------------- MQTT broker


class MqttBroker:
    """只够测试用的 MQTT 3.1.1 服务端。

    刻意保留的一点: 除了正常的 SUBSCRIBE 路由, 还会把 publish_topic 上的消息
    **强制投递**给 "被指定的设备连接" (expect_device)。原因: 固件侧只认
    publish_topic, 从未调用 Subscribe(); 生产环境不是靠固件订阅收下行, 就是靠
    服务端按 client_id 直投。测试台用后者, 结果与生产等价, 而且不依赖模组行为。
    设备真的发了 SUBSCRIBE 也会被如实记录 (device_subscribed + 日志), 顺便把答案测出来。

    ⚠️ **公网可达时必须设 username/password**: 无鉴权的 1883 会被互联网扫描器
    (Nmap 的 mqtt-subscribe 脚本、iw-xxxx 之类) 连上订阅 `#`, 既能窥探下行内容,
    又能往设备主题发命令。凭据由 OTA 响应的 mqtt 段下发给设备, 设备会用
    AT+MQTTCONN 带上, 所以配好之后设备照常连、扫描器连不上。
    """

    def __init__(self, log: LogRing, on_message=None, on_connect=None, on_subscribe=None,
                 username=None, password=None):
        self._log = log
        # None = 允许匿名 (只适合本机调试)。公网监听请务必传值。
        self._username = username
        self._password = password
        self._on_message = on_message
        self._on_connect = on_connect
        self._on_subscribe = on_subscribe
        self._lock = threading.RLock()
        self._clients = {}  # client_id -> client dict
        self._forced = {}  # client_id -> set(topic)
        self._force_seen = set()  # 已打过一次日志的 client_id
        self._listener = None
        self.port = None
        self.device_subscribed = False
        self.forced_deliveries = 0

    # ------------------------------------------------------------ 生命周期

    def start(self, host: str, port: int) -> None:
        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind((host, port))
        listener.listen(8)
        self._listener = listener
        self.port = listener.getsockname()[1]
        threading.Thread(target=self._accept_loop, daemon=True).start()
        self._log(f"MQTT broker 监听 {host}:{self.port}")

    def stop(self) -> None:
        if self._listener is not None:
            try:
                self._listener.close()
            except OSError:
                pass
            self._listener = None

    def _accept_loop(self) -> None:
        listener = self._listener
        while listener is not None:
            try:
                conn, addr = listener.accept()
            except OSError:
                return
            threading.Thread(target=self._serve, args=(conn, addr), daemon=True).start()

    # ------------------------------------------------------------ 连接处理

    def _serve(self, conn: socket.socket, addr) -> None:
        client_id = None
        self._log(f"MQTT 连接来自 {addr[0]}:{addr[1]}")
        try:
            while True:
                packet = read_packet(conn)
                if packet is None:
                    break
                packet_type, flags, body = packet
                name = MQTT_PACKET_NAMES.get(packet_type, str(packet_type))

                if packet_type == CONNECT:
                    info = parse_connect(body)
                    client_id = info["client_id"]
                    if not self._credentials_ok(info):
                        # CONNACK return code 5 = not authorized; 不登记, 直接断开
                        conn.sendall(encode_packet(CONNACK, 0, b"\x00\x05"))
                        self._log(
                            f"CONNECT 拒绝 client_id={client_id!r} "
                            f"user={info['username']!r} (凭据不符, 疑似扫描器) "
                            f"from {addr[0]}"
                        )
                        return
                    self._register(client_id, conn, addr, info)
                    conn.sendall(encode_packet(CONNACK, 0, b"\x00\x00"))
                    self._log(
                        f"CONNECT client_id={client_id!r} keepalive={info['keepalive']}s "
                        f"user={info['username']!r} -> CONNACK ok"
                    )
                    if self._on_connect is not None:
                        self._on_connect(client_id)
                elif packet_type == PINGREQ:
                    conn.sendall(encode_packet(PINGRESP, 0, b""))
                    self._log("PINGREQ -> PINGRESP")
                elif packet_type == SUBSCRIBE:
                    packet_id, filters = parse_subscribe(body)
                    self._subscribe(client_id, filters)
                    conn.sendall(
                        encode_packet(SUBACK, 0, packet_id.to_bytes(2, "big")
                                      + bytes(len(filters)))
                    )
                    self._log(
                        f"SUBSCRIBE {[f for f, _ in filters]} -> SUBACK (granted qos 0)"
                    )
                    self.device_subscribed = True
                    if self._on_subscribe is not None:
                        self._on_subscribe(client_id, [f for f, _ in filters])
                elif packet_type == UNSUBSCRIBE:
                    packet_id = int.from_bytes(body[0:2], "big")
                    conn.sendall(encode_packet(UNSUBACK, 0, packet_id.to_bytes(2, "big")))
                    self._log("UNSUBSCRIBE -> UNSUBACK")
                elif packet_type == PUBLISH:
                    topic, packet_id, payload = parse_publish(flags, body)
                    if packet_id is not None:
                        conn.sendall(encode_packet(PUBACK, 0, packet_id.to_bytes(2, "big")))
                    self._remember_publish(client_id, topic)
                    text = payload.decode("utf-8", "replace")
                    self._log(f"PUBLISH <- 设备 topic={topic} {len(payload)}B: {_short(text)}")
                    if self._on_message is not None:
                        self._on_message(client_id, topic, text)
                    self._route(topic, payload, origin=client_id)
                elif packet_type == DISCONNECT:
                    self._log("DISCONNECT")
                    break
                else:
                    self._log(f"忽略报文 {name}")
        except (OSError, ValueError) as error:
            self._log(f"连接异常 ({addr[0]}): {error}")
        finally:
            self._unregister(client_id, conn)
            try:
                conn.close()
            except OSError:
                pass
            self._log(f"连接关闭 {addr[0]}:{addr[1]}")

    # ------------------------------------------------------------ 状态表

    def _credentials_ok(self, info: dict) -> bool:
        """没配凭据就放行 (本机调试); 配了就要求完全一致。"""
        if self._username is None and self._password is None:
            return True
        return (info.get("username") or "") == (self._username or "") and \
               (info.get("password") or "") == (self._password or "")

    def _register(self, client_id: str, conn, addr, info: dict) -> None:
        with self._lock:
            previous = self._clients.pop(client_id, None)
            self._clients[client_id] = {
                "id": client_id,
                "sock": conn,
                "addr": addr,
                "info": info,
                "subs": set(),
                "published": set(),
                "send_lock": threading.Lock(),
            }
        if previous is not None:
            self._log(f"client_id={client_id!r} 重复连接, 已替换旧连接")

    def _unregister(self, client_id, conn) -> None:
        if client_id is None:
            return
        with self._lock:
            current = self._clients.get(client_id)
            if current is not None and current["sock"] is conn:
                del self._clients[client_id]

    def _subscribe(self, client_id, filters) -> None:
        if client_id is None:
            return
        with self._lock:
            current = self._clients.get(client_id)
            if current is not None:
                current["subs"].update(topic_filter for topic_filter, _ in filters)

    def _remember_publish(self, client_id, topic: str) -> None:
        if client_id is None:
            return
        with self._lock:
            current = self._clients.get(client_id)
            if current is not None:
                current["published"].add(topic)

    def expect_device(self, client_id: str, topic: str) -> None:
        """把某个 MQTT 连接指定为 "该 topic 的设备", 之后不需要它订阅也能收到下行。"""
        if not client_id:
            return
        with self._lock:
            self._forced.setdefault(client_id, set()).add(topic)

    def clients(self) -> list:
        with self._lock:
            return [
                {"id": c["id"], "addr": f"{c['addr'][0]}:{c['addr'][1]}",
                 "subscriptions": sorted(c["subs"]), "published_topics": sorted(c["published"])}
                for c in self._clients.values()
            ]

    # ------------------------------------------------------------ 投递

    def publish(self, topic: str, payload, qos: int = 0) -> int:
        """TSP 自己往设备发消息 (不经过 socket, 直接进路由)。返回投递份数。"""
        if isinstance(payload, (dict, list)):
            payload = json.dumps(payload, ensure_ascii=False)
        data = payload.encode("utf-8") if isinstance(payload, str) else payload
        return self._route(topic, data, origin=None, log_outgoing=True, allow_forced=True)

    def _route(self, topic: str, payload: bytes, origin, log_outgoing: bool = False,
               allow_forced: bool = False) -> int:
        """origin=None 表示 TSP 自己发起的下行, 可以直投设备; 否则是**某个客户端**发来的,
        此时只按订阅投递, 绝不做 "指定设备直投" —— 否则任何人都能往设备主题发命令,
        而设备固件会把它当服务器消息执行 (真机上等于远程命令执行)。
        """
        with self._lock:
            candidates = list(self._clients.values())

        delivered = 0
        for client in candidates:
            if origin is not None and client["id"] == origin:
                continue
            matched = any(topic_matches(f, topic) for f in client["subs"])
            forced = allow_forced and topic in self._forced.get(client["id"], ())
            if not matched and not forced:
                continue
            if forced and not matched:
                self.forced_deliveries += 1
                if client["id"] not in self._force_seen:
                    self._force_seen.add(client["id"])
                    self._log(
                        f"强制投递生效: client_id={client['id']!r} 没有订阅 {topic}, "
                        f"按指定设备直投 (它确实从未 Subscribe)"
                    )
            body = encode_utf8(topic) + payload
            with client["send_lock"]:
                try:
                    client["sock"].sendall(encode_packet(PUBLISH, 0, body))
                    delivered += 1
                except OSError as error:
                    self._log(f"投递失败 client_id={client['id']!r}: {error}")
            if log_outgoing:
                self._log(
                    f"PUBLISH -> 设备 topic={topic} {len(payload)}B: "
                    f"{_short(payload.decode('utf-8', 'replace'))}"
                )
        if log_outgoing and delivered == 0:
            self._log(f"没有连接会收到 {topic} (设备还没连上?)")
        return delivered


def _short(text: str, limit: int = 220) -> str:
    text = text.replace("\n", " ")
    return text if len(text) <= limit else text[:limit] + "…"


# ---------------------------------------------------------------- 测试台逻辑


class TcpLinkServer:
    """TBOX 链路的 TCP + 行分隔 JSON (NDJSON) 端点 (台架用, 明文)。

    固件 `main/tbox_link.cc` 每行一个 JSON; 连上后先发一行 hello 说明自己是谁。
    为什么不用第二个 MQTT 连接: ML307 的 `MQTTSTATE` URC 不带客户端编号, 两个
    Ml307Mqtt 实例会互相把对方的 connected_ 清成 false (真机实测)。
    """

    def __init__(self, log: LogRing, on_line, on_gone, token: str = ""):
        self._log = log
        self._on_line = on_line      # (client_id, line) -> None
        self._on_gone = on_gone      # (client_id) -> None
        # hello 里带的 token 必须与它一致 (空 = 不校验, 只本机调试用)。
        # 固件侧来源: NVS "tbox"/token 或 CONFIG_TBOX_LINK_TOKEN。
        self._token = token
        self._lock = threading.RLock()
        self._links = {}             # client_id -> {"sock", "send_lock", "addr"}
        self._listener = None
        self.port = None

    def start(self, host: str, port: int) -> None:
        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind((host, port))
        listener.listen(4)
        self._listener = listener
        self.port = listener.getsockname()[1]
        threading.Thread(target=self._accept_loop, daemon=True).start()
        self._log(f"TBOX 链路 (TCP/NDJSON) 监听 {host}:{self.port}")

    def stop(self) -> None:
        if self._listener is not None:
            try:
                self._listener.close()
            except OSError:
                pass
            self._listener = None

    def _accept_loop(self) -> None:
        listener = self._listener
        while listener is not None:
            try:
                conn, addr = listener.accept()
            except OSError:
                return
            threading.Thread(target=self._serve, args=(conn, addr), daemon=True).start()

    def _serve(self, conn: socket.socket, addr) -> None:
        self._log(f"TBOX 链路接入 {addr[0]}:{addr[1]}")
        client_id = None
        buffer = ""
        try:
            while True:
                chunk = conn.recv(4096)
                if not chunk:
                    break
                buffer += chunk.decode("utf-8", "replace")
                while "\n" in buffer:
                    line, buffer = buffer.split("\n", 1)
                    line = line.strip()
                    if not line:
                        continue
                    if client_id is None:
                        client_id = self._register_from_hello(line, conn, addr)
                        if client_id is None:
                            return   # hi 不合法/token 不符: 交给 finally 关闭连接
                    else:
                        self._on_line(client_id, line)
                if len(buffer) > 65536:
                    self._log(f"TBOX 链路收到超长未分行数据, 丢弃 ({len(buffer)} 字节)")
                    buffer = ""
        except OSError as error:
            self._log(f"TBOX 链路异常 ({addr[0]}): {error}")
        finally:
            self._unregister(client_id, conn)
            try:
                conn.close()
            except OSError:
                pass
            self._log(f"TBOX 链路关闭 {addr[0]}:{addr[1]}")

    def _register_from_hello(self, line: str, conn, addr):
        try:
            message = json.loads(line)
        except ValueError:
            self._log(f"TBOX 链路第一行不是 JSON, 断开: {_short(line)}")
            return None
        if not isinstance(message, dict) or message.get("type") != "hello":
            self._log("TBOX 链路第一行不是 hello, 断开")
            return None
        if self._token and message.get("token") != self._token:
            self._log(f"TBOX 链路 hello token 不符, 断开 {addr[0]}:{addr[1]}")
            return None
        client_id = message.get("client_id") or f"{addr[0]}:{addr[1]}"
        with self._lock:
            previous = self._links.pop(client_id, None)
            self._links[client_id] = {"sock": conn, "addr": addr,
                                      "send_lock": threading.Lock()}
        if previous is not None:
            self._log(f"TBOX 链路 client_id={client_id!r} 重复接入, 已替换旧连接")
        self._on_line(client_id, line)   # 交给 app 处理 hello (注册 Device / 探测工具)
        return client_id

    def _unregister(self, client_id, conn) -> None:
        if client_id is None:
            return
        with self._lock:
            current = self._links.get(client_id)
            if current is not None and current["sock"] is conn:
                del self._links[client_id]
        if self._on_gone is not None:
            self._on_gone(client_id)

    def send(self, client_id: str, message) -> int:
        with self._lock:
            link = self._links.get(client_id)
        if link is None:
            return 0
        data = (json.dumps(message, ensure_ascii=False) + "\n").encode("utf-8")
        with link["send_lock"]:
            try:
                link["sock"].sendall(data)
            except OSError as error:
                self._log(f"TBOX 链路发送失败 client_id={client_id!r}: {error}")
                return 0
        self._log(f"TBOX 链路 -> {client_id!r}: {_short(data.decode('utf-8', 'replace'))}")
        return 1

    def clients(self) -> list:
        with self._lock:
            return [{"client_id": cid, "addr": f"{info['addr'][0]}:{info['addr'][1]}"}
                    for cid, info in self._links.items()]


class Device:
    """一条连到本 broker 的设备连接。

    TBOX + AIBOX 二合一时会同时出现两条, 必须分得清:
      * AI 通道  (client_id = 设备 UUID)  —— 固件从不 Subscribe, 靠 broker 直投
      * TBOX 链路 (client_id = 配置里的 id) —— 固件会正常 Subscribe
    两者的主题是分开的, 控制台选哪条就往哪条发。
    """

    def __init__(self, client_id: str, topic: str, kind: str = "mqtt"):
        self.client_id = client_id
        self.topic = topic
        self.kind = kind          # "mqtt" = AI 通道 / "tcp" = TBOX 独立链路
        self.tools = {}
        self.subscribed = False
        self.probed = False
        self.replies = 0

    def summary(self) -> dict:
        return {
            "client_id": self.client_id,
            "kind": self.kind,
            "topic": self.topic,
            "subscribed": self.subscribed,
            "tool_count": len(self.tools),
            "tools": list(self.tools.values()),
            "replies": self.replies,
        }


class TspApp:
    """TSP 侧的状态机: 探询工具、把人的选择翻成 command 消息。

    支持多条设备连接 (AI 通道 + TBOX 链路), 控制台里选一条来下发。
    """

    def __init__(self, log: LogRing, publish_topic: str = "", token: str = "",
                 mqtt_user: str = "", mqtt_password: str = ""):
        self.log = log
        # OTA 响应要把同样的凭据下发给设备 (固件会用它去 AT+MQTTCONN)
        self.mqtt_user = mqtt_user
        self.mqtt_password = mqtt_password
        self.broker = MqttBroker(log, on_message=self._on_device_publish,
                                 on_connect=self._on_device_connect,
                                 on_subscribe=self._on_device_subscribe,
                                 username=mqtt_user or None,
                                 password=mqtt_password or None)
        self.tcp = TcpLinkServer(log, on_line=self._on_tcp_line, on_gone=self._on_tcp_gone,
                                 token=token)
        self.devices = {}          # client_id -> Device
        self.selected = None       # 控制台当前选中的 client_id
        self._request_id = 0
        self._default_topic = publish_topic

    @staticmethod
    def topic_for(client_id: str) -> str:
        return f"tsp/dev/{client_id}"

    # -------------------------------------------------- 设备连上 / 订阅

    def _ensure_device(self, client_id: str, topic: str = None, kind: str = "mqtt") -> Device:
        device = self.devices.get(client_id)
        if device is not None and device.kind != kind:
            # 同一个 client_id 换了传输通道 (例如换固件: 旧的 MQTT 链路 -> 新的 TCP 链路)。
            # 必须把 kind 改过来并重新探测, 否则命令会往已经没有连接的那条路上发。
            self.log(f"设备 {client_id!r} 换了链路: {device.kind} -> {kind}")
            device.kind = kind
            if topic:
                device.topic = topic
            device.tools = {}
            device.replies = 0
            device.probed = False
        if device is None:
            topic = topic or self._default_topic or self.topic_for(client_id)
            device = Device(client_id, topic, kind)
            self.devices[client_id] = device
            if self.selected is None:
                self.selected = client_id
            self.log(f"发现设备连接 {client_id!r} ({kind}), channel={topic}")
        if kind == "mqtt":
            self.broker.expect_device(client_id, device.topic)
        return device

    def _on_device_connect(self, client_id: str) -> None:
        """MQTT 设备 (AI 通道) 一 CONNECT 就把它认下来, 并自动探一次工具列表。"""
        device = self._ensure_device(client_id)
        if device.probed:
            return
        device.probed = True
        # 稍等一拍再探, 让设备把 CONNACK 之后的事情 (订阅等) 做完
        threading.Timer(1.0, self.probe, args=(client_id,)).start()

    def _on_device_subscribe(self, client_id, filters) -> None:
        device = self._ensure_device(client_id)
        device.subscribed = True
        self.log(f"设备 {client_id!r} 订阅了 {list(filters)} (会正常收到下行)")

    # -------------------------------------------------- TBOX 链路 (TCP)

    def _on_tcp_line(self, client_id: str, line: str) -> None:
        try:
            message = json.loads(line)
        except ValueError:
            self.log(f"[{client_id}] TBOX 链路发来的不是 JSON: {_short(line)}")
            return
        if not isinstance(message, dict):
            return
        if message.get("type") == "ping":
            return
        if message.get("type") == "hello":
            channel = message.get("channel") or self.topic_for(client_id)
            device = self._ensure_device(client_id, channel, kind="tcp")
            device.subscribed = True   # 点对点链路, 不存在"订阅"这一步
            self.log(f"[{client_id}] hello device_id={message.get('device_id')!r} "
                     f"channel={channel!r} version={message.get('version')}")
            if not device.probed:
                device.probed = True
                threading.Timer(0.5, self.probe, args=(client_id,)).start()
            return
        self._handle_device_message(client_id, line, message)

    def _on_tcp_gone(self, client_id: str) -> None:
        self.log(f"TBOX 链路 {client_id!r} 断开")

    # -------------------------------------------------- 设备 -> TSP

    def _on_device_publish(self, client_id, topic: str, payload: str) -> None:
        try:
            message = json.loads(payload)
        except ValueError:
            self.log(f"[{client_id}] 设备发来的不是 JSON, 已忽略: {_short(payload)}")
            return
        if not isinstance(message, dict):
            return
        self._handle_device_message(client_id, payload, message)

    def _handle_device_message(self, client_id, raw: str, message: dict) -> None:
        # 注意: 这里**不能**调 _ensure_device (它默认 kind="mqtt"), 否则 TCP 链路来的回执
        # 会把该设备的 kind 翻回 mqtt, 之后命令就发到已经没有连接的那条路上了。
        # 设备在处理任何消息之前一定已经被 hello / CONNECT 注册过, 查表即可。
        device = self.devices.get(client_id) if client_id else None
        tag = f"[{client_id}]" if client_id else "[?]"

        message_type = message.get("type")
        if message_type == "goodbye":
            self.log(f"{tag} 设备发送 goodbye")
            return
        if message_type != "mcp":
            self.log(f"{tag} 设备消息 type={message_type!r} (已记录, 未处理)")
            return

        rpc = message.get("payload")
        if not isinstance(rpc, dict):
            return
        if device is not None:
            device.replies += 1
        if "error" in rpc:
            self.log(f"{tag} MCP 错误回执 id={rpc.get('id')}: {rpc['error']}")
            return
        result = rpc.get("result")
        if isinstance(result, dict) and isinstance(result.get("tools"), list):
            self._store_tools(device, result["tools"], tag)
        else:
            self.log(f"{tag} MCP 回执 id={rpc.get('id')}: "
                     f"{_short(json.dumps(rpc, ensure_ascii=False))}")

    def _store_tools(self, device, tools: list, tag: str) -> None:
        if device is None:
            return
        device.tools = {}
        for tool in tools:
            if isinstance(tool, dict) and isinstance(tool.get("name"), str):
                device.tools[tool["name"]] = tool
        self.log(f"{tag} 从设备拿到 {len(device.tools)} 个工具 (tools/list)")

    # -------------------------------------------------- TSP -> 设备

    def _next_id(self) -> int:
        self._request_id += 1
        return self._request_id

    def _resolve(self, client_id=None):
        target = client_id or self.selected
        device = self.devices.get(target) if target else None
        if device is None:
            self.log(f"没有可下发的设备 (client_id={target!r})")
        return device

    def _send_to_device(self, device, message) -> int:
        """投递到设备: MQTT 设备走 broker 的 topic, TBOX 链路走它自己的 TCP 连接。"""
        if device.kind == "tcp":
            return self.tcp.send(device.client_id, message)
        return self.broker.publish(device.topic, message)

    def send_mcp(self, rpc: dict, client_id=None) -> int:
        device = self._resolve(client_id)
        if device is None:
            return 0
        return self._send_to_device(
            device, {"session_id": "tsp", "type": "mcp", "payload": rpc})

    def probe(self, client_id=None) -> int:
        """让设备把它注册的工具体系报一遍 (设备不会主动上报)。"""
        if self._resolve(client_id) is None:
            return 0
        sent = self.send_mcp({
            "jsonrpc": "2.0", "id": self._next_id(), "method": "initialize",
            "params": {"protocolVersion": "2024-11-05", "capabilities": {},
                       "clientInfo": {"name": "tsp-test-server", "version": "1.0"}},
        }, client_id)
        sent += self.send_mcp({
            "jsonrpc": "2.0", "id": self._next_id(), "method": "tools/list",
            "params": {"cursor": "", "withUserTools": True},
        }, client_id)
        self.log(f"已向 {client_id or self.selected!r} 请求 initialize + tools/list "
                 f"(投递 {sent} 份)")
        return sent

    def call_tool(self, name: str, arguments, client_id=None) -> int:
        """人在控制台选择的命令 -> type:"command" 消息 (等价于 AI 的 tools/call)。"""
        device = self._resolve(client_id)
        if device is None:
            return 0
        message = {"type": "command", "name": name}
        if arguments is not None:
            message["arguments"] = arguments
        sent = self._send_to_device(device, message)
        self.log(f"下发命令 {name} -> {device.client_id!r} ({device.kind}) "
                 f"args={json.dumps(arguments, ensure_ascii=False)} (投递 {sent} 份)")
        return sent

    def send_raw(self, message: dict, client_id=None) -> int:
        """直接发一条协议消息; 没有 type 的当成 MCP payload 自动包一层。"""
        device = self._resolve(client_id)
        if device is None:
            return 0
        if "type" not in message:
            message = {"session_id": "tsp", "type": "mcp", "payload": message}
        sent = self._send_to_device(device, message)
        self.log(f"下发原始消息 -> {device.client_id!r} ({device.kind}) (投递 {sent} 份): "
                 f"{_short(json.dumps(message, ensure_ascii=False))}")
        return sent

    def expect_ota_device(self, client_id: str, topic: str) -> None:
        """OTA 提前告知一条设备连接 (设备接下来会用这个 client_id 连上来)。"""
        device = self.devices.get(client_id)
        if device is None:
            device = Device(client_id, topic)
            self.devices[client_id] = device
            if self.selected is None:
                self.selected = client_id
        self.broker.expect_device(client_id, topic)

    def select(self, client_id) -> bool:
        if client_id not in self.devices:
            return False
        self.selected = client_id
        self.log(f"控制台选中 {client_id!r}")
        return True

    def state(self) -> dict:
        selected = self.devices.get(self.selected)
        return {
            "selected": self.selected,
            "devices": [d.summary() for d in self.devices.values()],
            "tools": list(selected.tools.values()) if selected else [],
            "forced_deliveries": self.broker.forced_deliveries,
            "log": self.log.tail(),
        }


# ---------------------------------------------------------------- OTA / 控制台


def _split_host(host_header: str, default: str) -> str:
    if not host_header:
        return default
    parsed = urlparse(f"//{host_header}")
    return parsed.hostname or default


def _sanitize_topic_part(text: str) -> str:
    cleaned = "".join(ch if (ch.isalnum() or ch in "-_.") else "-" for ch in text)
    return cleaned.strip("-") or "unknown"


class TspHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    app = None            # 由 create_server 注入
    public_host = ""
    mqtt_port = 1883
    ota_path = DEFAULT_OTA_PATH
    index_html = b""

    # -------------------------------------------------- 工具

    def log_message(self, fmt, *args):  # 静音 BaseHTTPRequestHandler 的默认输出
        pass

    def _send(self, status: int, body: bytes, content_type: str) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def _send_json(self, status: int, payload) -> None:
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self._send(status, body, "application/json; charset=utf-8")

    def _read_body(self) -> bytes:
        try:
            length = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            length = 0
        return self.rfile.read(length) if length > 0 else b""

    # -------------------------------------------------- 路由

    def do_GET(self):
        path = urlparse(self.path).path
        if path in (self.ota_path, self.ota_path.rstrip("/"), "/ota", "/ota/"):
            return self._ota()
        if path in ("/", "/index.html"):
            return self._send(200, self.index_html, "text/html; charset=utf-8")
        if path == "/api/state":
            return self._send_json(200, self.app.state())
        if path == "/api/tools":
            device = self.app.devices.get(self.app.selected)
            return self._send_json(200, list(device.tools.values()) if device else [])
        if path == "/healthz":
            return self._send_json(200, {"ok": True})
        return self._send(404, b"not found\n", "text/plain; charset=utf-8")

    def do_POST(self):
        path = urlparse(self.path).path
        if path in (self.ota_path, self.ota_path.rstrip("/"), "/ota", "/ota/"):
            return self._ota()

        body = self._read_body()
        try:
            payload = json.loads(body.decode("utf-8")) if body else {}
        except ValueError:
            return self._send_json(400, {"ok": False, "error": "invalid json body"})
        if not isinstance(payload, dict):
            return self._send_json(400, {"ok": False, "error": "body must be a json object"})

        if path == "/api/select":
            client_id = payload.get("client_id")
            if not self.app.select(client_id):
                return self._send_json(404, {"ok": False,
                                             "error": f"unknown device {client_id!r}"})
            return self._send_json(200, {"ok": True, "selected": client_id})
        if path == "/api/probe":
            return self._send_json(200, {"ok": True,
                                         "delivered": self.app.probe(payload.get("client_id"))})
        if path == "/api/call":
            name = payload.get("name")
            if not isinstance(name, str) or not name:
                return self._send_json(400, {"ok": False, "error": "missing name"})
            delivered = self.app.call_tool(name, payload.get("arguments"),
                                           payload.get("client_id"))
            return self._send_json(200, {"ok": True, "delivered": delivered})
        if path == "/api/send":
            message = dict(payload)
            client_id = message.pop("client_id", None)
            return self._send_json(200, {"ok": True,
                                         "delivered": self.app.send_raw(message, client_id)})

        return self._send_json(404, {"ok": False, "error": "not found"})

    # -------------------------------------------------- OTA

    def _ota(self):
        self._read_body()  # 设备是 POST + JSON body; 不读干净会让 keep-alive 串包
        host = self.public_host or _split_host(self.headers.get("Host", ""), "127.0.0.1")
        client_id = self.headers.get("Client-Id") or self.headers.get("Device-Id") or "unknown"
        client_id = _sanitize_topic_part(client_id)
        topic = TspApp.topic_for(client_id)
        # 先把它登记成一条设备连接: 设备接下来用这个 client_id 连 broker 时主题要对得上
        self.app.expect_ota_device(client_id, topic)
        self.app.log(
            f"OTA 请求 Host={self.headers.get('Host')!r} Device-Id="
            f"{self.headers.get('Device-Id')!r} Client-Id={client_id!r} "
            f"-> mqtt {host}:{self.mqtt_port} topic={topic}"
        )
        response = {
            "server_time": {
                "timestamp": int(time.time() * 1000),
                "timezone_offset": 480,
            },
            "mqtt": {
                "endpoint": f"{host}:{self.mqtt_port}",
                "client_id": client_id,
                "username": self.app.mqtt_user or "",
                "password": self.app.mqtt_password or "",
                "keepalive": 240,
                "publish_topic": topic,
            },
        }
        self._send_json(200, response)


INDEX_HTML = """<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8">
<title>XiaoZhi TBOX 测试 TSP</title>
<style>
 body{font-family:system-ui,Segoe UI,sans-serif;margin:16px;background:#111;color:#ddd}
 h1{font-size:18px} h2{font-size:15px;margin-top:22px;border-bottom:1px solid #333;padding-bottom:4px}
 .muted{color:#888} code{background:#1e1e1e;padding:1px 4px;border-radius:3px}
 table{border-collapse:collapse;width:100%} td,th{border:1px solid #333;padding:4px 6px;
 font-size:13px;vertical-align:top;text-align:left}
 button{background:#2d6cdf;color:#fff;border:0;border-radius:4px;padding:4px 10px;cursor:pointer}
 button.gray{background:#444}
 textarea{width:100%;height:70px;background:#1b1b1b;color:#eee;border:1px solid #333;border-radius:4px;
 font-family:Consolas,monospace;font-size:13px;padding:6px}
 pre{background:#181818;border:1px solid #333;border-radius:4px;padding:8px;height:260px;overflow:auto;
 font-size:12px;line-height:1.45}
</style></head><body>
<h1>XiaoZhi TBOX 测试 TSP</h1>
<div id="status" class="muted">加载中…</div>
<p class="muted">
  这里会同时看到两条连接, 分属两个服务器通道:
  <b>AI 通道</b>（kind=mqtt, client_id = 设备 UUID, 固件从不 Subscribe, 靠 broker 直投）
  与 <b>TBOX 链路</b>（kind=tcp, 固件 main/tbox_link.cc 主动连上来的点对点 TCP, 行分隔 JSON）。
  选哪条, 命令就发到哪条。工具表取自当前选中设备的 tools/list。
</p>
<h2>设备连接 <span id="devCount" class="muted"></span></h2>
<div id="devices" class="muted">还没有设备连上</div>
<button class="gray" onclick="post('api/probe',{}).then(refresh)">向选中设备重新探测 tools/list</button>
<h2>工具 <span id="toolCount" class="muted"></span></h2>
<table id="tools"><thead><tr><th>工具名</th><th>描述 / 参数</th><th></th></tr></thead><tbody></tbody></table>
<h2>下发命令 (type: command)</h2>
<textarea id="cmd">{"type":"command","name":"self.hui.vehicle.request_wake","arguments":{}}</textarea>
<button onclick="sendCmd()">下发命令</button>
<button class="gray" onclick="refresh()">仅刷新</button>
<h2>日志</h2><pre id="log"></pre>
<script>
let TOOLS = {};
function esc(t){ return String(t).replace(/[&<>"]/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c])); }
function currentDevice(){
  const el = document.querySelector('input[name=dev]:checked');
  return el ? el.value : null;
}
function defaultsFor(tool){
  const props = (tool.inputSchema && tool.inputSchema.properties) || {};
  const out = {};
  for (const [k,v] of Object.entries(props)){
    if (v.type === 'boolean') out[k] = false;
    else if (v.type === 'integer' || v.type === 'number') out[k] = 0;
    else out[k] = '';
  }
  return out;
}
function fill(name){
  const tool = TOOLS[name] || {inputSchema:{}};
  document.getElementById('cmd').value = JSON.stringify(
    {type:'command', client_id: currentDevice(), name:name, arguments:defaultsFor(tool)}, null, 1);
}
function renderDevices(devices, selected){
  const box = document.getElementById('devices');
  box.innerHTML = '';
  document.getElementById('devCount').textContent = '(' + devices.length + ')';
  if (!devices.length){ box.innerHTML = '<span class="muted">还没有设备连上</span>'; return; }
  for (const d of devices){
    const row = document.createElement('div');
    row.style.margin = '3px 0';
    const radio = document.createElement('input');
    radio.type = 'radio'; radio.name = 'dev'; radio.value = d.client_id;
    radio.checked = (d.client_id === selected);
    radio.addEventListener('change', () => {
      post('api/select', {client_id:d.client_id}).then(refresh);
    });
    const label = document.createElement('label');
    label.appendChild(radio);
    const text = document.createElement('span');
    text.innerHTML = ' <code>' + esc(d.client_id) + '</code> &nbsp;[' + esc(d.kind) + '] '
      + 'channel=' + esc(d.topic) + ' &nbsp;工具=' + d.tool_count + ' &nbsp;回执=' + d.replies
      + ' &nbsp;' + (d.kind === 'tcp' ? '点对点链路'
                    : (d.subscribed ? '已订阅' : '未订阅(直投)'));
    label.appendChild(text);
    row.appendChild(label);
    box.appendChild(row);
  }
}
function renderTools(tools){
  const tbody = document.querySelector('#tools tbody');
  tbody.innerHTML = '';
  TOOLS = {};
  document.getElementById('toolCount').textContent = '(' + tools.length + ')';
  for (const tool of tools){
    TOOLS[tool.name] = tool;
    const props = (tool.inputSchema && tool.inputSchema.properties) || {};
    const args = Object.entries(props).map(([k,v]) => k + ':' + v.type).join(', ');
    const tr = document.createElement('tr');
    const td1 = document.createElement('td');
    td1.innerHTML = '<code>' + esc(tool.name) + '</code>';
    const td2 = document.createElement('td');
    td2.innerHTML = esc((tool.description || '').slice(0, 140))
      + (args ? '<br><span class="muted">参数 ' + esc(args) + '</span>' : '');
    const td3 = document.createElement('td');
    const btn = document.createElement('button');
    btn.textContent = '填入';
    btn.addEventListener('click', () => fill(tool.name));
    td3.appendChild(btn);
    tr.append(td1, td2, td3);
    tbody.appendChild(tr);
  }
}
async function post(path, body){
  return await (await fetch(path, {method:'POST', headers:{'Content-Type':'application/json'},
    body: JSON.stringify(body)})).json();
}
async function sendCmd(){
  let msg;
  try { msg = JSON.parse(document.getElementById('cmd').value); }
  catch(e){ alert('JSON 语法错误: ' + e.message); return; }
  if (msg.client_id === undefined) msg.client_id = currentDevice();
  let result;
  if (msg.type === 'command' && msg.name) result = await post('api/call', msg);
  else result = await post('api/send', msg);
  if (!result.ok) alert('发送失败: ' + result.error);
  refresh();
}
async function refresh(){
  const s = await (await fetch('api/state')).json();
  renderDevices(s.devices || [], s.selected);
  document.getElementById('status').innerHTML =
    '选中 = <code>' + esc(s.selected || '(无)') + '</code> &nbsp; 强制直投次数 = '
    + s.forced_deliveries;
  renderTools(s.tools || []);
  const log = document.getElementById('log');
  const atBottom = log.scrollTop + log.clientHeight >= log.scrollHeight - 20;
  log.textContent = (s.log || []).join('\\n');
  if (atBottom) log.scrollTop = log.scrollHeight;
}
setInterval(refresh, 1500); refresh();
</script></body></html>
"""


def create_server(bind_host: str, http_port: int, app: TspApp, public_host: str,
                  mqtt_port: int) -> ThreadingHTTPServer:
    handler = type("BoundTspHandler", (TspHandler,), {
        "app": app,
        "public_host": public_host,
        "mqtt_port": mqtt_port,
        "index_html": INDEX_HTML.encode("utf-8"),
    })
    return ThreadingHTTPServer((bind_host, http_port), handler)


# ---------------------------------------------------------------- main


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="测试用 TSP: OTA 端点 + MQTT broker + 网页控制台 (人在服务器端选命令执行)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "设备侧:\n"
            "  1. 打开 CONFIG_TBOX_COMMAND_MESSAGE\n"
            "  2. CONFIG_OTA_URL=\"http://<本机IP>:<http-port>/xiaozhi/ota/\"\n"
            "然后设备上电, 打开 http://<本机IP>:<http-port>/ 就能点命令了。\n"
        ),
    )
    parser.add_argument("--host", default="0.0.0.0", help="监听地址 (默认 0.0.0.0)")
    parser.add_argument("--http-port", type=int, default=8000, help="OTA/控制台端口 (默认 8000)")
    parser.add_argument("--mqtt-port", type=int, default=1883,
                        help="MQTT 端口 (默认 1883; 注意 8883 会让设备走 SSL, 别用)")
    parser.add_argument("--tcp-port", type=int, default=9000,
                        help="TBOX 链路 TCP/NDJSON 端口 (默认 9000)")
    parser.add_argument("--public-host", default="",
                        help="下发给设备的 broker 主机名; 留空则用 OTA 请求的 Host 头推导")
    parser.add_argument("--publish-topic", default="",
                        help="强制所有设备共用这个主题 (仅在只有一个设备时用); "
                             "留空则按 Client-Id 推导 tsp/dev/<client_id>")
    parser.add_argument("--token", default="tsp",
                        help="TBOX 链路 hello 必须带的 token (默认 tsp; 空则不校验)。"
                             "固件侧对应 CONFIG_TBOX_LINK_TOKEN 或 NVS \"tbox\"/token")
    parser.add_argument("--mqtt-user", default="tsp", help="MQTT 用户名 (默认 tsp)")
    parser.add_argument("--mqtt-password", default="tsp", help="MQTT 密码 (默认 tsp)")
    parser.add_argument("--allow-anonymous", action="store_true",
                        help="❗关掉 MQTT 鉴权 (只在本机调试用): 公网监听时互联网扫描器会连上来")
    parser.add_argument("--quiet", action="store_true", help="不打屏, 只看网页")
    return parser


def main() -> int:
    args = build_arg_parser().parse_args()

    log = LogRing(echo=not args.quiet)
    mqtt_user = "" if args.allow_anonymous else args.mqtt_user
    mqtt_password = "" if args.allow_anonymous else args.mqtt_password
    if args.host not in ("127.0.0.1", "localhost") and not mqtt_user:
        log("⚠️  监听 0.0.0.0 但没有 MQTT 凭据: 公网扫描器会连上来并能看到/伪造下行。")
    app = TspApp(log, args.publish_topic, token=args.token,
                 mqtt_user=mqtt_user, mqtt_password=mqtt_password)
    app.broker.start(args.host, args.mqtt_port)
    app.tcp.start(args.host, args.tcp_port)
    log(f"MQTT 鉴权: " + (f"已启用 (user={mqtt_user!r})" if mqtt_user else "未启用 (允许匿名!)"))
    log(f"TBOX 链路 token 校验: " + ("已启用" if args.token else "未启用"))

    server = create_server(args.host, args.http_port, app, args.public_host, args.mqtt_port)
    log(f"HTTP 控制台/OTA: http://{args.host}:{server.server_address[1]}/")
    log(f"设备请把 OTA 指到: http://<本机IP>:{server.server_address[1]}{DEFAULT_OTA_PATH}")
    log("Ctrl+C 退出")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        log("退出")
    finally:
        server.shutdown()
        server.server_close()
        app.broker.stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
