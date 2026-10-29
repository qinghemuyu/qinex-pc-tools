#include "app.h"

namespace {

constexpr int kHotkeyFirst = 510;
constexpr int kHotkeyClearFirst = 520;
constexpr int kHotkeyDefaults = 530;
constexpr int kCursorChoose = 610;
constexpr int kCursorCenter = 611;

struct ModalWindowState {
    HWND window{};
    HFONT font{};
    bool accepted = false;
};

int DialogPx(HWND window, double value) {
    const UINT windowDpi = window ? GetDpiForWindow(window) : USER_DEFAULT_SCREEN_DPI;
    const double dpi = windowDpi ? static_cast<double>(windowDpi) : USER_DEFAULT_SCREEN_DPI;
    return static_cast<int>(std::lround(value * dpi / USER_DEFAULT_SCREEN_DPI));
}

void MoveDialogControl(HWND control, HWND dialog, int x, int y, int width, int height) {
    if (!control) return;
    SetWindowPos(control, nullptr, DialogPx(dialog, x), DialogPx(dialog, y),
        DialogPx(dialog, width), DialogPx(dialog, height),
        SWP_NOACTIVATE | SWP_NOZORDER);
}

void UseDialogFont(HWND parent, HFONT font) {
    EnumChildWindows(parent, [](HWND child, LPARAM value) {
        SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(value), TRUE);
        return TRUE;
    }, reinterpret_cast<LPARAM>(font));
}

