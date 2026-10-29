#include "app.h"
#include "pointer_runtime.h"

namespace {
constexpr UINT kTrayMessage = WM_APP + 11;
constexpr const wchar_t* kPauseHotkeyProperty = L"QInEX.Pointer.Hotkey.Pause.v1";
constexpr const wchar_t* kExitHotkeyProperty = L"QInEX.Pointer.Hotkey.Exit.v1";
constexpr const wchar_t* kRangeHotkeyProperty = L"QInEX.Pointer.Hotkey.Range.v1";
constexpr const wchar_t* kHudHotkeyProperty = L"QInEX.Pointer.Hotkey.Hud.v1";

qp::HotkeyBinding PauseHotkey(const Settings& settings) {
    return {settings.pauseHotkey, settings.pauseHotkeyModifiers};
}

qp::HotkeyBinding ExitHotkey(const Settings& settings) {
    return {settings.exitHotkey, settings.exitHotkeyModifiers};
}

qp::HotkeyBinding RangeHotkey(const Settings& settings) {
    return {settings.rangeHotkey, settings.rangeHotkeyModifiers};
}

qp::HotkeyBinding HudHotkey(const Settings& settings) {
    return {settings.hudHotkey, settings.hudHotkeyModifiers};
}

bool RegisterBinding(HWND window, int id, qp::HotkeyBinding binding) {
    binding = qp::NormalizeHotkey(binding);
    return binding.virtualKey != 0 && RegisterHotKey(window, id,
        static_cast<UINT>(binding.modifiers) | MOD_NOREPEAT,
        static_cast<UINT>(binding.virtualKey)) != FALSE;
}

std::optional<std::wstring> ChooseScenePath(HWND owner, bool save) {
    wchar_t path[32768]{};
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = owner; dialog.lpstrFile = path; dialog.nMaxFile = 32768;
    dialog.lpstrFilter = L"QInEX Pointer 场景 (*.ini)\0*.ini\0\0";
    dialog.lpstrDefExt = L"ini";
    dialog.lpstrTitle = save ? W(L"保存当前设备与画面场景", L"保存当前场景喵") : W(L"打开设备与画面场景", L"打开场景喵");
    dialog.Flags = OFN_NOCHANGEDIR | OFN_EXPLORER | OFN_PATHMUSTEXIST | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog)) return std::wstring(path);
    return std::nullopt;
}
}

void Application::UpdateTray(bool force) {
    if (hostWindow || shuttingDown || !main) return;
    const std::wstring tip = L"QInEX Pointer · " + (paused ? std::wstring(W(L"已暂停显示", L"暂停显示喵")) : deviceStatus);
    if (!force && trayInstalled && trayTooltip == tip) return;
    NOTIFYICONDATAW data{sizeof(data)}; data.hWnd = main; data.uID = 1;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data.uCallbackMessage = kTrayMessage;
    data.hIcon = applicationSmallIcon;
    wcsncpy_s(data.szTip, tip.c_str(), _TRUNCATE);
    if (trayInstalled && Shell_NotifyIconW(NIM_MODIFY, &data)) {
        trayTooltip = tip;
        return;
    }
    // Explorer 重建托盘后旧图标已失效；修改失败时重新安装并恢复键盘操作语义。
    trayInstalled = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
    trayVersion4 = false;
    if (trayInstalled) {
        data.uVersion = NOTIFYICON_VERSION_4;
        trayVersion4 = Shell_NotifyIconW(NIM_SETVERSION, &data) != FALSE;
        trayTooltip = tip;
    } else if (!IsWindowVisible(main)) {
        // 托盘不可用时保留可见入口，不能让用户找不到仍在后台运行的程序。
        ShowControlWindow();
    }
}

void Application::HideToTray() {
    if (hostWindow) {
        // 统一入口仍由 Link 的主窗口和托盘提供；保留草稿、接收及全屏输出。
        ShowWindow(main, SW_HIDE);
        SendMessageW(hostWindow, qp::desktop::kShowDeviceMessage, 0, 0);
        return;
    }
    UpdateTray(true);
    if (!trayInstalled) {
        Error(W(L"暂时无法显示托盘图标，请稍后重试。窗口已保留。",
            L"暂时放不进托盘喵，请稍后重试，窗口先保留着。"));
        return;
    }
    // 只隐藏控制窗口；不销毁控件和草稿，也不停止接收、采集或独立全屏输出。
    ShowWindow(main, SW_HIDE);
    if (!trayHintShown) {
        NOTIFYICONDATAW data{sizeof(data)}; data.hWnd = main; data.uID = 1;
        data.uFlags = NIF_INFO;
        data.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND | NIIF_RESPECT_QUIET_TIME;
        wcscpy_s(data.szInfoTitle, L"QInEX Pointer");
        wcscpy_s(data.szInfo, W(
            L"已在后台运行。点击托盘图标恢复窗口，右键选择“退出程序”结束运行。",
            L"已经在后台运行啦。点托盘图标回来，右键选“退出程序”结束喵。"));
        Shell_NotifyIconW(NIM_MODIFY, &data);
        trayHintShown = true;
    }
}

