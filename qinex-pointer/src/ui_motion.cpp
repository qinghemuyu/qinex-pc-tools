#include "ui_motion.h"
#include <algorithm>

namespace qpointer {
namespace {
constexpr wchar_t kTransitionClass[] = L"QInEX.Pointer.UiTransition";
}

void UiTransition::Capture(HWND window, bool allowed) {
    Cancel();
    if (!allowed || !IsWindowVisible(window) || IsIconic(window)) return;
    if (!GetClientRect(window, &bounds) || bounds.right <= 0 || bounds.bottom <= 0 ||
            static_cast<LONGLONG>(bounds.right) * bounds.bottom > 4000000) return;
    owner = window;
    HDC dc = GetDC(window);
    if (dc) {
        imageDc = CreateCompatibleDC(dc);
        bitmap = CreateCompatibleBitmap(dc, bounds.right, bounds.bottom);
        ReleaseDC(window, dc);
    }
    if (!imageDc || !bitmap) { Cancel(); return; }
    previous = SelectObject(imageDc, bitmap);
    if (!previous || previous == HGDI_ERROR) { previous = nullptr; Cancel(); return; }
    // 由控件自身绘制，窗口被其他应用遮挡时也不会采到桌面或其他应用。
    SendMessageW(window, WM_PRINT, reinterpret_cast<WPARAM>(imageDc),
        PRF_CHECKVISIBLE | PRF_CLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
}

void UiTransition::Play(UINT durationMs) {
    RECT current{};
    if (!owner || !bitmap) return;
    if (!IsWindowVisible(owner) || IsIconic(owner) || !GetClientRect(owner, &current) ||
            !EqualRect(&current, &bounds)) { Cancel(); return; }
    // 先把真实页面画好，再覆盖旧帧，避免父窗体的子窗口裁剪留下旧背景。
    RedrawWindow(owner, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    const auto instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    WNDCLASSEXW cls{sizeof(cls)};
    cls.hInstance = instance; cls.lpfnWndProc = WindowProc; cls.lpszClassName = kTransitionClass;
    if (!GetClassInfoExW(instance, kTransitionClass, &cls)) {
        cls = {sizeof(cls)};
        cls.hInstance = instance; cls.lpfnWndProc = WindowProc; cls.lpszClassName = kTransitionClass;
        if (!RegisterClassExW(&cls)) { Cancel(); return; }
    }
    layer = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
        kTransitionClass, L"", WS_CHILD, 0, 0, bounds.right, bounds.bottom,
        owner, nullptr, instance, this);
    if (!layer || !SetLayeredWindowAttributes(layer, 0, 255, LWA_ALPHA)) { Cancel(); return; }
    SetWindowPos(layer, HWND_TOP, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    UpdateWindow(layer);
    duration = std::clamp(durationMs, 80u, 240u);
    started = GetTickCount64();
    if (!SetTimer(owner, kTimer, 16, nullptr)) Cancel();
}

void UiTransition::Advance() {
    if (!layer) return;
    if (!IsWindowVisible(owner) || IsIconic(owner)) { Cancel(); return; }
    const ULONGLONG elapsed = GetTickCount64() - started;
    if (elapsed >= duration) { Cancel(); return; }
    const double t = static_cast<double>(elapsed) / duration;
    // 平滑起止；每帧只修改旧画面的不透明度，不重新遍历和绘制表单。
    const double eased = t * t * (3.0 - 2.0 * t);
    if (!SetLayeredWindowAttributes(layer, 0, static_cast<BYTE>(255 * (1.0 - eased)), LWA_ALPHA))
        Cancel();
}

void UiTransition::Cancel() {
    if (owner) KillTimer(owner, kTimer);
    if (layer) { HWND window = layer; layer = nullptr; DestroyWindow(window); }
    if (imageDc) {
        if (previous) SelectObject(imageDc, previous);
        DeleteDC(imageDc); imageDc = nullptr;
    }
    if (bitmap) { DeleteObject(bitmap); bitmap = nullptr; }
    previous = nullptr; owner = nullptr;
}

LRESULT CALLBACK UiTransition::WindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    auto* transition = reinterpret_cast<UiTransition*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        transition = static_cast<UiTransition*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(transition));
    }
    switch (message) {
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        if (transition && transition->imageDc)
            BitBlt(dc, 0, 0, transition->bounds.right, transition->bounds.bottom,
                transition->imageDc, 0, 0, SRCCOPY);
        EndPaint(window, &paint); return 0;
    }
    case WM_NCDESTROY:
        if (transition && transition->layer == window) transition->layer = nullptr;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        break;
    }
    return DefWindowProcW(window, message, wp, lp);
}
}  // namespace qpointer