void RefreshDialogFont(ModalWindowState& state) {
    HFONT replacement = CreateFontW(-DialogPx(state.window, 15), 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    if (!replacement) return;
    UseDialogFont(state.window, replacement);
    if (state.font) DeleteObject(state.font);
    state.font = replacement;
}

void ReleaseDialogFont(ModalWindowState& state) {
    if (!state.font) return;
    DeleteObject(state.font);
    state.font = nullptr;
}

void CenterDialog(HWND window, HWND owner) {
    RECT dialog{}, anchor{};
    GetWindowRect(window, &dialog);
    if (!owner || !GetWindowRect(owner, &anchor)) {
        anchor = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    }
    const int width = dialog.right - dialog.left, height = dialog.bottom - dialog.top;
    const int x = anchor.left + ((anchor.right - anchor.left) - width) / 2;
    const int y = anchor.top + ((anchor.bottom - anchor.top) - height) / 2;
    SetWindowPos(window, HWND_TOP, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
}

bool RunModalWindow(HWND owner, ModalWindowState& state) {
    // Hosted settings are a child; disable the whole owner while the stack-backed dialog runs.
    owner=GetAncestor(owner,GA_ROOT);
    EnableWindow(owner, FALSE);
    ShowWindow(state.window, SW_SHOW);
    SetForegroundWindow(state.window);
    MSG message{};
    bool quit = false;
    int quitCode = 0;
    while (IsWindow(state.window)) {
        const BOOL result = GetMessageW(&message, nullptr, 0, 0);
        if (result <= 0) {
            if (result == 0) { quit = true; quitCode = static_cast<int>(message.wParam); }
            // 状态保存在调用栈上；退出嵌套消息循环前必须同步销毁窗口，避免
            // GWLP_USERDATA 在栈对象失效后仍被窗口清理消息访问。
            if (state.window && IsWindow(state.window)) DestroyWindow(state.window);
            break;
        }
        if (!IsDialogMessageW(state.window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    if (quit) PostQuitMessage(quitCode);
    return state.accepted;
}

struct HotkeyDialogState : ModalWindowState {
    Application* application{};
    qp::HotkeyBinding bindings[4];
    HWND labels[4]{};
    HWND hotkeys[4]{};
    HWND clearButtons[4]{};
    HWND hint{};
    HWND defaults{};
    HWND save{};
    HWND cancel{};
};

void LayoutHotkeyDialog(HotkeyDialogState& state) {
    for (int index = 0; index < 4; ++index) {
        MoveDialogControl(state.labels[index], state.window, 24, 25 + index * 45, 138, 24);
        MoveDialogControl(state.hotkeys[index], state.window, 166, 20 + index * 45, 238, 29);
        MoveDialogControl(state.clearButtons[index], state.window,
            414, 20 + index * 45, 72, 29);
    }
    MoveDialogControl(state.hint, state.window, 24, 205, 462, 40);
    MoveDialogControl(state.defaults, state.window, 24, 252, 112, 30);
    MoveDialogControl(state.save, state.window, 252, 252, 112, 30);
    MoveDialogControl(state.cancel, state.window, 374, 252, 112, 30);
}

void SetHotkeyDefaults(HotkeyDialogState& state) {
    const qp::HotkeyBinding defaults[]{{VK_F10, MOD_CONTROL | MOD_ALT},
        {VK_F11, 0}, {VK_F8, 0}, {VK_F7, 0}};
    for (int index = 0; index < 4; ++index)
        qp::WriteHotkeyControl(GetDlgItem(state.window, kHotkeyFirst + index), defaults[index]);
}

bool ValidateHotkeys(HotkeyDialogState& state) {
    for (int index = 0; index < 4; ++index)
        state.bindings[index] = qp::ReadHotkeyControl(
            GetDlgItem(state.window, kHotkeyFirst + index));
    const wchar_t* names[]{W(L"暂停 / 恢复", L"暂停 / 恢复喵"),
        W(L"返回设置", L"返回设置喵"), W(L"调整光标范围", L"调整光标范围喵"),
        W(L"显示 / 隐藏 HUD", L"显示 / 隐藏 HUD 喵")};
    for (int index = 0; index < 4; ++index) {
        if (!qp::IsUsableHotkey(state.bindings[index], index == 1)) {
            MessageBoxW(state.window,
                (std::wstring(names[index]) + W(
                    L"快捷键无效。普通按键需要搭配 Ctrl、Alt 或 Shift；也可以直接使用功能键。",
                    L"快捷键不对喵。普通按键要搭配 Ctrl、Alt 或 Shift，也可以直接用功能键喵。")).c_str(),
                L"QInEX Pointer", MB_OK | MB_ICONWARNING);
            SetFocus(GetDlgItem(state.window, kHotkeyFirst + index));
            return false;
        }
        if (qp::IsLinkPanicHotkey(state.bindings[index])) {
            MessageBoxW(state.window,
                W(L"包含 Ctrl + Alt + Q 的组合是 QInEX-Link 固定紧急释放键，请换一个组合。",
                    L"包含 Ctrl + Alt + Q 的组合要留给 Link 紧急释放喵，请换一个。"),
                L"QInEX Pointer", MB_OK | MB_ICONWARNING);
            SetFocus(GetDlgItem(state.window, kHotkeyFirst + index));
            return false;
        }
        if (qp::IsCaptureFallbackHotkey(state.bindings[index])) {
            MessageBoxW(state.window,
                W(L"Ctrl + Alt + F12 是全屏固定备用退出键，请换一个组合。",
                    L"Ctrl + Alt + F12 要留作全屏备用退出键喵，请换一个。"),
                L"QInEX Pointer", MB_OK | MB_ICONWARNING);
            SetFocus(GetDlgItem(state.window, kHotkeyFirst + index));
            return false;
        }
        if (!state.bindings[index].virtualKey) continue;
        for (int prior = 0; prior < index; ++prior) {
            if (state.bindings[index] == state.bindings[prior]) {
                MessageBoxW(state.window, W(L"同一个快捷键不能分配给两个操作。",
                        L"同一个快捷键不能同时管两个操作喵。"),
                    L"QInEX Pointer", MB_OK | MB_ICONWARNING);
                SetFocus(GetDlgItem(state.window, kHotkeyFirst + index));
                return false;
            }
        }
    }
    return true;
}

LRESULT CALLBACK HotkeyDialogProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<HotkeyDialogState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = static_cast<HotkeyDialogState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        state->window = window;
    }
    if (!state) return DefWindowProcW(window, message, wParam, lParam);
    switch (message) {
    case WM_CREATE: {
        const wchar_t* labels[]{W(L"暂停 / 恢复", L"暂停 / 恢复喵"),
            W(L"返回设置", L"返回设置喵"), W(L"调整光标范围", L"调整光标范围喵"),
            W(L"显示 / 隐藏 HUD", L"显示 / 隐藏 HUD 喵")};
        for (int index = 0; index < 4; ++index) {
            state->labels[index] = CreateWindowExW(0, L"STATIC", labels[index],
                WS_CHILD | WS_VISIBLE, 0, 0, 1, 1, window, nullptr,
                state->application->instance, nullptr);
            state->hotkeys[index] = CreateWindowExW(WS_EX_CLIENTEDGE, HOTKEY_CLASSW, L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 1, 1,
                window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kHotkeyFirst + index)),
                state->application->instance, nullptr);
            qp::WriteHotkeyControl(state->hotkeys[index], state->bindings[index]);
            if (index != 1) {
                state->clearButtons[index] = CreateWindowExW(0, L"BUTTON",
                    W(L"清除", L"清除喵"), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                    0, 0, 1, 1, window,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(kHotkeyClearFirst + index)),
                    state->application->instance, nullptr);
            }
        }
        state->hint = CreateWindowExW(0, L"STATIC", W(
                L"Esc 或 Ctrl + Alt + F12 始终可以退出全屏；Ctrl + Alt + Q 始终可以紧急释放 QInEX-Link。",
                L"Esc 或 Ctrl + Alt + F12 随时退出全屏；Ctrl + Alt + Q 随时紧急释放 QInEX-Link 喵。"),
            WS_CHILD | WS_VISIBLE, 0, 0, 1, 1, window, nullptr,
            state->application->instance, nullptr);
        state->defaults = CreateWindowExW(0, L"BUTTON", W(L"恢复默认", L"恢复默认喵"),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 1, 1, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kHotkeyDefaults)),
            state->application->instance, nullptr);
        state->save = CreateWindowExW(0, L"BUTTON", W(L"保存并应用", L"保存并应用喵"),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0, 1, 1, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)), state->application->instance, nullptr);
        state->cancel = CreateWindowExW(0, L"BUTTON", W(L"取消", L"取消喵"),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 1, 1, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)), state->application->instance, nullptr);
        LayoutHotkeyDialog(*state);
        RefreshDialogFont(*state);
        return 0;
    }
    case WM_DPICHANGED: {
        const auto* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(window, nullptr, suggested->left, suggested->top,
            suggested->right - suggested->left, suggested->bottom - suggested->top,
            SWP_NOACTIVATE | SWP_NOZORDER);
        LayoutHotkeyDialog(*state);
        RefreshDialogFont(*state);
        InvalidateRect(window, nullptr, TRUE);
        return 0;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        if (id >= kHotkeyClearFirst && id < kHotkeyClearFirst + 4) {
            qp::WriteHotkeyControl(GetDlgItem(window,
                kHotkeyFirst + id - kHotkeyClearFirst), {});
            return 0;
        }
        if (id == kHotkeyDefaults) { SetHotkeyDefaults(*state); return 0; }
        if (id == IDOK) {
            if (!ValidateHotkeys(*state)) return 0;
            state->accepted = true;
            DestroyWindow(window);
            return 0;
        }
        if (id == IDCANCEL) { DestroyWindow(window); return 0; }
        break;
    }
    case WM_CTLCOLORSTATIC:
        SetBkMode(reinterpret_cast<HDC>(wParam), TRANSPARENT);
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    case WM_CLOSE: DestroyWindow(window); return 0;
    case WM_DESTROY:
        ReleaseDialogFont(*state);
        state->window = nullptr;
        return 0;
    default: break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool ChooseCursorImage(HWND owner, std::wstring& path) {
    wchar_t selected[32768]{};
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = owner;
    dialog.lpstrFile = selected;
    dialog.nMaxFile = static_cast<DWORD>(std::size(selected));
    dialog.lpstrFilter = W(
        L"图片文件 (*.png;*.jpg;*.jpeg;*.bmp;*.gif)\0*.png;*.jpg;*.jpeg;*.bmp;*.gif\0所有文件 (*.*)\0*.*\0\0",
        L"图片文件喵 (*.png;*.jpg;*.jpeg;*.bmp;*.gif)\0*.png;*.jpg;*.jpeg;*.bmp;*.gif\0所有文件喵 (*.*)\0*.*\0\0");
    dialog.lpstrTitle = W(L"选择自定义光标图片", L"选择自定义光标图片喵");
    dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&dialog)) return false;
    path = selected;
    return true;
}