void Application::RegisterPauseHotkey() {
    if (!main) return;
    const qp::HotkeyBinding requested = qp::NormalizeHotkey(PauseHotkey(settings));
    if (registeredPauseHotkey == requested) return;
    UnregisterHotKey(main, 1);
    registeredPauseHotkey = {};
    if (!requested.virtualKey) return;
    if (RegisterBinding(main, 1, requested)) registeredPauseHotkey = requested;
    else notice = W(L"暂停快捷键已被其他程序占用；请在设置中更换，或使用托盘暂停。",
        L"暂停快捷键被别的程序占用啦，请换一个或用托盘暂停喵。");
}

void Application::PublishHotkeys() {
    if (!main) return;
    const auto publish = [&](const wchar_t* name, qp::HotkeyBinding binding) {
        SetPropW(main, name, reinterpret_cast<HANDLE>(qp::PackHotkeyProperty(binding)));
    };
    publish(kPauseHotkeyProperty, PauseHotkey(settings));
    publish(kExitHotkeyProperty, ExitHotkey(settings));
    publish(kRangeHotkeyProperty, RangeHotkey(settings));
    publish(kHudHotkeyProperty, HudHotkey(settings));
}

void Application::RefreshOutputHotkeys() {
    if (!main) return;
    UnregisterHotKey(main, 3);
    UnregisterHotKey(main, 4);
    UnregisterHotKey(main, 5);
    captureExitHotkeyReady = false;
    captureRangeHotkeyReady = false;
    outputHudHotkeyReady = false;
    if (captureWindow) {
        captureExitHotkeyReady = RegisterBinding(main, 3, ExitHotkey(settings));
        captureRangeHotkeyReady = RegisterBinding(main, 4, RangeHotkey(settings));
        outputHudHotkeyReady = RegisterBinding(main, 5, HudHotkey(settings));
    }
    PublishHotkeys();
}

void Application::RefreshHotkeys() {
    // 一次性释放整组旧绑定后再注册，支持在动作之间交换组合，也确保关闭的按键
    // 不会因缓存提前返回而继续留在系统全局热键表中。
    UnregisterHotKey(main, 1);
    UnregisterHotKey(main, 3);
    UnregisterHotKey(main, 4);
    UnregisterHotKey(main, 5);
    registeredPauseHotkey = {};
    captureExitHotkeyReady = false;
    captureRangeHotkeyReady = false;
    outputHudHotkeyReady = false;
    RegisterPauseHotkey();
    RefreshOutputHotkeys();
}

void Application::SaveScene() {
    if (HasDraft()) { Error(W(L"请先应用当前修改，再保存为场景。", L"先应用当前修改，再保存场景喵。")); return; }
    auto path = ChooseScenePath(main, true); if (!path) return;
    if (!SaveSettings(settings, *path)) { Error(W(L"场景保存失败，请检查文件夹是否可写。", L"场景保存失败，请检查文件夹能否写入喵。")); return; }
    notice = W(L"场景已保存，可通过“打开场景”恢复。", L"场景保存好啦，用“打开场景”就能恢复喵。"); statusAt = 0;
}

void Application::LoadScene() {
    if (HasDraft()) { Error(W(L"请先应用或还原当前修改，再打开场景。", L"先应用或还原当前修改，再打开场景喵。")); return; }
    auto path = ChooseScenePath(main, false); if (!path) return;
    if (ReadIni(L"source", L"", *path).empty() || ReadIni(L"frameW", L"", *path).empty()) {
        Error(W(L"所选文件不是完整的 QInEX Pointer 场景。", L"这个文件不是完整的 Pointer 场景喵。")); return;
    }
    Settings loaded = LoadSettings(*path);
    CancelCalibration(true);
    restoringScene = true; AdoptSettings(loaded); restoringScene = false;
    ResetImportedCalibrationSession();
    targetWindow = nullptr; Inventory(); ReloadControls(); MarkDirty();
    notice = W(L"场景已打开，请复核目标预览窗口和真实触点。", L"场景打开啦，请复核预览窗口和真实触点喵。"); statusAt = 0;
}

bool Application::CanExit() {
    if (dirty || HasDraft()) {
        ShowControlWindow();
        const int answer = MessageBoxW(main,
            W(L"还有未保存的修改。是否保留窗口继续处理？\n选择“否”将退出，本次未保存修改不会写入配置。", L"还有修改没保存喵。是否留下继续处理？\n选择“否”会退出，本次修改不会保存喵。"),
            L"QInEX Pointer", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON1);
        if (answer != IDNO) return false;
    }
    return true;
}

