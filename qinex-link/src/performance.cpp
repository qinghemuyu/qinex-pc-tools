#include "performance.h"

namespace performance {
namespace {

bool SetProcessPowerMask(ULONG mask, DWORD& error) {
    PROCESS_POWER_THROTTLING_STATE state{};
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = mask;
    state.StateMask = 0;  // 选中策略均关闭：HighQoS + 尊重高分辨率定时器
    if (SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling,
                              &state, sizeof(state))) {
        error = ERROR_SUCCESS;
        return true;
    }
    error = GetLastError();
    return false;
}

}  // namespace

ProcessResult EnableProcessHighPerformance() {
    ProcessResult result;
    result.highPriority = SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS) != FALSE;
    if (!result.highPriority) result.priorityError = GetLastError();

    // Win11：程序完全遮挡/最小化时可能忽略 timeBeginPeriod(1)。一次接管两项策略；
    // 老系统若不认识 IGNORE_TIMER_RESOLUTION 位，则回退为只关闭执行速度节流。
    constexpr ULONG fullMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED |
                               PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    if (SetProcessPowerMask(fullMask, result.powerError)) {
        result.executionSpeed = true;
        result.timerResolution = true;
    } else {
        DWORD fallbackError = ERROR_SUCCESS;
        if (SetProcessPowerMask(PROCESS_POWER_THROTTLING_EXECUTION_SPEED, fallbackError)) {
            result.executionSpeed = true;
            result.timerResolution = false;
        } else {
            result.powerError = fallbackError;
        }
    }
    return result;
}

ThreadResult EnableCurrentThreadHighPerformance(int threadPriority) {
    ThreadResult result;
    result.priority = SetThreadPriority(GetCurrentThread(), threadPriority) != FALSE;
    if (!result.priority) result.priorityError = GetLastError();

    THREAD_POWER_THROTTLING_STATE state{};
    state.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    state.StateMask = 0;  // HighQoS：禁止把关键线程归入 EcoQoS
    result.executionSpeed = SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling,
                                                  &state, sizeof(state)) != FALSE;
    if (!result.executionSpeed) result.powerError = GetLastError();
    return result;
}

}  // namespace performance
