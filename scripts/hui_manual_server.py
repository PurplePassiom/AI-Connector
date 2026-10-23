#!/usr/bin/env python3
"""本地 HUI 命令手册服务 (台架/联调用)。

固件侧的流程是:

    read 1000 (HUI 通用命令)  ->  2 字节大端设备 ID, 例如 "0001"
    GET <HUI_MANUAL_URL>      ->  该主机的命令手册 (JSON)

所以本脚本只要把 main/soa/manuals/ 下的手册按 device_id 发出去就够了,
固件那边不需要任何特殊照顾。

用法:

    python3 scripts/hui_manual_server.py              # 监听 0.0.0.0:8080
    python3 scripts/hui_manual_server.py --port 9000
    python3 scripts/hui_manual_server.py --check      # 只校验手册, 不启服务

启动后把打印出来的那行 CONFIG_HUI_MANUAL_URL 抄进板级 config.json 的
sdkconfig_append, 重新构建烧录即可。

约定:
  * 手册文件不用改名 —— 索引按 JSON 里的 "device_id" 字段建立。
  * common.json 里的通用命令会自动合并进每台主机的 commands (按 kind+id 去重,
    主机自己的定义优先)。
"""

from __future__ import annotations

import argparse
import json
import socket
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

DEFAULT_MANUALS_DIR = Path(__file__).resolve().parents[1] / "main" / "soa" / "manuals"
COMMON_FILENAME = "common.json"
KINDS = ("read", "write", "ioctl")


# --------------------------------------------------------------------------- #
# 手册加载 / 校验 (纯函数, 方便单测)
# --------------------------------------------------------------------------- #

def _is_hex_id(text: str) -> bool:
    if not isinstance(text, str) or not 1 <= len(text) <= 4:
        return False
    try:
        int(text, 16)
    except ValueError:
        return False
    return True


def validate_manual(manual: dict, source: str) -> list[str]:
    """按固件侧的解析规则校验一本手册, 返回问题列表 (空 = 没问题)。"""
    problems: list[str] = []

    if not isinstance(manual, dict):
        return [f"{source}: 顶层不是 JSON 对象"]

    commands = manual.get("commands")
    if not isinstance(commands, list) or not commands:
        return [f"{source}: 缺少非空的 commands 数组"]

    seen: set[tuple[str, int]] = set()
    for index, command in enumerate(commands):
        where = f"{source}: commands[{index}]"
        if not isinstance(command, dict):
            problems.append(f"{where}: 不是对象")
            continue
        if not _is_hex_id(command.get("id")):
            problems.append(f"{where}: id 缺失或不是 1~4 位十六进制")
            continue

        kind = command.get("kind", "read")
        if kind not in KINDS:
            problems.append(f"{where}: kind 非法 ({kind!r}, 只支持 read/write/ioctl)")
            continue

        key = (kind, int(command["id"], 16))
        if key in seen:
            problems.append(f"{where}: {kind} 0x{key[1]:04X} 重复")
        seen.add(key)

        if not command.get("name"):
            problems.append(f"{where}: 没有 name, 固件会用 cmd_{key[1]:04X} 兜底")

        for arg_index, arg in enumerate(command.get("args") or []):
            arg_where = f"{where}.args[{arg_index}]"
            if not isinstance(arg, dict):
                problems.append(f"{arg_where}: 不是对象")
                continue
            if not arg.get("name"):
                problems.append(f"{arg_where}: 没有 name, 固件会跳过整条命令")
            if arg.get("scale", 1) != 1 or arg.get("offset", 0) != 0:
                problems.append(
                    f"{arg_where}: 入参的 scale/offset 不参与编码, 固件会告警; "
                    "请把量化写进 unit (如 \"0.1 km/h\")"
                )

        value = command.get("value")
        if isinstance(value, dict) and value.get("type"):
            if value["type"] not in ("u8", "i8", "u16", "i16", "u32", "i32", "bool", "hex"):
                problems.append(f"{where}.value: 未知类型 {value['type']!r}")

    return problems


def load_manuals(manuals_dir: Path) -> tuple[dict[str, dict], dict | None, list[str]]:
    """加载目录下所有手册。

    @return (device_id -> 手册, 通用命令手册, 提示信息列表)
    """
    notes: list[str] = []
    index: dict[str, dict] = {}
    common: dict | None = None

    if not manuals_dir.is_dir():
        raise FileNotFoundError(f"手册目录不存在: {manuals_dir}")

    for path in sorted(manuals_dir.glob("*.json")):
        try:
            manual = json.loads(path.read_text(encoding="utf-8"))
        except json.JSONDecodeError as error:
            notes.append(f"{path.name}: JSON 解析失败 ({error})")
            continue

        if path.name == COMMON_FILENAME:
            common = manual
            notes.append(f"{path.name}: 通用命令 {len(manual.get('commands') or [])} 条")
            continue

        device_id = str(manual.get("device_id") or "").strip().upper()
        if not device_id:
            notes.append(f"{path.name}: 没有 device_id, 跳过 (无法按设备 ID 下发)")
            continue
        if device_id in index:
            notes.append(f"{path.name}: device_id {device_id} 与已有手册冲突, 覆盖它")
        index[device_id] = manual

    return index, common, notes


