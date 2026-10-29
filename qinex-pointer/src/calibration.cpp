#include "app.h"

namespace {
const wchar_t* SlotName(int slot) {
    static const wchar_t* names[] = {L"左上", L"上中", L"右上", L"左中", L"中心", L"右中", L"左下", L"下中", L"右下", L"独立验收"};
    return names[std::clamp(slot, 0, 9)];
}
std::wstring SlotLabel(int slot) { return slot == 9 ? W(L"独立验收点", L"独立验收点喵") : std::to_wstring(slot + 1) + L" · " + SlotName(slot); }
std::wstring SessionKey(const Settings& settings) {
    return settings.sourceIp.empty() ? L"" : std::to_wstring(settings.source) + L"|" + settings.sourceIp + L"|" + std::to_wstring(settings.port);
}
double IntersectionArea(qp::Rect a, qp::Rect b) {
    return std::max(0.0, std::min(a.x + a.w, b.x + b.w) - std::max(a.x, b.x)) * std::max(0.0, std::min(a.y + a.h, b.y + b.h) - std::max(a.y, b.y));
}
bool SameSourceGeometry(const qp::Geometry& a, const qp::Geometry& b) {
    const auto closeEnough = [](double x, double y) {
        return std::abs(x - y) <= 1e-6 * std::max({1.0, std::abs(x), std::abs(y)});
    };
    const auto sameRect = [&](qp::Rect x, qp::Rect y) {
        return closeEnough(x.x, y.x) && closeEnough(x.y, y.y) && closeEnough(x.w, y.w) && closeEnough(x.h, y.h);
    };
    if (!sameRect(a.content, b.content) || !sameRect(a.crop, b.crop) ||
        a.rotation != b.rotation || a.mirrorX != b.mirrorX || a.mirrorY != b.mirrorY ||
        a.stretch != b.stretch) return false;
    if (a.stretch) return true;
    auto aspect = [](const qp::Geometry& g) {
        double width = g.frameWidth * g.crop.w, height = g.frameHeight * g.crop.h;
        if (g.rotation == 90 || g.rotation == 270) std::swap(width, height);
        return width / height;
    };
    const double x = aspect(a), y = aspect(b);
    return std::isfinite(x) && std::isfinite(y) && closeEnough(x, y);
}
LRESULT CALLBACK CalibrationPromptProc(HWND h, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_CREATE: {
        auto button = CreateWindowW(L"BUTTON", W(L"取消取样", L"取消取样喵"), WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            app->Px(290), app->Px(101), app->Px(105), app->Px(28), h, reinterpret_cast<HMENU>(1), app->instance, nullptr);
        SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(app->font), TRUE); return 0;
    }
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_COMMAND:
        if (LOWORD(wp) == 1) { app->CancelCalibration(); app->notice = W(L"已取消取样，已有采样和校准均已保留。", L"取样取消啦，已有成果保留着喵。"); return 0; } break;
    case WM_KEYDOWN: if (wp == VK_ESCAPE) { app->CancelCalibration(); return 0; } break;
    case WM_CLOSE: app->CancelCalibration(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PRINTCLIENT: app->PaintCalibrationPrompt(reinterpret_cast<HDC>(wp)); return 0;
    case WM_PAINT: { PAINTSTRUCT ps; auto dc = BeginPaint(h, &ps); app->PaintCalibrationPrompt(dc); EndPaint(h, &ps); return 0; }
    }
    return DefWindowProcW(h, message, wp, lp);
}
}

void Application::CalibrationControls(int p) {
    Label(W(L"采集卡 / 融合器：优先在全屏画面拖动边框，一次完成范围校准。", L"采集卡 / 融合器优先拖动边框，一次校准范围喵。"), 26, 96, 600, p, 28);
    const std::wstring rangeKey = qp::HotkeyText(
        {settings.rangeHotkey, settings.rangeHotkeyModifiers}, W(L"设置页按钮", L"设置页按钮喵"));
    const std::wstring rangeButton = W(L"快捷范围校准（", L"快捷范围校准喵（") + rangeKey + L"）";
    Button(ID_CAL_RANGE, rangeButton.c_str(), 635, 92, 199, p);
    Label(W(L"采集画面仍有局部偏差时，再用下方九点精调。", L"采集画面还有局部偏差时，再用九点精调喵。"), 26, 137, 808, p, 25);
    for (int i = 0; i < 9; ++i) Button(ID_CAL_GRID_FIRST + i, L"", 26 + (i % 3) * 92, 169 + (i / 3) * 47, 84, p);
    Add(L"STATIC", L"", SS_LEFT, ID_CAL_STATUS, 330, 171, 504, 124, p);
    Label(W(L"准备时间", L"准备时间喵"), 26, 323, 80, p); Edit(ID_CAPTURE_DELAY, L"5", 111, 321, 46, p);
    Label(W(L"秒（3～30）", L"秒（3～30）喵"), 167, 323, 140, p);
    Button(ID_CAPTURE, W(L"开始取样", L"开始取样喵"), 26, 359, 180, p);
    Button(ID_CAL_UNDO, W(L"撤销", L"撤销喵"), 220, 359, 110, p);
    Button(ID_FIT, W(L"计算并应用", L"计算并应用喵"), 344, 359, 170, p);
    Button(ID_CAL_MORE, W(L"更多…", L"更多喵…"), 528, 359, 118, p);
    Button(ID_CAL_INDEPENDENT, W(L"采验收点", L"采验收点喵"), 26, 397, 180, p);
    Button(ID_VERIFY, W(L"检查验收点", L"检查验收点喵"), 220, 397, 140, p);
    UpdateCalibrationText();
}

void Application::SyncCalibrationSamples() {
    if (calibrationSlots.size() == samples.size() && calibrationTokens.size() == samples.size()) return;
    calibrationSlots.clear(); calibrationTokens.clear(); fittedCalibrationTokens.clear(); calibrationErrors.clear(); calibrationUndo.clear(); calibrationFitToken = 0;
    for (size_t i = 0; i < samples.size(); ++i) { calibrationSlots.push_back(static_cast<int>(std::min<size_t>(i, 9))); calibrationTokens.push_back(nextCalibrationToken++); }
    calibrationSlot = 0; calibrationResult.clear();
}

void Application::ClearCalibrationSession() {
    CancelCalibration(false); samples.clear(); calibrationSlots.clear(); calibrationTokens.clear(); fittedCalibrationTokens.clear();
    calibrationErrors.clear(); calibrationUndo.clear(); fitSampleCount = 0; calibrationFitToken = 0; calibrationSlot = 0; calibrationResult.clear(); lastCalibrationDownMs = calibrationPressedSince = 0;
    UpdateCalibrationText();
}

