"""测试用 TSP (scripts/tsp_server.py) 的自测。

不依赖任何第三方库: 用裸 socket 扮一个 "设备", 走真实的 MQTT 报文和 HTTP 请求。
组网方式与真机一致 —— 设备先问 OTA 拿 mqtt 配置, 再连 broker, 然后 TSP 探测 tools/list,
人在网页上选一条命令, 设备收到 type:"command"。
"""

import importlib.util
import json
import socket
import threading
import unittest
import urllib.error
import urllib.request
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("tsp_server", ROOT / "scripts/tsp_server.py")
tsp = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(tsp)


CLIENT_ID = "aabbccdd-1122-3344-5566-778899aabbcc"
TOPIC = f"tsp/dev/{CLIENT_ID}"
TOOLS = [
    {
        "name": "self.hui.vehicle.request_wake",
        "description": "唤醒车辆主机",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "self.hui.host1.set_headlight",
        "description": "打开/关闭大灯",
        "inputSchema": {"type": "object", "properties": {"on": {"type": "integer"}}},
    },
]


def _connect_body(client_id, username=None, password=None) -> bytes:
    """按 MQTT 3.1.1 拼 CONNECT 可变头+载荷 (bit7=username, bit6=password)。"""
    flags = 0x02   # clean session
    payload = tsp.encode_utf8(client_id)
    if username is not None:
        flags |= 0x80
        payload += tsp.encode_utf8(username)
    if password is not None:
        flags |= 0x40
        payload += tsp.encode_utf8(password)
    return (tsp.encode_utf8("MQTT") + bytes([4, flags])
            + (60).to_bytes(2, "big") + payload)


class FakeDevice:
    """裸 socket 版的 "设备": 只会 CONNECT / PUBLISH, 以及收 PUBLISH。"""

    def __init__(self, mqtt_port: int, client_id: str = CLIENT_ID):
        self.sock = socket.create_connection(("127.0.0.1", mqtt_port), timeout=5)
        self.sock.settimeout(5)
        self.client_id = client_id
        body = (
            tsp.encode_utf8("MQTT")
            + bytes([4, 0x02])
            + (60).to_bytes(2, "big")
            + tsp.encode_utf8(client_id)
        )
        self.sock.sendall(tsp.encode_packet(tsp.CONNECT, 0, body))
        self._read_until(tsp.CONNACK)

    def _read(self):
        packet = tsp.read_packet(self.sock)
        if packet is None:
            raise AssertionError("连接被对端关闭")
        return packet

    def _read_until(self, wanted_type):
        while True:
            packet_type, flags, body = self._read()
            if packet_type == wanted_type:
                return packet_type, flags, body

    def subscribe(self, topic_filter: str) -> None:
        body = (1).to_bytes(2, "big") + tsp.encode_utf8(topic_filter) + bytes([0])
        self.sock.sendall(tsp.encode_packet(tsp.SUBSCRIBE, 2, body))
        self._read_until(tsp.SUBACK)

    def publish(self, topic: str, payload) -> None:
        if not isinstance(payload, (str, bytes)):
            payload = json.dumps(payload, ensure_ascii=False)
        data = payload.encode("utf-8") if isinstance(payload, str) else payload
        body = tsp.encode_utf8(topic) + data
        self.sock.sendall(tsp.encode_packet(tsp.PUBLISH, 0, body))

    def next_publish(self):
        """读到下一条 PUBLISH (topic, json/dict 或原始字符串)。"""
        while True:
            packet_type, flags, body = self._read()
            if packet_type != tsp.PUBLISH:
                continue
            topic, _, payload = tsp.parse_publish(flags, body)
            text = payload.decode("utf-8", "replace")
            try:
                return topic, json.loads(text)
            except ValueError:
                return topic, text

    def close(self) -> None:
        try:
            self.sock.sendall(tsp.encode_packet(tsp.DISCONNECT, 0, b""))
            self.sock.close()
        except OSError:
            pass


class FakeTcpDevice:
    """裸 socket 扮 TBOX 链路 (行分隔 JSON, 对应固件 main/tbox_link.cc)。"""

    def __init__(self, port: int, client_id: str = "tbox-test", token: str = None):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.sock.settimeout(5)
        self.buffer = ""
        hello = {"type": "hello", "device_id": "aa:bb:cc:dd:ee:ff",
                 "client_id": client_id, "channel": f"tsp/dev/{client_id}",
                 "version": 1}
        if token is not None:
            hello["token"] = token
        self.send(hello)

    def send(self, message) -> None:
        self.sock.sendall((json.dumps(message, ensure_ascii=False) + "\n").encode("utf-8"))

    def next_message(self):
        while "\n" not in self.buffer:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise AssertionError("TBOX 链路被对端关闭")
            self.buffer += chunk.decode("utf-8", "replace")
        line, self.buffer = self.buffer.split("\n", 1)
        return json.loads(line)

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            pass


