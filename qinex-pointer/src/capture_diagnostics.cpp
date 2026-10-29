#include "capture_diagnostics.h"
#include "capture_output.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace qpointer {

void CaptureDiagnosticsTracker::Reset() noexcept {
    status = {};
    width = height = 0;
    started = previousContent = previousTimestamp = false;
    previousMs = windowStartMs = unchangedSinceMs = 0;
    previousPts = 0;
    previousSkipped = 0;
    intervalCount = comparisons = identical = changed = 0;
}

void CaptureDiagnosticsTracker::Observe(const CaptureFrame& frame, double arrivalMs) noexcept {
    if (!std::isfinite(arrivalMs)) return;
    if (started && (frame.width != width || frame.height != height ||
            std::memcmp(nativeSubtype.data(), &frame.nativeSubtype, nativeSubtype.size()) != 0 ||
            arrivalMs < previousMs)) Reset();
    width = frame.width;
    height = frame.height;
    std::memcpy(nativeSubtype.data(), &frame.nativeSubtype, nativeSubtype.size());

    const bool validPixels = frame.storage == CaptureFrameStorage::CpuBgra32 &&
        width > 0 && height > 0 && frame.stride > 0 &&
        static_cast<std::size_t>(width) <= static_cast<std::size_t>(frame.stride) / 4 &&
        static_cast<std::size_t>(height) <=
            frame.bgra.size() / static_cast<std::size_t>(frame.stride);
    bool same = true;
    if (validPixels) {
        for (std::size_t row = 0; row < kRows; ++row) {
            const auto y = (2 * row + 1) * static_cast<std::size_t>(height) / (2 * kRows);
            const auto* line = frame.bgra.data() + y * static_cast<std::size_t>(frame.stride);
            for (std::size_t column = 0; column < kColumns; ++column) {
                const auto x = (2 * column + 1) * static_cast<std::size_t>(width) / (2 * kColumns);
                const auto* pixel = line + x * 4;
                // RGB32 的 alpha 可能是驱动未初始化的填充字节，不能参与画面变化判断。
                const std::uint32_t rgb = pixel[0] | (std::uint32_t{pixel[1]} << 8) |
                    (std::uint32_t{pixel[2]} << 16);
                const std::size_t index = row * kColumns + column;
                same = same && pixels[index] == rgb;
                pixels[index] = rgb;
            }
        }
        ++status.analyzedFrames;
    }
    status.contentAvailable = validPixels;
    status.timestampAvailable = frame.hasSampleTimestamp;
    if (previousTimestamp && frame.hasSampleTimestamp) {
        if (frame.sampleTimestamp100ns == previousPts) ++status.timestampRepeats;
        if (frame.sampleTimestamp100ns < previousPts) ++status.timestampRegressions;
    }
    previousTimestamp = frame.hasSampleTimestamp;
    previousPts = frame.sampleTimestamp100ns;
    const bool skipped = frame.mailboxSkippedFrames != previousSkipped;
    status.mailboxSkippedFrames = previousSkipped = frame.mailboxSkippedFrames;

    if (!started) {
        started = true;
        windowStartMs = unchangedSinceMs = arrivalMs;
    } else {
        intervals[intervalCount++] = arrivalMs - previousMs;
        if (validPixels && previousContent && !skipped) {
            ++comparisons;
            if (same) ++identical;
            else ++changed;
        }
        // 入口漏掉中间帧或像素不可读时，不推断它们的画面内容，也不跨缺口累计静止时长。
        if (!validPixels || !previousContent || !same || skipped)
            unchangedSinceMs = arrivalMs;
        status.unchangedMs = validPixels ? arrivalMs - unchangedSinceMs : 0;
        status.longestUnchangedMs = std::max(status.longestUnchangedMs, status.unchangedMs);
        const double elapsed = arrivalMs - windowStartMs;
        if (elapsed >= 1000.0 || intervalCount == intervals.size()) {
            if (elapsed > 0) {
                status.ready = true;
                status.deliveredFps = intervalCount * 1000.0 / elapsed;
                status.changedFps = changed * 1000.0 / elapsed;
                status.identicalPercent = comparisons ? identical * 100.0 / comparisons : 0;
                std::sort(intervals.begin(), intervals.begin() + intervalCount);
                const auto p95 = static_cast<std::size_t>(std::ceil(intervalCount * .95)) - 1;
                status.intervalP95Ms = intervals[p95];
                status.intervalMaxMs = intervals[intervalCount - 1];
            }
            windowStartMs = arrivalMs;
            intervalCount = comparisons = identical = changed = 0;
        }
    }
    previousContent = validPixels;
    previousMs = arrivalMs;
}

}  // namespace qpointer
