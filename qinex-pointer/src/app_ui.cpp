#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include "app.h"

namespace {
struct DisplayIdentity {
    std::wstring gdiName;
    std::wstring stableId;
    std::wstring friendlyName;
    double refreshHz = 0;
};

std::vector<DisplayIdentity> ActiveDisplayIdentities() {
    for (int attempt = 0; attempt < 3; ++attempt) {
        UINT32 pathCount = 0, modeCount = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS || !pathCount)
            return {};
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
        LONG result = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(),
            &modeCount, modes.empty() ? nullptr : modes.data(), nullptr);
        if (result == ERROR_INSUFFICIENT_BUFFER) continue;
        if (result != ERROR_SUCCESS) return {};
        paths.resize(pathCount);
        std::vector<DisplayIdentity> identities;
        for (const auto& path : paths) {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
            source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            source.header.size = sizeof(source);
            source.header.adapterId = path.sourceInfo.adapterId;
            source.header.id = path.sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS || !source.viewGdiDeviceName[0]) continue;

            DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
            target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
            target.header.size = sizeof(target);
            target.header.adapterId = path.targetInfo.adapterId;
            target.header.id = path.targetInfo.id;
            const bool targetReady = DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS;
            DisplayIdentity identity;
            identity.gdiName = source.viewGdiDeviceName;
            identity.stableId = targetReady && target.monitorDevicePath[0]
                ? target.monitorDevicePath : source.viewGdiDeviceName;
            if (targetReady && target.monitorFriendlyDeviceName[0])
                identity.friendlyName = target.monitorFriendlyDeviceName;
            if (path.targetInfo.refreshRate.Denominator)
                identity.refreshHz = static_cast<double>(path.targetInfo.refreshRate.Numerator) /
                    path.targetInfo.refreshRate.Denominator;
            identities.push_back(std::move(identity));
        }
        return identities;
    }
    return {};
}

std::wstring CaptureFpsText(const qp::CaptureNativeMode& mode, bool includeFraction) {
    if (!mode.fpsDenominator) return L"未知帧率";
    wchar_t decimal[48]{};
    swprintf_s(decimal, L"%.3f", mode.Fps());
    std::wstring text = decimal;
    while (text.size() > 1 && text.back() == L'0') text.pop_back();
    if (!text.empty() && text.back() == L'.') text.pop_back();
    text += L" fps";
    if (includeFraction && mode.fpsDenominator != 1)
        text += L"（" + std::to_wstring(mode.fpsNumerator) + L"/" + std::to_wstring(mode.fpsDenominator) + L"）";
    return text;
}

std::wstring CapturePixelFormat(const qp::CaptureNativeMode& mode) {
    if (!mode.subtypeFourcc.empty()) return mode.subtypeFourcc;
    if (!mode.subtypeName.empty()) return mode.subtypeName;
    return L"未知格式";
}

std::wstring CaptureModeLabel(const qp::CaptureNativeMode& mode, bool exact) {
    std::wstring label = std::to_wstring(mode.width) + L" × " + std::to_wstring(mode.height) + L" · " +
        CaptureFpsText(mode, exact) + L" · " + CapturePixelFormat(mode);
    if (exact) {
        if (mode.progressive || mode.interlaceMode == 2) label += L" · 逐行";
        else if (mode.interlaceMode >= 3 && mode.interlaceMode <= 6) label += L" · 隔行";
        else label += L" · 扫描方式未知";
    }
    return label;
}

std::wstring CaptureModeKey(const qp::CaptureNativeMode& mode) {
    return mode.stableKey.empty() ? qp::CaptureOutput::BuildModeStableKey(mode) : mode.stableKey;
}

const qp::CaptureNativeMode* FindCaptureMode(const std::vector<qp::CaptureNativeMode>& modes, const std::wstring& key) {
    auto found = std::find_if(modes.begin(), modes.end(), [&](const qp::CaptureNativeMode& mode) {
        return qp::CaptureOutput::ModeMatchesStableKey(mode, key);
    });
    return found == modes.end() ? nullptr : &*found;
}

std::wstring CaptureBackendLabel(qp::CaptureBackend backend) {
    switch (backend) {
    case qp::CaptureBackend::MediaFoundation: return L"Media Foundation";
    case qp::CaptureBackend::DirectShow: return L"DirectShow 兼容";
    default: return L"—";
    }
}

const qp::CaptureNativeMode* BestCaptureMode(const std::vector<qp::CaptureNativeMode>& modes, unsigned fpsBucket = 0) {
    if (!fpsBucket) {
        const int index = qp::CaptureOutput::SelectMode(modes, qp::CaptureModePolicy::Auto, L"");
        return index >= 0 && index < static_cast<int>(modes.size()) ? &modes[index] : nullptr;
    }
    std::vector<qp::CaptureNativeMode> matching;
    for (const auto& mode : modes)
        if (qp::CaptureOutput::RefreshRateBucket(mode.fpsNumerator, mode.fpsDenominator) == fpsBucket) matching.push_back(mode);
    const int index = qp::CaptureOutput::SelectMode(matching, qp::CaptureModePolicy::Auto, L"");
    if (index < 0 || index >= static_cast<int>(matching.size())) return nullptr;
    return FindCaptureMode(modes, CaptureModeKey(matching[index]));
}

std::wstring FusionColorText(COLORREF color) {
    wchar_t text[8]{};
    swprintf_s(text, L"#%02X%02X%02X", GetRValue(color), GetGValue(color), GetBValue(color));
    return text;
}

bool ParseFusionColor(std::wstring text, COLORREF& color) {
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), [](wchar_t c) { return !iswspace(c); }));
    while (!text.empty() && iswspace(text.back())) text.pop_back();
    if (!text.empty() && text.front() == L'#') text.erase(text.begin());
    if (text.size() != 6 || !std::all_of(text.begin(), text.end(), [](wchar_t c) { return iswxdigit(c) != 0; }))
        return false;
    wchar_t* end = nullptr;
    const unsigned long value = wcstoul(text.c_str(), &end, 16);
    if (!end || *end) return false;
    color = RGB((value >> 16) & 0xff, (value >> 8) & 0xff, value & 0xff);
    return true;
}
}  // namespace

// 保留为可独立烟测的纯决策函数：工作线程异常退出后，静态设备/规格提示不能
// 覆盖后端已经给出的具体失败原因。
std::wstring CaptureStatusTextWithTerminalError(const qp::CaptureStatus& status,
    bool active, std::wstring configuredText) {
    if (!active && status.state == qp::CaptureState::Error && !status.message.empty())
        return status.message;
    return configuredText;
}

void Application::CopyComputerAddress() {
    int selected = Selected(ID_LAN_ADDRESS); if (selected < 0 || selected >= static_cast<int>(localAddresses.size())) return;
    const auto& text = localAddresses[selected].address; const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL copy = GlobalAlloc(GMEM_MOVEABLE, bytes); if (!copy) return;
    void* data = GlobalLock(copy); if (!data) { GlobalFree(copy); return; }
    memcpy(data, text.c_str(), bytes); GlobalUnlock(copy);
    if (!OpenClipboard(main)) { GlobalFree(copy); Error(W(L"剪贴板暂时不可用，请再试一次。", L"剪贴板暂时不可用，再试一次喵。")); return; }
    EmptyClipboard(); const bool ok = SetClipboardData(CF_UNICODETEXT, copy) != nullptr; CloseClipboard(); if (!ok) GlobalFree(copy);
    notice = ok ? W(L"已复制电脑地址。请粘贴到 P4 指针设置的目标 IP。", L"电脑地址复制好了，粘贴到 P4 指针目标 IP 喵。") : W(L"未能复制，请手动填写上方地址。", L"复制失败，请手动填写上方地址喵。"); statusAt = 0;
}

void Application::NextStep() {
    if (page == 0) { SelectPage(running && state.hasPosition && Age(GetTickCount64(), state.lastPacketMs) <= static_cast<uint64_t>(settings.timeoutMs) ? 2 : 1); return; }
    if (page == 3 || page == 4) { ReturnToHost(); return; }
    Settings candidate; if (!ReadPage(candidate, page)) return;
    if (page == 1) {
        Apply(false); if (!running && !Start()) return;
        if (!state.hasPosition || sender.empty() || Age(GetTickCount64(), state.lastPacketMs) > static_cast<uint64_t>(settings.timeoutMs)) {
            notice = W(L"连接已启动。移动设备光标，收到真实位置后再次点击“连接并继续”。", L"连接启动啦，移动设备光标，收到真实位置后再继续喵。"); statusAt = 0; return;
        }
        SelectPage(2); return;
    }
    if (demoActive || !running || !state.hasPosition || Age(GetTickCount64(), state.lastPacketMs) > static_cast<uint64_t>(settings.timeoutMs)) {
        notice = W(L"完成检查前，请连接真实设备并移动光标。离线时仍可先应用、保存画面设置。", L"完成检查前先连接真实设备并移动光标喵，离线也能先应用、保存画面设置。"); statusAt = 0; return;
    }
    if (!Checked(ID_ALIGNMENT_CHECK)) {
        notice = W(L"请先把光标移到四角和中心，检查方向与行程，确认后勾选下方检查项。", L"先检查四角、中心、方向和行程，确认后勾选检查项喵。"); statusAt = 0; SetFocus(Item(ID_ALIGNMENT_CHECK)); return;
    }
    Apply(false); settings.setupComplete = true; MarkDirty(); CommitDraft(2); Apply(true); if (!dirty) ReturnToHost();
}

