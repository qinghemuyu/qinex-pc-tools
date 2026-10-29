#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>

#include "capture_output.h"
#include "custom_cursor.h"
#include "pointer_core.h"

namespace qpointer {

// 一次呈现使用同一份画面、几何和指针状态，保证视频与热点走同一坐标变换。
struct CapturePresentation {
    std::shared_ptr<const CaptureFrame> frame;
    COLORREF backgroundColor = RGB(0, 0, 0);
    Geometry geometry;
    PointerState pointer;
    bool showPointer = false;
    int pointerShape = 0;
    int pointerSize = 28;
    COLORREF pointerColor = RGB(0, 205, 230);
    std::shared_ptr<const CustomCursorImage> customCursorImage;
    double customCursorHotspotX = 0.5;
    double customCursorHotspotY = 0.5;
    bool showHint = false;
    std::wstring hintText;
    // 调用方仅在内容变化时构造新字符串；高频 Submit 共享不可变文本。
    bool showHud = false;
    std::shared_ptr<const std::wstring> hudStatusText;
    std::shared_ptr<const std::wstring> hudShortcutText;
    bool showRangeEditor = false;
    bool shadeRangeOutside = true;
    Rect rangeRect;
    std::wstring rangeHint;
};

struct CapturePresenterStatus {
    bool active = false;
    bool hardwareDevice = false;
    bool tearingSupported = false;
    bool tearingActive = false;
    uint64_t presents = 0;
    uint64_t videoFrames = 0;
    uint64_t coalescedVideoFrames = 0;
    // 最近约一秒成功 Present 的实测速率，不代表采集输入帧率。
    double measuredPresentFps = 0;
    // 仅计首次 Present 的新采集序号；同一视频帧上的光标/HUD 重绘不计入。
    double measuredVideoFps = 0;
    double lastSubmitToPresentMs = -1;
    double peakSubmitToPresentMs = 0;
    double lastCaptureToPresentMs = -1;
    double peakCaptureToPresentMs = 0;
    std::wstring backend;
    std::wstring message;
};

// D3D11/DXGI flip-model 呈现器。所有图形对象只在内部线程使用；Submit 只替换最新状态。
class CapturePresenter {
public:
    CapturePresenter();
    ~CapturePresenter();
    CapturePresenter(const CapturePresenter&) = delete;
    CapturePresenter& operator=(const CapturePresenter&) = delete;

    // lowLatencyTearing=true 时尽量立即扫描输出；系统或显示路径不支持时自动使用同步模式。
    bool Start(HWND window, bool lowLatencyTearing, std::wstring& error);
    void Stop();
    bool Active() const;
    void Submit(CapturePresentation presentation);
    void OnResize();
    CapturePresenterStatus Status() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}  // namespace qpointer
