#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "output_hud.h"

#include <d2d1_1.h>
#include <d3d11.h>
#include <dwrite.h>
#include <dxgi1_5.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>

#include "capture_presenter.h"

namespace qpointer {
namespace {
using Microsoft::WRL::ComPtr;

std::wstring HResultMessage(const wchar_t* operation, HRESULT result) {
    wchar_t code[16]{};
    swprintf_s(code, L"0x%08X", static_cast<unsigned>(result));
    return std::wstring(operation) + L"（" + code + L"）";
}

double QpcMilliseconds(int64_t begin, int64_t end) {
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    if (frequency.QuadPart <= 0 || end < begin) return 0;
    return static_cast<double>(end - begin) * 1000.0 / static_cast<double>(frequency.QuadPart);
}

D2D1_COLOR_F D2DColor(COLORREF value, float alpha = 1.0f) {
    return D2D1::ColorF(GetRValue(value) / 255.0f, GetGValue(value) / 255.0f,
        GetBValue(value) / 255.0f, alpha);
}

bool ValidFrame(const std::shared_ptr<const CaptureFrame>& frame) {
    return frame && frame->width >= 2 && frame->height >= 2 && frame->stride >= frame->width * 4 &&
        frame->bgra.size() >= static_cast<size_t>(frame->stride) * static_cast<size_t>(frame->height);
}
}  // namespace

struct CapturePresenter::Impl {
    HWND window{};
    bool requestTearing = false;
    std::thread worker;
    mutable std::mutex mutex;
    std::condition_variable changed;
    bool stopping = true;
    bool wake = false;
    bool resize = false;
    bool initialized = false;
    bool initSucceeded = false;
    uint64_t generation = 0;
    int64_t submittedQpc = 0;
    CapturePresentation latest;
    CapturePresenterStatus status;

    ComPtr<ID3D11Device> d3dDevice;
    ComPtr<ID3D11DeviceContext> d3dContext;
    ComPtr<IDXGISwapChain1> swapChain;
    ComPtr<ID2D1Factory1> d2dFactory;
    ComPtr<ID2D1Device> d2dDevice;
    ComPtr<ID2D1DeviceContext> d2dContext;
    ComPtr<ID2D1Bitmap1> targetBitmap;
    ComPtr<ID2D1Bitmap1> videoBitmap;
    ComPtr<ID2D1Bitmap1> customCursorBitmap;
    ComPtr<ID2D1StrokeStyle> roundStrokeStyle;
    ComPtr<ID2D1SolidColorBrush> pointerAccentBrush;
    ComPtr<ID2D1SolidColorBrush> pointerDarkBrush;
    ComPtr<ID2D1SolidColorBrush> whiteBrush;
    ComPtr<ID2D1SolidColorBrush> pointerPressedBrush;
    ComPtr<ID2D1SolidColorBrush> hintPanelBrush;
    ComPtr<ID2D1SolidColorBrush> hintTextBrush;
    ComPtr<ID2D1SolidColorBrush> hudPanelBrush;
    ComPtr<ID2D1SolidColorBrush> hudTextBrush;
    ComPtr<ID2D1SolidColorBrush> rangeShadeBrush;
    ComPtr<ID2D1SolidColorBrush> rangeAccentBrush;
    ComPtr<ID2D1SolidColorBrush> rangePanelBrush;
    ComPtr<ID2D1SolidColorBrush> rangeTextBrush;
    COLORREF pointerAccentColor = CLR_INVALID;
    std::shared_ptr<const CustomCursorImage> customCursorIdentity;
    ComPtr<IDWriteFactory> writeFactory;
    ComPtr<IDWriteTextFormat> hintFormat;
    ComPtr<IDWriteTextFormat> hudFormat;
    ComPtr<IDWriteInlineObject> hudEllipsis;
    HANDLE frameLatencyWaitable = nullptr;
    UINT outputWidth = 0;
    UINT outputHeight = 0;
    int bitmapWidth = 0;
    int bitmapHeight = 0;
    uint64_t uploadedSerial = 0;
    uint64_t lastSubmittedVideoSerial = 0;

    void SetFailure(const std::wstring& message) {
        std::lock_guard<std::mutex> lock(mutex);
        status.active = false;
        status.message = message;
    }

    HRESULT CreateDevice(bool warp) {
        constexpr D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1
        };
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        D3D_FEATURE_LEVEL level{};
        HRESULT result = D3D11CreateDevice(nullptr,
            warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
            levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
            &d3dDevice, &level, &d3dContext);
        if (result == E_INVALIDARG) {
            result = D3D11CreateDevice(nullptr,
                warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                levels + 1, static_cast<UINT>(std::size(levels) - 1), D3D11_SDK_VERSION,
                &d3dDevice, &level, &d3dContext);
        }
        return result;
    }

