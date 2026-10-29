#pragma once
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include "pointer_core.h"
#include "udp_receiver.h"
#include "demo_sender.h"
#include "capture_output.h"
#include "capture_presenter.h"
#include "settings.h"
#include "hotkey_utils.h"
#include "profile_store.h"
#if QINEX_WITH_ANDROID_TOOLS
#include "android_tools_service.h"
#endif
#include "ui_motion.h"

namespace qp = qpointer;
inline const wchar_t* W(const wchar_t* normal, const wchar_t* cute) { return Messages::Pick(normal, cute); }
inline std::wstring Wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring r(n, 0); if (n) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), r.data(), n); return r;
}
inline std::string Utf8(const std::wstring& s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string r(n, 0); if (n) WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), r.data(), n, nullptr, nullptr); return r;
}
inline std::wstring Text(HWND h) { int n = GetWindowTextLengthW(h); std::wstring s(n + 1, 0); GetWindowTextW(h, s.data(), n + 1); s.resize(n); return s; }
inline std::wstring Num(double n) { wchar_t b[64]; swprintf_s(b, L"%.6g", n); return b; }
inline qp::Rect FromRect(RECT r) { return {double(r.left), double(r.top), double(r.right - r.left), double(r.bottom - r.top)}; }
inline uint64_t Age(uint64_t now, uint64_t then) { return now >= then ? now - then : 0; }
// Windows 设备接口路径不区分大小写。驱动或不同采集 API 可能只改变字符大小写，
// 不能因此把同一块采集卡误判为离线、新设备或需要重启采集。
inline bool CaptureDeviceIdsEqual(const std::wstring& left, const std::wstring& right) {
    if (left.empty() || right.empty()) return left.empty() && right.empty();
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}
inline bool ClientRectOnScreen(HWND h, qp::Rect& r) {
    RECT c{}; POINT p{};
    if (!IsWindow(h) || !GetClientRect(h, &c) || !ClientToScreen(h, &p) || c.right < 2 || c.bottom < 2) return false;
    r = {double(p.x), double(p.y), double(c.right), double(c.bottom)}; return true;
}
inline HBITMAP CaptureRect(qp::Rect r, HWND printWindow = nullptr) {
    int w = static_cast<int>(std::lround(r.w)), h = static_cast<int>(std::lround(r.h));
    if (w < 2 || h < 2 || int64_t(w) * h > 20000000) return nullptr;
    HDC dc = GetDC(nullptr);
    HDC mem = dc ? CreateCompatibleDC(dc) : nullptr;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = w;
    info.bmiHeader.biHeight = -h;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    // DIB section 不依赖当前桌面色深，经历采集交换链创建/销毁后也能稳定截图。
    HBITMAP bitmap = mem ? CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0) : nullptr;
    HGDIOBJ old = bitmap ? SelectObject(mem, bitmap) : nullptr;
    if (old == HGDI_ERROR) old = nullptr;
    BOOL ok = FALSE;
    if (old && printWindow) {
        // 测试截图对象均为本线程自有窗口。同步绘制可避开新窗口合成尚未完成时的空白帧。
        SendMessageW(printWindow, WM_PRINT, reinterpret_cast<WPARAM>(mem), PRF_CLIENT | PRF_NONCLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
        ok = TRUE;
    } else if (old) ok = BitBlt(mem, 0, 0, w, h, dc, static_cast<int>(std::lround(r.x)), static_cast<int>(std::lround(r.y)), SRCCOPY | CAPTUREBLT);
    // 部分显示驱动不支持 CAPTUREBLT，退回普通桌面拷贝。
    if (!ok && !printWindow) ok = BitBlt(mem, 0, 0, w, h, dc, static_cast<int>(std::lround(r.x)), static_cast<int>(std::lround(r.y)), SRCCOPY);
    if (!ok && !printWindow) {
        // 桌面复制暂时不可用时，优先让目标位置的窗口在内存中重绘；调用设有短超时，
        // 不让无响应的第三方预览窗口卡住校准。
        POINT center{static_cast<LONG>(std::lround(r.x + r.w / 2)),
            static_cast<LONG>(std::lround(r.y + r.h / 2))};
        HWND sourceWindow = GetAncestor(WindowFromPoint(center), GA_ROOT);
        RECT sourceClient{};
        POINT sourceOrigin{};
        if (sourceWindow && IsWindowVisible(sourceWindow) && GetClientRect(sourceWindow, &sourceClient) &&
            ClientToScreen(sourceWindow, &sourceOrigin)) {
            const int sourceWidth = sourceClient.right - sourceClient.left;
            const int sourceHeight = sourceClient.bottom - sourceClient.top;
            const int sourceX = static_cast<int>(std::lround(r.x)) - sourceOrigin.x;
            const int sourceY = static_cast<int>(std::lround(r.y)) - sourceOrigin.y;
            if (sourceWidth >= 2 && sourceHeight >= 2 && sourceX >= 0 && sourceY >= 0 &&
                sourceX + w <= sourceWidth && sourceY + h <= sourceHeight &&
                int64_t(sourceWidth) * sourceHeight <= 20000000) {
                HDC sourceMemory = CreateCompatibleDC(dc);
                BITMAPINFO sourceInfo = info;
                sourceInfo.bmiHeader.biWidth = sourceWidth;
                sourceInfo.bmiHeader.biHeight = -sourceHeight;
                void* sourcePixels = nullptr;
                HBITMAP sourceBitmap = sourceMemory ? CreateDIBSection(dc, &sourceInfo, DIB_RGB_COLORS,
                    &sourcePixels, nullptr, 0) : nullptr;
                HGDIOBJ sourceOld = sourceBitmap ? SelectObject(sourceMemory, sourceBitmap) : nullptr;
                if (sourceOld == HGDI_ERROR) sourceOld = nullptr;
                DWORD_PTR ignored = 0;
                if (sourceOld && SendMessageTimeoutW(sourceWindow, WM_PRINTCLIENT,
                        reinterpret_cast<WPARAM>(sourceMemory), PRF_CLIENT | PRF_ERASEBKGND | PRF_CHILDREN,
                        SMTO_ABORTIFHUNG | SMTO_BLOCK, 250, &ignored)) {
                    ok = BitBlt(mem, 0, 0, w, h, sourceMemory, sourceX, sourceY, SRCCOPY);
                }
                if (sourceOld) SelectObject(sourceMemory, sourceOld);
                if (sourceBitmap) DeleteObject(sourceBitmap);
                if (sourceMemory) DeleteDC(sourceMemory);
            }
        }
    }
    if (!ok && !printWindow) {
        // DXGI flip 链创建/销毁后，少数驱动会暂时拒绝虚拟桌面 DC 的 BitBlt。
        // 目标完整落在单屏时改用该显示设备的新 DC，避免让校准流程失效。
        POINT center{static_cast<LONG>(std::lround(r.x + r.w / 2)),
            static_cast<LONG>(std::lround(r.y + r.h / 2))};
        MONITORINFOEXW monitor{sizeof(monitor)};
        if (GetMonitorInfoW(MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST), &monitor) &&
            r.x >= monitor.rcMonitor.left && r.y >= monitor.rcMonitor.top &&
            r.x + r.w <= monitor.rcMonitor.right && r.y + r.h <= monitor.rcMonitor.bottom) {
            HDC monitorDc = CreateDCW(L"DISPLAY", monitor.szDevice, nullptr, nullptr);
            if (monitorDc) {
                ok = BitBlt(mem, 0, 0, w, h, monitorDc,
                    static_cast<int>(std::lround(r.x)) - monitor.rcMonitor.left,
                    static_cast<int>(std::lround(r.y)) - monitor.rcMonitor.top, SRCCOPY);
                DeleteDC(monitorDc);
            }
        }
    }
    if (old) SelectObject(mem, old);
    if (mem) DeleteDC(mem);
    if (dc) ReleaseDC(nullptr, dc);
    if (!ok) { DeleteObject(bitmap); return nullptr; } return bitmap;
}
inline bool SavePng(HBITMAP bitmap, const std::wstring& path) {
    UINT count = 0, bytes = 0; Gdiplus::GetImageEncodersSize(&count, &bytes);
    std::vector<BYTE> storage(bytes); auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(storage.data());
    if (!bytes || Gdiplus::GetImageEncoders(count, bytes, codecs) != Gdiplus::Ok) return false;
    for (UINT i = 0; i < count; ++i) if (wcscmp(codecs[i].MimeType, L"image/png") == 0) {
        Gdiplus::Bitmap image(bitmap, nullptr); return image.Save(path.c_str(), &codecs[i].Clsid, nullptr) == Gdiplus::Ok;
    }
    return false;
}

