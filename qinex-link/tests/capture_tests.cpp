// 使用真实消息泵/定时器与生产队列，模拟 Raw Input 缓冲；不安装 Hook 或注入用户输入。
#include "../src/app.h"
#include <vector>
#include <thread>
#include <iostream>
#include <stdexcept>
#include <cstring>

namespace {
std::mutex inputMutex;
std::vector<RAWINPUT> input;
std::atomic<unsigned> reads{0};
std::atomic<bool> failTimer{false};
std::atomic<bool> rawWakeWhileActive{false};
std::atomic<unsigned> activeWaits{0};
std::vector<std::vector<RAWINPUTDEVICE>> registrations;
unsigned registrationFailures = 0;
BOOL WINAPI RegisterDevices(PCRAWINPUTDEVICE devices, UINT count, UINT size) {
    if (size != sizeof(RAWINPUTDEVICE)) return FALSE;
    registrations.emplace_back(devices, devices + count);
    if (registrationFailures > 0) {
        --registrationFailures;
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    return TRUE;
}
DWORD WINAPI WaitMessages(DWORD count, const HANDLE* handles, DWORD timeout, DWORD mask, DWORD flags) {
    if (g.forwarding.load()) {
        ++activeWaits;
        if (mask & QS_RAWINPUT) rawWakeWhileActive = true;
    }
    return MsgWaitForMultipleObjectsEx(count, handles, timeout, mask, flags);
}
UINT WINAPI ReadBuffer(PRAWINPUT out, PUINT bytes, UINT) {
    ++reads;
    std::lock_guard<std::mutex> lock(inputMutex);
    UINT count = static_cast<UINT>(input.size());
    if (*bytes < count * sizeof(RAWINPUT)) return static_cast<UINT>(-1);
    if (count) memcpy(out, input.data(), count * sizeof(RAWINPUT));
    input.clear();
    return count;
}
UINT WINAPI ReadCurrent(HRAWINPUT handle, UINT, LPVOID out, PUINT bytes, UINT) {
    if (!out) { *bytes = sizeof(RAWINPUT); return 0; }
    memcpy(out, handle, sizeof(RAWINPUT));
    return sizeof(RAWINPUT);
}
HANDLE WINAPI HighTimer(LPSECURITY_ATTRIBUTES a, LPCWSTR n, DWORD f, DWORD access) {
    return failTimer ? nullptr : CreateWaitableTimerExW(a, n, f, access);
}
HANDLE WINAPI NormalTimer(LPSECURITY_ATTRIBUTES a, BOOL reset, LPCWSTR n) {
    return failTimer ? nullptr : CreateWaitableTimerW(a, reset, n);
}
}
#define GetRawInputBuffer ReadBuffer
#define GetRawInputData ReadCurrent
#define CreateWaitableTimerExW HighTimer
#define CreateWaitableTimerW NormalTimer
#define MsgWaitForMultipleObjectsEx WaitMessages
#define RegisterRawInputDevices RegisterDevices
#include "../src/capture.cpp"
#undef GetRawInputBuffer
#undef GetRawInputData
#undef CreateWaitableTimerExW
#undef CreateWaitableTimerW
#undef MsgWaitForMultipleObjectsEx
#undef RegisterRawInputDevices

App g;
void App::log(const std::wstring&) {}

namespace {
void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void RegistrationFallback() {
    const HWND window = reinterpret_cast<HWND>(42);
    registrations.clear(); registrationFailures = 0;
    Check(registerCaptureDevices(window), "enhanced registration must succeed");
    Check(registrations.size() == 1 && registrations[0].size() == 2, "register both devices once");
    const auto& mouse = registrations[0][0];
    const auto& keyboard = registrations[0][1];
    Check(mouse.usUsagePage == 1 && mouse.usUsage == 2 && mouse.dwFlags == 0x8100 &&
          mouse.hwndTarget == window, "request per-listener background mouse input");
    Check(keyboard.usUsagePage == 1 && keyboard.usUsage == 6 && keyboard.dwFlags == RIDEV_INPUTSINK &&
          keyboard.hwndTarget == window, "keyboard registration must retain standard background input");
    registrations.clear(); registrationFailures = 1;
    Check(registerCaptureDevices(window), "unsupported flag must fall back to standard registration");
    Check(registrations.size() == 2 && registrations[1].size() == 2, "standard fallback must register both devices");
    Check(registrations[1][0].dwFlags == RIDEV_INPUTSINK &&
          registrations[1][1].dwFlags == RIDEV_INPUTSINK, "fallback must remove only compatibility flag");
    registrations.clear(); registrationFailures = 2;
    Check(!registerCaptureDevices(window) && registrations.size() == 2,
          "registration failure must remain a failure with bounded retry");
}
RAWINPUT Mouse(int x, int y, USHORT flags = 0, SHORT wheel = 0) {
    RAWINPUT r{};
    r.header = {RIM_TYPEMOUSE, sizeof(RAWINPUT), reinterpret_cast<HANDLE>(1), RIM_INPUTSINK};
    r.data.mouse.lLastX = x; r.data.mouse.lLastY = y;
    r.data.mouse.usButtonFlags = flags;
    r.data.mouse.usButtonData = static_cast<USHORT>(wheel);
    return r;
}
struct Pump {
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD tid = 0;
    std::thread worker;
    Pump() : worker([this] {
        MSG m{};
        PeekMessageW(&m, nullptr, 0, 0, PM_NOREMOVE);
        tid = GetCurrentThreadId();
        SetEvent(ready);
        runCaptureMessageLoop();
    }) { WaitForSingleObject(ready, INFINITE); }
    ~Pump() {
        PostThreadMessageW(tid, WM_QUIT, 0, 0);
        worker.join();
        CloseHandle(ready);
    }
};
void BackgroundBatch(bool fallback) {
    capture::resetState();
    g.inEvents = 0;
    g.forwarding = false;
    reads = 0;
    rawWakeWhileActive = false;
    activeWaits = 0;
    failTimer = fallback;
    {
        Pump pump;
        Sleep(20);
        Check(reads == 0, "idle pump must not poll raw input");
        g.forwarding = true;
        PostThreadMessageW(pump.tid, WM_NULL, 0, 0);
        ULONGLONG until = GetTickCount64() + 1000;
        while (reads == 0 && GetTickCount64() < until) Sleep(1);
        Check(reads != 0, "forwarding activation must wake idle pump");
        {
            std::lock_guard<std::mutex> lock(inputMutex);
            for (int i = 0; i < 10; ++i)
                input.push_back(Mouse(i % 2 ? -7 : 11, i + 1,
                    i == 0 ? RI_MOUSE_LEFT_BUTTON_DOWN : i == 9 ? RI_MOUSE_LEFT_BUTTON_UP : 0));
        }
        // 入缓冲后没有 WM_INPUT 或其它消息：必须依靠独立定时唤醒取得十份报告。
        until = GetTickCount64() + 1000;
        while (g.inEvents < 10 && GetTickCount64() < until) Sleep(1);
        Check(g.inEvents == 10, "background buffer must drain without WM_INPUT notification");
        uint64_t last = 0;
        for (int i = 0; i < 10; ++i) {
            capture::Motion m{};
            Check(capture::popMotion(m), "lost motion report");
            Check(m.dx == (i % 2 ? -7 : 11) && m.dy == i + 1, "motion order/direction changed");
            Check(m.atUs > last, "batch timestamps collapsed");
            last = m.atUs;
        }
        capture::Motion m{};
        Check(!capture::popMotion(m), "motion report replayed");
        capture::Edge edge{};
        Check(capture::popEdge(edge) && edge.btn == 1, "button down lost");
        Check(capture::popEdge(edge) && edge.btn == 0, "button up lost");
        Check(!capture::popEdge(edge), "button edge replayed");
        Check(activeWaits > 0 && !rawWakeWhileActive,
            "throttled raw-input queue must not wake active polling into a busy loop");
        g.forwarding = false;
        PostThreadMessageW(pump.tid, WM_NULL, 0, 0);
        Sleep(30);
        unsigned stoppedReads = reads;
        Sleep(30);
        Check(reads == stoppedReads, "stopping must disable active polling");
    }
}
void CurrentAndBuffered() {
    capture::resetState();
    RAWINPUT current = Mouse(3, -4, RI_MOUSE_WHEEL, WHEEL_DELTA);
    input.push_back(Mouse(-9, 6));
    dispatchRawInputBatch(reinterpret_cast<HRAWINPUT>(&current), GetTickCount64() * 1000ull);
    dispatchRawInputBatch(nullptr, 0);
    capture::Motion m{};
    Check(capture::popMotion(m) && m.dx == 3 && m.dy == -4 && m.wheel == 1, "current input lost");
    Check(capture::popMotion(m) && m.dx == -9 && m.dy == 6, "buffered input lost");
    Check(!capture::popMotion(m), "current/buffered input duplicated");
    capture::resetState();
    Check(!capture::popMotion(m) && capture::btnState() == 0, "reset must release state");
}
}
int main() {
    try {
        RegistrationFallback();
        BackgroundBatch(false);
        BackgroundBatch(true);
        CurrentAndBuffered();
        std::cout << "Capture background, fallback, motion and release tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
