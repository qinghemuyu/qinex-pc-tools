// Windows 11 任务栏控制岛：仅负责展示与向主窗口投递命令，不参与输入捕获/发送热路径。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace taskbar_widget {

// 启动独立的低优先级 UI 线程。失败只影响控制岛，绝不影响 QInEX-Link 主功能。
void start(HINSTANCE instance, bool enabled, int tier, bool exclusive);
void stop();

// 以下接口均可由主 UI 线程调用，内部只更新原子快照并投递消息。
void setEnabled(bool enabled);
void updateConfig(int tier, bool exclusive);

}  // namespace taskbar_widget
