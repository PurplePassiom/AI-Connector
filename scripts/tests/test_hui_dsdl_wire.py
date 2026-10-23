"""守住 SlaveHeartbeat 的线上字节格式, 防止"直接发裸结构体"这个 bug 回归。

背景
----
main/soa/hui.cc 里 HuiNode_RequestHostSleep / HuiNode_RequestHostWakeup 曾经这样发:

    soa_request_send(..., (uint8_t*)&hb, sizeof(hb));

而同一文件里正确的 HuiNode_PublishHeartbeat 是走 dsdl_..._serialize_() 的。
两者的差别不在"风格", 而是**真的会发错字节**:

  * 线上格式 (Cyphal) 是 7 字节紧凑打包, count 位于偏移 5;
  * C 结构体是 8 字节 —— 4 个 u8 枚举 + session_id 之后有 1 字节对齐填充,
    count 位于偏移 6。

所以裸结构体会在帧中间多插一个 0x00, 让接收端把 count 读成错的值(或按长度直接丢弃)。
这类错误**不会**被构建、日志或 CAN 波形发现 —— 只有编译真实的 DSDL 头文件才能钉住,
因为它取决于 C 结构体的内存布局。

因此本测试在主机上编译 main/dsdl 下真实的生成头文件来验证。
没有可用的主机 C 编译器时跳过 (与 test_hui_flash_manuals.py 的跳过风格一致)。
"""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
DSDL_COMMON = REPO_ROOT / "main" / "dsdl" / "dsdl_Common"
# 生成的类型头文件会 #include <nunavut/support/serialization.h>, 它在 main/dsdl 下
DSDL_ROOT = REPO_ROOT / "main" / "dsdl"

# 打印一次性结果, 顺便把"结构体 != 线上格式"这句话用实测数字复核一遍。
PROBE_SOURCE = r"""
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "Health_1_0.h"
#include "PowerMode_1_0.h"
#include "SlaveHeartbeat_1_0.h"
#include "SleepReason_1_0.h"
#include "WakeReason_1_0.h"

int main(void)
{
    dsdl_Common_SlaveHeartbeat_1_0 hb;
    memset(&hb, 0, sizeof(hb));
    hb.mode.value         = dsdl_Common_PowerMode_1_0_WAKING;
    hb.health.value       = dsdl_Common_Health_1_0_NOMINAL;
    hb.sleep_reason.value = dsdl_Common_SleepReason_1_0_NONE;
    hb.wake_reason.value  = dsdl_Common_WakeReason_1_0_USER_REQUEST;
    hb.session_id         = 1u;
    hb.count              = 0x1234u;

    uint8_t      buf[dsdl_Common_SlaveHeartbeat_1_0_SERIALIZATION_BUFFER_SIZE_BYTES_];
    size_t       size = sizeof(buf);
    const int8_t rc   = dsdl_Common_SlaveHeartbeat_1_0_serialize_(&hb, buf, &size);

    printf("sizeof_struct=%zu\n", sizeof(hb));
    printf("buffer_size=%u\n",
           (unsigned)dsdl_Common_SlaveHeartbeat_1_0_SERIALIZATION_BUFFER_SIZE_BYTES_);
    printf("count_offset_struct=%zu\n",
           offsetof(dsdl_Common_SlaveHeartbeat_1_0, count));
    printf("serialized_len=%zu\n", size);
    printf("serialize_rc=%d\n", (int)rc);
    printf("wire=");
    for (size_t i = 0; i < size; i++) printf("%02X", buf[i]);
    printf("\nraw=");
    for (size_t i = 0; i < sizeof(hb); i++) printf("%02X", ((const uint8_t *)&hb)[i]);
    printf("\n");
    return 0;
}
"""


def _find_compiler():
    for name in ("gcc", "clang", "cc"):
        path = shutil.which(name)
        if path:
            return path
    return None


