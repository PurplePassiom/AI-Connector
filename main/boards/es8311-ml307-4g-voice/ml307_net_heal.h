#ifndef _ML307_NET_HEAL_H_
#define _ML307_NET_HEAL_H_

#include <memory>

class AtUart;

// 启动 4G 网络 / SIM 自愈看门狗（由 config.h 的 NET_HEAL_ENABLE 控制）。
//
// 背景（本板实测 + 日志证据）：
//   ML307C 会偶发进入「掉网 + SIM 不可用」的锁死状态，日志特征是三件事同时出现：
//     +CEREG: 0               模组主动注销（不是 "2,0 正在搜网"，是直接不搜了）
//     +CSQ: 99,99             射频读不到任何信号
//     +CME ERROR: 30 / 13     COPS/其它网络命令报「无网络服务」；SIM 相关命令报「SIM 失败」
//   之后上层每次重试（检查新版本 → 读 COPS/CSQ/MCCID）就会成对打印 30 + 13，
//   而**模组永远不会自己重试**，只能靠重新初始化射频与 SIM 接口（AT+CFUN=0 → AT+CFUN=1）
//   或真正断电来恢复。
//
// at_uart：4G 模组的 AT 通道（AtModem::GetAtUart()）。任务不返回。
void StartMl307NetHealTask(std::shared_ptr<AtUart> at_uart);

#endif  // _ML307_NET_HEAL_H_