struct CursorDialogState : ModalWindowState {
    Application* application{};
    std::shared_ptr<const qp::CustomCursorImage> image;
    std::wstring sourcePath;
    double hotspotX = .5, hotspotY = .5;
    double zoom = 1;
    double panX = 0, panY = 0;
    bool draggingHotspot = false;
    HWND choose{};
    HWND center{};
    HWND save{};
    HWND cancel{};
    HBRUSH checkerLight{};
    HBRUSH checkerDark{};
    HDC backBufferDc{};
    HBITMAP backBufferBitmap{};
    HGDIOBJ backBufferOld{};
    int backBufferWidth = 0;
    int backBufferHeight = 0;
};

void LayoutCursorDialog(CursorDialogState& state) {
    MoveDialogControl(state.choose, state.window, 20, 570, 112, 30);
    MoveDialogControl(state.center, state.window, 142, 570, 126, 30);
    MoveDialogControl(state.save, state.window, 432, 570, 112, 30);
    MoveDialogControl(state.cancel, state.window, 554, 570, 106, 30);
}

void ReleaseCursorBackBuffer(CursorDialogState& state) {
    if (state.backBufferDc) {
        if (state.backBufferOld && state.backBufferOld != HGDI_ERROR)
            SelectObject(state.backBufferDc, state.backBufferOld);
        if (state.backBufferBitmap) DeleteObject(state.backBufferBitmap);
        DeleteDC(state.backBufferDc);
    }
    state.backBufferDc = nullptr;
    state.backBufferBitmap = nullptr;
    state.backBufferOld = nullptr;
    state.backBufferWidth = 0;
    state.backBufferHeight = 0;
}