enum {
    ID_TAB = 100, ID_START, ID_STOP, ID_APPLY, ID_SAVE, ID_GUIDE,
    ID_SOURCE = 110, ID_IP, ID_PORT, ID_SIMULATOR, ID_SHAPE, ID_SIZE, ID_COLOR, ID_DELAY, ID_TIMEOUT, ID_ANIME,
    ID_CUSTOM_CURSOR, ID_HOTKEYS,
    ID_MODE = 130, ID_MONITOR, ID_WINDOW, ID_REFRESH, ID_REGION, ID_ENTIRE, ID_FOREGROUND,
    ID_X = 140, ID_Y, ID_WIDTH, ID_HEIGHT, ID_FRAME_W, ID_FRAME_H,
    ID_CONTENT_X = 150, ID_CONTENT_Y, ID_CONTENT_W, ID_CONTENT_H, ID_CROP_X, ID_CROP_Y, ID_CROP_W, ID_CROP_H,
    ID_ROTATE = 160, ID_MIRROR_X, ID_MIRROR_Y, ID_STRETCH,
    ID_CAPTURE = 170, ID_FIT, ID_CLEAR, ID_RESET, ID_CAL_STATUS, ID_CAPTURE_DELAY, ID_VERIFY,
    ID_STATUS = 190,
    ID_NAV_FIRST = 700, ID_UI_THEME = 710, ID_UI_SUBTITLE
};