class TspFixture:
    """起一套 TSP (HTTP + MQTT broker + TCP 链路), 退出时收干净。"""

    def __init__(self):
        self.log = tsp.LogRing(echo=False)
        self.app = tsp.TspApp(self.log)
        self.app.broker.start("127.0.0.1", 0)
        self.mqtt_port = self.app.broker.port
        self.app.tcp.start("127.0.0.1", 0)
        self.tcp_port = self.app.tcp.port
        self.server = tsp.create_server("127.0.0.1", 0, self.app, "", self.mqtt_port)
        self.http_port = self.server.server_address[1]
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def url(self, path: str) -> str:
        return f"http://127.0.0.1:{self.http_port}{path}"

    def request(self, path: str, body=None, headers=None):
        data = None if body is None else json.dumps(body).encode("utf-8")
        request = urllib.request.Request(self.url(path), data=data)
        request.add_header("Content-Type", "application/json")
        for key, value in (headers or {}).items():
            request.add_header(key, value)
        with urllib.request.urlopen(request, timeout=5) as response:
            return response.status, json.loads(response.read().decode("utf-8"))

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.app.broker.stop()
        self.app.tcp.stop()


class HelperTests(unittest.TestCase):
    def test_topic_matches(self):
        self.assertTrue(tsp.topic_matches("tsp/dev/abc", "tsp/dev/abc"))
        self.assertTrue(tsp.topic_matches("tsp/+/abc", "tsp/dev/abc"))
        self.assertTrue(tsp.topic_matches("tsp/#", "tsp/dev/abc"))
        self.assertFalse(tsp.topic_matches("tsp/dev/abc", "tsp/dev/xyz"))
        self.assertFalse(tsp.topic_matches("tsp/dev", "tsp/dev/abc"))

    def test_remaining_length_round_trip(self):
        def decode(encoded: bytes):
            value = 0
            multiplier = 1
            pos = 0
            while True:
                digit = encoded[pos]
                pos += 1
                value += (digit & 0x7F) * multiplier
                if not digit & 0x80:
                    return value, pos
                multiplier *= 128

        for length in (0, 1, 127, 128, 16383, 16384, 2097151, 2097152, 268435455):
            encoded = tsp.encode_remaining_length(length)
            self.assertEqual(decode(encoded), (length, len(encoded)))

    def test_read_packet_over_socket(self):
        server, client = socket.socketpair()
        try:
            payload = tsp.encode_utf8("a/b") + b"{}"
            client.sendall(tsp.encode_packet(tsp.PUBLISH, 0, payload))
            packet_type, flags, body = tsp.read_packet(server)
            self.assertEqual((packet_type, flags), (tsp.PUBLISH, 0))
            topic, packet_id, data = tsp.parse_publish(flags, body)
            self.assertEqual((topic, packet_id, data), ("a/b", None, b"{}"))
        finally:
            server.close()
            client.close()

    def test_topic_part_sanitizing(self):
        self.assertEqual(tsp._sanitize_topic_part("aabb-cc.dd_ee"), "aabb-cc.dd_ee")
        self.assertEqual(tsp._sanitize_topic_part("a/b c"), "a-b-c")
        self.assertEqual(tsp._sanitize_topic_part("///"), "unknown")


class OtaTests(unittest.TestCase):
    def setUp(self):
        self.fixture = TspFixture()
        self.addCleanup(self.fixture.close)

    def test_ota_points_device_at_this_broker(self):
        status, body = self.fixture.request(
            "/xiaozhi/ota/",
            body={"mac": "aa:bb:cc:dd:ee:ff"},
            headers={"Host": f"203.0.113.9:{self.fixture.http_port}", "Client-Id": CLIENT_ID},
        )
        self.assertEqual(status, 200)
        mqtt = body["mqtt"]
        # broker 地址从 Host 头推导, 端口用 --mqtt-port
        self.assertEqual(mqtt["endpoint"], f"203.0.113.9:{self.fixture.mqtt_port}")
        self.assertEqual(mqtt["client_id"], CLIENT_ID)
        self.assertEqual(mqtt["publish_topic"], TOPIC)
        self.assertNotIn("firmware", body)  # 不触发 OTA 升级
        self.assertNotIn("activation", body)  # 不触发激活流程
        # 毫秒时间戳
        self.assertGreater(body["server_time"]["timestamp"], 1_600_000_000_000)


