import contextlib
import importlib.util
import io
import json
import tempfile
import threading
import unittest
import urllib.error
import urllib.request
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "hui_manual_server", ROOT / "scripts/hui_manual_server.py"
)
hui = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(hui)


def write_manual(directory: Path, filename: str, payload: dict) -> Path:
    path = directory / filename
    path.write_text(json.dumps(payload, ensure_ascii=False), encoding="utf-8")
    return path


class LoadManualsTests(unittest.TestCase):
    def test_index_by_device_id_and_skip_missing_id(self):
        # ignore_cleanup_errors: Windows 上临时目录偶尔会被索引器/杀毒占住
        with tempfile.TemporaryDirectory(ignore_cleanup_errors=True) as tmp:
            directory = Path(tmp)
            write_manual(
                directory,
                "anything.json",  # 文件名不参与索引
                {
                    "device_id": "0001",
                    "name": "主机1",
                    "commands": [{"id": "ff01", "kind": "read", "name": "get_battery"}],
                },
            )
            write_manual(
                directory,
                "no_id.json",
                {"name": "没有设备 ID", "commands": [{"id": "1000", "kind": "read"}]},
            )
            write_manual(
                directory,
                "common.json",
                {"commands": [{"id": "1000", "kind": "read", "name": "get_device_id"}]},
            )

            index, common, notes = hui.load_manuals(directory)

            self.assertEqual(list(index), ["0001"])
            self.assertIsNotNone(common)
            self.assertTrue(any("no_id.json" in note for note in notes))

    def test_missing_directory_raises(self):
        with self.assertRaises(FileNotFoundError):
            hui.load_manuals(Path("definitely-not-here"))


class MergeCommonTests(unittest.TestCase):
    def test_common_is_prepended_and_deduped_by_kind_and_id(self):
        manual = {
            "device_id": "0001",
            "commands": [
                {"id": "1000", "kind": "read", "name": "host_own_device_id"},
                {"id": "ff01", "kind": "read", "name": "get_battery"},
            ],
        }
        common = {
            "commands": [
                {"id": "1000", "kind": "read", "name": "common_device_id"},
                {"id": "1001", "kind": "read", "name": "common_ping"},
                # 同一个 id 但 kind 不同 -> 必须保留 (read 用 sub_service, ioctl 用命令码)
                {"id": "ff01", "kind": "ioctl", "name": "ioctl_same_id"},
            ]
        }

        merged = hui.merge_common(manual, common)
        keys = [(command["kind"], command["id"]) for command in merged["commands"]]

        # 通用命令里主机没有的那些排在最前 (与固件"公共工具靠前"的约定一致)
        self.assertEqual(
            keys,
            [("read", "1001"), ("ioctl", "ff01"), ("read", "1000"), ("read", "ff01")],
        )
        # 主机自己的定义优先 (没被 common.json 里的同名命令覆盖)
        own = next(
            command
            for command in merged["commands"]
            if (command["kind"], command["id"]) == ("read", "1000")
        )
        self.assertEqual(own["name"], "host_own_device_id")

    def test_merge_without_common_is_identity(self):
        manual = {"device_id": "0001", "commands": [{"id": "ff01", "kind": "read"}]}
        self.assertEqual(hui.merge_common(manual, None)["commands"], manual["commands"])


class FindDeviceIdTests(unittest.TestCase):
    def setUp(self):
        self.index = {"0001": {"commands": []}, "0010": {"commands": []}}

    def test_exact(self):
        self.assertEqual(hui.find_device_id(self.index, "0001"), "0001")

    def test_lenient_forms(self):
        for raw in ("1", "01", "0x0001", "0X0001", " 0001 "):
            self.assertEqual(hui.find_device_id(self.index, raw), "0001", raw)

    def test_unknown_and_empty(self):
        self.assertIsNone(hui.find_device_id(self.index, "9999"))
        self.assertIsNone(hui.find_device_id(self.index, ""))


class ValidateManualTests(unittest.TestCase):
    def test_accepts_good_manual(self):
        problems = hui.validate_manual(
            {
                "device_id": "0001",
                "commands": [
                    {"id": "ff01", "kind": "read", "name": "get_battery", "value": {"type": "u8"}},
                    {
                        "id": "f101",
                        "kind": "ioctl",
                        "name": "set_headlight",
                        "args": [{"name": "on", "type": "u8"}],
                    },
                ],
            },
            "src",
        )
        self.assertEqual(problems, [])

    def test_flags_bad_id_kind_duplicate_and_arg_scale(self):
        problems = hui.validate_manual(
            {
                "commands": [
                    {"id": "zzzz", "kind": "read"},
                    {"id": "ff01", "kind": "poke"},
                    {"id": "ff02", "kind": "read", "name": "a"},
                    {"id": "ff02", "kind": "read", "name": "b"},
                    {
                        "id": "2001",
                        "kind": "write",
                        "name": "set_limit",
                        "args": [{"name": "limit", "type": "u16", "scale": 0.1}],
                    },
                    {"id": "3000", "kind": "read", "name": "bad_type", "value": {"type": "f64"}},
                ]
            },
            "src",
        )
        joined = "\n".join(problems)
        self.assertIn("id 缺失或不是", joined)
        self.assertIn("kind 非法", joined)
        self.assertIn("重复", joined)
        self.assertIn("scale/offset 不参与编码", joined)
        self.assertIn("未知类型", joined)

    def test_rejects_empty_commands(self):
        self.assertTrue(hui.validate_manual({"commands": []}, "src"))
        self.assertTrue(hui.validate_manual({}, "src"))