bool EnsureCursorBackBuffer(CursorDialogState& state, HDC reference, int width, int height) {
    if (width <= 0 || height <= 0) return false;
    if (state.backBufferDc && state.backBufferWidth == width &&
            state.backBufferHeight == height) return true;
    ReleaseCursorBackBuffer(state);
    state.backBufferDc = CreateCompatibleDC(reference);
    state.backBufferBitmap = state.backBufferDc
        ? CreateCompatibleBitmap(reference, width, height) : nullptr;
    if (!state.backBufferDc || !state.backBufferBitmap) {
        ReleaseCursorBackBuffer(state);
        return false;
    }
    state.backBufferOld = SelectObject(state.backBufferDc, state.backBufferBitmap);
    if (!state.backBufferOld || state.backBufferOld == HGDI_ERROR) {
        ReleaseCursorBackBuffer(state);
        return false;
    }
    state.backBufferWidth = width;
    state.backBufferHeight = height;
    return true;
}

void ReleaseCursorPaintResources(CursorDialogState& state) {
    ReleaseCursorBackBuffer(state);
    if (state.checkerLight) DeleteObject(state.checkerLight);
    if (state.checkerDark) DeleteObject(state.checkerDark);
    state.checkerLight = nullptr;
    state.checkerDark = nullptr;
}

RECT CursorCanvas(const CursorDialogState& state) {
    RECT client{};
    GetClientRect(state.window, &client);
    const LONG inset = DialogPx(state.window, 20);
    return {inset, DialogPx(state.window, 58), std::max(inset + 1, client.right - inset),
        std::max<LONG>(DialogPx(state.window, 59), client.bottom - DialogPx(state.window, 116))};
}

Gdiplus::RectF CursorImageRect(const CursorDialogState& state, const RECT& canvas) {
    if (!state.image || !state.image->width || !state.image->height) return {};
    const LONG margin = DialogPx(state.window, 36);
    const double availableWidth = std::max(1L, canvas.right - canvas.left - margin);
    const double availableHeight = std::max(1L, canvas.bottom - canvas.top - margin);
    const double fit = std::min(availableWidth / state.image->width,
        availableHeight / state.image->height);
    const double scale = fit * state.zoom;
    const float width = static_cast<float>(state.image->width * scale);
    const float height = static_cast<float>(state.image->height * scale);
    const float centerX = static_cast<float>((canvas.left + canvas.right) * .5 + state.panX);
    const float centerY = static_cast<float>((canvas.top + canvas.bottom) * .5 + state.panY);
    return {centerX - width * .5f, centerY - height * .5f, width, height};
}

