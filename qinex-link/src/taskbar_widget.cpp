// QInEX-Link 任务栏控制岛。
// 窗口附着思路参考 MIT 项目 Taskbar-Lyrics，但本模块按 QInEX 状态/交互独立实现：
// https://github.com/mo-jinran/Taskbar-Lyrics
#include "taskbar_widget.h"

#include "app.h"
#include "capture.h"

#include <objbase.h>
#include <UIAutomation.h>
#include <d2d1.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace taskbar_widget {
namespace {

constexpr wchar_t kHostClass[] = L"QInEXLinkTaskbarHost";
constexpr wchar_t kWidgetClass[] = L"QInEXLinkTaskbarWidget";
constexpr UINT WM_APPLY = WM_APP + 1;
constexpr UINT_PTR TIMER_ATTACH = 1;
constexpr UINT_PTR TIMER_RENDER = 2;

enum WidgetHit {
    WH_NONE = 0,
    WH_SHOW,
    WH_DRAG,
};

std::thread s_thread;
std::atomic<HWND> s_host{nullptr};
std::atomic<bool> s_enabled{false};
std::atomic<bool> s_stopping{false};
std::atomic<int> s_tier{1000};
std::atomic<bool> s_exclusive{false};
HINSTANCE s_instance = nullptr;

struct Segment {
    LONG left = 0;
    LONG right = 0;
};

bool validRect(const RECT& r) {
    return r.right > r.left && r.bottom > r.top &&
           r.left > LONG_MIN / 2 && r.right < LONG_MAX / 2;
}

float clamp01(float value) {
    return std::max(0.0f, std::min(1.0f, value));
}

D2D1_COLOR_F color(unsigned rgb, float alpha = 1.0f) {
    return D2D1::ColorF(
        ((rgb >> 16) & 0xFF) / 255.0f,
        ((rgb >> 8) & 0xFF) / 255.0f,
        (rgb & 0xFF) / 255.0f,
        alpha);
}

std::wstring tierText(int tier) {
    if (tier == 0) return L"直通";
    if (tier >= 1000 && tier % 1000 == 0) return std::to_wstring(tier / 1000) + L"K";
    return std::to_wstring(tier);
}

std::wstring rateText(int value) {
    if (value >= 1000 && value % 1000 == 0)
        return std::to_wstring(value / 1000) + L"K";
    return std::to_wstring(std::max(0, value));
}

struct Snapshot {
    int state = LS_IDLE;
    bool verified = false;
    char mode = '?';
    bool usb = false;
    bool forwarding = false;
    bool grabbed = false;
    bool exclusive = false;
    int tier = 1000;
    int rttX10 = -1;
    int fps = 0;
    int dps = 0;
    int errors = 0;
    int dropped = 0;
};

Snapshot takeSnapshot() {
    Snapshot s;
    s.state = g.state.load();
    s.verified = g.verified.load();
    s.mode = g.boardMode.load();
    s.usb = g.usbMounted.load();
    s.forwarding = g.forwarding.load();
    s.grabbed = g.grabbed.load();
    s.exclusive = s_exclusive.load();
    s.tier = s_tier.load();
    s.rttX10 = g.rttX10.load();
    s.fps = (g.boardFps.load() + 2) / 5;
    s.dps = (g.boardDps.load() + 2) / 5;
    s.errors = g.uerrSession.load();
    s.dropped = g.boardUdrop.load();
    return s;
}

std::wstring titleFor(const Snapshot& s) {
    if (s.state == LS_IDLE) return L"未连接";
    if (s.state != LS_CONNECTED) return L"连接中";
    if (!s.verified) return L"等待验证";
    if (s.mode != 'C') return L"单机模式";
    if (!s.usb) return L"等待 USB 连接";
    if (s.grabbed) return L"独占中";
    if (s.forwarding) return L"转发中";
    return L"已就绪";
}

unsigned accentFor(const Snapshot& s) {
    if (s.errors > 0 || s.dropped > 0 || (s.state == LS_CONNECTED && s.mode != 'C')) return 0xFF5470;
    if (s.state == LS_IDLE) return 0x829088;
    if (s.state != LS_CONNECTED || !s.verified || !s.usb) return 0xF2BD5E;
    return 0x00FF88;
}

struct Layout {
    D2D1_RECT_F panel{};
    D2D1_RECT_F logo{};
    D2D1_RECT_F statusDot{};
    D2D1_RECT_F title{};
    D2D1_RECT_F tier{};
    D2D1_RECT_F metrics{};
};

bool pointIn(const D2D1_RECT_F& r, float x, float y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

Layout makeLayout(float width, float height) {
    Layout l;
    const float outerX = 3.0f;
    const float outerY = 5.0f;
    const float inner = 5.0f;
    l.panel = D2D1::RectF(outerX, outerY, std::max(outerX, width - outerX),
                         std::max(outerY, height - outerY));
    float logoSize = std::min(30.0f, std::max(24.0f, height - 18.0f));
    float top = (height - logoSize) * 0.5f;
    l.logo = D2D1::RectF(outerX + inner, top, outerX + inner + logoSize, top + logoSize);

    float contentLeft = l.logo.right + 6.0f;
    float contentRight = width - outerX - inner;
    if (contentRight < contentLeft) contentRight = contentLeft;
    float tierWidth = std::min(27.0f, std::max(0.0f, contentRight - contentLeft));
    l.tier = D2D1::RectF(contentRight - tierWidth, 7.5f, contentRight, 19.5f);
    float dotTop = 10.4f;
    l.statusDot = D2D1::RectF(contentLeft, dotTop, contentLeft + 4.2f, dotTop + 4.2f);
    l.title = D2D1::RectF(contentLeft + 7.5f, 5.0f,
                         std::max(contentLeft + 7.5f, l.tier.left - 4.0f), 21.0f);
    l.metrics = D2D1::RectF(contentLeft, 23.0f, contentRight,
                            std::max(23.0f, height - 9.0f));
    return l;
}

class Renderer {
public:
    bool create(HWND hwnd) {
        reset();
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        D3D_FEATURE_LEVEL level{};
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                       nullptr, 0, D3D11_SDK_VERSION, &d3d_, &level, nullptr);
        if (FAILED(hr)) {
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
                                   nullptr, 0, D3D11_SDK_VERSION, &d3d_, &level, nullptr);
        }
        if (FAILED(hr)) return false;
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                     IID_PPV_ARGS(&d2dFactory_)))) return false;
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                       reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf())))) return false;
        if (FAILED(d3d_.As(&dxgiDevice_))) return false;

        ComPtr<IDXGIAdapter> adapter;
        if (FAILED(dxgiDevice_->GetAdapter(&adapter))) return false;
        if (FAILED(adapter->GetParent(IID_PPV_ARGS(&dxgiFactory_)))) return false;

        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = 1;
        desc.Height = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        if (FAILED(dxgiFactory_->CreateSwapChainForComposition(d3d_.Get(), &desc, nullptr,
                                                                &swapChain_))) return false;
        if (FAILED(DCompositionCreateDevice(dxgiDevice_.Get(), IID_PPV_ARGS(&dcomp_)))) return false;
        if (FAILED(dcomp_->CreateTargetForHwnd(hwnd, FALSE, &target_))) return false;
        if (FAILED(dcomp_->CreateVisual(&visual_))) return false;
        if (FAILED(visual_->SetContent(swapChain_.Get()))) return false;
        if (FAILED(target_->SetRoot(visual_.Get()))) return false;

        if (FAILED(dwrite_->CreateTextFormat(L"Microsoft YaHei UI", nullptr,
                                             DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                             DWRITE_FONT_STYLE_NORMAL,
                                             DWRITE_FONT_STRETCH_NORMAL, 9.6f, L"zh-cn",
                                             &titleFormat_))) return false;
        if (FAILED(dwrite_->CreateTextFormat(L"Microsoft YaHei UI", nullptr,
                                             DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                             DWRITE_FONT_STYLE_NORMAL,
                                             DWRITE_FONT_STRETCH_NORMAL, 7.6f, L"zh-cn",
                                             &detailFormat_))) return false;
        if (FAILED(dwrite_->CreateTextFormat(L"Microsoft YaHei UI", nullptr,
                                             DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                             DWRITE_FONT_STYLE_NORMAL,
                                             DWRITE_FONT_STRETCH_NORMAL, 6.8f, L"zh-cn",
                                             &badgeFormat_))) return false;
        if (FAILED(dwrite_->CreateTextFormat(L"Segoe UI Symbol", nullptr,
                                             DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                             DWRITE_FONT_STYLE_NORMAL,
                                             DWRITE_FONT_STRETCH_NORMAL, 12.5f, L"zh-cn",
                                             &iconFormat_))) return false;
        titleFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        detailFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        titleFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        detailFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        badgeFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        badgeFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        iconFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        hwnd_ = hwnd;
        return true;
    }

    void reset() {
        brush_.Reset();
        target2d_.Reset();
        surface_.Reset();
        titleFormat_.Reset();
        detailFormat_.Reset();
        badgeFormat_.Reset();
        iconFormat_.Reset();
        visual_.Reset();
        target_.Reset();
        dcomp_.Reset();
        swapChain_.Reset();
        dxgiFactory_.Reset();
        dxgiDevice_.Reset();
        dwrite_.Reset();
        d2dFactory_.Reset();
        d3d_.Reset();
        hwnd_ = nullptr;
        width_ = height_ = 0;
    }

    bool resize(UINT width, UINT height) {
        if (!swapChain_ || width == 0 || height == 0) return false;
        if (width == width_ && height == height_ && target2d_) return true;
        brush_.Reset();
        target2d_.Reset();
        surface_.Reset();
        HRESULT hr = swapChain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(hr)) return false;
        if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&surface_)))) return false;
        float dpi = hwnd_ ? static_cast<float>(GetDpiForWindow(hwnd_)) : 96.0f;
        auto props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            dpi, dpi);
        if (FAILED(d2dFactory_->CreateDxgiSurfaceRenderTarget(surface_.Get(), &props,
                                                              &target2d_))) return false;
        target2d_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        if (FAILED(target2d_->CreateSolidColorBrush(color(0xFFFFFF), &brush_))) return false;
        width_ = width;
        height_ = height;
        return true;
    }

    void advance(bool) {}

    void paint(WidgetHit hover) {
        if (!target2d_ || !swapChain_ || !dcomp_) return;
        Snapshot s = takeSnapshot();
        std::wstring title = titleFor(s);

        D2D1_SIZE_F size = target2d_->GetSize();
        Layout layout = makeLayout(size.width, size.height);

        target2d_->BeginDraw();
        target2d_->Clear(D2D1::ColorF(0, 0.0f));
        setBrush(0x07100B, 0.94f);
        target2d_->FillRoundedRectangle(D2D1::RoundedRect(layout.panel, 8.0f, 8.0f), brush_.Get());
        setBrush(0x284036, 0.95f);
        target2d_->DrawRoundedRectangle(D2D1::RoundedRect(layout.panel, 8.0f, 8.0f), brush_.Get(), 0.8f);

        unsigned accent = accentFor(s);
        bool logoHot = hover == WH_SHOW;
        setBrush(logoHot ? 0x173A28 : 0x10281B, 1.0f);
        target2d_->FillRoundedRectangle(D2D1::RoundedRect(layout.logo, 6.0f, 6.0f), brush_.Get());
        setBrush(accent, 1.0f);
        target2d_->DrawRoundedRectangle(D2D1::RoundedRect(layout.logo, 6.0f, 6.0f),
                                        brush_.Get(), logoHot ? 1.5f : 1.0f);
        drawCentered(L"P4", layout.logo, iconFormat_.Get(), accent);

        D2D1_POINT_2F dotCenter{(layout.statusDot.left + layout.statusDot.right) * 0.5f,
                               (layout.statusDot.top + layout.statusDot.bottom) * 0.5f};
        setBrush(accent, 1.0f);
        target2d_->FillEllipse(D2D1::Ellipse(dotCenter,
                                             (layout.statusDot.right - layout.statusDot.left) * 0.5f,
                                             (layout.statusDot.bottom - layout.statusDot.top) * 0.5f),
                               brush_.Get());
        drawClipped(title, layout.title, titleFormat_.Get(), 0xEEF5F0, 1.0f);

        setBrush(accent, 0.12f);
        target2d_->FillRoundedRectangle(D2D1::RoundedRect(layout.tier, 4.0f, 4.0f), brush_.Get());
        setBrush(accent, 0.75f);
        target2d_->DrawRoundedRectangle(D2D1::RoundedRect(layout.tier, 4.0f, 4.0f),
                                        brush_.Get(), 0.7f);
        drawCentered(tierText(s.tier), layout.tier, badgeFormat_.Get(), accent);

        bool ready = s.state == LS_CONNECTED && s.verified && s.mode == 'C' && s.usb;
        if (ready) drawMetrics(s, layout.metrics);
        else drawConnectionPath(s, layout.metrics);

        HRESULT hr = target2d_->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET) {
            resize(width_, height_);
            return;
        }
        if (SUCCEEDED(hr)) {
            swapChain_->Present(1, 0);
            dcomp_->Commit();
        }
    }

    WidgetHit hitTest(float x, float y) const {
        if (!target2d_) return WH_NONE;
        D2D1_SIZE_F size = target2d_->GetSize();
        Layout layout = makeLayout(size.width, size.height);
        if (pointIn(layout.logo, x, y)) return WH_SHOW;
        if (pointIn(layout.panel, x, y)) return WH_DRAG;
        return WH_NONE;
    }

