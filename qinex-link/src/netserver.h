// kmboxNET 本机模拟(P3):在本机开 UDP 服务,协议与板端 kmnet.cpp 一致。
// 第三方软件把本工具当作一台 kmbox 连接;收到的鼠标/键盘命令与物理输入**合流**后,
// 经当前链路(UART 0xF6/F8/0xF7)转发给 P4 板子。monitor 回读把物理捕获推回第三方,
// mask 屏蔽在工具侧执行(见 capture setPhysMask)。
//
// 盒子 MAC 由工具自持(g.netMac),UI 显示;用户在第三方软件里填同一个 MAC/IP(127.0.0.1),
// 双方据 MAC 派生同一 XXTEA 密钥。板子无需知情——工具是完整的软 kmbox,只吐 UART 帧给板子。
#pragma once

namespace netserver {

void start();   // 若 g.netEnable 则起 UDP 服务线程(幂等)
void stop();
void applyConfig();   // 端口/开关变更后重启服务(读 g.netEnable/g.netPort)
bool isUp();

}  // namespace netserver
