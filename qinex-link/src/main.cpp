// QInEX-Link 主程序:无边框自绘深色 UI(模块入口见 docs/架构与维护.md)
#include "app.h"
#include "link.h"
#include "capture.h"
#include "sender.h"
#include "settings.h"
#include "netserver.h"
#include "performance.h"
#include "taskbar_widget.h"
#include <algorithm>
#include <cmath>
#include <shellapi.h>
#include <windowsx.h>
#include <vector>
#include <cwchar>
#pragma comment(lib, "msimg32.lib")   // AlphaBlend(启动验证锁定层半透明纱)

App g;

namespace {
void openPerformanceGuide() {
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(_countof(exe)));
    std::wstring path = exe;
    size_t slash = path.find_last_of(L"\\/");
    path = (slash == std::wstring::npos ? L"" : path.substr(0, slash + 1)) +
           L"后台性能说明.txt";
    HINSTANCE result = ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        MessageBoxW(g.hwnd,
                    L"未找到“后台性能说明.txt”。\n\n"
                    L"请把 Windows 电源模式设为“最佳性能”，关闭节电模式与 USB 选择性暂停，"
                    L"并以管理员身份运行 QInEX-Link。",
                    L"后台性能说明", MB_OK | MB_ICONINFORMATION);
    }
}

// 日志行 → UTF-8(带 BOM 的文件,记事本直接打开不乱码)
std::string logU8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
}  // namespace

void App::log(const std::wstring& s) {
    {
        std::lock_guard<std::mutex> lk(logMx);
        SYSTEMTIME t; GetLocalTime(&t);
        wchar_t ts[16];
        swprintf(ts, 16, L"[%02d:%02d:%02d] ", t.wHour, t.wMinute, t.wSecond);
        logLines.push_back(ts + s);
        while (logLines.size() > 200) logLines.pop_front();
        if (logFp) {   // 落盘开着:实时写 + 逐行 flush(用户侧崩溃/断电也不丢已发生的日志)
            fputs(logU8(logLines.back()).c_str(), logFp);
            fputs("\n", logFp);
            fflush(logFp);
        }
    }
    if (hwnd) PostMessage(hwnd, MSG_LOG, 0, 0);
}