bool Application::HandleUiCommand(int id, int event) {
    if (uiBuilding) return true;
    if (event == BN_CLICKED && id >= ID_NAV_FIRST && id < ID_NAV_FIRST + kPageCount) {
        SelectPage(id - ID_NAV_FIRST);
        return true;
    }
    if (event == BN_CLICKED && (id == ID_ANIME || id == ID_UI_THEME)) {
        uiTransition.Capture(main, UiMotionAllowed());
        settings.anime = id == ID_ANIME ? Checked(ID_ANIME) : !settings.anime;
        SendMessageW(Item(ID_ANIME), BM_SETCHECK, settings.anime ? BST_CHECKED : BST_UNCHECKED, 0);
        Messages::anime = settings.anime;
        MarkDirty();
        BuildControls();
        return true;
    }
    if (id == ID_MODE && event == CBN_SELCHANGE && Selected(ID_MODE) == 3 && settings.captureDeviceId.empty() && !uiCaptureDefaultsApplied) {
        // 新用户进入采集输出时先走完整画面路径；需要裁边时再主动展开高级参数。
        SendMessageW(Item(ID_MAPPING_STYLE), CB_SETCURSEL, 0, 0);
        uiCaptureDefaultsApplied = true;
    }
    if (id == ID_SOURCE && event == CBN_SELCHANGE) {
        uiSourceAddresses[std::clamp(uiDisplayedSource, 0, 1)] = Text(Item(ID_IP)); uiDisplayedSource = std::max(0, Selected(ID_SOURCE));
        Set(ID_IP, uiSourceAddresses[uiDisplayedSource]); if (uiDisplayedSource == 1) Set(ID_PORT, L"6533");
        UpdateVisibility(); UpdatePresentation(); statusAt = 0; return true;
    }
    if (id == ID_CAPTURE_DEVICE && event == CBN_SELCHANGE) {
        RememberDraft();
        const int selected = Selected(ID_CAPTURE_DEVICE);
        const std::wstring deviceId = selected >= 0 && selected < static_cast<int>(uiCaptureDeviceIds.size())
            ? uiCaptureDeviceIds[selected] : L"";
        uiDraft.erase(ID_CAPTURE_SPEC);
        uiDraft.erase(ID_CAPTURE_EXACT_MODE);
        uiDraft.erase(ID_CAPTURE_ADVANCED);
        RefreshCaptureModesForDevice(deviceId);
        uiSkipRemember = true;
        BuildControls();
        return true;
    }
    if (id == ID_CAPTURE_ADVANCED && event == BN_CLICKED) {
        // 在普通帧率档和精确规格之间切换时保留当前时序意图。取消精确规格只
        // 放宽像素格式，不能意外回到 Auto；勾选时则定位到对应的精确条目。
        const bool exact = Checked(ID_CAPTURE_ADVANCED);
        if (exact) {
            const int preset = Selected(ID_CAPTURE_SPEC);
            std::wstring targetKey;
            if (preset >= 0 && preset < static_cast<int>(uiCapturePresetModeKeys.size())) {
                targetKey = uiCapturePresetModeKeys[preset];
                if (targetKey.empty()) {
                    if (const auto* automatic = BestCaptureMode(captureModes))
                        targetKey = CaptureModeKey(*automatic);
                }
            }
            int exactIndex = -1;
            if (!targetKey.empty()) {
                for (size_t index = 0; index < captureModes.size(); ++index) {
                    if (qp::CaptureOutput::ModeMatchesStableKey(captureModes[index], targetKey)) {
                        exactIndex = static_cast<int>(index);
                        break;
                    }
                }
                if (exactIndex < 0) {
                    for (size_t index = 0; index < captureModes.size(); ++index) {
                        if (qp::CaptureOutput::ModeMatchesTimingStableKey(captureModes[index], targetKey)) {
                            exactIndex = static_cast<int>(index);
                            break;
                        }
                    }
                }
            }
            if (exactIndex >= 0) SendMessageW(Item(ID_CAPTURE_EXACT_MODE), CB_SETCURSEL, exactIndex, 0);
        } else {
            const int exactIndex = Selected(ID_CAPTURE_EXACT_MODE);
            const qp::CaptureNativeMode* mode = exactIndex >= 0 && exactIndex < static_cast<int>(uiCaptureExactModeKeys.size())
                ? FindCaptureMode(captureModes, uiCaptureExactModeKeys[exactIndex]) : nullptr;
            if (mode) {
                const int fps = static_cast<int>(qp::CaptureOutput::RefreshRateBucket(
                    mode->fpsNumerator, mode->fpsDenominator));
                int presetIndex = -1;
                for (size_t index = 0; index < uiCapturePresetFps.size(); ++index) {
                    if (uiCapturePresetFps[index] == fps) {
                        presetIndex = static_cast<int>(index);
                        break;
                    }
                }
                if (fps > 0 && fps <= 1000) {
                    if (presetIndex < 0) {
                        presetIndex = static_cast<int>(uiCapturePresetFps.size());
                        uiCapturePresetFps.push_back(fps);
                        uiCapturePresetModeKeys.push_back(CaptureModeKey(*mode));
                        uiCapturePresetAvailable.push_back(true);
                    } else {
                        uiCapturePresetModeKeys[static_cast<size_t>(presetIndex)] = CaptureModeKey(*mode);
                        uiCapturePresetAvailable[static_cast<size_t>(presetIndex)] = true;
                    }
                    // 普通档继续使用当前精确项的宽高、刷新率和扫描方式，仅放宽 subtype。
                    const std::wstring label = std::to_wstring(fps) +
                        W(L" 帧率优先 · ", L" 帧率优先喵 · ") + CaptureModeLabel(*mode, false);
                    HWND combo = Item(ID_CAPTURE_SPEC);
                    if (presetIndex < static_cast<int>(SendMessageW(combo, CB_GETCOUNT, 0, 0)))
                        SendMessageW(combo, CB_DELETESTRING, presetIndex, 0);
                    SendMessageW(combo, CB_INSERTSTRING, presetIndex, reinterpret_cast<LPARAM>(label.c_str()));
                    // push_back 可能移动字符串对象，统一刷新全部 item data 指针。
                    for (size_t index = 0; index < uiCapturePresetModeKeys.size(); ++index)
                        SendMessageW(combo, CB_SETITEMDATA, index,
                            reinterpret_cast<LPARAM>(uiCapturePresetModeKeys[index].c_str()));
                    SendMessageW(combo, CB_SETCURSEL, presetIndex, 0);
                }
            }
        }
        SendMessageW(Item(ID_ALIGNMENT_CHECK), BM_SETCHECK, BST_UNCHECKED, 0);
        UpdateVisibility(); UpdatePresentation(); statusAt = 0; return true;
    }
    if ((id == ID_SHAPE && event == CBN_SELCHANGE) || (id == ID_SIZE && event == EN_CHANGE)) {
        ApplyAppearance(); const auto values = CaptureUiValues(); const auto current = values.find(id); bool valid = id == ID_SHAPE;
        if (id == ID_SIZE) { try { size_t used = 0; auto text = Text(Item(id)); const int size = std::stoi(text, &used); valid = used == text.size() && size == settings.size; } catch (...) {} }
        if (valid && current != values.end()) uiBaseline[id] = current->second;
        UpdatePresentation(); return true;
    }
    if (id == ID_COLOR && event == BN_CLICKED) {
        static COLORREF custom[16]{}; CHOOSECOLORW color{sizeof(color)}; color.hwndOwner = main; color.rgbResult = settings.color; color.lpCustColors = custom; color.Flags = CC_FULLOPEN | CC_RGBINIT;
        if (ChooseColorW(&color) && color.rgbResult != settings.color) {
            if (IsFusionDisplayMode(settings.mode) && color.rgbResult == settings.fusionBackgroundColor) {
                Error(W(L"光标颜色不能与融合背景色完全相同。", L"光标颜色不能和融合背景色一样喵。"));
                return true;
            }
            settings.color = color.rgbResult; cacheShape = -1; MarkDirty(); Render(); UpdatePresentation();
        }
        return true;
    }
    if (id == ID_CUSTOM_CURSOR && event == BN_CLICKED) {
        EditCustomCursor(); UpdatePresentation(); return true;
    }
    if (id == ID_HOTKEYS && event == BN_CLICKED) {
        EditHotkeys(); UpdatePresentation(); return true;
    }
    if ((id == ID_FUSION_GREEN || id == ID_FUSION_BLUE || id == ID_FUSION_BLACK) && event == BN_CLICKED) {
        const COLORREF selected = id == ID_FUSION_GREEN ? RGB(0, 255, 0)
            : id == ID_FUSION_BLUE ? RGB(0, 0, 255) : RGB(0, 0, 0);
        Set(ID_FUSION_COLOR, FusionColorText(selected));
        return true;
    }
    if (id == ID_FUSION_PICK && event == BN_CLICKED) {
        COLORREF current = settings.fusionBackgroundColor;
        ParseFusionColor(Text(Item(ID_FUSION_COLOR)), current);
        static COLORREF custom[16]{};
        CHOOSECOLORW color{sizeof(color)};
        color.hwndOwner = main;
        color.rgbResult = current;
        color.lpCustColors = custom;
        color.Flags = CC_FULLOPEN | CC_RGBINIT;
        if (ChooseColorW(&color)) Set(ID_FUSION_COLOR, FusionColorText(color.rgbResult));
        return true;
    }
    if (id == ID_FUSION_COLOR && event == EN_CHANGE) {
        COLORREF color{};
        if (ParseFusionColor(Text(Item(ID_FUSION_COLOR)), color) && color != settings.color) {
            if (settings.fusionBackgroundColor != color) {
                settings.fusionBackgroundColor = color;
                MarkDirty();
                SubmitCapturePresentation();
                if (captureWindow && !capturePresenter.Active()) InvalidateRect(captureWindow, nullptr, FALSE);
            }
            const auto values = CaptureUiValues();
            const auto current = values.find(ID_FUSION_COLOR);
            if (current != values.end()) uiBaseline[ID_FUSION_COLOR] = current->second;
        }
        statusAt = 0;
        UpdatePresentation();
        return true;
    }
    const bool geometryChange = ((id == ID_MODE || id == ID_MONITOR || id == ID_WINDOW || id == ID_CAPTURE_DEVICE ||
        id == ID_CAPTURE_MONITOR || id == ID_FUSION_MONITOR || id == ID_CAPTURE_SPEC ||
        id == ID_CAPTURE_EXACT_MODE || id == ID_MAPPING_STYLE || id == ID_ROTATE) && event == CBN_SELCHANGE)
        || (id >= ID_X && id <= ID_CROP_H && event == EN_CHANGE)
        || ((id == ID_MIRROR_X || id == ID_MIRROR_Y || id == ID_STRETCH) && event == BN_CLICKED);
    if (geometryChange) SendMessageW(Item(ID_ALIGNMENT_CHECK), BM_SETCHECK, BST_UNCHECKED, 0);
    if (((id == ID_MODE || id == ID_MAPPING_STYLE) && event == CBN_SELCHANGE) || (id == ID_NETWORK_ADVANCED && event == BN_CLICKED)) UpdateVisibility();
    if (event == EN_CHANGE || event == CBN_SELCHANGE || event == BN_CLICKED) { statusAt = 0; UpdatePresentation(); }
    if (event != BN_CLICKED) return false;
    switch (id) {
    case ID_NEXT: NextStep(); return true;
    case ID_BACK: SelectPage(page == 2 && !settings.setupComplete ? 1 : 0); return true;
    case ID_HOME_PRIMARY:
        if (demoActive) EndDemo(); else if (!settings.setupComplete) NextStep(); else if (!running) { if (Start()) paused = false; } else TogglePause();
        UpdatePresentation(); return true;
    case ID_HOME_CHANGE_DEVICE: SelectPage(1); return true;
    case ID_HOME_CHANGE_PREVIEW: SelectPage(2); return true;
    case ID_HOME_APPEARANCE: SelectPage(4); return true;
    case ID_HOME_CALIBRATE: SelectPage(3); return true;
    case ID_HOME_REGION: SelectPage(2); if (!IsOwnedFullscreenDisplayMode(settings.mode)) BeginRegion(); return true;
    case ID_CHECK_ALIGNMENT: CheckAlignment(); return true;
    case ID_COPY_ADDRESS: CopyComputerAddress(); return true;
    case ID_BIND_SOURCE: BindSource(); return true;
    case ID_START: {
        Settings candidate;
        if (ReadPage(candidate, 1)) {
            const bool reconnect = running && SameSource(settings, candidate);
            Apply(false); if (reconnect) Stop(); if (!running) Start();
        }
        return true;
    }
    case ID_SIMULATOR: if (demoActive) EndDemo(); else Simulate(); return true;
    case ID_REFRESH: RememberDraft(); Inventory(); uiSkipRemember = true; BuildControls(); return true;
    case ID_CAPTURE_REFRESH:
        RememberDraft(); captureModesDeviceId.clear(); Inventory(); uiSkipRemember = true; BuildControls(); return true;
    case ID_CAPTURE_TOGGLE:
        if (captureOutput.Active()) ToggleCaptureOutput();
        else {
            Settings candidate;
            if (ReadPage(candidate, 2)) { Apply(false); if (!HasDraft(2)) ToggleCaptureOutput(); }
        }
        UpdatePresentation(); return true;
    case ID_CAPTURE_STOP:
        StopCaptureOutput(); UpdatePresentation(); return true;
    case ID_CAPTURE_RANGE:
        BeginCaptureRangeAdjustment(); return true;
    case ID_FUSION_TOGGLE:
        ToggleCaptureOutput(); UpdatePresentation(); return true;
    case ID_FUSION_RANGE: {
        Settings candidate;
        if (ReadPage(candidate, 2)) {
            AdoptSettings(candidate); CommitDraft(2); BeginCaptureRangeAdjustment();
        }
        return true;
    }
    case ID_SAVE_SCENE: SaveScene(); return true;
    case ID_LOAD_SCENE: LoadScene(); return true;
    case ID_GUIDE: ShowHelp(); return true;
    case ID_DIAGNOSTICS:
    case ID_CAPTURE_DIAGNOSTICS: {
        auto text = deviceStatus + L"\r\n" + displayStatus + W(L"\r\n\r\n已接受报文：", L"\r\n\r\n已接受报文喵：") + std::to_wstring(accepted)
            + W(L"\r\n无效报文：", L"\r\n无效报文喵：") + std::to_wstring(rejected)
            + W(L"\r\n接收队列丢弃：", L"\r\n接收队列丢弃喵：") + std::to_wstring(receiver.Dropped())
            + W(L"\r\n来源：", L"\r\n来源喵：") + sender + L"\r\nnx=" + Num(state.position.x) + L"  ny=" + Num(state.position.y)
            + L"  show=" + std::to_wstring(state.show) + L"  down=" + std::to_wstring(state.down);
        const auto capture = captureOutput.Status();
        if (settings.mode == 3 || capture.state != qp::CaptureState::Stopped ||
                !capture.mediaFoundationError.empty() || !capture.directShowError.empty()) {
            text += W(L"\r\n\r\n采集方式：", L"\r\n\r\n采集方式喵：") +
                CaptureBackendLabel(capture.backend) + W(L"\r\n采集状态：", L"\r\n采集状态喵：") +
                capture.message + W(L"\r\n后端切换：", L"\r\n后端切换喵：") +
                std::to_wstring(capture.backendSwitchCount);
            const auto& diagnostic = capture.diagnostics;
            if (diagnostic.analyzedFrames) {
                text += W(L"\r\n\r\n画面分析（最近约 1 秒）：", L"\r\n\r\n画面分析喵（最近约 1 秒）：");
                if (diagnostic.ready && diagnostic.contentAvailable) {
                    text += W(L"\r\n接收 / 画面变化：", L"\r\n接收 / 画面变化喵：") +
                        Num(capture.measuredFps) + L" / ≈" + Num(diagnostic.changedFps) +
                        L" FPS" + W(L"\r\n重复采样：", L"\r\n重复采样喵：") +
                        Num(diagnostic.identicalPercent) + L"%";
                    text += W(L"\r\n帧间隔 P95 / 峰值：", L"\r\n帧间隔 P95 / 峰值喵：") +
                        Num(diagnostic.intervalP95Ms) + L" / " +
                        Num(diagnostic.intervalMaxMs) + L" ms";
                } else text += W(L"\r\n等待有效采样窗口。", L"\r\n等有效采样窗口喵。");
                text += W(L"\r\n当前 / 最长连续相同：", L"\r\n当前 / 最长连续相同喵：") +
                    Num(diagnostic.unchangedMs) + L" / " + Num(diagnostic.longestUnchangedMs) + L" ms";
                text += W(L"\r\n已分析 / 入口跳过：", L"\r\n已分析 / 入口跳过喵：") +
                    std::to_wstring(diagnostic.analyzedFrames) + L" / " +
                    std::to_wstring(diagnostic.mailboxSkippedFrames);
                text += diagnostic.timestampAvailable
                    ? W(L"\r\n时间戳重复 / 倒退：", L"\r\n时间戳重复 / 倒退喵：") +
                        std::to_wstring(diagnostic.timestampRepeats) + L" / " +
                        std::to_wstring(diagnostic.timestampRegressions)
                    : W(L"\r\n样本时间戳不可用。", L"\r\n样本时间戳不可用喵。");
                const auto presenter = capturePresenter.Status();
                if (presenter.active) {
                    text += W(L"\r\n累计采集 / 新视频呈现 / 重绘：",
                        L"\r\n累计采集 / 新视频呈现 / 重绘喵：") +
                        std::to_wstring(capture.frameSerial) + L" / " +
                        std::to_wstring(presenter.videoFrames) + L" / " +
                        std::to_wstring(presenter.presents);
                }
                text += W(L"\r\n\r\n变化与重复来自叠加光标前的 RGB 网格采样。请持续移动游戏画面观察；静止画面会正常重复，小范围变化可能漏检，噪点也可能增加变化率。入口跳过是程序邮箱覆盖，不能当作驱动丢帧。重连或格式变化后重新统计。",
                    L"\r\n\r\n变化与重复来自叠加光标前的 RGB 网格采样喵。持续移动游戏画面再看；静止会正常重复，小变化可能漏检，噪点也可能增加变化率。入口跳过是程序邮箱覆盖，不是驱动丢帧喵。重连或格式变化后重新统计。");
            }
            if (!capture.mediaFoundationError.empty())
                text += L"\r\nMedia Foundation: " + capture.mediaFoundationError;
            if (!capture.directShowError.empty())
                text += L"\r\nDirectShow: " + capture.directShowError;
        }
        MessageBoxW(main, text.c_str(), W(L"诊断详情 · QInEX Pointer", L"诊断详情喵 · QInEX Pointer"), MB_OK | MB_ICONINFORMATION); return true;
    }
    }
    return false;
}

