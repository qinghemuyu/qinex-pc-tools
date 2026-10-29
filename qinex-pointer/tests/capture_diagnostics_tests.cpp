#include "capture_output.h"
#include "output_hud.h"

#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
bool Near(double actual, double expected, double tolerance = .02) {
    return std::abs(actual - expected) <= tolerance;
}
qpointer::CaptureFrame Frame(int width = 128, int height = 72) {
    qpointer::CaptureFrame frame;
    frame.width = width; frame.height = height; frame.stride = width * 4;
    frame.bgra.resize(static_cast<std::size_t>(frame.stride) * height, 0);
    frame.hasSampleTimestamp = true;
    return frame;
}
void Fill(qpointer::CaptureFrame& frame, int value) {
    for (std::size_t offset = 0; offset < frame.bgra.size(); offset += 4) {
        frame.bgra[offset] = static_cast<std::uint8_t>(value);
        frame.bgra[offset + 1] = static_cast<std::uint8_t>(value >> 8);
        frame.bgra[offset + 2] = 30;
        frame.bgra[offset + 3] = static_cast<std::uint8_t>(offset + value);
    }
}
void CheckRates() {
    qpointer::CaptureDiagnosticsTracker tracker;
    auto frame = Frame();
    // 驱动以 200 FPS 交付、PTS 每次递增，但每秒只更新 60 次画面。
    for (int index = 0; index <= 200; ++index) {
        Fill(frame, index * 60 / 200);
        frame.sampleTimestamp100ns = index * 50000;
        tracker.Observe(frame, index * 5.0);
    }
    auto status = tracker.Snapshot();
    Check(status.ready && Near(status.deliveredFps, 200), "200 FPS 接收统计错误");
    Check(Near(status.changedFps, 60) && Near(status.identicalPercent, 70),
        "未识别 200 次交付中只有 60 次画面更新");
    Check(status.timestampRepeats == 0 && status.timestampRegressions == 0,
        "把正常递增 PTS 误报为重复时间戳");
    Check(Near(status.intervalP95Ms, 5) && Near(status.intervalMaxMs, 5),
        "高分辨率时钟间隔统计错误");

    tracker.Reset();
    for (int index = 0; index <= 240; ++index) {
        Fill(frame, index);
        frame.sampleTimestamp100ns = index * 41667;
        tracker.Observe(frame, index * (1000.0 / 240));
    }
    status = tracker.Snapshot();
    Check(Near(status.deliveredFps, 240) && Near(status.changedFps, 240) &&
        Near(status.identicalPercent, 0), "240 FPS 连续变化错误");

    tracker.Reset();
    Fill(frame, 12);
    for (int index = 0; index <= 200; ++index) {
        // RGB 不变，模拟驱动 alpha 填充字节变化。
        for (std::size_t offset = 3; offset < frame.bgra.size(); offset += 4)
            frame.bgra[offset] = static_cast<std::uint8_t>(index);
        frame.sampleTimestamp100ns = index * 50000;
        tracker.Observe(frame, index * 5.0);
    }
    status = tracker.Snapshot();
    Check(Near(status.deliveredFps, 200) && Near(status.changedFps, 0) &&
        Near(status.identicalPercent, 100), "alpha 填充字节不应影响内容检测");
    Check(Near(status.unchangedMs, 1000) && Near(status.longestUnchangedMs, 1000),
        "静止持续时间错误");
}
void CheckBoundaries() {
    qpointer::CaptureDiagnosticsTracker tracker;
    auto frame = Frame();
    tracker.Observe(frame, 0); // 0 是合法的样本时间戳
    tracker.Observe(frame, 5);
    frame.sampleTimestamp100ns = -1;
    tracker.Observe(frame, 10);
    Check(tracker.Snapshot().timestampRepeats == 1 &&
        tracker.Snapshot().timestampRegressions == 1, "PTS 重复/倒退未识别");
    frame.hasSampleTimestamp = false;
    tracker.Observe(frame, 15);
    tracker.Observe(frame, 20);
    Check(!tracker.Snapshot().timestampAvailable &&
        tracker.Snapshot().timestampRepeats == 1, "缺失 PTS 不应计为重复零时间戳");
    frame.hasSampleTimestamp = true;
    tracker.Observe(frame, 25);
    Check(tracker.Snapshot().timestampRegressions == 1, "跨缺失时间戳错误比较");
    frame.mailboxSkippedFrames = 3;
    tracker.Observe(frame, 30);
    Check(tracker.Snapshot().mailboxSkippedFrames == 3 &&
        Near(tracker.Snapshot().unchangedMs, 0), "入口跳帧不应跨缺口推断内容");
    frame.bgra.clear();
    tracker.Observe(frame, 35);
    Check(!tracker.Snapshot().contentAvailable, "不完整像素缓冲应明确不可用");
    frame = Frame(64, 36);
    tracker.Observe(frame, 40);
    Check(!tracker.Snapshot().ready && tracker.Snapshot().analyzedFrames == 1 &&
        tracker.Snapshot().timestampRepeats == 0, "格式变化没有重置分析基线");
    tracker.Observe(frame, 39);
    Check(tracker.Snapshot().analyzedFrames == 1, "时钟倒退没有重置分析基线");
    frame.nativeSubtype.Data1 = 123;
    tracker.Observe(frame, 45);
    Check(tracker.Snapshot().analyzedFrames == 1, "像素格式变化没有重置分析基线");
    tracker.Reset();
    Check(!tracker.Snapshot().ready && tracker.Snapshot().analyzedFrames == 0,
        "重连后保留了旧会话分析");
    // 统计窗口内一次卡顿需进入峰值；不能只展示平均值。
    for (int index = 0; index <= 180; ++index)
        tracker.Observe(frame, index * 5.0 + (index >= 100 ? 100 : 0));
    Check(Near(tracker.Snapshot().intervalP95Ms, 5) &&
        Near(tracker.Snapshot().intervalMaxMs, 105), "卡顿峰值统计错误");
    // 超出固定容量也要有界刷新，保持所有索引有效。
    tracker.Reset();
    for (int index = 0; index < 5000; ++index) tracker.Observe(frame, index * .01);
    Check(tracker.Snapshot().ready && Near(tracker.Snapshot().deliveredFps, 100000, 1),
        "统计窗口容量边界错误");
}
void CheckHudLines() {
    std::wstring status = L"接收 200 FPS · 变化≈60 FPS\n帧间隔 5 ms\n静止画面不代表假帧";
    std::wstring shortcuts = L"F7 HUD · F11 返回";
    Check(qpointer::OutputHudLineCount(&status, &shortcuts) == 4, "采集 HUD 行数错误");
    int seen = 0;
    qpointer::ForEachOutputHudLine(&status, &shortcuts, [&](std::wstring_view text, int row) {
        Check(row == seen++ && !text.empty(), "HUD 顺序或切片错误");
    });
    std::wstring fusion = L"融合 · 无采集链路";
    Check(qpointer::OutputHudLineCount(&fusion, &shortcuts) == 2, "融合 HUD 行数被改变");
    Check(qpointer::OutputHudLineCount(nullptr, nullptr) == 0, "空 HUD 行数错误");
}
void MeasureCost() {
    auto frame = Frame(3840, 2160);
    qpointer::CaptureDiagnosticsTracker tracker;
    constexpr int count = 4000;
    const auto start = std::chrono::steady_clock::now();
    for (int index = 0; index < count; ++index) tracker.Observe(frame, index * 5.0);
    const auto us = std::chrono::duration<double, std::micro>(
        std::chrono::steady_clock::now() - start).count() / count;
    Check(tracker.Snapshot().analyzedFrames == count, "性能采样没有执行");
    std::cout << "4K fixed-grid analysis average: " << us << " us/frame (cached buffer, local CPU only)\n";
}
}  // namespace

int main() {
    try {
        CheckRates(); CheckBoundaries(); CheckHudLines(); MeasureCost();
        std::cout << "Capture diagnostics behavior checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