def merge_common(manual: dict, common: dict | None) -> dict:
    """把通用命令并进主机手册。按 (kind, id) 去重, 主机自己的定义优先。"""
    merged = dict(manual)
    commands = list(manual.get("commands") or [])

    if common:
        own = {(str(c.get("kind") or "read"), str(c.get("id", "")).upper()) for c in commands}
        prepended = []
        for command in common.get("commands") or []:
            key = (str(command.get("kind") or "read"), str(command.get("id", "")).upper())
            if key in own:
                continue  # 主机手册里已经写了这条通用命令
            own.add(key)
            prepended.append(command)
        commands = prepended + commands

    merged["commands"] = commands
    return merged


def find_device_id(index: dict[str, dict], raw: str) -> str | None:
    """宽松匹配设备 ID: 大小写、"1" → "0001"、带 0x 前缀都认。"""
    if not raw:
        return None
    key = raw.strip().upper()
    candidates = [key]
    if key.startswith("0X"):
        candidates.append(key[2:])
    for candidate in list(candidates):
        if candidate and len(candidate) < 4:
            candidates.append(candidate.zfill(4))
    for candidate in candidates:
        if candidate in index:
            return candidate
    return None


# --------------------------------------------------------------------------- #
# HTTP 服务
# --------------------------------------------------------------------------- #

def lan_ip() -> str:
    """猜本机在局域网里的地址 (不会真的发包)。"""
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            probe.connect(("8.8.8.8", 53))
            return probe.getsockname()[0]
    except OSError:
        return "127.0.0.1"


class ManualHandler(BaseHTTPRequestHandler):
    server_version = "HuiManualServer/1.0"

    # 由 create_server() 在类属性上注入
    manuals: dict[str, dict] = {}
    common: dict | None = None
    manuals_dir: Path = DEFAULT_MANUALS_DIR

    # ---------------------------- 工具方法 ---------------------------- #

    def _send(self, status: int, body: bytes, content_type: str, note: str = "") -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")  # 方便改手册后立刻生效
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)
        logger = getattr(self.server, "hui_log", None)
        if logger:
            logger(status, note)

    def _send_json(self, status: int, payload: dict, note: str = "") -> None:
        body = json.dumps(payload, ensure_ascii=False, indent=2).encode("utf-8")
        self._send(status, body, "application/json; charset=utf-8", note)

    def _send_text(self, status: int, text: str, note: str = "") -> None:
        self._send(status, text.encode("utf-8"), "text/plain; charset=utf-8", note)

    # ---------------------------- 路由 ---------------------------- #

    def do_GET(self) -> None:  # noqa: N802 (http.server 的命名约定)
        self._route(with_body=True)

    def do_HEAD(self) -> None:  # noqa: N802
        self._route(with_body=False)

    def _route(self, with_body: bool) -> None:
        del with_body  # _send() 里按 self.command 判断
        parsed = urlparse(self.path)
        path = parsed.path.rstrip("/") or "/"

        if path == "/":
            self._send_text(200, self._help_text(), "索引页")
            return
        if path in ("/healthz", "/health"):
            self._send_text(200, "ok\n", "健康检查")
            return
        if self._is_manual_path(path):
            self._serve_manual(parsed)
            return

        self._send_json(404, {"error": "not found", "path": path}, "未知路径")

    @staticmethod
    def _is_manual_path(path: str) -> bool:
        return path in ("/api/hui/manual", "/manual") or path.endswith(".json")

    def _serve_manual(self, parsed) -> None:
        query = parse_qs(parsed.query)

        device_id = (query.get("device_id") or [""])[0]
        if not device_id and parsed.path.endswith(".json"):
            # 也支持 /api/hui/manual/0001.json 这种写法
            device_id = Path(parsed.path).stem

        if not device_id:
            self._send_json(
                400,
                {
                    "error": "missing device_id",
                    "hint": "固件会带 ?device_id=0001 过来; 手动测可以加这个参数",
                    "known": sorted(self.manuals),
                },
                "缺少 device_id",
            )
            return

        matched = find_device_id(self.manuals, device_id)
        if matched is None:
            self._send_json(
                404,
                {
                    "error": f"unknown device_id: {device_id}",
                    "hint": "把主机手册的 device_id 改成它, 或在 manuals 目录里加一份",
                    "known": sorted(self.manuals),
                },
                f"未知设备 ID {device_id}",
            )
            return

        manual = merge_common(self.manuals[matched], self.common)
        self._send_json(
            200,
            manual,
            f"{manual.get('name', '?')} (device_id={matched}, "
            f"{len(manual['commands'])} 条命令)",
        )

    def _help_text(self) -> str:
        ip = lan_ip()
        port = self.server.server_address[1]
        lines = [
            "HUI 命令手册服务 (台架用)",
            f"手册目录: {self.manuals_dir}",
            "",
            "路由:",
            "  GET /                       本页",
            "  GET /healthz                健康检查",
            "  GET /api/hui/manual?device_id=0001",
            "  GET /api/hui/manual/0001.json",
            "",
            "已索引的主机:",
        ]
        if self.manuals:
            for device_id in sorted(self.manuals):
                manual = merge_common(self.manuals[device_id], self.common)
                lines.append(
                    f"  {device_id}  {manual.get('name', '?'):<12} {len(manual['commands'])} 条命令"
                )
        else:
            lines.append("  (空 —— 手册目录里没有带 device_id 的 json)")

        lines += [
            "",
            "固件侧配置 (加到板级 config.json 的 sdkconfig_append):",
            f'  "CONFIG_HUI_MANUAL_URL=\\"http://{ip}:{port}/api/hui/manual?device_id=%s\\""',
            "",
            f"给 PC 自查: http://127.0.0.1:{port}/api/hui/manual?device_id="
            f"{sorted(self.manuals)[0] if self.manuals else '0001'}",
            "",
            "注意: 板子走 4G 时在运营商 NAT 后面, 这个地址它访问不到 ——",
            "      要么让板子连同一局域网的 WiFi, 要么把端口映射到公网。",
            "",
        ]
        return "\n".join(lines)

    # 默认的请求日志太吵, 换成一行摘要
    def log_message(self, fmt: str, *args) -> None:  # noqa: A003
        del fmt, args

    def log_error(self, fmt: str, *args) -> None:
        print(f"  [http] {fmt % args}", file=sys.stderr, flush=True)