void Application::ShowHelp() {
    if (page == 5) {
        MessageBoxW(main, W(
            L"先连接并选定手机，再检查手机。USB 调试需要手机授权；手机接入 P4 时可使用 Android 11+ 无线调试，配对端口与连接端口不同。\n\n"
            L"开启会先备份原值。Android 15+ 且 ROM 允许时使用用户开关；其它情况尝试强制属性，零售 ROM 可能拒绝。用户开关写入与强制失败分别反馈。强制属性变更后需重启，再用 P4 确认显示。\n\n"
            L"默认图标不可见时，可单独开启触摸反馈，通常立即生效。它也会显示手指触摸点，关闭开发者总开关可能被重置。工具显示读回状态，并独立保存首次修改前的反馈设置；关闭与恢复原反馈分别操作。此功能仍需已授权 ADB，厂商可能拒绝写入。\n\n"
            L"替换外观前分析手机资源，选择实际悬停图标的路径。默认空图标不能通过图片覆盖显示。ROOT 安装使用 Magisk，安装后重启并检查；恢复外观与恢复开启设置分别操作。\n\n"
            L"Android 草稿与备份保存在当前用户目录，图片热点编辑不会改变 PC 光标。",
            L"先连接手机并授权，再检查喵。接着 P4 时可用无线调试，配对和连接端口不同。\n\n"
            L"开启先备份喵。Android 15+ 且 ROM 允许就用用户开关，其它情况尝试强制属性，零售 ROM 可能拒绝。用户开关和强制结果分别显示喵。强制值变更后重启，再用 P4 确认图标。\n\n"
            L"默认图标看不见时可单独开启触摸反馈喵，通常立即生效；也显示手指触点，关开发者总开关可能重置。读回状态和最初反馈备份单独保存，可关闭或恢复原反馈喵。仍需已授权 ADB，厂商可能拒绝写入。\n\n"
            L"换图前分析资源并选实际悬停路径，空图标不能靠换图片显示喵。Magisk 安装后重启检查；图片和开启设置分别恢复。\n\n"
            L"手机草稿和备份独立保存，不会改变 PC 光标喵。"),
            W(L"Android 系统光标帮助", L"Android 系统光标帮助喵"), MB_OK); return;
    }
    const wchar_t* introductions[] = {
        W(L"把设备指针画到电脑的采集预览上", L"把设备指针画到采集预览上喵"), W(L"先确认数据从设备到达这台电脑", L"先让设备数据到达电脑喵"),
        W(L"让指针的四角与手机画面的四角一致", L"让指针与手机画面的四角一致喵"), W(L"基础对齐后仍有偏差，再进行精细校准", L"基础对齐后还有偏差，再精细校准喵"), W(L"外观立即预览，其余设置应用后生效", L"外观立即预览，其余设置应用后生效喵")
    };
    const wchar_t* help[] = {
        W(L"首次使用：设备 → 画面 → 检查四角与中心 → 保存并使用。\n\n配置完成后，用首页的暂停 / 恢复控制指针显示。暂停时仍保持连接。关闭设置窗口后仍在后台运行：独立版从托盘恢复，合并版从 Link 顶部“光标设置”恢复。退出请使用 Link 或独立版的托盘菜单。\n\n“演示指针”运行独立测试，结束后恢复真实设备，不会覆盖设备和校准。", L"首次按设备、画面、检查、保存的顺序完成喵。\n\n首页暂停 / 恢复只控制显示，连接仍然保持。关闭设置后仍在后台运行喵；独立版点托盘、合并版点 Link 顶部“光标设置”回来，退出请用托盘菜单。\n\n演示是独立测试，结束后恢复真实设备和校准喵。"),
        W(L"P4：打开板载页“网络 → VP 光标（PC / App）”，启用 VP 光标。在“高级”中填入上方电脑地址，关闭“兼容旧版 App”和“自动发现接收端”。多个地址时选择连接 P4 所在网络的网卡。\n\n“P4 地址”填板子地址，不知道时先留空。点击开始连接并移动光标，收到数据后可绑定这台设备。\n\nS3：在本机 QInEX 开启 JSON v2 指针，目标 127.0.0.1，默认端口 6533。\n\n没有数据时，先检查两端设置和电脑网络。只有实际允许的接收来源才会显示。", L"P4 在“网络 → VP 光标（PC / App）”启用 VP 光标喵。“高级”里填电脑地址，关闭“兼容旧版 App”和“自动发现接收端”。多个网卡时选与 P4 相连的网络。\n\nP4 地址填板子地址，不知道就先留空。连接后移动光标，收到数据再绑定。\n\nS3 在 QInEX 开启 JSON v2，目标 127.0.0.1、默认端口 6533 喵。\n\n没数据时先核对设置和网络。"),
        W(L"采集卡全屏输出：选择采集设备和输出显示器，点击“启动全屏”。默认自动选择低延迟规格；精确规格只列出设备真实报告的分辨率、刷新率和像素格式。\n\n纯色背景全屏：给视频融合器提供纯色与光标，不打开采集卡。选择输出屏和背景色后启动，再由融合器抠除该颜色。\n\n两种全屏底部都会显示运行指标和当前快捷键。显隐 HUD、范围校准和退出键可在“外观与快捷键”中自定义；校准时用 Enter 应用、Esc 取消。\n\n外部预览窗口可框住完整手机画面；有黑边、裁剪或旋转时再使用高级设置。", L"采集卡全屏时选采集设备和显示器再启动喵；精确规格只列设备真实支持的模式。\n\n纯色背景全屏给融合器输出纯色和光标，不打开采集卡喵；选屏幕和颜色后，由融合器抠除背景。\n\n两种全屏底部都有运行指标和当前快捷键喵。HUD、范围校准和退出键可在外观设置中自定义；校准时 Enter 应用、Esc 取消。"),
        W(L"采集卡和融合器模式优先使用快捷范围校准：全屏时触发“调整光标范围”，直接拖动边框匹配光标应到达的范围，Enter 应用。移动光标检查四角和中心，满意后保存。\n\n纯色融合输出没有手机画面，请对照融合器最终画面调整。该快捷键可在“外观与快捷键”中自定义。\n\n采集画面仍有局部偏差时才做九点精调：打开手机系统触点显示，逐点取样；冻结截图后点击真实触点中心并确认。", L"采集卡和融合器都先用快捷范围校准喵：拖边框匹配范围，Enter 应用，检查四角和中心后保存。\n\n纯色融合没有手机画面，请对照融合器最终画面调整喵。范围校准键可在外观设置中自定义。采集画面还有局部偏差时，再用九点精调。"),
        W(L"形状、大小和颜色立即更新指针。自定义图片可以选择光标热点；快捷键可分别设置暂停、HUD、范围校准和退出全屏。\n\n自定义图片和快捷键在各自窗口保存后立即生效。延迟、断流隐藏和启动连接需要点击“应用本页”。\n\n采集画面有明显延迟时，可增加指针显示延迟。先用低值体验，再根据实际预览调整。\n\n“保存场景”可为不同设备或预览布局保存独立文件。“打开场景”恢复对应配置。", L"形状、大小和颜色立即更新喵。自定义图片可选热点，快捷键可分别设置暂停、HUD、范围校准和退出全屏。\n\n图片和快捷键在各自窗口保存后立即生效。延迟、断流隐藏和启动连接要应用本页喵。\n\n采集画面明显滞后时，可逐步增加指针延迟。\n\n用场景文件保存不同设备或预览布局喵。")
    };
    TASKDIALOGCONFIG dialog{sizeof(dialog)}; dialog.hwndParent = main; dialog.hInstance = instance;
    dialog.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT; dialog.dwCommonButtons = TDCBF_CLOSE_BUTTON;
    dialog.pszWindowTitle = W(L"使用帮助 · QInEX Pointer", L"使用帮助喵 · QInEX Pointer");
    dialog.pszMainInstruction = introductions[page]; dialog.pszContent = help[page]; dialog.cxWidth = 490;
    TaskDialogIndirect(&dialog, nullptr, nullptr, nullptr);
}