void Application::ResetImportedCalibrationSession() {
    calibrationSessions.erase(SessionKey(settings));
    ClearCalibrationSession();
}

void Application::SwitchCalibrationSource(const Settings& previous, const Settings& next) {
    SyncCalibrationSamples(); CancelCalibration(false);
    auto oldKey = SessionKey(previous), newKey = SessionKey(next);
    if (!oldKey.empty()) calibrationSessions[oldKey] = {{samples, calibrationSlots, calibrationTokens, fittedCalibrationTokens, previous.geometry.calibration, fitSampleCount, calibrationFitToken, calibrationSlot, calibrationResult}, calibrationUndo};
    ClearCalibrationSession();
    auto found = calibrationSessions.find(newKey);
    if (!newKey.empty() && found != calibrationSessions.end()) {
        const auto& saved = found->second; const auto& c = saved.current;
        samples = c.samples; calibrationSlots = c.slots; calibrationTokens = c.tokens; fittedCalibrationTokens = c.fittedTokens;
        fitSampleCount = c.fitCount; calibrationFitToken = c.fitToken; calibrationSlot = c.slot; calibrationResult = c.result; calibrationUndo = saved.undo;
    }
    UpdateCalibrationText();
}

void Application::PushCalibrationUndo() {
    SyncCalibrationSamples(); if (calibrationUndo.size() >= 32) calibrationUndo.erase(calibrationUndo.begin());
    calibrationUndo.push_back({samples, calibrationSlots, calibrationTokens, fittedCalibrationTokens, settings.geometry.calibration, fitSampleCount, calibrationFitToken, calibrationSlot, calibrationResult});
}

void Application::UndoCalibration() {
    SyncCalibrationSamples(); if (calibrationUndo.empty()) return;
    CancelCalibration(); auto previous = std::move(calibrationUndo.back()); calibrationUndo.pop_back();
    samples = std::move(previous.samples); calibrationSlots = std::move(previous.slots); calibrationTokens = std::move(previous.tokens);
    fittedCalibrationTokens = std::move(previous.fittedTokens); settings.geometry.calibration = previous.calibration;
    fitSampleCount = previous.fitCount; calibrationFitToken = previous.fitToken; calibrationSlot = previous.slot; calibrationResult = std::move(previous.result); calibrationErrors.clear();
    delayed.clear(); MarkDirty(); UpdateCalibrationText(); notice = W(L"已撤销上一步，采样和校准已恢复。", L"上一步撤销啦，采样和校准恢复好喵。");
}

bool Application::HandleCalibrationCommand(int id) {
    if (id >= ID_CAL_GRID_FIRST && id <= ID_CAL_GRID_LAST) { calibrationSlot = id - ID_CAL_GRID_FIRST; UpdateCalibrationText(); return true; }
    switch (id) {
    case ID_CAL_LIST: { int row = static_cast<int>(SendMessageW(Item(ID_CAL_LIST), LB_GETCURSEL, 0, 0)); if (row >= 0 && row <= 9) { calibrationSlot = row; UpdateCalibrationText(); } return true; }
    case ID_CAPTURE: BeginCapture(); return true;
    case ID_CAL_INDEPENDENT: calibrationSlot = 9; UpdateCalibrationText(); BeginCapture(); return true;
    case ID_CAL_CANCEL: CancelCalibration(); return true;
    case ID_CAL_UNDO: UndoCalibration(); return true;
    case ID_CLEAR:
        if (!samples.empty()) { PushCalibrationUndo(); samples.clear(); calibrationSlots.clear(); calibrationTokens.clear(); calibrationErrors.clear(); calibrationSlot = 0; calibrationResult = W(L"本轮采样已清空；已应用的校准保持有效。可撤销。", L"本轮采样清空啦，校准仍有效，可撤销喵。"); UpdateCalibrationText(); } return true;
    case ID_RESET:
        PushCalibrationUndo(); settings.geometry.calibration = {}; fitSampleCount = 0; calibrationFitToken = 0; fittedCalibrationTokens.clear(); calibrationErrors.clear();
        calibrationResult = W(L"已恢复默认校准，可撤销。采样已保留，可重新计算。", L"默认校准恢复啦，可撤销；采样留着，可重新计算喵。");
        delayed.clear(); MarkDirty(); UpdateCalibrationText(); return true;
    case ID_FIT: Fit(); return true;
    case ID_VERIFY: Verify(); return true;
    case ID_CAL_DETAIL: calibrationDetails = !calibrationDetails; UpdateCalibrationText(); return true;
    case ID_CAL_RANGE: BeginCaptureRangeAdjustment(); return true;
    case ID_CAL_MORE: {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, 1, W(L"清空本轮采样", L"清空本轮采样喵"));
        AppendMenuW(menu, MF_STRING, 2, W(L"恢复默认校准", L"恢复默认校准喵"));
        AppendMenuW(menu, MF_STRING, 3, calibrationDetails ? W(L"收起计算详情", L"收起计算详情喵") : W(L"显示计算详情", L"显示计算详情喵"));
        RECT anchor{}; GetWindowRect(Item(ID_CAL_MORE), &anchor);
        const int selected = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
            anchor.left, anchor.bottom, 0, main, nullptr);
        DestroyMenu(menu);
        if (selected == 1) HandleCalibrationCommand(ID_CLEAR);
        else if (selected == 2) HandleCalibrationCommand(ID_RESET);
        else if (selected == 3) HandleCalibrationCommand(ID_CAL_DETAIL);
        return true;
    }
    }
    return false;
}

void Application::ObserveCalibrationPacket(const qp::Packet& packet, uint64_t receivedMs) {
    // 心跳只能证明连接存活，不能让旧的按下状态重新变得新鲜。
    if (packet.kind == qp::PacketKind::Heartbeat) return;
    if (packet.kind == qp::PacketKind::Hide || (packet.show && !*packet.show) || (packet.down && !*packet.down)) lastCalibrationDownMs = calibrationPressedSince = 0;
    else if (packet.down && *packet.down) {
        if (!lastCalibrationDownMs || Age(receivedMs, lastCalibrationDownMs) >= static_cast<uint64_t>(std::min(settings.timeoutMs, 1200))) calibrationPressedSince = receivedMs;
        lastCalibrationDownMs = receivedMs;
    }
}

bool Application::CalibrationFresh(uint64_t now) const {
    uint64_t limit = static_cast<uint64_t>(std::min(settings.timeoutMs, 1200));
    return running && !demoActive && state.hasPosition && state.show && state.down && !settings.sourceIp.empty() && sender == settings.sourceIp &&
        state.lastPacketMs && lastCalibrationDownMs && Age(now, state.lastPacketMs) < limit && Age(now, lastCalibrationDownMs) < limit;
}