def create_server(host: str, port: int, manuals_dir: Path, index: dict[str, dict],
                  common: dict | None) -> ThreadingHTTPServer:
    """建好服务器 (测试里可以 port=0 拿随机端口)。

    加载/校验/打印都由调用方负责 —— 这样 main() 不会和它重复输出一遍。
    """
    handler = type(
        "BoundManualHandler",
        (ManualHandler,),
        {"manuals": index, "common": common, "manuals_dir": manuals_dir},
    )
    server = ThreadingHTTPServer((host, port), handler)

    def hui_log(status: int, note: str) -> None:
        mark = "OK " if status == 200 else "!! "
        print(f"  [{mark}{status}] {note}", flush=True)

    server.hui_log = hui_log  # type: ignore[attr-defined]
    return server


def main() -> int:
    parser = argparse.ArgumentParser(
        description="本地 HUI 命令手册服务 (把 main/soa/manuals/ 按 device_id 发出去)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--host", default="0.0.0.0", help="监听地址 (默认 0.0.0.0)")
    parser.add_argument("--port", type=int, default=8080, help="监听端口 (默认 8080)")
    parser.add_argument(
        "--manuals-dir",
        type=Path,
        default=DEFAULT_MANUALS_DIR,
        help=f"手册目录 (默认 {DEFAULT_MANUALS_DIR})",
    )
    parser.add_argument("--check", action="store_true", help="只校验手册文件, 不启服务")
    args = parser.parse_args()

    print(f"HUI 命令手册服务: 手册目录 {args.manuals_dir}")

    index, common, notes = load_manuals(args.manuals_dir)
    for note in notes:
        print(f"  · {note}")

    problems = []
    for device_id, manual in sorted(index.items()):
        merged = merge_common(manual, common)
        print(f"  · device_id={device_id} {merged.get('name', '?')}: "
              f"{len(merged['commands'])} 条命令")
        problems += validate_manual(merged, f"device_id={device_id}")

    # 手册里的问题只告警 (固件会跳过单条坏命令), 但 --check 下要报错退出
    for problem in problems:
        print(f"  ! {problem}", file=sys.stderr)

    if args.check:
        if not index:
            print("没有可下发的手册 (检查 device_id 字段)", file=sys.stderr)
            return 1
        return 1 if problems else 0

    if not index:
        print("警告: 没有可下发的手册, 固件会一直报 404", file=sys.stderr)

    server = create_server(args.host, args.port, args.manuals_dir, index, common)
    port = server.server_address[1]
    ip = lan_ip()

    print()
    lines = [
        f"服务已启动: http://{args.host}:{port}",
        f"  索引页:    http://{ip}:{port}/",
        f"  健康检查:  http://{ip}:{port}/healthz",
    ]
    if index:
        first = sorted(index)[0]
        lines.append(f"  手册示例:  http://{ip}:{port}/api/hui/manual?device_id={first}")
    lines += [
        "",
        "把下面这行加进板级 config.json 的 sdkconfig_append (然后重新构建烧录):",
        f'  "CONFIG_HUI_MANUAL_URL=\\"http://{ip}:{port}/api/hui/manual?device_id=%s\\""',
        "",
        "板子走 4G 时在 NAT 后面访问不到这个地址: 让板子连同一局域网的 WiFi, 或把端口映射出去。",
        "Ctrl+C 停止。",
    ]
    print("\n".join(lines))
    print()

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n收到 Ctrl+C, 停止服务")
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
