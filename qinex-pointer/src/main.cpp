#include "app.h"
#include <shobjidl.h>
#include "output_hud.h"
#include "resource.h"
#include "pointer_runtime.h"
#include "glass_theme.h"

Application* app;

std::wstring CaptureStatusTextWithTerminalError(const qp::CaptureStatus& status,
    bool active, std::wstring configuredText);

namespace {
constexpr UINT kCaptureFrameMessage = WM_APP + 30;
constexpr UINT kPointerDatagramMessage = WM_APP + 31;
constexpr int kRangeMove = 1;
constexpr int kRangeLeft = 1 << 1;
constexpr int kRangeRight = 1 << 2;
constexpr int kRangeTop = 1 << 3;
constexpr int kRangeBottom = 1 << 4;

std::wstring ColorHex(COLORREF color) {
    wchar_t text[8]{};
    swprintf_s(text, L"#%02X%02X%02X", GetRValue(color), GetGValue(color), GetBValue(color));
    return text;
}

std::wstring Metric1(double value) {
    wchar_t text[32]{};
    swprintf_s(text, L"%.1f", value);
    return text;
}

const wchar_t* CaptureBackendHud(qp::CaptureBackend backend) {
    switch (backend) {
    case qp::CaptureBackend::MediaFoundation: return L"Media Foundation";
    case qp::CaptureBackend::DirectShow: return L"DirectShow";
    default: return L"";
    }
}

bool PaintDecodedFrame(HDC dc, const RECT& client, const std::shared_ptr<const qp::CaptureFrame>& frame,
                       const qp::Geometry& screenGeometry) {
    HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(dc, &client, black); DeleteObject(black);
    if (!frame || frame->width < 2 || frame->height < 2 || frame->stride < frame->width * 4 || frame->bgra.empty()) return false;

    qp::Geometry local = screenGeometry;
    local.target = {0, 0, double(client.right - client.left), double(client.bottom - client.top)};
    local.frameWidth = frame->width; local.frameHeight = frame->height;
    // 让原始裁剪矩形的三个角走与光标完全相同的 C/O/D 变换，视频和热点因此共用同一坐标路径。
    qp::Geometry imageGeometry = local;
    imageGeometry.content = local.crop;
    imageGeometry.calibration = {};
    auto topLeft = qp::MapPoint(imageGeometry, {0, 0});
    auto topRight = qp::MapPoint(imageGeometry, {1, 0});
    auto bottomLeft = qp::MapPoint(imageGeometry, {0, 1});
    if (!topLeft || !topRight || !bottomLeft) return false;

    const auto& crop = local.crop;
    const float sourceX = static_cast<float>(crop.x * frame->width);
    const float sourceY = static_cast<float>(crop.y * frame->height);
    const float sourceWidth = static_cast<float>(crop.w * frame->width);
    const float sourceHeight = static_cast<float>(crop.h * frame->height);
    if (local.rotation == 0 && !local.mirrorX && !local.mirrorY &&
        std::abs(crop.x) < 1e-12 && std::abs(crop.y) < 1e-12 &&
        std::abs(crop.w - 1) < 1e-12 && std::abs(crop.h - 1) < 1e-12) {
        BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = frame->width; info.bmiHeader.biHeight = -frame->height;
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
        const int x = static_cast<int>(std::lround(topLeft->x)), y = static_cast<int>(std::lround(topLeft->y));
        const int right = static_cast<int>(std::lround(topRight->x));
        const int bottom = static_cast<int>(std::lround(bottomLeft->y));
        const int width = std::max(1, right - x + 1);
        const int height = std::max(1, bottom - y + 1);
        const int oldMode = SetStretchBltMode(dc, COLORONCOLOR);
        const int rows = StretchDIBits(dc, x, y, width, height, 0, 0, frame->width, frame->height,
            frame->bgra.data(), &info, DIB_RGB_COLORS, SRCCOPY);
        SetStretchBltMode(dc, oldMode);
        return rows != GDI_ERROR;
    }
    Gdiplus::Bitmap bitmap(frame->width, frame->height, frame->stride, PixelFormat32bppRGB,
        const_cast<BYTE*>(reinterpret_cast<const BYTE*>(frame->bgra.data())));
    Gdiplus::Graphics graphics(dc);
    graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighSpeed);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeBilinear);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    double ux = topRight->x - topLeft->x, uy = topRight->y - topLeft->y;
    double vx = bottomLeft->x - topLeft->x, vy = bottomLeft->y - topLeft->y;
    const double uLength = std::hypot(ux, uy), vLength = std::hypot(vx, vy);
    if (uLength < 1e-9 || vLength < 1e-9) return false;
    ux /= uLength; uy /= uLength; vx /= vLength; vy /= vLength;
    // MapPoint 给出首尾像素中心；DrawImage 需要外边界。补齐半像素后，视频四边与指针端点一致。
    Gdiplus::PointF destination[] = {
        {static_cast<float>(topLeft->x - .5 * ux - .5 * vx + .5),
         static_cast<float>(topLeft->y - .5 * uy - .5 * vy + .5)},
        {static_cast<float>(topRight->x + .5 * ux - .5 * vx + .5),
         static_cast<float>(topRight->y + .5 * uy - .5 * vy + .5)},
        {static_cast<float>(bottomLeft->x - .5 * ux + .5 * vx + .5),
         static_cast<float>(bottomLeft->y - .5 * uy + .5 * vy + .5)}
    };
    Gdiplus::ImageAttributes attributes;
    attributes.SetWrapMode(Gdiplus::WrapModeTileFlipXY);
    return graphics.DrawImage(&bitmap, destination, 3, sourceX, sourceY, sourceWidth, sourceHeight,
        Gdiplus::UnitPixel, &attributes) == Gdiplus::Ok;
}

const wchar_t* CaptureStateLabel(qp::CaptureState state) {
    switch (state) {
    case qp::CaptureState::Starting: return W(L"正在打开采集卡", L"正在打开采集卡喵");
    case qp::CaptureState::Running: return W(L"采集画面正常", L"采集画面正常喵");
    case qp::CaptureState::WaitingSignal: return W(L"等待 HDMI 信号或视频帧", L"等待 HDMI 信号或视频帧喵");
    case qp::CaptureState::Reconnecting: return W(L"采集已中断，正在重连", L"采集中断啦，正在重连喵");
    case qp::CaptureState::Error: return W(L"采集卡打开失败", L"采集卡打开失败喵");
    default: return W(L"采集尚未启动", L"采集还没启动喵");
    }
}
}

void Application::AdoptSettings(const Settings& requested) {
    const Settings previous = settings;
    const bool sourceChanged = !SameSource(previous, requested);
    Settings next = requested;
    bool wasRunning = running;
    if (sourceChanged) {
        profiles.Remember(previous);
        if (!restoringScene) {
            if (auto stored = profiles.Find(requested)) next = *stored;
            else {
                next.geometry.calibration = {};
                next.fusionCalibration = {};
                next.setupComplete = false;
            }
        }
        // 明确选择的新来源优先，已知设备恢复自己的画面与校准。
        next.source = requested.source; next.port = requested.port; next.sourceIp = requested.sourceIp;
        if (wasRunning) Stop();
    }
    // 快捷键属于程序级偏好，切换设备档案或导入场景时保持当前绑定。
    if (sourceChanged || restoringScene) {
        next.pauseHotkey = previous.pauseHotkey;
        next.pauseHotkeyModifiers = previous.pauseHotkeyModifiers;
        next.exitHotkey = previous.exitHotkey;
        next.exitHotkeyModifiers = previous.exitHotkeyModifiers;
        next.rangeHotkey = previous.rangeHotkey;
        next.rangeHotkeyModifiers = previous.rangeHotkeyModifiers;
        next.hudHotkey = previous.hudHotkey;
        next.hudHotkeyModifiers = previous.hudHotkeyModifiers;
    }
    settings = next;
    const bool captureSourceChanged =
        !CaptureDeviceIdsEqual(previous.captureDeviceId, settings.captureDeviceId) ||
        previous.captureModeKey != settings.captureModeKey ||
        previous.captureModeSelection != settings.captureModeSelection ||
        previous.captureFpsPreference != settings.captureFpsPreference;
    const bool displayModeChanged = previous.mode != settings.mode;
    const bool outputMonitorChanged = IsCaptureDisplayMode(settings.mode)
        ? previous.captureMonitorId != settings.captureMonitorId
        : IsFusionDisplayMode(settings.mode) && previous.fusionMonitorId != settings.fusionMonitorId;
    const bool outputPolicyChanged = IsCaptureDisplayMode(settings.mode)
        ? previous.capturePresentMode != settings.capturePresentMode
        : IsFusionDisplayMode(settings.mode) && previous.fusionPresentMode != settings.fusionPresentMode;
    if (captureOutput.Active() && (!IsCaptureDisplayMode(settings.mode) || captureSourceChanged)) {
        StopCaptureOutput();
        captureRuntimeNotice = IsCaptureDisplayMode(settings.mode)
            ? W(L"采集设备已更改，请重新开始全屏输出。", L"采集设备换啦，请重新开始全屏输出喵。")
            : L"";
    } else if ((captureWindow || captureFullscreenRequested) && displayModeChanged) {
        ExitCaptureFullscreen(false);
        captureRuntimeNotice.clear();
    } else if (captureWindow && outputPolicyChanged) {
        ExitCaptureFullscreen(false);
        captureFullscreenRequested = true;
        captureCreateRetryAt = 0;
        captureHintUntil = IsCaptureDisplayMode(settings.mode) ? GetTickCount64() + 8000 : 0;
        captureRuntimeNotice = W(L"正在按新的呈现方式重建全屏输出。", L"正在按新的呈现方式重建全屏输出喵。");
    } else if (captureWindow && outputMonitorChanged) {
        PositionCaptureOutput();
    } else if (captureWindow && IsFusionDisplayMode(settings.mode) &&
            previous.fusionBackgroundColor != settings.fusionBackgroundColor) {
        SubmitCapturePresentation();
        if (!capturePresenter.Active()) InvalidateRect(captureWindow, nullptr, FALSE);
    }
    if (!sourceChanged && settings.mode == 2 && Has(ID_WINDOW)) {
        int index = Selected(ID_WINDOW);
        if (index >= 0 && index < static_cast<int>(windows.size()) && windows[index].title == settings.windowTitle) targetWindow = windows[index].hwnd;
    }
    if (sourceChanged) {
        SwitchCalibrationSource(previous, settings);
        targetWindow = nullptr; Inventory(); ReloadControls();
        notice = W(L"设备已切换，已保留其他设备的设置。", L"设备换好啦，其他设备的设置都保留着喵。");
        if (wasRunning) Start();
    }
    if (!SameSettings(previous, settings)) MarkDirty();
    RefreshHotkeys();
    UpdateCalibrationText();
}

void Application::Apply(bool save) {
    if (save) {
        if (HasDraft()) { Error(W(L"还有未应用的设置，请先在对应页面点击应用或下一步。已使用的配置尚未被覆盖。", L"还有没应用的设置喵，先应用再保存吧。")); return; }
        profiles.Remember(settings);
        std::wstring error;
        if (!profiles.Save(storageDirectory + L"\\profiles", error) || !SaveSettings(settings, storageDirectory + L"\\pointer.ini")) {
            Error(W(L"保存未完成，请把程序放在可写目录后重试。", L"保存没完成喵，请放到可写目录再试。") + error); return;
        }
        dirty = false; notice = W(L"已保存，下次可继续使用。", L"保存好啦，下次接着用喵。"); statusAt = 0; return;
    }
    Settings next; if (!ReadPage(next, page)) return;
    bool sourceChanged = !SameSource(settings, next);
    const bool delayChanged = next.delayMs != settings.delayMs;
    notice = W(L"设置已应用。", L"设置应用好啦喵。");
    AdoptSettings(next);
    if (delayChanged) delayed.clear();
    if (!sourceChanged) CommitDraft(page);
    statusAt = 0;
}

void Application::ApplyAppearance() {
    const int shape = Selected(ID_SHAPE);
    int size = settings.size;
    try { auto text = Text(Item(ID_SIZE)); size_t n = 0; int parsed = std::stoi(text, &n); if (n == text.size() && parsed >= 12 && parsed <= 144) size = parsed; }
    catch (...) {}
    if (shape >= 0 && shape <= 7 && (settings.shape != shape || settings.size != size)) {
        settings.shape = shape; settings.size = size; cacheShape = -1; MarkDirty();
    }
    Render();
}

bool Application::Start() {
    if (running) return true;
    std::string error;
    receiver.SetNotification(main, kPointerDatagramMessage);
    if (!receiver.Start(static_cast<uint16_t>(settings.port), Utf8(settings.sourceIp), error)) {
        notice = W(L"设备接收未启动。", L"设备接收没启动喵。");
        Error(W(L"接收启动失败：", L"接收启动失败喵：") + Wide(error)); statusAt = 0; return false;
    }
    state = {}; displayed = {}; delayed.clear(); accepted = rejected = 0; sender.clear(); lastMove = GetTickCount64();
    lastCalibrationDownMs = calibrationPressedSince = 0;
    running = true; notice = W(L"设备接收已启动。", L"设备接收启动啦喵。"); statusAt = 0; return true;
}

void Application::Stop() {
    receiver.Stop(); running = false; state = {}; displayed = {}; delayed.clear();
    lastCalibrationDownMs = calibrationPressedSince = 0;
    CancelCalibration(false);
    ShowWindow(overlay, SW_HIDE); notice = W(L"设备接收已停止。", L"设备接收停好啦喵。"); statusAt = 0;
}