private:
    void setBrush(unsigned rgb, float alpha) {
        if (brush_) brush_->SetColor(color(rgb, clamp01(alpha)));
    }

    void drawMetricShell(const D2D1_RECT_F& rect) {
        setBrush(0x0D1813, 0.98f);
        target2d_->FillRoundedRectangle(D2D1::RoundedRect(rect, 4.0f, 4.0f), brush_.Get());
        setBrush(0x20352A, 0.92f);
        target2d_->DrawRoundedRectangle(D2D1::RoundedRect(rect, 4.0f, 4.0f),
                                        brush_.Get(), 0.6f);
    }

    void drawMetrics(const Snapshot& s, const D2D1_RECT_F& area) {
        float width = std::max(0.0f, area.right - area.left);
        float gap = 2.5f;
        if (width < 48.0f) return;
        float usable = width - gap * 2.0f;
        float rttWidth = usable * 0.29f;
        float syncWidth = usable * 0.45f;
        D2D1_RECT_F rtt = D2D1::RectF(area.left, area.top,
                                      area.left + rttWidth, area.bottom);
        D2D1_RECT_F sync = D2D1::RectF(rtt.right + gap, area.top,
                                       rtt.right + gap + syncWidth, area.bottom);
        D2D1_RECT_F health = D2D1::RectF(sync.right + gap, area.top,
                                         area.right, area.bottom);
        drawMetricShell(rtt);
        drawMetricShell(sync);
        drawMetricShell(health);

        unsigned latencyColor = 0x829088;
        if (s.rttX10 >= 0) latencyColor = s.rttX10 <= 20 ? 0x00FF88 :
                                            (s.rttX10 <= 50 ? 0xF2BD5E : 0xFF5470);
        float cy = (rtt.top + rtt.bottom) * 0.5f;
        float x = rtt.left + 4.0f;
        D2D1_POINT_2F wave[5] = {
            {x, cy}, {x + 3.0f, cy}, {x + 5.5f, cy - 3.2f},
            {x + 8.5f, cy + 3.0f}, {x + 12.0f, cy - 0.5f},
        };
        setBrush(latencyColor, 1.0f);
        for (int i = 0; i < 4; ++i)
            target2d_->DrawLine(wave[i], wave[i + 1], brush_.Get(), 1.2f);
        std::wstring rttText = L"--";
        if (s.rttX10 >= 0) {
            rttText = std::to_wstring(s.rttX10 / 10) + L"." +
                      std::to_wstring(s.rttX10 % 10) + L" ms";
        }
        drawClipped(rttText, D2D1::RectF(rtt.left + 18.0f, rtt.top,
                                         rtt.right - 1.5f, rtt.bottom),
                    detailFormat_.Get(), 0xD8E5DD, 1.0f);

        float trackLeft = sync.left + 4.0f;
        float trackWidth = std::min(20.0f, std::max(7.0f, syncWidth * 0.25f));
        int reference = s.tier > 0 ? s.tier : std::max(1, std::max(s.fps, s.dps));
        float fpsRatio = clamp01(static_cast<float>(s.fps) / reference);
        float dpsRatio = clamp01(static_cast<float>(s.dps) / reference);
        float y1 = sync.top + 4.0f;
        float y2 = sync.bottom - 5.5f;
        setBrush(0x294137, 1.0f);
        target2d_->FillRectangle(D2D1::RectF(trackLeft, y1, trackLeft + trackWidth, y1 + 2.0f),
                                 brush_.Get());
        target2d_->FillRectangle(D2D1::RectF(trackLeft, y2, trackLeft + trackWidth, y2 + 2.0f),
                                 brush_.Get());
        setBrush(0x00FF88, 1.0f);
        target2d_->FillRectangle(D2D1::RectF(trackLeft, y1,
                                             trackLeft + trackWidth * fpsRatio, y1 + 2.0f),
                                 brush_.Get());
        setBrush(0x63C7FF, 1.0f);
        target2d_->FillRectangle(D2D1::RectF(trackLeft, y2,
                                             trackLeft + trackWidth * dpsRatio, y2 + 2.0f),
                                 brush_.Get());
        drawClipped(rateText(s.fps) + L"/" + rateText(s.dps),
                    D2D1::RectF(trackLeft + trackWidth + 4.0f, sync.top,
                                sync.right - 1.5f, sync.bottom),
                    detailFormat_.Get(), 0xD8E5DD, 1.0f);

        int faults = std::max(0, s.errors) + std::max(0, s.dropped);
        unsigned healthColor = faults == 0 ? 0x00FF88 : 0xFF5470;
        D2D1_POINT_2F hc{health.left + 8.5f, (health.top + health.bottom) * 0.5f};
        setBrush(healthColor, 1.0f);
        target2d_->DrawEllipse(D2D1::Ellipse(hc, 4.2f, 4.2f), brush_.Get(), 1.0f);
        if (faults == 0) {
            target2d_->DrawLine(D2D1::Point2F(hc.x - 2.2f, hc.y),
                                D2D1::Point2F(hc.x - 0.4f, hc.y + 1.8f), brush_.Get(), 1.1f);
            target2d_->DrawLine(D2D1::Point2F(hc.x - 0.4f, hc.y + 1.8f),
                                D2D1::Point2F(hc.x + 2.7f, hc.y - 2.0f), brush_.Get(), 1.1f);
        }
        drawClipped(faults == 0 ? L"OK" : std::to_wstring(faults),
                    D2D1::RectF(health.left + 15.5f, health.top,
                                health.right - 1.5f, health.bottom),
                    detailFormat_.Get(), healthColor, 1.0f);
    }

    void drawConnectionPath(const Snapshot& s, const D2D1_RECT_F& area) {
        drawMetricShell(area);
        float width = std::max(0.0f, area.right - area.left);
        if (width < 48.0f) return;
        float centers[4] = {
            area.left + width * 0.125f,
            area.left + width * 0.375f,
            area.left + width * 0.625f,
            area.left + width * 0.875f,
        };
        const wchar_t* labels[4] = {L"PC", L"P4", L"USB", L"手机"};
        bool p4Ready = s.state == LS_CONNECTED && s.verified && s.mode == 'C';
        bool complete[4] = {true, p4Ready, p4Ready && s.usb, p4Ready && s.usb};
        int current = p4Ready ? 2 : 1;
        bool modeError = s.state == LS_CONNECTED && s.mode != 'C';
        float cy = (area.top + area.bottom) * 0.5f;

        for (int i = 0; i < 3; ++i) {
            unsigned lineColor = complete[i + 1] ? 0x00FF88 :
                                 (current == i + 1 ? (modeError ? 0xFF5470 : 0xF2BD5E) : 0x31453B);
            setBrush(lineColor, complete[i + 1] ? 0.85f : 0.65f);
            target2d_->DrawLine(D2D1::Point2F(centers[i] + 12.0f, cy),
                                D2D1::Point2F(centers[i + 1] - 12.0f, cy),
                                brush_.Get(), 1.0f);
        }

        float pulse = 0.72f + 0.28f * static_cast<float>(
            std::sin(static_cast<double>(GetTickCount64()) * 0.006));
        for (int i = 0; i < 4; ++i) {
            bool isCurrent = i == current && !complete[i];
            unsigned nodeColor = complete[i] ? 0x00FF88 :
                                 (isCurrent ? (modeError ? 0xFF5470 : 0xF2BD5E) : 0x697A71);
            float alpha = isCurrent ? pulse : (complete[i] ? 1.0f : 0.65f);
            D2D1_POINT_2F node{centers[i] - 9.5f, cy};
            setBrush(nodeColor, alpha);
            if (complete[i] || isCurrent)
                target2d_->FillEllipse(D2D1::Ellipse(node, 2.7f, 2.7f), brush_.Get());
            else
                target2d_->DrawEllipse(D2D1::Ellipse(node, 2.7f, 2.7f), brush_.Get(), 0.9f);
            drawCentered(labels[i], D2D1::RectF(centers[i] - 5.0f, area.top,
                                                 centers[i] + 17.0f, area.bottom),
                         badgeFormat_.Get(), complete[i] || isCurrent ? 0xDDE9E2 : 0x829088);
        }
    }

    void drawClipped(const std::wstring& text, D2D1_RECT_F rect, IDWriteTextFormat* format,
                     unsigned rgb, float alpha) {
        setBrush(rgb, alpha);
        target2d_->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format, rect,
                             brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP,
                             DWRITE_MEASURING_MODE_NATURAL);
    }

    void drawCentered(const std::wstring& text, D2D1_RECT_F rect, IDWriteTextFormat* format,
                      unsigned rgb) {
        setBrush(rgb, 1.0f);
        target2d_->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format, rect,
                             brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP,
                             DWRITE_MEASURING_MODE_NATURAL);
    }

    HWND hwnd_ = nullptr;
    UINT width_ = 0;
    UINT height_ = 0;

    ComPtr<ID3D11Device> d3d_;
    ComPtr<IDXGIDevice> dxgiDevice_;
    ComPtr<IDXGIFactory2> dxgiFactory_;
    ComPtr<IDXGISwapChain1> swapChain_;
    ComPtr<IDXGISurface> surface_;
    ComPtr<ID2D1Factory> d2dFactory_;
    ComPtr<ID2D1RenderTarget> target2d_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<IDWriteFactory> dwrite_;
    ComPtr<IDWriteTextFormat> titleFormat_;
    ComPtr<IDWriteTextFormat> detailFormat_;
    ComPtr<IDWriteTextFormat> badgeFormat_;
    ComPtr<IDWriteTextFormat> iconFormat_;
    ComPtr<IDCompositionDevice> dcomp_;
    ComPtr<IDCompositionTarget> target_;
    ComPtr<IDCompositionVisual> visual_;
};