void Application::SelectPage(int p) {
    const int nextPage = std::clamp(p, 0, kPageCount - 1);
    const bool pageChanged = page != nextPage;
    // 工具页高度不同，直接完成布局，避免对不同尺寸的画面做缩放过渡。
    if (pageChanged) uiTransition.Capture(main, page != 5 && nextPage != 5 && UiMotionAllowed());
    const bool batch = main && IsWindowVisible(main);
    if (batch) SendMessageW(main, WM_SETREDRAW, FALSE, 0);
    page = nextPage;
    if (!hostWindow) {
        RECT outer{0, 0, Px(1064), Px(page == 5 ? 744 : 704)};
        AdjustWindowRectEx(&outer, static_cast<DWORD>(GetWindowLongPtrW(main, GWL_STYLE)), FALSE, 0);
        SetWindowPos(main, nullptr, 0, 0, outer.right - outer.left, outer.bottom - outer.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    for (auto& c : controls) ShowWindow(c.hwnd, c.page == -1 || c.page == page ? SW_SHOW : SW_HIDE);
    const wchar_t* titles[] = {W(L"指针状态", L"指针状态喵"), W(L"连接设备", L"连接设备喵"), W(L"画面与全屏", L"画面与全屏喵"), W(L"校准光标", L"校准光标喵"), W(L"外观与快捷键", L"外观与快捷键喵"), W(L"Android 系统光标", L"Android 系统光标喵")};
    Set(ID_PAGE_TITLE, titles[page]);
    const wchar_t* subtitles[] = {
        W(L"设备、画面与指针，一眼掌握。", L"设备、画面和小指针，都在这里喵。"),
        W(L"选择指针来源，建立设备连接。", L"连接你的设备，让小指针上线吧。"),
        W(L"选择输出方式，让指针与手机画面对齐。", L"选好画面，让小指针跟着你走。"),
        W(L"先拖拽调整范围，再按需进行九点精调。", L"先拖一拖边框，再慢慢调整小细节。"),
        W(L"让指针外观和操作习惯更适合你。", L"换上喜欢的样子，留住顺手的操作。"),
        W(L"通过 USB 或无线连接管理手机系统光标。", L"连接手机，为系统光标换个样子吧。")
    };
    Set(ID_UI_SUBTITLE, subtitles[page]);
    SetWindowPos(Item(ID_UI_THEME), nullptr, Px(20), Px((page == 5 ? 744 : 704) - 112),
        Px(144), Px(36), SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(Item(ID_BACK), page == 2 ? SW_SHOW : SW_HIDE);
    Set(ID_BACK, page == 2 && !settings.setupComplete ? W(L"设备", L"设备喵") : W(L"回首页", L"回首页喵"));
    ShowWindow(Item(ID_APPLY), page == 1 || page == 2 || page == 4 ? SW_SHOW : SW_HIDE);
    ShowWindow(Item(ID_NEXT), page > 0 && page < 5 ? SW_SHOW : SW_HIDE);
    for (int id : std::initializer_list<int>{ID_SAVE, ID_SAVE_STATE, ID_STATUS}) ShowWindow(Item(id), page == 5 ? SW_HIDE : SW_SHOW);
    Set(ID_NEXT, page == 1 ? W(L"连接并继续", L"连接并继续喵") : page == 2 ? W(L"保存并使用", L"保存并使用喵") : hostWindow ? W(L"返回上位机", L"返回上位机喵") : W(L"回首页", L"回首页喵"));
    UpdateVisibility(); UpdatePresentation();
    InvalidateRect(main, nullptr, FALSE);
    for (int i = 0; i < kPageCount; ++i) InvalidateRect(Item(ID_NAV_FIRST + i), nullptr, FALSE);
#if QINEX_WITH_ANDROID_TOOLS
    if (page == 5) UpdateAndroidTools();
#endif
    if (batch) {
        SendMessageW(main, WM_SETREDRAW, TRUE, 0);
        if (pageChanged) uiTransition.Play(150);
        RedrawWindow(main, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }
}

void Application::UpdateVisibility() {
    const bool p4 = Selected(ID_SOURCE) == 1, extra = Checked(ID_NETWORK_ADVANCED);
    const int mode = Selected(ID_MODE); const bool geometryExtra = Selected(ID_MAPPING_STYLE) == 1;
    const bool captureAdvanced = Checked(ID_CAPTURE_ADVANCED);
    for (const auto& c : uiConditional) {
        bool visible = false;
        switch (c.kind) {
        case UiP4: visible = page == 1 && p4; break;
        case UiNetworkExtra: visible = page == 1 && extra; break;
        case UiSourceAddress: visible = page == 1 && (p4 || extra); break;
        case UiMonitor: visible = page == 2 && mode == 0; break;
        case UiWindow: visible = page == 2 && mode == 2; break;
        case UiCustom: visible = page == 2 && mode == 1; break;
        case UiGeometryExtra: visible = page == 2 && geometryExtra && !IsFusionDisplayMode(mode); break;
        case UiGeometrySimple: visible = page == 2 && !geometryExtra && !IsFusionDisplayMode(mode); break;
        case UiGeometryFrameInput: visible = page == 2 && geometryExtra && !IsOwnedFullscreenDisplayMode(mode); break;
        case UiCaptureGeometryFrame: visible = page == 2 && geometryExtra && IsCaptureDisplayMode(mode); break;
        case UiCapture: visible = page == 2 && IsCaptureDisplayMode(mode); break;
        case UiCaptureSpecBasic: visible = page == 2 && IsCaptureDisplayMode(mode) && !captureAdvanced; break;
        case UiCaptureSpecAdvanced: visible = page == 2 && IsCaptureDisplayMode(mode) && captureAdvanced; break;
        case UiCaptureGeometrySimple: visible = page == 2 && IsCaptureDisplayMode(mode) && !geometryExtra; break;
        case UiFusion: visible = page == 2 && IsFusionDisplayMode(mode); break;
        case UiMapping: visible = page == 2 && !IsFusionDisplayMode(mode); break;
        case UiDesktopTarget: visible = page == 2 && !IsOwnedFullscreenDisplayMode(mode); break;
        }
        ShowWindow(c.hwnd, visible ? SW_SHOW : SW_HIDE);
    }
    Set(ID_SOURCE_LABEL, p4 ? W(L"P4 地址（可先留空）", L"P4 地址（可先留空）喵") : W(L"发送端地址", L"发送端地址喵"));
    Set(ID_DEVICE_HELP, p4
        ? W(L"P4“网络 → VP 光标”启用后，在“高级”填电脑 IP，关闭“兼容旧版 App”和“自动发现接收端”。", L"P4“网络 → VP 光标”启用后，在“高级”填电脑 IP，关闭“兼容旧版 App”和“自动发现接收端”喵。")
        : W(L"在 QInEX 开启 JSON v2 指针，目标填 127.0.0.1:6533。", L"在 QInEX 开 JSON v2 指针，目标填 127.0.0.1:6533 喵。"));
    Enable(ID_PORT, !p4); Enable(ID_COPY_ADDRESS, !localAddresses.empty());
    Set(ID_REGION, geometryExtra ? W(L"框住预览视频容器", L"框住预览视频容器喵") : W(L"框住完整手机画面", L"框住完整手机画面喵"));
    Set(ID_ALIGNMENT_CHECK, IsFusionDisplayMode(mode)
        ? W(L"我已在融合器最终画面检查四角、中心和移动方向", L"我已在融合器最终画面检查四角、中心和方向喵")
        : W(L"我已在手机画面检查四角、中心和移动方向", L"我已检查四角、中心和移动方向喵"));
}

void Application::UpdatePresentation() {
    if (!main || !Has(ID_HOME_STATE)) return;
    const std::wstring sourceName = settings.source == 1 ? L"P4" : L"S3 + QInEX";
    Set(ID_HOME_STATE, demoActive ? W(L"正在演示指针", L"正在演示指针喵") : !running ? W(L"连接设备，让指针出现在采集画面上", L"连接设备，让指针出现在采集画面上喵") : paused ? W(L"指针显示已暂停", L"指针显示暂停啦喵") : !state.lastPacketMs ? W(L"等待设备数据", L"等待设备数据喵") : Age(GetTickCount64(), state.lastPacketMs) > static_cast<uint64_t>(settings.timeoutMs) ? W(L"设备数据已中断", L"设备数据中断啦喵") : displayStatus);
    std::wstring homeDetail;
    if (demoActive) homeDetail = W(L"演示结束后恢复真实设备。设备设置和校准会保留。", L"演示结束后恢复真实设备，设置和校准都会保留喵。");
    else if (IsCaptureDisplayMode(settings.mode)) {
        const auto capture = captureOutput.Status();
        const std::wstring output = captureWindow ? W(L"全屏输出中", L"全屏输出中喵")
            : captureOutput.Active() ? W(L"全屏未显示", L"全屏没显示喵") : W(L"尚未启动", L"还没启动喵");
        homeDetail = W(L"指针数据：", L"指针数据喵：") + deviceStatus + L"\r\n" +
            W(L"采集画面：", L"采集画面喵：") + capture.message + L"  ·  " + output;
    } else if (IsFusionDisplayMode(settings.mode)) {
        homeDetail = W(L"指针数据：", L"指针数据喵：") + deviceStatus + L"\r\n" +
            W(L"融合输出：", L"融合输出喵：") +
            (captureWindow ? W(L"纯色背景与光标正在全屏输出", L"纯色背景和光标正在全屏输出喵")
                : W(L"尚未启动；不占用采集卡", L"还没启动；不会占用采集卡喵"));
    } else homeDetail = deviceStatus + (displayStatus.empty() ? L"" : L"\r\n" + displayStatus);
    Set(ID_HOME_DETAIL, homeDetail);
    Set(ID_HOME_DEVICE, W(L"设备：", L"设备喵：") + sourceName + (settings.sourceIp.empty() ? W(L"  ·  等待绑定", L"  ·  等待绑定喵") : L"  ·  " + settings.sourceIp));
    std::wstring target;
    if (IsCaptureDisplayMode(settings.mode)) {
        target = settings.captureDeviceName.empty() ? W(L"采集卡全屏输出 · 尚未选择采集设备", L"采集卡全屏输出 · 还没选采集设备喵")
            : W(L"采集卡全屏输出 · ", L"采集卡全屏输出喵 · ") + settings.captureDeviceName;
    } else if (IsFusionDisplayMode(settings.mode)) {
        target = W(L"纯色融合输出 · ", L"纯色融合输出喵 · ") + FusionColorText(settings.fusionBackgroundColor);
    } else if (settings.mode == 2) target = settings.windowTitle;
    else if (settings.mode == 0) target = W(L"显示器 ", L"显示器喵 ") + std::to_wstring(settings.monitor + 1);
    else target = W(L"固定桌面区域", L"固定桌面区域喵");
    if (target.empty()) target = W(L"尚未选择", L"还没选择喵");
    Set(ID_HOME_PREVIEW, W(L"预览：", L"预览喵：") + target);
    Set(ID_HOME_REGION, IsOwnedFullscreenDisplayMode(settings.mode)
        ? W(L"画面输出设置", L"画面输出设置喵") : W(L"重新框选画面", L"重新框选喵"));
    Set(ID_HOME_PRIMARY, demoActive ? W(L"结束演示", L"结束演示喵") : !settings.setupComplete ? W(L"继续完成设置", L"继续设置喵") : !running ? W(L"连接并显示", L"连接并显示喵") : paused ? W(L"恢复显示", L"恢复显示喵") : W(L"暂停显示", L"暂停显示喵"));
    const bool connectionDraft = HasDraft(1);
    if (connectionDraft) {
        Set(ID_DEVICE_STATE, std::wstring(W(L"连接修改尚未应用，请点击“应用并连接”。", L"连接修改还没应用，请点击“应用并连接”喵。"))
            + (running ? W(L"\r\n当前接收：", L"\r\n当前接收喵：") + sourceName + L"  ·  " + deviceStatus + (sender.empty() ? L"" : L"  ·  " + sender)
                : W(L"\r\n当前未连接设备。", L"\r\n当前还没连接设备喵。")));
    } else {
        Set(ID_DEVICE_STATE, sourceName + L"  ·  " + (deviceStatus.empty() ? W(L"尚未连接。设置好发送端后，点击“开始连接”。", L"还没连接，设置好发送端后开始连接喵。") : deviceStatus)
            + (sender.empty() ? L"" : W(L"\r\n最近收到：", L"\r\n最近收到喵：") + sender));
    }
    Set(ID_SIMULATOR, demoActive ? W(L"结束演示，返回真实设备", L"结束演示，回到真实设备喵") : W(L"演示指针（15 秒）", L"演示指针（15 秒）喵"));
    Enable(ID_BIND_SOURCE, !demoActive && !sender.empty() && Selected(ID_SOURCE) == settings.source);
    Enable(ID_STOP, running);
    Set(ID_START, connectionDraft ? W(L"应用并连接", L"应用并连接喵") : running ? W(L"重新连接", L"重新连接喵") : W(L"开始连接", L"开始连接喵"));
    if (Has(ID_CAPTURE_STATE)) {
        const auto captureStatus = captureOutput.Status();
        std::wstring text = captureStatus.message;
        const bool captureActive = captureOutput.Active();
        const int captureDevice = Selected(ID_CAPTURE_DEVICE), captureMonitor = Selected(ID_CAPTURE_MONITOR);
        const bool advancedSpec = Checked(ID_CAPTURE_ADVANCED);
        const int captureSpec = Selected(advancedSpec ? ID_CAPTURE_EXACT_MODE : ID_CAPTURE_SPEC);
        const bool captureSpecAvailable = advancedSpec
            ? captureSpec >= 0 && captureSpec < static_cast<int>(uiCaptureExactAvailable.size()) && uiCaptureExactAvailable[captureSpec]
            : captureSpec >= 0 && captureSpec < static_cast<int>(uiCapturePresetAvailable.size()) && uiCapturePresetAvailable[captureSpec];
        if (!captureActive && (captureDevice < 0 || captureDevice >= static_cast<int>(uiCaptureDeviceIds.size())))
            text = W(L"请连接并选择采集设备。", L"请连接并选择采集设备喵。");
        else if (!captureActive && (captureDevice >= static_cast<int>(uiCaptureDeviceAvailable.size()) || !uiCaptureDeviceAvailable[captureDevice]))
            text = W(L"保存的采集设备当前未连接。", L"保存的采集设备现在没连接喵。");
        else if (!captureActive && captureMonitor < 0)
            text = W(L"保存的输出显示器不在，请重新选择。", L"保存的输出显示器不在，请重选喵。");
        else if (!captureActive && !captureSpecAvailable)
            text = W(L"所选规格当前不受此采集设备支持；请重新选择，程序不会自动降档。", L"所选规格现在不支持，请重选；程序不会偷偷降档喵。");
        else if (!captureActive && !advancedSpec && captureModes.empty())
            text = W(L"此设备未公开规格列表；自动兼容会直接尝试驱动当前格式。", L"设备没公开规格列表，自动兼容会直接试驱动当前格式喵。");
        else if (!captureActive && !captureModesError.empty()) text = captureModesError;
        else if (HasDraft(2)) text = W(L"画面修改尚未应用；应用后可启动。", L"画面修改还没应用，应用后可启动喵。");
        if (text.empty()) text = W(L"选择采集设备和输出显示器后即可启动。", L"选好采集设备和显示器就能启动喵。");
        text = CaptureStatusTextWithTerminalError(captureStatus, captureActive, std::move(text));
        Set(ID_CAPTURE_STATE, text);
        Set(ID_CAPTURE_TOGGLE, captureWindow ? W(L"退出全屏", L"退出全屏喵")
            : captureActive && captureFullscreenRequested ? W(L"等待视频帧…", L"等待视频帧喵…")
            : captureActive ? W(L"进入全屏", L"进入全屏喵") : W(L"启动全屏", L"启动全屏喵"));
        Enable(ID_CAPTURE_TOGGLE, !(captureActive && captureFullscreenRequested && !captureWindow));
        Enable(ID_CAPTURE_STOP, captureActive);
        Enable(ID_CAPTURE_DEVICE, !captureActive);
        Enable(ID_CAPTURE_REFRESH, !captureActive);
        Enable(ID_CAPTURE_SPEC, !captureActive);
        Enable(ID_CAPTURE_EXACT_MODE, !captureActive && !captureModes.empty());
        Enable(ID_CAPTURE_ADVANCED, !captureActive && !captureModes.empty());
        Enable(ID_CAPTURE_PRESENT_MODE, !captureActive);
        Enable(ID_CAPTURE_RANGE, captureActive);

        std::wstring requested;
        if (captureActive) {
            if (captureStatus.requestedPolicy == qp::CaptureModePolicy::Auto) {
                requested = W(L"自动低延迟", L"自动低延迟喵") +
                    (captureStatus.hasRequestedMode ? L" → " + CaptureModeLabel(captureStatus.requestedMode, false) : L"");
            } else if (captureStatus.requestedPolicy == qp::CaptureModePolicy::Preferred) {
                requested = captureStatus.hasRequestedMode
                    ? W(L"帧率优先 · ", L"帧率优先喵 · ") + CaptureModeLabel(captureStatus.requestedMode, false)
                    : W(L"帧率优先规格尚未建立", L"帧率优先规格还没建立喵");
            } else {
                requested = captureStatus.hasRequestedMode
                    ? CaptureModeLabel(captureStatus.requestedMode, false)
                    : W(L"已选择精确规格，尚未建立", L"精确规格已选择，还没建立喵");
            }
        } else {
            const int control = Checked(ID_CAPTURE_ADVANCED) ? ID_CAPTURE_EXACT_MODE : ID_CAPTURE_SPEC;
            requested = Text(Item(control));
            if (requested.empty()) requested = W(L"尚未选择", L"还没选择喵");
        }
        const std::wstring actual = captureStatus.hasActualMode
            ? CaptureModeLabel(captureStatus.actualMode, false) : L"—";
        const std::wstring measured = captureStatus.measuredFps > 0.0 ? Num(captureStatus.measuredFps) + L" fps" : L"—";
        const std::wstring shownMode = captureStatus.hasActualMode ? actual : requested;
        std::wstring summary = W(L"规格：", L"规格喵：") + shownMode + L"  ·  " + measured + L"  ·  " + CaptureBackendLabel(captureStatus.backend);
        if (captureStatus.dropCountAvailable && captureStatus.droppedFrames > 0)
            summary += W(L"  ·  掉帧 ", L"  ·  掉帧喵 ") + std::to_wstring(captureStatus.droppedFrames);
        Set(ID_CAPTURE_METRICS, summary);
    }
    if (Has(ID_FUSION_STATE)) {
        const int output = Selected(ID_FUSION_MONITOR);
        COLORREF background{};
        const bool validColor = ParseFusionColor(Text(Item(ID_FUSION_COLOR)), background);
        std::wstring text;
        if (output < 0 || output >= static_cast<int>(monitors.size()))
            text = W(L"保存的输出显示器不在，请重新选择。", L"保存的输出显示器不在，请重选喵。");
        else if (!validColor)
            text = W(L"背景色请填写 #RRGGBB，例如 #00FF00。", L"背景色请填 #RRGGBB，例如 #00FF00 喵。");
        else if (background == settings.color)
            text = W(L"背景色与光标颜色相同，融合器会把光标一起抠除。", L"背景色和光标颜色一样，光标也会被抠除喵。");
        else if (HasDraft(2))
            text = W(L"设置有修改；点击启动或校准时会自动应用。",
                L"设置有修改喵；点击启动或校准会自动应用。");
        else if (captureWindow)
            text = W(L"纯色背景与光标正在输出；采集卡未被占用。", L"纯色背景和光标正在输出喵；没有占用采集卡。");
        else
            text = W(L"将该显示器接入融合器，在融合器中抠除所选背景色。", L"把这块屏接入融合器，并在融合器里抠除背景色喵。");
        Set(ID_FUSION_STATE, text);
        Set(ID_FUSION_TOGGLE, captureWindow
            ? W(L"退出全屏", L"退出全屏喵")
            : captureFullscreenRequested ? W(L"正在打开…", L"正在打开喵…")
            : W(L"启动融合输出", L"启动融合输出喵"));
        Enable(ID_FUSION_TOGGLE, !captureFullscreenRequested || captureWindow);
        Enable(ID_FUSION_MONITOR, !captureWindow);
        Enable(ID_FUSION_PRESENT_MODE, !captureWindow);
        Enable(ID_FUSION_RANGE, captureWindow || (output >= 0 && validColor && background != settings.color));
    }
    if (Has(ID_MAPPING_HELP)) {
        const bool captureMode = IsCaptureDisplayMode(Selected(ID_MODE));
        Set(ID_MAPPING_HELP, captureMode
            ? W(L"画面会自动等比显示。范围不准时用“拖拽校准光标范围”；完成后检查四角和中心。",
                L"画面会自动等比显示喵。范围不准就拖拽校准光标范围，再检查四角和中心。")
            : W(L"框线贴住手机画面四边并排除黑边；再检查四角、中心和方向。",
                L"框线贴住手机画面四边、排除黑边，再检查四角和方向喵。"));
    }
    Set(ID_SAVE_STATE, HasDraft() ? W(L"有未应用的修改", L"有未应用修改喵") : dirty ? W(L"已应用，尚未保存", L"已应用，还没保存喵") : settings.setupComplete ? W(L"设置已保存", L"设置已保存喵") : W(L"首次设置尚未完成", L"首次设置还没完成喵"));
}

std::map<int, Application::UiValue> Application::CaptureUiValues() const {
    std::map<int, UiValue> values;
    for (const auto& c : controls) {
        if (c.page == 5) continue;
        int id = GetDlgCtrlID(c.hwnd); if (!id || id >= 300) continue;
        wchar_t cls[32]{}; GetClassNameW(c.hwnd, cls, 32); UiValue value;
        if (_wcsicmp(cls, L"Edit") == 0) value.text = Text(c.hwnd);
        else if (_wcsicmp(cls, L"ComboBox") == 0) {
            value.text = Text(c.hwnd); value.selected = static_cast<int>(SendMessageW(c.hwnd, CB_GETCURSEL, 0, 0));
            if (id == ID_WINDOW && value.selected >= 0) {
                const auto data = SendMessageW(c.hwnd, CB_GETITEMDATA, value.selected, 0);
                if (data != CB_ERR) value.identity = static_cast<UINT_PTR>(data);
            }
            if (id == ID_CAPTURE_DEVICE && value.selected >= 0 && value.selected < static_cast<int>(uiCaptureDeviceIds.size()))
                value.stableIdentity = uiCaptureDeviceIds[value.selected];
            if (id == ID_CAPTURE_SPEC && value.selected >= 0 && value.selected < static_cast<int>(uiCapturePresetModeKeys.size())) {
                value.stableIdentity = uiCapturePresetModeKeys[value.selected];
                value.identity = static_cast<UINT_PTR>(uiCapturePresetFps[value.selected]);
            }
            if (id == ID_CAPTURE_EXACT_MODE && value.selected >= 0 && value.selected < static_cast<int>(uiCaptureExactModeKeys.size()))
                value.stableIdentity = uiCaptureExactModeKeys[value.selected];
            if ((id == ID_MONITOR || id == ID_CAPTURE_MONITOR || id == ID_FUSION_MONITOR) &&
                    value.selected >= 0 && value.selected < static_cast<int>(monitors.size()))
                value.stableIdentity = monitors[value.selected].stableId;
        }
        else if (_wcsicmp(cls, L"Button") == 0 && (GetWindowLongPtrW(c.hwnd, GWL_STYLE) & BS_TYPEMASK) == BS_AUTOCHECKBOX) value.checked = static_cast<int>(SendMessageW(c.hwnd, BM_GETCHECK, 0, 0));
        else continue;
        values[id] = std::move(value);
    }
    return values;
}

void Application::RememberDraft() { uiDraft = CaptureUiValues(); }
void Application::RestoreDraft() {
    for (const auto& entry : uiDraft) {
        HWND h = Item(entry.first); if (!h) continue; const auto& value = entry.second;
        if (value.checked >= 0) SendMessageW(h, BM_SETCHECK, value.checked, 0);
        else { wchar_t cls[32]{}; GetClassNameW(h, cls, 32);
            if (_wcsicmp(cls, L"ComboBox") == 0) {
                LRESULT index = value.text.empty() ? -1 : SendMessageW(h, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(value.text.c_str()));
                // 主题切换会同时切换文案，固定选项沿用索引；设备类选项随后按稳定身份恢复。
                if (index == CB_ERR && value.selected >= 0 && value.selected < SendMessageW(h, CB_GETCOUNT, 0, 0))
                    index = value.selected;
                if (entry.first == ID_WINDOW && value.identity) { index = -1; for (size_t i = 0; i < windows.size(); ++i) if (reinterpret_cast<UINT_PTR>(windows[i].hwnd) == value.identity) { index = static_cast<LRESULT>(i); break; } }
                if (entry.first == ID_CAPTURE_DEVICE && !value.stableIdentity.empty()) { index = -1; for (size_t i = 0; i < uiCaptureDeviceIds.size(); ++i) if (CaptureDeviceIdsEqual(uiCaptureDeviceIds[i], value.stableIdentity)) { index = static_cast<LRESULT>(i); break; } }
                if (entry.first == ID_CAPTURE_SPEC) {
                    index = -1;
                    for (size_t i = 0; i < uiCapturePresetModeKeys.size(); ++i)
                        if (uiCapturePresetFps[i] == static_cast<int>(value.identity) && uiCapturePresetModeKeys[i] == value.stableIdentity) { index = static_cast<LRESULT>(i); break; }
                }
                if (entry.first == ID_CAPTURE_EXACT_MODE && !value.stableIdentity.empty()) { index = -1; for (size_t i = 0; i < uiCaptureExactModeKeys.size(); ++i) if (uiCaptureExactModeKeys[i] == value.stableIdentity) { index = static_cast<LRESULT>(i); break; } }
                if ((entry.first == ID_MONITOR || entry.first == ID_CAPTURE_MONITOR || entry.first == ID_FUSION_MONITOR) && !value.stableIdentity.empty()) { index = -1; for (size_t i = 0; i < monitors.size(); ++i) if (MatchesMonitor(monitors[i], value.stableIdentity)) { index = static_cast<LRESULT>(i); break; } }
                SendMessageW(h, CB_SETCURSEL, index, 0);
            } else SetWindowTextW(h, value.text.c_str());
        }
    }
}
void Application::ClearDraft() { uiDraft.clear(); uiBaseline.clear(); uiSkipRemember = true; }
void Application::ReloadControls() { ClearDraft(); BuildControls(); }
void Application::ReloadPage(int scope) {
    RememberDraft();
    for (const auto& c : controls) if (c.page == scope) uiDraft.erase(GetDlgCtrlID(c.hwnd));
    uiSkipRemember = true; BuildControls();
}
void Application::CommitDraft(int scope) {
    auto values = CaptureUiValues();
    for (const auto& c : controls) {
        int id = GetDlgCtrlID(c.hwnd); if (scope >= 0 && c.page != scope) continue;
        auto value = values.find(id); if (value != values.end()) { uiBaseline[id] = value->second; uiDraft.erase(id); }
    }
    UpdatePresentation();
}
bool Application::HasDraft(int scope) const {
    const auto values = CaptureUiValues();
    for (const auto& c : controls) {
        int id = GetDlgCtrlID(c.hwnd);
        // 展开开关、电脑网卡和校准倒计时不属于需要提交的配置。
        if (id == ID_NETWORK_ADVANCED || id == ID_LAN_ADDRESS || id == ID_CAPTURE_DELAY || id == ID_ALIGNMENT_CHECK) continue;
        if (scope >= 0 && c.page != scope) continue;
        auto current = values.find(id), baseline = uiBaseline.find(id);
        if (current != values.end() && (baseline == uiBaseline.end() || !(current->second == baseline->second))) return true;
    }
    return false;
}
bool Application::ReadControls(Settings& s) { return ReadPage(s, page); }

bool Application::ReadPage(Settings& s, int scope, bool showErrors) {
    s = settings;
    auto fail = [&](int id, const std::wstring& message) {
        if (showErrors) {
            int p = scope; for (const auto& c : controls) if (c.hwnd == Item(id)) { p = c.page; break; }
            if (id == ID_PORT || (id == ID_IP && Selected(ID_SOURCE) == 0)) SendMessageW(Item(ID_NETWORK_ADVANCED), BM_SETCHECK, BST_CHECKED, 0);
            SelectPage(std::max(0, p)); Error(message); SetFocus(Item(id)); SendMessageW(Item(id), EM_SETSEL, 0, -1);
        }
        return false;
    };
    auto number = [&](int id, double& v, double lo, double hi) {
        try { auto str = Text(Item(id)); size_t n = 0; double x = std::stod(str, &n); while (n < str.size() && iswspace(str[n])) ++n; if (n != str.size() || !std::isfinite(x) || x < lo || x > hi) return false; v = x; return true; } catch (...) { return false; }
    };
    auto integer = [&](int id, int& v, int lo, int hi) { double n = 0; if (!number(id, n, lo, hi) || n != std::floor(n)) return false; v = static_cast<int>(n); return true; };
    if (scope == -1 || scope == 1) {
        s.source = std::max(0, Selected(ID_SOURCE)); s.sourceIp = Text(Item(ID_IP));
        while (!s.sourceIp.empty() && iswspace(s.sourceIp.front())) s.sourceIp.erase(s.sourceIp.begin());
        while (!s.sourceIp.empty() && iswspace(s.sourceIp.back())) s.sourceIp.pop_back();
        IN_ADDR parsed{};
        if (!s.sourceIp.empty() && InetPtonW(AF_INET, s.sourceIp.c_str(), &parsed) != 1) return fail(ID_IP, W(L"请填写设备的 IPv4 地址，例如 192.168.1.80。P4 地址未知时可以先留空。", L"请输入设备 IPv4 地址；不知道 P4 地址时可以留空喵。"));
        if (s.source == 1) s.port = 6533;
        else if (!integer(ID_PORT, s.port, 1, 65535)) return fail(ID_PORT, W(L"接收端口应为 1～65535 的整数。", L"接收端口要是 1～65535 的整数喵。"));
    }
    if (scope == -1 || scope == 2) {
        s.mode = std::max(0, Selected(ID_MODE));
        if (!IsFusionDisplayMode(s.mode)) s.simpleMapping = Selected(ID_MAPPING_STYLE) == 0;
        s.foregroundOnly = Checked(ID_FOREGROUND);
        auto& g = s.geometry;
        if (s.mode == 0) { s.monitor = Selected(ID_MONITOR); if (s.monitor < 0 || s.monitor >= static_cast<int>(monitors.size())) return fail(ID_MONITOR, W(L"请选择当前可用的显示器。", L"请选择可用显示器喵。")); }
        if (s.mode == 1) {
            double* target[] = {&g.target.x, &g.target.y, &g.target.w, &g.target.h};
            for (int i = 0; i < 4; ++i) if (!number(ID_X + i, *target[i], i < 2 ? -100000 : 2, 100000)) return fail(ID_X + i, W(L"请填写有效的桌面区域；宽、高至少为 2 像素。", L"请填写有效桌面区域，宽高至少 2 像素喵。"));
        }
        if (s.mode == 2) {
            int i = Selected(ID_WINDOW);
            if (i < 0 || i >= static_cast<int>(windows.size()) || !IsWindow(windows[i].hwnd)) return fail(ID_WINDOW, W(L"请选择正在打开的采集预览窗口；列表里没有时点击“刷新窗口”。", L"请选择打开的采集预览窗口，找不到就刷新窗口喵。"));
            s.windowTitle = windows[i].title;
        }
        if (IsCaptureDisplayMode(s.mode)) {
            const int device = Selected(ID_CAPTURE_DEVICE);
            if (device < 0 || device >= static_cast<int>(uiCaptureDeviceIds.size()))
                return fail(ID_CAPTURE_DEVICE, W(L"没有选中采集设备。请连接采集卡后点击“刷新采集设备”。", L"还没选采集设备，连接采集卡后刷新一下喵。"));
            if (device >= static_cast<int>(uiCaptureDeviceAvailable.size()) || !uiCaptureDeviceAvailable[device])
                return fail(ID_CAPTURE_DEVICE, W(L"上次使用的采集设备当前没有连接。请检查采集卡，刷新后重新选择。", L"上次的采集设备现在没连接，检查采集卡并刷新喵。"));
            const int output = Selected(ID_CAPTURE_MONITOR);
            if (output < 0 || output >= static_cast<int>(monitors.size()))
                return fail(ID_CAPTURE_MONITOR, W(L"请选择全屏画面要输出到哪一台显示器。", L"请选择全屏画面要输出到哪块屏幕喵。"));
            s.captureDeviceId = uiCaptureDeviceIds[device];
            s.captureDeviceName = uiCaptureDeviceNames[device];
            s.captureMonitorId = monitors[output].stableId;
            if (!CaptureDeviceIdsEqual(captureModesDeviceId, s.captureDeviceId)) RefreshCaptureModesForDevice(s.captureDeviceId);
            if (captureModes.empty()) {
                if (Checked(ID_CAPTURE_ADVANCED))
                    return fail(ID_CAPTURE_EXACT_MODE, W(L"此设备没有公开可验证的精确规格，请改用自动兼容。", L"设备没公开精确规格，请用自动兼容喵。"));
                const int preset = Selected(ID_CAPTURE_SPEC);
                if (preset < 0 || preset >= static_cast<int>(uiCapturePresetFps.size()) ||
                    uiCapturePresetFps[preset] != 0)
                    return fail(ID_CAPTURE_SPEC, W(L"此设备只能使用自动兼容模式。", L"这个设备请使用自动兼容喵。"));
                s.captureFpsPreference = 0;
                s.captureModeKey.clear();
                s.captureModeSelection = CaptureModeSelection::Auto;
            } else if (Checked(ID_CAPTURE_ADVANCED)) {
                const int mode = Selected(ID_CAPTURE_EXACT_MODE);
                if (mode < 0 || mode >= static_cast<int>(uiCaptureExactModeKeys.size()))
                    return fail(ID_CAPTURE_EXACT_MODE, W(L"请选择采集设备真实报告的一项精确规格。", L"请选择采集设备真实报告的精确规格喵。"));
                if (mode >= static_cast<int>(uiCaptureExactAvailable.size()) || !uiCaptureExactAvailable[mode] ||
                    !FindCaptureMode(captureModes, uiCaptureExactModeKeys[mode]))
                    return fail(ID_CAPTURE_EXACT_MODE, W(L"保存的精确规格当前不受此设备支持，请重新选择。程序不会自动降档。", L"保存的精确规格现在不支持，请重选；程序不会偷偷降档喵。"));
                s.captureModeKey = uiCaptureExactModeKeys[mode];
                s.captureModeSelection = CaptureModeSelection::Exact;
                const auto* exact = FindCaptureMode(captureModes, s.captureModeKey);
                const int bucket = exact ? static_cast<int>(qp::CaptureOutput::RefreshRateBucket(exact->fpsNumerator, exact->fpsDenominator)) : 0;
                s.captureFpsPreference = bucket == 60 || bucket == 120 || bucket == 144 || bucket == 165 || bucket == 240 ? bucket : 0;
            } else {
                const int preset = Selected(ID_CAPTURE_SPEC);
                if (preset < 0 || preset >= static_cast<int>(uiCapturePresetModeKeys.size()))
                    return fail(ID_CAPTURE_SPEC, W(L"请选择自动低延迟或设备支持的帧率档位。", L"请选择自动低延迟或设备支持的帧率档位喵。"));
                if (preset >= static_cast<int>(uiCapturePresetAvailable.size()) || !uiCapturePresetAvailable[preset])
                    return fail(ID_CAPTURE_SPEC, W(L"所选帧率档位当前不受此设备支持，请重新选择。程序不会自动降档。", L"所选帧率现在不支持，请重选；程序不会偷偷降档喵。"));
                s.captureFpsPreference = uiCapturePresetFps[preset];
                s.captureModeKey = uiCapturePresetModeKeys[preset];
                s.captureModeSelection = s.captureFpsPreference == 0
                    ? CaptureModeSelection::Auto : CaptureModeSelection::Preferred;
                if (s.captureFpsPreference == 0) s.captureModeKey.clear();
                else if (!FindCaptureMode(captureModes, s.captureModeKey))
                    return fail(ID_CAPTURE_SPEC, W(L"设备能力已经变化，请刷新后重新选择采集规格。", L"设备能力变了，请刷新后重选采集规格喵。"));
            }
            s.capturePresentMode = Checked(ID_CAPTURE_PRESENT_MODE) ? 1 : 0;
        }
        if (IsFusionDisplayMode(s.mode)) {
            const int output = Selected(ID_FUSION_MONITOR);
            if (output < 0 || output >= static_cast<int>(monitors.size()))
                return fail(ID_FUSION_MONITOR, W(L"请选择纯色画面要输出到哪一台显示器。",
                    L"请选择纯色画面要输出到哪块屏幕喵。"));
            s.fusionMonitorId = monitors[output].stableId;
            if (!ParseFusionColor(Text(Item(ID_FUSION_COLOR)), s.fusionBackgroundColor))
                return fail(ID_FUSION_COLOR, W(L"背景色请填写六位十六进制颜色，例如 #00FF00。",
                    L"背景色请填六位十六进制颜色，例如 #00FF00 喵。"));
            if (s.fusionBackgroundColor == s.color)
                return fail(ID_FUSION_COLOR, W(L"背景色与光标颜色完全相同，融合器会同时抠除光标。请更换其中一种颜色。",
                    L"背景色和光标颜色一样喵，融合器会把光标也抠除，请换一种颜色。"));
            s.fusionPresentMode = Checked(ID_FUSION_PRESENT_MODE) ? 1 : 0;
        }
        if (!s.simpleMapping && !IsFusionDisplayMode(s.mode)) {
            if (!IsCaptureDisplayMode(s.mode)) {
                if (!number(ID_FRAME_W, g.frameWidth, 2, 65536)) return fail(ID_FRAME_W, W(L"完整采集帧宽度应为 2～65536。", L"采集帧宽度要在 2～65536 之间喵。"));
                if (!number(ID_FRAME_H, g.frameHeight, 2, 65536)) return fail(ID_FRAME_H, W(L"完整采集帧高度应为 2～65536。", L"采集帧高度要在 2～65536 之间喵。"));
            } else if (auto frame = presentedCaptureFrame) {
                g.frameWidth = frame->width;
                g.frameHeight = frame->height;
            }
            double* roi[] = {&g.content.x, &g.content.y, &g.content.w, &g.content.h, &g.crop.x, &g.crop.y, &g.crop.w, &g.crop.h};
            for (int i = 0; i < 8; ++i) { double n = 0; if (!number(ID_CONTENT_X + i, n, 0, 100)) return fail(ID_CONTENT_X + i, W(L"区域百分比应在 0～100 之间。", L"区域百分比要在 0～100 之间喵。")); *roi[i] = n / 100; }
            g.stretch = Checked(ID_STRETCH);
        }
        g.rotation = std::max(0, Selected(ID_ROTATE)) * 90; g.mirrorX = Checked(ID_MIRROR_X); g.mirrorY = Checked(ID_MIRROR_Y);
        auto effective = g;
        if (IsFusionDisplayMode(s.mode)) effective.calibration = s.fusionCalibration;
        if (s.simpleMapping || IsFusionDisplayMode(s.mode)) {
            effective.content = effective.crop = {0, 0, 1, 1}; effective.stretch = true;
        }
        std::string error; if (!qp::ValidateGeometry(effective, error)) return fail(s.simpleMapping ? ID_MAPPING_STYLE : ID_CONTENT_X, W(L"请检查画面区域：", L"请检查画面区域喵：") + Wide(error));
        if (!Checked(ID_ALIGNMENT_CHECK)) s.setupComplete = false;
    }
    if (scope == -1 || scope == 4) {
        if (!integer(ID_SIZE, s.size, 12, 144)) return fail(ID_SIZE, W(L"指针大小应为 12～144 像素的整数。", L"指针大小要是 12～144 像素的整数喵。"));
        if (!integer(ID_DELAY, s.delayMs, 0, 2000)) return fail(ID_DELAY, W(L"显示延迟应为 0～2000 毫秒。", L"显示延迟要在 0～2000 毫秒之间喵。"));
        if (!integer(ID_TIMEOUT, s.timeoutMs, 600, 15000)) return fail(ID_TIMEOUT, W(L"断流隐藏时间应为 600～15000 毫秒。", L"断流隐藏时间要在 600～15000 毫秒之间喵。"));
        if (s.timeoutMs <= s.delayMs + 500) return fail(ID_TIMEOUT, W(L"断流隐藏时间需要比显示延迟多 500 毫秒以上。", L"断流隐藏时间要比显示延迟多 500 毫秒以上喵。"));
        s.shape = std::clamp(Selected(ID_SHAPE), 0, 7); s.anime = Checked(ID_ANIME);
        s.resumeOnLaunch = Checked(ID_RESUME_LAUNCH);
    }
    return true;
}

void Application::RefreshCaptureModesForDevice(const std::wstring& deviceId) {
    captureModes.clear();
    captureModesError.clear();
    captureModesDeviceId = deviceId;
    if (deviceId.empty()) return;
    const auto device = std::find_if(captureDevices.begin(), captureDevices.end(),
        [&](const qp::CaptureDevice& item) { return CaptureDeviceIdsEqual(item.symbolicLink, deviceId); });
    if (device != captureDevices.end()) captureModesDeviceId = device->symbolicLink;
    const bool enumerated = device != captureDevices.end()
        ? qp::CaptureOutput::EnumerateModes(*device, captureModes, captureModesError)
        : qp::CaptureOutput::EnumerateModes(deviceId, captureModes, captureModesError);
    if (!enumerated) captureModes.clear();
    for (auto& mode : captureModes)
        if (mode.stableKey.empty()) mode.stableKey = qp::CaptureOutput::BuildModeStableKey(mode);
    if (captureModes.empty() && captureModesError.empty())
        captureModesError = W(L"采集设备没有报告可用的视频规格。", L"采集设备没有报告可用视频规格喵。");
}

void Application::Inventory() {
    monitors.clear(); windows.clear();
    const auto displayIdentities = ActiveDisplayIdentities();
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR h, HDC, LPRECT, LPARAM l) -> BOOL {
        auto* a = reinterpret_cast<Application*>(l); MonitorItem m{}; m.handle = h; m.info.cbSize = sizeof(m.info);
        GetMonitorInfoW(h, &m.info); a->monitors.push_back(m); return TRUE;
    }, reinterpret_cast<LPARAM>(this));
    for (auto& monitor : monitors) {
        monitor.stableId = monitor.info.szDevice;
        auto identity = std::find_if(displayIdentities.begin(), displayIdentities.end(), [&](const DisplayIdentity& item) {
            return CompareStringOrdinal(item.gdiName.c_str(), -1, monitor.info.szDevice, -1, TRUE) == CSTR_EQUAL;
        });
        if (identity != displayIdentities.end()) {
            monitor.stableId = identity->stableId;
            monitor.friendlyName = identity->friendlyName;
            monitor.refreshHz = identity->refreshHz;
        }
        if (monitor.refreshHz <= 0) {
            DEVMODEW mode{}; mode.dmSize = sizeof(mode);
            if (EnumDisplaySettingsExW(monitor.info.szDevice, ENUM_CURRENT_SETTINGS, &mode, 0) &&
                mode.dmDisplayFrequency > 1)
                monitor.refreshHz = mode.dmDisplayFrequency;
        }
    }
    std::stable_sort(monitors.begin(), monitors.end(), [](const MonitorItem& a, const MonitorItem& b) { return (a.info.dwFlags & MONITORINFOF_PRIMARY) > (b.info.dwFlags & MONITORINFOF_PRIMARY); });
    EnumWindows([](HWND h, LPARAM l) -> BOOL {
        auto* a = reinterpret_cast<Application*>(l); DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
        if (!IsWindowVisible(h) || pid == GetCurrentProcessId() || GetWindowTextLengthW(h) == 0 || (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)) return TRUE;
        a->windows.push_back({h, Text(h)}); return TRUE;
    }, reinterpret_cast<LPARAM>(this));
    if (settings.mode == 2 && !IsWindow(targetWindow)) {
        HWND unique = nullptr; size_t matches = 0;
        for (const auto& window : windows) if (window.title == settings.windowTitle) { unique = window.hwnd; ++matches; }
        targetWindow = matches == 1 ? unique : nullptr;
    }
    std::vector<qp::CaptureDevice> foundCaptureDevices;
    std::wstring captureError;
    if (qp::CaptureOutput::EnumerateDevices(foundCaptureDevices, captureError)) captureDevices = std::move(foundCaptureDevices);
    else {
        captureDevices.clear();
        if (!captureError.empty()) notice = captureError;
    }
    std::wstring modeDeviceId = settings.captureDeviceId;
    const auto draftDevice = uiDraft.find(ID_CAPTURE_DEVICE);
    if (draftDevice != uiDraft.end() && !draftDevice->second.stableIdentity.empty()) modeDeviceId = draftDevice->second.stableIdentity;
    if (modeDeviceId.empty() && captureDevices.size() == 1) modeDeviceId = captureDevices.front().symbolicLink;
    const auto modeDevice = std::find_if(captureDevices.begin(), captureDevices.end(), [&](const qp::CaptureDevice& device) {
        return CaptureDeviceIdsEqual(device.symbolicLink, modeDeviceId);
    });
    if (modeDevice != captureDevices.end()) {
        modeDeviceId = modeDevice->symbolicLink;
        if (!captureOutput.Active() && !CaptureDeviceIdsEqual(modeDeviceId, captureModesDeviceId))
            RefreshCaptureModesForDevice(modeDeviceId);
    } else {
        captureModes.clear();
        captureModesError.clear();
        captureModesDeviceId = modeDeviceId;
    }
    InventoryAddresses();
}

void Application::InventoryAddresses() {
    localAddresses.clear(); ULONG bytes = 16384; std::vector<unsigned char> storage(bytes);
    auto read = [&] { return GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data()), &bytes); };
    ULONG result = read(); if (result == ERROR_BUFFER_OVERFLOW) { storage.resize(bytes); result = read(); }
    if (result != NO_ERROR) return;
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data()); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* address = a->FirstUnicastAddress; address; address = address->Next) {
            if (!address->Address.lpSockaddr || address->Address.lpSockaddr->sa_family != AF_INET) continue;
            auto* ipv4 = reinterpret_cast<sockaddr_in*>(address->Address.lpSockaddr); wchar_t text[INET_ADDRSTRLEN]{};
            if (!InetNtopW(AF_INET, &ipv4->sin_addr, text, INET_ADDRSTRLEN)) continue;
            std::wstring ip = text; if (ip == L"0.0.0.0" || ip.compare(0, 8, L"169.254.") == 0) continue;
            localAddresses.push_back({ip, std::wstring(a->FriendlyName ? a->FriendlyName : L"IPv4") + L"  ·  " + ip});
        }
    }
}