class EndToEndTests(unittest.TestCase):
    def setUp(self):
        self.fixture = TspFixture()
        self.addCleanup(self.fixture.close)
        self.device = FakeDevice(self.fixture.mqtt_port)
        self.addCleanup(self.device.close)

    def _reply_tools_list(self, request_id=2):
        self.device.publish(TOPIC, {
            "session_id": "x",
            "type": "mcp",
            "payload": {"jsonrpc": "2.0", "id": request_id,
                        "result": {"tools": TOOLS}},
        })

    def test_probe_reaches_device_even_without_subscribe(self):
        # 设备固件从不 Subscribe(): TSP 靠 "指定设备连接" 直投, 所以第一次探测也能到
        topic, message = self.device.next_publish()
        self.assertEqual(topic, TOPIC)
        self.assertEqual(message["type"], "mcp")
        self.assertEqual(message["payload"]["method"], "initialize")

        topic, message = self.device.next_publish()
        self.assertEqual(message["payload"]["method"], "tools/list")
        self.assertTrue(message["payload"]["params"]["withUserTools"])

    def test_subscribe_still_works_and_is_recorded(self):
        self.device.subscribe(TOPIC)
        topic, message = self.device.next_publish()
        self.assertEqual(topic, TOPIC)
        self.assertTrue(self.fixture.app.broker.device_subscribed)
        for _ in range(50):
            if self.fixture.app.devices.get(CLIENT_ID) and \
                    self.fixture.app.devices[CLIENT_ID].subscribed:
                break
            threading.Event().wait(0.05)
        self.assertTrue(self.fixture.app.devices[CLIENT_ID].subscribed)

    def test_tools_list_result_populates_console(self):
        self._reply_tools_list()
        for _ in range(50):
            if CLIENT_ID in self.fixture.app.devices:
                break
            threading.Event().wait(0.05)
        device = self.fixture.app.devices[CLIENT_ID]
        for _ in range(50):
            if device.tools:
                break
            threading.Event().wait(0.05)
        self.assertEqual(sorted(device.tools), sorted(t["name"] for t in TOOLS))

        status, state = self.fixture.request("/api/state")
        self.assertEqual(status, 200)
        self.assertEqual(len(state["tools"]), 2)
        self.assertEqual(state["selected"], CLIENT_ID)
        self.assertEqual(len(state["devices"]), 1)
        self.assertEqual(state["devices"][0]["topic"], TOPIC)

    def test_call_endpoint_sends_command_message(self):
        self._reply_tools_list()
        status, body = self.fixture.request(
            "/api/call",
            body={"name": "self.hui.host1.set_headlight", "arguments": {"on": 1}},
        )
        self.assertEqual(status, 200)
        self.assertEqual(body["delivered"], 1)

        # 设备先收到 initialize / tools/list (自动探测), 之后才是这条命令
        while True:
            topic, message = self.device.next_publish()
            if message.get("type") == "command":
                break
        self.assertEqual(topic, TOPIC)
        self.assertEqual(message["name"], "self.hui.host1.set_headlight")
        self.assertEqual(message["arguments"], {"on": 1})

    def test_send_endpoint_wraps_bare_mcp_payload(self):
        status, body = self.fixture.request(
            "/api/send",
            body={"jsonrpc": "2.0", "id": 99, "method": "tools/list", "params": {}},
        )
        self.assertEqual(status, 200)
        self.assertEqual(body["delivered"], 1)

        while True:
            _, message = self.device.next_publish()
            if message.get("type") == "mcp" and message["payload"].get("id") == 99:
                break
        self.assertEqual(message["payload"]["method"], "tools/list")

    def test_unknown_route_returns_404(self):
        with self.assertRaises(urllib.error.HTTPError) as caught:
            self.fixture.request("/api/nope")
        self.assertEqual(caught.exception.code, 404)

    # ---------------------------------------------- 两条通道 (AI + TBOX) 必须分得清

    def test_two_devices_are_tracked_separately(self):
        """TBOX + AIBOX 二合一时会有两条连接: AI 通道 (设备 UUID, 不订阅) 与
        TBOX 链路 (配置的 client_id, 会订阅), 主题必须分开且各归各。"""
        second = FakeDevice(self.fixture.mqtt_port, client_id="tbox-test")
        self.addCleanup(second.close)
        second.subscribe("tsp/dev/tbox-test")

        for _ in range(60):
            if len(self.fixture.app.devices) == 2:
                break
            threading.Event().wait(0.05)

        status, state = self.fixture.request("/api/state")
        self.assertEqual(status, 200)
        self.assertEqual(sorted(d["client_id"] for d in state["devices"]),
                         sorted([CLIENT_ID, "tbox-test"]))
        topics = {d["client_id"]: d["topic"] for d in state["devices"]}
        self.assertEqual(topics["tbox-test"], "tsp/dev/tbox-test")
        self.assertEqual(topics[CLIENT_ID], TOPIC)

    def test_command_goes_only_to_the_selected_device(self):
        second = FakeDevice(self.fixture.mqtt_port, client_id="tbox-test")
        self.addCleanup(second.close)

        status, body = self.fixture.request("/api/select", body={"client_id": "tbox-test"})
        self.assertEqual(status, 200)
        status, body = self.fixture.request("/api/call", body={
            "name": "self.hui.host1.set_headlight", "arguments": {"on": 1},
        })
        self.assertEqual(body["delivered"], 1)

        while True:
            topic, message = second.next_publish()
            if message.get("type") == "command":
                break
        self.assertEqual(topic, "tsp/dev/tbox-test")
        self.assertEqual(message["name"], "self.hui.host1.set_headlight")
        self.assertEqual(message["arguments"], {"on": 1})

    def test_select_unknown_device_returns_404(self):
        with self.assertRaises(urllib.error.HTTPError) as caught:
            self.fixture.request("/api/select", body={"client_id": "nope"})
        self.assertEqual(caught.exception.code, 404)


