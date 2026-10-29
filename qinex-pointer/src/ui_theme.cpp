#include "app.h"
#include "ui_theme.h"
#include "glass_theme.h"
#include "pointer_runtime.h"
#include <new>
#include <uxtheme.h>

namespace {
constexpr UINT_PTR kThemeSubclass = 1;
constexpr UINT_PTR kHoverTimer = 0x5148;
constexpr ULONGLONG kHoverDuration = 110;

struct ThemeControlState {
    Application* application{};
    bool hovered = false;
    double from = 0, target = 0;
    ULONGLONG started = 0;
    double pressFrom = 0, pressTarget = 0;
    ULONGLONG pressStarted = 0;
    double Press() const {
        const double t = std::min(1.0, static_cast<double>(GetTickCount64() - pressStarted) / 130.0);
        return pressFrom + (pressTarget - pressFrom) * (t * t * (3.0 - 2.0 * t));
    }
    double Hover() const {
        const double t = std::min(1.0, static_cast<double>(GetTickCount64() - started) / kHoverDuration);
        return from + (target - from) * (t * t * (3.0 - 2.0 * t));
    }
};

COLORREF Mix(COLORREF from, COLORREF to, double amount) {
    auto channel = [amount](int a, int b) { return static_cast<BYTE>(a + (b - a) * amount + .5); };
    return RGB(channel(GetRValue(from), GetRValue(to)), channel(GetGValue(from), GetGValue(to)),
        channel(GetBValue(from), GetBValue(to)));
}

Gdiplus::Color Color(COLORREF color) {
    return {255, GetRValue(color), GetGValue(color), GetBValue(color)};
}

void Fill(HDC dc, const RECT& rect, COLORREF color) {
    SetDCBrushColor(dc, color);
    FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

void Round(HDC dc, RECT rect, int radius, COLORREF fill, COLORREF border) {
    qp::glass::Surface(dc, rect, float(radius), fill, border);
}

void TextLine(HDC dc, const std::wstring& text, RECT rect, HFONT font, COLORREF color,
              UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS) {
    const auto previous = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect, flags | DT_NOPREFIX);
    SelectObject(dc, previous);
}

bool ClassIs(HWND window, const wchar_t* expected) {
    wchar_t name[40]{};
    GetClassNameW(window, name, static_cast<int>(std::size(name)));
    return _wcsicmp(name, expected) == 0;
}

bool PrimaryButton(int id) {
    return id == Application::ID_NEXT || id == Application::ID_HOME_PRIMARY ||
        id == ID_START || id == Application::ID_CAPTURE_TOGGLE ||
        id == Application::ID_FUSION_TOGGLE || id == ID_FIT;
}

LRESULT CALLBACK ThemeControlProc(HWND window, UINT message, WPARAM wp, LPARAM lp,
                                 UINT_PTR, DWORD_PTR reference) {
    auto* state = reinterpret_cast<ThemeControlState*>(reference);
    auto& application = *state->application;
    const bool edit = ClassIs(window, L"Edit");
    const bool button = ClassIs(window, L"Button");
    const bool combo = ClassIs(window, L"ComboBox");
    const bool label = ClassIs(window, L"Static");
    if (message == WM_NCDESTROY) {
        KillTimer(window, kHoverTimer);
        RemoveWindowSubclass(window, ThemeControlProc, kThemeSubclass);
        const LRESULT result = DefSubclassProc(window, message, wp, lp);
        delete state;
        return result;
    }
    auto repaint = [&] {
        if (edit) RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME);
        else InvalidateRect(window, nullptr, FALSE);
    };
    if (message == BM_SETSTATE && button) {
        state->pressFrom = state->Press();
        state->pressTarget = wp ? 1.0 : 0.0;
        state->pressStarted = GetTickCount64();
        if (!application.UiMotionAllowed() || !SetTimer(window, kHoverTimer, 16, nullptr))
            state->pressFrom = state->pressTarget;
        repaint();
    }
    if ((message == WM_MOUSEMOVE && !state->hovered) || message == WM_MOUSELEAVE) {
        state->from = state->Hover();
        state->hovered = message == WM_MOUSEMOVE;
        state->target = state->hovered && IsWindowEnabled(window) ? 1.0 : 0.0;
        state->started = GetTickCount64();
        if (state->hovered) {
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0};
            TrackMouseEvent(&track);
        }
        if (state->from != state->target && application.UiMotionAllowed() && IsWindowVisible(window)) {
            if (!SetTimer(window, kHoverTimer, 16, nullptr)) state->from = state->target;
        } else {
            state->from = state->target;
            KillTimer(window, kHoverTimer);
        }
        repaint();
    } else if (message == WM_TIMER && wp == kHoverTimer) {
        if (!IsWindowVisible(window) || !application.UiMotionAllowed() ||
                (GetTickCount64() - state->started >= kHoverDuration &&
                 GetTickCount64() - state->pressStarted >= 130)) {
            state->from = state->target;
            state->pressFrom = state->pressTarget;
            KillTimer(window, kHoverTimer);
        }
        repaint();
        return 0;
    } else if ((message == WM_SHOWWINDOW && !wp) || (message == WM_ENABLE && !wp)) {
        KillTimer(window, kHoverTimer);
        state->from = state->target = 0;
        state->pressFrom = state->pressTarget = 0;
        state->hovered = false;
    }
    // 保留原生按钮的键盘、复选框和下拉选择行为，只替换绘制。
    if ((button || combo || label) && (message == WM_PAINT || message == WM_PRINTCLIENT || message == WM_PRINT)) {
        if (button && (GetWindowLongPtrW(window, GWL_STYLE) & BS_TYPEMASK) == BS_OWNERDRAW)
            return DefSubclassProc(window, message, wp, lp);
        if (message == WM_PRINT && (!(lp & PRF_CLIENT) ||
                ((lp & PRF_CHECKVISIBLE) && !IsWindowVisible(window)))) return 0;
        PAINTSTRUCT paint{};
        HDC dc = message == WM_PAINT ? BeginPaint(window, &paint) : reinterpret_cast<HDC>(wp);
        application.PaintThemedControl(window, dc);
        if (message == WM_PAINT) EndPaint(window, &paint);
        return 0;
    }
    if ((button || combo || label) && message == WM_ERASEBKGND) return 1;
    if (edit && message == WM_NCCALCSIZE) {
        auto* rect = wp ? &reinterpret_cast<NCCALCSIZE_PARAMS*>(lp)->rgrc[0]
                        : reinterpret_cast<RECT*>(lp);
        InflateRect(rect, -application.Px(8), -application.Px(6));
        return 0;
    }
    if (edit && message == WM_NCPAINT) {
        HDC dc = GetWindowDC(window);
        if (dc) {
            RECT outer{}, client{}; GetWindowRect(window, &outer); GetClientRect(window, &client);
            POINT origin{}; ClientToScreen(window, &origin);
            OffsetRect(&client, origin.x - outer.left, origin.y - outer.top);
            ExcludeClipRect(dc, client.left, client.top, client.right, client.bottom);
            OffsetRect(&outer, -outer.left, -outer.top);
            const auto palette = qp::PointerPalette(application.settings.anime);
            Fill(dc, outer, palette.panel);
            Round(dc, outer, application.Px(7), palette.field,
                GetFocus() == window ? palette.accent : Mix(palette.border, palette.accent, state->Hover() * .65));
            ReleaseDC(window, dc);
        }
        return 0;
    }
    const LRESULT result = DefSubclassProc(window, message, wp, lp);
    switch (message) {
    case WM_SETFOCUS: case WM_KILLFOCUS: case WM_ENABLE:
    case BM_SETCHECK: case BM_SETSTATE: case WM_SETTEXT:
    case CB_SETCURSEL: case WM_LBUTTONDOWN: case WM_LBUTTONUP:
        if (edit) RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME);
        else InvalidateRect(window, nullptr, FALSE);
        break;
    }
    return result;
}
}  // namespace