bool Application::StartCaptureOutput() {
    if (!IsCaptureDisplayMode(settings.mode)) {
        Error(W(L"请先在画面页选择“采集卡全屏输出”并应用。", L"先在画面页选“采集卡全屏输出”并应用喵。")); return false;
    }
    auto device = std::find_if(captureDevices.begin(), captureDevices.end(), [&](const qp::CaptureDevice& item) {
        return CaptureDeviceIdsEqual(item.symbolicLink, settings.captureDeviceId);
    });
    if (settings.captureDeviceId.empty() || device == captureDevices.end()) {
        Error(W(L"所选采集卡当前不可用。请插入设备并刷新后重新选择。", L"所选采集卡现在不可用喵，请插入后刷新重选。")); return false;
    }
    auto monitor = std::find_if(monitors.begin(), monitors.end(), [&](const MonitorItem& item) {
        return MatchesMonitor(item, settings.captureMonitorId);
    });
    if (settings.captureMonitorId.empty() || monitor == monitors.end()) {
        Error(W(L"请选择当前存在的输出显示器。", L"请选择现在存在的输出显示器喵。")); return false;
    }
    if (!captureOutput.Active()) {
        qp::CaptureOptions options;
        options.symbolicLink = settings.captureDeviceId;
        const auto selectedDevice = std::find_if(captureDevices.begin(), captureDevices.end(),
            [&](const qp::CaptureDevice& item) { return CaptureDeviceIdsEqual(item.symbolicLink, settings.captureDeviceId); });
        if (selectedDevice != captureDevices.end()) {
            options.symbolicLink = selectedDevice->symbolicLink;
            options.mediaFoundationId = selectedDevice->mediaFoundationId;
            options.directShowId = selectedDevice->directShowId;
            options.backendIdsResolved = true;
        }
        switch (settings.captureModeSelection) {
        case CaptureModeSelection::Preferred:
            options.modePolicy = qp::CaptureModePolicy::Preferred;
            break;
        case CaptureModeSelection::Exact:
            options.modePolicy = qp::CaptureModePolicy::Exact;
            break;
        default:
            options.modePolicy = qp::CaptureModePolicy::Auto;
            break;
        }
        options.exactModeKey = settings.captureModeKey;
        // Auto 也把合并后的最佳规格交给核心，核心据此先在对应后端尝试目标档；
        // 若设备没有公开规格，仍保持 hasRequestedMode=false 走驱动默认兼容路径。
        const int requestedIndex = qp::CaptureOutput::SelectMode(
            captureModes, options.modePolicy, options.exactModeKey);
        if (requestedIndex >= 0 && requestedIndex < static_cast<int>(captureModes.size())) {
            options.requestedMode = captureModes[requestedIndex];
            options.hasRequestedMode = true;
        }
        options.notifyWindow = main; options.notifyMessage = kCaptureFrameMessage;
        std::wstring error;
        if (!captureOutput.Start(options, error)) {
            captureRuntimeNotice = error;
            Error(W(L"采集启动失败：", L"采集启动失败喵：") + error); return false;
        }
        presentedCaptureFrame.reset(); captureKnownWidth = captureKnownHeight = 0;
    }
    captureFullscreenRequested = true;
    captureCreateRetryAt = 0;
    captureHintUntil = GetTickCount64() + 8000;
    captureRuntimeNotice = W(L"正在连接采集画面；收到第一帧后自动进入全屏。", L"正在连接采集画面喵，收到第一帧后自动全屏。") ;
    statusAt = 0;
    return true;
}

bool Application::StartFusionOutput() {
    if (!IsFusionDisplayMode(settings.mode)) {
        Error(W(L"请先在画面页选择“纯色背景全屏（融合器）”并应用。",
            L"先在画面页选“纯色背景全屏（融合器）”并应用喵。"));
        return false;
    }
    const auto monitor = std::find_if(monitors.begin(), monitors.end(), [&](const MonitorItem& item) {
        return MatchesMonitor(item, settings.fusionMonitorId);
    });
    if (settings.fusionMonitorId.empty() || monitor == monitors.end()) {
        Error(W(L"请选择当前存在的融合输出显示器。", L"请选择现在存在的融合输出显示器喵。"));
        return false;
    }
    if (settings.fusionBackgroundColor == settings.color) {
        Error(W(L"背景色与光标颜色完全相同，融合器会同时抠除光标。请更换其中一种颜色。",
            L"背景色和光标颜色一样喵，融合器会把光标也抠除，请换一种颜色。"));
        return false;
    }
    // 纯色输出不打开采集设备，也不等待视频首帧。
    captureFullscreenRequested = true;
    captureCreateRetryAt = 0;
    captureHintUntil = 0;
    captureRuntimeNotice = W(L"正在打开纯色融合输出。", L"正在打开纯色融合输出喵。");
    statusAt = 0;
    return true;
}

int Application::ReleaseLinkInput() {
    HWND link = hostWindow ? hostWindow : FindWindowW(L"QInEXLinkMain", nullptr);
    if (!link) return 0;
    const UINT message = RegisterWindowMessageW(L"QInEX.Pointer.ReleaseLocalInput.v1");
    if (!message) return -1;
    DWORD_PTR handled = 0;
    const LRESULT sent = SendMessageTimeoutW(link, message, 0, 0,
        SMTO_ABORTIFHUNG | SMTO_BLOCK, 250, &handled);
    return sent && handled ? 1 : -1;
}

std::wstring Application::CaptureExitHint() const {
    const bool linkReady = FindWindowW(L"QInEXLinkMain", nullptr) != nullptr;
    const qp::HotkeyBinding rangeBinding{settings.rangeHotkey, settings.rangeHotkeyModifiers};
    const qp::HotkeyBinding exitBinding{settings.exitHotkey, settings.exitHotkeyModifiers};
    const std::wstring range = rangeBinding.virtualKey && (captureRangeHotkeyReady || linkReady)
        ? qp::HotkeyText(rangeBinding) + W(L" 调整光标范围 · ", L" 调整光标范围喵 · ") : L"";
    if (captureExitHotkeyReady || linkReady)
        return range + qp::HotkeyText(exitBinding) + W(L" / Esc 返回设置 · 右键菜单",
            L" / Esc 返回设置 · 右键菜单喵");
    if (captureFallbackHotkeyReady)
        return range + qp::HotkeyText(exitBinding) + W(L" / Esc 返回 · 后台备用 Ctrl+Alt+F12",
            L" / Esc 返回喵 · 后台备用 Ctrl+Alt+F12");
    return range + qp::HotkeyText(exitBinding) + W(L" / Esc 返回设置 · 右键菜单",
        L" / Esc 返回设置 · 右键菜单喵");
}

void Application::UpdateOutputHud(uint64_t now) {
    if (!IsOwnedFullscreenDisplayMode(settings.mode)) {
        outputHudStatusText.reset();
        outputHudShortcutText.reset();
        return;
    }
    std::wstring statusText, shortcutText;
    auto append = [](std::wstring& text, std::wstring segment) {
        if (segment.empty()) return;
        if (!text.empty() && text.back() != L'\n') text += L"  ·  ";
        text += std::move(segment);
    };
    auto addStatus = [&](std::wstring segment) { append(statusText, std::move(segment)); };
    auto addShortcut = [&](std::wstring segment) { append(shortcutText, std::move(segment)); };
    const auto presenter = capturePresenter.Status();
    if (IsCaptureDisplayMode(settings.mode)) {
        const auto capture = captureOutput.Status();
        if (capture.state == qp::CaptureState::Running) {
            std::wstring input = W(L"输入 ", L"输入喵 ") + std::to_wstring(capture.width) +
                L"×" + std::to_wstring(capture.height);
            const double nominal = capture.hasActualMode ? capture.actualMode.Fps() : capture.nominalFps;
            if (nominal > 0) input += L" @ " + Metric1(nominal) + L" Hz";
            addStatus(std::move(input));
            const auto& diagnostic = capture.diagnostics;
            const bool freshFrames = capture.lastFrameMs && Age(now, capture.lastFrameMs) < 500;
            if (!freshFrames) {
                addStatus(W(L"等待新帧…", L"等新帧喵…"));
            } else if (diagnostic.ready) {
                addStatus(W(L"接收 ", L"接收喵 ") + Metric1(capture.measuredFps) + L" FPS");
                if (diagnostic.contentAvailable) {
                    addStatus(W(L"画面变化≈", L"画面变化喵≈") +
                        Metric1(diagnostic.changedFps) + L" FPS");
                    addStatus(W(L"重复采样 ", L"重复采样喵 ") +
                        Metric1(diagnostic.identicalPercent) + L"%");
                } else {
                    addStatus(W(L"画面采样不可用", L"画面采样不可用喵"));
                }
            } else addStatus(W(L"帧分析中…", L"帧分析中喵…"));
            addStatus(CaptureBackendHud(capture.backend));
            statusText += L"\n";
            // 新视频呈现按新序号统计；重绘含光标/HUD 更新，不能冒充视频输入 FPS。
            if (presenter.active) {
                addStatus(W(L"新视频呈现 ", L"新视频呈现喵 ") +
                    Metric1(presenter.measuredVideoFps) + L" FPS");
                addStatus(W(L"重绘 ", L"重绘喵 ") +
                    Metric1(presenter.measuredPresentFps) + L" FPS");
            } else addStatus(W(L"兼容显示 · 呈现率不可用", L"兼容显示喵 · 呈现率不可用"));
            if (presenter.presents && presenter.lastCaptureToPresentMs >= 0)
                addStatus(W(L"回调→Present ", L"回调→Present 喵 ") +
                    Metric1(presenter.lastCaptureToPresentMs) + L" ms");
            if (capture.lastFrameMs)
                addStatus(W(L"最新帧 ", L"最新帧喵 ") +
                    std::to_wstring(Age(now, capture.lastFrameMs)) + L" ms");
            if (diagnostic.ready)
                addStatus(W(L"帧间隔 P95/峰值 ", L"帧间隔 P95/峰值喵 ") +
                    Metric1(diagnostic.intervalP95Ms) + L"/" +
                    Metric1(diagnostic.intervalMaxMs) + L" ms");
            statusText += L"\n";
            addStatus(W(L"连续相同 ", L"连续相同喵 ") + Metric1(diagnostic.unchangedMs) + L" ms");
            if (diagnostic.timestampAvailable)
                addStatus(W(L"时间戳重复/倒退 ", L"时间戳重复/倒退喵 ") +
                    std::to_wstring(diagnostic.timestampRepeats) + L"/" +
                    std::to_wstring(diagnostic.timestampRegressions));
            else addStatus(W(L"时间戳不可用", L"时间戳不可用喵"));
            addStatus(W(L"入口跳过 ", L"入口跳过喵 ") +
                std::to_wstring(diagnostic.mailboxSkippedFrames));
            addStatus(W(L"采样估算；静止画面不代表假帧", L"采样估算喵；静止画面不代表假帧"));
            if (capture.dropCountAvailable)
                addStatus(W(L"驱动掉帧 ", L"驱动掉帧喵 ") +
                    std::to_wstring(capture.droppedFrames));
        } else {
            addStatus(CaptureStateLabel(capture.state));
            if (!capture.message.empty()) addStatus(capture.message);
        }
    } else {
        addStatus(W(L"融合 ", L"融合喵 ") + ColorHex(settings.fusionBackgroundColor));
        addStatus(W(L"无采集链路", L"没有采集链路喵"));
        const auto monitor = std::find_if(monitors.begin(), monitors.end(), [&](const MonitorItem& item) {
            return MatchesMonitor(item, settings.fusionMonitorId);
        });
        if (monitor != monitors.end() && monitor->refreshHz > 0)
            addStatus(W(L"输出 ", L"输出喵 ") + Metric1(monitor->refreshHz) + L" Hz");
        if (presenter.measuredPresentFps > 0)
            addStatus(L"Present " + Metric1(presenter.measuredPresentFps) + L" FPS");
        if (running && state.lastPacketMs) {
            const uint64_t packetAge = Age(now, state.lastPacketMs);
            addStatus(packetAge <= static_cast<uint64_t>(settings.timeoutMs)
                ? W(L"指针数据 ", L"指针数据喵 ") + std::to_wstring(packetAge) + L" ms"
                : W(L"指针数据已中断", L"指针数据中断啦喵"));
        } else {
            addStatus(W(L"等待指针数据", L"等指针数据喵"));
        }
        if (presenter.presents && presenter.lastSubmitToPresentMs >= 0)
            addStatus(W(L"提交→Present ", L"提交→Present 喵 ") +
                Metric1(presenter.lastSubmitToPresentMs) + L" ms");
        if (presenter.presents)
            addStatus(std::wstring(L"Present ") + W(L"帧 ", L"帧喵 ") +
                std::to_wstring(presenter.presents));
    }

    const qp::HotkeyBinding hudBinding{settings.hudHotkey, settings.hudHotkeyModifiers};
    const qp::HotkeyBinding rangeBinding{settings.rangeHotkey, settings.rangeHotkeyModifiers};
    const qp::HotkeyBinding exitBinding{settings.exitHotkey, settings.exitHotkeyModifiers};
    const qp::HotkeyBinding pauseBinding{settings.pauseHotkey, settings.pauseHotkeyModifiers};
    if (hudBinding.virtualKey)
        addShortcut(qp::HotkeyText(hudBinding) + L" " + W(L"隐藏信息", L"隐藏信息喵"));
    if (rangeBinding.virtualKey)
        addShortcut(qp::HotkeyText(rangeBinding) + L" " + W(L"校准", L"校准喵"));
    addShortcut(qp::HotkeyText(exitBinding) + W(L" / Esc 返回", L" / Esc 返回喵"));
    if (pauseBinding.virtualKey)
        addShortcut(qp::HotkeyText(pauseBinding) +
            (paused ? W(L" 恢复指针", L" 恢复指针喵") : W(L" 暂停指针", L" 暂停指针喵")));
    addShortcut(W(L"右键菜单", L"右键菜单喵"));

    if (!outputHudStatusText || *outputHudStatusText != statusText)
        outputHudStatusText = std::make_shared<const std::wstring>(std::move(statusText));
    if (!outputHudShortcutText || *outputHudShortcutText != shortcutText)
        outputHudShortcutText = std::make_shared<const std::wstring>(std::move(shortcutText));
}

void Application::ToggleOutputHud() {
    if (!captureWindow) return;
    const uint64_t now = GetTickCount64();
    // 注册热键、前台窗口和 QInEX-Link 可能在同一次按键中同时投递，合并为一次切换。
    if (outputHudToggleAt && Age(now, outputHudToggleAt) < 120) return;
    outputHudToggleAt = now;
    outputHudVisible = !outputHudVisible;
    UpdateOutputHud(now);
    Render();
    if (!capturePresenter.Active()) InvalidateRect(captureWindow, nullptr, FALSE);
    statusAt = 0;
}

