#pragma once
#include <windows.h>

namespace qpointer {

// 仅由设置窗口的 UI 线程调用。切换前保存一帧，切换后淡出旧帧；
// 不阻塞消息循环，结束或取消立即释放位图、窗口和定时器。
class UiTransition {
public:
    static constexpr UINT_PTR kTimer = 0x514D;
    UiTransition() = default;
    ~UiTransition() { Cancel(); }
    UiTransition(const UiTransition&) = delete;
    UiTransition& operator=(const UiTransition&) = delete;
    void Capture(HWND owner, bool allowed);
    void Play(UINT durationMs);
    void Advance();
    void Cancel();
    bool Active() const { return layer != nullptr; }

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp);
    HWND owner{}, layer{};
    HDC imageDc{};
    HBITMAP bitmap{};
    HGDIOBJ previous{};
    RECT bounds{};
    ULONGLONG started{};
    UINT duration{};
};

}  // namespace qpointer
