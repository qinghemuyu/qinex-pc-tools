#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace qpointer {

struct CaptureFrame;

// 采样来自叠加光标/HUD 前的 RGB 像素。变化率是估算值，不能据此判定源设备真实 FPS。
struct CaptureDiagnostics {
    bool ready = false;
    bool contentAvailable = false;
    bool timestampAvailable = false;
    double deliveredFps = 0;
    double changedFps = 0;
    double identicalPercent = 0;
    double unchangedMs = 0;
    double longestUnchangedMs = 0;
    double intervalP95Ms = 0;
    double intervalMaxMs = 0;
    std::uint64_t analyzedFrames = 0;
    std::uint64_t timestampRepeats = 0;
    std::uint64_t timestampRegressions = 0;
    std::uint64_t mailboxSkippedFrames = 0;
};

// 仅由采集工作线程调用；快照随 CaptureStatus 在现有锁内发布。
// 每帧固定读取至多 64×36 个 RGB 点，不扫描整图、不分配内存、不改变视频交付。
// 使用回调时的单调时钟计时，约每秒更新统计；重连/格式变化须 Reset。
class CaptureDiagnosticsTracker {
public:
    void Reset() noexcept;
    void Observe(const CaptureFrame& frame, double arrivalMs) noexcept;
    const CaptureDiagnostics& Snapshot() const noexcept { return status; }

private:
    static constexpr std::size_t kColumns = 64, kRows = 36;
    static constexpr std::size_t kIntervalCapacity = 2048;
    CaptureDiagnostics status;
    std::array<std::uint32_t, kColumns * kRows> pixels{};
    std::array<double, kIntervalCapacity> intervals{};
    std::array<std::uint8_t, 16> nativeSubtype{};
    int width = 0, height = 0;
    bool started = false, previousContent = false, previousTimestamp = false;
    double previousMs = 0, windowStartMs = 0, unchangedSinceMs = 0;
    std::int64_t previousPts = 0;
    std::uint64_t previousSkipped = 0;
    std::size_t intervalCount = 0, comparisons = 0, identical = 0, changed = 0;
};

}  // namespace qpointer