class TcpLinkTests(unittest.TestCase):
    """TBOX 独立链路: 一条 TCP + 行分隔 JSON (对应固件 main/tbox_link.cc)。"""

    def setUp(self):
        self.fixture = TspFixture()
        self.addCleanup(self.fixture.close)
        self.device = FakeTcpDevice(self.fixture.tcp_port)
        self.addCleanup(self.device.close)

    def test_hello_registers_device_and_probes_tools(self):
        first = self.device.next_message()
        self.assertEqual(first["type"], "mcp")
        self.assertEqual(first["payload"]["method"], "initialize")
        second = self.device.next_message()
        self.assertEqual(second["payload"]["method"], "tools/list")

        status, state = self.fixture.request("/api/state")
        self.assertEqual(status, 200)
        kinds = {d["client_id"]: d["kind"] for d in state["devices"]}
        self.assertEqual(kinds.get("tbox-test"), "tcp")

    def test_tools_reply_and_command_round_trip(self):
        self.device.next_message()   # initialize
        self.device.next_message()   # tools/list
        self.device.send({"type": "mcp",
                          "payload": {"jsonrpc": "2.0", "id": 2,
                                      "result": {"tools": TOOLS}}})
        for _ in range(60):
            device = self.fixture.app.devices.get("tbox-test")
            if device and device.tools:
                break
            threading.Event().wait(0.05)
        self.assertEqual(len(self.fixture.app.devices["tbox-test"].tools), 2)

        status, body = self.fixture.request(
            "/api/call",
            body={"name": "self.hui.host1.set_headlight", "arguments": {"on": 1}})
        self.assertEqual(status, 200)
        self.assertEqual(body["delivered"], 1)

        while True:
            message = self.device.next_message()
            if message.get("type") == "command":
                break
        self.assertEqual(message["name"], "self.hui.host1.set_headlight")
        self.assertEqual(message["arguments"], {"on": 1})

    def test_ping_is_ignored(self):
        self.device.next_message()
        self.device.next_message()
        self.device.send({"type": "ping"})
        status, state = self.fixture.request("/api/state")
        self.assertEqual(status, 200)
        self.assertEqual(len(state["devices"]), 1)