void SetCursorHotspotFromPoint(CursorDialogState& state, POINT point) {
    const auto imageRect = CursorImageRect(state, CursorCanvas(state));
    if (imageRect.Width <= 0 || imageRect.Height <= 0) return;
    state.hotspotX = std::clamp((point.x - imageRect.X) / imageRect.Width, 0.0f, 1.0f);
    state.hotspotY = std::clamp((point.y - imageRect.Y) / imageRect.Height, 0.0f, 1.0f);
    InvalidateRect(state.window, nullptr, FALSE);
}

void PaintCursorEditor(CursorDialogState& state) {
    PAINTSTRUCT paint{};
    HDC windowDc = BeginPaint(state.window, &paint);
    RECT client{};
    GetClientRect(state.window, &client);
    const int width = std::max<LONG>(0, client.right - client.left);
    const int height = std::max<LONG>(0, client.bottom - client.top);
    const bool buffered = EnsureCursorBackBuffer(state, windowDc, width, height);
    HDC dc = buffered ? state.backBufferDc : windowDc;
    FillRect(dc, &client, GetSysColorBrush(COLOR_WINDOW));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(33, 42, 48));
    HGDIOBJ oldFont = state.font ? SelectObject(dc, state.font) : nullptr;
    RECT title{DialogPx(state.window, 20), DialogPx(state.window, 17),
        client.right - DialogPx(state.window, 20), DialogPx(state.window, 47)};
    DrawTextW(dc, W(L"拖动十字设置点击热点；滚轮缩放图片以便精确标定。",
            L"拖动十字设置点击热点，滚轮缩放后能标得更准喵。"), -1,
        &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (oldFont && oldFont != HGDI_ERROR) SelectObject(dc, oldFont);

    const RECT canvas = CursorCanvas(state);
    const int tile = std::max(8, DialogPx(state.window, 16));
    HBRUSH light = state.checkerLight ? state.checkerLight : GetSysColorBrush(COLOR_WINDOW);
    HBRUSH dark = state.checkerDark ? state.checkerDark : GetSysColorBrush(COLOR_3DFACE);
    for (int y = canvas.top; y < canvas.bottom; y += tile) {
        for (int x = canvas.left; x < canvas.right; x += tile) {
            RECT square{x, y, std::min<LONG>(x + tile, canvas.right),
                std::min<LONG>(y + tile, canvas.bottom)};
            FillRect(dc, &square,
                ((x - canvas.left) / tile + (y - canvas.top) / tile) % 2 ? dark : light);
        }
    }
    FrameRect(dc, &canvas, reinterpret_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));
    if (state.image) {
        auto* pixels = const_cast<BYTE*>(reinterpret_cast<const BYTE*>(
            state.image->premultipliedBgra.data()));
        Gdiplus::Bitmap bitmap(state.image->width, state.image->height, state.image->stride,
            PixelFormat32bppPARGB, pixels);
        {
            Gdiplus::Graphics graphics(dc);
            graphics.SetClip(Gdiplus::Rect(canvas.left + 1, canvas.top + 1,
                std::max(0L, canvas.right - canvas.left - 2),
                std::max(0L, canvas.bottom - canvas.top - 2)));
            graphics.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
            const auto destination = CursorImageRect(state, canvas);
            graphics.DrawImage(&bitmap, destination);
            const float hotspotX = destination.X + destination.Width *
                static_cast<float>(state.hotspotX);
            const float hotspotY = destination.Y + destination.Height *
                static_cast<float>(state.hotspotY);
            Gdiplus::Pen shadow(Gdiplus::Color(220, 0, 0, 0),
                static_cast<Gdiplus::REAL>(DialogPx(state.window, 5)));
            Gdiplus::Pen marker(Gdiplus::Color(255, 0, 215, 235),
                static_cast<Gdiplus::REAL>(DialogPx(state.window, 2)));
            for (auto* pen : {&shadow, &marker}) {
                const float arm = static_cast<float>(DialogPx(state.window, 18));
                const float radius = static_cast<float>(DialogPx(state.window, 7));
                graphics.DrawLine(pen, hotspotX - arm, hotspotY, hotspotX + arm, hotspotY);
                graphics.DrawLine(pen, hotspotX, hotspotY - arm, hotspotX, hotspotY + arm);
                graphics.DrawEllipse(pen, hotspotX - radius, hotspotY - radius,
                    radius * 2, radius * 2);
            }
        }
    }
    if (buffered) BitBlt(windowDc, 0, 0, width, height, dc, 0, 0, SRCCOPY);
    EndPaint(state.window, &paint);
}

