// QInEX-Link 后台性能策略。
// 目标：主窗口完全被遮挡/最小化时，关键输入链路仍按前台质量调度。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace performance {

struct ProcessResult {
    bool highPriority = false;
    bool executionSpeed = false;
    bool timerResolution = false;
    DWORD priorityError = ERROR_SUCCESS;
    DWORD powerError = ERROR_SUCCESS;
};

struct ThreadResult {
    bool priority = false;
    bool executionSpeed = false;
    DWORD priorityError = ERROR_SUCCESS;
    DWORD powerError = ERROR_SUCCESS;
};

// 使用 HIGH_PRIORITY_CLASS，绝不使用会威胁系统响应性的 REALTIME_PRIORITY_CLASS。
// 同时关闭进程级执行速度节流，并要求 Windows 11 在窗口不可见时继续尊重 1ms 定时器。
ProcessResult EnableProcessHighPerformance();

// 对当前关键线程关闭 EcoQoS，并设置调用方指定的线程优先级。
// 任一设置失败都只返回状态，不终止程序。
ThreadResult EnableCurrentThreadHighPerformance(int threadPriority);

}  // namespace performance