void Application::ShowControlWindow() {
    if (!IsWindow(main)) return;
    if (captureRangeEditing) FinishCaptureRangeAdjustment(false);
    // 从采集全屏返回后必须同时解除 Link 独占，否则主窗口虽已出现仍无法点击。
    ReleaseLinkInput();
    // 全屏输出只负责呈现。用户回到控制页时临时降低它的 Z 序，避免同屏下
    // WS_EX_TOPMOST 把主窗口永久压在后面；采样或再次点击全屏时会重新置顶。
    if (IsWindow(captureWindow))
        SetWindowPos(captureWindow, HWND_NOTOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (hostWindow) {
        SendMessageW(hostWindow, qp::desktop::kShowPointerMessage, 0, 0);
        ShowWindow(hostWindow, SW_RESTORE);
        ShowWindow(main, SW_SHOW);
        SetForegroundWindow(hostWindow);
        SetFocus(Item(ID_NAV_FIRST + page));
        return;
    }
    ShowWindow(main, SW_RESTORE);
    SetWindowPos(main, HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetWindowPos(main, HWND_NOTOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetForegroundWindow(main);
}

void Application::FocusCaptureFullscreen() {
    if (!IsWindow(captureWindow)) return;
    SetWindowPos(captureWindow, HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(captureWindow);
}

void Application::BeginCaptureRangeAdjustment() {
    if (calibrationPhase != CapturePhase::Idle) {
        Error(W(L"请先完成或取消当前点位取样。", L"请先完成或取消当前点位取样喵。")); return;
    }
    if (!IsOwnedFullscreenDisplayMode(settings.mode) ||
            (IsCaptureDisplayMode(settings.mode) && !captureOutput.Active())) {
        Error(W(L"请先在“画面”页启动采集卡或纯色融合全屏输出。",
            L"请先在“画面”页启动采集卡或纯色融合全屏输出喵。")); return;
    }
    if (!captureWindow) {
        captureRangePending = true;
        if (IsFusionDisplayMode(settings.mode)) {
            if (!StartFusionOutput()) { captureRangePending = false; return; }
            notice = W(L"正在打开纯色输出，随后进入范围校准。", L"正在打开纯色输出，接着调整范围喵。");
        } else {
            captureFullscreenRequested = true;
            captureCreateRetryAt = 0;
            notice = W(L"正在恢复采集画面，随后进入范围校准。", L"正在恢复采集画面，接着调整范围喵。");
        }
        statusAt = 0;
        return;
    }
    if (captureRangeEditing) {
        FocusCaptureFullscreen(); return;
    }

    qp::Geometry geometry;
    RECT client{};
    if (!Geometry(geometry, true) || !GetClientRect(captureWindow, &client) || client.right < 2 || client.bottom < 2) {
        Error(W(L"当前采集画面还未准备好，请稍后再使用范围调整快捷键。",
            L"采集画面还没准备好喵，稍后再用范围调整快捷键。")); return;
    }
    geometry.target = {0, 0, static_cast<double>(client.right), static_cast<double>(client.bottom)};
    captureRangeGeometry = geometry;
    qp::Geometry base = geometry;
    base.calibration = {};
    captureRangeRect = qp::DisplayRect(base);

    const qp::Point corners[]{{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    bool complete = true;
    double left = 0, top = 0, right = 0, bottom = 0;
    for (size_t index = 0; index < std::size(corners); ++index) {
        const auto point = qp::MapPoint(geometry, corners[index]);
        if (!point) { complete = false; break; }
        if (!index) left = right = point->x, top = bottom = point->y;
        else {
            left = std::min(left, point->x); right = std::max(right, point->x);
            top = std::min(top, point->y); bottom = std::max(bottom, point->y);
        }
    }
    if (complete && right - left >= 2 && bottom - top >= 2)
        captureRangeRect = {left, top, right - left + 1, bottom - top + 1};

    captureRangePending = false;
    captureRangeEditing = true;
    captureRangeDrag = 0;
    ReleaseLinkInput();
    SetForegroundWindow(captureWindow);
    captureHintUntil = 0;
    SubmitCapturePresentation();
    if (!capturePresenter.Active()) InvalidateRect(captureWindow, nullptr, FALSE);
}

int Application::CaptureRangeHitTest(POINT point) const {
    if (!captureRangeEditing) return 0;
    constexpr double edge = 15;
    const auto& r = captureRangeRect;
    if (point.x < r.x - edge || point.x > r.x + r.w + edge ||
        point.y < r.y - edge || point.y > r.y + r.h + edge) return 0;
    int hit = 0;
    if (std::abs(point.x - r.x) <= edge) hit |= kRangeLeft;
    if (std::abs(point.x - (r.x + r.w)) <= edge) hit |= kRangeRight;
    if (std::abs(point.y - r.y) <= edge) hit |= kRangeTop;
    if (std::abs(point.y - (r.y + r.h)) <= edge) hit |= kRangeBottom;
    if (!hit && point.x >= r.x && point.x <= r.x + r.w &&
        point.y >= r.y && point.y <= r.y + r.h) hit = kRangeMove;
    return hit;
}

void Application::UpdateCaptureRangeDrag(POINT point) {
    if (!captureRangeEditing || !captureRangeDrag) return;
    const qp::Rect bounds = qp::DisplayRect(captureRangeGeometry);
    const double dx = point.x - captureRangeDragStart.x;
    const double dy = point.y - captureRangeDragStart.y;
    qp::Rect next = captureRangeDragOriginal;
    const double minimum = std::max(32.0, std::min(bounds.w, bounds.h) * .04);
    if (captureRangeDrag == kRangeMove) {
        next.x = std::clamp(next.x + dx, bounds.x, bounds.x + bounds.w - next.w);
        next.y = std::clamp(next.y + dy, bounds.y, bounds.y + bounds.h - next.h);
    } else {
        double left = next.x, right = next.x + next.w;
        double top = next.y, bottom = next.y + next.h;
        if (captureRangeDrag & kRangeLeft) left = std::clamp(left + dx, bounds.x, right - minimum);
        if (captureRangeDrag & kRangeRight) right = std::clamp(right + dx, left + minimum, bounds.x + bounds.w);
        if (captureRangeDrag & kRangeTop) top = std::clamp(top + dy, bounds.y, bottom - minimum);
        if (captureRangeDrag & kRangeBottom) bottom = std::clamp(bottom + dy, top + minimum, bounds.y + bounds.h);
        next = {left, top, right - left, bottom - top};
    }
    captureRangeRect = next;
    SubmitCapturePresentation();
    if (!capturePresenter.Active()) InvalidateRect(captureWindow, nullptr, FALSE);
}

void Application::FinishCaptureRangeAdjustment(bool apply) {
    if (!captureRangeEditing) { captureRangePending = false; return; }
    if (GetCapture() == captureWindow) ReleaseCapture();
    captureRangeDrag = 0;
    if (apply) {
        qp::Affine fitted;
        std::string error;
        if (!qp::FitRangeCalibration(captureRangeGeometry, captureRangeRect, fitted, error)) {
            Error(W(L"无法应用这个范围：", L"这个范围还不能应用喵：") + Wide(error)); return;
        }
        if (IsFusionDisplayMode(settings.mode)) {
            settings.fusionCalibration = fitted;
        } else {
            PushCalibrationUndo();
            settings.geometry.calibration = fitted;
            samples.clear(); calibrationSlots.clear(); calibrationTokens.clear();
            fittedCalibrationTokens.clear(); calibrationErrors.clear();
            fitSampleCount = 0; calibrationFitToken = 0; calibrationSlot = 0;
        }
        calibrationResult = W(L"快捷范围已应用。请移动光标检查四角和中心，满意后保存。",
            L"快捷范围应用好啦喵，检查四角和中心后保存。" );
        MarkDirty();
        notice = calibrationResult;
    } else {
        notice = W(L"已取消范围调整，原校准保持不变。", L"范围调整取消啦，原校准保持不变喵。");
    }
    captureRangeEditing = false;
    captureRangePending = false;
    captureRangeGeometry = {};
    UpdateCalibrationText();
    SubmitCapturePresentation();
    if (!capturePresenter.Active() && captureWindow) InvalidateRect(captureWindow, nullptr, FALSE);
    statusAt = 0;
}

void Application::PaintCaptureRange(HDC dc) {
    if (!captureRangeEditing) return;
    const auto& r = captureRangeRect;
    HPEN pen = CreatePen(PS_SOLID, 3, RGB(0, 220, 235));
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
    Rectangle(dc, static_cast<int>(std::lround(r.x)), static_cast<int>(std::lround(r.y)),
        static_cast<int>(std::lround(r.x + r.w)), static_cast<int>(std::lround(r.y + r.h)));
    const LONG left = static_cast<LONG>(std::lround(r.x));
    const LONG top = static_cast<LONG>(std::lround(r.y));
    const LONG right = static_cast<LONG>(std::lround(r.x + r.w));
    const LONG bottom = static_cast<LONG>(std::lround(r.y + r.h));
    const LONG middleX = (left + right) / 2;
    const LONG middleY = (top + bottom) / 2;
    const POINT handles[]{{left, top}, {middleX, top}, {right, top},
        {left, middleY}, {right, middleY}, {left, bottom}, {middleX, bottom}, {right, bottom}};
    HBRUSH handleBrush = CreateSolidBrush(RGB(255, 255, 255));
    SelectObject(dc, handleBrush);
    for (const POINT handle : handles)
        Rectangle(dc, handle.x - 6, handle.y - 6, handle.x + 7, handle.y + 7);
    SelectObject(dc, oldBrush);
    DeleteObject(handleBrush);
    SelectObject(dc, oldPen); DeleteObject(pen);
    RECT client{}; GetClientRect(captureWindow, &client);
    RECT panel{std::max(20L, client.right / 2 - 360L), 20,
        std::min(client.right - 20L, client.right / 2 + 360L), 72};
    HBRUSH background = CreateSolidBrush(RGB(9, 18, 22)); FillRect(dc, &panel, background); DeleteObject(background);
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(245, 250, 252));
    HGDIOBJ oldFont = SelectObject(dc, font);
    DrawTextW(dc, W(L"拖框内移动 · 拖边角缩放 · Enter 应用 · Esc 取消",
        L"拖动和缩放边框喵 · Enter 应用 · Esc 取消"), -1, &panel,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, oldFont);
}

void Application::ExitCaptureFullscreen(bool restoreControl) {
    if (captureRangeEditing) FinishCaptureRangeAdjustment(false);
    captureRangePending = false;
    if (calibrationPhase != CapturePhase::Idle) CancelCalibration(false);
    captureFullscreenRequested = false;
    captureCreateRetryAt = 0;
    UnregisterHotKey(main, 2);
    UnregisterHotKey(main, 3);
    UnregisterHotKey(main, 4);
    UnregisterHotKey(main, 5);
    captureExitHotkeyReady = false;
    captureFallbackHotkeyReady = false;
    captureRangeHotkeyReady = false;
    outputHudHotkeyReady = false;
    SetThreadExecutionState(ES_CONTINUOUS);
    capturePresenter.Stop();
    if (captureWindow) {
        HWND window = captureWindow; captureWindow = nullptr;
        DestroyWindow(window);
    }
    ShowWindow(overlay, SW_HIDE);
    if (restoreControl && IsWindow(main)) {
        const HWND control=hostWindow ? hostWindow : main;
        RECT current{}; GetWindowRect(control, &current);
        MONITORINFO monitor{sizeof(monitor)};
        if (GetMonitorInfoW(MonitorFromWindow(control, MONITOR_DEFAULTTONEAREST), &monitor)) {
            const int width = current.right - current.left, height = current.bottom - current.top;
            const int x = std::clamp(current.left, monitor.rcWork.left, std::max(monitor.rcWork.left, monitor.rcWork.right - width));
            const int y = std::clamp(current.top, monitor.rcWork.top, std::max(monitor.rcWork.top, monitor.rcWork.bottom - height));
            SetWindowPos(control, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        ShowControlWindow();
    }
    if (IsFusionDisplayMode(settings.mode))
        captureRuntimeNotice = W(L"已退出纯色融合全屏，可从画面页或托盘重新进入。",
            L"退出纯色融合全屏啦，可从画面页或托盘重新进入喵。");
    else if (captureOutput.Active())
        captureRuntimeNotice = W(L"已退出全屏，采集连接保持，可随时重新全屏。",
            L"退出全屏啦，采集连接还保留着喵，随时可重新全屏。");
    else captureRuntimeNotice.clear();
    statusAt = 0;
}

void Application::StopCaptureOutput() {
    const bool hadCapture = captureOutput.Active();
    const bool hadFusion = IsFusionDisplayMode(settings.mode) &&
        (captureWindow || captureFullscreenRequested);
    captureFullscreenRequested = false;
    ExitCaptureFullscreen(false);
    captureOutput.Stop();
    presentedCaptureFrame.reset(); captureKnownWidth = captureKnownHeight = 0;
    if (hadCapture)
        captureRuntimeNotice = W(L"采集画面已停止，指针设备连接不受影响。",
            L"采集画面停啦，指针设备连接不受影响喵。");
    else if (hadFusion)
        captureRuntimeNotice = W(L"纯色融合输出已停止，指针设备连接不受影响。",
            L"纯色融合输出停啦，指针设备连接不受影响喵。");
    else captureRuntimeNotice.clear();
    statusAt = 0;
}

void Application::ToggleCaptureOutput() {
    if (Has(ID_MODE)) {
        Settings next;
        if (!ReadPage(next, 2)) return;
        AdoptSettings(next); CommitDraft(2);
    }
    if (!IsOwnedFullscreenDisplayMode(settings.mode)) {
        Error(W(L"请先选择采集卡或纯色融合全屏输出。", L"请先选择采集卡或纯色融合全屏输出喵。")); return;
    }
    if (captureWindow) { ExitCaptureFullscreen(); return; }
    if (IsFusionDisplayMode(settings.mode)) {
        StartFusionOutput();
        return;
    }
    if (captureOutput.Active()) {
        captureFullscreenRequested = true; captureCreateRetryAt = 0; captureHintUntil = GetTickCount64() + 8000;
        captureRuntimeNotice = W(L"正在恢复全屏输出。", L"正在恢复全屏输出喵。"); statusAt = 0; return;
    }
    StartCaptureOutput();
}

void Application::PositionCaptureOutput() {
    if (!captureWindow) return;
    const std::wstring& monitorId = IsFusionDisplayMode(settings.mode)
        ? settings.fusionMonitorId : settings.captureMonitorId;
    auto monitor = std::find_if(monitors.begin(), monitors.end(), [&](const MonitorItem& item) {
        return MatchesMonitor(item, monitorId);
    });
    if (monitor == monitors.end()) {
        ExitCaptureFullscreen();
        captureRuntimeNotice = W(L"输出显示器已断开。全屏已退出，请重新选择显示器。", L"输出显示器断开啦，已退出全屏，请重新选择喵。");
        return;
    }
    const RECT& r = monitor->info.rcMonitor;
    SetWindowPos(captureWindow, HWND_TOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top,
        SWP_FRAMECHANGED);
    capturePresenter.OnResize();
}

void Application::PaintOutputHud(HDC dc) {
    const auto status = outputHudStatusText;
    const auto shortcuts = outputHudShortcutText;
    if (!outputHudVisible || captureRangeEditing ||
            ((!status || status->empty()) && (!shortcuts || shortcuts->empty()))) return;
    RECT client{};
    GetClientRect(captureWindow, &client);
    if (client.right - client.left < 32 || client.bottom - client.top < 24) return;
    const int lineCount = qp::OutputHudLineCount(status.get(), shortcuts.get());
    RECT panel{client.left + 12, std::max(client.top, client.bottom - 8 - lineCount * 28),
        client.right - 12, client.bottom - 8};
    HBRUSH background = CreateSolidBrush(RGB(9, 14, 18));
    FillRect(dc, &panel, background);
    DeleteObject(background);
    const int previousMode = SetBkMode(dc, TRANSPARENT);
    const COLORREF previousColor = SetTextColor(dc, RGB(238, 244, 248));
    HGDIOBJ previousFont = SelectObject(dc, font);
    qp::ForEachOutputHudLine(status.get(), shortcuts.get(), [&](std::wstring_view text, int row) {
        RECT line{panel.left + 12, panel.top + row * 28, panel.right - 12,
            std::min(panel.bottom, panel.top + (row + 1) * 28)};
        DrawTextW(dc, text.data(), static_cast<int>(text.size()), &line,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    });
    SelectObject(dc, previousFont);
    SetTextColor(dc, previousColor);
    SetBkMode(dc, previousMode);
}

void Application::PaintCaptureOutput(HDC dc) {
    RECT client{}; GetClientRect(captureWindow, &client);
    if (IsFusionDisplayMode(settings.mode)) {
        HBRUSH background = CreateSolidBrush(settings.fusionBackgroundColor);
        FillRect(dc, &client, background);
        DeleteObject(background);
        PaintOutputHud(dc);
        PaintCaptureRange(dc);
        return;
    }
    auto frame = presentedCaptureFrame; auto status = captureOutput.Status();
    qp::Geometry geometry;
    bool painted = frame && Geometry(geometry) && PaintDecodedFrame(dc, client, frame, geometry);
    if (!painted) {
        HBRUSH black = CreateSolidBrush(RGB(0, 0, 0)); FillRect(dc, &client, black); DeleteObject(black);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(235, 239, 244)); HGDIOBJ old = SelectObject(dc, font);
        std::wstring message = CaptureStateLabel(status.state);
        if (!status.message.empty()) message += L"\r\n" + status.message;
        message += L"\r\n\r\n" + CaptureExitHint();
        RECT text{client.left + 40, client.top + 40, client.right - 40, client.bottom - 40};
        DrawTextW(dc, message.c_str(), -1, &text, DT_CENTER | DT_VCENTER | DT_WORDBREAK);
        SelectObject(dc, old);
    }
    PaintOutputHud(dc);
    PaintCaptureRange(dc);
}

HBITMAP Application::SnapshotCaptureOutput() {
    qp::Geometry geometry;
    auto frame = presentedCaptureFrame; auto status = captureOutput.Status();
    const uint64_t now = GetTickCount64();
    if (!frame || status.state != qp::CaptureState::Running || Age(now, frame->capturedMs) > 1000 || !Geometry(geometry)) return nullptr;
    int width = static_cast<int>(std::lround(geometry.target.w)), height = static_cast<int>(std::lround(geometry.target.h));
    if (width < 2 || height < 2 || int64_t(width) * height > 20000000) return nullptr;
    HDC screen = GetDC(nullptr), memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
    HGDIOBJ old = bitmap ? SelectObject(memory, bitmap) : nullptr;
    RECT client{0, 0, width, height};
    bool ok = bitmap && PaintDecodedFrame(memory, client, frame, geometry);
    if (ok && captureRangeEditing) PaintCaptureRange(memory);
    if (old) SelectObject(memory, old); DeleteDC(memory); ReleaseDC(nullptr, screen);
    if (!ok) { if (bitmap) DeleteObject(bitmap); return nullptr; }
    return bitmap;
}

void Application::TogglePause() {
    paused = !paused;
    notice = paused ? W(L"指针已暂停，设备数据仍在接收。", L"指针暂停啦，设备数据还在接收喵。") : W(L"指针已恢复显示。", L"指针恢复显示啦喵。");
    Render(); statusAt = 0; UpdateTray();
}

void Application::BindSource() {
    if (!running || sender.empty() || !state.hasPosition || Age(GetTickCount64(), state.lastPacketMs) > static_cast<uint64_t>(settings.timeoutMs)) {
        notice = W(L"尚未收到可绑定的设备，请先移动设备鼠标。", L"还没收到可绑定设备，先动一下设备鼠标喵。"); statusAt = 0; return;
    }
    Settings next = settings; next.sourceIp = sender;
    AdoptSettings(next); ReloadControls();
    notice = W(L"已绑定设备地址：", L"已绑定设备地址喵：") + settings.sourceIp; statusAt = 0;
}

void Application::UpdateStatus() {
    const uint64_t now = GetTickCount64();
    UpdateOutputHud(now);
    if (!running) deviceStatus = W(L"未开始接收设备", L"还没开始接收设备喵");
    else if (!state.lastPacketMs) deviceStatus = W(L"等待设备数据", L"正在等设备数据喵");
    else if (Age(now, state.lastPacketMs) > static_cast<uint64_t>(settings.timeoutMs))
        deviceStatus = W(L"数据已中断：", L"数据中断啦喵：") + std::to_wstring(Age(now, state.lastPacketMs) / 1000) + W(L" 秒未收到数据", L" 秒没收到数据喵");
    else deviceStatus = W(L"已收到设备数据", L"收到设备数据啦喵");
    std::wstring captureStatusText;
    if (IsCaptureDisplayMode(settings.mode)) {
        const auto capture = captureOutput.Status();
        captureStatusText = CaptureStateLabel(capture.state);
        if (capture.state == qp::CaptureState::Running) {
            captureStatusText += L" " + std::to_wstring(capture.width) + L"×" + std::to_wstring(capture.height);
            if (capture.measuredFps > 0) captureStatusText += L" @ " + Num(capture.measuredFps) + L" fps";
            auto outputMonitor = std::find_if(monitors.begin(), monitors.end(), [&](const MonitorItem& item) {
                return MatchesMonitor(item, settings.captureMonitorId);
            });
            if (outputMonitor != monitors.end() && outputMonitor->refreshHz > 0)
                captureStatusText += W(L" · 输出 ", L" · 输出喵 ") + Num(outputMonitor->refreshHz) + L" Hz";
            captureStatusText += captureWindow ? W(L" · 全屏输出中", L" · 全屏输出中喵") : W(L" · 全屏已退出", L" · 全屏已退出喵");
            const auto presenter = capturePresenter.Status();
            if (presenter.active) {
                captureStatusText += L" · " + presenter.backend;
                captureStatusText += presenter.tearingActive
                    ? W(L" · 极限低延迟", L" · 极限低延迟喵")
                    : W(L" · 自动同步", L" · 自动同步喵");
                if (presenter.presents && presenter.lastCaptureToPresentMs >= 0)
                    captureStatusText += W(L" · 采集回调→Present ", L" · 采集回调→Present 喵 ") +
                    Num(presenter.lastCaptureToPresentMs) + L" ms";
            }
        }
    } else if (IsFusionDisplayMode(settings.mode)) {
        auto outputMonitor = std::find_if(monitors.begin(), monitors.end(), [&](const MonitorItem& item) {
            return MatchesMonitor(item, settings.fusionMonitorId);
        });
        captureStatusText = W(L"纯色融合 ", L"纯色融合喵 ") + ColorHex(settings.fusionBackgroundColor);
        if (outputMonitor == monitors.end())
            captureStatusText += W(L" · 输出显示器不可用", L" · 输出显示器不可用喵");
        else {
            if (outputMonitor->refreshHz > 0)
                captureStatusText += W(L" · 输出 ", L" · 输出喵 ") + Num(outputMonitor->refreshHz) + L" Hz";
            captureStatusText += captureWindow
                ? W(L" · 全屏输出中", L" · 全屏输出中喵")
                : W(L" · 尚未启动", L" · 还没启动喵");
        }
        const auto presenter = capturePresenter.Status();
        if (presenter.active) {
            captureStatusText += L" · " + presenter.backend;
            captureStatusText += presenter.tearingActive
                ? W(L" · 极限低延迟", L" · 极限低延迟喵")
                : W(L" · 自动同步", L" · 自动同步喵");
        }
    }
    qp::Geometry g;
    if (demoActive) displayStatus = W(L"正在演示；真实设备配置保持完整", L"正在演示喵，真实设备配置都保留着");
    else if (paused) displayStatus = W(L"指针已暂停，全屏画面继续显示", L"指针暂停啦，全屏画面继续显示喵");
    else if (!running) displayStatus = W(L"请先连接设备或体验演示", L"先连接设备或体验演示喵");
    else if (!state.lastPacketMs) displayStatus = W(L"请移动设备鼠标；核对板端填写的电脑地址", L"请动一下设备鼠标，核对板端电脑地址喵");
    else if (Age(now, state.lastPacketMs) > static_cast<uint64_t>(settings.timeoutMs)) displayStatus = W(L"已隐藏指针；等待发送端恢复数据", L"指针已隐藏，等发送端恢复数据喵");
    else if (captureDue || captureWaiting || snapshot) displayStatus = W(L"正在校准，按取样提示操作", L"正在校准，按取样提示操作喵");
    else if (region || alignmentCheck) displayStatus = W(L"正在检查画面区域", L"正在检查画面区域喵");
    else if (!state.hasPosition) displayStatus = W(L"已收到消息，等待完整光标位置", L"收到消息啦，等完整光标位置喵");
    else if (!state.show) displayStatus = W(L"设备当前要求隐藏光标", L"设备现在要求隐藏光标喵");
    else if (IsFusionDisplayMode(settings.mode) && !captureWindow)
        displayStatus = W(L"纯色融合全屏尚未启动", L"纯色融合全屏还没启动喵");
    else if (settings.mode == 2 && !IsWindow(targetWindow)) displayStatus = W(L"预览窗口未找到，请重新选择", L"没找到预览窗口，请重新选择喵");
    else if (settings.mode == 2 && IsIconic(targetWindow)) displayStatus = W(L"预览已最小化，恢复预览即可显示", L"预览最小化啦，恢复预览就会显示喵");
    else if (!Geometry(g)) displayStatus = W(L"目标区域不可用，请检查画面设置", L"目标区域不可用，请检查画面设置喵");
    else if (!Geometry(g, true)) displayStatus = W(L"预览未在前台，按当前设置暂时隐藏", L"预览没在前台，按当前设置先隐藏喵");
    else if (!displayed.hasPosition) displayStatus = W(L"正在等待显示延迟缓冲", L"正在等显示延迟缓冲喵");
    else if (!displayed.show) displayStatus = W(L"正在等待延迟中的显示状态", L"正在等延迟中的显示状态喵");
    else if (!qp::MapPoint(g, displayed.position)) displayStatus = W(L"当前位置在可见画面外，请核对框选与裁剪", L"当前位置在画面外，核对框选和裁剪喵");
    else if (!lastRenderOk) displayStatus = W(L"指针绘制失败，请重新启动程序", L"指针没画出来，请重新启动程序喵");
    else displayStatus = W(L"正在显示光标", L"正在显示光标喵");
    runtimeStatus = deviceStatus;
    if (!captureStatusText.empty()) runtimeStatus += L"  ·  " + captureStatusText;
    runtimeStatus += L"  ·  " + displayStatus;
    if (!captureRuntimeNotice.empty()) runtimeStatus += L"\r\n" + captureRuntimeNotice;
    if (dirty) runtimeStatus += W(L"  ·  有未保存更改", L"  ·  还有更改没保存喵");
    Set(ID_STATUS, runtimeStatus + (notice.empty() ? L"" : L"\r\n" + notice));
    UpdatePresentation(); UpdateTray();
}

bool Application::Geometry(qp::Geometry& g, bool requireVisible) {
    g = settings.geometry;
    if (settings.mode == 0) {
        if (monitors.empty()) return false;
        auto i = std::clamp(settings.monitor, 0, static_cast<int>(monitors.size()) - 1);
        MONITORINFO mi{sizeof(mi)}; if (!GetMonitorInfoW(monitors[i].handle, &mi)) return false; g.target = FromRect(mi.rcMonitor);
    } else if (settings.mode == 2) {
        if (!IsWindow(targetWindow) || IsIconic(targetWindow) || !IsWindowVisible(targetWindow)) return false;
        if (requireVisible && settings.foregroundOnly && GetAncestor(GetForegroundWindow(), GA_ROOT) != GetAncestor(targetWindow, GA_ROOT)) return false;
        qp::Rect base; if (!ClientRectOnScreen(targetWindow, base)) return false;
        auto r = settings.windowRegion; g.target = {base.x + r.x * base.w, base.y + r.y * base.h, r.w * base.w, r.h * base.h};
    } else if (IsCaptureDisplayMode(settings.mode)) {
        auto frame = presentedCaptureFrame; auto status = captureOutput.Status();
        if (!frame || status.state != qp::CaptureState::Running) return false;
        if (Age(GetTickCount64(), frame->capturedMs) > 1000) return false;
        qp::Rect base;
        if (captureWindow && IsWindowVisible(captureWindow) && !IsIconic(captureWindow)) {
            if (!ClientRectOnScreen(captureWindow, base)) return false;
        } else {
            if (requireVisible) return false;
            // 解码会话与全屏窗口彼此独立。退出全屏后仍使用已选择显示器的实际矩形，
            // 让控制页可见时也能截图校准；再次取样会自动恢复全屏预览。
            auto monitor = std::find_if(monitors.begin(), monitors.end(), [&](const MonitorItem& item) {
                return MatchesMonitor(item, settings.captureMonitorId);
            });
            if (monitor == monitors.end()) return false;
            base = FromRect(monitor->info.rcMonitor);
        }
        g.target = base; g.frameWidth = frame->width; g.frameHeight = frame->height;
    } else if (IsFusionDisplayMode(settings.mode)) {
        qp::Rect base;
        if (captureWindow && IsWindowVisible(captureWindow) && !IsIconic(captureWindow)) {
            if (!ClientRectOnScreen(captureWindow, base)) return false;
        } else {
            if (requireVisible) return false;
            const auto monitor = std::find_if(monitors.begin(), monitors.end(), [&](const MonitorItem& item) {
                return MatchesMonitor(item, settings.fusionMonitorId);
            });
            if (monitor == monitors.end()) return false;
            base = FromRect(monitor->info.rcMonitor);
        }
        g.target = base;
        g.frameWidth = base.w;
        g.frameHeight = base.h;
        g.content = {0, 0, 1, 1};
        g.crop = {0, 0, 1, 1};
        g.stretch = true;
        g.calibration = settings.fusionCalibration;
    }
    if (settings.simpleMapping) {
        g.content = {0, 0, 1, 1}; g.crop = {0, 0, 1, 1};
        // 采集播放器自己拥有整块输出屏，基础模式保持画面比例；手工框选的旧模式仍把所选矩形视为实际视频区。
        g.stretch = !IsCaptureDisplayMode(settings.mode);
    }
    std::string error; return qp::ValidateGeometry(g, error);
}

void Application::AdoptLatestCaptureFrame() {
    auto latest = captureOutput.LatestFrame();
    if (latest == presentedCaptureFrame) return;
    if (latest) {
        if (captureKnownWidth && (latest->width != captureKnownWidth || latest->height != captureKnownHeight)) {
            const double previousAspect = static_cast<double>(captureKnownWidth) / captureKnownHeight;
            const double currentAspect = static_cast<double>(latest->width) / latest->height;
            if (std::abs(previousAspect - currentAspect) >
                1e-6 * std::max({1.0, std::abs(previousAspect), std::abs(currentAspect)})) {
                settings.setupComplete = false;
                MarkDirty();
                captureRuntimeNotice = W(L"采集画面比例已变化，请重新检查四角和中心。",
                    L"采集画面比例变啦，请重新检查四角和中心喵。");
            }
        }
        captureKnownWidth = latest->width;
        captureKnownHeight = latest->height;
    }
    presentedCaptureFrame = std::move(latest);
}

void Application::ProcessPointerDatagrams() {
    if (running) {
        for (auto& data : receiver.Drain()) {
            qp::Packet packet;
            std::string error;
            if (!qp::ParsePacket(data.payload, packet, error)) { ++rejected; continue; }
            // 留空来源过滤时，一次接收会话只跟随首个合法发送 IP，避免混合两台设备的状态。
            if (!sender.empty() && sender != Wide(data.sourceIp)) { ++rejected; continue; }
            qp::PointerState before = state;
            if (!qp::ApplyPacket(state, packet, data.receivedMs)) { ++rejected; continue; }
            ++accepted;
            sender = Wide(data.sourceIp);
            ObserveCalibrationPacket(packet, data.receivedMs);
            // 与静止区间起点比较，连续细小移动也会累计越过阈值。
            if (!before.hasPosition ||
                std::hypot(stableAnchor.x - state.position.x, stableAnchor.y - state.position.y) > 0.0001 ||
                before.down != state.down || before.show != state.show) {
                stableAnchor = state.position;
                lastMove = data.receivedMs;
            }
            if (settings.delayMs == 0) {
                displayed = state;
            } else {
                // 完整状态按接收时刻排队，显示延迟不会把按下/显隐拆开。
                delayed.push_back(state);
                if (delayed.size() > 4096) delayed.pop_front();
            }
        }
    } else {
        receiver.Drain();
    }
    // Drain 之后确认，若其间又有报文进入，接收线程会重新投递一次合并通知。
    receiver.AcknowledgeNotification();
}

void Application::SubmitCapturePresentation() {
    if (!capturePresenter.Active() || !captureWindow ||
            !IsOwnedFullscreenDisplayMode(settings.mode)) return;
    if (IsCaptureDisplayMode(settings.mode) && !presentedCaptureFrame) return;
    qp::Geometry geometry;
    bool pointerGeometryReady = Geometry(geometry, true);
    if (!pointerGeometryReady) {
        geometry = settings.geometry;
        qp::Rect target;
        if (!ClientRectOnScreen(captureWindow, target)) return;
        geometry.target = target;
        if (IsCaptureDisplayMode(settings.mode)) {
            geometry.frameWidth = presentedCaptureFrame->width;
            geometry.frameHeight = presentedCaptureFrame->height;
        } else {
            geometry.frameWidth = target.w;
            geometry.frameHeight = target.h;
            geometry.content = {0, 0, 1, 1};
            geometry.crop = {0, 0, 1, 1};
            geometry.stretch = true;
            geometry.calibration = settings.fusionCalibration;
        }
        if (settings.simpleMapping) {
            geometry.content = {0, 0, 1, 1};
            geometry.crop = {0, 0, 1, 1};
            geometry.stretch = !IsCaptureDisplayMode(settings.mode);
        }
        std::string geometryError;
        if (!qp::ValidateGeometry(geometry, geometryError)) return;
    }
    qp::CapturePresentation presentation;
    presentation.frame = IsCaptureDisplayMode(settings.mode) ? presentedCaptureFrame : nullptr;
    presentation.backgroundColor = IsFusionDisplayMode(settings.mode)
        ? settings.fusionBackgroundColor : RGB(0, 0, 0);
    presentation.geometry = geometry;
    presentation.pointer = displayed;
    presentation.showPointer = pointerGeometryReady && running && !paused && !demoActive &&
        !captureWaiting && !snapshot && !region && !alignmentCheck && !captureRangeEditing;
    presentation.pointerShape = settings.shape;
    presentation.pointerSize = settings.size;
    presentation.pointerColor = settings.color;
    presentation.customCursorImage = customCursorImage;
    presentation.customCursorHotspotX = customCursorHotspotX;
    presentation.customCursorHotspotY = customCursorHotspotY;
    presentation.showHud = outputHudVisible && !captureRangeEditing;
    presentation.hudStatusText = outputHudStatusText;
    presentation.hudShortcutText = outputHudShortcutText;
    // 常驻 HUD 已包含可操作快捷键，旧的启动提示条不再重复占用画面。
    presentation.showHint = false;
    presentation.hintText = CaptureExitHint();
    const qp::HotkeyBinding pauseBinding{settings.pauseHotkey, settings.pauseHotkeyModifiers};
    if (pauseBinding.virtualKey) {
        presentation.hintText += L" · " + qp::HotkeyText(pauseBinding) +
            W(L" 只暂停指针", L" 只暂停指针喵");
    } else {
        presentation.hintText += W(L" · 指针暂停热键已关闭", L" · 指针暂停热键关着喵");
    }
    presentation.showRangeEditor = captureRangeEditing;
    presentation.rangeRect = captureRangeRect;
    presentation.shadeRangeOutside = !IsFusionDisplayMode(settings.mode);
    presentation.rangeHint = W(L"拖框内移动 · 拖边角缩放 · Enter 应用 · Esc 取消",
        L"拖动和缩放边框喵 · Enter 应用 · Esc 取消");
    if (captureRangeEditing) presentation.showHint = false;
    capturePresenter.Submit(std::move(presentation));
}

void Application::Render() {
    qp::Geometry g;
    const qp::PointerState& visible = demoActive ? demoState : displayed;
    if (!demoActive && IsOwnedFullscreenDisplayMode(settings.mode) &&
            captureWindow && capturePresenter.Active()) {
        ShowWindow(overlay, SW_HIDE);
        SubmitCapturePresentation();
        lastRenderOk = true;
        return;
    }
    if (demoActive) {
        qp::Rect area;
        if (!IsWindow(demoWindow) || IsIconic(demoWindow) || !ClientRectOnScreen(demoWindow, area)) { ShowWindow(overlay, SW_HIDE); return; }
        g = {}; g.target = {area.x + 20, area.y + 66, std::max(2.0, area.w - 40), std::max(2.0, area.h - 92)}; g.stretch = true;
    } else if (!running || paused || captureWaiting || snapshot || region || alignmentCheck || !Geometry(g, true)) { ShowWindow(overlay, SW_HIDE); return; }
    if (!visible.hasPosition || !visible.show) { ShowWindow(overlay, SW_HIDE); return; }
    auto point = qp::MapPoint(g, visible.position);
    if (!point) { ShowWindow(overlay, SW_HIDE); return; }
    const int cursorSize = std::clamp(settings.size, 12, 144);
    // 所有样式都让触点位于画布中心；两倍尺寸的透明边界可容纳箭头、极端热点和按下反馈。
    const int side = cursorSize * 2 + 24;
    const int hot = side / 2;
    bool rebuild = !cursorDc || cacheShape != settings.shape || cacheSize != cursorSize ||
        cacheColor != settings.color || cacheDown != visible.down ||
        cacheCustomCursor != customCursorImage.get() ||
        cacheCustomHotspotX != customCursorHotspotX || cacheCustomHotspotY != customCursorHotspotY;
    POINT pos{static_cast<LONG>(std::lround(point->x)) - hot,
        static_cast<LONG>(std::lround(point->y)) - hot};
    if (!rebuild && pos.x == lastPixel.x && pos.y == lastPixel.y && IsWindowVisible(overlay)) return;
    HDC screen = GetDC(nullptr);
    if (rebuild) {
        if (cursorDc) { SelectObject(cursorDc, cursorOld); DeleteObject(cursorBitmap); DeleteDC(cursorDc); }
        cursorDc = CreateCompatibleDC(screen);
        BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = side; info.bmiHeader.biHeight = -side;
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
        void* pixels{}; cursorBitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!cursorBitmap || !cursorDc) { if (cursorBitmap) DeleteObject(cursorBitmap); if (cursorDc) DeleteDC(cursorDc); cursorBitmap = nullptr; cursorDc = nullptr; cacheShape = -1; ReleaseDC(nullptr, screen); return; }
        cursorOld = SelectObject(cursorDc, cursorBitmap);
        Gdiplus::Bitmap bitmap(side, side, side * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(pixels));
        Gdiplus::Graphics graphics(&bitmap); graphics.Clear(Gdiplus::Color(0, 0, 0, 0)); graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        const Gdiplus::Color accent(255, GetRValue(settings.color), GetGValue(settings.color),
            GetBValue(settings.color));
        Gdiplus::SolidBrush fill(accent), white(Gdiplus::Color(255, 255, 255, 255));
        Gdiplus::SolidBrush pressed(Gdiplus::Color(88, 255, 155, 45));
        const float size = static_cast<float>(cursorSize);
        const float center = static_cast<float>(hot);
        const float accentWidth = std::max(2.0f, size * .07f);
        const float outlineWidth = std::max(2.0f, size * .04f);
        Gdiplus::Pen outline(Gdiplus::Color(190, 0, 0, 0), outlineWidth);
        Gdiplus::Pen line(accent, accentWidth);
        if (visible.down) {
            const float radius = size * .42f;
            graphics.FillEllipse(&pressed, center - radius, center - radius,
                radius * 2, radius * 2);
        }
        const auto drawCrosshair = [&] {
            const float gap = size * .15f, arm = size * .43f;
            graphics.DrawLine(&line, center - arm, center, center - gap, center);
            graphics.DrawLine(&line, center + gap, center, center + arm, center);
            graphics.DrawLine(&line, center, center - arm, center, center - gap);
            graphics.DrawLine(&line, center, center + gap, center, center + arm);
            graphics.DrawEllipse(&line, center - size * .08f, center - size * .08f,
                size * .16f, size * .16f);
        };
        const auto drawDot = [&] {
            const float radius = size * .18f;
            graphics.FillEllipse(&fill, center - radius, center - radius, radius * 2, radius * 2);
            graphics.DrawEllipse(&outline, center - radius, center - radius, radius * 2, radius * 2);
        };
        if (settings.shape == 0) {
            const float s = size / 48.0f;
            Gdiplus::PointF points[] = {{center, center}, {center, center + 34*s},
                {center + 9*s, center + 26*s}, {center + 16*s, center + 40*s},
                {center + 25*s, center + 36*s}, {center + 18*s, center + 23*s},
                {center + 33*s, center + 23*s}};
            graphics.FillPolygon(&white, points, 7); graphics.DrawPolygon(&outline, points, 7);
        } else if (settings.shape == 1) {
            drawDot();
        } else if (settings.shape == 2) {
            drawCrosshair();
        } else if (settings.shape == 3) {
            graphics.DrawEllipse(&line, center - size * .30f, center - size * .30f,
                size * .60f, size * .60f);
            graphics.DrawEllipse(&line, center - size * .06f, center - size * .06f,
                size * .12f, size * .12f);
        } else if (settings.shape == 4) {
            graphics.FillEllipse(&fill, center - size * .08f, center - size * .08f,
                size * .16f, size * .16f);
            const float radius = size * (visible.down ? .42f : .28f);
            graphics.DrawEllipse(&line, center - radius, center - radius, radius * 2, radius * 2);
        } else if (settings.shape == 5) {
            const float half = size * .32f, corner = size * .16f;
            const float left = center - half, right = center + half;
            const float top = center - half, bottom = center + half;
            graphics.DrawLine(&line, left, top, left + corner, top);
            graphics.DrawLine(&line, left, top, left, top + corner);
            graphics.DrawLine(&line, right, top, right - corner, top);
            graphics.DrawLine(&line, right, top, right, top + corner);
            graphics.DrawLine(&line, left, bottom, left + corner, bottom);
            graphics.DrawLine(&line, left, bottom, left, bottom - corner);
            graphics.DrawLine(&line, right, bottom, right - corner, bottom);
            graphics.DrawLine(&line, right, bottom, right, bottom - corner);
            graphics.DrawEllipse(&line, center - size * .05f, center - size * .05f,
                size * .10f, size * .10f);
        } else if (settings.shape == 6) {
            for (int index = 0; index <= 3; ++index) {
                const BYTE alpha = static_cast<BYTE>(70 + index * 45);
                Gdiplus::Pen trail(Gdiplus::Color(alpha, GetRValue(settings.color),
                    GetGValue(settings.color), GetBValue(settings.color)), accentWidth);
                const float step = static_cast<float>(index);
                const float radius = size * (.05f + step * .02f);
                graphics.DrawEllipse(&trail,
                    center - size * (.26f - step * .07f) - radius,
                    center + size * (.18f - step * .05f) - radius,
                    radius * 2, radius * 2);
            }
            drawDot();
        } else if (settings.shape == 7 && customCursorImage &&
                customCursorImage->width > 0 && customCursorImage->height > 0 &&
                customCursorImage->width <= std::numeric_limits<int>::max() / 4 &&
                customCursorImage->stride >= customCursorImage->width * 4 &&
                static_cast<size_t>(customCursorImage->height) <=
                    std::numeric_limits<size_t>::max() /
                        static_cast<size_t>(customCursorImage->stride) &&
                customCursorImage->premultipliedBgra.size() >=
                    static_cast<size_t>(customCursorImage->stride) *
                        static_cast<size_t>(customCursorImage->height)) {
            auto* customPixels = const_cast<BYTE*>(reinterpret_cast<const BYTE*>(
                customCursorImage->premultipliedBgra.data()));
            Gdiplus::Bitmap image(customCursorImage->width, customCursorImage->height,
                customCursorImage->stride, PixelFormat32bppPARGB, customPixels);
            const float imageScale = size / static_cast<float>(
                std::max(customCursorImage->width, customCursorImage->height));
            const float width = customCursorImage->width * imageScale;
            const float height = customCursorImage->height * imageScale;
            const float hotspotX = static_cast<float>(std::clamp(customCursorHotspotX, 0.0, 1.0));
            const float hotspotY = static_cast<float>(std::clamp(customCursorHotspotY, 0.0, 1.0));
            graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            graphics.DrawImage(&image, center - hotspotX * width, center - hotspotY * height,
                width, height);
        } else {
            drawCrosshair();
        }
        cacheShape = settings.shape; cacheSize = cursorSize; cacheColor = settings.color;
        cacheDown = visible.down; cacheCustomCursor = customCursorImage.get();
        cacheCustomHotspotX = customCursorHotspotX; cacheCustomHotspotY = customCursorHotspotY;
    }
    SIZE size{side, side}; POINT zero{}; BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    lastRenderOk = UpdateLayeredWindow(overlay, screen, &pos, &size, cursorDc, &zero, 0, &blend, ULW_ALPHA) != FALSE;
    ReleaseDC(nullptr, screen); SetWindowPos(overlay, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW); lastPixel = pos;
}

void Application::Tick() {
    uint64_t now = GetTickCount64();
    AdoptLatestCaptureFrame();
    if (captureFullscreenRequested && IsOwnedFullscreenDisplayMode(settings.mode) &&
            !captureWindow && now >= captureCreateRetryAt) {
        auto capture = captureOutput.Status(); auto frame = presentedCaptureFrame;
        const bool outputReady = IsFusionDisplayMode(settings.mode) ||
            (capture.state == qp::CaptureState::Running && frame && Age(now, frame->capturedMs) <= 1000);
        if (outputReady) {
            captureWindow = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"QPointerCaptureOutput",
                IsFusionDisplayMode(settings.mode)
                    ? W(L"QInEX Pointer · 纯色融合输出", L"QInEX Pointer · 纯色融合输出喵")
                    : W(L"QInEX Pointer · 采集卡全屏输出", L"QInEX Pointer · 采集卡全屏输出喵"),
                WS_POPUP, 0, 0, 640, 360, nullptr, nullptr, instance, nullptr);
            if (captureWindow) {
                captureCreateRetryAt = 0;
                captureFallbackHotkeyReady = RegisterHotKey(main, 2, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F12) != FALSE;
                RefreshOutputHotkeys();
                if (!captureExitHotkeyReady && !captureFallbackHotkeyReady)
                    captureRuntimeNotice = W(L"全局退出键被占用；请用 Esc、右键菜单或托盘返回设置。", L"全局退出键被占用啦，请用 Esc、右键菜单或托盘返回设置喵。");
                PositionCaptureOutput();
                if (captureWindow) {
                    SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED);
                    UpdateOutputHud(now);
                    // 隐藏状态下先把客户区铺成最终底色，避免融合器看到启动黑闪或状态文字。
                    if (HDC dc = GetDC(captureWindow)) {
                        PaintCaptureOutput(dc);
                        ReleaseDC(captureWindow, dc);
                    }
                    std::wstring presenterError;
                    const bool extremeLowLatency = IsFusionDisplayMode(settings.mode)
                        ? settings.fusionPresentMode == 1 : settings.capturePresentMode == 1;
                    if (capturePresenter.Start(captureWindow, extremeLowLatency, presenterError)) {
                        // D3D 初始化期间采集线程仍会持续收帧。启动完成后先追到最新帧，
                        // 避免把建窗前的旧帧等待时间误记为采集回调到呈现延迟。
                        if (IsCaptureDisplayMode(settings.mode)) AdoptLatestCaptureFrame();
                        SubmitCapturePresentation();
                        if (captureExitHotkeyReady || captureFallbackHotkeyReady) {
                            if (IsFusionDisplayMode(settings.mode)) {
                                captureRuntimeNotice = extremeLowLatency
                                    ? W(L"纯色背景与指针正在极限低延迟输出；采集卡未被占用。",
                                        L"纯色背景和指针正在极限低延迟输出喵；没有占用采集卡。")
                                    : W(L"纯色背景与指针正在同步输出；采集卡未被占用。",
                                        L"纯色背景和指针正在同步输出喵；没有占用采集卡。");
                            } else {
                                captureRuntimeNotice = extremeLowLatency
                                    ? W(L"采集画面正在极限低延迟输出；指针与画面已在同一 GPU 表面合成。",
                                        L"采集画面正在极限低延迟输出喵；指针和画面已经一起合成啦。")
                                    : W(L"采集画面正在低延迟同步输出；指针与画面已在同一 GPU 表面合成。",
                                        L"采集画面正在低延迟同步输出喵；指针和画面已经一起合成啦。");
                            }
                            if (!captureExitHotkeyReady && captureFallbackHotkeyReady &&
                                !FindWindowW(L"QInEXLinkMain", nullptr))
                                captureRuntimeNotice += L"\r\n" +
                                    qp::HotkeyText({settings.exitHotkey, settings.exitHotkeyModifiers}) +
                                    W(L" 全局热键被占用；窗口失去焦点时请用 Ctrl+Alt+F12。",
                                        L" 全局热键被占用喵；窗口失焦时用 Ctrl+Alt+F12。");
                        }
                    } else {
                        captureRuntimeNotice = W(L"GPU 低延迟呈现不可用，已使用兼容显示：",
                            L"GPU 低延迟呈现不可用，先用兼容显示喵：") + presenterError;
                    }
                    ShowWindow(captureWindow, SW_SHOW);
                    UpdateWindow(captureWindow);
                    SetForegroundWindow(captureWindow);
                    if (calibrationPrompt)
                        SetWindowPos(calibrationPrompt, HWND_TOPMOST, 0, 0, 0, 0,
                            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
                }
            } else {
                captureCreateRetryAt = now + 1000;
                captureRuntimeNotice = W(L"未能创建全屏输出窗口，正在重试。", L"全屏输出窗口没创建成功，正在重试喵。");
            }
        }
    }
    if (captureRangePending && captureWindow && !captureRangeEditing)
        BeginCaptureRangeAdjustment();
    if (captureWindow && !capturePresenter.Active()) InvalidateRect(captureWindow, nullptr, FALSE);
    ProcessPointerDatagrams();
    if (running) {
        now = GetTickCount64();
        if (state.lastPacketMs && Age(now, state.lastPacketMs) > static_cast<uint64_t>(settings.timeoutMs)) {
            state.show = false; state.down = false; displayed.show = false; displayed.down = false; delayed.clear();
        }
        while (!delayed.empty() && now >= delayed.front().lastPacketMs + static_cast<uint64_t>(settings.delayMs)) { displayed = delayed.front(); delayed.pop_front(); }
    }
    if (demoActive) {
        for (auto& data : demoReceiver.Drain()) {
            qp::Packet packet; std::string error;
            if (qp::ParsePacket(data.payload, packet, error) && qp::ApplyPacket(demoState, packet, data.receivedMs)) ++demoAccepted;
        }
        if (now >= demoDue || !demoSender.Active()) EndDemo();
    }
    TickCalibration(now);
    Render();
    // 运行指标每秒刷新已足够判断状态；用户操作仍把 statusAt 清零并立即反馈。
    // 避免帧率与呈现耗时的小幅波动让原生文本控件持续重绘。
    if (Age(now, statusAt) >= 1000) { statusAt = now; UpdateStatus(); }
}

void Application::BeginRegion() {
    if (demoActive) { EndDemo(); }
    if (calibrationPhase != CapturePhase::Idle) { notice = W(L"请先完成或取消本次取样。", L"请先完成或取消本次取样喵。"); statusAt = 0; return; }
    if (region) { SetForegroundWindow(region); return; }
    Settings next; if (!ReadPage(next, 2)) return;
    regionBefore = settings;
    regionBeforeWindow = targetWindow;
    // 框选只应用画面草稿，不改连接设置，取消可恢复原画面。
    settings = next;
    if (next.mode == 2) {
        int index = Selected(ID_WINDOW);
        if (index >= 0 && index < static_cast<int>(windows.size()) && windows[index].title == next.windowTitle) targetWindow = windows[index].hwnd;
    }
    qp::Geometry g; if (!Geometry(g)) { settings = *regionBefore; targetWindow = regionBeforeWindow; regionBefore.reset(); Error(W(L"请先选择可见的预览窗口或桌面区域。", L"先选可见的预览窗口或桌面区域喵。")); return; }
    auto r = g.target; regionOriginal = r;
    ShowWindow(overlay, SW_HIDE);
    const auto title = settings.simpleMapping ? W(L"框住完整手机画面 · 拖边框调整 · Enter 确认 · Esc 取消", L"框住完整手机画面喵 · Enter 确认 · Esc 取消") : W(L"框住视频容器 · 拖边框调整 · Enter 确认 · Esc 取消", L"框住视频容器喵 · Enter 确认 · Esc 取消");
    region = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED, L"QPointerRegion", title, WS_POPUP | WS_THICKFRAME | WS_CAPTION,
        static_cast<int>(r.x), static_cast<int>(r.y), static_cast<int>(r.w), static_cast<int>(r.h), main, nullptr, instance, nullptr);
    if (!region) { settings = *regionBefore; targetWindow = regionBeforeWindow; regionBefore.reset(); return; }
    SetLayeredWindowAttributes(region, 0, 75, LWA_ALPHA); ShowWindow(region, SW_SHOW); SetForegroundWindow(region);
}

void Application::FinishRegion(bool commit) {
    if (!region) return;
    if (commit) {
        RECT rect{}; GetWindowRect(region, &rect); qp::Rect r = FromRect(rect);
        if (settings.mode == 2) {
            qp::Rect base; if (!ClientRectOnScreen(targetWindow, base)) { Error(W(L"预览窗口已关闭。按 Esc 取消后重新选择。", L"预览窗口关掉啦，按 Esc 后重新选喵。")); return; }
            qp::Rect relative{(r.x - base.x) / base.w, (r.y - base.y) / base.h, r.w / base.w, r.h / base.h};
            if (relative.x < -.001 || relative.y < -.001 || relative.x + relative.w > 1.001 || relative.y + relative.h > 1.001) { Error(W(L"请将边框完整放在预览窗口的画面区域内。", L"请把边框完整放进预览画面里喵。")); return; }
            relative.x = std::max(0.0, relative.x); relative.y = std::max(0.0, relative.y); relative.w = std::min(relative.w, 1 - relative.x); relative.h = std::min(relative.h, 1 - relative.y);
            settings.windowRegion = relative;
        } else { settings.mode = 1; settings.geometry.target = r; }
        settings.setupComplete = false; MarkDirty();
        notice = W(L"区域已选好，请检查四角和中心的对应位置。", L"区域选好啦，请检查四角和中心喵。");
    } else if (regionBefore) { settings = *regionBefore; targetWindow = regionBeforeWindow; }
    regionBefore.reset();
    HWND h = region; region = nullptr; DestroyWindow(h); ReloadPage(2); ShowControlWindow();
    statusAt = 0;
}

void Application::Simulate() {
    if (demoActive) { EndDemo(); return; }
    if (calibrationPhase != CapturePhase::Idle || region) {
        notice = W(L"请先完成或取消当前取样／框选，再体验演示。", L"先完成或取消当前取样和框选，再体验演示喵。"); statusAt = 0; return;
    }
    std::string error;
    bool opened = false;
    for (int i = 0; i < 32; ++i) {
        const auto candidate = static_cast<uint16_t>(50000 + (GetTickCount64() + i) % 10000);
        if (candidate != settings.port && demoReceiver.Start(candidate, "127.0.0.1", error)) { demoPort = candidate; opened = true; break; }
    }
    if (!opened) { Error(W(L"演示暂时无法启动：", L"演示暂时没启动喵：") + Wide(error)); return; }
    std::string senderError;
    if (!demoSender.Start(demoPort, 15000, senderError)) {
        demoReceiver.Stop(); Error(W(L"演示暂时无法启动：", L"演示暂时没启动喵：") + Wide(senderError)); return;
    }
    demoState = {}; demoAccepted = 0; demoActive = true; demoDue = GetTickCount64() + 16000;
    demoWindow = CreateWindowExW(WS_EX_TOPMOST, L"QPointerDemo", W(L"QInEX Pointer · 独立演示", L"QInEX Pointer · 独立演示喵"), WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, Px(640), Px(420), main, nullptr, instance, nullptr);
    if (!demoWindow) { EndDemo(); Error(W(L"演示窗口创建失败，请稍后重试。", L"演示窗口没打开喵，请稍后重试。")); return; }
    ShowWindow(demoWindow, SW_SHOW); UpdateWindow(demoWindow);
    notice = W(L"演示将在 15 秒后结束；设备连接、画面和校准保持原样。", L"演示 15 秒后结束，设备连接、画面和校准都保留喵。"); statusAt = 0;
}

void Application::EndDemo() {
    demoActive = false; demoReceiver.Stop(); demoState = {}; demoDue = 0;
    demoSender.Stop();
    if (demoWindow) { HWND h = demoWindow; demoWindow = nullptr; DestroyWindow(h); }
    if (!shuttingDown) { notice = W(L"演示已结束，继续使用原设备设置。", L"演示结束啦，继续用原来的设备设置喵。"); statusAt = 0; Render(); }
}

void Application::Shutdown() {
    shuttingDown = true;
    uiTransition.Cancel();
#if QINEX_WITH_ANDROID_TOOLS
    SaveAndroidPreferences();
    androidTools.Stop();
#endif
    StopCaptureOutput(); EndDemo(); Stop();
    UnregisterHotKey(main, 1);
    if (trayInstalled) { NOTIFYICONDATAW data{sizeof(data)}; data.hWnd = main; data.uID = 1; Shell_NotifyIconW(NIM_DELETE, &data); trayInstalled = false; }
    if (alignmentCheck) { DestroyWindow(alignmentCheck); alignmentCheck = nullptr; }
    if (snapshot) { DestroyWindow(snapshot); snapshot = nullptr; }
    if (region) { DestroyWindow(region); region = nullptr; }
    if (captured) { DeleteObject(captured); captured = nullptr; }
    if (cursorDc) { SelectObject(cursorDc, cursorOld); DeleteObject(cursorBitmap); DeleteDC(cursorDc); cursorDc = nullptr; }
    if (font) { DeleteObject(font); font = nullptr; }
    if (headingFont) { DeleteObject(headingFont); headingFont = nullptr; }
    if (uiSmallFont) { DeleteObject(uiSmallFont); uiSmallFont = nullptr; }
    if (uiStrongFont) { DeleteObject(uiStrongFont); uiStrongFont = nullptr; }
}

LRESULT CALLBACK CaptureOutputProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT:
    case WM_PRINTCLIENT: {
        PAINTSTRUCT ps{}; HDC dc = msg == WM_PAINT ? BeginPaint(h, &ps) : reinterpret_cast<HDC>(wp);
        // GPU 呈现期间 WM_PAINT 只确认脏区；打印/截图仍走确定性的 CPU 路径。
        if (app && (msg == WM_PRINTCLIENT || !app->capturePresenter.Active())) app->PaintCaptureOutput(dc);
        if (msg == WM_PAINT) EndPaint(h, &ps); return 0;
    }
    case WM_SIZE:
        if (app) app->capturePresenter.OnResize();
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SETCURSOR:
        if (app && app->captureRangeEditing) {
            POINT point{}; GetCursorPos(&point); ScreenToClient(h, &point);
            const int hit = app->CaptureRangeHitTest(point);
            const bool horizontal = hit & (kRangeLeft | kRangeRight);
            const bool vertical = hit & (kRangeTop | kRangeBottom);
            LPCWSTR cursor = IDC_ARROW;
            if (hit == kRangeMove) cursor = IDC_SIZEALL;
            else if (horizontal && vertical)
                cursor = ((hit & kRangeLeft) != 0) == ((hit & kRangeTop) != 0) ? IDC_SIZENWSE : IDC_SIZENESW;
            else if (horizontal) cursor = IDC_SIZEWE;
            else if (vertical) cursor = IDC_SIZENS;
            SetCursor(LoadCursorW(nullptr, cursor));
        } else SetCursor(nullptr);
        return TRUE;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        if (!app) break;
        // 全局注册失败时才走窗口内兜底；按键自动重复不重复切换 HUD 或校准状态。
        const bool firstPress = (static_cast<ULONG_PTR>(lp) & (ULONG_PTR{1} << 30)) == 0;
        if (!app->outputHudHotkeyReady && qp::MatchesHotkeyKeyDown(
                {app->settings.hudHotkey, app->settings.hudHotkeyModifiers}, wp)) {
            if (firstPress) app->ToggleOutputHud(); return 0;
        }
        if (app->captureRangeEditing) {
            if (wp == VK_RETURN) { app->FinishCaptureRangeAdjustment(true); return 0; }
            if (wp == VK_ESCAPE) { app->FinishCaptureRangeAdjustment(false); return 0; }
            if (!app->captureExitHotkeyReady && qp::MatchesHotkeyKeyDown(
                    {app->settings.exitHotkey, app->settings.exitHotkeyModifiers}, wp)) {
                if (firstPress) { app->FinishCaptureRangeAdjustment(false); app->ExitCaptureFullscreen(); }
                return 0;
            }
            if (qp::MatchesHotkeyKeyDown(
                    {app->settings.rangeHotkey, app->settings.rangeHotkeyModifiers}, wp)) return 0;
        }
        if (!app->captureRangeHotkeyReady && qp::MatchesHotkeyKeyDown(
                {app->settings.rangeHotkey, app->settings.rangeHotkeyModifiers}, wp)) {
            if (firstPress) app->BeginCaptureRangeAdjustment(); return 0;
        }
        if (wp == VK_ESCAPE || (!app->captureExitHotkeyReady && qp::MatchesHotkeyKeyDown(
                {app->settings.exitHotkey, app->settings.exitHotkeyModifiers}, wp))) {
            if (firstPress) app->ExitCaptureFullscreen(); return 0;
        }
        break;
    }
    case WM_LBUTTONDOWN:
        if (app && app->captureRangeEditing) {
            POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            const int hit = app->CaptureRangeHitTest(point);
            if (hit) {
                app->captureRangeDrag = hit;
                app->captureRangeDragStart = point;
                app->captureRangeDragOriginal = app->captureRangeRect;
                SetCapture(h);
            }
            return 0;
        }
        break;
    case WM_MOUSEMOVE:
        if (app && app->captureRangeEditing && app->captureRangeDrag) {
            app->UpdateCaptureRangeDrag({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (app && app->captureRangeEditing && app->captureRangeDrag) {
            app->UpdateCaptureRangeDrag({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            app->captureRangeDrag = 0;
            if (GetCapture() == h) ReleaseCapture();
            return 0;
        }
        break;
    case WM_CAPTURECHANGED:
        if (app) app->captureRangeDrag = 0;
        return 0;
    case WM_CONTEXTMENU:
    case WM_RBUTTONUP: {
        POINT point{};
        if (msg == WM_CONTEXTMENU && static_cast<short>(LOWORD(lp)) != -1) {
            point = {static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
        } else GetCursorPos(&point);
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        HMENU menu = CreatePopupMenu();
        if (app && app->captureRangeEditing) {
            AppendMenuW(menu, MF_STRING, 6, W(L"应用当前范围", L"应用当前范围喵"));
            AppendMenuW(menu, MF_STRING, 7, W(L"取消范围调整", L"取消范围调整喵"));
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        } else {
            const std::wstring rangeItem = W(L"调整光标范围（", L"调整光标范围喵（") +
                qp::HotkeyText({app->settings.rangeHotkey, app->settings.rangeHotkeyModifiers}) + L"）";
            AppendMenuW(menu, MF_STRING, 8, rangeItem.c_str());
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        }
        const std::wstring hudItem = (app && app->outputHudVisible
            ? std::wstring(W(L"隐藏底部 HUD（", L"隐藏底部 HUD 喵（"))
            : std::wstring(W(L"显示底部 HUD（", L"显示底部 HUD 喵（"))) +
            qp::HotkeyText({app->settings.hudHotkey, app->settings.hudHotkeyModifiers}) + L"）";
        AppendMenuW(menu, MF_STRING, 9, hudItem.c_str());
        AppendMenuW(menu, MF_STRING, 1, W(L"打开控制面板", L"打开控制面板喵"));
        const bool fusionOutput = app && IsFusionDisplayMode(app->settings.mode);
        if (!fusionOutput)
            AppendMenuW(menu, MF_STRING, 2, W(L"校准光标", L"校准光标喵"));
        AppendMenuW(menu, MF_STRING, 3, app && app->paused ? W(L"恢复指针", L"恢复指针喵") : W(L"暂停指针", L"暂停指针喵"));
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 4, fusionOutput
            ? W(L"退出纯色融合全屏", L"退出纯色融合全屏喵")
            : W(L"退出采集全屏", L"退出采集全屏喵"));
        if (!fusionOutput)
            AppendMenuW(menu, MF_STRING, 5, W(L"停止采集", L"停止采集喵"));
        const int selected = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
            point.x, point.y, 0, h, nullptr);
        DestroyMenu(menu);
        if (!app) return 0;
        if (selected == 6) app->FinishCaptureRangeAdjustment(true);
        else if (selected == 7) app->FinishCaptureRangeAdjustment(false);
        else if (selected == 8) app->BeginCaptureRangeAdjustment();
        else if (selected == 9) app->ToggleOutputHud();
        else if (selected == 1) app->ShowControlWindow();
        else if (selected == 2) { app->ShowControlWindow(); app->SelectPage(3); }
        else if (selected == 3) app->TogglePause();
        else if (selected == 4) app->ExitCaptureFullscreen();
        else if (selected == 5) { app->StopCaptureOutput(); app->ShowControlWindow(); }
        return 0;
    }
    case WM_CLOSE: app->ExitCaptureFullscreen(); return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xfff0) == SC_SCREENSAVE || (wp & 0xfff0) == SC_MONITORPOWER) return 0;
        break;
    case WM_DESTROY:
        if (app && app->captureWindow == h) {
            if (GetCapture() == h) ReleaseCapture();
            app->captureRangeEditing = false;
            app->captureRangePending = false;
            app->captureRangeDrag = 0;
            app->captureRangeGeometry = {};
            app->capturePresenter.Stop();
            app->captureWindow = nullptr;
        }
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

LRESULT CALLBACK RegionProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_KEYDOWN:
        if (wp == VK_RETURN) { app->FinishRegion(true); return 0; }
        if (wp == VK_ESCAPE) { app->FinishRegion(false); return 0; }
        break;
    case WM_CLOSE: app->FinishRegion(false); return 0;
    case WM_NCHITTEST: {
        auto hit = DefWindowProcW(h, msg, wp, lp); return hit == HTCLIENT ? HTCAPTION : hit;
    }
    case WM_ERASEBKGND: {
        RECT r{}; GetClientRect(h, &r); HBRUSH brush = CreateSolidBrush(RGB(30, 180, 220)); FillRect(reinterpret_cast<HDC>(wp), &r, brush); DeleteObject(brush); return 1;
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void Application::ShowTrayMenu(POINT p) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, hostWindow ? W(L"PC 光标设置", L"PC 光标设置喵") : W(L"打开 QInEX Pointer", L"打开 QInEX Pointer 喵"));
    AppendMenuW(menu, MF_STRING, 2, paused ? W(L"恢复指针", L"恢复指针喵") : W(L"暂停指针", L"暂停指针喵"));
    if (captureOutput.Active()) {
        AppendMenuW(menu, MF_STRING, 4, captureWindow ? W(L"退出采集全屏", L"退出采集全屏喵") : W(L"进入采集全屏", L"进入采集全屏喵"));
        AppendMenuW(menu, MF_STRING, 5, W(L"停止采集画面", L"停止采集画面喵"));
    } else if (IsFusionDisplayMode(settings.mode)) {
        AppendMenuW(menu, MF_STRING, 4, captureWindow
            ? W(L"退出纯色融合全屏", L"退出纯色融合全屏喵")
            : W(L"进入纯色融合全屏", L"进入纯色融合全屏喵"));
    }
    if (captureWindow) {
        const std::wstring hudItem = (outputHudVisible
            ? std::wstring(W(L"隐藏底部 HUD（", L"隐藏底部 HUD 喵（"))
            : std::wstring(W(L"显示底部 HUD（", L"显示底部 HUD 喵（"))) +
            qp::HotkeyText({settings.hudHotkey, settings.hudHotkeyModifiers}) + L"）";
        const std::wstring rangeItem = W(L"调整光标范围（", L"调整光标范围喵（") +
            qp::HotkeyText({settings.rangeHotkey, settings.rangeHotkeyModifiers}) + L"）";
        AppendMenuW(menu, MF_STRING, 6, hudItem.c_str());
        AppendMenuW(menu, MF_STRING, 7, rangeItem.c_str());
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    if (!hostWindow) AppendMenuW(menu, MF_STRING, 3, W(L"退出程序", L"退出程序喵"));
    AppendMenuW(menu, MF_STRING, 8, running ? W(L"断开光标设备", L"断开光标设备喵") : W(L"连接光标设备", L"连接光标设备喵"));
    SetMenuDefaultItem(menu, 1, FALSE);
    HWND owner = hostWindow ? hostWindow : main;
    SetForegroundWindow(owner);
    int selected = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, owner, nullptr); DestroyMenu(menu);
    if (selected == 1) ShowControlWindow();
    else if (selected == 2) TogglePause();
    else if (selected == 8) {
        if (running) Stop();
        else if (!settings.setupComplete || HasDraft()) { ShowControlWindow(); SelectPage(1); }
        else if (Start()) paused = false;
    }
    else if (selected == 4) {
        if (captureWindow) ExitCaptureFullscreen();
        else if (IsFusionDisplayMode(settings.mode)) StartFusionOutput();
        else { captureFullscreenRequested = true; captureCreateRetryAt = 0; captureHintUntil = GetTickCount64() + 8000; statusAt = 0; }
    }
    else if (selected == 5) { StopCaptureOutput(); ShowControlWindow(); }
    else if (selected == 6) ToggleOutputHud();
    else if (selected == 7) BeginCaptureRangeAdjustment();
    else if (selected == 3) { RequestExit(); return; }
    PostMessageW(owner, WM_NULL, 0, 0);
}

LRESULT CALLBACK MainProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    static const UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    if (taskbarCreated && msg == taskbarCreated) {
        app->trayInstalled = false;
        app->UpdateTray(true);
        return 0;
    }
    switch (msg) {
    case WM_CREATE:
        app->main = h; app->Inventory(); app->BuildControls(); SetTimer(h, 1, 15, nullptr); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{}; HDC dc = BeginPaint(h, &paint);
        app->PaintChrome(dc); EndPaint(h, &paint); return 0;
    }
    case WM_PRINTCLIENT:
        app->PaintChrome(reinterpret_cast<HDC>(wp)); return 0;
    case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: case WM_CTLCOLORBTN:
        return app->ThemeControlColor(reinterpret_cast<HDC>(wp), reinterpret_cast<HWND>(lp), msg);
    case WM_MEASUREITEM: {
        auto* item = reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
        if (item->CtlType == ODT_COMBOBOX) { item->itemHeight = app->Px(32); return TRUE; }
        break;
    }
    case WM_NOTIFY:
        if (reinterpret_cast<NMHDR*>(lp)->idFrom == ID_TAB && reinterpret_cast<NMHDR*>(lp)->code == TCN_SELCHANGE) app->SelectPage(TabCtrl_GetCurSel(app->Item(ID_TAB)));
        return 0;
    case WM_COMMAND: {
#if QINEX_WITH_ANDROID_TOOLS
        if (app->HandleAndroidCommand(LOWORD(wp), HIWORD(wp))) return 0;
#endif
        int id = LOWORD(wp), event = HIWORD(wp);
        if ((event == BN_CLICKED || (id == Application::ID_CAL_LIST && event == LBN_SELCHANGE)) && app->HandleCalibrationCommand(id)) return 0;
        if (app->HandleUiCommand(id, event)) return 0;
        if (event != BN_CLICKED) break;
        switch (id) {
        case ID_START: app->Start(); break;
        case ID_STOP: app->Stop(); break;
        case ID_APPLY: app->Apply(false); break;
        case ID_SAVE: app->Apply(true); break;
        case ID_SIMULATOR: app->Simulate(); break;
        case ID_COLOR: {
            static COLORREF custom[16]{}; CHOOSECOLORW color{sizeof(color)}; color.hwndOwner = h; color.rgbResult = app->settings.color; color.lpCustColors = custom; color.Flags = CC_FULLOPEN | CC_RGBINIT;
            if (ChooseColorW(&color)) { app->settings.color = color.rgbResult; app->cacheShape = -1; app->MarkDirty(); app->Render(); } break;
        }
        case ID_REFRESH: {
            app->Inventory(); auto combo = app->Item(ID_WINDOW); SendMessageW(combo, CB_RESETCONTENT, 0, 0);
            int selected = -1; for (size_t i = 0; i < app->windows.size(); ++i) { SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(app->windows[i].title.c_str())); if (app->windows[i].hwnd == app->targetWindow) selected = static_cast<int>(i); }
            if (!app->windows.empty()) SendMessageW(combo, CB_SETCURSEL, selected, 0); break;
        }
        case ID_REGION: app->BeginRegion(); break;
        case ID_ENTIRE: app->settings.windowRegion = {0, 0, 1, 1}; app->settings.setupComplete = false; app->MarkDirty(); app->notice = W(L"跟随区域已恢复为整个客户区。", L"跟随整个客户区啦喵。"); break;
        case ID_CAPTURE: app->BeginCapture(); break;
        case ID_FIT: app->Fit(); break;
        case ID_VERIFY: app->Verify(); break;
        case ID_GUIDE: app->ShowHelp(); break;
        }
        return 0;
    }
    case WM_TIMER:
        if (wp == 1) app->Tick();
        else if (wp == qp::UiTransition::kTimer) {
            if (app->UiMotionAllowed()) app->uiTransition.Advance();
            else app->uiTransition.Cancel();
        }
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE && app->uiTransition.Active()) app->uiTransition.Cancel();
        if (LOWORD(wp) != WA_INACTIVE && app->captureWindow && !IsIconic(h))
            SetWindowPos(app->captureWindow, HWND_NOTOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        break;
    case WM_SIZE:
        if (app->uiTransition.Active()) app->uiTransition.Cancel();
        if (wp == SIZE_MINIMIZED && app->captureWindow && !app->snapshot && !app->calibrationPrompt)
            app->FocusCaptureFullscreen();
        break;
    case WM_SHOWWINDOW:
        if (!wp && app->uiTransition.Active()) app->uiTransition.Cancel();
        break;
    case WM_SETTINGCHANGE:
        if (app->uiTransition.Active() && !app->UiMotionAllowed()) app->uiTransition.Cancel();
        break;
    case WM_DPICHANGED: {
        if (app->hostWindow) { app->BuildControls(); return 0; }
        auto* r = reinterpret_cast<RECT*>(lp); SetWindowPos(h, nullptr, r->left, r->top, r->right-r->left, r->bottom-r->top, SWP_NOACTIVATE | SWP_NOZORDER);
        app->BuildControls(); return 0;
    }
    case WM_DISPLAYCHANGE:
        app->Inventory();
        if (app->captureWindow) app->PositionCaptureOutput();
        app->BuildControls(); return 0;
    case WM_HOTKEY: {
        const int hotkeyId = static_cast<int>(wp);
        if (hotkeyId >= 1 && hotkeyId <= 5) {
            const uint64_t now = GetTickCount64();
            uint64_t& previous = app->hotkeyActionAt[static_cast<size_t>(hotkeyId)];
            if (previous && Age(now, previous) < 120) return 0;
            previous = now;
        }
        if (wp == 1) { if (app->calibrationPhase != Application::CapturePhase::Idle) app->CancelCalibration(); else app->TogglePause(); return 0; }
        if (wp == 2 || wp == 3) { app->ExitCaptureFullscreen(); return 0; }
        if (wp == 4) {
            app->BeginCaptureRangeAdjustment();
            return 0;
        }
        if (wp == 5) { app->ToggleOutputHud(); return 0; }
        return 0;
    }
#if QINEX_WITH_ANDROID_TOOLS
    case qp::android::kToolsMessage:
        app->UpdateAndroidTools(); return 0;
#endif
    case WM_DRAWITEM:
#if QINEX_WITH_ANDROID_TOOLS
        if (wp == Application::ID_ANDROID_PREVIEW) { app->PaintAndroidPreview(*reinterpret_cast<DRAWITEMSTRUCT*>(lp)); return TRUE; }
#endif
        if (app->DrawThemedItem(*reinterpret_cast<DRAWITEMSTRUCT*>(lp))) return TRUE;
        break;
    case WM_APP + 30:
        app->captureOutput.AcknowledgeNotification();
        app->AdoptLatestCaptureFrame();
        if (app->captureWindow) {
            if (app->capturePresenter.Active()) app->SubmitCapturePresentation();
            else InvalidateRect(app->captureWindow, nullptr, FALSE);
        }
        return 0;
    case WM_APP + 31:
        app->ProcessPointerDatagrams();
        app->Render();
        return 0;
    case WM_APP + 11: {
        const UINT event = app->trayVersion4 ? LOWORD(lp) : static_cast<UINT>(lp);
        if (event == NIN_SELECT || event == NIN_KEYSELECT || event == NIN_BALLOONUSERCLICK ||
                (!app->trayVersion4 && (event == WM_LBUTTONUP || event == WM_LBUTTONDBLCLK)))
            app->ShowControlWindow();
        else if (event == WM_CONTEXTMENU || (!app->trayVersion4 && event == WM_RBUTTONUP)) {
            POINT p{};
            if (app->trayVersion4) p = {GET_X_LPARAM(wp), GET_Y_LPARAM(wp)};
            else GetCursorPos(&p);
            app->ShowTrayMenu(p);
        } return 0;
    }
    case WM_CLOSE: app->HideToTray(); return 0;
    case WM_QUERYENDSESSION: return TRUE;
    case WM_ENDSESSION:
        if (wp) DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        KillTimer(h, 1); app->Shutdown();
        if (app->overlay) { DestroyWindow(app->overlay); app->overlay = nullptr; }
        if (app->hostWindow) RemovePropW(app->hostWindow, qp::desktop::kEmbeddedWindowProperty);
        app->main = nullptr;
        if (!app->hostWindow) PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

namespace qpointer::desktop {
namespace {
std::unique_ptr<Application> application;
ULONG_PTR graphicsToken{};
// 独立与嵌入初始化均使用逐屏 DPI；返回时恢复调用线程上下文。
struct DpiScope {
    DPI_AWARENESS_CONTEXT previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ~DpiScope() { if (previous) SetThreadDpiAwarenessContext(previous); }
};
}

bool ResumeRequested() {
    const auto settings = LoadSettings();
    return settings.resumeOnLaunch && settings.setupComplete;
}

bool Initialize(HINSTANCE instance, HWND host, int show, const RECT* bounds) {
    if (application) return IsWindow(app->main) != FALSE;
    DpiScope dpi;
    Gdiplus::GdiplusStartupInput input;
    if (Gdiplus::GdiplusStartup(&graphicsToken, &input, nullptr) != Gdiplus::Ok) return false;
    INITCOMMONCONTROLSEX common{sizeof(common), ICC_WIN95_CLASSES};
    if (!InitCommonControlsEx(&common)) { Shutdown(); return false; }
    application = std::make_unique<Application>();
    app = application.get(); app->instance = instance; app->hostWindow = host; app->settings = LoadSettings();
    app->applicationIcon = static_cast<HICON>(LoadImageW(instance,
        MAKEINTRESOURCEW(IDI_QINEX_POINTER), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_SHARED));
    app->applicationSmallIcon = static_cast<HICON>(LoadImageW(instance,
        MAKEINTRESOURCEW(IDI_QINEX_POINTER), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
    if (!app->applicationIcon) app->applicationIcon = LoadIconW(nullptr, IDI_APPLICATION);
    if (!app->applicationSmallIcon) app->applicationSmallIcon = app->applicationIcon;
    app->storageDirectory = ExeDirectory(); app->page = app->settings.setupComplete ? 0 : 1;
    if (!host) {
        int argc = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        for (int i = 1; i < argc; ++i) {
            if (wcscmp(argv[i], L"--page") == 0 && i + 1 < argc) app->page = std::clamp(_wtoi(argv[++i]), 0, 5);
        }
        if (argv) LocalFree(argv);
    }
    app->profiles.Load(app->storageDirectory + L"\\profiles");
    // 首次使用直接进入完整手机框选；已有配置保持原先几何解释。
    if (GetFileAttributesW(IniPath().c_str()) == INVALID_FILE_ATTRIBUTES) {
        app->settings.simpleMapping = true;
        // 仅新配置默认随宿主启动；旧配置的选择保持不变，完成向导保存后才生效。
        if (host) app->settings.resumeOnLaunch = true;
    }
    app->profiles.Remember(app->settings);
    Messages::anime = app->settings.anime;
    auto registerClass = [&](const wchar_t* name, WNDPROC proc, HBRUSH background) {
        WNDCLASSEXW c{sizeof(c)}; c.lpfnWndProc = proc; c.hInstance = instance; c.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        c.hIcon = app->applicationIcon; c.hIconSm = app->applicationSmallIcon;
        c.hbrBackground = background; c.lpszClassName = name; return RegisterClassExW(&c);
    };
    registerClass(L"QPointerMain", MainProc, nullptr);
    registerClass(L"QPointerOverlay", DefWindowProcW, nullptr);
    registerClass(L"QPointerRegion", RegionProc, GetSysColorBrush(COLOR_HIGHLIGHT));
    registerClass(L"QPointerSnapshot", SnapshotProc, GetSysColorBrush(COLOR_BTNFACE));
    registerClass(L"QPointerDemo", DemoProc, GetSysColorBrush(COLOR_WINDOW));
    registerClass(L"QPointerAlignment", AlignmentProc, nullptr);
    registerClass(L"QPointerCaptureOutput", CaptureOutputProc, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    RECT area{0, 0, 1064, 744};
    if (host) { if (bounds) area = *bounds; else GetClientRect(host, &area); }
    const DWORD style = host ? WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS :
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    HWND main = CreateWindowExW(host ? WS_EX_CONTROLPARENT : 0, L"QPointerMain", L"QInEX Pointer", style,
        host ? area.left : CW_USEDEFAULT, host ? area.top : CW_USEDEFAULT,
        area.right-area.left, area.bottom-area.top, host, nullptr, instance, nullptr);
    if (!main) { Shutdown(); return false; }
    app->overlay = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        L"QPointerOverlay", L"QInEX Pointer Overlay", WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, instance, nullptr);
    if (!app->overlay) { Shutdown(); return false; }
    if (host && !SetPropW(host, kEmbeddedWindowProperty, main)) { Shutdown(); return false; }
    ShowWindow(main, show); UpdateWindow(main);
    app->RegisterPauseHotkey(); app->PublishHotkeys(); app->ReloadCustomCursor(); app->UpdateStatus();
    if (app->settings.resumeOnLaunch && app->settings.setupComplete && !app->Start()) app->ShowControlWindow();
    return true;
}

bool ProcessMessage(MSG& message) {
    if (!app || !IsWindow(app->main)) return false;
    HWND main = app->main;
    // 新输入立即显示真实页面；过渡层不抢焦点，也不要求用户等动画结束。
    if (app->uiTransition.Active() && (message.hwnd == main || IsChild(main, message.hwnd)) &&
            (message.message == WM_LBUTTONDOWN || message.message == WM_RBUTTONDOWN ||
             message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN ||
             message.message == WM_MOUSEWHEEL))
        app->uiTransition.Cancel();
    // 普通窗体保留键盘 Tab 导航；截图的 Enter/Esc 由窗口显式处理。
    const auto belongsTo = [&](HWND window) { return window && (message.hwnd == window || IsChild(window, message.hwnd)); };
    if (belongsTo(app->snapshot) && message.message == WM_KEYDOWN && (message.wParam == VK_RETURN || message.wParam == VK_ESCAPE || (message.wParam >= VK_LEFT && message.wParam <= VK_DOWN))) { SendMessageW(app->snapshot, message.message, message.wParam, message.lParam); return true; }
    if (belongsTo(app->calibrationPrompt) && message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) { SendMessageW(app->calibrationPrompt, message.message, message.wParam, message.lParam); return true; }
    if (belongsTo(app->region) && message.message == WM_KEYDOWN && (message.wParam == VK_RETURN || message.wParam == VK_ESCAPE)) { SendMessageW(app->region, message.message, message.wParam, message.lParam); return true; }
    if (!belongsTo(main)) return false;
    return IsDialogMessageW(main, &message) != FALSE;
}

void ShowSettings() { DpiScope dpi; if (app) app->ShowControlWindow(); }
void HideSettings() { if (app && app->main) ShowWindow(app->main, SW_HIDE); }
void Resize(const RECT& bounds) {
    if (!app || !app->hostWindow || !app->main) return;
    SetWindowPos(app->main, nullptr, bounds.left, bounds.top, bounds.right-bounds.left,
        bounds.bottom-bounds.top, SWP_NOZORDER | SWP_NOACTIVATE);
    app->BuildControls();
}
void ShowMenu(POINT position) { if (app) app->ShowTrayMenu(position); }
bool CanExit() { DpiScope dpi; return !app || app->CanExit(); }

void Shutdown() {
    if (application) {
        if (IsWindow(app->main)) DestroyWindow(app->main);
        else if (!app->shuttingDown) app->Shutdown();
        application.reset();
        app = nullptr;
    }
    // 所有 GDI+ 图像及过渡资源必须先析构。
    if (graphicsToken) { Gdiplus::GdiplusShutdown(graphicsToken); graphicsToken = 0; }
}
} // namespace qpointer::desktop