LRESULT CALLBACK CursorDialogProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<CursorDialogState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = static_cast<CursorDialogState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        state->window = window;
    }
    if (!state) return DefWindowProcW(window, message, wParam, lParam);
    switch (message) {
    case WM_CREATE: {
        state->checkerLight = CreateSolidBrush(RGB(247, 249, 250));
        state->checkerDark = CreateSolidBrush(RGB(224, 228, 231));
        state->choose = CreateWindowExW(0, L"BUTTON", W(L"重新选图…", L"重新选图喵…"),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 1, 1, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCursorChoose)),
            state->application->instance, nullptr);
        state->center = CreateWindowExW(0, L"BUTTON", W(L"热点回到中心", L"热点回到中心喵"),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 1, 1, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCursorCenter)),
            state->application->instance, nullptr);
        state->save = CreateWindowExW(0, L"BUTTON", W(L"保存并应用", L"保存并应用喵"),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0, 1, 1, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)), state->application->instance, nullptr);
        state->cancel = CreateWindowExW(0, L"BUTTON", W(L"取消", L"取消喵"),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 1, 1, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)), state->application->instance, nullptr);
        LayoutCursorDialog(*state);
        RefreshDialogFont(*state);
        return 0;
    }
    case WM_PAINT: PaintCursorEditor(*state); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE:
        ReleaseCursorBackBuffer(*state);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_DPICHANGED: {
        const auto* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(window, nullptr, suggested->left, suggested->top,
            suggested->right - suggested->left, suggested->bottom - suggested->top,
            SWP_NOACTIVATE | SWP_NOZORDER);
        LayoutCursorDialog(*state);
        RefreshDialogFont(*state);
        ReleaseCursorBackBuffer(*state);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const RECT canvas = CursorCanvas(*state);
        if (PtInRect(&canvas, point)) {
            state->draggingHotspot = true;
            SetCapture(window);
            SetCursorHotspotFromPoint(*state, point);
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (state->draggingHotspot && (wParam & MK_LBUTTON)) {
            SetCursorHotspotFromPoint(*state, {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (state->draggingHotspot) {
            state->draggingHotspot = false;
            ReleaseCapture();
            return 0;
        }
        break;
    case WM_MOUSEWHEEL: {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(window, &point);
        const RECT canvas = CursorCanvas(*state);
        if (PtInRect(&canvas, point)) {
            state->zoom = std::clamp(state->zoom *
                (GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? 1.18 : 1.0 / 1.18), .5, 8.0);
            const auto destination = CursorImageRect(*state, canvas);
            // 放大后让已选热点停在画布中心，便于继续微调。
            state->panX = (0.5 - state->hotspotX) * destination.Width;
            state->panY = (0.5 - state->hotspotY) * destination.Height;
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        if (id == kCursorChoose) {
            std::wstring selected;
            if (!ChooseCursorImage(window, selected)) return 0;
            std::wstring error;
            auto loaded = qp::LoadCustomCursorImage(selected, error);
            if (!loaded) {
                MessageBoxW(window, error.c_str(), L"QInEX Pointer", MB_OK | MB_ICONWARNING);
                return 0;
            }
            state->sourcePath = selected;
            state->image = std::move(loaded);
            state->hotspotX = state->hotspotY = .5;
            state->zoom = 1; state->panX = state->panY = 0;
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        if (id == kCursorCenter) {
            state->hotspotX = state->hotspotY = .5;
            state->zoom = 1; state->panX = state->panY = 0;
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        if (id == IDOK) { state->accepted = true; DestroyWindow(window); return 0; }
        if (id == IDCANCEL) { DestroyWindow(window); return 0; }
        break;
    }
    case WM_CLOSE: DestroyWindow(window); return 0;
    case WM_DESTROY:
        if (GetCapture() == window) ReleaseCapture();
        ReleaseCursorPaintResources(*state);
        ReleaseDialogFont(*state);
        state->window = nullptr;
        return 0;
    default: break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

ATOM RegisterCustomizationClass(HINSTANCE instance, const wchar_t* name, WNDPROC procedure,
        HBRUSH background) {
    WNDCLASSEXW cls{sizeof(cls)};
    cls.lpfnWndProc = procedure;
    cls.hInstance = instance;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hbrBackground = background;
    cls.lpszClassName = name;
    const ATOM atom = RegisterClassExW(&cls);
    return atom ? atom : (GetLastError() == ERROR_CLASS_ALREADY_EXISTS ? 1 : 0);
}

}  // namespace

bool Application::EditHotkeys() {
    HotkeyDialogState dialogState;
    dialogState.application = this;
    dialogState.bindings[0] = {settings.pauseHotkey, settings.pauseHotkeyModifiers};
    dialogState.bindings[1] = {settings.exitHotkey, settings.exitHotkeyModifiers};
    dialogState.bindings[2] = {settings.rangeHotkey, settings.rangeHotkeyModifiers};
    dialogState.bindings[3] = {settings.hudHotkey, settings.hudHotkeyModifiers};
    if (!RegisterCustomizationClass(instance, L"QPointerHotkeyDialog", HotkeyDialogProc,
            reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1))) return false;
    dialogState.window = CreateWindowExW(WS_EX_DLGMODALFRAME, L"QPointerHotkeyDialog",
        W(L"快捷键自定义", L"快捷键自定义喵"), WS_POPUP | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, DialogPx(main, 526), DialogPx(main, 335),
        main, nullptr, instance, &dialogState);
    if (!dialogState.window) return false;
    CenterDialog(dialogState.window, main);
    if (!RunModalWindow(main, dialogState)) return false;

    settings.pauseHotkey = dialogState.bindings[0].virtualKey;
    settings.pauseHotkeyModifiers = dialogState.bindings[0].modifiers;
    settings.exitHotkey = dialogState.bindings[1].virtualKey;
    settings.exitHotkeyModifiers = dialogState.bindings[1].modifiers;
    settings.rangeHotkey = dialogState.bindings[2].virtualKey;
    settings.rangeHotkeyModifiers = dialogState.bindings[2].modifiers;
    settings.hudHotkey = dialogState.bindings[3].virtualKey;
    settings.hudHotkeyModifiers = dialogState.bindings[3].modifiers;
    // 先释放整组旧绑定再注册新组合，允许用户在四个动作之间交换按键，
    // 也确保把暂停键设为关闭时旧的全局热键会真正注销。
    RefreshHotkeys();
    UpdateOutputHud(GetTickCount64());
    SubmitCapturePresentation();
    MarkDirty();
    std::vector<std::wstring> registrationFailures;
    const qp::HotkeyBinding requestedPause = qp::NormalizeHotkey(
        {settings.pauseHotkey, settings.pauseHotkeyModifiers});
    if (requestedPause.virtualKey && registeredPauseHotkey != requestedPause)
        registrationFailures.emplace_back(W(L"暂停 / 恢复", L"暂停 / 恢复喵"));
    if (captureWindow) {
        if (!captureExitHotkeyReady)
            registrationFailures.emplace_back(W(L"返回设置", L"返回设置喵"));
        if (settings.rangeHotkey && !captureRangeHotkeyReady)
            registrationFailures.emplace_back(W(L"调整范围", L"调整范围喵"));
        if (settings.hudHotkey && !outputHudHotkeyReady)
            registrationFailures.emplace_back(W(L"显示 / 隐藏 HUD", L"显示 / 隐藏 HUD 喵"));
    }
    if (registrationFailures.empty()) {
        notice = W(L"快捷键已应用；点击“保存”可保留到下次启动。",
            L"快捷键应用好啦，点保存就会记住喵。");
    } else {
        std::wstring names;
        for (const auto& name : registrationFailures) {
            if (!names.empty()) names += L"、";
            names += name;
        }
        notice = W(L"设置已应用，但以下全局快捷键被其他程序占用：",
            L"设置应用啦，但这些全局快捷键被别的程序占用了喵：") + names;
    }
    if (page == 4) ReloadPage(4);
    return true;
}

void Application::ReloadCustomCursor() {
    const std::wstring imagePath = storageDirectory + L"\\custom_cursor.png";
    const std::wstring hotspotPath = storageDirectory + L"\\custom_cursor.ini";
    std::wstring error;
    customCursorImage = qp::LoadCustomCursorImage(imagePath, error);
    const auto hotspot = qp::LoadCustomCursorHotspot(hotspotPath);
    customCursorHotspotX = hotspot.x;
    customCursorHotspotY = hotspot.y;
    cacheShape = -1;
    cacheCustomCursor = nullptr;
    if (!customCursorImage && settings.shape == 7 &&
            GetFileAttributesW(imagePath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        notice = W(L"自定义光标图片无法读取，当前先显示十字光标：",
            L"自定义光标图片读不了，先显示十字喵：") + error;
        statusAt = 0;
    }
    Render();
    SubmitCapturePresentation();
}

bool Application::EditCursorDraft(const std::wstring& title, std::wstring& source, double& hotspotX, double& hotspotY) {
    CursorDialogState dialogState;
    dialogState.application = this;
    std::wstring loadError;
    dialogState.image = qp::LoadCustomCursorImage(source, loadError);
    dialogState.sourcePath = source;
    dialogState.hotspotX = hotspotX;
    dialogState.hotspotY = hotspotY;
    if (!dialogState.image) {
        if (!ChooseCursorImage(main, dialogState.sourcePath)) return false;
        std::wstring error;
        dialogState.image = qp::LoadCustomCursorImage(dialogState.sourcePath, error);
        if (!dialogState.image) { Error(error); return false; }
        dialogState.hotspotX = dialogState.hotspotY = .5;
    }
    if (!RegisterCustomizationClass(instance, L"QPointerCursorDialog", CursorDialogProc,
            reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1))) return false;
    dialogState.window = CreateWindowExW(WS_EX_DLGMODALFRAME, L"QPointerCursorDialog",
        title.c_str(),
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
        DialogPx(main, 700), DialogPx(main, 660), main, nullptr, instance, &dialogState);
    if (!dialogState.window) return false;
    CenterDialog(dialogState.window, main);
    if (!RunModalWindow(main, dialogState)) return false;
    source = dialogState.sourcePath; hotspotX = dialogState.hotspotX; hotspotY = dialogState.hotspotY;
    return true;
}

bool Application::EditCustomCursor() {
    const std::wstring destination = storageDirectory + L"\\custom_cursor.png";
    std::wstring source = destination;
    double hotspotX = customCursorHotspotX, hotspotY = customCursorHotspotY;
    if (!EditCursorDraft(W(L"自定义光标热点", L"自定义光标热点喵"), source, hotspotX, hotspotY)) return false;

    std::wstring error;
    if (!qp::SaveCustomCursorFiles(source, destination,
            storageDirectory + L"\\custom_cursor.ini",
            {hotspotX, hotspotY}, error)) {
        Error(error); return false;
    }
    ReloadCustomCursor();
    if (!customCursorImage) {
        Error(error.empty() ? W(L"保存后的光标图片无法读取。",
            L"保存后的光标图片读不了喵。") : error);
        return false;
    }
    settings.shape = 7;
    cacheShape = -1;
    MarkDirty();
    notice = W(L"图片与热点已保存并应用；点击“保存”可保留样式和大小。",
        L"图片和热点保存并应用啦，点“保存”会记住样式和大小喵。");
    if (page == 4) ReloadPage(4);
    Render();
    SubmitCapturePresentation();
    return true;
}
