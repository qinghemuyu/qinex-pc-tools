// 串口链路:COM 枚举、连接状态机(打开→等心跳→KMLINK 3M 协商→已连)、STATUS/PONG 解析
#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace link {

struct PortInfo {
    std::wstring name;       // "COM47"
    std::wstring friendly;   // "USB-Enhanced-SERIAL CH343 (COM47)"
};
std::vector<PortInfo> enumPorts();

void connect(const std::wstring& port);   // 异步:起工作线程跑状态机
void disconnect();                        // 取消挂起 I/O、停线程并关串口(UI 点击不会卡死)
void shutdown();                          // 程序退出

bool write(const uint8_t* data, int len); // 线程安全;未连接时返回 false
bool txReady();                           // 已连接 且 板子 Mode C(转发/心跳/PING 的前置)

}  // namespace link
