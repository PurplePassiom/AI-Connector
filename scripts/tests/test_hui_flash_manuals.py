"""校验 main/soa/manuals/*.json 真的被打进了 assets 镜像, 且格式与固件侧读法一致。

背景: 固件 (main/soa/hui_manual.cc) 通过 Assets::GetAssetData(name) 从 assets 分区
读手册, 这个 API 能工作的前提是打包格式正确:

    12 字节头:  [file_count:u32][checksum:u32][len:u32]
    每个文件一条 44 字节表项:
        [name:32B (NUL 填充)][size:u32][offset:u32][width:u16][height:u16]
    数据区: 每个文件前置 2 字节魔数 0x5A5A, 然后才是内容;
            offset 是相对数据区开头的偏移, size 是**不含魔数**的文件长度。

格式定义见 scripts/build_default_assets.py 的 pack_assets_simple()。固件侧把绝对
偏移算成 12 + 44 * file_count + offset, 再跳过 2 字节魔数。

镜像不在 (没构建过) 就跳过 —— 干净检出不该因此失败。
"""

import importlib.util
import json
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MANUALS_DIR = ROOT / "main" / "soa" / "manuals"
ASSETS_BIN = ROOT / "build" / "generated_assets.bin"

HEADER_SIZE = 12
ENTRY_SIZE = 44
NAME_LEN = 32
MAGIC_SIZE = 2

# 复用服务端那份加载器, 目的是交叉验证"flash 里的手册 == 后端会下发的手册"
SERVER_SPEC = importlib.util.spec_from_file_location(
    "hui_manual_server", ROOT / "scripts/hui_manual_server.py"
)
server = importlib.util.module_from_spec(SERVER_SPEC)
assert SERVER_SPEC.loader is not None
SERVER_SPEC.loader.exec_module(server)


def read_asset_table(image: Path):
    """返回 (原始数据, {名字: (绝对偏移, 长度)}, 头部声明, 头部字节数)。"""
    data = image.read_bytes()
    file_count = int.from_bytes(data[0:4], "little")
    checksum = int.from_bytes(data[4:8], "little")
    length = int.from_bytes(data[8:12], "little")

    table = {}
    for index in range(file_count):
        base = HEADER_SIZE + index * ENTRY_SIZE
        name = data[base : base + NAME_LEN].split(b"\0", 1)[0].decode("utf-8", "replace")
        size = int.from_bytes(data[base + 32 : base + 36], "little")
        offset = int.from_bytes(data[base + 36 : base + 40], "little")
        # 固件侧的算法: 表之后才是数据区, 再加表内偏移
        absolute = HEADER_SIZE + ENTRY_SIZE * file_count + offset
        table[name] = (absolute, size)

    return data, table, {"file_count": file_count, "checksum": checksum, "length": length}, data[HEADER_SIZE : HEADER_SIZE + length]


def asset_payload(data: bytes, absolute_offset: int, size: int) -> bytes:
    """按固件的读法取出内容: 跳过 0x5A5A 魔数, 取 size 字节。"""
    magic = data[absolute_offset : absolute_offset + MAGIC_SIZE]
    assert magic == b"\x5a\x5a", f"魔数不对: {magic!r}"
    return data[absolute_offset + MAGIC_SIZE : absolute_offset + MAGIC_SIZE + size]