def _run_probe():
    """编译并运行探测程序; 返回 stdout 里的 key=value 字典。"""
    compiler = _find_compiler()
    if compiler is None:
        raise unittest.SkipTest("本机没有可用的主机 C 编译器 (gcc/clang/cc)")

    with tempfile.TemporaryDirectory(ignore_cleanup_errors=True) as tmp:
        tmp_dir = Path(tmp)
        src = tmp_dir / "probe.c"
        exe = tmp_dir / "probe.exe" if _is_windows() else tmp_dir / "probe"
        src.write_text(PROBE_SOURCE, encoding="utf-8")

        build = subprocess.run(
            [compiler, f"-I{DSDL_COMMON}", f"-I{DSDL_ROOT}", "-o", str(exe), str(src)],
            capture_output=True,
            text=True,
        )
        if build.returncode != 0:
            raise unittest.SkipTest(
                "编译 DSDL 头文件失败, 跳过 (可能缺少该编译器需要的头/环境):\n"
                + build.stderr
            )

        run = subprocess.run([str(exe)], capture_output=True, text=True)
        if run.returncode != 0:
            raise AssertionError(f"探测程序退出码 {run.returncode}: {run.stderr}")

    values = {}
    for line in run.stdout.splitlines():
        key, _, value = line.partition("=")
        values[key.strip()] = value.strip()
    return values


def _is_windows():
    import os

    return os.name == "nt"


class SlaveHeartbeatWireFormatTest(unittest.TestCase):
    """心跳帧必须是 serialize_ 的输出, 不能是结构体的内存镜像。"""

    @classmethod
    def setUpClass(cls):
        if not DSDL_COMMON.is_dir() or not DSDL_ROOT.is_dir():
            raise unittest.SkipTest(f"找不到 DSDL 目录: {DSDL_ROOT}")
        cls.probe = _run_probe()

    def test_serializer_writes_the_packed_seven_byte_format(self):
        self.assertEqual(self.probe["serialize_rc"], "0", "serialize_ 返回了错误")
        self.assertEqual(self.probe["buffer_size"], "7")
        self.assertEqual(self.probe["serialized_len"], "7")

    def test_struct_is_not_the_wire_format(self):
        """这正是必须序列化的原因: 结构体比线上格式多 1 字节对齐填充。"""
        self.assertEqual(
            self.probe["sizeof_struct"],
            "8",
            "SlaveHeartbeat 结构体不再是 8 字节 —— 请重新确认 hui.cc 里"
            " HuiNode_SendHeartbeat 的注释与这个测试的预期",
        )
        self.assertEqual(
            self.probe["count_offset_struct"],
            "6",
            "count 在结构体里的偏移变了, 请复核线上/内存布局的差异说明",
        )

    def test_count_lands_at_wire_offset_five_little_endian(self):
        """count=0x1234 在线上是偏移 5..6 的 34 12 (Cyphal 位序小端)。"""
        wire = bytes.fromhex(self.probe["wire"])
        self.assertEqual(len(wire), 7)
        self.assertEqual(wire[5], 0x34)
        self.assertEqual(wire[6], 0x12)

    def test_raw_struct_would_corrupt_the_frame(self):
        """裸结构体: 偏移 5 是对齐填充 0x00, count 被推到 6..7 —— 就是那个 bug。"""
        raw = bytes.fromhex(self.probe["raw"])
        wire = bytes.fromhex(self.probe["wire"])

        self.assertEqual(len(raw), 8)
        self.assertEqual(raw[5], 0x00, "对齐填充字节应为 0x00")
        self.assertEqual(raw[6:8], b"\x34\x12", "裸结构体里 count 在偏移 6")

        # 前 5 字节一致, 从第 6 字节起就被填充错开了
        self.assertEqual(raw[:5], wire[:5])
        self.assertNotEqual(raw[:7], wire, "若二者相同, 这个测试就失去意义了")


if __name__ == "__main__":
    unittest.main()