struct MonitorItem {
    HMONITOR handle{};
    MONITORINFOEXW info{};
    // 优先使用 DisplayConfig 提供的显示器设备接口路径；GDI 名称只作旧配置兼容和兜底。
    std::wstring stableId;
    std::wstring friendlyName;
    double refreshHz = 0;
};
inline bool MatchesMonitor(const MonitorItem& monitor, const std::wstring& savedId) {
    if (savedId.empty()) return false;
    auto same = [&](const wchar_t* value) {
        return value && *value && CompareStringOrdinal(savedId.c_str(), -1, value, -1, TRUE) == CSTR_EQUAL;
    };
    return same(monitor.stableId.c_str()) || same(monitor.info.szDevice);
}
struct WindowItem { HWND hwnd; std::wstring title; };
struct Control { HWND hwnd; int page; };
struct Sample { qp::Point input, observed; qp::Geometry geometry; qp::Point screen; };

class Application {
public:
    HINSTANCE instance{};
    HWND hostWindow{}; // 嵌入 Link 时统一托盘与退出，独立运行时为空。
    HWND main{}, overlay{}, region{}, snapshot{}, targetWindow{}, captureWindow{};
    HICON applicationIcon{}, applicationSmallIcon{};
    HFONT font{};
    HFONT uiSmallFont{}, uiStrongFont{};
    qp::UiTransition uiTransition;
    Settings settings;
    ProfileStore profiles;
    std::wstring storageDirectory;
    bool paused = false, demoActive = false, dirty = false;
    bool shuttingDown = false, trayInstalled = false;
    bool trayVersion4 = false, trayHintShown = false;
    std::wstring trayTooltip;
    qp::HotkeyBinding registeredPauseHotkey;
    qp::UdpReceiver demoReceiver;
    qp::PointerState demoState;
    uint64_t demoDue = 0;
    uint64_t demoAccepted = 0;
    uint16_t demoPort = 0;
    HWND demoWindow{};
    std::wstring runtimeStatus, deviceStatus, displayStatus;
    bool lastRenderOk = true;
    HWND alignmentCheck{};
    qp::Rect regionOriginal;
    std::optional<Settings> regionBefore;
    HWND regionBeforeWindow{};
    bool restoringScene = false;
    qp::UdpReceiver receiver;
    qp::CaptureOutput captureOutput;
    qp::CapturePresenter capturePresenter;
    std::shared_ptr<const qp::CaptureFrame> presentedCaptureFrame;
    int captureKnownWidth = 0, captureKnownHeight = 0;
    std::vector<qp::CaptureDevice> captureDevices;
    std::vector<qp::CaptureNativeMode> captureModes;
    std::wstring captureModesDeviceId, captureModesError;
    bool captureFullscreenRequested = false;
    bool captureExitHotkeyReady = false;
    bool captureFallbackHotkeyReady = false;
    bool captureRangeHotkeyReady = false;
    bool outputHudHotkeyReady = false;
    bool outputHudVisible = true;
    uint64_t outputHudToggleAt = 0;
    // Link 的 Raw 后备与系统 RegisterHotKey 可能对同一物理沿各投递一次。
    // 在最终动作入口统一合并，避免暂停/HUD 等切换动作立刻切回原状态。
    std::array<uint64_t, 6> hotkeyActionAt{};
    std::shared_ptr<const std::wstring> outputHudStatusText;
    std::shared_ptr<const std::wstring> outputHudShortcutText;
    std::shared_ptr<const qp::CustomCursorImage> customCursorImage;
    double customCursorHotspotX = .5, customCursorHotspotY = .5;
    bool captureRangePending = false;
    bool captureRangeEditing = false;
    int captureRangeDrag = 0;
    POINT captureRangeDragStart{};
    qp::Rect captureRangeRect;
    qp::Rect captureRangeDragOriginal;
    qp::Geometry captureRangeGeometry;
    uint64_t captureHintUntil = 0, captureCreateRetryAt = 0;
    std::wstring captureRuntimeNotice;
    qp::PointerState state, displayed;
    std::deque<qp::PointerState> delayed;
    std::vector<Control> controls;
    std::vector<MonitorItem> monitors;
    std::vector<WindowItem> windows;
    std::vector<Sample> samples;
    size_t fitSampleCount = 0;
    qp::Point stableAnchor;
    uint64_t accepted = 0, rejected = 0, lastMove = 0, statusAt = 0;
    bool running = false, captureWaiting = false;
    uint64_t captureDue = 0;
    std::wstring notice, sender;
    double scale = 1;
    int page = 0;
    HBITMAP captured{};
    qp::Geometry snapshotGeometry;
    qp::Point snapshotInput;
    std::optional<qp::Point> marked;
    qp::Rect imageRect;
    qp::LoopbackDemoSender demoSender;
    HDC cursorDc{};
    HBITMAP cursorBitmap{};
    HGDIOBJ cursorOld{};
    int cacheShape = -1, cacheSize = 0;
    COLORREF cacheColor{};
    bool cacheDown = false;
    const qp::CustomCursorImage* cacheCustomCursor = nullptr;
    double cacheCustomHotspotX = -1, cacheCustomHotspotY = -1;
    POINT lastPixel{-1000000, -1000000};
    bool Has(int id) const { return GetDlgItem(main, id) != nullptr; }
    HWND Item(int id) const { return GetDlgItem(main, id); }
    int Px(double x) const { return static_cast<int>(std::lround(x * scale)); }
    void Error(const std::wstring& text) { MessageBoxW(main, text.c_str(), L"QInEX Pointer", MB_OK | MB_ICONWARNING); }
    int Selected(int id) const { return static_cast<int>(SendMessageW(Item(id), CB_GETCURSEL, 0, 0)); }
    bool Checked(int id) const { return SendMessageW(Item(id), BM_GETCHECK, 0, 0) == BST_CHECKED; }
    void Set(int id, const std::wstring& s) {
        HWND control = Item(id);
        if (control && Text(control) != s) SetWindowTextW(control, s.c_str());
    }
    void Enable(int id, bool enabled) {
        HWND control = Item(id);
        if (control && (IsWindowEnabled(control) != FALSE) != enabled) EnableWindow(control, enabled ? TRUE : FALSE);
    }
    HWND Add(const wchar_t* cls, const wchar_t* text, DWORD style, int id, int x, int y, int w, int h, int p) {
        const bool sidebar = (id >= ID_NAV_FIRST && id < ID_NAV_FIRST + 6) || id == ID_UI_THEME;
        if (!sidebar) {
            x += 184;
            if (id == ID_PAGE_TITLE) y = 34;
            else if (id == ID_UI_SUBTITLE) y = 80;
            else if (id == ID_GUIDE) y = 38;
            else if (id == ID_STATUS) y = 580;
            else if (p == -1) y += 75;
            else y += 40;
        }
        const bool edit = wcscmp(cls, L"EDIT") == 0;
        const bool combo = wcscmp(cls, L"COMBOBOX") == 0;
        if (edit) { style |= WS_BORDER; h = std::max(h, 32); }
        if (combo) style |= CBS_OWNERDRAWFIXED | CBS_HASSTRINGS;
        if (wcscmp(cls, L"BUTTON") == 0) {
            const DWORD kind = style & BS_TYPEMASK;
            if (kind == BS_PUSHBUTTON || kind == BS_DEFPUSHBUTTON) h = std::max(h, 34);
            if (kind == BS_AUTOCHECKBOX) h = std::max(h, 28);
        }
        HWND child = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
            Px(x), Px(y), Px(w), Px(h), main, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        if (combo) {
            SendMessageW(child, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), Px(28));
            SendMessageW(child, CB_SETITEMHEIGHT, 0, Px(32));
        }
        ThemeControl(child);
        controls.push_back({child, p}); return child;
    }
    void Label(const wchar_t* s, int x, int y, int w, int p, int h = 24) { Add(L"STATIC", s, SS_LEFT, 0, x, y, w, h, p); }
    void Edit(int id, const std::wstring& s, int x, int y, int w, int p) { Add(L"EDIT", s.c_str(), WS_TABSTOP | ES_AUTOHSCROLL, id, x, y, w, 25, p); }
    void Button(int id, const wchar_t* s, int x, int y, int w, int p) { Add(L"BUTTON", s, WS_TABSTOP | BS_PUSHBUTTON, id, x, y, w, 28, p); }
    void Check(int id, const wchar_t* s, bool checked, int x, int y, int w, int p) {
        HWND h = Add(L"BUTTON", s, WS_TABSTOP | BS_AUTOCHECKBOX, id, x, y, w, 26, p); SendMessageW(h, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    void Combo(int id, const std::vector<std::wstring>& names, int selected, int x, int y, int w, int p) {
        HWND h = Add(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, id, x, y, w, 210, p);
        for (auto& name : names) SendMessageW(h, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
        if (!names.empty() && selected >= 0) SendMessageW(h, CB_SETCURSEL, std::clamp(selected, 0, static_cast<int>(names.size()) - 1), 0);
    }
    void Inventory();
    void BuildControls();
    void ThemeControl(HWND control);
    void RefreshUiTheme();
    bool UiMotionAllowed() const;
    void PaintChrome(HDC dc);
    void PaintThemedControl(HWND control, HDC dc);
    bool DrawThemedItem(const DRAWITEMSTRUCT& item);
    LRESULT ThemeControlColor(HDC dc, HWND control, UINT message);
    bool ReadControls(Settings& candidate);
    void SelectPage(int p);
    void Apply(bool save);
    void MarkDirty() { dirty = true; statusAt = 0; }
    void ApplyAppearance();
    void UpdateStatus();
    void TogglePause();
    void BindSource();
    void EndDemo();
    void UpdateTray(bool force = false);
    void HideToTray();
    void RegisterPauseHotkey();
    void RefreshOutputHotkeys();
    void RefreshHotkeys();
    void PublishHotkeys();
    bool EditHotkeys();
    bool EditCustomCursor();
    bool EditCursorDraft(const std::wstring& title, std::wstring& source, double& hotspotX, double& hotspotY);
    void ReloadCustomCursor();
    void ShowHelp();
    void SaveScene();
    void LoadScene();
    void CheckAlignment();
    void RequestExit();
    bool CanExit();
    void ShowTrayMenu(POINT position);
    void ReturnToHost();
    void AdoptSettings(const Settings& next);
    bool Start();
    void Stop();
    bool StartCaptureOutput();
    bool StartFusionOutput();
    void StopCaptureOutput();
    void ExitCaptureFullscreen(bool restoreControl = true);
    int ReleaseLinkInput();
    std::wstring CaptureExitHint() const;
    void UpdateOutputHud(uint64_t now);
    void ToggleOutputHud();
    void ShowControlWindow();
    void FocusCaptureFullscreen();
    void BeginCaptureRangeAdjustment();
    void FinishCaptureRangeAdjustment(bool apply);
    int CaptureRangeHitTest(POINT point) const;
    void UpdateCaptureRangeDrag(POINT point);
    void PaintCaptureRange(HDC dc);
    void PositionCaptureOutput();
    void PaintOutputHud(HDC dc);
    void PaintCaptureOutput(HDC dc);
    HBITMAP SnapshotCaptureOutput();
    void AdoptLatestCaptureFrame();
    void ProcessPointerDatagrams();
    void SubmitCapturePresentation();
    bool Geometry(qp::Geometry& g, bool requireVisible = false);
    void Render();
    void Tick();
    void BeginRegion();
    void FinishRegion(bool commit);
    void BeginCapture();
    void FinishCapture();
    void PaintSnapshot(HDC dc);
    void AcceptSample();
    void Fit();
    void Verify();
    void UpdateCalibrationText();
    void Simulate();
    void Shutdown();
static constexpr int kPageCount = QINEX_WITH_ANDROID_TOOLS ? 6 : 5;
#include "app_ui_members.inc"
#if QINEX_WITH_ANDROID_TOOLS
#include "android_tools_members.inc"
#endif
#include "calibration_members.inc"
};


extern Application* app;
LRESULT CALLBACK MainProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK RegionProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK SnapshotProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK DemoProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK AlignmentProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK CaptureOutputProc(HWND, UINT, WPARAM, LPARAM);