void App::logFileOpen() {
    std::wstring path;
    {
        std::lock_guard<std::mutex> lk(logMx);
        if (logFp) return;   // 已开(幂等)
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring dir = exe;
        dir = dir.substr(0, dir.find_last_of(L"\\/") + 1) + L"logs";
        CreateDirectoryW(dir.c_str(), nullptr);
        SYSTEMTIME t; GetLocalTime(&t);
        wchar_t name[64];
        swprintf(name, 64, L"\\QInEX-Link_%04d%02d%02d_%02d%02d%02d.log",
                 t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        path = dir + name;
        logFp = _wfopen(path.c_str(), L"wb");
        if (!logFp) return;
        logPath = path;
        fputs("\xEF\xBB\xBF", logFp);   // UTF-8 BOM
        char head[128];
        snprintf(head, sizeof(head), "=== QInEX-Link 日志 %04d-%02d-%02d %02d:%02d:%02d ===\n",
                 t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        fputs(head, logFp);
        for (const auto& l : logLines) {   // 中途开启:先补写内存里已有的历史(最近 200 行)
            fputs(logU8(l).c_str(), logFp);
            fputs("\n", logFp);
        }
        fflush(logFp);
    }
    log(L"日志保存已开启 → " + path);
}

void App::logFileClose() {
    std::lock_guard<std::mutex> lk(logMx);
    if (!logFp) return;
    fclose(logFp);
    logFp = nullptr;
    logPath.clear();
}

namespace {

// ---- 主题色(对齐官网 index/static/css/style.css 的设计令牌:OLED 近黑 + 信号绿)----
#define C_BG    RGB(0x06, 0x09, 0x08)   // --ink
#define C_CARD  RGB(0x0D, 0x14, 0x11)   // --panel-solid
#define C_CARD2 RGB(0x14, 0x1E, 0x18)   // 面板提亮一档
#define C_BORD  RGB(0x1E, 0x26, 0x22)   // --line(绿调细线)
#define C_ACC   RGB(0x00, 0xFF, 0x88)   // --signal 品牌信号绿
#define C_ACCD  RGB(0x0A, 0x2E, 0x1D)   // signal 暗底
#define C_TEXT  RGB(0xEE, 0xF5, 0xF0)   // --paper
#define C_DIM   RGB(0x9D, 0xB0, 0xA6)   // --mist
#define C_GREEN RGB(0x00, 0xFF, 0x88)   // 健康状态 = signal
#define C_RED   RGB(0xFF, 0x54, 0x70)   // --velocity 速度红
#define C_YEL   RGB(0xF2, 0xBD, 0x5E)   // --solder 焊锡琥珀
#define C_LOGBG RGB(0x04, 0x06, 0x05)
#define C_ONACC RGB(0x04, 0x16, 0x0D)   // 信号绿底上的深色文字
#define C_GRID  RGB(0x08, 0x12, 0x0C)   // 背景电路网格线(signal 4% over ink)

constexpr int BASE_W = 460, BASE_H = 646;
constexpr wchar_t APP_VER[] = L"v0.1.13";

float SC = 1.0f;
int S(int v) { return (int)(v * SC + 0.5f); }
RECT SR(int x, int y, int w, int h) { return {S(x), S(y), S(x + w), S(y + h)}; }

HFONT fTitle, fText, fSmall, fBig, fMono, fIcon, fStat, fLock;

// 命中区
enum Hit {
    HIT_NONE = 0, HIT_CLOSE, HIT_MIN, HIT_PIN,
    HIT_COMBO, HIT_REFRESH, HIT_CONNECT,
    HIT_FWD, HIT_SHARE, HIT_EXCL, HIT_HOTKEY, HIT_STOP_ON_RELEASE,
    HIT_TIER0,   // ..HIT_TIER0+7
    HIT_AUTOSTART = HIT_TIER0 + 8,
    HIT_LINK3M,
    HIT_NET,        // kmboxNET 模拟开关
    HIT_LOGSAVE,    // 日志落盘开关(用户侧排障)
    HIT_TASKBAR,    // Windows 11 任务栏控制岛
};
const int TIER_VALS[8]   = {125, 250, 500, 1000, 2000, 4000, 8000, 0};
const wchar_t* TIER_LBL[8] = {L"125", L"250", L"500", L"1K", L"2K", L"4K", L"8K", L"直通"};

bool s_topmost = false;
int  s_inRate = 0, s_outRate = 0;
int  s_logScroll = 0;   // 日志上滚行数(0=贴最新);滚轮翻看历史
NOTIFYICONDATAW s_nid{};
UINT s_pointerReleaseMessage = 0;

// ---- 动画引擎:按需 60fps 定时器,全部停稳自动熄火(空闲零重绘零 CPU) ----
constexpr UINT_PTR TIMER_ANIM = 2;
constexpr int HIT_MAX = HIT_TASKBAR + 1;
bool  s_animOn = false;
bool  s_tracking = false;        // TrackMouseEvent 已挂
int   s_hoverHit = HIT_NONE;
float s_hov[HIT_MAX]   = {};     // 各命中区悬停强度 0..1(渐入渐出)
float s_press[HIT_MAX] = {};     // 点击闪烁 1→0
float s_segPos  = 0;             // 共享/独占滑块 0..1
float s_tierPos = -1;            // 回报率选中滑块(浮点档位索引,-1=未初始化)
float s_knobPos = 0;             // kmboxNET 开关旋钮 0..1
float s_barIn = 0, s_barOut = 0; // 速率条平滑值
float s_gate = 1.f;              // 启动验证锁定层 0..1(验证通过淡出)

// 颜色插值
COLORREF mix(COLORREF a, COLORREF b, float t) {
    if (t <= 0.f) return a;
    if (t >= 1.f) return b;
    auto ch = [t](int x, int y) { return x + (int)((y - x) * t + 0.5f); };
    return RGB(ch(GetRValue(a), GetRValue(b)), ch(GetGValue(a), GetGValue(b)),
               ch(GetBValue(a), GetBValue(b)));
}
// 呼吸脉冲 0..1(基于系统时钟,免相位漂移)
float pulse01(int periodMs) {
    float ph = (GetTickCount64() % periodMs) / (float)periodMs;
    return 0.5f - 0.5f * cosf(ph * 6.2831853f);
}
RECT lerpRect(RECT a, RECT b, float t) {
    auto L = [t](LONG x, LONG y) { return (LONG)(x + (y - x) * t + (y > x ? 0.5f : -0.5f)); };
    return {L(a.left, b.left), L(a.top, b.top), L(a.right, b.right), L(a.bottom, b.bottom)};
}
// 指数趋近;返回是否仍在运动
bool stepTo(float& v, float target, float k) {
    v += (target - v) * k;
    if (fabsf(target - v) < 0.004f) { v = target; return false; }
    return true;
}
void ensureAnim() {
    if (!s_animOn && g.hwnd) {
        SetTimer(g.hwnd, TIMER_ANIM, 15, nullptr);
        s_animOn = true;
    }
}
// 悬停提亮 + 按下闪烁的合成强度
float lift(int hit) { return s_hov[hit] * 0.6f + s_press[hit] * 0.4f; }
// 按下微缩(触感反馈)
RECT fx(RECT r, int hit) {
    int d = (int)(S(1) * s_press[hit] + 0.5f);
    if (d) InflateRect(&r, -d, -d);
    return r;
}

// ---- 布局(基准 460×646) ----
RECT rTitle()   { return SR(0, 0, BASE_W, 44); }
RECT rClose()   { return SR(BASE_W - 40, 6, 32, 32); }
RECT rMin()     { return SR(BASE_W - 76, 6, 32, 32); }
RECT rPin()     { return SR(BASE_W - 112, 6, 32, 32); }
RECT rDot()     { return SR(BASE_W - 136, 17, 10, 10); }
RECT rCard1()   { return SR(12, 54, 436, 100); }
RECT rCombo()   { return SR(24, 84, 288, 30); }
RECT rRefresh() { return SR(320, 84, 34, 30); }
RECT rConnect() { return SR(362, 84, 74, 30); }
RECT rStatus()  { return SR(24, 122, 412, 18); }
RECT rCard2()   { return SR(12, 162, 436, 88); }
RECT rFwd()     { return SR(24, 180, 150, 46); }
RECT rSeg(int i){ return SR(250, 178, 186, 28); }   // 两格
RECT rShare()   { return SR(250, 178, 93, 28); }
RECT rExcl()    { return SR(343, 178, 93, 28); }
RECT rHotkey()  { return SR(250, 208, 186, 22); }
RECT rStopOnRelease() { return SR(250, 230, 186, 18); }
RECT rCard3()   { return SR(12, 258, 436, 112); }
RECT rTier(int i) {
    int x0 = 24, w = 412;
    int a = x0 + i * w / 8, b = x0 + (i + 1) * w / 8;
    return SR(a, 288, b - a, 28);
}
RECT rRateIn()  { return SR(24, 326, 412, 16); }
RECT rRateOut() { return SR(24, 346, 412, 16); }
RECT rCard4()   { return SR(12, 378, 436, 124); }
RECT rTile(int i) {
    int col = i % 3, row = i / 3;
    return SR(24 + col * 143, 386 + row * 58, 136, 52);
}
RECT rCard5()   { return SR(12, 510, 436, 48); }
RECT rNetTog()  { return SR(158, 524, 38, 20); }
RECT rLog()     { return SR(12, 566, 436, 50); }
RECT rGear()    { return SR(16, 622, 20, 20); }
RECT rAuto()    { return SR(42, 622, 82, 20); }
RECT rLink3m()  { return SR(128, 622, 68, 20); }
RECT rLogSave() { return SR(200, 622, 78, 20); }
RECT rTaskbar() { return SR(282, 622, 82, 20); }

// ---- 绘制辅助 ----
void rrect(HDC dc, RECT r, COLORREF fill, COLORREF border, int rad) {
    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, S(rad), S(rad));
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(b); DeleteObject(p);
}
void txt(HDC dc, RECT r, HFONT f, COLORREF c, UINT fmt, const std::wstring& s) {
    HGDIOBJ of = SelectObject(dc, f);
    SetTextColor(dc, c);
    DrawTextW(dc, s.c_str(), -1, &r, fmt | DT_NOPREFIX | DT_SINGLELINE);
    SelectObject(dc, of);
}
void dot(HDC dc, RECT r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    HPEN p = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    Ellipse(dc, r.left, r.top, r.right, r.bottom);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(b); DeleteObject(p);
}

std::wstring keyName(UINT vk) {
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    bool ext = (vk == VK_RCONTROL || vk == VK_RMENU || vk == VK_INSERT || vk == VK_DELETE ||
                vk == VK_HOME || vk == VK_END || vk == VK_PRIOR || vk == VK_NEXT ||
                vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN ||
                vk == VK_DIVIDE || vk == VK_NUMLOCK || vk == VK_APPS || vk == VK_LWIN || vk == VK_RWIN);
    LONG l = (LONG)(sc << 16) | (ext ? (1 << 24) : 0);
    wchar_t buf[64];
    if (GetKeyNameTextW(l, buf, 64) > 0) return buf;
    return L"VK" + std::to_wstring(vk);
}

bool heartbeatOk() {
    ULONGLONG last = g.lastStatusMs.load();
    return last && GetTickCount64() - last < 12000;
}

// ---- 主绘制 ----
void paint(HDC dc, RECT rc) {
    // 背景:OLED 近黑 + 56px 电路网格(与官网 body 背景同款)
    HBRUSH bg = CreateSolidBrush(C_BG);
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    {
        HPEN gp = CreatePen(PS_SOLID, 1, C_GRID);
        HGDIOBJ op = SelectObject(dc, gp);
        int step = S(56);
        for (int gx = step; gx < rc.right; gx += step) {
            MoveToEx(dc, gx, 0, nullptr); LineTo(dc, gx, rc.bottom);
        }
        for (int gy = step; gy < rc.bottom; gy += step) {
            MoveToEx(dc, 0, gy, nullptr); LineTo(dc, rc.right, gy);
        }
        SelectObject(dc, op);
        DeleteObject(gp);
    }
    SetBkMode(dc, TRANSPARENT);

    int st = g.state.load();

    // === 标题栏 ===
    dot(dc, SR(16, 15, 14, 14), C_ACC);
    txt(dc, SR(40, 0, 260, 44), fTitle, C_TEXT, DT_LEFT | DT_VCENTER, L"QInEX-Link · 上位机直驱");
    COLORREF dc1 = (st == LS_CONNECTED && g.boardMode == 'C') ? C_GREEN
                 : (st == LS_IDLE ? C_DIM : C_YEL);
    if (dc1 == C_YEL) dc1 = mix(C_YEL, RGB(0x6B, 0x54, 0x20), 0.55f * pulse01(1200));  // 连接中呼吸
    dot(dc, rDot(), dc1);
    auto sysBtn = [&](RECT r, int hit, COLORREF hoverBg, const wchar_t* glyph, COLORREF base) {
        float t = s_hov[hit];
        if (t > 0.02f) rrect(dc, r, mix(C_BG, hoverBg, t), mix(C_BG, hoverBg, t), 6);
        txt(dc, r, fIcon, mix(base, C_TEXT, t), DT_CENTER | DT_VCENTER, glyph);
    };
    sysBtn(rPin(),   HIT_PIN,   C_CARD2, L"\uE718", s_topmost ? C_ACC : C_DIM);
    sysBtn(rMin(),   HIT_MIN,   C_CARD2, L"\uE921", C_DIM);
    sysBtn(rClose(), HIT_CLOSE, RGB(0xA8, 0x2E, 0x44), L"\uE8BB", C_DIM);

    // === 卡1 连接 ===
    rrect(dc, rCard1(), C_CARD, C_BORD, 10);
    txt(dc, SR(24, 60, 100, 18), fSmall, C_DIM, DT_LEFT | DT_VCENTER, L"连接");
    {
        float t = s_hov[HIT_COMBO];
        rrect(dc, fx(rCombo(), HIT_COMBO), mix(C_CARD2, RGB(0x1C, 0x2B, 0x22), t),
              mix(C_BORD, C_ACC, t * 0.7f), 6);
        RECT r = rCombo(); r.left += S(10); r.right -= S(26);
        std::wstring pt = g.portName.empty() ? L"选择串口…" : g.portName;
        txt(dc, r, fText, g.portName.empty() ? mix(C_DIM, C_TEXT, t) : C_TEXT,
            DT_LEFT | DT_VCENTER, pt);
        RECT ch = rCombo(); ch.left = ch.right - S(26);
        txt(dc, ch, fIcon, mix(C_DIM, C_ACC, t), DT_CENTER | DT_VCENTER, L"\uE70D");
    }
    {
        float t = s_hov[HIT_REFRESH];
        rrect(dc, fx(rRefresh(), HIT_REFRESH), mix(C_CARD2, RGB(0x1C, 0x2B, 0x22), t),
              mix(C_BORD, C_ACC, t * 0.7f), 6);
        txt(dc, rRefresh(), fIcon, mix(C_DIM, C_ACC, t), DT_CENTER | DT_VCENTER, L"\uE72C");
    }
    bool conn = (st != LS_IDLE);
    {
        float lf = lift(HIT_CONNECT);
        COLORREF fill = conn ? mix(C_CARD2, RGB(0x1C, 0x2B, 0x22), s_hov[HIT_CONNECT])
                             : mix(C_ACC, RGB(0x7A, 0xFF, 0xC0), lf);
        COLORREF bord = conn ? mix(C_BORD, C_RED, s_hov[HIT_CONNECT] * 0.6f) : fill;
        rrect(dc, fx(rConnect(), HIT_CONNECT), fill, bord, 6);
        txt(dc, rConnect(), fText, conn ? C_TEXT : C_ONACC, DT_CENTER | DT_VCENTER,
            conn ? L"断开" : L"连接");
    }
    {
        std::wstring s; COLORREF c = C_DIM;
        switch (st) {
        case LS_IDLE:       s = L"未连接"; break;
        case LS_OPENING:
        case LS_WAITSTATUS: s = L"等待 P4 响应…"; c = C_YEL; break;
        case LS_NEGOTIATE:  s = L"协商 3 Mbps 波特率…"; c = C_YEL; break;
        case LS_RECONNECT:  s = L"链路中断,重连中…"; c = C_YEL; break;
        case LS_CONNECTED:
            if (g.boardMode == 'C' && g.boardAct.load() == 0) {
                s = L"P4 未授权：请在板载页完成设备授权";
                c = C_RED;
            } else if (g.boardMode == 'C') {
                int b = g.boardBaud.load();
                wchar_t buf[128];
                swprintf(buf, 128, L"上位机直驱 · %s · %s",
                         g.usbMounted ? L"USB 已连接" : L"等待 USB 连接",
                         b >= 3000000 ? L"3 Mbps" : L"921.6 Kbps");
                s = buf; c = g.usbMounted ? C_GREEN : C_YEL;
            } else {
                s = L"单机模式：请开启上位机直驱，接线步骤见日志";
                c = C_RED;
            }
            break;
        }
        txt(dc, rStatus(), fSmall, c, DT_LEFT | DT_VCENTER, s);
    }

    // === 卡2 主开关 ===
    rrect(dc, rCard2(), C_CARD, C_BORD, 10);
    bool ready = link::txReady();
    bool fwd = g.forwarding;
    {
        float lf = lift(HIT_FWD);
        if (fwd) {   // 转发中:外圈呼吸光环
            RECT rg = rFwd(); InflateRect(&rg, S(3), S(3));
            rrect(dc, rg, C_CARD, mix(C_CARD, C_ACC, 0.22f + 0.45f * pulse01(1600)), 26);
        }
        COLORREF fill = fwd ? mix(C_ACC, RGB(0x7A, 0xFF, 0xC0), lf * 0.6f)
                            : (ready ? mix(C_ACCD, RGB(0x10, 0x40, 0x27), lf) : C_CARD2);
        COLORREF tc   = fwd ? C_ONACC
                            : (ready ? mix(C_ACC, RGB(0x7A, 0xFF, 0xC0), lf) : C_DIM);
        rrect(dc, fx(rFwd(), HIT_FWD), fill,
              fwd ? C_ACC : mix(C_BORD, C_ACC, ready ? 0.3f + lf * 0.7f : lf * 0.4f), 23);
        std::wstring lbl = fwd ? (g.exclusive && !g.grabbed ? L"转发中·已放行" : L"转发中")
                               : L"开始转发";
        txt(dc, rFwd(), fBig, tc, DT_CENTER | DT_VCENTER, lbl);
    }
    {
        rrect(dc, rSeg(0), C_CARD2, C_BORD, 6);
        RECT rs = rShare(), re = rExcl();
        rrect(dc, lerpRect(rs, re, s_segPos), C_ACC, C_ACC, 6);   // 选中滑块平滑滑动
        txt(dc, rs, fText, mix(C_ONACC, mix(C_DIM, C_TEXT, s_hov[HIT_SHARE]), s_segPos),
            DT_CENTER | DT_VCENTER, L"共享");
        txt(dc, re, fText, mix(mix(C_DIM, C_TEXT, s_hov[HIT_EXCL]), C_ONACC, s_segPos),
            DT_CENTER | DT_VCENTER, L"独占");
        std::wstring hk = g.bindWait ? L"按下新热键…"
                                     : L"收放热键: " + keyName(g.hotkeyVk.load());
        COLORREF hc = g.bindWait ? mix(C_YEL, RGB(0x8A, 0x6E, 0x28), 0.5f * pulse01(900))
                                 : mix(g.exclusive ? C_ACC : C_DIM, C_TEXT, s_hov[HIT_HOTKEY] * 0.8f);
        txt(dc, rHotkey(), fSmall, hc, DT_LEFT | DT_VCENTER, hk + L"  \uE70F");
        COLORREF sc = mix(g.stopOnRelease ? C_ACC : C_DIM, C_TEXT,
                          s_hov[HIT_STOP_ON_RELEASE] * 0.8f);
        txt(dc, rStopOnRelease(), fSmall, sc, DT_LEFT | DT_VCENTER,
            g.stopOnRelease ? L"热键放行时停转发 \uE73E" : L"热键放行时停转发");
    }

    // === 卡3 回报率 ===
    rrect(dc, rCard3(), C_CARD, C_BORD, 10);
    txt(dc, SR(24, 264, 100, 18), fSmall, C_DIM, DT_LEFT | DT_VCENTER, L"回报率");
    {
        int selIdx = 0;
        for (int i = 0; i < 8; i++) if (g.tier == TIER_VALS[i]) selIdx = i;
        if (s_tierPos < 0) s_tierPos = (float)selIdx;
        for (int i = 0; i < 8; i++) {   // 底格(带 hover 提亮)
            float t = s_hov[HIT_TIER0 + i];
            rrect(dc, rTier(i), mix(C_CARD2, RGB(0x1C, 0x2B, 0x22), t),
                  mix(C_BORD, C_ACC, t * 0.5f), 6);
        }
        int i0 = (int)s_tierPos, i1 = i0 + 1 > 7 ? 7 : i0 + 1;   // 选中块滑动
        rrect(dc, lerpRect(rTier(i0), rTier(i1), s_tierPos - i0), C_ACC, C_ACC, 6);
        for (int i = 0; i < 8; i++) {
            float prox = 1.f - fabsf(i - s_tierPos); if (prox < 0) prox = 0;
            COLORREF lc = mix(mix(C_DIM, C_TEXT, s_hov[HIT_TIER0 + i] * 0.8f),
                              C_ONACC, prox);
            txt(dc, rTier(i), fText, lc, DT_CENTER | DT_VCENTER, TIER_LBL[i]);
        }
    }
    auto rateRow = [&](RECT r, const wchar_t* name, int rate, float anim) {
        RECT rl = r; rl.right = rl.left + S(86);
        txt(dc, rl, fSmall, C_DIM, DT_LEFT | DT_VCENTER, name);
        RECT rv = r; rv.left += S(88); rv.right = rv.left + S(74);
        txt(dc, rv, fSmall, C_TEXT, DT_LEFT | DT_VCENTER, std::to_wstring(rate) + L" /s");
        RECT rb = r; rb.left += S(168); rb.top += S(5); rb.bottom -= S(5);
        rrect(dc, rb, C_CARD2, C_BORD, 4);
        double k = anim / 8000.0; if (k > 1) k = 1;   // 平滑追赶,不跳变
        if (k > 0.004) {
            RECT rf = rb;
            rf.right = rf.left + (int)((rb.right - rb.left) * k);
            rrect(dc, rf, mix(C_ACC, C_GREEN, (float)(k > 0.9 ? (k - 0.9) * 10 : 0)), C_ACC, 4);
        }
    };
    rateRow(rRateIn(),  L"输入事件率", s_inRate,  s_barIn);
    rateRow(rRateOut(), L"输出帧率",   s_outRate, s_barOut);

    // === 卡4 仪表 ===
    rrect(dc, rCard4(), C_CARD, C_BORD, 10);
    auto tile = [&](int i, const wchar_t* name, const std::wstring& val, COLORREF vc) {
        RECT r = rTile(i);
        rrect(dc, r, C_CARD2, C_BORD, 8);
        RECT rn = r; rn.bottom = rn.top + S(22); rn.top += S(4);
        txt(dc, rn, fSmall, C_DIM, DT_CENTER | DT_VCENTER, name);
        RECT rv = r; rv.top += S(22);
        txt(dc, rv, fStat, vc, DT_CENTER | DT_VCENTER, val);
    };
    {
        int rtt = g.rttX10.load();
        wchar_t buf[32];
        if (rtt >= 0) swprintf(buf, 32, L"%d.%d ms", rtt / 10, rtt % 10); else wcscpy(buf, L"—");
        tile(0, L"RTT", buf, C_TEXT);
        int b = g.boardBaud.load();
        tile(1, L"链路", b >= 3000000 ? L"3 Mbps" : (b > 0 ? L"921.6 K" : L"—"), C_TEXT);
        int ue = g.uerrSession.load();   // 本次连接新增(非板子累计)
        tile(2, L"校验错误", std::to_wstring(ue), ue > 0 ? C_RED : C_TEXT);
        // STATUS 是 5 秒窗口累计值,旧界面直接把 dps 原数显示成“每秒”会放大 5 倍。
        // 现在把 PC(上方两行)→UART→映射生成→手机实际取走完整串起来,用户一张截图即可定位。
        int ukHz = (g.boardUkm.load() + 2) / 5;
        int fpHz = (g.boardFps.load() + 2) / 5;
        int dpHz = (g.boardDps.load() + 2) / 5;
        int uqMax = g.boardUqMax.load(), udrop = g.boardUdrop.load();
        std::wstring ukText = std::to_wstring(ukHz);
        if (st == LS_CONNECTED && g.mouseV2.load()) ukText += L" / q" + std::to_wstring(uqMax);
        tile(3, L"板端输入 Hz / 队列峰值", st == LS_CONNECTED ? ukText : L"—",
             (st == LS_CONNECTED && udrop > 0) ? C_RED : C_TEXT);
        tile(4, L"生成 / 手机 Hz",
             st == LS_CONNECTED ? std::to_wstring(fpHz) + L" / " + std::to_wstring(dpHz) : L"—",
             (st == LS_CONNECTED && fpHz > 0 && dpHz * 4 < fpHz * 3) ? C_RED : C_TEXT);
        int gm = g.boardGmaxUs.load(), dm = g.boardDmaxUs.load();
        if (st == LS_CONNECTED && (gm > 0 || dm > 0)) {
            swprintf(buf, 32, L"%.1f / %.1f", gm / 1000.0, dm / 1000.0);
            // 空闲/低回报率档本来就会按 5~100ms 保活,不能把正常间隔标红。只有当前窗口
            // 确实在跑高频输入/输出时,>3ms 才代表一次真实卡拍。
            bool highRateActive = ukHz >= 500 || fpHz >= 500 || dpHz >= 500;
            tile(5, L"最大帧间隔 G / D ms", buf,
                 (highRateActive && (gm > 3000 || dm > 3000)) ? C_RED : C_TEXT);
        } else {
            tile(5, L"最大帧间隔 G / D ms", L"—", C_DIM);
        }
    }

    // === 卡5 kmboxNET 模拟 ===
    rrect(dc, rCard5(), C_CARD, C_BORD, 10);
    bool netOn = g.netEnable;
    bool netUp = g.netUp.load();
    txt(dc, SR(24, 510, 130, 48), fText, netOn ? C_TEXT : C_DIM, DT_LEFT | DT_VCENTER, L"kmboxNET 模拟");
    // 开关滑块(点击切换,旋钮平滑滑动)
    {
        float t = s_hov[HIT_NET];
        RECT tog = rNetTog();
        rrect(dc, tog, mix(netOn ? C_ACCD : C_CARD2, RGB(0x17, 0x33, 0x24), t * 0.6f),
              mix(netOn ? C_ACC : C_BORD, C_ACC, t), 10);
        RECT knob = lerpRect(SR(161, 527, 14, 14), SR(178, 527, 14, 14), s_knobPos);
        dot(dc, knob, mix(C_DIM, C_ACC, s_knobPos));
    }
    {
        wchar_t nb[96];
        if (netOn && netUp) {
            swprintf(nb, 96, L"UDP %d · MAC %08X · 收 %u", g.netPort, g.netMac, g.netRx.load());
            txt(dc, SR(210, 510, 228, 48), fSmall, C_GREEN, DT_LEFT | DT_VCENTER, nb);
        } else if (netOn) {
            txt(dc, SR(210, 510, 228, 48), fSmall, C_RED, DT_LEFT | DT_VCENTER, L"启动失败(端口被占?)");
        } else {
            swprintf(nb, 96, L"UDP %d · 关闭(第三方软件入口)", g.netPort);
            txt(dc, SR(210, 510, 228, 48), fSmall, C_DIM, DT_LEFT | DT_VCENTER, nb);
        }
    }

    // === 启动验证锁定层(罩住功能区卡2–卡5;见过一次「P4 已激活」即淡出解锁)===
    // 与 APP /api/appauth 同定位:防"没 P4 的人白嫖工具",不防伪造串口心跳(离线无密码学可依)。
    if (s_gate > 0.01f) {
        RECT ov{rCard2().left, rCard2().top, rCard5().right, rCard5().bottom};
        {   // 半透明黑纱:1×1 位图 AlphaBlend 拉伸,常量透明度随 s_gate 淡出
            HDC mdc = CreateCompatibleDC(dc);
            HBITMAP mb = CreateCompatibleBitmap(dc, 1, 1);
            HGDIOBJ omb = SelectObject(mdc, mb);
            SetPixel(mdc, 0, 0, RGB(0x03, 0x05, 0x04));
            BLENDFUNCTION bf{AC_SRC_OVER, 0, (BYTE)(215 * s_gate), 0};
            AlphaBlend(dc, ov.left, ov.top, ov.right - ov.left, ov.bottom - ov.top,
                       mdc, 0, 0, 1, 1, bf);
            SelectObject(mdc, omb); DeleteObject(mb); DeleteDC(mdc);
        }
        // 文案随纱一起淡出(颜色向纱底色回混)
        auto fade = [&](COLORREF c) { return mix(RGB(0x03, 0x05, 0x04), c, s_gate); };
        int cy = (ov.top + ov.bottom) / 2;
        bool bad = (st == LS_CONNECTED && g.boardAct.load() == 0);
        COLORREF lc = bad ? C_RED : mix(C_YEL, RGB(0x8A, 0x6E, 0x28), 0.4f * pulse01(1600));
        txt(dc, {ov.left, cy - S(56), ov.right, cy - S(16)}, fLock, fade(lc),
            DT_CENTER | DT_VCENTER, L"\uE72E");   // MDL2 Lock
        txt(dc, {ov.left, cy - S(10), ov.right, cy + S(12)}, fBig, fade(C_TEXT),
            DT_CENTER | DT_VCENTER, L"启动验证");
        std::wstring hint;
        switch (st) {
        case LS_IDLE:      hint = L"请连接 P4 设备完成验证"; break;
        case LS_CONNECTED: hint = bad ? L"P4 未授权，请在板载页完成设备授权"
                                      : L"已连接,等待板子心跳确认…"; break;
        default:           hint = L"正在等待 P4 响应…"; break;
        }
        txt(dc, {ov.left, cy + S(16), ov.right, cy + S(36)}, fSmall, fade(bad ? C_RED : C_DIM),
            DT_CENTER | DT_VCENTER, hint);
    }

    // === 日志条(3 行可见,滚轮翻看历史)===
    rrect(dc, rLog(), C_LOGBG, C_BORD, 8);
    {
        std::lock_guard<std::mutex> lk(g.logMx);
        int n = (int)g.logLines.size();
        int maxScroll = n > 3 ? n - 3 : 0;
        if (s_logScroll > maxScroll) s_logScroll = maxScroll;
        if (s_logScroll < 0) s_logScroll = 0;
        for (int i = 0; i < 3; i++) {
            int idx = n - 1 - s_logScroll - i;   // 顶行最旧、底行最新
            if (idx < 0 || idx >= n) continue;
            RECT r = rLog();
            r.left += S(10); r.right -= S(20);
            r.top += S(3 + (2 - i) * 15);
            r.bottom = r.top + S(15);
            txt(dc, r, fMono, RGB(0x54, 0xD8, 0x94), DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS,
                g.logLines[idx]);
        }
        // 右侧滚动指示:上滚了显示 ▲,还有更新的显示 ▼
        RECT ri = rLog(); ri.left = ri.right - S(16); ri.right -= S(4);
        if (s_logScroll < maxScroll)
            txt(dc, {ri.left, ri.top + S(2), ri.right, ri.top + S(15)}, fIcon, C_ACC,
                DT_CENTER | DT_VCENTER, L"\uE70E");
        if (s_logScroll > 0)
            txt(dc, {ri.left, ri.bottom - S(15), ri.right, ri.bottom - S(2)}, fIcon, C_ACC,
                DT_CENTER | DT_VCENTER, L"\uE70D");
    }

    // === 底栏 ===
    txt(dc, rGear(), fIcon, C_DIM, DT_CENTER | DT_VCENTER, L"\uE713");
    txt(dc, rAuto(), fSmall,
        mix(g.autostart ? C_GREEN : C_DIM, C_TEXT, s_hov[HIT_AUTOSTART] * 0.7f),
        DT_LEFT | DT_VCENTER, g.autostart ? L"开机自启 \uE73E" : L"开机自启");
    txt(dc, rLink3m(), fSmall,
        mix(g.link3m ? C_ACC : C_DIM, C_TEXT, s_hov[HIT_LINK3M] * 0.7f),
        DT_LEFT | DT_VCENTER, g.link3m ? L"3M链路 \uE73E" : L"3M链路");
    txt(dc, rLogSave(), fSmall,
        mix(g.logSave ? C_YEL : C_DIM, C_TEXT, s_hov[HIT_LOGSAVE] * 0.7f),
        DT_LEFT | DT_VCENTER, g.logSave ? L"保存日志 \uE73E" : L"保存日志");
    txt(dc, rTaskbar(), fSmall,
        mix(g.taskbarWidget ? C_ACC : C_DIM, C_TEXT, s_hov[HIT_TASKBAR] * 0.7f),
        DT_LEFT | DT_VCENTER, g.taskbarWidget ? L"任务栏 \uE73E" : L"任务栏");
    txt(dc, SR(BASE_W - 76, 622, 64, 20), fSmall, C_DIM, DT_RIGHT | DT_VCENTER, APP_VER);

    // 窗口外框:1px 绿调细线(官网 --line-strong 面板描边)
    {
        HPEN fp = CreatePen(PS_SOLID, 1, RGB(0x2E, 0x3E, 0x35));
        HGDIOBJ op = SelectObject(dc, fp), ob2 = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, 0, 0, rc.right, rc.bottom);
        SelectObject(dc, op); SelectObject(dc, ob2);
        DeleteObject(fp);
    }
}

// 推一帧动画;返回是否还有动画在跑(false → 熄火停定时器)
bool animTick() {
    bool live = false;
    for (int i = 0; i < HIT_MAX; i++) {
        live |= stepTo(s_hov[i], i == s_hoverHit ? 1.f : 0.f, 0.28f);
        live |= stepTo(s_press[i], 0.f, 0.14f);
    }
    live |= stepTo(s_segPos, g.exclusive ? 1.f : 0.f, 0.30f);
    int ti = 0;
    for (int i = 0; i < 8; i++) if (g.tier == TIER_VALS[i]) ti = i;
    if (s_tierPos < 0) s_tierPos = (float)ti;
    live |= stepTo(s_tierPos, (float)ti, 0.30f);
    live |= stepTo(s_knobPos, g.netEnable ? 1.f : 0.f, 0.30f);
    live |= stepTo(s_barIn,  (float)s_inRate,  0.18f);
    live |= stepTo(s_barOut, (float)s_outRate, 0.18f);
    live |= stepTo(s_gate, g.verified ? 0.f : 1.f, 0.16f);   // 验证通过锁定层淡出
    if (s_gate > 0.5f && GetForegroundWindow() == g.hwnd) live = true;   // 锁图标呼吸
    // 持续呼吸:连接中/转发中/等待绑定热键期间保持心跳。
    // 仅前台时跑——转发中窗口多半被游戏盖住,后台 60fps GDI 重绘白耗 CPU,还跟
    // 转发链路(最忌抖动的时刻)抢时间片;失焦即熄火,WM_ACTIVATE 回前台再续。
    int st = g.state.load();
    if ((st == LS_OPENING || st == LS_WAITSTATUS || st == LS_NEGOTIATE || st == LS_RECONNECT ||
         g.forwarding || g.bindWait) && GetForegroundWindow() == g.hwnd)
        live = true;
    return live;
}

int hitTest(POINT p) {
    auto in = [&](RECT r) { return PtInRect(&r, p); };
    if (in(rClose()))   return HIT_CLOSE;
    if (in(rMin()))     return HIT_MIN;
    if (in(rPin()))     return HIT_PIN;
    if (in(rCombo()))   return HIT_COMBO;
    if (in(rRefresh())) return HIT_REFRESH;
    if (in(rConnect())) return HIT_CONNECT;
    if (!g.verified) {   // 启动验证锁定层罩住功能区:连接卡/标题栏/底栏之外一律不可点
        if (in(rAuto()))    return HIT_AUTOSTART;
        if (in(rLink3m()))  return HIT_LINK3M;
        if (in(rLogSave())) return HIT_LOGSAVE;   // 排障开关锁定期也可用(验证失败正需要日志)
        if (in(rTaskbar())) return HIT_TASKBAR;
        return HIT_NONE;
    }
    if (in(rFwd()))     return HIT_FWD;
    if (in(rShare()))   return HIT_SHARE;
    if (in(rExcl()))    return HIT_EXCL;
    if (in(rHotkey()))  return HIT_HOTKEY;
    if (in(rStopOnRelease())) return HIT_STOP_ON_RELEASE;
    for (int i = 0; i < 8; i++) if (in(rTier(i))) return HIT_TIER0 + i;
    if (in(rAuto()))    return HIT_AUTOSTART;
    if (in(rLink3m()))  return HIT_LINK3M;
    if (in(rLogSave())) return HIT_LOGSAVE;
    if (in(rTaskbar())) return HIT_TASKBAR;
    if (in(rNetTog()))  return HIT_NET;
    return HIT_NONE;
}

void setForwarding(bool on) {
    if (on && !link::txReady()) return;
    bool was = g.forwarding.exchange(on);
    g.grabbed = on && g.exclusive;
    if (!on && was && capture::evtInput) {
        // UI 不同步等待 TIME_CRITICAL 发送线程；发送循环看到状态边沿后负责全抬起。
        // 直通档显式唤醒，确保停止不依赖下一个键鼠事件。
        SetEvent(capture::evtInput);
    }
    g.log(on ? (g.exclusive ? L"开始转发(独占,热键收放本机输入)" : L"开始转发(共享)")
             : L"停止转发");
    ensureAnim();   // 转发中呼吸光环
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

void pickPort() {
    auto ports = link::enumPorts();
    HMENU m = CreatePopupMenu();
    if (ports.empty()) AppendMenuW(m, MF_GRAYED, 0, L"(没有找到串口)");
    for (size_t i = 0; i < ports.size(); i++)
        AppendMenuW(m, MF_STRING, 100 + (UINT)i, ports[i].friendly.c_str());
    POINT p{rCombo().left, rCombo().bottom};
    ClientToScreen(g.hwnd, &p);
    int id = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN, p.x, p.y, 0, g.hwnd, nullptr);
    DestroyMenu(m);
    if (id >= 100 && id - 100 < (int)ports.size()) {
        g.portName = ports[id - 100].name;
        settings::save();
        InvalidateRect(g.hwnd, nullptr, FALSE);
    }
}

void onClick(int hit) {
    switch (hit) {
    case HIT_CLOSE:
        ShowWindow(g.hwnd, SW_HIDE);   // 收托盘,转发不中断
        break;
    case HIT_MIN:
        ShowWindow(g.hwnd, SW_MINIMIZE);
        break;
    case HIT_PIN:
        s_topmost = !s_topmost;
        SetWindowPos(g.hwnd, s_topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE);
        break;
    case HIT_COMBO:
    case HIT_REFRESH:
        if (g.state == LS_IDLE) pickPort();
        break;
    case HIT_CONNECT:
        if (g.state == LS_IDLE) {
            if (g.portName.empty()) { pickPort(); if (g.portName.empty()) break; }
            link::connect(g.portName);
        } else {
            setForwarding(false);
            sender::releaseAll();   // 串口关闭前同步兜底；发送锁已不覆盖节拍等待
            link::disconnect();
            g.log(L"已断开");
        }
        break;
    case HIT_FWD:
        setForwarding(!g.forwarding);
        break;
    case HIT_SHARE:
    case HIT_EXCL: {
        bool ex = (hit == HIT_EXCL);
        if (g.exclusive != ex) {
            g.exclusive = ex;
            g.grabbed = g.forwarding && ex;
            settings::save();
        }
        break;
    }
    case HIT_HOTKEY:
        g.bindWait = true;
        break;
    case HIT_STOP_ON_RELEASE:
        g.stopOnRelease = !g.stopOnRelease;
        settings::save();
        g.log(g.stopOnRelease ? L"已开启:热键放行时同时停止转发"
                              : L"已关闭:热键放行后继续转发");
        break;
    case HIT_AUTOSTART:
        g.autostart = !g.autostart;
        settings::applyAutostart();
        settings::save();
        break;
    case HIT_LINK3M:
        g.link3m = !g.link3m;
        settings::save();
        g.log(g.link3m ? L"已开启 3M 链路(下次连接时验证式提速,失败自动退回)"
                       : L"已关闭 3M 链路(稳在 921600,足够 8K)");
        break;
    case HIT_LOGSAVE:
        g.logSave = !g.logSave;
        settings::save();
        if (g.logSave) {
            g.logFileOpen();   // 开文件 + 补写内存 backlog;成功后自报路径
            if (!g.logFp) { g.logSave = false; settings::save(); g.log(L"日志文件创建失败(目录只读?)"); }
        } else {
            g.log(L"日志保存已关闭");   // 先记再关,这条留在文件里作结尾
            g.logFileClose();
        }
        break;
    case HIT_TASKBAR:
        g.taskbarWidget = !g.taskbarWidget;
        settings::save();
        taskbar_widget::setEnabled(g.taskbarWidget);
        g.log(g.taskbarWidget ? L"任务栏控制岛已开启" : L"任务栏控制岛已关闭");
        break;
    case HIT_NET:
        g.netEnable = !g.netEnable;
        settings::save();
        netserver::applyConfig();   // 内部据 g.netEnable 起/停;首次起服会分配随机 MAC
        settings::save();           // 再存一次:落盘首次生成的 MAC
        g.log(g.netEnable ? L"kmboxNET 模拟已开启(第三方连本机同端口/MAC)"
                          : L"kmboxNET 模拟已关闭");
        break;
    default:
        if (hit >= HIT_TIER0 && hit < HIT_TIER0 + 8) {
            g.tier = TIER_VALS[hit - HIT_TIER0];
            settings::save();
        }
        break;
    }
    taskbar_widget::updateConfig(g.tier, g.exclusive);
    ensureAnim();   // 状态变化 → 滑块/开关/呼吸动画补间
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

void trayMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, L"显示窗口");
    AppendMenuW(m, MF_STRING | (g.forwarding ? MF_CHECKED : 0), 2, L"转发");
    AppendMenuW(m, MF_STRING | (g.taskbarWidget ? MF_CHECKED : 0), 4, L"任务栏控制岛");
    AppendMenuW(m, MF_STRING, 5, L"后台性能说明");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 3, L"退出");
    POINT p; GetCursorPos(&p);
    SetForegroundWindow(g.hwnd);
    int id = TrackPopupMenu(m, TPM_RETURNCMD, p.x, p.y, 0, g.hwnd, nullptr);
    DestroyMenu(m);
    if (id == 1) { ShowWindow(g.hwnd, SW_SHOW); SetForegroundWindow(g.hwnd); }
    if (id == 2) setForwarding(!g.forwarding);
    if (id == 4) onClick(HIT_TASKBAR);
    if (id == 5) openPerformanceGuide();
    if (id == 3) DestroyWindow(g.hwnd);
}

LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (s_pointerReleaseMessage && msg == s_pointerReleaseMessage) {
        // QInEX Pointer 已冻结校准截图。只放行本机输入，链路继续转发，
        // 用户可以立即在截图窗口标记触点，无需记忆 Link 的收放热键。
        if (g.grabbed.exchange(false))
            g.log(L"光标校准：已临时放行本机键鼠（转发继续）");
        taskbar_widget::updateConfig(g.tier, g.exclusive);
        ensureAnim();
        InvalidateRect(h, nullptr, FALSE);
        return 1;
    }
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ ob = SelectObject(mem, bmp);
        paint(mem, rc);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, ob);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_NCHITTEST: {
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(h, &p);
        RECT t = rTitle();
        if (PtInRect(&t, p) && hitTest(p) == HIT_NONE) return HTCAPTION;
        break;
    }
    case WM_LBUTTONDOWN: {
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int hit = hitTest(p);
        if (hit != HIT_NONE) {
            s_press[hit] = 1.f;   // 按下闪烁 + 微缩
            ensureAnim();
            onClick(hit);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (!s_tracking) {   // 挂 WM_MOUSELEAVE,离窗时收掉 hover
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
            TrackMouseEvent(&tme);
            s_tracking = true;
        }
        int hv = hitTest(p);
        if (hv != s_hoverHit) { s_hoverHit = hv; ensureAnim(); }
        return 0;
    }
    case WM_MOUSELEAVE:
        s_tracking = false;
        if (s_hoverHit != HIT_NONE) { s_hoverHit = HIT_NONE; ensureAnim(); }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT p; GetCursorPos(&p); ScreenToClient(h, &p);
            SetCursor(LoadCursorW(nullptr, hitTest(p) != HIT_NONE ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE) {
            // 窗口重新激活意味着用户即将测试前台热键；清掉任何历史丢失的 key-up 锁。
            // 正常按住热键不会导致窗口激活,因此不会破坏一次按下一次切换语义。
            capture::resetHotkeyLatch();
            ensureAnim();   // 回前台续上呼吸动画(animTick 失焦熄火)
        }
        break;
    case WM_PRINTCLIENT: {   // AnimateWindow 淡入需要
        RECT rc; GetClientRect(h, &rc);
        paint((HDC)wp, rc);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};   // 屏幕坐标
        ScreenToClient(h, &p);
        RECT rl = rLog();
        // 光标在日志条(或下半区)上就滚日志历史;滚轮上=看更早
        RECT zone = rl; zone.top -= S(60);
        if (PtInRect(&zone, p)) {
            int d = GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1 : -1;
            s_logScroll += d;
            if (s_logScroll < 0) s_logScroll = 0;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_TIMER:
        if (wp == 1) {
            // 1s 速率差分
            static uint32_t pi = 0, po = 0;
            uint32_t ci = g.inEvents.load(), co = g.outFrames.load();
            s_inRate = (int)(ci - pi); s_outRate = (int)(co - po);
            pi = ci; po = co;
            if (IsWindowVisible(h)) { ensureAnim(); InvalidateRect(h, nullptr, FALSE); }
        } else if (wp == TIMER_ANIM) {
            if (!IsWindowVisible(h) || !animTick()) {   // 停稳/隐藏 → 熄火,零空转
                KillTimer(h, TIMER_ANIM);
                s_animOn = false;
            }
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case MSG_HOTKEY:
        // 全局独占热键状态机:不依赖窗口焦点。链路已就绪时,第一次按下可从“未转发/共享”
        // 直接切到独占转发;再次按下时按 stopOnRelease 设置决定仅放行还是同时停转发。
        if (g.grabbed) {
            if (g.stopOnRelease) {
                setForwarding(false);
            } else {
                g.grabbed = false;
                g.log(L"独占:已放行本机键鼠(转发继续)");
            }
        } else if (!link::txReady()) {
            // 未连接时绝不只锁本机却无处转发。把窗口带出来说明原因,由用户完成连接。
            g.grabbed = false;
            g.log(L"独占热键已收到,但 P4 链路未就绪——请先连接已授权的上位机直驱设备");
            ShowWindow(h, SW_SHOW);
            ShowWindow(h, SW_RESTORE);
            SetForegroundWindow(h);
        } else {
            bool switched = !g.exclusive;
            if (switched) {
                g.exclusive = true;
                settings::save();
            }
            if (!g.forwarding) {
                setForwarding(true);   // exclusive 已置 true → 自动开始转发并立即进入遮罩
            } else {
                g.grabbed = true;
                g.log(switched ? L"独占热键:已从共享转发切换为独占遮罩"
                               : L"独占:已收回本机键鼠(再按热键放行)");
            }
        }
        taskbar_widget::updateConfig(g.tier, g.exclusive);
        ensureAnim();
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case MSG_PANIC:
        // 应急解锁(Ctrl+Alt+Q):独占死锁的最后出口 —— 释放吞入 + 停转发 + 清残留状态,
        //   并把窗口拉到前台,让用户看得见发生了什么。
        g.grabbed = false;
        g.forwarding = false;
        capture::resetState();
        if (capture::evtInput) SetEvent(capture::evtInput);
        g.log(L"应急解锁 Ctrl+Alt+Q:已释放独占、停止转发");
        ShowWindow(h, SW_SHOW);
        SetForegroundWindow(h);
        ensureAnim();
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case MSG_BINDDONE:
        if (!g.bindWait) return 0;         // 只认第一次(防重复投递)
        g.hotkeyVk = (UINT)wp;
        g.bindWait = false;
        capture::resetHotkeyLatch();
        settings::save();
        g.log(L"收放热键已改为 " + keyName(g.hotkeyVk.load()));
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case MSG_LINKUPD: {
        // 链路掉线兜底:独占自动释放(安全阀)
        if (!link::txReady() && g.forwarding) {
            g.forwarding = false;
            g.grabbed = false;
            g.log(L"链路不可用,已自动停止转发并释放独占");
        }
        // kmboxNET 延迟起服:设置里开着但启动时被验证门禁压住 → 验证通过这里补起(幂等)
        if (g.verified && g.netEnable && !g.netUp) netserver::start();
        ensureAnim();   // 连接中黄点呼吸 / 验证通过锁定层淡出
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case MSG_LOG:
        if (IsWindowVisible(h)) InvalidateRect(h, nullptr, FALSE);
        return 0;
    case MSG_TASKBAR_COMMAND: {
        auto showMain = [&] {
            ShowWindow(h, SW_SHOW);
            ShowWindow(h, SW_RESTORE);
            SetForegroundWindow(h);
        };
        if (wp == TBC_SHOW) {
            showMain();
            return 0;
        }
        if (!g.verified) {
            g.log(L"任务栏控制岛：请先连接已授权的 P4 完成启动验证");
            showMain();
            return 0;
        }
        if (wp == TBC_FORWARD) {
            if (!link::txReady()) {
                g.log(L"任务栏控制岛：链路未就绪，无法开始转发");
                showMain();
            } else {
                onClick(HIT_FWD);
            }
        } else if (wp == TBC_EXCLUSIVE) {
            onClick(g.exclusive ? HIT_SHARE : HIT_EXCL);
        } else if (wp == TBC_TIER_DOWN || wp == TBC_TIER_UP) {
            int index = 0;
            for (int i = 0; i < 8; ++i) if (g.tier == TIER_VALS[i]) index = i;
            index += (wp == TBC_TIER_UP) ? 1 : -1;
            index = std::max(0, std::min(7, index));
            onClick(HIT_TIER0 + index);
        }
        taskbar_widget::updateConfig(g.tier, g.exclusive);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case MSG_TRAY:
        if (LOWORD(lp) == WM_LBUTTONDBLCLK) {
            ShowWindow(h, SW_SHOW);
            ShowWindow(h, SW_RESTORE);
            SetForegroundWindow(h);
        } else if (LOWORD(lp) == WM_RBUTTONUP) {
            trayMenu();
        }
        return 0;
    case WM_CLOSE:
        ShowWindow(h, SW_HIDE);
        return 0;
    case WM_DESTROY:
        settings::save();
        taskbar_widget::setEnabled(false);
        g.forwarding = false;
        g.grabbed = false;
        sender::releaseAll();
        g.logFileClose();
        Shell_NotifyIconW(NIM_DELETE, &s_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void makeFonts() {
    auto mk = [](int px, int weight, const wchar_t* face) {
        return CreateFontW(-S(px), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH, face);
    };
    fTitle = mk(16, FW_BOLD,   L"Microsoft YaHei UI");
    fText  = mk(14, FW_NORMAL, L"Microsoft YaHei UI");
    fSmall = mk(12, FW_NORMAL, L"Microsoft YaHei UI");
    fBig   = mk(17, FW_BOLD,   L"Microsoft YaHei UI");
    fStat  = mk(15, FW_BOLD,   L"Microsoft YaHei UI");
    fMono  = mk(11, FW_NORMAL, L"Consolas");
    fIcon  = mk(13, FW_NORMAL, L"Segoe MDL2 Assets");
    fLock  = mk(30, FW_NORMAL, L"Segoe MDL2 Assets");
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    // 单实例:已在跑就把老窗口带出来
    CreateMutexW(nullptr, TRUE, L"QInEXLinkSingleton");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND old = FindWindowW(L"QInEXLinkMain", nullptr);
        if (old) { ShowWindow(old, SW_SHOW); SetForegroundWindow(old); }
        return 0;
    }

    performance::ProcessResult perf = performance::EnableProcessHighPerformance();

    SetProcessDPIAware();
    {
        HDC dc = GetDC(nullptr);
        SC = GetDeviceCaps(dc, LOGPIXELSX) / 96.0f;
        ReleaseDC(nullptr, dc);
    }
    makeFonts();
    settings::load();
    s_pointerReleaseMessage = RegisterWindowMessageW(L"QInEX.Pointer.ReleaseLocalInput.v1");
    if (g.logSave) g.logFileOpen();   // 记住的开关:启动即落盘,本次会话全量在文件里
    if (perf.highPriority && perf.executionSpeed && perf.timerResolution) {
        g.log(L"后台性能保护已启用:High 优先级 + HighQoS + 后台 1ms 定时器");
    } else {
        std::wstring missing;
        if (!perf.highPriority) missing += L" High优先级";
        if (!perf.executionSpeed) missing += L" HighQoS";
        if (!perf.timerResolution) missing += L" 后台1ms定时器";
        g.log(L"⚠ 后台性能保护部分失败:" + missing +
              L"；请右键托盘查看“后台性能说明”并调整 Windows 电源/USB 设置");
    }

    // 程序图标(资源 IDI_APPICON=1);缺资源时回退系统默认,不影响运行
    HICON appIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    if (!appIcon) appIcon = LoadIconW(nullptr, IDI_APPLICATION);

    WNDCLASSW wc{};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"QInEXLinkMain";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = appIcon;
    RegisterClassW(&wc);

    int x = g.winX, y = g.winY;
    RECT wr{x, y, x + S(BASE_W), y + S(BASE_H)};
    if (x < -50000 || !MonitorFromRect(&wr, MONITOR_DEFAULTTONULL)) {
        // 首次启动,或保存的位置已不在任何显示器上(拔了扩展屏/改分辨率)→ 居中
        x = (GetSystemMetrics(SM_CXSCREEN) - S(BASE_W)) / 2;
        y = (GetSystemMetrics(SM_CYSCREEN) - S(BASE_H)) / 2;
    }
    g.hwnd = CreateWindowExW(0, wc.lpszClassName, L"QInEX-Link",
                             WS_POPUP | WS_MINIMIZEBOX,
                             x, y, S(BASE_W), S(BASE_H), nullptr, nullptr, inst, nullptr);
    if (g.hwnd && s_pointerReleaseMessage) {
        // Link 常以管理员身份运行，而 Pointer 通常按普通权限启动。显式允许这条
        // 已注册消息跨 UIPI 边界，确保返回设置和截图校准都能解除独占输入。
        CHANGEFILTERSTRUCT filter{sizeof(filter)};
        if (!ChangeWindowMessageFilterEx(g.hwnd, s_pointerReleaseMessage, MSGFLT_ALLOW, &filter))
            g.log(L"⚠ 未能开放 Pointer 校准消息；混合权限运行时请让两个程序使用相同权限");
    }
    SendMessageW(g.hwnd, WM_SETICON, ICON_BIG,   (LPARAM)appIcon);   // 任务栏/Alt-Tab
    SendMessageW(g.hwnd, WM_SETICON, ICON_SMALL, (LPARAM)appIcon);

    s_nid.cbSize = sizeof(s_nid);
    s_nid.hWnd = g.hwnd;
    s_nid.uID = 1;
    s_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    s_nid.uCallbackMessage = MSG_TRAY;
    s_nid.hIcon = appIcon;
    wcscpy(s_nid.szTip, L"QInEX-Link 上位机直驱");
    Shell_NotifyIconW(NIM_ADD, &s_nid);

    taskbar_widget::start(inst, g.taskbarWidget, g.tier, g.exclusive);

    capture::start();
    sender::start();
    // netserver 不在这里起:即使设置里开着,也等启动验证通过(MSG_LINKUPD)再补起——
    // 未验证时不对外开任何输入入口
    g.log(L"QInEX-Link 就绪。请连接已授权的 P4 完成启动验证");

    // 滑块初始位对齐已加载设置(避免开窗瞬间从 0 位滑过去)
    s_segPos  = g.exclusive ? 1.f : 0.f;
    s_knobPos = g.netEnable ? 1.f : 0.f;

    if (!AnimateWindow(g.hwnd, 200, AW_BLEND))   // 入场淡入;失败(远程桌面等)退回直显
        ShowWindow(g.hwnd, SW_SHOW);
    SetTimer(g.hwnd, 1, 1000, nullptr);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    taskbar_widget::stop();
    netserver::stop();
    sender::stop();
    link::shutdown();
    capture::stop();
    return 0;
}