def resolve_flash_manual(data: bytes, table: dict, device_id: str):
    """模拟固件 hui_manual.cc 的查找算法 (FLASH 分支)。

    固件做的三件事:
      1. 读 index.json, 枚举它的 extra_files 作为候选文件名
      2. 再试一次约定名 <device_id>.json
      3. 逐个 GetAssetData → 解析 → **按手册里的 device_id 匹配** (不看文件名)

    @return (文件名, 手册 dict) 或 None
    """
    candidates: list[str] = []
    if "index.json" in table:
        offset, size = table["index.json"]
        index = json.loads(asset_payload(data, offset, size).decode("utf-8"))
        candidates.extend(index.get("extra_files") or [])
    candidates.append(f"{device_id}.json")

    wanted = device_id.strip().upper().removeprefix("0X").zfill(4)
    for name in candidates:
        if name not in table:
            continue
        offset, size = table[name]
        try:
            manual = json.loads(asset_payload(data, offset, size).decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            continue
        found = str(manual.get("device_id") or "").strip().upper().removeprefix("0X")
        if found and found.zfill(4) == wanted:
            return name, manual
    return None


@unittest.skipUnless(ASSETS_BIN.exists(), "还没构建过 (build/generated_assets.bin 不存在)")
class PackedManualTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.data, cls.table, cls.header, cls.body = read_asset_table(ASSETS_BIN)

    def test_header_is_consistent(self):
        self.assertGreater(self.header["file_count"], 0)
        # length = 数据区字节数, 数据区 = mmap 表 + 所有文件 (含每文件的 2 字节魔数)。
        # 所以文件总长就是 12 字节头 + length —— 表本身也算在 length 里, 别再另外加。
        self.assertEqual(self.header["length"], len(self.body))
        self.assertEqual(len(self.data), HEADER_SIZE + self.header["length"])
        # 打包脚本用的是 16 位累加和 (build_default_assets.py: compute_checksum)
        self.assertEqual(sum(self.body) & 0xFFFF, self.header["checksum"] & 0xFFFF)

    def test_table_offsets_are_consecutive_and_fill_the_body(self):
        """表项必须首尾相接, 且正好填满数据区 —— 这同时验证了表项步长和魔数约定。

        数据区 (length 字节) = mmap 表 (44*count) + 各文件 (2 字节魔数 + size)。
        固件算的绝对偏移是 12 + 44*count + offset, 与这里一致。
        """
        base = HEADER_SIZE + ENTRY_SIZE * self.header["file_count"]
        entries = sorted(self.table.values(), key=lambda item: item[0])
        covered = 0
        for absolute, size in entries:
            self.assertEqual(absolute, base + covered, "表项偏移不连续")
            covered += size + MAGIC_SIZE
        self.assertEqual(
            covered + ENTRY_SIZE * self.header["file_count"],
            self.header["length"],
            "数据区没被表项+文件填满",
        )

    def test_every_shipped_manual_is_packed(self):
        expected = sorted(path.name for path in MANUALS_DIR.glob("*.json"))
        self.assertTrue(expected, f"{MANUALS_DIR} 里没有 json")
        packed = sorted(name for name in self.table if name.endswith(".json"))
        for name in expected:
            self.assertIn(
                name,
                self.table,
                f"{name} 没进 assets 镜像 (检查 main/CMakeLists.txt 的 "
                "DEFAULT_ASSETS_EXTRA_FILES 是否被别的来源占了)",
            )
        self.assertTrue(set(expected).issubset(set(packed)))

    def test_asset_names_fit_the_fixed_width_field(self):
        # 超了会被 pack_assets_simple 截断 —— 名字一变, 固件就找不到了
        for name in self.table:
            self.assertLessEqual(len(name.encode("utf-8")), NAME_LEN, name)

    def test_payloads_are_the_manual_json(self):
        for name, (offset, size) in self.table.items():
            if not name.endswith(".json"):
                continue
            payload = asset_payload(self.data, offset, size)
            self.assertEqual(size, len(payload), name)
            if name == "index.json":
                continue
            try:
                manual = json.loads(payload.decode("utf-8"))
            except (UnicodeDecodeError, json.JSONDecodeError) as error:
                self.fail(f"{name} 的内容不是合法 JSON: {error}")
            self.assertIn("commands", manual, name)

    def test_flash_covers_every_device_the_server_would_serve(self):
        """flash 里的手册必须覆盖后端会下发的所有主机 (方案 A 的意义所在)。"""
        index, common, _ = server.load_manuals(MANUALS_DIR)

        flashed: dict[str, dict] = {}
        for name, (offset, size) in self.table.items():
            if not name.endswith(".json") or name == "index.json":
                continue
            manual = json.loads(asset_payload(self.data, offset, size).decode("utf-8"))
            device_id = str(manual.get("device_id") or "").upper()
            if device_id:
                flashed[device_id] = manual

        for device_id, manual in index.items():
            self.assertIn(
                device_id,
                flashed,
                f"device_id={device_id} ({manual.get('name')}) 只在服务端有, flash 里没有",
            )
            # 两边内容应当一致 (同一份 json 打包 + 下发)
            merged = server.merge_common(manual, common)
            self.assertEqual(
                len(flashed[device_id]["commands"]),
                len(merged["commands"]),
                f"device_id={device_id} 的命令条数不一致",
            )

    def test_index_json_lists_the_manuals(self):
        """固件按 index.json 的 extra_files 枚举文件名 —— 这个列表必须有内容。"""
        self.assertIn("index.json", self.table)
        offset, size = self.table["index.json"]
        index = json.loads(asset_payload(self.data, offset, size).decode("utf-8"))
        extra = index.get("extra_files") or []
        self.assertTrue(
            any(name.endswith(".json") for name in extra),
            f"index.json 的 extra_files 里没有手册 json: {extra}",
        )

    def test_firmware_lookup_finds_each_host(self):
        """真的跑一遍固件的查找算法: 给个 device_id, 应该能命中对应的手册。

        这条用真的镜像 + 真的算法回答"把 host1.json 放进 flash 后固件找得到吗"。
        期望值取自仓库里的 JSON (不写死条数): 手册增删命令后这条不用改, 但镜像没跟着
        重建时会立刻报出来 —— 这正是"改了 JSON 忘了重烧 assets 分区"的典型故障。
        """
        expected = {"0001": "host1.json", "0002": "host2.json"}
        for device_id, filename in expected.items():
            with self.subTest(device_id=device_id):
                hit = resolve_flash_manual(self.data, self.table, device_id)
                self.assertIsNotNone(hit, f"固件按 device_id={device_id} 一份手册都找不到")
                name, manual = hit
                self.assertEqual(name, filename)
                on_disk = json.loads((MANUALS_DIR / filename).read_text(encoding="utf-8"))
                self.assertEqual(
                    [(c.get("kind", "read"), c["id"]) for c in manual["commands"]],
                    [(c.get("kind", "read"), c["id"]) for c in on_disk["commands"]],
                    f"flash 里的 {filename} 与仓库里的 JSON 不一致, 需要重新构建 assets 镜像",
                )

    def test_firmware_lookup_is_lenient_about_device_id_spelling(self):
        for spelling in ("1", "0x0001", "0001", "  0001 "):
            with self.subTest(spelling=spelling):
                hit = resolve_flash_manual(self.data, self.table, spelling)
                self.assertIsNotNone(hit, spelling)
                self.assertEqual(hit[0], "host1.json")

    def test_firmware_lookup_skips_common_json(self):
        """common.json 也躺在 flash 里 (device_id 是空), 不能被当成主机手册。"""
        self.assertIn("common.json", self.table)
        offset, size = self.table["common.json"]
        common = json.loads(asset_payload(self.data, offset, size).decode("utf-8"))
        self.assertEqual(str(common.get("device_id") or ""), "")
        hit = resolve_flash_manual(self.data, self.table, "0001")
        self.assertIsNotNone(hit)
        self.assertNotEqual(hit[0], "common.json")

    def test_firmware_lookup_misses_unknown_host(self):
        """未知设备 ID 必须老老实实返回 None (固件那边就会去走 HTTP 或报错)。"""
        self.assertIsNone(resolve_flash_manual(self.data, self.table, "00FF"))


if __name__ == "__main__":
    unittest.main()
