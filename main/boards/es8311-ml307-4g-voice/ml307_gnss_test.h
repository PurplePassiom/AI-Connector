#ifndef _ML307_GNSS_TEST_H_
#define _ML307_GNSS_TEST_H_

#include <memory>

class AtUart;

// 启动 ML307 GNSS 定位测试任务（由 config.h 的 GNSS_TEST 开关控制，见 config.h）。
//
// 任务做的事：
//   1) AT+MGNSS?                     查询当前 GNSS 状态
//   2) AT+MGNSS=0 / AT+MGNSS=<mode>  先关再开，保证从干净状态开始
//   3) 循环 AT+MGNSSNMEA="GGA"       解析并打印 UTC 时间、经纬度、定位状态
//
// at_uart：4G 模组的 AT 通道，通常来自 AtModem::GetAtUart()。
// 任务只发只读查询命令（不碰 TTS/音频/协议），发的每条命令都与驱动共用
// AtUart 的命令锁，所以不会破坏正常 4G 业务；任务不返回（纯测试模式）。
void StartMl307GnssTest(std::shared_ptr<AtUart> at_uart);

#endif  // _ML307_GNSS_TEST_H_