class ShippedManualsTests(unittest.TestCase):
    """仓库里发出去的手册必须干净 —— 固件会跳过坏命令, 但那等于功能静默缺失。"""

    def test_repo_manuals_are_valid(self):
        index, common, _ = hui.load_manuals(hui.DEFAULT_MANUALS_DIR)

        self.assertIn("0001", index)
        self.assertIn("0002", index)

        for device_id, manual in sorted(index.items()):
            merged = hui.merge_common(manual, common)
            problems = hui.validate_manual(merged, f"device_id={device_id}")
            self.assertEqual(problems, [], f"{device_id}: {problems}")

    def test_common_commands_reach_every_host(self):
        index, common, _ = hui.load_manuals(hui.DEFAULT_MANUALS_DIR)
        for device_id, manual in sorted(index.items()):
            merged = hui.merge_common(manual, common)
            keys = {(c["kind"], c["id"]) for c in merged["commands"]}
            self.assertIn(("read", "1000"), keys, device_id)

    def test_example_commands_from_the_requirement(self):
        """需求里点名的命令号必须都在。"""
        index, common, _ = hui.load_manuals(hui.DEFAULT_MANUALS_DIR)

        def keys_of(device_id):
            merged = hui.merge_common(index[device_id], common)
            return {(c["kind"], c["id"]) for c in merged["commands"]}

        self.assertTrue(
            {("read", "ff01"), ("read", "ff02"), ("ioctl", "f101")}.issubset(keys_of("0001"))
        )
        self.assertTrue(
            {
                ("read", "f110"),
                ("read", "f111"),
                ("ioctl", "f112"),
                ("ioctl", "f113"),
            }.issubset(keys_of("0002"))
        )


class HttpRouteTests(unittest.TestCase):
    """真的起一个服务, 按固件的请求方式取手册。"""

    @classmethod
    def setUpClass(cls):
        with contextlib.redirect_stdout(io.StringIO()):
            index, common, _ = hui.load_manuals(hui.DEFAULT_MANUALS_DIR)
            cls.server = hui.create_server(
                "127.0.0.1", 0, hui.DEFAULT_MANUALS_DIR, index, common
            )
        cls.server.hui_log = lambda *args: None  # 测试输出保持干净
        cls.port = cls.server.server_address[1]
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join(timeout=5)

    def get(self, path):
        url = f"http://127.0.0.1:{self.port}{path}"
        with urllib.request.urlopen(url, timeout=5) as response:
            return response.status, response.read().decode("utf-8")

    def get_error(self, path):
        url = f"http://127.0.0.1:{self.port}{path}"
        try:
            with urllib.request.urlopen(url, timeout=5) as response:
                return response.status, response.read().decode("utf-8")
        except urllib.error.HTTPError as error:
            return error.code, error.read().decode("utf-8")

    def test_manual_by_query_param(self):
        status, body = self.get("/api/hui/manual?device_id=0001")
        self.assertEqual(status, 200)
        manual = json.loads(body)
        self.assertEqual(manual["device_id"], "0001")
        self.assertTrue(any(c["id"] == "ff01" for c in manual["commands"]))

    def test_manual_by_path_with_extension(self):
        status, body = self.get("/api/hui/manual/0002.json")
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(body)["device_id"], "0002")

    def test_lenient_device_id(self):
        status, body = self.get("/api/hui/manual?device_id=1")
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(body)["device_id"], "0001")

    def test_unknown_device_id_is_404_with_hint(self):
        status, body = self.get_error("/api/hui/manual?device_id=9999")
        self.assertEqual(status, 404)
        payload = json.loads(body)
        self.assertIn("known", payload)
        self.assertIn("0001", payload["known"])

    def test_missing_device_id_is_400(self):
        status, _ = self.get_error("/api/hui/manual")
        self.assertEqual(status, 400)

    def test_healthz_and_index(self):
        status, body = self.get("/healthz")
        self.assertEqual(status, 200)
        self.assertEqual(body.strip(), "ok")

        status, body = self.get("/")
        self.assertEqual(status, 200)
        self.assertIn("HUI", body)
        self.assertIn("/api/hui/manual", body)

    def test_unknown_path_is_404(self):
        status, body = self.get_error("/nope")
        self.assertEqual(status, 404)
        self.assertIn("not found", body)


if __name__ == "__main__":
    unittest.main()