void Application::ThemeControl(HWND control) {
    if (!control) return;
    if (ClassIs(control, L"Button") || ClassIs(control, L"ComboBox") ||
            ClassIs(control, L"Edit") || ClassIs(control, L"Static")) {
        SetWindowTheme(control, L"", L"");
        auto* controlState = new(std::nothrow) ThemeControlState{this};
        if (!controlState) return;
        if (!SetWindowSubclass(control, ThemeControlProc, kThemeSubclass, reinterpret_cast<DWORD_PTR>(controlState))) {
            delete controlState; return;
        }
        if (ClassIs(control, L"Edit"))
            SetWindowPos(control, nullptr, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
}

bool Application::UiMotionAllowed() const {
    // 动效仅在前台设置时短暂运行；采集、全屏、远程会话和系统关闭动画时直接呈现。
    if (!main || uiBuilding || shuttingDown || !IsWindowVisible(main) || IsIconic(main) ||
            GetForegroundWindow() != (hostWindow ? hostWindow : main) || captureWindow || captureOutput.Active() ||
            GetSystemMetrics(SM_REMOTESESSION)) return false;
    BOOL enabled = FALSE;
    return SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0) && enabled;
}

void Application::RefreshUiTheme() {
    qp::glass::light = settings.anime;
    if (hostWindow) PostMessageW(hostWindow, qp::desktop::kThemeChangedMessage, 0, 0);
    const BOOL dark = !settings.anime;
    DwmSetWindowAttribute(main, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    const auto palette = qp::PointerPalette(settings.anime);
    DwmSetWindowAttribute(main, 35 /* DWMWA_CAPTION_COLOR */, &palette.sidebar, sizeof(COLORREF));
    DwmSetWindowAttribute(main, 36 /* DWMWA_TEXT_COLOR */, &palette.text, sizeof(COLORREF));
    RedrawWindow(main, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

LRESULT Application::ThemeControlColor(HDC dc, HWND control, UINT message) {
    const auto palette = qp::PointerPalette(settings.anime);
    COLORREF background = palette.panel;
    const int id = GetDlgCtrlID(control);
    if (id == ID_PAGE_TITLE || id == ID_UI_SUBTITLE || id == ID_STATUS)
        background = palette.background;
    if (message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX || ClassIs(control, L"Edit"))
        background = palette.field;
    const bool prominent = id == ID_PAGE_TITLE || id == ID_HOME_STATE;
    SetTextColor(dc, !IsWindowEnabled(control) ? palette.disabled :
        prominent ? palette.text : palette.secondary);
    if (message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX || ClassIs(control, L"Edit"))
        SetTextColor(dc, IsWindowEnabled(control) ? palette.text : palette.disabled);
    SetBkColor(dc, background);
    SetDCBrushColor(dc, background);
    return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
}

void Application::PaintChrome(HDC dc) {
    const auto palette = qp::PointerPalette(settings.anime);
    RECT client{}; GetClientRect(main, &client);
    RECT background = client;
    if (hostWindow) {
        GetClientRect(hostWindow, &background);
        MapWindowPoints(hostWindow, main, reinterpret_cast<POINT*>(&background), 2);
    }
    qp::glass::Background(dc, background);
    RECT sidebar{Px(2), Px(2), Px(184), client.bottom-Px(2)};
    Round(dc, sidebar, Px(12), palette.sidebar, palette.border);
    TextLine(dc, W(L"光标设置", L"光标设置喵"), {Px(22),Px(28),Px(170),Px(62)}, headingFont,palette.text);
    TextLine(dc, L"POINTER / CONTROL", {Px(22),Px(76),Px(170),Px(102)}, uiSmallFont,palette.accent);
    RECT panel{Px(196), Px(128), Px(1052), page == 5 ? client.bottom-Px(18) : Px(566)};
    Round(dc, panel, Px(12), palette.panel, palette.border);
    if (page != 5) {
        RECT footer{Px(196),Px(632),Px(1052),Px(696)};
        Round(dc, footer,Px(12),palette.panel,palette.border);
    }
    TextLine(dc, hostWindow ? W(L"设备与光标在同一窗口", L"设备与光标都在这里喵") : W(L"关闭后继续后台运行", L"关闭后继续后台运行喵"),
        {Px(18),client.bottom-Px(56),Px(170),client.bottom-Px(30)},uiSmallFont,palette.secondary);
    TextLine(dc,hostWindow?L"QInEX / TOUCH LINK":L"QInEX / POINTER",{Px(22),client.bottom-Px(31),Px(170),client.bottom-Px(10)},uiSmallFont,palette.disabled);
}

void Application::PaintThemedControl(HWND control, HDC target) {
    RECT rect{}; GetClientRect(control, &rect);
    if (!target || rect.right <= 0 || rect.bottom <= 0) return;
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, rect.right, rect.bottom);
    if (!dc || !bitmap) {
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
        return;
    }
    const auto previous = SelectObject(dc, bitmap);
    const auto palette = qp::PointerPalette(settings.anime);
    const int id = GetDlgCtrlID(control);
    const bool nav = id >= ID_NAV_FIRST && id < ID_NAV_FIRST + kPageCount;
    const bool theme = id == ID_UI_THEME;
    const bool enabled = IsWindowEnabled(control) != FALSE;
    DWORD_PTR reference = 0;
    GetWindowSubclass(control, ThemeControlProc, kThemeSubclass, &reference);
    const double hover = enabled && reference ? reinterpret_cast<ThemeControlState*>(reference)->Hover() : 0;
    const bool focused = GetFocus() == control &&
        !(SendMessageW(control, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS);
    const DWORD kind = static_cast<DWORD>(GetWindowLongPtrW(control, GWL_STYLE)) & BS_TYPEMASK;
    const bool combo = ClassIs(control, L"ComboBox");
    const bool checkbox = !combo && kind == BS_AUTOCHECKBOX;
    const bool group = !combo && kind == BS_GROUPBOX;
    const double press = !combo && reference ? reinterpret_cast<ThemeControlState*>(reference)->Press() : 0;
    const bool selected = nav && page == id - ID_NAV_FIRST;
    const bool primary = !checkbox && !group && PrimaryButton(id);
    const COLORREF parent = nav || theme ? palette.sidebar :
        id == ID_GUIDE ? palette.background : palette.panel;
    POINT origin{}; MapWindowPoints(control, main, &origin, 1);
    const int saved=SaveDC(dc);
    SetViewportOrgEx(dc,-origin.x,-origin.y,nullptr);
    PaintChrome(dc);
    RestoreDC(dc,saved);
    const auto label = Text(control);
    if (ClassIs(control,L"Static")) {
        const auto labelFont=reinterpret_cast<HFONT>(SendMessageW(control,WM_GETFONT,0,0));
        RECT text=rect;
        const DWORD style=static_cast<DWORD>(GetWindowLongPtrW(control,GWL_STYLE));
        UINT align=(style & SS_TYPEMASK)==SS_CENTER ? DT_CENTER : DT_LEFT;
        TextLine(dc,label,text,labelFont?labelFont:font,enabled ?
            ((id==ID_PAGE_TITLE || id==ID_HOME_STATE)?palette.text:palette.secondary):palette.disabled,
            align | DT_WORDBREAK | DT_NOPREFIX);
        BitBlt(target,0,0,rect.right,rect.bottom,dc,0,0,SRCCOPY);
        SelectObject(dc,previous); DeleteObject(bitmap); DeleteDC(dc); return;
    }
    if (nav) {
        if (selected || hover > 0 || focused) {
            const COLORREF fill = selected ? Mix(palette.selected, palette.hover, hover * .25)
                : Mix(parent, palette.hover, hover);
            RECT surface = rect;
            const int inset = static_cast<int>(Px(2) * press + .5);
            InflateRect(&surface, -inset, -inset);
            Round(dc, surface, Px(8), fill, focused ? palette.accent :
                Mix(palette.border,palette.accent,std::min(1.0,(selected?.22:0.0)+hover*.4+press*.3)));
        }
        if (selected) {
            RECT mark{0, Px(16), Px(3), rect.bottom - Px(16)};
            Fill(dc, mark, palette.accent);
        }
        const int index = id - ID_NAV_FIRST;
        TextLine(dc, L"0" + std::to_wstring(index + 1), {Px(13), 0, Px(42), rect.bottom},
            uiSmallFont, selected ? palette.accent : palette.disabled);
        const auto split = label.find(L'\n');
        TextLine(dc, label.substr(0, split), {Px(47), Px(7), rect.right - Px(6), Px(30)},
            uiStrongFont, selected ? palette.accent : palette.text);
        if (split != std::wstring::npos)
            TextLine(dc, label.substr(split + 1), {Px(47), Px(30), rect.right - Px(6), rect.bottom - Px(4)},
                uiSmallFont, palette.secondary);
    } else if (checkbox) {
        const bool checked = SendMessageW(control, BM_GETCHECK, 0, 0) == BST_CHECKED;
        const int size = Px(17), top = (rect.bottom - size) / 2;
        RECT box{Px(1), top, Px(1) + size, top + size};
        Round(dc, box, Px(4), checked && enabled ? palette.accent : palette.field,
            focused ? palette.accent : Mix(palette.border, palette.accent, hover));
        if (checked) {
            HPEN pen = CreatePen(PS_SOLID, std::max(1, Px(2)), enabled ? palette.accent : palette.disabled);
            auto oldPen = SelectObject(dc, pen);
            MoveToEx(dc, Px(5), top + Px(8), nullptr);
            LineTo(dc, Px(8), top + Px(11)); LineTo(dc, Px(14), top + Px(5));
            SelectObject(dc, oldPen); DeleteObject(pen);
        }
        TextLine(dc, label, {Px(27), 0, rect.right, rect.bottom}, font,
            enabled ? palette.text : palette.disabled);
    } else if (group) {
        Round(dc, rect, Px(10), palette.panel, palette.border);
        RECT caption{Px(14), Px(2), rect.right - Px(12), Px(27)};
        TextLine(dc, label, caption, uiSmallFont, palette.secondary);
    } else {
        const COLORREF fill = !enabled ? palette.field :
            primary ? Mix(palette.accent, palette.text, hover * .12) :
            Mix(Mix(palette.field, palette.hover, hover), palette.selected, press);
        RECT surface = rect;
        const int inset = enabled ? static_cast<int>(Px(2) * press + .5) : 0;
        InflateRect(&surface, -inset, -inset);
        Round(dc, surface, Px(8), fill, focused ? palette.accent : primary && enabled ? fill :
            Mix(palette.border,palette.accent,std::min(1.0,hover*.5+press*.5)));
        RECT text{Px(10), 0, rect.right - Px(combo ? 31 : 10), rect.bottom};
        TextLine(dc, label, text, primary ? uiStrongFont : font,
            !enabled ? palette.disabled : primary ? palette.accent : palette.text,
            (combo ? DT_LEFT : DT_CENTER) | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (combo) {
            HPEN pen = CreatePen(PS_SOLID, std::max(1, Px(2)), enabled ? palette.secondary : palette.disabled);
            auto oldPen = SelectObject(dc, pen);
            const int x = rect.right - Px(19), y = rect.bottom / 2;
            MoveToEx(dc, x - Px(4), y - Px(2), nullptr);
            LineTo(dc, x, y + Px(2)); LineTo(dc, x + Px(4), y - Px(2));
            SelectObject(dc, oldPen); DeleteObject(pen);
        }
    }
    BitBlt(target, 0, 0, rect.right, rect.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, previous); DeleteObject(bitmap); DeleteDC(dc);
}

bool Application::DrawThemedItem(const DRAWITEMSTRUCT& item) {
    if (item.CtlType != ODT_COMBOBOX) return false;
    const auto palette = qp::PointerPalette(settings.anime);
    const bool selected = (item.itemState & ODS_SELECTED) != 0;
    Fill(item.hDC, item.rcItem, selected ? palette.selected : palette.field);
    std::wstring label;
    if (item.itemID != static_cast<UINT>(-1)) {
        const LRESULT length = SendMessageW(item.hwndItem, CB_GETLBTEXTLEN, item.itemID, 0);
        if (length >= 0) {
            label.resize(static_cast<size_t>(length) + 1);
            SendMessageW(item.hwndItem, CB_GETLBTEXT, item.itemID, reinterpret_cast<LPARAM>(label.data()));
            label.resize(static_cast<size_t>(length));
        }
    }
    RECT text = item.rcItem; text.left += Px(10); text.right -= Px(8);
    TextLine(item.hDC, label, text, font,
        item.itemState & ODS_DISABLED ? palette.disabled : selected ? palette.accent : palette.text);
    return true;
}