class SecurityTests(unittest.TestCase):
    """台架暴露在公网, 必须防住三类事: 未授权 CONNECT、未知 token、往设备主题注入命令。

    真机背景: 1883 无鉴权时会被 Nmap 的 mqtt-subscribe 脚本、iw-xxxx 扫描器连上订阅
    `#`; 更严重的是 broker 会把任何客户端发到设备主题的消息**直投给设备**, 而固件会把
    `{"type":"command",name,arguments}` 当服务器消息执行 —— 等于公网可远程执行车控工具。
    """

    def test_parse_connect_reads_credentials_with_spec_flags(self):
        info = tsp.parse_connect(_connect_body("dev1", "user1", "pass1"))
        self.assertEqual(info["client_id"], "dev1")
        self.assertEqual(info["username"], "user1")
        self.assertEqual(info["password"], "pass1")
        info = tsp.parse_connect(_connect_body("dev2"))
        self.assertEqual((info["username"], info["password"]), ("", ""))

    def test_mqtt_rejects_anonymous_and_accepts_credentials(self):
        log = tsp.LogRing(echo=False)
        broker = tsp.MqttBroker(log, username="tsp", password="tsp")
        broker.start("127.0.0.1", 0)
        self.addCleanup(broker.stop)

        scanner = socket.create_connection(("127.0.0.1", broker.port), timeout=5)
        scanner.settimeout(5)
        scanner.sendall(tsp.encode_packet(tsp.CONNECT, 0, _connect_body("nmapABCDEFGHIJKLMNOP")))
        packet_type, _, body = tsp.read_packet(scanner)
        self.assertEqual(packet_type, tsp.CONNACK)
        self.assertEqual(body[-1], 5)          # 5 = not authorized
        self.assertEqual(broker.clients(), [])  # 不被登记为设备
        scanner.close()

        device = socket.create_connection(("127.0.0.1", broker.port), timeout=5)
        device.settimeout(5)
        device.sendall(tsp.encode_packet(tsp.CONNECT, 0, _connect_body("dev1", "tsp", "tsp")))
        packet_type, _, body = tsp.read_packet(device)
        self.assertEqual((packet_type, body[-1]), (tsp.CONNACK, 0))
        self.assertEqual([c["id"] for c in broker.clients()], ["dev1"])
        device.close()

    def test_tcp_hello_requires_token(self):
        log = tsp.LogRing(echo=False)
        app = tsp.TspApp(log, token="tsp")
        app.tcp.start("127.0.0.1", 0)
        self.addCleanup(app.tcp.stop)

        bad = socket.create_connection(("127.0.0.1", app.tcp.port), timeout=5)
        bad.settimeout(5)
        bad.sendall(b'{"type":"hello","client_id":"tbox-test"}\n')
        self.assertEqual(bad.recv(64), b"")     # 对端直接关闭
        bad.close()
        self.assertEqual(app.devices, {})       # 不登记

        good = FakeTcpDevice(app.tcp.port, token="tsp")
        self.addCleanup(good.close)
        self.assertEqual(good.next_message()["type"], "mcp")   # 连接活着, 开始探测
        self.assertIn("tbox-test", app.devices)

    def test_client_publish_cannot_be_injected_into_device(self):
        log = tsp.LogRing(echo=False)
        broker = tsp.MqttBroker(log)
        broker.start("127.0.0.1", 0)
        self.addCleanup(broker.stop)

        device = FakeDevice(broker.port, CLIENT_ID)
        self.addCleanup(device.close)
        broker.expect_device(CLIENT_ID, TOPIC)   # 就是这个 client_id 的设备 (靠直投收下行)

        # TSP 自己发的下行照常直达设备 (直投机制没坏)
        broker.publish(TOPIC, {"type": "mcp", "payload": {"jsonrpc": "2.0", "id": 1}})
        topic, _ = device.next_publish()
        self.assertEqual(topic, TOPIC)

        # 别的客户端往设备主题发命令 -> 绝不能投给设备
        attacker = FakeDevice(broker.port, "attacker")
        self.addCleanup(attacker.close)
        attacker.publish(TOPIC, {"type": "command", "name": "self.reboot", "arguments": {}})
        device.sock.settimeout(1.0)
        with self.assertRaises(socket.timeout):
            device.sock.recv(64)

    def test_attacker_cannot_steal_device_link_by_subscribing(self):
        """光订阅 '#': 不会被登记成设备, 也拿不到探测消息 (控制台看得到的才是设备)。"""
        fixture = TspFixture()          # 这个 fixture 不设凭据, 便于直接连
        self.addCleanup(fixture.close)
        attacker = FakeDevice(fixture.mqtt_port, "attacker")
        self.addCleanup(attacker.close)
        attacker.subscribe("#")
        threading.Event().wait(0.3)
        device = fixture.app.devices.get("attacker")
        if device is not None:          # 只允许登记但绝不能被当成被直投的设备
            self.assertFalse(device.probed and device.tools)


if __name__ == "__main__":
    unittest.main()
