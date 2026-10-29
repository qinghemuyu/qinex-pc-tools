#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "capture_diagnostics.h"

namespace qpointer {

struct CaptureDevice {
    std::wstring friendlyName;
    // 后端中立的稳定身份。现有 Media Foundation 设备继续沿用原符号链接，保证旧配置可恢复。
    std::wstring symbolicLink;
    std::wstring mediaFoundationId;
    std::wstring directShowId;
    bool mediaFoundationAvailable = false;
    bool directShowAvailable = false;
};

enum class CaptureModePolicy {
    Auto = 0,
    // 保持所选宽高、刷新率和扫描方式；后端切换时允许采用不同原生像素格式。
    Preferred = 1,
    // 高级模式：宽高、刷新率、扫描方式和原生像素格式均须匹配。
    Exact = 2
};

// 采集设备原生报告的一项模式。FPS 始终保留原始有理数，避免把 59.94/119.88
// 等时序误写成整数。stableKey 不含枚举序号和显示名，可跨重启恢复精确模式。
struct CaptureNativeMode {
    unsigned width = 0;
    unsigned height = 0;
    std::uint32_t fpsNumerator = 0;
    std::uint32_t fpsDenominator = 1;
    GUID subtype{};
    std::wstring subtypeFourcc;
    std::wstring subtypeName;
    bool compressed = false;
    bool progressive = false;
    std::uint32_t interlaceMode = 0;
    std::wstring stableKey;

    double Fps() const noexcept;
};

enum class CaptureState {
    Stopped,
    Starting,
    Running,
    WaitingSignal,
    Reconnecting,
    Error
};

enum class CaptureBackend {
    None,
    MediaFoundation,
    DirectShow
};

struct CaptureStatus {
    CaptureState state = CaptureState::Stopped;
    std::wstring message;
    int width = 0;
    int height = 0;
    double nominalFps = 0;
    double measuredFps = 0;
    CaptureDiagnostics diagnostics;
    std::uint64_t frameSerial = 0;
    std::uint64_t lastFrameMs = 0;
    std::uint64_t reconnectCount = 0;
    CaptureModePolicy requestedPolicy = CaptureModePolicy::Auto;
    std::wstring requestedModeKey;
    CaptureNativeMode requestedMode;
    bool hasRequestedMode = false;
    CaptureNativeMode actualMode;
    bool hasActualMode = false;
    // Media Foundation 没有统一可靠的驱动丢帧计数。只有后端能证明口径时才置 true。
    std::uint64_t droppedFrames = 0;
    bool dropCountAvailable = false;
    CaptureBackend backend = CaptureBackend::None;
    bool fallbackUsed = false;
    std::uint64_t backendSwitchCount = 0;
    // 仅供诊断和画面页展示；主状态 message 保持简短、可操作。
    std::wstring mediaFoundationError;
    std::wstring directShowError;
};

enum class CaptureFrameStorage {
    CpuBgra32 = 0,
    // 为后续 D3D 零拷贝路径保留；当前兼容路径不会发布这种帧。
    GpuTexture = 1
};

// BGRA32、从左上角开始、逐行向下，stride 恒为 width * 4。
// 帧对象发布后不可变，UI 可在下一帧到达的同时安全绘制旧帧。
struct CaptureFrame {
    int width = 0;
    int height = 0;
    int stride = 0;
    std::uint64_t serial = 0;
    std::uint64_t capturedMs = 0;
    GUID nativeSubtype{};
    std::int64_t sampleTimestamp100ns = 0;
    bool hasSampleTimestamp = false;
    // 当前采集尝试中，入口单槽邮箱被后续样本覆盖的累计数量；不是驱动丢帧。
    std::uint64_t mailboxSkippedFrames = 0;
    std::uint64_t arrivalQpc = 0;
    CaptureFrameStorage storage = CaptureFrameStorage::CpuBgra32;
    std::vector<std::uint8_t> bgra;
};

struct CaptureOptions {
    std::wstring symbolicLink;
    std::wstring mediaFoundationId;
    std::wstring directShowId;
    // true 表示调用方已通过 EnumerateDevices 解析过两个后端入口；此时空 ID 就代表
    // 该入口确实不存在。false 保留旧调用方式：只传 symbolicLink 时仍尝试两种后端。
    bool backendIdsResolved = false;
    CaptureModePolicy modePolicy = CaptureModePolicy::Auto;
    std::wstring exactModeKey;
    // 调用方已经枚举到的全局目标。Auto 时应传入合并 MF/DirectShow 规格后
    // 选出的最佳项，两个后端会先尝试相同时序，再允许 Auto 兼容降级；
    // Preferred/Exact 时表示用户明确选择的规格。
    CaptureNativeMode requestedMode;
    bool hasRequestedMode = false;