struct Runtime {
    HWND host = nullptr;
    HWND widget = nullptr;
    HWND taskbar = nullptr;
    WidgetHit hover = WH_NONE;
    WidgetHit pressedHit = WH_NONE;
    bool tracking = false;
    bool floating = false;
    bool dragCandidate = false;
    bool dragging = false;
    bool attachFailureLogged = false;
    bool rendererFailureLogged = false;
    bool spaceFailureLogged = false;
    bool lastLayoutNoSpace = false;
    POINT dragStart{};
    RECT dragWindow{};
    RECT lastTaskbarFrame{};
    ULONGLONG lastSlowPaint = 0;
    Renderer renderer;
    ComPtr<IUIAutomation> automation;

    ComPtr<IUIAutomationCondition> condition(PROPERTYID property, const wchar_t* value) {
        if (!automation) return nullptr;
        VARIANT v{};
        VariantInit(&v);
        v.vt = VT_BSTR;
        v.bstrVal = SysAllocString(value);
        ComPtr<IUIAutomationCondition> result;
        automation->CreatePropertyCondition(property, v, &result);
        VariantClear(&v);
        return result;
    }

    bool queryTaskbarRects(HWND bar, RECT& frame, RECT& taskList, RECT& tray, RECT& widgets) {
        frame = taskList = tray = widgets = RECT{};
        if (!automation && FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr,
                                                   CLSCTX_INPROC_SERVER,
                                                   IID_PPV_ARGS(&automation)))) return false;
        ComPtr<IUIAutomationElement> barElement;
        if (FAILED(automation->ElementFromHandle(bar, &barElement)) || !barElement) return false;
        auto inputSiteCond = condition(UIA_ClassNamePropertyId, L"Windows.UI.Input.InputSite.WindowClass");
        ComPtr<IUIAutomationElement> root;
        if (FAILED(barElement->FindFirst(TreeScope_Children, inputSiteCond.Get(), &root)) || !root)
            return false;

        auto frameCond = condition(UIA_ClassNamePropertyId, L"Taskbar.TaskbarFrameAutomationPeer");
        ComPtr<IUIAutomationElement> frameElement;
        if (FAILED(root->FindFirst(TreeScope_Children, frameCond.Get(), &frameElement)) || !frameElement ||
            FAILED(frameElement->get_CurrentBoundingRectangle(&frame)) || !validRect(frame)) return false;

        auto startCond = condition(UIA_AutomationIdPropertyId, L"StartButton");
        auto taskCond = condition(UIA_ClassNamePropertyId, L"Taskbar.TaskListButtonAutomationPeer");
        ComPtr<IUIAutomationCondition> listCond;
        automation->CreateOrCondition(startCond.Get(), taskCond.Get(), &listCond);
        ComPtr<IUIAutomationElementArray> listElements;
        if (SUCCEEDED(root->FindAll(TreeScope_Descendants, listCond.Get(), &listElements)) && listElements) {
            int count = 0;
            listElements->get_Length(&count);
            for (int i = 0; i < count; ++i) {
                ComPtr<IUIAutomationElement> element;
                RECT r{};
                if (SUCCEEDED(listElements->GetElement(i, &element)) && element &&
                    SUCCEEDED(element->get_CurrentBoundingRectangle(&r)) && validRect(r)) {
                    if (!validRect(taskList)) taskList = r;
                    else {
                        taskList.left = std::min(taskList.left, r.left);
                        taskList.top = std::min(taskList.top, r.top);
                        taskList.right = std::max(taskList.right, r.right);
                        taskList.bottom = std::max(taskList.bottom, r.bottom);
                    }
                }
            }
        }

        // Windows 11 的托盘并不是一种 AutomationId：
        // - NotifyItemIcon：普通常驻图标（微信、蓝牙、Snipaste 等）
        // - SystemTrayIcon：输入法、网络、音量、电池、时钟等系统图标
        // 只统计后者会把整段普通托盘误判为空白，顶层 Hub 就会覆盖用户图标。
        auto notifyCond = condition(UIA_AutomationIdPropertyId, L"NotifyItemIcon");
        auto systemTrayCond = condition(UIA_AutomationIdPropertyId, L"SystemTrayIcon");
        ComPtr<IUIAutomationCondition> trayCond;
        automation->CreateOrCondition(notifyCond.Get(), systemTrayCond.Get(), &trayCond);
        ComPtr<IUIAutomationElementArray> trayElements;
        if (trayCond &&
            SUCCEEDED(root->FindAll(TreeScope_Descendants, trayCond.Get(), &trayElements)) &&
            trayElements) {
            int count = 0;
            trayElements->get_Length(&count);
            for (int i = 0; i < count; ++i) {
                ComPtr<IUIAutomationElement> element;
                RECT r{};
                if (SUCCEEDED(trayElements->GetElement(i, &element)) && element &&
                    SUCCEEDED(element->get_CurrentBoundingRectangle(&r)) && validRect(r)) {
                    if (!validRect(tray)) tray = r;
                    else {
                        tray.left = std::min(tray.left, r.left);
                        tray.top = std::min(tray.top, r.top);
                        tray.right = std::max(tray.right, r.right);
                        tray.bottom = std::max(tray.bottom, r.bottom);
                    }
                }
            }
        }

        auto widgetsCond = condition(UIA_AutomationIdPropertyId, L"WidgetsButton");
        ComPtr<IUIAutomationElement> widgetElement;
        if (SUCCEEDED(root->FindFirst(TreeScope_Descendants, widgetsCond.Get(), &widgetElement)) &&
            widgetElement) widgetElement->get_CurrentBoundingRectangle(&widgets);

        return validRect(frame) && validRect(taskList) && validRect(tray);
    }

    struct ForeignContext {
        Runtime* self;
        std::vector<RECT>* rects;
        RECT frame;
    };

    static BOOL CALLBACK collectForeign(HWND hwnd, LPARAM lp) {
        auto* data = reinterpret_cast<ForeignContext*>(lp);
        Runtime* self = data->self;
        if (!IsWindowVisible(hwnd) || hwnd == self->widget) return TRUE;
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        DWORD explorerPid = 0;
        if (self->taskbar) GetWindowThreadProcessId(self->taskbar, &explorerPid);
        if (pid == explorerPid || pid == GetCurrentProcessId()) return TRUE;
        LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        // 汽水 Hub 的关键特征：独立顶层、TOPMOST + TOOLWINDOW。只避让同类小组件，
        // 不把普通最大化窗口误判成任务栏占位。
        if (!(exStyle & WS_EX_TOPMOST) || !(exStyle & WS_EX_TOOLWINDOW)) return TRUE;
        RECT r{};
        if (!GetWindowRect(hwnd, &r) || !validRect(r)) return TRUE;
        LONG frameH = data->frame.bottom - data->frame.top;
        if (r.bottom <= data->frame.top - frameH || r.top >= data->frame.bottom + frameH)
            return TRUE;
        if (r.bottom - r.top > frameH * 3) return TRUE;
        data->rects->push_back(r);
        return TRUE;
    }

    std::vector<RECT> foreignRects(const RECT& frame) {
        std::vector<RECT> rects;
        ForeignContext data{this, &rects, frame};
        EnumWindows(collectForeign, reinterpret_cast<LPARAM>(&data));
        return rects;
    }

    static void subtract(std::vector<Segment>& segments, LONG cutLeft, LONG cutRight) {
        if (cutRight <= cutLeft) return;
        std::vector<Segment> next;
        for (const Segment& s : segments) {
            if (cutRight <= s.left || cutLeft >= s.right) {
                next.push_back(s);
                continue;
            }
            if (cutLeft > s.left) next.push_back({s.left, std::min(cutLeft, s.right)});
            if (cutRight < s.right) next.push_back({std::max(cutRight, s.left), s.right});
        }
        segments.swap(next);
    }

    bool layoutWidget() {
        lastLayoutNoSpace = false;
        if (!widget || !taskbar) return false;
        RECT frame{}, taskList{}, tray{}, widgets{};
        if (!queryTaskbarRects(taskbar, frame, taskList, tray, widgets)) return false;
        if ((frame.right - frame.left) < (frame.bottom - frame.top) * 3) return false;
        lastTaskbarFrame = frame;

        LONG margin = std::max<LONG>(4, (frame.bottom - frame.top) / 12);
        LONG leftStart = validRect(widgets) ? widgets.right : frame.left;
        std::vector<Segment> segments;
        if (taskList.left - leftStart > 0) segments.push_back({leftStart + margin, taskList.left - margin});
        if (tray.left - taskList.right > 0) segments.push_back({taskList.right + margin, tray.left - margin});

        for (const RECT& r : foreignRects(frame)) {
            if (r.bottom > frame.top && r.top < frame.bottom)
                subtract(segments, r.left - margin, r.right + margin);
        }
        segments.erase(std::remove_if(segments.begin(), segments.end(), [](const Segment& s) {
            return s.right - s.left < 210;
        }), segments.end());
        if (segments.empty()) {
            lastLayoutNoSpace = true;
            return false;
        }

        Segment best = *std::max_element(segments.begin(), segments.end(), [](const Segment& a, const Segment& b) {
            return (a.right - a.left) < (b.right - b.left);
        });
        LONG dpi = static_cast<LONG>(GetDpiForWindow(taskbar));
        LONG desired = MulDiv(240, dpi > 0 ? dpi : 96, 96);
        LONG width = std::min(desired, best.right - best.left);
        LONG xScreen = best.left + ((best.right - best.left) - width) / 2;
        LONG height = frame.bottom - frame.top;
        SetWindowPos(widget, HWND_TOPMOST, xScreen, frame.top, width, height,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        return true;
    }

    bool fullscreenCoversTaskbar() const {
        if (!taskbar || !IsWindow(taskbar)) return false;
        HWND fg = GetForegroundWindow();
        if (!fg || fg == g.hwnd || fg == taskbar || IsChild(taskbar, fg)) return false;
        RECT wr{};
        if (!GetWindowRect(fg, &wr)) return false;
        HMONITOR mon = MonitorFromWindow(taskbar, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{sizeof(mi)};
        if (!GetMonitorInfoW(mon, &mi)) return false;
        const int slack = 2;
        return wr.left <= mi.rcMonitor.left + slack && wr.top <= mi.rcMonitor.top + slack &&
               wr.right >= mi.rcMonitor.right - slack && wr.bottom >= mi.rcMonitor.bottom - slack;
    }

    void destroyWidget() {
        capture::setUiMouseBlocked(false);
        if (GetCapture() == widget) ReleaseCapture();
        if (widget && IsWindow(widget)) DestroyWindow(widget);
        widget = nullptr;
        taskbar = nullptr;
        hover = WH_NONE;
        pressedHit = WH_NONE;
        tracking = false;
        floating = false;
        dragCandidate = false;
        dragging = false;
        renderer.reset();
    }

    bool shouldDock(POINT cursor) const {
        RECT frame = lastTaskbarFrame;
        if (!validRect(frame) && taskbar) GetWindowRect(taskbar, &frame);
        if (!validRect(frame)) return false;
        LONG magnet = std::max<LONG>(24, frame.bottom - frame.top);
        return cursor.x >= frame.left - magnet && cursor.x <= frame.right + magnet &&
               cursor.y >= frame.top - magnet && cursor.y <= frame.bottom + magnet;
    }

    void ensureAttached() {
        if (!s_enabled.load()) {
            destroyWidget();
            return;
        }
        HWND bar = FindWindowW(L"Shell_TrayWnd", nullptr);
        if (bar && IsWindow(bar)) taskbar = bar;
        else taskbar = nullptr;

        if (!widget || !IsWindow(widget)) {
            LONG dpi = bar ? static_cast<LONG>(GetDpiForWindow(bar)) : 96;
            LONG width = MulDiv(240, dpi > 0 ? dpi : 96, 96);
            LONG height = MulDiv(48, dpi > 0 ? dpi : 96, 96);
            LONG screenW = GetSystemMetrics(SM_CXSCREEN);
            LONG screenH = GetSystemMetrics(SM_CYSCREEN);
            widget = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
                                         WS_EX_NOREDIRECTIONBITMAP,
                                     kWidgetClass, L"", WS_POPUP | WS_VISIBLE,
                                     (screenW - width) / 2, screenH - height,
                                     width, height, nullptr, nullptr, s_instance, this);
            if (!widget) {
                if (!attachFailureLogged) {
                    attachFailureLogged = true;
                    g.log(L"任务栏控制岛窗口创建失败，已安全停用显示（不影响转发）");
                }
                return;
            }
        }

        // 顶层窗口不依附 Explorer：Shell 重启/暂无任务栏时保持在原位置，恢复后再吸附。
        if (!taskbar) {
            SetWindowPos(widget, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            return;
        }
        if (!floating && !layoutWidget()) {
            if (lastLayoutNoSpace) {
                ShowWindow(widget, SW_HIDE);
                if (!spaceFailureLogged) {
                    spaceFailureLogged = true;
                    g.log(L"任务栏控制岛暂无可用空间，已自动折叠等待（会继续检测）");
                }
                return;
            }
            // Explorer 刚恢复而 UIA 树尚未就绪：保留原窗口，不闪退、不销毁。
        }
        spaceFailureLogged = false;
        bool visible = IsWindowVisible(taskbar) && !fullscreenCoversTaskbar();
        if (!visible) capture::setUiMouseBlocked(false);
        ShowWindow(widget, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
        if (visible) InvalidateRect(widget, nullptr, FALSE);
    }
};

Runtime* runtimeFrom(HWND hwnd) {
    return reinterpret_cast<Runtime*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

void postCommand(WidgetHit hit) {
    if (g.hwnd && hit == WH_SHOW)
        PostMessageW(g.hwnd, MSG_TASKBAR_COMMAND, TBC_SHOW, 0);
}

LRESULT CALLBACK widgetProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Runtime* rt = runtimeFrom(hwnd);
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        rt = static_cast<Runtime*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(rt));
    }
    if (!rt) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_CREATE:
        rt->widget = hwnd;
        if (!rt->renderer.create(hwnd)) {
            if (!rt->rendererFailureLogged) {
                rt->rendererFailureLogged = true;
                g.log(L"任务栏控制岛图形初始化失败，已安全停用显示（不影响转发）");
            }
            return -1;
        }
        SetTimer(hwnd, TIMER_RENDER, 50, nullptr);
        return 0;
    case WM_SIZE:
        rt->renderer.resize(LOWORD(lp), HIWORD(lp));
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd, &ps);
        rt->renderer.paint(rt->hover);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEACTIVATE:
        capture::setUiMouseBlocked(true);
        return MA_NOACTIVATE;
    case WM_LBUTTONDOWN: {
        float scale = GetDpiForWindow(hwnd) / 96.0f;
        if (scale <= 0.0f) scale = 1.0f;
        rt->pressedHit = rt->renderer.hitTest(GET_X_LPARAM(lp) / scale,
                                              GET_Y_LPARAM(lp) / scale);
        if (rt->pressedHit == WH_DRAG) {
            rt->dragCandidate = true;
            rt->dragging = false;
            GetCursorPos(&rt->dragStart);
            GetWindowRect(hwnd, &rt->dragWindow);
            SetCapture(hwnd);
        }
        capture::setUiMouseBlocked(true);
        return 0;
    }
    case WM_LBUTTONUP: {
        float scale = GetDpiForWindow(hwnd) / 96.0f;
        if (scale <= 0.0f) scale = 1.0f;
        WidgetHit hit = rt->renderer.hitTest(GET_X_LPARAM(lp) / scale,
                                             GET_Y_LPARAM(lp) / scale);
        bool wasCandidate = rt->dragCandidate;
        bool wasDragging = rt->dragging;
        WidgetHit pressed = rt->pressedHit;
        rt->dragCandidate = false;
        rt->dragging = false;
        rt->pressedHit = WH_NONE;
        if (GetCapture() == hwnd) ReleaseCapture();
        capture::setUiMouseBlocked(false); // 点击可能切入独占，不能把悬停门禁带进游戏
        if (wasCandidate) {
            if (wasDragging) {
                POINT cursor{};
                GetCursorPos(&cursor);
                if (rt->shouldDock(cursor)) {
                    rt->floating = false;
                    rt->layoutWidget();
                }
            }
        } else if (hit != WH_NONE && hit == pressed) {
            postCommand(hit);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        capture::setUiMouseBlocked(true);
        if (rt->dragCandidate && (wp & MK_LBUTTON)) {
            POINT cursor{};
            GetCursorPos(&cursor);
            LONG dx = cursor.x - rt->dragStart.x;
            LONG dy = cursor.y - rt->dragStart.y;
            if (!rt->dragging && (std::abs(dx) >= 4 || std::abs(dy) >= 4)) {
                rt->dragging = true;
                rt->floating = true;
            }
            if (rt->dragging) {
                SetWindowPos(hwnd, HWND_TOPMOST,
                             rt->dragWindow.left + dx, rt->dragWindow.top + dy,
                             0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
                return 0;
            }
        }
        if (!rt->tracking) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            rt->tracking = true;
        }
        float scale = GetDpiForWindow(hwnd) / 96.0f;
        if (scale <= 0.0f) scale = 1.0f;
        WidgetHit hit = rt->renderer.hitTest(GET_X_LPARAM(lp) / scale,
                                             GET_Y_LPARAM(lp) / scale);
        if (hit != rt->hover) {
            rt->hover = hit;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        capture::setUiMouseBlocked(rt->dragCandidate);
        rt->tracking = false;
        rt->hover = WH_NONE;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_SETCURSOR: {
        POINT p{};
        GetCursorPos(&p);
        ScreenToClient(hwnd, &p);
        float scale = GetDpiForWindow(hwnd) / 96.0f;
        if (scale <= 0.0f) scale = 1.0f;
        WidgetHit hit = rt->renderer.hitTest(p.x / scale, p.y / scale);
        SetCursor(LoadCursorW(nullptr,
                              hit == WH_SHOW ? IDC_HAND :
                              hit == WH_DRAG ? IDC_SIZEALL : IDC_ARROW));
        return TRUE;
    }
    case WM_CAPTURECHANGED:
        if (reinterpret_cast<HWND>(lp) != hwnd) {
            rt->dragCandidate = false;
            rt->dragging = false;
            rt->pressedHit = WH_NONE;
            capture::setUiMouseBlocked(false);
        }
        return 0;
    case WM_TIMER:
        if (wp == TIMER_RENDER && IsWindowVisible(hwnd)) {
            bool animate = !g.grabbed.load();
            rt->renderer.advance(animate);
            ULONGLONG now = GetTickCount64();
            // 真独占时冻结滚动，只低频刷新状态，避免游戏期间产生持续 GPU/UI 唤醒。
            if (animate || now - rt->lastSlowPaint >= 500) {
                rt->lastSlowPaint = now;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;
    case WM_DESTROY:
        capture::setUiMouseBlocked(false);
        KillTimer(hwnd, TIMER_RENDER);
        if (rt->widget == hwnd) rt->widget = nullptr;
        rt->renderer.reset();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK hostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Runtime* rt = runtimeFrom(hwnd);
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        rt = static_cast<Runtime*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(rt));
    }
    if (!rt) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_CREATE:
        rt->host = hwnd;
        s_host = hwnd;
        SetTimer(hwnd, TIMER_ATTACH, 2000, nullptr);
        PostMessageW(hwnd, s_stopping.load() ? WM_CLOSE : WM_APPLY, 0, 0);
        return 0;
    case WM_APPLY:
        rt->ensureAttached();
        return 0;
    case WM_TIMER:
        if (wp == TIMER_ATTACH) rt->ensureAttached();
        return 0;
    case WM_CLOSE:
        rt->destroyWidget();
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_ATTACH);
        s_host = nullptr;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void threadMain() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);
    Runtime runtime;

    WNDCLASSW hostClass{};
    hostClass.lpfnWndProc = hostProc;
    hostClass.hInstance = s_instance;
    hostClass.lpszClassName = kHostClass;
    RegisterClassW(&hostClass);

    WNDCLASSW widgetClass{};
    widgetClass.lpfnWndProc = widgetProc;
    widgetClass.hInstance = s_instance;
    widgetClass.lpszClassName = kWidgetClass;
    widgetClass.hCursor = LoadCursorW(nullptr, IDC_HAND);
    RegisterClassW(&widgetClass);

    HWND host = CreateWindowExW(0, kHostClass, L"", 0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr, s_instance, &runtime);
    if (host) {
        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    } else {
        g.log(L"任务栏控制岛线程启动失败，主功能继续运行");
    }

    runtime.destroyWidget();
    runtime.automation.Reset();
    UnregisterClassW(kWidgetClass, s_instance);
    UnregisterClassW(kHostClass, s_instance);
    if (SUCCEEDED(com)) CoUninitialize();
}

}  // namespace

void start(HINSTANCE instance, bool enabled, int tier, bool exclusive) {
    if (s_thread.joinable()) return;
    s_instance = instance;
    s_enabled = enabled;
    s_tier = tier;
    s_exclusive = exclusive;
    s_stopping = false;
    s_thread = std::thread(threadMain);
}

void stop() {
    if (!s_thread.joinable()) return;
    s_stopping = true;
    HWND host = s_host.load();
    if (host) PostMessageW(host, WM_CLOSE, 0, 0);
    else {
        DWORD tid = GetThreadId(static_cast<HANDLE>(s_thread.native_handle()));
        if (tid) PostThreadMessageW(tid, WM_QUIT, 0, 0);
    }
    s_thread.join();
    s_stopping = false;
}

void setEnabled(bool enabled) {
    s_enabled = enabled;
    HWND host = s_host.load();
    if (host) PostMessageW(host, WM_APPLY, 0, 0);
}

void updateConfig(int tier, bool exclusive) {
    s_tier = tier;
    s_exclusive = exclusive;
    HWND host = s_host.load();
    if (host) PostMessageW(host, WM_APPLY, 0, 0);
}

}  // namespace taskbar_widget
