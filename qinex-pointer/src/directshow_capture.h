#pragma once

#include "capture_output.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace qpointer::directshow {

// DirectShow 的设备标识使用 moniker 显示名，DevicePath 用于与 Media Foundation
// 的符号链接合并。两者均由驱动提供，不能使用列表序号代替。
struct Device {
    std::wstring friendlyName;
    std::wstring stableId;
    std::wstring devicePath;
};

bool EnumerateDevices(std::vector<Device>& devices, std::wstring& error);
bool EnumerateModes(const std::wstring& stableId,
    std::vector<CaptureNativeMode>& modes, std::wstring& error);

struct FrameView {
    const std::uint8_t* bytes = nullptr;
    std::size_t length = 0;
    int width = 0;
    int height = 0;
    int stride = 0;
    bool bottomUp = false;
    std::int64_t sampleTimestamp100ns = 0;
    bool hasSampleTimestamp = false;
    std::uint64_t mailboxSkippedFrames = 0;
    std::uint64_t arrivalQpc = 0;
    CaptureNativeMode nativeMode;
};

using FrameCallback = std::function<bool(const FrameView&, std::wstring&)>;

enum class PumpResult {
    Idle,
    Frame,
    Failure
};

struct OpenOptions {
    std::wstring stableId;
    CaptureModePolicy modePolicy = CaptureModePolicy::Auto;
    // Exact 使用完整稳定键；Preferred 只取其中的宽高、刷新率与扫描方式。
    std::wstring exactModeKey;
    // 对筛选、排序后的原生候选循环偏移。首项已连接但不出帧时，调用方可递增后重开，
    // 让下一种 pin、像素格式或 AM_MEDIA_TYPE 变体先接受首帧验证。
    std::size_t candidateOffset = 0;
};

struct OpenResult {
    int width = 0;
    int height = 0;
    int stride = 0;
    double nominalFps = 0;
    CaptureNativeMode requestedMode;
    CaptureNativeMode actualMode;
    bool usedDriverDefault = false;
};

// 所有图表操作必须在创建实例的 COM 线程调用。视频回调由 DirectShow 工作线程触发，
// FrameView 只在回调期间有效，调用方应立即复制需要的数据。
class CaptureSession {
public:
    CaptureSession();
    ~CaptureSession();
    CaptureSession(const CaptureSession&) = delete;
    CaptureSession& operator=(const CaptureSession&) = delete;
    CaptureSession(CaptureSession&&) = delete;
    CaptureSession& operator=(CaptureSession&&) = delete;

    bool Open(const OpenOptions& options, FrameCallback callback,
        OpenResult& result, std::wstring& error);
    bool Run(std::wstring& error);
    bool WaitForFrame(unsigned timeoutMs) const;
    // 在创建图表的工作线程提取最新样本并调用 FrameCallback；采集线程只负责替换单槽样本。
    PumpResult Pump(std::wstring& error);
    // 返回 true 表示图表报告了设备丢失、流结束或不可恢复错误。
    bool PollFailure(std::wstring& error);
    void Close();
    bool IsOpen() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}  // namespace qpointer::directshow