    // 每次发布帧后向该窗口投递消息；0 表示由调用方定时刷新。
    // 消息只作刷新提示，画面仍通过 LatestFrame() 取得。
    HWND notifyWindow = nullptr;
    UINT notifyMessage = 0;

    // 部分采集卡建图和 HDMI 锁相需要数秒。首帧单独宽限，运行后的断流检测保持灵敏。
    unsigned firstFrameTimeoutMs = 5000;
    unsigned signalTimeoutMs = 1500;
    unsigned reconnectIntervalMs = 1000;
};

// Media Foundation 主采集 + DirectShow 自动兼容后端。所有设备 I/O 都在内部线程执行，
// UI 线程只读状态和最新帧。
// 同一实例可重复 Start/Stop；Stop 有界等待，异常驱动即使卡住也不会永久阻塞调用线程。
class CaptureOutput {
public:
    CaptureOutput();
    ~CaptureOutput();
    CaptureOutput(const CaptureOutput&) = delete;
    CaptureOutput& operator=(const CaptureOutput&) = delete;
    CaptureOutput(CaptureOutput&&) = delete;
    CaptureOutput& operator=(CaptureOutput&&) = delete;

    // 成功但没有设备时返回 true 且 devices 为空；失败时不保留旧列表。
    static bool EnumerateDevices(std::vector<CaptureDevice>& devices, std::wstring& error);

    // 同步枚举设备真实报告的原生模式。函数不使用 CaptureOutput 实例状态，可从后台线程
    // 调用，也可与其它设备的枚举并发；同一物理设备是否允许并发打开由其驱动决定。
    static bool EnumerateModes(const std::wstring& symbolicLink,
        std::vector<CaptureNativeMode>& modes, std::wstring& error);
    static bool EnumerateModes(const CaptureDevice& device,
        std::vector<CaptureNativeMode>& modes, std::wstring& error);

    // 以下各项是无设备 I/O 的纯逻辑，供 UI 分档与持久化复用。
    static unsigned RefreshRateBucket(std::uint32_t numerator, std::uint32_t denominator);
    static std::wstring BuildModeStableKey(const CaptureNativeMode& mode);
    // 兼容 Media Foundation 与 DirectShow 对 59.94 等帧率的不同有理数表达。
    static bool ModeMatchesStableKey(const CaptureNativeMode& mode,
        const std::wstring& stableKey);
    // 普通帧率档跨采集后端匹配时忽略原生像素格式，但不放宽宽高、刷新率或扫描方式。
    static bool ModeMatchesTimingStableKey(const CaptureNativeMode& mode,
        const std::wstring& stableKey);
    // 合并两个后端的规格；等价规格保留 preferredModes 中的条目，再按 Auto 规则排序。
    static void MergeModes(std::vector<CaptureNativeMode>& preferredModes,
        const std::vector<CaptureNativeMode>& additionalModes);
    // 返回 modes 下标；Auto 传入 modeKey 时优先选择相同时序，目标不存在才返回
    // 普通 Auto 最佳项。精确模式不存在或列表为空时返回 -1，绝不静默降档。
    static int SelectMode(const std::vector<CaptureNativeMode>& modes,
        CaptureModePolicy policy, const std::wstring& modeKey);
    // 设备不公开规格列表时，只有 Auto 可以交给驱动决定当前格式。
    static bool AllowsDriverDefault(CaptureModePolicy policy) noexcept;
    // 首帧使用独立宽限；收到首帧后只使用较短的断流超时。
    static unsigned FrameSilenceTimeoutMs(const CaptureOptions& options,
        bool receivedFirstFrame) noexcept;

    // Start 只等待工作线程建立，不等待采集卡出帧。打开结果由 Status() 异步报告。
    bool Start(const CaptureOptions& options, std::wstring& error);
    void Stop();

    CaptureStatus Status() const;
    bool Active() const;
    std::shared_ptr<const CaptureFrame> LatestFrame() const;

    // 收到 notifyMessage 后调用，允许工作线程投递下一次刷新提示。
    // 画面与状态始终以 LatestFrame()/Status() 的最新快照为准。
    void AcknowledgeNotification();

    // 在 client 中等比居中；未有帧时只清成背景色。返回实际视频矩形。
    RECT Paint(HDC dc, const RECT& client, COLORREF background = RGB(0, 0, 0)) const;
    static RECT FitRect(const RECT& client, int frameWidth, int frameHeight);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}  // namespace qpointer