    HRESULT CreateTarget() {
        ComPtr<IDXGISurface> surface;
        HRESULT result = swapChain->GetBuffer(0, IID_PPV_ARGS(&surface));
        if (FAILED(result)) return result;
        const auto properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        result = d2dContext->CreateBitmapFromDxgiSurface(surface.Get(), &properties, &targetBitmap);
        if (SUCCEEDED(result)) d2dContext->SetTarget(targetBitmap.Get());
        return result;
    }

    HRESULT ResizeTarget(UINT width, UINT height) {
        width = std::max<UINT>(1, width);
        height = std::max<UINT>(1, height);
        if (targetBitmap && width == outputWidth && height == outputHeight) return S_OK;
        d2dContext->SetTarget(nullptr);
        targetBitmap.Reset();
        HRESULT result = swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN,
            DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
            (status.tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0));
        if (FAILED(result)) return result;
        outputWidth = width;
        outputHeight = height;
        return CreateTarget();
    }

    HRESULT CreateCachedBrushes() {
        HRESULT result = S_OK;
        const auto create = [&](const D2D1_COLOR_F& color,
                ComPtr<ID2D1SolidColorBrush>& brush) {
            result = d2dContext->CreateSolidColorBrush(color, &brush);
            return SUCCEEDED(result);
        };
        pointerAccentColor = RGB(0, 205, 230);
        if (!create(D2DColor(pointerAccentColor), pointerAccentBrush) ||
            !create(D2DColor(RGB(0, 0, 0), 190.0f / 255.0f), pointerDarkBrush) ||
            !create(D2DColor(RGB(255, 255, 255)), whiteBrush) ||
            !create(D2DColor(RGB(255, 155, 45), 88.0f / 255.0f), pointerPressedBrush) ||
            !create(D2D1::ColorF(22 / 255.0f, 27 / 255.0f, 32 / 255.0f, .88f),
                hintPanelBrush) ||
            !create(D2DColor(RGB(245, 248, 251)), hintTextBrush) ||
            !create(D2D1::ColorF(9 / 255.0f, 14 / 255.0f, 18 / 255.0f, .76f),
                hudPanelBrush) ||
            !create(D2DColor(RGB(238, 244, 248)), hudTextBrush) ||
            !create(D2D1::ColorF(0, 0, 0, .48f), rangeShadeBrush) ||
            !create(D2DColor(RGB(0, 220, 235)), rangeAccentBrush) ||
            !create(D2D1::ColorF(9 / 255.0f, 18 / 255.0f, 22 / 255.0f, .92f),
                rangePanelBrush) ||
            !create(D2DColor(RGB(245, 250, 252)), rangeTextBrush)) return result;
        return S_OK;
    }

    HRESULT InitializeGraphics() {
        bool hardware = true;
        HRESULT result = CreateDevice(false);
        if (FAILED(result)) {
            d3dDevice.Reset(); d3dContext.Reset(); hardware = false;
            result = CreateDevice(true);
        }
        if (FAILED(result)) return result;

        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory2> factory;
        if (FAILED(result = d3dDevice.As(&dxgiDevice)) ||
            FAILED(result = dxgiDevice->GetAdapter(&adapter)) ||
            FAILED(result = adapter->GetParent(IID_PPV_ARGS(&factory)))) return result;

        BOOL allowTearing = FALSE;
        ComPtr<IDXGIFactory5> factory5;
        if (SUCCEEDED(factory.As(&factory5))) {
            factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                &allowTearing, sizeof(allowTearing));
        }
        RECT client{};
        GetClientRect(window, &client);
        outputWidth = std::max<LONG>(1, client.right - client.left);
        outputHeight = std::max<LONG>(1, client.bottom - client.top);
        DXGI_SWAP_CHAIN_DESC1 description{};
        description.Width = outputWidth;
        description.Height = outputHeight;
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.BufferCount = 2;
        description.Scaling = DXGI_SCALING_STRETCH;
        description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        description.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
            (allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
        result = factory->CreateSwapChainForHwnd(d3dDevice.Get(), window, &description,
            nullptr, nullptr, &swapChain);
        if (FAILED(result)) return result;
        factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
        ComPtr<IDXGISwapChain2> swapChain2;
        if (SUCCEEDED(swapChain.As(&swapChain2))) {
            swapChain2->SetMaximumFrameLatency(1);
            frameLatencyWaitable = swapChain2->GetFrameLatencyWaitableObject();
        }

        D2D1_FACTORY_OPTIONS factoryOptions{};
        result = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
            __uuidof(ID2D1Factory1), &factoryOptions,
            reinterpret_cast<void**>(d2dFactory.GetAddressOf()));
        if (FAILED(result) || FAILED(result = d2dFactory->CreateDevice(dxgiDevice.Get(), &d2dDevice)) ||
            FAILED(result = d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2dContext)) ||
            FAILED(result = CreateTarget())) return result;
        d2dContext->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        D2D1_STROKE_STYLE_PROPERTIES strokeProperties{};
        strokeProperties.startCap = D2D1_CAP_STYLE_ROUND;
        strokeProperties.endCap = D2D1_CAP_STYLE_ROUND;
        strokeProperties.dashCap = D2D1_CAP_STYLE_ROUND;
        strokeProperties.lineJoin = D2D1_LINE_JOIN_ROUND;
        strokeProperties.miterLimit = 10.0f;
        strokeProperties.dashStyle = D2D1_DASH_STYLE_SOLID;
        d2dFactory->CreateStrokeStyle(strokeProperties, nullptr, 0, &roundStrokeStyle);
        if (FAILED(result = CreateCachedBrushes())) return result;

        if (SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(writeFactory.GetAddressOf())))) {
            writeFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 18.0f, L"zh-CN", &hintFormat);
            if (hintFormat) {
                hintFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                hintFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }
            writeFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"zh-CN", &hudFormat);
            if (hudFormat) {
                hudFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                hudFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                hudFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                DWRITE_TRIMMING trimming{};
                trimming.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
                if (SUCCEEDED(writeFactory->CreateEllipsisTrimmingSign(hudFormat.Get(), &hudEllipsis)))
                    hudFormat->SetTrimming(&trimming, hudEllipsis.Get());
            }
        }

        std::lock_guard<std::mutex> lock(mutex);
        status.hardwareDevice = hardware;
        status.tearingSupported = allowTearing != FALSE;
        status.tearingActive = requestTearing && allowTearing != FALSE;
        status.backend = hardware ? L"D3D11 硬件 · DXGI Flip" : L"D3D11 WARP · DXGI Flip";
        status.message = requestTearing && !allowTearing
            ? L"当前系统不支持无撕裂限制的立即呈现，已使用自动同步。" : L"";
        return S_OK;
    }

    HRESULT UploadVideo(const std::shared_ptr<const CaptureFrame>& frame) {
        if (!ValidFrame(frame)) return E_INVALIDARG;
        if (!videoBitmap || bitmapWidth != frame->width || bitmapHeight != frame->height) {
            videoBitmap.Reset();
            const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
            HRESULT result = d2dContext->CreateBitmap(
                D2D1::SizeU(static_cast<UINT32>(frame->width), static_cast<UINT32>(frame->height)),
                nullptr, 0, &properties, &videoBitmap);
            if (FAILED(result)) return result;
            bitmapWidth = frame->width;
            bitmapHeight = frame->height;
        }
        HRESULT result = videoBitmap->CopyFromMemory(nullptr, frame->bgra.data(),
            static_cast<UINT32>(frame->stride));
        if (SUCCEEDED(result)) uploadedSerial = frame->serial;
        return result;
    }

    bool EnsureCustomCursorBitmap(const std::shared_ptr<const CustomCursorImage>& image) {
        if (customCursorIdentity.get() == image.get()) return customCursorBitmap != nullptr;
        customCursorIdentity = image;
        customCursorBitmap.Reset();
        if (!image || image->width <= 0 || image->height <= 0 ||
            image->width > std::numeric_limits<int>::max() / 4 ||
            image->stride < image->width * 4 ||
            static_cast<size_t>(image->height) >
                std::numeric_limits<size_t>::max() / static_cast<size_t>(image->stride)) return false;
        const size_t required = static_cast<size_t>(image->stride) *
            static_cast<size_t>(image->height);
        if (required > image->premultipliedBgra.size()) return false;
        const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                D2D1_ALPHA_MODE_PREMULTIPLIED));
        const HRESULT result = d2dContext->CreateBitmap(
            D2D1::SizeU(static_cast<UINT32>(image->width),
                static_cast<UINT32>(image->height)),
            image->premultipliedBgra.data(), static_cast<UINT32>(image->stride),
            &properties, &customCursorBitmap);
        return SUCCEEDED(result);
    }

    void DrawPointer(const CapturePresentation& presentation, UINT width, UINT height) {
        if (!presentation.showPointer || !presentation.pointer.hasPosition || !presentation.pointer.show) return;
        Geometry local = presentation.geometry;
        local.target = {0, 0, static_cast<double>(width), static_cast<double>(height)};
        local.frameWidth = presentation.frame ? presentation.frame->width : local.frameWidth;
        local.frameHeight = presentation.frame ? presentation.frame->height : local.frameHeight;
        auto mapped = MapPoint(local, presentation.pointer.position);
        if (!mapped) return;
        const float x = static_cast<float>(mapped->x);
        const float y = static_cast<float>(mapped->y);
        const float size = static_cast<float>(std::clamp(presentation.pointerSize, 12, 144));
        const float accentWidth = std::max(2.0f, size * .07f);
        const float outlineWidth = std::max(2.0f, size * .04f);
        if (!pointerAccentBrush || !pointerDarkBrush || !whiteBrush || !pointerPressedBrush)
            return;
        if (pointerAccentColor != presentation.pointerColor) {
            pointerAccentColor = presentation.pointerColor;
            pointerAccentBrush->SetColor(D2DColor(pointerAccentColor));
        }

        // 按下反馈独立于光标样式，先绘制半透明橙色圆，确保自定义图片也有反馈。
        if (presentation.pointer.down) {
            d2dContext->FillEllipse(
                {D2D1::Point2F(x, y), size * .42f, size * .42f},
                pointerPressedBrush.Get());
        }

        const auto drawCrosshair = [&] {
            const float gap = size * .15f;
            const float arm = size * .43f;
            d2dContext->DrawLine({x - arm, y}, {x - gap, y}, pointerAccentBrush.Get(), accentWidth,
                roundStrokeStyle.Get());
            d2dContext->DrawLine({x + gap, y}, {x + arm, y}, pointerAccentBrush.Get(), accentWidth,
                roundStrokeStyle.Get());
            d2dContext->DrawLine({x, y - arm}, {x, y - gap}, pointerAccentBrush.Get(), accentWidth,
                roundStrokeStyle.Get());
            d2dContext->DrawLine({x, y + gap}, {x, y + arm}, pointerAccentBrush.Get(), accentWidth,
                roundStrokeStyle.Get());
            d2dContext->DrawEllipse({{x, y}, size * .08f, size * .08f},
                pointerAccentBrush.Get(),
                accentWidth);
        };

        const auto drawDot = [&] {
            const D2D1_ELLIPSE circle{{x, y}, size * .18f, size * .18f};
            d2dContext->FillEllipse(circle, pointerAccentBrush.Get());
            d2dContext->DrawEllipse(circle, pointerDarkBrush.Get(), outlineWidth);
        };

        if (presentation.pointerShape == 0) {
            const float s = size / 48.0f;
            ComPtr<ID2D1PathGeometry> path;
            ComPtr<ID2D1GeometrySink> sink;
            if (FAILED(d2dFactory->CreatePathGeometry(&path)) || FAILED(path->Open(&sink))) return;
            sink->BeginFigure(D2D1::Point2F(x, y), D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine(D2D1::Point2F(x, y + 34 * s));
            sink->AddLine(D2D1::Point2F(x + 9 * s, y + 26 * s));
            sink->AddLine(D2D1::Point2F(x + 16 * s, y + 40 * s));
            sink->AddLine(D2D1::Point2F(x + 25 * s, y + 36 * s));
            sink->AddLine(D2D1::Point2F(x + 18 * s, y + 23 * s));
            sink->AddLine(D2D1::Point2F(x + 33 * s, y + 23 * s));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            if (SUCCEEDED(sink->Close())) {
                d2dContext->FillGeometry(path.Get(), whiteBrush.Get());
                d2dContext->DrawGeometry(path.Get(), pointerDarkBrush.Get(), outlineWidth,
                    roundStrokeStyle.Get());
            }
        } else if (presentation.pointerShape == 1) {
            drawDot();
        } else if (presentation.pointerShape == 2) {
            drawCrosshair();
        } else if (presentation.pointerShape == 3) {
            d2dContext->DrawEllipse({{x, y}, size * .30f, size * .30f},
                pointerAccentBrush.Get(),
                accentWidth);
            d2dContext->DrawEllipse({{x, y}, size * .06f, size * .06f},
                pointerAccentBrush.Get(),
                accentWidth);
        } else if (presentation.pointerShape == 4) {
            d2dContext->FillEllipse({{x, y}, size * .08f, size * .08f},
                pointerAccentBrush.Get());
            const float radius = size * (presentation.pointer.down ? .42f : .28f);
            d2dContext->DrawEllipse({{x, y}, radius, radius}, pointerAccentBrush.Get(),
                accentWidth);
        } else if (presentation.pointerShape == 5) {
            const float half = size * .32f;
            const float corner = size * .16f;
            const float left = x - half;
            const float right = x + half;
            const float top = y - half;
            const float bottom = y + half;
            d2dContext->DrawLine({left, top}, {left + corner, top},
                pointerAccentBrush.Get(), accentWidth,
                roundStrokeStyle.Get());
            d2dContext->DrawLine({left, top}, {left, top + corner},
                pointerAccentBrush.Get(), accentWidth,
                roundStrokeStyle.Get());
            d2dContext->DrawLine({right, top}, {right - corner, top},
                pointerAccentBrush.Get(), accentWidth,
                roundStrokeStyle.Get());
            d2dContext->DrawLine({right, top}, {right, top + corner},
                pointerAccentBrush.Get(), accentWidth,
                roundStrokeStyle.Get());
            d2dContext->DrawLine({left, bottom}, {left + corner, bottom},
                pointerAccentBrush.Get(), accentWidth,
                roundStrokeStyle.Get());
            d2dContext->DrawLine({left, bottom}, {left, bottom - corner},
                pointerAccentBrush.Get(), accentWidth,
                roundStrokeStyle.Get());
            d2dContext->DrawLine({right, bottom}, {right - corner, bottom},
                pointerAccentBrush.Get(), accentWidth,
                roundStrokeStyle.Get());
            d2dContext->DrawLine({right, bottom}, {right, bottom - corner},
                pointerAccentBrush.Get(), accentWidth,
                roundStrokeStyle.Get());
            d2dContext->DrawEllipse({{x, y}, size * .05f, size * .05f},
                pointerAccentBrush.Get(), accentWidth);
        } else if (presentation.pointerShape == 6) {
            for (int index = 0; index <= 3; ++index) {
                const float alpha = static_cast<float>(70 + index * 45) / 255.0f;
                pointerAccentBrush->SetOpacity(alpha);
                const float fx = static_cast<float>(index);
                d2dContext->DrawEllipse({{x - size * (.26f - fx * .07f),
                    y + size * (.18f - fx * .05f)},
                    size * (.05f + fx * .02f), size * (.05f + fx * .02f)},
                    pointerAccentBrush.Get(), accentWidth);
            }
            pointerAccentBrush->SetOpacity(1.0f);
            drawDot();
        } else if (presentation.pointerShape == 7) {
            if (EnsureCustomCursorBitmap(presentation.customCursorImage)) {
                const float imageWidth = static_cast<float>(presentation.customCursorImage->width);
                const float imageHeight = static_cast<float>(presentation.customCursorImage->height);
                const float scale = size / std::max(imageWidth, imageHeight);
                const float drawnWidth = imageWidth * scale;
                const float drawnHeight = imageHeight * scale;
                const float hotspotX = static_cast<float>(std::clamp(
                    presentation.customCursorHotspotX, 0.0, 1.0));
                const float hotspotY = static_cast<float>(std::clamp(
                    presentation.customCursorHotspotY, 0.0, 1.0));
                const float left = x - hotspotX * drawnWidth;
                const float top = y - hotspotY * drawnHeight;
                d2dContext->DrawBitmap(customCursorBitmap.Get(),
                    D2D1::RectF(left, top, left + drawnWidth, top + drawnHeight), 1.0f,
                    D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            } else {
                drawCrosshair();
            }
        } else {
            drawCrosshair();
        }
    }

    void DrawHint(const CapturePresentation& presentation, UINT width, UINT height,
                  float hudInset) {
        if (!presentation.showHint || presentation.hintText.empty() || !hintFormat ||
            !writeFactory || !hintPanelBrush || !hintTextBrush) return;
        const float bottom = std::max(21.0f, height - 18.0f - hudInset);
        const D2D1_RECT_F area = D2D1::RectF(20.0f, std::max(20.0f, bottom - 48.0f),
            std::max(21.0f, width - 20.0f), bottom);
        if (area.bottom <= area.top) return;
        d2dContext->FillRectangle(area, hintPanelBrush.Get());
        d2dContext->DrawTextW(presentation.hintText.c_str(),
            static_cast<UINT32>(presentation.hintText.size()), hintFormat.Get(), area,
            hintTextBrush.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    void DrawHud(const std::wstring* statusText, const std::wstring* shortcutText,
                 UINT width, UINT height) {
        if ((!statusText && !shortcutText) || !hudFormat || width < 32 || height < 18 ||
            !hudPanelBrush || !hudTextBrush) return;

        const float margin = std::min(12.0f, width / 4.0f);
        const float bottom = static_cast<float>(height) - std::min(8.0f, height / 6.0f);
        const int lineCount = OutputHudLineCount(statusText, shortcutText);
        const float top = std::max(0.0f, bottom - lineCount * 28.0f);
        const D2D1_RECT_F panelArea = D2D1::RectF(margin, top,
            std::max(margin + 1.0f, width - margin), bottom);
        d2dContext->FillRoundedRectangle(D2D1::RoundedRect(panelArea, 6, 6),
            hudPanelBrush.Get());
        const float textLeft = panelArea.left + 12.0f;
        const float textRight = std::max(textLeft + 1.0f, panelArea.right - 12.0f);
        ForEachOutputHudLine(statusText, shortcutText, [&](std::wstring_view text, int row) {
            const D2D1_RECT_F textArea = D2D1::RectF(textLeft, panelArea.top + row * 28.0f,
                textRight, std::min(panelArea.bottom, panelArea.top + (row + 1) * 28.0f));
            d2dContext->DrawTextW(text.data(), static_cast<UINT32>(text.size()), hudFormat.Get(),
                textArea, hudTextBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        });
    }

    void DrawRangeEditor(const CapturePresentation& presentation, UINT width, UINT height) {
        if (!presentation.showRangeEditor) return;
        const auto& r = presentation.rangeRect;
        const float left = static_cast<float>(std::clamp(r.x, 0.0, static_cast<double>(width)));
        const float top = static_cast<float>(std::clamp(r.y, 0.0, static_cast<double>(height)));
        const float right = static_cast<float>(std::clamp(r.x + r.w, 0.0, static_cast<double>(width)));
        const float bottom = static_cast<float>(std::clamp(r.y + r.h, 0.0, static_cast<double>(height)));
        if (right - left < 2 || bottom - top < 2) return;

        if (!rangeShadeBrush || !rangeAccentBrush || !whiteBrush) return;

        if (presentation.shadeRangeOutside) {
            d2dContext->FillRectangle(D2D1::RectF(0, 0, static_cast<float>(width), top),
                rangeShadeBrush.Get());
            d2dContext->FillRectangle(D2D1::RectF(0, bottom, static_cast<float>(width),
                static_cast<float>(height)), rangeShadeBrush.Get());
            d2dContext->FillRectangle(D2D1::RectF(0, top, left, bottom),
                rangeShadeBrush.Get());
            d2dContext->FillRectangle(D2D1::RectF(right, top,
                static_cast<float>(width), bottom), rangeShadeBrush.Get());
        }
        d2dContext->DrawRectangle(D2D1::RectF(left, top, right, bottom),
            rangeAccentBrush.Get(), 3.0f);
        constexpr float half = 6.0f;
        const D2D1_POINT_2F points[] = {
            {left, top}, {(left + right) * .5f, top}, {right, top},
            {left, (top + bottom) * .5f}, {right, (top + bottom) * .5f},
            {left, bottom}, {(left + right) * .5f, bottom}, {right, bottom}
        };
        for (const auto& point : points) {
            const auto box = D2D1::RectF(point.x - half, point.y - half, point.x + half, point.y + half);
            d2dContext->FillRectangle(box, whiteBrush.Get());
            d2dContext->DrawRectangle(box, rangeAccentBrush.Get(), 2.0f);
        }
        if (hintFormat && rangePanelBrush && rangeTextBrush && !presentation.rangeHint.empty()) {
            const float panelWidth = std::min<float>(720.0f, std::max<float>(320.0f, width - 40.0f));
            const D2D1_RECT_F area = D2D1::RectF((width - panelWidth) * .5f, 20.0f,
                (width + panelWidth) * .5f, 72.0f);
            d2dContext->FillRoundedRectangle(D2D1::RoundedRect(area, 8, 8),
                rangePanelBrush.Get());
            d2dContext->DrawTextW(presentation.rangeHint.c_str(),
                static_cast<UINT32>(presentation.rangeHint.size()), hintFormat.Get(), area,
                rangeTextBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }

    HRESULT Draw(const CapturePresentation& presentation) {
        RECT client{};
        if (!GetClientRect(window, &client)) return HRESULT_FROM_WIN32(GetLastError());
        const UINT width = static_cast<UINT>(std::max<LONG>(1, client.right - client.left));
        const UINT height = static_cast<UINT>(std::max<LONG>(1, client.bottom - client.top));
        HRESULT result = ResizeTarget(width, height);
        if (FAILED(result)) return result;
        if (ValidFrame(presentation.frame) &&
            (!videoBitmap || uploadedSerial != presentation.frame->serial ||
             bitmapWidth != presentation.frame->width || bitmapHeight != presentation.frame->height)) {
            result = UploadVideo(presentation.frame);
            if (FAILED(result)) return result;
        }

        d2dContext->BeginDraw();
        d2dContext->SetTransform(D2D1::Matrix3x2F::Identity());
        d2dContext->Clear(D2DColor(presentation.backgroundColor));
        if (videoBitmap && ValidFrame(presentation.frame)) {
            Geometry local = presentation.geometry;
            local.target = {0, 0, static_cast<double>(width), static_cast<double>(height)};
            local.frameWidth = presentation.frame->width;
            local.frameHeight = presentation.frame->height;
            Geometry imageGeometry = local;
            imageGeometry.content = local.crop;
            imageGeometry.calibration = {};
            const auto topLeft = MapPoint(imageGeometry, {0, 0});
            const auto topRight = MapPoint(imageGeometry, {1, 0});
            const auto bottomLeft = MapPoint(imageGeometry, {0, 1});
            if (topLeft && topRight && bottomLeft) {
                const float sourceX = static_cast<float>(local.crop.x * presentation.frame->width);
                const float sourceY = static_cast<float>(local.crop.y * presentation.frame->height);
                const float sourceWidth = static_cast<float>(local.crop.w * presentation.frame->width);
                const float sourceHeight = static_cast<float>(local.crop.h * presentation.frame->height);
                if (sourceWidth > 0 && sourceHeight > 0) {
                    const float m11 = static_cast<float>((topRight->x - topLeft->x) / sourceWidth);
                    const float m12 = static_cast<float>((topRight->y - topLeft->y) / sourceWidth);
                    const float m21 = static_cast<float>((bottomLeft->x - topLeft->x) / sourceHeight);
                    const float m22 = static_cast<float>((bottomLeft->y - topLeft->y) / sourceHeight);
                    const float dx = static_cast<float>(topLeft->x) - sourceX * m11 - sourceY * m21;
                    const float dy = static_cast<float>(topLeft->y) - sourceX * m12 - sourceY * m22;
                    d2dContext->SetTransform(D2D1::Matrix3x2F(m11, m12, m21, m22, dx, dy));
                    const D2D1_RECT_F source = D2D1::RectF(sourceX, sourceY,
                        sourceX + sourceWidth, sourceY + sourceHeight);
                    d2dContext->DrawBitmap(videoBitmap.Get(), source, 1.0f,
                        D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, source);
                }
            }
        }
        d2dContext->SetTransform(D2D1::Matrix3x2F::Identity());
        const std::wstring* hudStatus = presentation.showHud && presentation.hudStatusText &&
            !presentation.hudStatusText->empty() ? presentation.hudStatusText.get() : nullptr;
        const std::wstring* hudShortcut = presentation.showHud && presentation.hudShortcutText &&
            !presentation.hudShortcutText->empty() ? presentation.hudShortcutText.get() : nullptr;
        DrawHud(hudStatus, hudShortcut, width, height);
        DrawHint(presentation, width, height,
            OutputHudLineCount(hudStatus, hudShortcut) * 28.0f);
        DrawPointer(presentation, width, height);
        DrawRangeEditor(presentation, width, height);
        result = d2dContext->EndDraw();
        if (FAILED(result)) return result;
        const UINT presentFlags = status.tearingActive ? DXGI_PRESENT_ALLOW_TEARING : 0;
        return swapChain->Present(status.tearingActive ? 0 : 1, presentFlags);
    }

    void ReleaseGraphics() {
        d2dContext.Reset(); targetBitmap.Reset(); videoBitmap.Reset(); customCursorBitmap.Reset();
        roundStrokeStyle.Reset();
        pointerAccentBrush.Reset(); pointerDarkBrush.Reset(); whiteBrush.Reset();
        pointerPressedBrush.Reset(); hintPanelBrush.Reset(); hintTextBrush.Reset();
        hudPanelBrush.Reset(); hudTextBrush.Reset(); rangeShadeBrush.Reset();
        rangeAccentBrush.Reset(); rangePanelBrush.Reset(); rangeTextBrush.Reset();
        pointerAccentColor = CLR_INVALID;
        customCursorIdentity.reset(); hintFormat.Reset();
        hudFormat.Reset(); hudEllipsis.Reset();
        writeFactory.Reset(); d2dDevice.Reset(); d2dFactory.Reset(); swapChain.Reset();
        d3dContext.Reset(); d3dDevice.Reset(); frameLatencyWaitable = nullptr;
    }

    void Run() {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const HRESULT graphics = InitializeGraphics();
        {
            std::lock_guard<std::mutex> lock(mutex);
            initialized = true;
            initSucceeded = SUCCEEDED(graphics);
            status.active = initSucceeded;
            if (!initSucceeded) status.message = HResultMessage(L"初始化低延迟呈现失败", graphics);
        }
        changed.notify_all();
        if (FAILED(graphics)) {
            ReleaseGraphics();
            if (SUCCEEDED(com)) CoUninitialize();
            return;
        }

        LARGE_INTEGER presentRateWindow{};
        QueryPerformanceCounter(&presentRateWindow);
        uint64_t presentsInWindow = 0;
        uint64_t videoFramesInWindow = 0;
        while (true) {
            CapturePresentation presentation;
            int64_t submitQpc = 0;
            {
                std::unique_lock<std::mutex> lock(mutex);
                changed.wait(lock, [&] { return stopping || wake; });
                if (stopping) break;
            }
            if (frameLatencyWaitable) WaitForSingleObject(frameLatencyWaitable, 100);
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (stopping) break;
                // 等到交换链允许提交后再取得最新快照，让等待期间到达的视频帧和
                // 指针状态参与本次呈现，避免稳定多出一帧排队延迟。
                presentation = latest;
                submitQpc = submittedQpc;
                wake = false;
                resize = false;
            }
            const HRESULT draw = Draw(presentation);
            if (draw == DXGI_ERROR_DEVICE_REMOVED || draw == DXGI_ERROR_DEVICE_RESET || FAILED(draw)) {
                SetFailure(HResultMessage(L"低延迟呈现已中断", draw));
                break;
            }
            LARGE_INTEGER presented{};
            QueryPerformanceCounter(&presented);
            std::lock_guard<std::mutex> lock(mutex);
            ++status.presents;
            ++presentsInWindow;
            if (presentation.frame && presentation.frame->serial != 0 &&
                presentation.frame->serial != lastSubmittedVideoSerial) {
                ++status.videoFrames;
                ++videoFramesInWindow;
                lastSubmittedVideoSerial = presentation.frame->serial;
                // 采集延迟只描述新视频帧首次成功上屏。光标或 HUD 更新会重复
                // Present 同一帧，不能因此让“采集→呈现”指标随帧龄持续增长。
                if (presentation.frame->arrivalQpc) {
                    status.lastCaptureToPresentMs = QpcMilliseconds(
                        static_cast<int64_t>(presentation.frame->arrivalQpc), presented.QuadPart);
                    status.peakCaptureToPresentMs = std::max(
                        status.peakCaptureToPresentMs, status.lastCaptureToPresentMs);
                }
            }
            const double presentRateElapsedMs = QpcMilliseconds(
                presentRateWindow.QuadPart, presented.QuadPart);
            if (presentRateElapsedMs >= 1000.0) {
                status.measuredPresentFps = static_cast<double>(presentsInWindow) *
                    1000.0 / presentRateElapsedMs;
                status.measuredVideoFps = static_cast<double>(videoFramesInWindow) *
                    1000.0 / presentRateElapsedMs;
                presentRateWindow = presented;
                presentsInWindow = videoFramesInWindow = 0;
            }
            status.lastSubmitToPresentMs = QpcMilliseconds(submitQpc, presented.QuadPart);
            status.peakSubmitToPresentMs = std::max(status.peakSubmitToPresentMs, status.lastSubmitToPresentMs);
        }
        ReleaseGraphics();
        if (SUCCEEDED(com)) CoUninitialize();
    }
};

CapturePresenter::CapturePresenter() : impl(std::make_unique<Impl>()) {}

CapturePresenter::~CapturePresenter() {
    Stop();
}

bool CapturePresenter::Start(HWND window, bool lowLatencyTearing, std::wstring& error) {
    Stop();
    error.clear();
    if (!window || !IsWindow(window)) {
        error = L"低延迟呈现没有可用的输出窗口。";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        impl->window = window;
        impl->requestTearing = lowLatencyTearing;
        impl->stopping = false;
        impl->wake = false;
        impl->resize = false;
        impl->initialized = false;
        impl->initSucceeded = false;
        impl->generation = 0;
        impl->submittedQpc = 0;
        impl->latest = {};
        impl->status = {};
        impl->uploadedSerial = 0;
        impl->lastSubmittedVideoSerial = 0;
    }
    try {
        impl->worker = std::thread([this] { impl->Run(); });
    } catch (...) {
        std::lock_guard<std::mutex> lock(impl->mutex);
        impl->stopping = true;
        error = L"无法创建低延迟呈现线程。";
        return false;
    }
    std::unique_lock<std::mutex> lock(impl->mutex);
    if (!impl->changed.wait_for(lock, std::chrono::seconds(5), [&] { return impl->initialized; })) {
        lock.unlock(); Stop(); error = L"低延迟呈现初始化超时。"; return false;
    }
    if (!impl->initSucceeded) {
        error = impl->status.message;
        lock.unlock(); Stop(); return false;
    }
    return true;
}

void CapturePresenter::Stop() {
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        impl->stopping = true;
        impl->wake = true;
    }
    impl->changed.notify_all();
    if (impl->worker.joinable()) impl->worker.join();
    std::lock_guard<std::mutex> lock(impl->mutex);
    impl->window = nullptr;
    impl->status.active = false;
    impl->latest = {};
}

bool CapturePresenter::Active() const {
    std::lock_guard<std::mutex> lock(impl->mutex);
    return impl->status.active;
}

void CapturePresenter::Submit(CapturePresentation presentation) {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        if (!impl->status.active || impl->stopping) return;
        if (impl->wake && presentation.frame && impl->latest.frame &&
            presentation.frame->serial != impl->latest.frame->serial) {
            ++impl->status.coalescedVideoFrames;
        }
        impl->latest = std::move(presentation);
        impl->submittedQpc = now.QuadPart;
        ++impl->generation;
        impl->wake = true;
    }
    impl->changed.notify_one();
}

void CapturePresenter::OnResize() {
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        if (!impl->status.active || impl->stopping) return;
        impl->resize = true;
        impl->wake = true;
    }
    impl->changed.notify_one();
}

CapturePresenterStatus CapturePresenter::Status() const {
    std::lock_guard<std::mutex> lock(impl->mutex);
    return impl->status;
}

}  // namespace qpointer