void Application::BeginCapture() {
    if (demoActive) { Error(W(L"请先结束演示，再使用真实设备取样。", L"先结束演示，再用真实设备取样喵。")); return; }
    if (IsFusionDisplayMode(settings.mode)) {
        BeginCaptureRangeAdjustment();
        return;
    }
    if (!running) { Error(W(L"请先连接设备并收到真实光标，再开始取样。", L"先连接设备收到真实光标，再取样喵。")); return; }
    if (snapshot || calibrationPhase != CapturePhase::Idle) return;
    if (HasDraft(1) || HasDraft(2)) { Error(W(L"设备或画面还有未应用的修改。请先在对应页面应用，再开始取样。", L"设备或画面还有修改没应用喵，先应用再取样。")); return; }
    if (settings.sourceIp.empty()) { Error(W(L"请先在设备页选择“使用这台设备”，绑定已收到数据的设备，再开始取样。", L"先在设备页点“使用这台设备”，绑定好来源再取样喵。")); return; }
    uint64_t now = GetTickCount64();
    if (!state.hasPosition || !state.lastPacketMs || Age(now, state.lastPacketMs) >= static_cast<uint64_t>(std::min(settings.timeoutMs, 1200)) || sender != settings.sourceIp) {
        Error(W(L"当前没有这台设备的新鲜光标。请先移动设备鼠标，确认收到数据。", L"还没收到这台设备的新鲜光标喵，先移动设备鼠标确认数据。")); return;
    }
    int seconds = 5;
    try { auto s = Text(Item(ID_CAPTURE_DELAY)); size_t n; seconds = std::stoi(s, &n); if (n != s.size() || seconds < 3 || seconds > 30) throw 0; }
    catch (...) { SetFocus(Item(ID_CAPTURE_DELAY)); Error(W(L"准备时间请填写 3～30 秒。", L"准备时间填 3～30 秒喵。")); return; }
    if (settings.mode == 3 && !captureOutput.Active()) {
        Error(W(L"请先在“画面”页启动采集，再开始校准。", L"先到“画面”页启动采集，再校准喵。")); return;
    }
    SyncCalibrationSamples();
    qp::Geometry g; if (!Geometry(g)) { Error(W(L"预览目前不可见。请打开目标窗口后再取样。", L"预览现在看不见，打开目标窗口再取样喵。")); return; }
    if (!samples.empty() && !SameSourceGeometry(samples.front().geometry, g)) { Error(W(L"画面参数与本轮采样不同。请先清空本轮采样，再重新取样；清空后仍可撤销。", L"画面参数变了喵，先清空本轮采样再重采；清空可撤销。")); return; }
    snapshotSlot = calibrationSlot; calibrationPhase = CapturePhase::Preparing;
    captureDue = now + static_cast<uint64_t>(seconds) * 1000; calibrationDeadline = captureDue + 45000; lastMove = now; stableAnchor = state.position;
    calibrationCaptureFrameBaseline = 0; calibrationCaptureStableSince = 0;
    notice = W(L"取样准备中。按提示保持真实触点稳定，截图完成后再释放转发。", L"取样准备中喵，停稳真实触点，截图完成后再释放转发。");
    ShowCalibrationPrompt(); ShowWindow(hostWindow ? hostWindow : main, SW_MINIMIZE);
    if (settings.mode == 2 && IsWindow(targetWindow)) SetForegroundWindow(targetWindow);
    else if (settings.mode == 3) {
        captureFullscreenRequested = true; captureCreateRetryAt = 0; captureHintUntil = 0;
        FocusCaptureFullscreen();
        if (calibrationPrompt) SetWindowPos(calibrationPrompt, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    TickCalibration(now);
}

void Application::ShowCalibrationPrompt() {
    if (calibrationPrompt) return;
    WNDCLASSW cls{}; cls.hInstance = instance; cls.lpfnWndProc = CalibrationPromptProc; cls.lpszClassName = L"QPointerCalibrationPrompt";
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.hbrBackground = GetSysColorBrush(COLOR_WINDOW); RegisterClassW(&cls);
    qp::Geometry g; Geometry(g); int w = Px(420), h = Px(145);
    qp::Rect best{g.target.x + g.target.w - w - 12, g.target.y + 12, double(w), double(h)}; double bestOverlap = 1e300;
    for (auto& monitor : monitors) {
        RECT r = monitor.info.rcWork;
        for (int corner = 0; corner < 4; ++corner) {
            qp::Rect candidate{double(corner % 2 ? r.right - w - 12 : r.left + 12), double(corner / 2 ? r.bottom - h - 12 : r.top + 12), double(w), double(h)};
            double overlap = IntersectionArea(candidate, g.target); if (overlap < bestOverlap) { best = candidate; bestOverlap = overlap; }
        }
    }
    calibrationPrompt = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, cls.lpszClassName, W(L"校准取样进度", L"校准取样进度喵"),
        WS_POPUP | WS_BORDER, static_cast<int>(best.x), static_cast<int>(best.y), w, h, nullptr, nullptr, instance, nullptr);
    if (calibrationPrompt) ShowWindow(calibrationPrompt, SW_SHOWNOACTIVATE);
}

void Application::UpdateCalibrationPrompt(const std::wstring& title, const std::wstring& body) {
    if (calibrationPromptTitle == title && calibrationPromptBody == body) return;
    calibrationPromptTitle = title; calibrationPromptBody = body; if (calibrationPrompt) InvalidateRect(calibrationPrompt, nullptr, FALSE);
}

void Application::PaintCalibrationPrompt(HDC dc) {
    RECT r{}; GetClientRect(calibrationPrompt, &r); FillRect(dc, &r, GetSysColorBrush(COLOR_WINDOW)); SetBkMode(dc, TRANSPARENT);
    auto old = SelectObject(dc, font); SetTextColor(dc, RGB(0, 95, 115)); RECT title{Px(14), Px(10), r.right - Px(14), Px(38)};
    DrawTextW(dc, calibrationPromptTitle.c_str(), -1, &title, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    SetTextColor(dc, RGB(40, 40, 40)); RECT body{Px(14), Px(43), r.right - Px(14), Px(96)}; DrawTextW(dc, calibrationPromptBody.c_str(), -1, &body, DT_LEFT | DT_WORDBREAK);
    RECT hint{Px(14), Px(101), Px(275), Px(139)};
    const std::wstring cancelHint = IsCaptureDisplayMode(settings.mode)
        ? qp::HotkeyText({settings.exitHotkey, settings.exitHotkeyModifiers}) +
            W(L" 取消并返回设置", L" 取消并返回设置喵")
        : W(L"Esc 取消", L"Esc 取消喵");
    DrawTextW(dc, cancelHint.c_str(), -1, &hint, DT_LEFT | DT_WORDBREAK); SelectObject(dc, old);
}

void Application::TickCalibration(uint64_t now) {
    if (calibrationPhase == CapturePhase::Idle || calibrationPhase == CapturePhase::Marking) return;
    if (!running || demoActive) { CancelCalibration(); notice = W(L"设备连接已变化，本次取样取消；已有成果保留。", L"设备连接变了喵，本次取样取消，已有成果保留。"); return; }
    if (now >= calibrationDeadline) { CancelCalibration(); notice = W(L"取样等待超时。请确认设备数据和视频触点后重试；已有成果已保留。", L"取样等超时啦，确认设备数据和视频触点再试喵，成果还在。"); return; }
    if (calibrationPhase == CapturePhase::Preparing) {
        if (now < captureDue) {
            UpdateCalibrationPrompt(W(L"准备 · ", L"准备喵 · ") + SlotLabel(snapshotSlot) + L" · " + std::to_wstring((captureDue - now + 999) / 1000) + W(L" 秒", L" 秒喵"),
                W(L"将真实触点移到对应区域，按住并让视频画面稳定。截图完成后再释放转发。", L"把真实触点移到对应区域，按住并停稳视频画面喵。截图完成再释放转发。")); return;
        }
        calibrationPhase = CapturePhase::Waiting; captureDue = 0;
    }
    if (calibrationPhase == CapturePhase::Hiding) { if (now >= captureDue) { captureDue = 0; FinishCapture(); } return; }
    if (calibrationPhase == CapturePhase::Stable) {
        bool videoFresh = true;
        if (settings.mode == 3) {
            auto frame = presentedCaptureFrame; auto status = captureOutput.Status();
            videoFresh = frame && status.state == qp::CaptureState::Running && Age(now, frame->capturedMs) <= 1000;
        }
        if (!CalibrationFresh(now) || Age(now, lastMove) < 800 || Age(now, calibrationPressedSince) < 800 || !videoFresh) {
            calibrationPhase = CapturePhase::Waiting; calibrationCaptureFrameBaseline = 0; calibrationCaptureStableSince = 0;
        }
        else {
            if (now < captureDue) return;
            calibrationPhase = CapturePhase::Hiding; captureWaiting = true; captureDue = now + 160;
            ShowWindow(overlay, SW_HIDE); if (calibrationPrompt) ShowWindow(calibrationPrompt, SW_HIDE); DwmFlush(); return;
        }
    }
    uint64_t remain = (calibrationDeadline - now + 999) / 1000; qp::Geometry geometry; std::wstring body;
    auto captureFrame = settings.mode == 3 ? presentedCaptureFrame : nullptr;
    auto captureStatus = settings.mode == 3 ? captureOutput.Status() : qp::CaptureStatus{};
    if (settings.mode == 3 && (!captureFrame || captureStatus.state != qp::CaptureState::Running || Age(now, captureFrame->capturedMs) > 1000))
        body = W(L"等待采集画面更新。请检查 HDMI 信号或采集卡连接。", L"等待采集画面更新喵，请检查 HDMI 信号或采集卡连接。");
    else if (!Geometry(geometry, true)) body = W(L"等待预览可见。请恢复目标窗口。", L"等待预览出现喵，请恢复目标窗口。");
    else if (!state.lastPacketMs || Age(now, state.lastPacketMs) >= 1200 || sender != settings.sourceIp) body = W(L"等待已绑定设备的新数据。请检查设备连接。", L"等待绑定设备的新数据喵，请检查连接。");
    else if (!CalibrationFresh(now)) body = W(L"请按住真实触点。需要设备发来新的按下状态。", L"请按住真实触点喵，等待设备新的按下状态。");
    else if (Age(now, lastMove) < 800 || Age(now, calibrationPressedSince) < 800) body = W(L"已按下，等待位置停稳。请同时确认视频中的真实触点停稳。", L"按下啦，等待位置停稳喵，也要确认视频里的真实触点停稳。");
    else {
        UpdateCalibrationPrompt(W(L"设备位置已停稳 · 准备截图", L"设备位置停稳啦 · 准备截图喵"), W(L"继续保持真实画面稳定。听到提示音、看到截图后即可释放转发。", L"继续保持真实画面稳定喵。听到提示音、看到截图就能释放转发。"));
        calibrationPhase = CapturePhase::Stable; captureDue = now + 500;
        calibrationCaptureFrameBaseline = captureFrame ? captureFrame->serial : 0; calibrationCaptureStableSince = now; return;
    }
    UpdateCalibrationPrompt(W(L"等待取样条件 · ", L"等待取样条件喵 · ") + std::to_wstring(remain) + W(L" 秒后取消", L" 秒后取消喵"), body);
}

void Application::CancelCalibration(bool restoreMain) {
    captureDue = 0; captureWaiting = false; calibrationDeadline = 0; calibrationPhase = CapturePhase::Idle;
    calibrationCaptureFrameBaseline = 0; calibrationCaptureStableSince = 0;
    if (calibrationPrompt) { HWND h = calibrationPrompt; calibrationPrompt = nullptr; DestroyWindow(h); }
    if (snapshot) { HWND h = snapshot; snapshot = nullptr; DestroyWindow(h); }
    if (captured) { DeleteObject(captured); captured = nullptr; }
    marked.reset(); magnifierPoint.reset(); imageRect = {};
    calibrationPromptTitle.clear(); calibrationPromptBody.clear(); calibrationLinkControl = 0;
    if (restoreMain && IsWindow(main)) ShowControlWindow();
}

void Application::FinishCapture() {
    captureWaiting = false; uint64_t now = GetTickCount64(); qp::Geometry g;
    bool captureFrameReady = true;
    if (settings.mode == 3) {
        auto frame = presentedCaptureFrame; auto status = captureOutput.Status();
        captureFrameReady = frame && status.state == qp::CaptureState::Running && Age(now, frame->capturedMs) <= 1000 &&
            frame->serial > calibrationCaptureFrameBaseline && frame->capturedMs >= calibrationCaptureStableSince;
    }
    if (!CalibrationFresh(now) || Age(now, lastMove) < 800 || Age(now, calibrationPressedSince) < 800 || !captureFrameReady || !Geometry(g, true)) {
        if (calibrationDeadline && now < calibrationDeadline) {
            calibrationPhase = CapturePhase::Waiting; ShowCalibrationPrompt(); if (calibrationPrompt) ShowWindow(calibrationPrompt, SW_SHOWNOACTIVATE);
            UpdateCalibrationPrompt(W(L"条件发生变化 · 继续等待", L"条件变了喵 · 继续等待"),
                settings.mode == 3 && !captureFrameReady
                    ? W(L"正在等待停稳后到达的新视频帧，请继续按住。", L"正在等停稳后到达的新视频帧喵，请继续按住。")
                    : W(L"继续按住，让设备位置和真实视频触点保持稳定。", L"继续按住喵，让设备位置和真实视频触点稳定。")); return;
        }
        CancelCalibration(); Error(W(L"未取样：需要已绑定设备的新鲜按下状态，并让真实视频触点停稳。", L"还没取样喵，需要绑定设备的新鲜按下状态和停稳的真实视频触点。")); return;
    }
    const auto& r = g.target;
    RECT desktop{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
    if (r.x < desktop.left || r.y < desktop.top || r.x + r.w > desktop.right || r.y + r.h > desktop.bottom) { CancelCalibration(); Error(W(L"取样画面需完整位于桌面内。请将预览窗口移回屏幕。", L"取样画面要完整在桌面里喵，请把预览移回屏幕。")); return; }
    snapshotInput = state.position; snapshotGeometry = g;
    captured = settings.mode == 3 ? SnapshotCaptureOutput() : CaptureRect(g.target);
    if (!captured) { CancelCalibration(); Error(W(L"截图失败或画面超过 2000 万像素，请缩小预览后重试。", L"截图失败或画面太大啦，缩小预览后再试喵。")); return; }
    marked.reset(); magnifierPoint.reset(); imageRect = {}; calibrationPhase = CapturePhase::Marking;
    if (calibrationPrompt) { DestroyWindow(calibrationPrompt); calibrationPrompt = nullptr; }
    calibrationLinkControl = ReleaseLinkInput();
    const wchar_t* snapshotTitle = calibrationLinkControl > 0
        ? W(L"截图已冻结 · 输入已切回电脑", L"截图冻好啦 · 输入已切回电脑喵")
        : calibrationLinkControl < 0
            ? W(L"截图已冻结 · 请先按 QInEX-Link 收放键", L"截图冻好啦 · 请先按 Link 收放键喵")
            : W(L"截图已冻结 · 标记真实触点", L"截图冻好啦 · 标记真实触点喵");
    POINT center{static_cast<LONG>(std::lround(g.target.x + g.target.w / 2)),
        static_cast<LONG>(std::lround(g.target.y + g.target.h / 2))};
    MONITORINFO work{sizeof(work)}; GetMonitorInfoW(MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST), &work);
    const int workWidth = work.rcWork.right - work.rcWork.left, workHeight = work.rcWork.bottom - work.rcWork.top;
    const int snapshotWidth = std::min(Px(1040), std::max(320, workWidth - Px(24)));
    const int snapshotHeight = std::min(Px(690), std::max(260, workHeight - Px(24)));
    const int snapshotX = work.rcWork.left + std::max(0, (workWidth - snapshotWidth) / 2);
    const int snapshotY = work.rcWork.top + std::max(0, (workHeight - snapshotHeight) / 2);
    snapshot = CreateWindowExW(WS_EX_TOPMOST, L"QPointerSnapshot", snapshotTitle, WS_OVERLAPPEDWINDOW,
        snapshotX, snapshotY, snapshotWidth, snapshotHeight, nullptr, nullptr, instance, nullptr);
    if (!snapshot) { CancelCalibration(); return; }
    if (captureWindow) SetWindowPos(captureWindow, HWND_NOTOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    MessageBeep(MB_OK); ShowWindow(snapshot, SW_SHOW); UpdateWindow(snapshot); SetForegroundWindow(snapshot);
}

void Application::PaintSnapshot(HDC dc) {
    RECT c{}; GetClientRect(snapshot, &c); FillRect(dc, &c, GetSysColorBrush(COLOR_BTNFACE)); SetBkMode(dc, TRANSPARENT); HGDIOBJ old = SelectObject(dc, font);
    RECT text{Px(15), Px(8), c.right - Px(15), Px(48)};
    const wchar_t* instruction = calibrationLinkControl > 0
        ? W(L"点击截图中的真实触点，再确认。方向键可微调 1 像素。", L"点截图里的真实触点再确认喵，方向键可微调 1 像素。")
        : calibrationLinkControl < 0
            ? W(L"先按 QInEX-Link 收放键，再点击真实触点并确认。", L"先按 Link 收放键，再点真实触点确认喵。")
            : W(L"点击截图中的真实触点，再确认。", L"点截图里的真实触点再确认喵。");
    DrawTextW(dc, instruction, -1, &text, DT_LEFT | DT_WORDBREAK);
    if (captured) {
        BITMAP b{}; GetObjectW(captured, sizeof(b), &b);
        double side = std::min<double>(Px(220), std::max<double>(Px(100), c.right * .26));
        double availableWidth = std::max(2.0, c.right - side - Px(45.0));
        double s = std::min(availableWidth / b.bmWidth, std::max<LONG>(2, c.bottom - Px(112)) / double(b.bmHeight));
        imageRect = {Px(15) + (availableWidth - b.bmWidth * s) / 2, double(Px(52)), b.bmWidth * s, b.bmHeight * s};
        Gdiplus::Bitmap bitmap(captured, nullptr); Gdiplus::Graphics graphics(dc); graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        graphics.DrawImage(&bitmap, Gdiplus::RectF(static_cast<float>(imageRect.x), static_cast<float>(imageRect.y), static_cast<float>(imageRect.w), static_cast<float>(imageRect.h)));
        Gdiplus::Pen pen(Gdiplus::Color(255, 250, 45, 80), 2.0f);
        if (marked) {
            float x = static_cast<float>(imageRect.x + marked->x * (imageRect.w - 1)), y = static_cast<float>(imageRect.y + marked->y * (imageRect.h - 1));
            graphics.DrawEllipse(&pen, x - 9, y - 9, 18.0f, 18.0f); graphics.DrawLine(&pen, x - 13, y, x + 13, y); graphics.DrawLine(&pen, x, y - 13, x, y + 13);
        }
        auto focus = marked ? marked : magnifierPoint; double lx = c.right - side - Px(15), ly = Px(84);
        Gdiplus::SolidBrush background(Gdiplus::Color(255, 32, 36, 39)); graphics.FillRectangle(&background, static_cast<float>(lx), static_cast<float>(ly), static_cast<float>(side), static_cast<float>(side));
        if (focus) {
            double cx = focus->x * (b.bmWidth - 1), cy = focus->y * (b.bmHeight - 1), sourceSide = side / magnifierZoom;
            graphics.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor); graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
            graphics.DrawImage(&bitmap, Gdiplus::RectF(static_cast<float>(lx), static_cast<float>(ly), static_cast<float>(side), static_cast<float>(side)),
                static_cast<float>(cx - sourceSide / 2), static_cast<float>(cy - sourceSide / 2), static_cast<float>(sourceSide), static_cast<float>(sourceSide), Gdiplus::UnitPixel);
            graphics.DrawLine(&pen, static_cast<float>(lx + side / 2 - 12), static_cast<float>(ly + side / 2), static_cast<float>(lx + side / 2 + 12), static_cast<float>(ly + side / 2));
            graphics.DrawLine(&pen, static_cast<float>(lx + side / 2), static_cast<float>(ly + side / 2 - 12), static_cast<float>(lx + side / 2), static_cast<float>(ly + side / 2 + 12));
        }
        RECT detail{static_cast<LONG>(lx), static_cast<LONG>(ly + side + Px(12)), c.right - Px(12), c.bottom - Px(54)};
        std::wstring help = SlotLabel(snapshotSlot) + L"\r\n\r\n" + (marked ? W(L"已标记：方向键微调\r\n再次点击可重新定位。", L"标记好啦，方向键微调喵。\r\n再点一下可重新定位。") : W(L"将鼠标移到图上查看细节；点击标记真实触点。", L"鼠标移到图上看细节喵；点击标记真实触点。"));
        if (focus) help += std::wstring(L"\r\n\r\n") + W(L"原图像素：", L"原图像素喵：") + Num(std::round(focus->x * (b.bmWidth - 1))) + L", " + Num(std::round(focus->y * (b.bmHeight - 1)));
        DrawTextW(dc, help.c_str(), -1, &detail, DT_LEFT | DT_WORDBREAK);
    }
    SelectObject(dc, old);
}

void Application::MoveSampleMark(int dx, int dy) {
    if (!captured || !marked) return; BITMAP b{}; GetObjectW(captured, sizeof(b), &b);
    marked->x = std::clamp(std::round(marked->x * (b.bmWidth - 1)) + dx, 0.0, double(b.bmWidth - 1)) / (b.bmWidth - 1);
    marked->y = std::clamp(std::round(marked->y * (b.bmHeight - 1)) + dy, 0.0, double(b.bmHeight - 1)) / (b.bmHeight - 1);
    InvalidateRect(snapshot, nullptr, FALSE);
}

void Application::AcceptSample() {
    if (!snapshot || !captured) return;
    if (!marked) {
        SetWindowTextW(GetDlgItem(snapshot, 1), W(L"请先点击真实触点", L"先点真实触点喵"));
        MessageBeep(MB_ICONWARNING); return;
    }
    BITMAP b{}; GetObjectW(captured, sizeof(b), &b);
    qp::Point screen{snapshotGeometry.target.x + marked->x * (snapshotGeometry.target.w - 1),
                     snapshotGeometry.target.y + marked->y * (snapshotGeometry.target.h - 1)};
    auto observed = qp::UnmapPoint(snapshotGeometry, screen);
    if (!observed) { MessageBoxW(snapshot, W(L"这里在有效手机画面之外。请点击视频中的真实触点。", L"这里在手机画面外啦，请点视频里的真实触点喵。"), L"QInEX Pointer", MB_OK | MB_ICONWARNING); return; }
    PushCalibrationUndo(); auto found = std::find(calibrationSlots.begin(), calibrationSlots.end(), snapshotSlot); Sample sample{snapshotInput, *observed, snapshotGeometry, screen};
    if (found != calibrationSlots.end()) { size_t index = static_cast<size_t>(found - calibrationSlots.begin()); samples[index] = sample; calibrationTokens[index] = nextCalibrationToken++; }
    else { samples.push_back(sample); calibrationSlots.push_back(snapshotSlot); calibrationTokens.push_back(nextCalibrationToken++); }
    calibrationErrors.clear(); calibrationResult = SlotLabel(snapshotSlot) + W(L"已记录。", L"记录好啦喵。");
    int nextSlot = 9; for (int i = 0; i < 9; ++i) if (std::find(calibrationSlots.begin(), calibrationSlots.end(), i) == calibrationSlots.end()) { nextSlot = i; break; }
    calibrationSlot = snapshotSlot == 9 ? 9 : nextSlot; CancelCalibration(); UpdateCalibrationText();
    notice = snapshotSlot == 9 ? W(L"独立验收点已记录，请点击“检查验收点”。", L"验收点记录好啦，点“检查验收点”喵。") : W(L"采样已记录，可选择任意点重采。九点齐全后计算校准。", L"采样记录好啦，任意点都能重采，九点齐后计算校准喵。");
}

void Application::UpdateCalibrationText() {
    SyncCalibrationSamples(); size_t training = 0;
    if (Has(ID_CAL_LIST)) SendMessageW(Item(ID_CAL_LIST), LB_RESETCONTENT, 0, 0);
    for (int slot = 0; slot <= 9; ++slot) {
        auto found = std::find(calibrationSlots.begin(), calibrationSlots.end(), slot); bool present = found != calibrationSlots.end();
        size_t index = static_cast<size_t>(found - calibrationSlots.begin()); if (slot < 9 && present) ++training;
        std::wstring line = SlotLabel(slot) + (present ? W(L"    已采样", L"    采好啦喵") : W(L"    待采样", L"    等取样喵"));
        if (present && index < calibrationErrors.size() && calibrationErrors[index] >= 0) line += L"    " + Num(calibrationErrors[index]) + L" px";
        if (Has(ID_CAL_LIST)) SendMessageW(Item(ID_CAL_LIST), LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line.c_str()));
        if (slot < 9 && Has(ID_CAL_GRID_FIRST + slot)) {
            std::wstring button = std::to_wstring(slot + 1) + L" " + SlotName(slot) + (present ? L" ✓" : L""); Set(ID_CAL_GRID_FIRST + slot, button);
            SendMessageW(Item(ID_CAL_GRID_FIRST + slot), BM_SETSTYLE, calibrationSlot == slot ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON, TRUE);
        }
    }
    if (Has(ID_CAL_LIST)) SendMessageW(Item(ID_CAL_LIST), LB_SETCURSEL, calibrationSlot, 0);
    bool present = std::find(calibrationSlots.begin(), calibrationSlots.end(), calibrationSlot) != calibrationSlots.end();
    if (Has(ID_CAPTURE)) Set(ID_CAPTURE, (present ? W(L"重采：", L"重采喵：") : W(L"取样：", L"取样喵：")) + SlotLabel(calibrationSlot));
    if (IsFusionDisplayMode(settings.mode)) {
        for (int id = ID_CAL_GRID_FIRST; id <= ID_CAL_GRID_LAST; ++id) if (Has(id)) Enable(id, false);
        const int fusionDisabled[] = {ID_CAPTURE, ID_CAL_INDEPENDENT, ID_CAL_UNDO, ID_CLEAR,
            ID_RESET, ID_FIT, ID_VERIFY, ID_CAL_MORE, ID_CAL_DETAIL};
        for (const int id : fusionDisabled)
            if (Has(id)) Enable(id, false);
        if (Has(ID_CAL_RANGE)) Enable(ID_CAL_RANGE, true);
        if (Has(ID_CAL_STATUS)) {
            const std::wstring rangeKey = qp::HotkeyText(
                {settings.rangeHotkey, settings.rangeHotkeyModifiers}, W(L"页面按钮", L"页面按钮喵"));
            Set(ID_CAL_STATUS, W(L"融合输出只需快捷范围校准。\r\n使用 ", L"融合输出只需快捷范围校准喵。\r\n使用 ") +
                rangeKey + W(L" 后，对照融合器最终画面拖动或缩放边框；Enter 应用，Esc 取消。",
                    L" 后对照融合器最终画面拖动边框，Enter 应用，Esc 取消。"));
        }
        return;
    }
    for (int id = ID_CAL_GRID_FIRST; id <= ID_CAL_GRID_LAST; ++id) if (Has(id)) Enable(id, true);
    const int samplingControls[] = {ID_CAPTURE, ID_CAL_INDEPENDENT, ID_CAL_MORE, ID_VERIFY};
    for (const int id : samplingControls)
        if (Has(id)) Enable(id, true);
    if (Has(ID_CAL_UNDO)) Enable(ID_CAL_UNDO, !calibrationUndo.empty());
    if (Has(ID_CAL_RANGE)) Enable(ID_CAL_RANGE, IsCaptureDisplayMode(settings.mode) && captureOutput.Active());
    if (Has(ID_CLEAR)) Enable(ID_CLEAR, !samples.empty()); if (Has(ID_FIT)) Enable(ID_FIT, training == 9);
    if (Has(ID_CAL_DETAIL)) Set(ID_CAL_DETAIL, calibrationDetails ? W(L"收起计算详情", L"收起计算详情喵") : W(L"显示计算详情", L"显示计算详情喵"));
    if (!Has(ID_CAL_STATUS)) return;
    std::wstring text = W(L"进度：", L"进度喵：") + std::to_wstring(training) + L" / 9\r\n" + calibrationResult;
    if (calibrationDetails) {
        const auto& a = settings.geometry.calibration;
        text += L"\r\nx = " + Num(a.a) + L" nx + " + Num(a.b) + L" ny + " + Num(a.c) + L"    y = " + Num(a.d) + L" nx + " + Num(a.e) + L" ny + " + Num(a.f);
    } else if (calibrationResult.empty()) text += W(L"选择一个位置开始。边缘点离手机边缘约一成。", L"选一个位置开始喵，边缘点离手机边缘约一成。");
    Set(ID_CAL_STATUS, text);
}

void Application::Fit() {
    SyncCalibrationSamples(); std::vector<qp::Point> input, observed; std::vector<size_t> indices;
    qp::Geometry current;
    if (!Geometry(current)) {
        Error(W(L"当前画面暂时不可用。请恢复采集画面后再计算校准。", L"当前画面暂时不可用喵，恢复采集画面后再计算校准。")); return;
    }
    if (!samples.empty() && !SameSourceGeometry(samples.front().geometry, current)) {
        Error(W(L"画面参数已变化。请先清空本轮采样并重采；清空可撤销。", L"画面参数变了喵，先清空本轮采样并重采；清空可撤销。")); return;
    }
    for (int slot = 0; slot < 9; ++slot) {
        auto it = std::find(calibrationSlots.begin(), calibrationSlots.end(), slot);
        if (it == calibrationSlots.end()) { Error(W(L"请补齐九宫格中的待采样点，再计算校准。", L"先补齐九宫格的待采样点，再计算喵。")); return; }
        size_t i = static_cast<size_t>(it - calibrationSlots.begin()); input.push_back(samples[i].input); observed.push_back(samples[i].observed); indices.push_back(i);
    }
    qp::Affine fitted; double rms = 0, maxError = 0; std::string error;
    if (!qp::FitAffine(input, observed, fitted, rms, maxError, error)) { Error(W(L"校准计算未完成：", L"校准还没算好喵：") + Wide(error)); return; }
    double worstPixels = 0; int worstSlot = 0; calibrationErrors.assign(samples.size(), -1);
    for (auto i : indices) {
        const auto& s = samples[i]; auto g = s.geometry; g.calibration = fitted; auto prediction = qp::MapPoint(g, s.input);
        if (!prediction) {
            calibrationSlot = calibrationSlots[i]; calibrationResult = W(L"本次未应用：", L"这次还没应用喵：") + SlotLabel(calibrationSlot) + W(L"落在画面外。请核对画面范围并重采。", L"落在画面外啦，核对范围并重采喵。");
            UpdateCalibrationText(); notice = calibrationResult; return;
        }
        double pixels = std::hypot(prediction->x - s.screen.x, prediction->y - s.screen.y); calibrationErrors[i] = pixels;
        if (pixels > worstPixels) { worstPixels = pixels; worstSlot = calibrationSlots[i]; }
    }
    if (worstPixels > 10) {
        calibrationSlot = worstSlot; calibrationResult = W(L"本次未应用：最大误差 ", L"这次还没应用喵：最大误差 ") + Num(worstPixels) + L" px。" + W(L"建议先重采", L"建议先重采喵：") + SlotLabel(worstSlot);
        UpdateCalibrationText(); notice = calibrationResult; return;
    }
    PushCalibrationUndo(); settings.geometry.calibration = fitted; fittedCalibrationTokens.clear();
    for (auto i : indices) fittedCalibrationTokens.push_back(calibrationTokens[i]);
    calibrationFitToken = nextCalibrationToken++; fitSampleCount = 9; delayed.clear(); MarkDirty();
    calibrationResult = W(L"已应用：采样最大误差 ", L"应用啦：采样最大误差喵 ") + Num(worstPixels) + W(L" px。请采独立验收点，满意后保存。", L" px。再采独立验收点，满意后保存喵。");
    calibrationSlot = 9; UpdateCalibrationText(); notice = calibrationResult;
}

void Application::Verify() {
    SyncCalibrationSamples(); auto it = std::find(calibrationSlots.begin(), calibrationSlots.end(), 9);
    if (it == calibrationSlots.end()) { Error(W(L"请点击“采一个独立验收点”。它不会参与九点校准计算。", L"先点“采一个独立验收点”喵，它不参与九点校准计算。")); return; }
    size_t index = static_cast<size_t>(it - calibrationSlots.begin());
    qp::Geometry current;
    if (!Geometry(current)) {
        Error(W(L"当前画面暂时不可用。请恢复采集画面后再检查验收点。", L"当前画面暂时不可用喵，恢复采集画面后再验收。")); return;
    }
    if (!SameSourceGeometry(samples[index].geometry, current)) { Error(W(L"画面参数已变化，请在当前画面重新采一个独立验收点。", L"画面参数变了喵，请在当前画面重新采验收点。")); return; }
    if (calibrationTokens[index] <= calibrationFitToken || std::find(fittedCalibrationTokens.begin(), fittedCalibrationTokens.end(), calibrationTokens[index]) != fittedCalibrationTokens.end()) {
        Error(W(L"请在本次计算之后重新采一个独立验收点。", L"请在本次计算之后重新采验收点喵。")); return;
    }
    const auto& s = samples[index]; auto g = s.geometry; g.calibration = settings.geometry.calibration; auto p = qp::MapPoint(g, s.input);
    if (!p) { Error(W(L"验收点落在画面外，请检查当前校准和画面范围。", L"验收点跑到画面外啦，检查校准和画面范围喵。")); return; }
    double error = std::hypot(p->x - s.screen.x, p->y - s.screen.y);
    if (calibrationErrors.size() != samples.size()) calibrationErrors.assign(samples.size(), -1); calibrationErrors[index] = error;
    calibrationResult = W(L"独立验收误差：", L"独立验收误差喵：") + Num(error) + L" px。" +
        (error > 10 ? W(L"偏差仍大，请核对画面范围和标记后再校准。", L"偏差还大喵，核对画面和标记再校准。") : W(L"请再检查边缘位置和移动轨迹，满意后保存。", L"再看看边缘和移动轨迹，满意后保存喵。"));
    calibrationSlot = 9; UpdateCalibrationText(); notice = calibrationResult;
}

LRESULT CALLBACK SnapshotProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        auto add = [&](int id, const wchar_t* label, DWORD style) {
            auto child = CreateWindowW(L"BUTTON", label, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style, 0, 0, 150, 32, h, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), app->instance, nullptr);
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(app->font), TRUE);
        };
        add(1, W(L"先点击真实触点", L"先点真实触点喵"), BS_DEFPUSHBUTTON); add(2, W(L"取消本次取样", L"取消这次取样喵"), BS_PUSHBUTTON);
        add(3, L"4×", BS_AUTORADIOBUTTON | WS_GROUP); add(4, L"8×", BS_AUTORADIOBUTTON);
        EnableWindow(GetDlgItem(h, 1), FALSE);
        SendMessageW(GetDlgItem(h, app->magnifierZoom == 8 ? 4 : 3), BM_SETCHECK, BST_CHECKED, 0); return 0;
    }
    case WM_GETMINMAXINFO: {
        auto info = reinterpret_cast<MINMAXINFO*>(lp);
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &monitor);
        const int width = monitor.rcWork.right - monitor.rcWork.left;
        const int height = monitor.rcWork.bottom - monitor.rcWork.top;
        info->ptMinTrackSize = {std::min(app->Px(690), width), std::min(app->Px(500), height)};
        info->ptMaxTrackSize = {width, height};
        return 0;
    }
    case WM_SIZE: {
        RECT r{}; GetClientRect(h, &r); MoveWindow(GetDlgItem(h, 1), app->Px(20), r.bottom - app->Px(44), app->Px(170), app->Px(30), TRUE);
        MoveWindow(GetDlgItem(h, 2), app->Px(205), r.bottom - app->Px(44), app->Px(165), app->Px(30), TRUE);
        int side = static_cast<int>(std::min<double>(app->Px(220), std::max<double>(app->Px(100), r.right * .26)));
        MoveWindow(GetDlgItem(h, 3), r.right - side - app->Px(15), app->Px(52), app->Px(85), app->Px(28), TRUE);
        MoveWindow(GetDlgItem(h, 4), r.right - side + app->Px(75), app->Px(52), app->Px(85), app->Px(28), TRUE); InvalidateRect(h, nullptr, FALSE); return 0;
    }
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN: {
        auto r = app->imageRect; double x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (r.w > 1 && r.h > 1 && x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h && app->captured) {
            BITMAP b{}; GetObjectW(app->captured, sizeof(b), &b);
            qp::Point point{std::round(std::clamp((x - r.x) / (r.w - 1), 0.0, 1.0) * (b.bmWidth - 1)) / (b.bmWidth - 1), std::round(std::clamp((y - r.y) / (r.h - 1), 0.0, 1.0) * (b.bmHeight - 1)) / (b.bmHeight - 1)};
            app->magnifierPoint = point;
            if (msg == WM_LBUTTONDOWN) {
                app->marked = point;
                HWND confirm = GetDlgItem(h, 1);
                EnableWindow(confirm, TRUE);
                SetWindowTextW(confirm, W(L"确认并记录", L"确认并记录喵"));
                SetFocus(h);
            }
            InvalidateRect(h, nullptr, FALSE);
        } return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == 1) { app->AcceptSample(); return 0; }
        if (LOWORD(wp) == 2) { SendMessageW(h, WM_CLOSE, 0, 0); return 0; }
        if (LOWORD(wp) == 3 || LOWORD(wp) == 4) { app->magnifierZoom = LOWORD(wp) == 3 ? 4 : 8; CheckRadioButton(h, 3, 4, LOWORD(wp)); SetFocus(h); InvalidateRect(h, nullptr, FALSE); return 0; } break;
    case WM_KEYDOWN:
        if (wp == VK_RETURN) { app->AcceptSample(); return 0; } if (wp == VK_ESCAPE) { SendMessageW(h, WM_CLOSE, 0, 0); return 0; }
        if (wp == VK_LEFT || wp == VK_RIGHT || wp == VK_UP || wp == VK_DOWN) { app->MoveSampleMark(wp == VK_LEFT ? -1 : wp == VK_RIGHT ? 1 : 0, wp == VK_UP ? -1 : wp == VK_DOWN ? 1 : 0); return 0; } break;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); app->PaintSnapshot(dc); EndPaint(h, &ps); return 0; }
    case WM_PRINTCLIENT: app->PaintSnapshot(reinterpret_cast<HDC>(wp)); return 0;
    case WM_CLOSE: app->CancelCalibration(); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