void Application::TrackControls(size_t first, UiCondition condition) {
    for (size_t i = first; i < controls.size(); ++i) uiConditional.push_back({controls[i].hwnd, condition});
}

void Application::BuildControls() {
    if (uiTransition.Active()) uiTransition.Cancel();
#if QINEX_WITH_ANDROID_TOOLS
    RememberAndroidDraft();
#endif
    const bool redrawAfterBuild = main && IsWindowVisible(main);
    if (redrawAfterBuild) SendMessageW(main, WM_SETREDRAW, FALSE, 0);
    if (!uiSkipRemember && !controls.empty()) RememberDraft();
    uiSkipRemember = false; uiBuilding = true;
    for (auto& c : controls) DestroyWindow(c.hwnd);
    controls.clear(); uiConditional.clear();
    if (font) DeleteObject(font); if (headingFont) DeleteObject(headingFont);
    if (uiSmallFont) DeleteObject(uiSmallFont); if (uiStrongFont) DeleteObject(uiStrongFont);
    MONITORINFO mi{sizeof(mi)}; GetMonitorInfoW(MonitorFromWindow(main, MONITOR_DEFAULTTONEAREST), &mi);
    scale = std::min(GetDpiForWindow(main) / 96.0, std::max(0.65,
        std::min((mi.rcWork.bottom - mi.rcWork.top - 70) / 744.0,
                 (mi.rcWork.right - mi.rcWork.left - 40) / 1064.0)));
    if (hostWindow) {
        RECT bounds{}; GetClientRect(main, &bounds);
        scale = std::min(bounds.right / 1064.0, bounds.bottom / 744.0);
    }
    font = CreateFontW(-Px(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
    headingFont = CreateFontW(-Px(24), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
    uiSmallFont = CreateFontW(-Px(12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
    uiStrongFont = CreateFontW(-Px(15), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
    if (!hostWindow) {
        RECT outer{0, 0, Px(1064), Px(704)}; AdjustWindowRectEx(&outer, static_cast<DWORD>(GetWindowLongPtrW(main, GWL_STYLE)), FALSE, 0);
        SetWindowPos(main, nullptr, 0, 0, outer.right - outer.left, outer.bottom - outer.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    const wchar_t* names[] = {L"总览\n状态与快捷操作", L"设备连接\nP4 / S3", L"画面输出\n采集 · 覆盖 · 融合",
        L"光标校准\n范围与九点精调", L"个性设置\n外观与快捷键", L"手机工具\nAndroid 系统光标"};
    for (int i = 0; i < kPageCount; ++i)
        Add(L"BUTTON", names[i], WS_TABSTOP | BS_PUSHBUTTON, ID_NAV_FIRST + i, 14, 132 + i * 59, 156, 54, -1);
    Button(ID_UI_THEME, settings.anime ? L"切换普通模式" : L"切换二次元模式", 20, 592, 144, -1);
    Add(L"STATIC", L"", SS_LEFT, ID_PAGE_TITLE, 26, 57, 680, 33, -1);
    Add(L"STATIC", L"", SS_LEFT, ID_UI_SUBTITLE, 26, 80, 800, 24, -1);
    SendMessageW(Item(ID_PAGE_TITLE), WM_SETFONT, reinterpret_cast<WPARAM>(headingFont), TRUE);
    Button(ID_GUIDE, W(L"本页帮助", L"本页帮助喵"), 738, 58, 114, -1);

    // 首页集中呈现日常所需的状态和动作。
    Add(L"BUTTON", W(L"当前显示", L"当前显示喵"), BS_GROUPBOX, 0, 26, 105, 826, 142, 0);
    Add(L"STATIC", L"", SS_LEFT, ID_HOME_STATE, 46, 138, 782, 36, 0);
    SendMessageW(Item(ID_HOME_STATE), WM_SETFONT, reinterpret_cast<WPARAM>(headingFont), TRUE);
    Add(L"STATIC", L"", SS_LEFT, ID_HOME_DETAIL, 46, 187, 780, 45, 0);
    Add(L"STATIC", L"", SS_LEFT, ID_HOME_DEVICE, 26, 270, 630, 29, 0);
    Button(ID_HOME_CHANGE_DEVICE, W(L"更换设备", L"更换设备喵"), 701, 267, 150, 0);
    Add(L"STATIC", L"", SS_LEFT, ID_HOME_PREVIEW, 26, 317, 630, 47, 0);
    Button(ID_HOME_CHANGE_PREVIEW, W(L"选择预览", L"选择预览喵"), 701, 314, 150, 0);
    Add(L"BUTTON", L"", WS_TABSTOP | BS_DEFPUSHBUTTON, ID_HOME_PRIMARY, 26, 387, 280, 45, 0);
    Button(ID_HOME_REGION, W(L"重新框选画面", L"重新框选喵"), 329, 395, 168, 0);
    Button(ID_HOME_APPEARANCE, W(L"指针外观", L"指针外观喵"), 514, 395, 151, 0);
    Button(ID_HOME_CALIBRATE, W(L"检查与校准", L"检查与校准喵"), 682, 395, 168, 0);

    Label(W(L"指针来自", L"指针来自喵"), 26, 108, 188, 1);
    Combo(ID_SOURCE, {W(L"S3 + QInEX（本机）", L"S3 + QInEX（本机）喵"), W(L"P4 板子（单机 / 上位机直驱）", L"P4 板子（单机 / 上位机直驱）喵")}, settings.source, 230, 102, 622, 1);
    size_t group = controls.size();
    Label(W(L"这台电脑的地址", L"这台电脑的地址喵"), 26, 153, 195, 1);
    std::vector<std::wstring> addresses; for (const auto& address : localAddresses) addresses.push_back(address.label);
    if (addresses.empty()) addresses.push_back(W(L"未找到可用地址，请检查电脑网络", L"没有可用地址，检查电脑网络喵"));
    Combo(ID_LAN_ADDRESS, addresses, 0, 230, 147, 489, 1);
    Button(ID_COPY_ADDRESS, W(L"复制地址", L"复制地址喵"), 732, 146, 120, 1); TrackControls(group, UiP4);
    Add(L"STATIC", L"", SS_LEFT, ID_DEVICE_HELP, 26, 190, 826, 59, 1);
    group = controls.size();
    Add(L"STATIC", L"", SS_LEFT, ID_SOURCE_LABEL, 26, 269, 200, 26, 1);
    Edit(ID_IP, settings.sourceIp, 230, 263, 317, 1);
    Button(ID_BIND_SOURCE, W(L"使用收到数据的这台设备", L"绑定收到数据的设备喵"), 562, 262, 290, 1); TrackControls(group, UiSourceAddress);
    Check(ID_NETWORK_ADVANCED, W(L"高级连接设置", L"高级连接设置喵"), settings.source == 0 && settings.port != 6533, 26, 308, 208, 1);
    group = controls.size();
    Label(W(L"接收端口", L"接收端口喵"), 255, 313, 115, 1); Edit(ID_PORT, std::to_wstring(settings.port), 376, 307, 111, 1);
    Label(W(L"发送端与这里保持一致", L"发送端和这里要一致喵"), 507, 313, 345, 1); TrackControls(group, UiNetworkExtra);
    Add(L"STATIC", L"", SS_LEFT, ID_DEVICE_STATE, 26, 354, 826, 48, 1);
    Button(ID_SIMULATOR, W(L"演示指针（15 秒）", L"演示指针（15 秒）喵"), 26, 421, 240, 1);
    Button(ID_START, W(L"开始连接", L"开始连接喵"), 282, 421, 165, 1);
    Button(ID_STOP, W(L"断开设备", L"断开设备喵"), 463, 421, 165, 1);
    Button(ID_DIAGNOSTICS, W(L"诊断详情", L"诊断详情喵"), 644, 421, 208, 1);

    // 基础模式直接对应完整手机矩形；裁剪设置按需展开。
    Label(W(L"显示方式", L"显示方式喵"), 26, 108, 108, 2);
    Combo(ID_MODE, {W(L"显示器全屏", L"显示器全屏喵"), W(L"固定桌面区域", L"固定桌面区域喵"),
        W(L"跟随预览窗口", L"跟随预览窗口喵"), W(L"采集卡全屏输出", L"采集卡全屏输出喵"),
        W(L"纯色背景全屏（融合器）", L"纯色背景全屏（融合器）喵")}, settings.mode, 140, 102, 234, 2);
    group = controls.size(); std::vector<std::wstring> monitorNames;
    for (size_t i = 0; i < monitors.size(); ++i) {
        auto r = FromRect(monitors[i].info.rcMonitor);
        std::wstring name = W(L"显示器 ", L"显示器喵 ") + std::to_wstring(i + 1);
        if (monitors[i].info.dwFlags & MONITORINFOF_PRIMARY)
            name += W(L"（主屏）", L"（主屏喵）");
        if (!monitors[i].friendlyName.empty()) name += L"  ·  " + monitors[i].friendlyName;
        name += L"  ·  " + Num(r.w) + L" × " + Num(r.h);
        if (monitors[i].refreshHz > 0) name += L" @ " + Num(monitors[i].refreshHz) + L" Hz";
        monitorNames.push_back(std::move(name));
    }
    Combo(ID_MONITOR, monitorNames, settings.monitor, 392, 102, 460, 2);
    for (size_t i = 0; i < monitors.size(); ++i) SendMessageW(Item(ID_MONITOR), CB_SETITEMDATA, i, reinterpret_cast<LPARAM>(monitors[i].stableId.c_str()));
    TrackControls(group, UiMonitor);
    group = controls.size(); Label(W(L"预览窗口", L"预览窗口喵"), 26, 150, 108, 2);
    std::vector<std::wstring> titles; int windowIndex = -1;
    for (size_t i = 0; i < windows.size(); ++i) { titles.push_back(windows[i].title); if (windows[i].hwnd == targetWindow) windowIndex = static_cast<int>(i); }
    Combo(ID_WINDOW, titles, windowIndex, 140, 144, 566, 2);
    for (size_t i = 0; i < windows.size(); ++i) SendMessageW(Item(ID_WINDOW), CB_SETITEMDATA, i, reinterpret_cast<LPARAM>(windows[i].hwnd));
    Button(ID_REFRESH, W(L"刷新窗口", L"刷新窗口喵"), 724, 143, 128, 2); TrackControls(group, UiWindow);
    group = controls.size(); Label(W(L"左 / 上 / 宽 / 高", L"左 / 上 / 宽 / 高喵"), 26, 150, 242, 2);
    const double targets[] = {settings.geometry.target.x, settings.geometry.target.y, settings.geometry.target.w, settings.geometry.target.h};
    for (int i = 0; i < 4; ++i) Edit(ID_X + i, Num(targets[i]), 305 + i * 137, 144, 120, 2); TrackControls(group, UiCustom);
    group = controls.size(); Button(ID_REGION, W(L"框住完整手机画面", L"框住完整手机画面喵"), 26, 185, 268, 2); TrackControls(group, UiDesktopTarget);
    group = controls.size();
    Button(ID_ENTIRE, W(L"使用整个预览窗口", L"使用整个预览窗口喵"), 313, 185, 225, 2);
    Check(ID_FOREGROUND, W(L"仅在预览前台时显示", L"仅在预览前台显示喵"), settings.foregroundOnly, 565, 186, 287, 2); TrackControls(group, UiWindow);

    // 全屏输出直接采集视频并与指针合成；下拉项使用稳定身份恢复，友好名只负责展示。
    uiCaptureDeviceIds.clear(); uiCaptureDeviceNames.clear(); uiCaptureDeviceLabels.clear(); uiCaptureDeviceAvailable.clear();
    for (const auto& device : captureDevices) {
        uiCaptureDeviceIds.push_back(device.symbolicLink);
        uiCaptureDeviceNames.push_back(device.friendlyName.empty() ? W(L"未命名采集设备", L"未命名采集设备喵") : device.friendlyName);
        uiCaptureDeviceLabels.push_back(uiCaptureDeviceNames.back());
        uiCaptureDeviceAvailable.push_back(true);
    }
    for (size_t i = 0; i < uiCaptureDeviceLabels.size(); ++i) {
        int sameName = 0, ordinal = 0;
        for (size_t j = 0; j < uiCaptureDeviceNames.size(); ++j) if (uiCaptureDeviceNames[j] == uiCaptureDeviceNames[i]) {
            ++sameName; if (j <= i) ++ordinal;
        }
        if (sameName > 1) uiCaptureDeviceLabels[i] += W(L"  ·  同名设备 ", L"  ·  同名设备喵 ") + std::to_wstring(ordinal);
    }
    auto appendOfflineCapture = [&](const std::wstring& id, const std::wstring& name) {
        if (id.empty() || std::any_of(uiCaptureDeviceIds.begin(), uiCaptureDeviceIds.end(),
                [&](const std::wstring& existing) { return CaptureDeviceIdsEqual(existing, id); })) return;
        uiCaptureDeviceIds.push_back(id);
        uiCaptureDeviceNames.push_back(name.empty() ? W(L"上次使用的采集设备", L"上次使用的采集设备喵") : name);
        uiCaptureDeviceLabels.push_back(uiCaptureDeviceNames.back() + W(L"（当前未连接）", L"（当前没连接喵）"));
        uiCaptureDeviceAvailable.push_back(false);
    };
    appendOfflineCapture(settings.captureDeviceId, settings.captureDeviceName);
    const auto draftCapture = uiDraft.find(ID_CAPTURE_DEVICE);
    if (draftCapture != uiDraft.end()) appendOfflineCapture(draftCapture->second.stableIdentity, draftCapture->second.text);
    const std::wstring selectedCaptureId = draftCapture != uiDraft.end() && !draftCapture->second.stableIdentity.empty()
        ? draftCapture->second.stableIdentity : settings.captureDeviceId;
    int captureIndex = -1;
    for (size_t i = 0; i < uiCaptureDeviceIds.size(); ++i) if (CaptureDeviceIdsEqual(uiCaptureDeviceIds[i], selectedCaptureId)) { captureIndex = static_cast<int>(i); break; }
    if (captureIndex < 0 && uiCaptureDeviceIds.size() == 1 && settings.captureDeviceId.empty()) captureIndex = 0;
    const bool modesForSelectedDevice = !captureModesDeviceId.empty() &&
        captureIndex >= 0 && captureIndex < static_cast<int>(uiCaptureDeviceIds.size()) &&
        CaptureDeviceIdsEqual(uiCaptureDeviceIds[captureIndex], captureModesDeviceId);
    const bool selectedCaptureAvailable = captureIndex >= 0 &&
        captureIndex < static_cast<int>(uiCaptureDeviceAvailable.size()) &&
        uiCaptureDeviceAvailable[captureIndex];
    uiCapturePresetModeKeys.clear(); uiCapturePresetFps.clear(); uiCapturePresetAvailable.clear();
    uiCaptureExactModeKeys.clear(); uiCaptureExactAvailable.clear();
    std::vector<std::wstring> presetLabels, exactLabels;
    auto addPreset = [&](int fps, const std::wstring& key, const std::wstring& label, bool available) {
        uiCapturePresetFps.push_back(fps); uiCapturePresetModeKeys.push_back(key);
        uiCapturePresetAvailable.push_back(available); presetLabels.push_back(label);
    };
    const auto* automatic = modesForSelectedDevice ? BestCaptureMode(captureModes) : nullptr;
    addPreset(0, L"", automatic
        ? W(L"自动低延迟（推荐） · ", L"自动低延迟（推荐）喵 · ") + CaptureModeLabel(*automatic, false)
        : modesForSelectedDevice
            ? W(L"自动兼容（使用驱动当前格式）", L"自动兼容（用驱动当前格式喵）")
            : W(L"自动低延迟（选择设备后检测）", L"自动低延迟（选设备后检测喵）"),
        selectedCaptureAvailable);
    for (const int fps : {60, 120, 144, 165, 240}) {
        const auto* preferred = modesForSelectedDevice ? BestCaptureMode(captureModes, static_cast<unsigned>(fps)) : nullptr;
        if (preferred) addPreset(fps, CaptureModeKey(*preferred), std::to_wstring(fps) +
            W(L" 帧率优先 · ", L" 帧率优先喵 · ") + CaptureModeLabel(*preferred, false), true);
    }
    for (const auto& mode : captureModes) {
        uiCaptureExactModeKeys.push_back(CaptureModeKey(mode));
        uiCaptureExactAvailable.push_back(true);
        exactLabels.push_back(CaptureModeLabel(mode, true));
    }
    int presetIndex = 0, exactIndex = -1;
    const bool savedModeMatchesDevice = modesForSelectedDevice && CaptureDeviceIdsEqual(settings.captureDeviceId, captureModesDeviceId);
    if (!captureModes.empty() && savedModeMatchesDevice && !settings.captureModeKey.empty()) {
        if (settings.captureModeSelection == CaptureModeSelection::Preferred) {
            presetIndex = -1;
            const auto preferredMode = std::find_if(captureModes.begin(), captureModes.end(),
                [&](const qp::CaptureNativeMode& mode) {
                    return qp::CaptureOutput::ModeMatchesTimingStableKey(mode, settings.captureModeKey);
                });
            if (preferredMode != captureModes.end()) {
                bool attached = false;
                for (size_t i = 0; i < uiCapturePresetFps.size(); ++i) {
                    if (uiCapturePresetFps[i] != settings.captureFpsPreference) continue;
                    uiCapturePresetModeKeys[i] = CaptureModeKey(*preferredMode);
                    presetLabels[i] = std::to_wstring(settings.captureFpsPreference) +
                        W(L" 帧率优先 · ", L" 帧率优先喵 · ") + CaptureModeLabel(*preferredMode, false);
                    presetIndex = static_cast<int>(i);
                    attached = true;
                    break;
                }
                if (!attached) {
                    addPreset(settings.captureFpsPreference, CaptureModeKey(*preferredMode),
                        std::to_wstring(settings.captureFpsPreference) +
                            W(L" 帧率优先 · ", L" 帧率优先喵 · ") +
                            CaptureModeLabel(*preferredMode, false), true);
                    presetIndex = static_cast<int>(presetLabels.size()) - 1;
                }
            }
            if (presetIndex < 0) {
                addPreset(settings.captureFpsPreference, settings.captureModeKey,
                    std::to_wstring(settings.captureFpsPreference) +
                        W(L" 帧率优先（当前设备不支持）", L" 帧率优先（当前设备不支持喵）"), false);
                presetIndex = static_cast<int>(presetLabels.size()) - 1;
            }
        } else if (settings.captureModeSelection == CaptureModeSelection::Exact) {
            for (size_t i = 0; i < captureModes.size(); ++i) {
                if (qp::CaptureOutput::ModeMatchesStableKey(captureModes[i], settings.captureModeKey)) {
                    exactIndex = static_cast<int>(i);
                    break;
                }
            }
            if (exactIndex < 0) {
                uiCaptureExactModeKeys.push_back(settings.captureModeKey);
                uiCaptureExactAvailable.push_back(false);
                exactLabels.push_back(W(L"上次使用的精确规格（当前设备不支持）", L"上次的精确规格（当前设备不支持喵）"));
                exactIndex = static_cast<int>(exactLabels.size()) - 1;
            }
        }
    }
    // DPI/显示器变化会重建控件。若用户刚从精确规格切到普通档，草稿中的
    // preferred key 可能不是该帧率档默认挑出的分辨率，需要把它重新挂回同一档。
    const auto draftSpec = uiDraft.find(ID_CAPTURE_SPEC);
    if (modesForSelectedDevice && draftSpec != uiDraft.end() &&
            draftSpec->second.identity > 0 && !draftSpec->second.stableIdentity.empty()) {
        const auto draftMode = std::find_if(captureModes.begin(), captureModes.end(),
            [&](const qp::CaptureNativeMode& mode) {
                return qp::CaptureOutput::ModeMatchesTimingStableKey(
                    mode, draftSpec->second.stableIdentity);
            });
        if (draftMode != captureModes.end()) {
            bool attached = false;
            for (size_t index = 0; index < uiCapturePresetFps.size(); ++index) {
                if (uiCapturePresetFps[index] != static_cast<int>(draftSpec->second.identity)) continue;
                uiCapturePresetModeKeys[index] = CaptureModeKey(*draftMode);
                presetLabels[index] = std::to_wstring(uiCapturePresetFps[index]) +
                    W(L" 帧率优先 · ", L" 帧率优先喵 · ") + CaptureModeLabel(*draftMode, false);
                attached = true;
                break;
            }
            if (!attached) {
                const int fps = static_cast<int>(draftSpec->second.identity);
                addPreset(fps, CaptureModeKey(*draftMode), std::to_wstring(fps) +
                    W(L" 帧率优先 · ", L" 帧率优先喵 · ") +
                    CaptureModeLabel(*draftMode, false), true);
            }
        }
    }
    if (exactIndex < 0 && automatic) {
        const auto key = CaptureModeKey(*automatic);
        for (size_t i = 0; i < uiCaptureExactModeKeys.size(); ++i) if (uiCaptureExactModeKeys[i] == key) { exactIndex = static_cast<int>(i); break; }
    }
    if (exactLabels.empty()) {
        uiCaptureExactModeKeys.push_back(L""); uiCaptureExactAvailable.push_back(false);
        exactLabels.push_back(W(L"设备未报告可用精确规格", L"设备没报告可用精确规格喵")); exactIndex = 0;
    }
    const bool captureAdvanced = settings.captureModeSelection == CaptureModeSelection::Exact;

    group = controls.size(); Add(L"STATIC", L"", SS_LEFT, ID_CAPTURE_STATE, 392, 104, 460, 39, 2);
    Label(W(L"采集设备", L"采集设备喵"), 26, 150, 108, 2);
    Combo(ID_CAPTURE_DEVICE, uiCaptureDeviceLabels, captureIndex, 140, 144, 566, 2);
    for (size_t i = 0; i < uiCaptureDeviceIds.size(); ++i) SendMessageW(Item(ID_CAPTURE_DEVICE), CB_SETITEMDATA, i, reinterpret_cast<LPARAM>(uiCaptureDeviceIds[i].c_str()));
    Button(ID_CAPTURE_REFRESH, W(L"刷新设备", L"刷新设备喵"), 724, 143, 128, 2);
    Label(W(L"输出显示器", L"输出显示器喵"), 26, 191, 108, 2);
    int captureMonitorIndex = -1;
    for (size_t i = 0; i < monitors.size(); ++i) if (MatchesMonitor(monitors[i], settings.captureMonitorId)) { captureMonitorIndex = static_cast<int>(i); break; }
    if (captureMonitorIndex < 0 && settings.captureMonitorId.empty() && !monitors.empty()) {
        captureMonitorIndex = 0;
        for (size_t i = 0; i < monitors.size(); ++i) if (!(monitors[i].info.dwFlags & MONITORINFOF_PRIMARY)) { captureMonitorIndex = static_cast<int>(i); break; }
    }
    Combo(ID_CAPTURE_MONITOR, monitorNames, captureMonitorIndex, 140, 184, 350, 2);
    for (size_t i = 0; i < monitors.size(); ++i) SendMessageW(Item(ID_CAPTURE_MONITOR), CB_SETITEMDATA, i, reinterpret_cast<LPARAM>(monitors[i].stableId.c_str()));
    Button(ID_CAPTURE_TOGGLE, W(L"启动全屏", L"启动全屏喵"), 507, 183, 166, 2);
    Button(ID_CAPTURE_STOP, W(L"停止采集", L"停止采集喵"), 690, 183, 162, 2);
    Label(W(L"采集规格", L"采集规格喵"), 26, 225, 108, 2);
    Check(ID_CAPTURE_ADVANCED, W(L"精确规格", L"精确规格喵"), captureAdvanced, 562, 214, 124, 2);
    TrackControls(group, UiCapture);
    group = controls.size(); Combo(ID_CAPTURE_SPEC, presetLabels, presetIndex, 140, 214, 405, 2);
    for (size_t i = 0; i < uiCapturePresetModeKeys.size(); ++i) SendMessageW(Item(ID_CAPTURE_SPEC), CB_SETITEMDATA, i, reinterpret_cast<LPARAM>(uiCapturePresetModeKeys[i].c_str()));
    TrackControls(group, UiCaptureSpecBasic);
    group = controls.size(); Combo(ID_CAPTURE_EXACT_MODE, exactLabels, exactIndex, 140, 214, 405, 2);
    for (size_t i = 0; i < uiCaptureExactModeKeys.size(); ++i) SendMessageW(Item(ID_CAPTURE_EXACT_MODE), CB_SETITEMDATA, i, reinterpret_cast<LPARAM>(uiCaptureExactModeKeys[i].c_str()));
    TrackControls(group, UiCaptureSpecAdvanced);
    group = controls.size();
    Check(ID_CAPTURE_PRESENT_MODE, W(L"极限低延迟", L"极限低延迟喵"), settings.capturePresentMode == 1, 690, 214, 162, 2);
    TrackControls(group, UiCapture);

    // 视频融合器从另一输入取得手机画面；本路只输出可色键抠除的纯色和光标。
    group = controls.size();
    Add(L"STATIC", L"", SS_LEFT, ID_FUSION_STATE, 392, 104, 460, 39, 2);
    Label(W(L"输出显示器", L"输出显示器喵"), 26, 150, 108, 2);
    int fusionMonitorIndex = -1;
    for (size_t i = 0; i < monitors.size(); ++i)
        if (MatchesMonitor(monitors[i], settings.fusionMonitorId)) { fusionMonitorIndex = static_cast<int>(i); break; }
    if (fusionMonitorIndex < 0 && settings.fusionMonitorId.empty() && !monitors.empty()) {
        fusionMonitorIndex = 0;
        for (size_t i = 0; i < monitors.size(); ++i)
            if (!(monitors[i].info.dwFlags & MONITORINFOF_PRIMARY)) { fusionMonitorIndex = static_cast<int>(i); break; }
    }
    Combo(ID_FUSION_MONITOR, monitorNames, fusionMonitorIndex, 140, 144, 350, 2);
    for (size_t i = 0; i < monitors.size(); ++i)
        SendMessageW(Item(ID_FUSION_MONITOR), CB_SETITEMDATA, i,
            reinterpret_cast<LPARAM>(monitors[i].stableId.c_str()));
    Button(ID_FUSION_TOGGLE, W(L"启动融合输出", L"启动融合输出喵"), 507, 143, 345, 2);
    Label(W(L"背景色", L"背景色喵"), 26, 191, 108, 2);
    Edit(ID_FUSION_COLOR, FusionColorText(settings.fusionBackgroundColor), 140, 184, 132, 2);
    Button(ID_FUSION_GREEN, W(L"绿幕", L"绿幕喵"), 288, 183, 76, 2);
    Button(ID_FUSION_BLUE, W(L"蓝幕", L"蓝幕喵"), 374, 183, 76, 2);
    Button(ID_FUSION_BLACK, W(L"黑场", L"黑场喵"), 460, 183, 76, 2);
    Button(ID_FUSION_PICK, W(L"自定义…", L"自定义喵…"), 546, 183, 126, 2);
    Check(ID_FUSION_PRESENT_MODE, W(L"极限低延迟", L"极限低延迟喵"), settings.fusionPresentMode == 1, 690, 184, 162, 2);
    Add(L"STATIC", W(L"电脑只输出纯色背景和光标，不打开采集卡。将该显示器接入视频融合器，并在融合器中抠除背景色。底部 HUD 会进入融合画面；HUD、校准和退出键可在设置中自定义，也可使用右键或托盘。",
        L"电脑只输出纯色背景和光标，不打开采集卡喵。把显示器接入融合器并抠除背景色。底部 HUD 会进入融合画面；HUD、校准和退出键可在设置中自定义喵。"),
        SS_LEFT, 0, 26, 229, 826, 72, 2);
    Button(ID_FUSION_RANGE, W(L"拖拽校准光标范围", L"拖拽校准光标范围喵"), 26, 321, 300, 2);
    Add(L"STATIC", W(L"在融合器最终画面中对照四角和中心调整。", L"请对照融合器最终画面的四角和中心调整喵。"),
        SS_LEFT, 0, 346, 327, 506, 28, 2);
    TrackControls(group, UiFusion);

    group = controls.size();
    Label(W(L"手机画面范围", L"手机画面范围喵"), 26, 256, 194, 2);
    Combo(ID_MAPPING_STYLE, {W(L"完整手机画面（推荐）", L"完整手机画面（推荐）喵"), W(L"黑边、裁剪或旋转（高级）", L"黑边、裁剪或旋转（高级）喵")}, settings.simpleMapping ? 0 : 1, 230, 249, 622, 2);
    TrackControls(group, UiMapping);
    group = controls.size();
    Add(L"STATIC", L"", SS_LEFT, ID_MAPPING_HELP, 26, 296, 822, 78, 2);
    Button(ID_CHECK_ALIGNMENT, W(L"显示四角与中心检查点", L"显示四角与中心检查点喵"), 26, 373, 285, 2); TrackControls(group, UiGeometrySimple);
    group = controls.size();
    Button(ID_CAPTURE_RANGE, W(L"拖拽校准光标范围", L"拖拽校准光标范围喵"), 330, 373, 270, 2);
    TrackControls(group, UiCaptureGeometrySimple);
    group = controls.size();
    Label(W(L"完整采集帧 宽 / 高", L"完整采集帧 宽 / 高喵"), 26, 299, 275, 2);
    Edit(ID_FRAME_W, Num(settings.geometry.frameWidth), 305, 292, 120, 2); Edit(ID_FRAME_H, Num(settings.geometry.frameHeight), 442, 292, 120, 2);
    Label(W(L"例如 HDMI 1920 × 1080", L"例如 HDMI 1920 × 1080 喵"), 580, 298, 272, 2);
    TrackControls(group, UiGeometryFrameInput);
    group = controls.size();
    Add(L"STATIC", W(L"输入分辨率由采集卡自动读取；下方百分比以实际输入帧为基准。", L"输入分辨率由采集卡自动读取，下方百分比按实际帧填写喵。"),
        SS_LEFT, 0, 26, 298, 826, 24, 2);
    TrackControls(group, UiCaptureGeometryFrame);
    group = controls.size();
    Label(W(L"帧内手机 %：左 / 上 / 宽 / 高", L"帧内手机 %：左 / 上 / 宽 / 高喵"), 26, 338, 275, 2);
    const auto& r = settings.geometry.content; const double roi[] = {r.x, r.y, r.w, r.h};
    for (int i = 0; i < 4; ++i) Edit(ID_CONTENT_X + i, Num(roi[i] * 100), 305 + i * 137, 331, 120, 2);
    Label(W(L"保留整帧 %：左 / 上 / 宽 / 高", L"保留整帧 %：左 / 上 / 宽 / 高喵"), 26, 377, 275, 2);
    const auto& c = settings.geometry.crop; const double crop[] = {c.x, c.y, c.w, c.h};
    for (int i = 0; i < 4; ++i) Edit(ID_CROP_X + i, Num(crop[i] * 100), 305 + i * 137, 370, 120, 2); TrackControls(group, UiGeometryExtra);
    Label(W(L"额外顺时针旋转", L"额外顺时针旋转喵"), 26, 419, 194, 2);
    Combo(ID_ROTATE, {L"0°", L"90°", L"180°", L"270°"}, settings.geometry.rotation / 90, 230, 412, 105, 2);
    Check(ID_MIRROR_X, W(L"水平镜像", L"水平镜像喵"), settings.geometry.mirrorX, 355, 412, 150, 2);
    Check(ID_MIRROR_Y, W(L"垂直镜像", L"垂直镜像喵"), settings.geometry.mirrorY, 510, 412, 150, 2);
    group = controls.size(); Check(ID_STRETCH, W(L"拉伸铺满", L"拉伸铺满喵"), settings.geometry.stretch, 670, 412, 182, 2); TrackControls(group, UiGeometryExtra);
    Check(ID_ALIGNMENT_CHECK, W(L"我已在手机画面检查四角、中心和移动方向", L"我已检查四角、中心和移动方向喵"), settings.setupComplete, 26, 459, 826, 2);
    group = controls.size();
    Add(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, ID_CAPTURE_METRICS, 26, 489, 646, 24, 2);
    Button(ID_CAPTURE_DIAGNOSTICS, W(L"采集诊断详情", L"采集诊断详情喵"), 690, 481, 162, 2);
    TrackControls(group, UiCapture);

    CalibrationControls(3);

    Label(W(L"指针外观", L"指针外观喵"), 26, 109, 120, 4);
    Combo(ID_SHAPE, {W(L"箭头", L"箭头喵"), W(L"圆点", L"圆点喵"), W(L"十字", L"十字喵"),
        W(L"圆环", L"圆环喵"), W(L"涟漪", L"涟漪喵"), W(L"锁定框", L"锁定框喵"),
        W(L"轨迹", L"轨迹喵"), W(L"自定义图片", L"自定义图片喵")}, settings.shape, 160, 102, 165, 4);
    Label(W(L"大小", L"大小喵"), 344, 108, 50, 4); Edit(ID_SIZE, std::to_wstring(settings.size), 399, 102, 70, 4);
    Label(W(L"像素", L"像素喵"), 477, 108, 44, 4); Button(ID_COLOR, W(L"选择颜色", L"选择颜色喵"), 528, 101, 140, 4);
    Button(ID_CUSTOM_CURSOR, W(L"选择 / 标定图片…", L"选择 / 标定图片喵…"), 682, 101, 170, 4);
    Label(W(L"外观立即预览；自定义图片可调整热点位置。", L"外观立即预览，自定义图片还能调整热点喵。"), 160, 141, 692, 4);
    Label(W(L"显示延迟（ms）", L"显示延迟（ms）喵"), 26, 196, 190, 4); Edit(ID_DELAY, std::to_wstring(settings.delayMs), 230, 189, 120, 4);
    Label(W(L"断流后隐藏（ms）", L"断流后隐藏（ms）喵"), 436, 196, 198, 4); Edit(ID_TIMEOUT, std::to_wstring(settings.timeoutMs), 662, 189, 190, 4);
    Label(W(L"采集预览明显滞后时，可适当增加显示延迟。", L"采集预览有明显延迟时，可以稍加显示延迟喵。"), 230, 228, 622, 4);
    Label(W(L"快捷键", L"快捷键喵"), 26, 277, 120, 4);
    Button(ID_HOTKEYS, W(L"自定义快捷键…", L"自定义快捷键喵…"), 160, 270, 196, 4);
    Label(W(L"可分别设置暂停、HUD、范围校准和退出全屏。", L"可分别设置暂停、HUD、范围校准和退出全屏喵。"), 376, 276, 476, 4);
    Check(ID_RESUME_LAUNCH, hostWindow ? W(L"随上位机启动连接上次设备", L"随上位机连接上次设备喵") : W(L"启动时连接上次设备", L"启动时连接上次设备喵"), settings.resumeOnLaunch, 26, 314, 390, 4);
    Check(ID_ANIME, L"二次元模式（即时切换）", settings.anime, 436, 314, 416, 4);
    Add(L"BUTTON", W(L"设备与画面场景", L"设备与画面场景喵"), BS_GROUPBOX, 0, 26, 365, 826, 97, 4);
    Label(W(L"为不同设备、采集卡或预览布局保留一份配置。", L"给不同设备和预览布局保留配置喵。"), 46, 393, 470, 4, 44);
    Button(ID_SAVE_SCENE, W(L"保存场景…", L"保存场景喵…"), 534, 399, 143, 4); Button(ID_LOAD_SCENE, W(L"打开场景…", L"打开场景喵…"), 690, 399, 143, 4);

#if QINEX_WITH_ANDROID_TOOLS
    BuildAndroidTools();
#endif
    Add(L"STATIC", L"", SS_LEFT, ID_STATUS, 26, 519, 824, 43, -1);
    Button(ID_BACK, W(L"上一步", L"上一步喵"), 26, 575, 111, -1);
    Add(L"STATIC", L"", SS_LEFT, ID_SAVE_STATE, 154, 581, 268, 26, -1);
    Button(ID_APPLY, W(L"应用本页", L"应用本页喵"), 439, 575, 137, -1); Button(ID_SAVE, W(L"保存", L"保存喵"), 590, 575, 113, -1);
    Add(L"BUTTON", L"", WS_TABSTOP | BS_DEFPUSHBUTTON, ID_NEXT, 718, 575, 134, 28, -1);
    uiBaseline = CaptureUiValues(); RestoreDraft();
    uiDisplayedSource = std::max(0, Selected(ID_SOURCE)); uiSourceAddresses[uiDisplayedSource] = Text(Item(ID_IP));
    uiBuilding = false; SelectPage(page); UpdateCalibrationText(); UpdatePresentation(); statusAt = 0;
    RefreshUiTheme();
    if (redrawAfterBuild) {
        SendMessageW(main, WM_SETREDRAW, TRUE, 0);
        uiTransition.Play(180);
        RedrawWindow(main, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }
}