void Application::RequestExit() {
    if (hostWindow) { PostMessageW(hostWindow, qp::desktop::kExitHostMessage, 0, 0); return; }
    if (CanExit()) DestroyWindow(main);
}

void Application::ReturnToHost() {
    if (!hostWindow) { SelectPage(0); return; }
    HideToTray();
    if (IsWindow(captureWindow)) SetWindowPos(captureWindow, HWND_NOTOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    ShowWindow(hostWindow, SW_RESTORE);
    SetForegroundWindow(hostWindow);
}

LRESULT CALLBACK DemoProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CLOSE) { app->EndDemo(); return 0; }
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) { app->EndDemo(); return 0; }
    if (msg == WM_PAINT || msg == WM_PRINTCLIENT) {
        PAINTSTRUCT ps{}; HDC dc = msg == WM_PAINT ? BeginPaint(h, &ps) : reinterpret_cast<HDC>(wp); RECT r{}; GetClientRect(h, &r);
        HBRUSH background = CreateSolidBrush(RGB(238, 245, 251)); FillRect(dc, &r, background); DeleteObject(background);
        HGDIOBJ old = SelectObject(dc, app->font); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(22, 53, 75));
        RECT title{20, 14, r.right - 20, 58};
        DrawTextW(dc, W(L"观察圆周移动与橙色按下反馈。15 秒后自动结束，Esc 可提前结束。", L"看看圆周移动和橙色按下反馈喵。15 秒后结束，Esc 可提前结束。"), -1, &title, DT_WORDBREAK);
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(201, 218, 230)); HGDIOBJ oldPen = SelectObject(dc, pen);
        for (int x = 20; x < r.right - 20; x += 32) { MoveToEx(dc, x, 66, nullptr); LineTo(dc, x, r.bottom - 26); }
        for (int y = 66; y < r.bottom - 26; y += 32) { MoveToEx(dc, 20, y, nullptr); LineTo(dc, r.right - 20, y); }
        SelectObject(dc, oldPen); DeleteObject(pen); SelectObject(dc, old); if (msg == WM_PAINT) EndPaint(h, &ps); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void Application::CheckAlignment() {
    if (alignmentCheck) { DestroyWindow(alignmentCheck); alignmentCheck = nullptr; return; }
    Settings next; if (!ReadPage(next, 2)) return; AdoptSettings(next); CommitDraft(2);
    qp::Geometry g; if (!Geometry(g)) { Error(W(L"先选择可见的预览区域，再检查对齐。", L"先选可见的预览区域，再检查对齐喵。")); return; }
    ShowWindow(overlay, SW_HIDE);
    alignmentCheck = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED, L"QPointerAlignment",
        W(L"画面位置检查", L"画面位置检查喵"), WS_POPUP,
        static_cast<int>(std::round(g.target.x)), static_cast<int>(std::round(g.target.y)),
        static_cast<int>(std::round(g.target.w)), static_cast<int>(std::round(g.target.h)), main, nullptr, instance, nullptr);
    if (!alignmentCheck) return;
    SetLayeredWindowAttributes(alignmentCheck, RGB(1, 2, 3), 255, LWA_COLORKEY);
    ShowWindow(alignmentCheck, SW_SHOW); SetForegroundWindow(alignmentCheck);
    notice = W(L"标记用于检查画面范围，不会向手机点击。点击标记层或按 Esc 结束。", L"标记只检查画面，不会点击手机喵。点击或 Esc 结束。"); statusAt = 0;
}

LRESULT CALLBACK AlignmentProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CLOSE || msg == WM_LBUTTONDOWN || (msg == WM_KEYDOWN && (wp == VK_ESCAPE || wp == VK_RETURN))) {
        app->alignmentCheck = nullptr; DestroyWindow(h); app->statusAt = 0; return 0;
    }
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps{}; HDC dc = BeginPaint(h, &ps); RECT client{}; GetClientRect(h, &client);
        HBRUSH background = CreateSolidBrush(RGB(1, 2, 3)); FillRect(dc, &client, background); DeleteObject(background);
        qp::Geometry g;
        if (app->Geometry(g)) {
            RECT bounds{}; GetWindowRect(h, &bounds);
            Gdiplus::Graphics graphics(dc); graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::Pen outline(Gdiplus::Color(255, 10, 30, 40), 5.0f), line(Gdiplus::Color(255, 0, 225, 225), 2.0f);
            const qp::Point points[] = {{0, 0}, {1, 0}, {.5, .5}, {0, 1}, {1, 1}};
            for (auto input : points) if (auto p = qp::MapPoint(g, input)) {
                float x = static_cast<float>(p->x - bounds.left), y = static_cast<float>(p->y - bounds.top);
                for (auto* pen : {&outline, &line}) { graphics.DrawLine(pen, x - 13, y, x + 13, y); graphics.DrawLine(pen, x, y - 13, x, y + 13); }
            }
        }
        EndPaint(h, &ps); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
