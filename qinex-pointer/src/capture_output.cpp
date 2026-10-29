#include "capture_output.h"
#include "directshow_capture.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mfreadwrite.h>
#include <objbase.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <cwctype>
#include <limits>
#include <mutex>
#include <new>
#include <numeric>
#include <sstream>
#include <thread>
#include <utility>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace qpointer {

double CaptureNativeMode::Fps() const noexcept {
    return fpsDenominator ? static_cast<double>(fpsNumerator) / fpsDenominator : 0.0;
}

namespace {
using Microsoft::WRL::ComPtr;

constexpr DWORD kVideoStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
constexpr unsigned kStopWaitMs = 1200;
constexpr unsigned kMaximumFrameWidth = 8192;
constexpr unsigned kMaximumFrameHeight = 4320;

std::wstring HResultText(const wchar_t* operation, HRESULT result) {
    wchar_t* systemText = nullptr;
    const DWORD count = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
            FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(result), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t*>(&systemText), 0, nullptr);
    std::wostringstream text;
    text << operation;
    if (count && systemText) {
        std::wstring message(systemText, count);
        while (!message.empty() && iswspace(message.back())) message.pop_back();
        text << L"：" << message;
    }
    text << L"（0x" << std::hex << std::uppercase << static_cast<unsigned long>(result) << L"）";
    if (systemText) LocalFree(systemText);
    return text.str();
}

class CaptureApiScope {
public:
    bool StartCom(std::wstring& error) {
        if (comReady) return true;
        const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(apartment)) comStarted = true;
        else if (apartment != RPC_E_CHANGED_MODE) {
            error = HResultText(L"初始化 COM 失败", apartment);
            return false;
        }
        // RPC_E_CHANGED_MODE 表示本线程已由宿主按另一种 apartment 初始化；COM 仍可使用。
        comReady = true;
        return true;
    }

    bool StartMediaFoundation(std::wstring& error) {
        if (mfStarted) return true;
        if (!StartCom(error)) return false;
        const HRESULT result = MFStartup(MF_VERSION, MFSTARTUP_LITE);
        if (FAILED(result)) {
            error = HResultText(L"初始化 Media Foundation 失败", result);
            return false;
        }
        mfStarted = true;
        return true;
    }

    ~CaptureApiScope() {
        if (mfStarted) MFShutdown();
        if (comStarted) CoUninitialize();
    }

private:
    bool comReady = false;
    bool comStarted = false;
    bool mfStarted = false;
};

bool DeviceLess(const CaptureDevice& left, const CaptureDevice& right) {
    const int byName = CompareStringOrdinal(left.friendlyName.c_str(), -1,
        right.friendlyName.c_str(), -1, TRUE);
    if (byName != CSTR_EQUAL) return byName == CSTR_LESS_THAN;
    return CompareStringOrdinal(left.symbolicLink.c_str(), -1,
        right.symbolicLink.c_str(), -1, TRUE) == CSTR_LESS_THAN;
}

std::wstring DeviceHardwareKey(std::wstring path) {
    constexpr wchar_t pnpPrefix[] = L"@device:pnp:";
    if (path.compare(0, std::size(pnpPrefix) - 1, pnpPrefix) == 0)
        path.erase(0, std::size(pnpPrefix) - 1);
    const std::size_t interfaceClass = path.find(L"#{");
    if (interfaceClass != std::wstring::npos) path.resize(interfaceClass);
    while (!path.empty() && (path.back() == L'\\' || iswspace(path.back()))) path.pop_back();
    return path;
}

bool SameDevicePath(const std::wstring& left, const std::wstring& right) {
    if (left.empty() || right.empty()) return false;
    if (CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL)
        return true;
    const std::wstring leftKey = DeviceHardwareKey(left);
    const std::wstring rightKey = DeviceHardwareKey(right);
    return !leftKey.empty() && !rightKey.empty() &&
        CompareStringOrdinal(leftKey.c_str(), -1, rightKey.c_str(), -1, TRUE) == CSTR_EQUAL;
}

bool NotifyWindow(HWND window, UINT message, std::uint64_t serial, bool statusOnly) {
    if (!window || !message) return false;
    // PostMessage 不等待窗口线程；窗口销毁或消息队列已结束时直接失败即可。
    return PostMessageW(window, message,
        static_cast<WPARAM>(serial), statusOnly ? 1 : 0) != FALSE;
}

struct VideoMode {
    ComPtr<IMFMediaType> type;
    CaptureNativeMode description;
};

bool SameGuid(const GUID& left, const GUID& right) {
    return InlineIsEqualGUID(left, right) != FALSE;
}

std::wstring GuidText(const GUID& value) {
    wchar_t text[40]{};
    swprintf_s(text, L"{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
        value.Data1, value.Data2, value.Data3, value.Data4[0], value.Data4[1],
        value.Data4[2], value.Data4[3], value.Data4[4], value.Data4[5],
        value.Data4[6], value.Data4[7]);
    return text;
}

struct StableModeKey {
    unsigned width = 0;
    unsigned height = 0;
    std::uint32_t fpsNumerator = 0;
    std::uint32_t fpsDenominator = 1;
    GUID subtype{};
    std::uint32_t interlaceMode = 0;
};

bool ParseUnsigned(const std::wstring& text, std::size_t begin, std::size_t end,
    std::uint32_t& value) {
    if (begin >= end) return false;
    std::uint64_t parsed = 0;
    for (std::size_t index = begin; index < end; ++index) {
        if (text[index] < L'0' || text[index] > L'9') return false;
        parsed = parsed * 10 + static_cast<unsigned>(text[index] - L'0');
        if (parsed > std::numeric_limits<std::uint32_t>::max()) return false;
    }
    value = static_cast<std::uint32_t>(parsed);
    return true;
}

bool ParseStableModeKey(const std::wstring& key, StableModeKey& parsed) {
    if (key.compare(0, 4, L"mf1|") != 0) return false;
    const std::size_t sizeEnd = key.find(L'|', 4);
    const std::size_t x = key.find(L'x', 4);
    if (sizeEnd == std::wstring::npos || x == std::wstring::npos || x >= sizeEnd ||
            !ParseUnsigned(key, 4, x, parsed.width) ||
            !ParseUnsigned(key, x + 1, sizeEnd, parsed.height)) return false;
    const std::size_t fpsEnd = key.find(L'|', sizeEnd + 1);
    const std::size_t slash = key.find(L'/', sizeEnd + 1);
    if (fpsEnd == std::wstring::npos || slash == std::wstring::npos || slash >= fpsEnd ||
            !ParseUnsigned(key, sizeEnd + 1, slash, parsed.fpsNumerator) ||
            !ParseUnsigned(key, slash + 1, fpsEnd, parsed.fpsDenominator) ||
            parsed.fpsDenominator == 0) return false;
    const std::size_t guidEnd = key.find(L'|', fpsEnd + 1);
    if (guidEnd == std::wstring::npos) return false;
    const std::wstring guidText = key.substr(fpsEnd + 1, guidEnd - fpsEnd - 1);
    if (FAILED(CLSIDFromString(guidText.c_str(), &parsed.subtype))) return false;
    if (guidEnd + 2 >= key.size() || key[guidEnd + 1] != L'i' ||
            !ParseUnsigned(key, guidEnd + 2, key.size(), parsed.interlaceMode)) return false;
    return true;
}

bool IsFourccSubtype(const GUID& value) {
    static constexpr BYTE suffix[8]{0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
    return value.Data2 == 0 && value.Data3 == 0x0010 &&
        std::memcmp(value.Data4, suffix, sizeof(suffix)) == 0;
}

std::wstring FourccText(const GUID& subtype) {
    if (SameGuid(subtype, MFVideoFormat_H264_ES)) return L"H264_ES";
    if (!IsFourccSubtype(subtype)) return L"";
    wchar_t value[5]{
        static_cast<wchar_t>(subtype.Data1 & 0xff),
        static_cast<wchar_t>((subtype.Data1 >> 8) & 0xff),
        static_cast<wchar_t>((subtype.Data1 >> 16) & 0xff),
        static_cast<wchar_t>((subtype.Data1 >> 24) & 0xff), 0};
    for (int index = 0; index < 4; ++index) {
        if (value[index] < 0x20 || value[index] > 0x7e) return L"";
    }
    return value;
}

std::wstring SubtypeName(const GUID& subtype, const std::wstring& fourcc) {
    if (SameGuid(subtype, MFVideoFormat_RGB32)) return L"RGB32";
    if (SameGuid(subtype, MFVideoFormat_ARGB32)) return L"ARGB32";
    if (SameGuid(subtype, MFVideoFormat_NV12)) return L"NV12";
    if (SameGuid(subtype, MFVideoFormat_YUY2)) return L"YUY2";
    if (SameGuid(subtype, MFVideoFormat_UYVY)) return L"UYVY";
    if (SameGuid(subtype, MFVideoFormat_MJPG)) return L"Motion JPEG";
    if (SameGuid(subtype, MFVideoFormat_H264) || SameGuid(subtype, MFVideoFormat_H264_ES)) return L"H.264";
    if (SameGuid(subtype, MFVideoFormat_HEVC) || SameGuid(subtype, MFVideoFormat_HEVC_ES)) return L"HEVC";
    if (SameGuid(subtype, MFVideoFormat_AV1)) return L"AV1";
    return fourcc.empty() ? GuidText(subtype) : fourcc;
}

bool KnownCompressedSubtype(const GUID& subtype) {
    return SameGuid(subtype, MFVideoFormat_MJPG) || SameGuid(subtype, MFVideoFormat_H264) ||
        SameGuid(subtype, MFVideoFormat_H264_ES) || SameGuid(subtype, MFVideoFormat_HEVC) ||
        SameGuid(subtype, MFVideoFormat_HEVC_ES) || SameGuid(subtype, MFVideoFormat_VP80) ||
        SameGuid(subtype, MFVideoFormat_VP90) || SameGuid(subtype, MFVideoFormat_AV1);
}

bool ReadMode(IMFMediaType* type, VideoMode& mode) {
    GUID major{};
    if (!type || FAILED(type->GetGUID(MF_MT_MAJOR_TYPE, &major)) || major != MFMediaType_Video) return false;
    auto& description = mode.description;
    if (FAILED(MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &description.width, &description.height)) ||
        description.width == 0 || description.height == 0 ||
        description.width > 16384 || description.height > 16384) return false;
    if (FAILED(MFGetAttributeRatio(type, MF_MT_FRAME_RATE,
            &description.fpsNumerator, &description.fpsDenominator)) ||
        description.fpsNumerator == 0 || description.fpsDenominator == 0) {
        // 缺失的时序保持未知，不能虚构为 30 Hz。
        description.fpsNumerator = 0;
        description.fpsDenominator = 1;
    }
    if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &description.subtype))) return false;
    UINT32 interlace = MFVideoInterlace_Unknown;
    type->GetUINT32(MF_MT_INTERLACE_MODE, &interlace);
    description.interlaceMode = interlace;
    description.progressive = interlace == MFVideoInterlace_Progressive;
    UINT32 compressed = 0;
    description.compressed = KnownCompressedSubtype(description.subtype) ||
        (SUCCEEDED(type->GetUINT32(MF_MT_COMPRESSED, &compressed)) && compressed != 0);
    description.subtypeFourcc = FourccText(description.subtype);
    description.subtypeName = SubtypeName(description.subtype, description.subtypeFourcc);
    description.stableKey = CaptureOutput::BuildModeStableKey(description);
    mode.type = type;
    return true;
}

int FormatPreference(const CaptureNativeMode& mode) {
    if (SameGuid(mode.subtype, MFVideoFormat_NV12)) return 5;
    if (SameGuid(mode.subtype, MFVideoFormat_YUY2) || SameGuid(mode.subtype, MFVideoFormat_UYVY)) return 4;
    if (SameGuid(mode.subtype, MFVideoFormat_RGB32) || SameGuid(mode.subtype, MFVideoFormat_ARGB32)) return 3;
    if (SameGuid(mode.subtype, MFVideoFormat_MJPG)) return 2;
    if (mode.compressed) return 0;
    return 1;
}

enum class ScanKind {
    Interlaced,
    Unknown,
    Progressive
};

ScanKind ClassifyScan(std::uint32_t interlaceMode, bool progressive) {
    if (interlaceMode == MFVideoInterlace_Progressive) return ScanKind::Progressive;
    switch (interlaceMode) {
    case MFVideoInterlace_FieldInterleavedUpperFirst:
    case MFVideoInterlace_FieldInterleavedLowerFirst:
    case MFVideoInterlace_FieldSingleUpper:
    case MFVideoInterlace_FieldSingleLower:
        return ScanKind::Interlaced;
    default:
        // Unknown 和 MixedInterlaceOrProgressive 都不能被当成明确隔行。
        return progressive ? ScanKind::Progressive : ScanKind::Unknown;
    }
}

ScanKind ClassifyScan(const CaptureNativeMode& mode) {
    return ClassifyScan(mode.interlaceMode, mode.progressive);
}

bool TimingScanCompatible(std::uint32_t actualInterlace, bool actualProgressive,
    std::uint32_t expectedInterlace) {
    if (actualInterlace == expectedInterlace) return true;
    const ScanKind actual = ClassifyScan(actualInterlace, actualProgressive);
    const ScanKind expected = ClassifyScan(expectedInterlace,
        expectedInterlace == MFVideoInterlace_Progressive);
    if (actual == expected) return actual == ScanKind::Unknown;
    // 后端未报告扫描方式时允许与明确逐行互认；明确逐行和明确隔行始终不兼容。
    return (actual == ScanKind::Unknown && expected == ScanKind::Progressive) ||
        (actual == ScanKind::Progressive && expected == ScanKind::Unknown);
}

bool AutoModeBetter(const CaptureNativeMode& left, const CaptureNativeMode& right) {
    const ScanKind leftScan = ClassifyScan(left);
    const ScanKind rightScan = ClassifyScan(right);
    // 明确隔行放在所有逐行/未知扫描模式之后。Unknown 不能冒充隔行，和 Progressive
    // 比较时先看刷新率，避免 240 Hz 未知扫描被 60 Hz 逐行错误压下去。
    const bool leftExplicitInterlaced = leftScan == ScanKind::Interlaced;
    const bool rightExplicitInterlaced = rightScan == ScanKind::Interlaced;
    if (leftExplicitInterlaced != rightExplicitInterlaced) return !leftExplicitInterlaced;
    const unsigned leftBucket = CaptureOutput::RefreshRateBucket(
        left.fpsNumerator, left.fpsDenominator);
    const unsigned rightBucket = CaptureOutput::RefreshRateBucket(
        right.fpsNumerator, right.fpsDenominator);
    if (leftBucket != rightBucket) return leftBucket > rightBucket;
    if (std::abs(left.Fps() - right.Fps()) > 0.001) return left.Fps() > right.Fps();
    const std::uint64_t leftPixels = static_cast<std::uint64_t>(left.width) * left.height;
    const std::uint64_t rightPixels = static_cast<std::uint64_t>(right.width) * right.height;
    if (leftPixels != rightPixels) return leftPixels > rightPixels;
    if (leftScan != rightScan) return leftScan == ScanKind::Progressive;
    if (left.compressed != right.compressed) return !left.compressed;
    const int leftFormat = FormatPreference(left), rightFormat = FormatPreference(right);
    if (leftFormat != rightFormat) return leftFormat > rightFormat;
    return left.stableKey < right.stableKey;
}

bool ModeLess(const VideoMode& left, const VideoMode& right) {
    return AutoModeBetter(left.description, right.description);
}

bool AppendMode(IMFMediaType* type, std::vector<VideoMode>& modes) {
    VideoMode mode;
    if (!ReadMode(type, mode)) return false;
    const bool duplicate = std::any_of(modes.begin(), modes.end(), [&](const VideoMode& existing) {
        return CompareStringOrdinal(existing.description.stableKey.c_str(), -1,
            mode.description.stableKey.c_str(), -1, TRUE) == CSTR_EQUAL;
    });
    if (!duplicate) modes.push_back(std::move(mode));
    return true;
}

void SortModes(std::vector<VideoMode>& modes) {
    std::stable_sort(modes.begin(), modes.end(), ModeLess);
}

bool CollectNativeModes(IMFSourceReader* reader, std::vector<VideoMode>& modes,
    std::wstring& error) {
    modes.clear();
    if (!reader) {
        error = L"视频读取器不可用。";
        return false;
    }
    for (DWORD index = 0;; ++index) {
        ComPtr<IMFMediaType> nativeType;
        const HRESULT result = reader->GetNativeMediaType(kVideoStream, index, &nativeType);
        if (result == MF_E_NO_MORE_TYPES) break;
        if (FAILED(result)) {
            if (modes.empty()) error = HResultText(L"读取采集设备视频模式失败", result);
            break;
        }
        AppendMode(nativeType.Get(), modes);
    }
    if (modes.empty()) {
        if (error.empty()) error = L"采集设备没有报告可用的视频格式。";
        return false;
    }
    SortModes(modes);
    return true;
}

// Microsoft 对视频采集格式的推荐枚举入口是媒体源的 Presentation Descriptor。
// 部分专用采集卡驱动可被 Source Reader 打开，却不会通过 GetNativeMediaType(index)
// 返回列表；从源描述符读取可覆盖这类驱动，同时仍保留原始 IMFMediaType 用于协商。
bool CollectPresentationModes(IMFMediaSource* source, std::vector<VideoMode>& modes,
    std::wstring& error) {
    modes.clear();
    if (!source) {
        error = L"视频采集源不可用。";
        return false;
    }
    ComPtr<IMFPresentationDescriptor> presentation;
    HRESULT result = source->CreatePresentationDescriptor(&presentation);
    DWORD streamCount = 0;
    if (SUCCEEDED(result)) result = presentation->GetStreamDescriptorCount(&streamCount);
    if (FAILED(result)) {
        error = HResultText(L"读取采集设备流描述失败", result);
        return false;
    }
    for (DWORD streamIndex = 0; streamIndex < streamCount; ++streamIndex) {
        BOOL selected = FALSE;
        ComPtr<IMFStreamDescriptor> stream;
        if (FAILED(presentation->GetStreamDescriptorByIndex(streamIndex, &selected, &stream))) continue;
        ComPtr<IMFMediaTypeHandler> handler;
        GUID major{};
        if (FAILED(stream->GetMediaTypeHandler(&handler)) ||
            FAILED(handler->GetMajorType(&major)) || !SameGuid(major, MFMediaType_Video)) continue;

        DWORD typeCount = 0;
        if (SUCCEEDED(handler->GetMediaTypeCount(&typeCount))) {
            for (DWORD typeIndex = 0; typeIndex < typeCount; ++typeIndex) {
                ComPtr<IMFMediaType> type;
                if (SUCCEEDED(handler->GetMediaTypeByIndex(typeIndex, &type))) AppendMode(type.Get(), modes);
            }
        }
        // 有些驱动把列表计数报告为 0，却仍提供当前类型。
        if (modes.empty()) {
            ComPtr<IMFMediaType> current;
            if (SUCCEEDED(handler->GetCurrentMediaType(&current))) AppendMode(current.Get(), modes);
        }
        if (!modes.empty()) break;
    }
    if (modes.empty()) {
        error = L"采集设备的流描述没有公开可用视频规格。";
        return false;
    }
    SortModes(modes);
    error.clear();
    return true;
}

bool CollectCurrentMode(IMFSourceReader* reader, std::vector<VideoMode>& modes,
    std::wstring& error) {
    modes.clear();
    if (!reader) return false;
    ComPtr<IMFMediaType> current;
    HRESULT result = reader->GetNativeMediaType(kVideoStream,
        static_cast<DWORD>(MF_SOURCE_READER_CURRENT_TYPE_INDEX), &current);
    if (FAILED(result) || !AppendMode(current.Get(), modes)) {
        current.Reset();
        modes.clear();
        result = reader->GetCurrentMediaType(kVideoStream, &current);
        if (FAILED(result) || !AppendMode(current.Get(), modes)) {
            error = FAILED(result) ? HResultText(L"读取驱动当前视频规格失败", result)
                                   : L"驱动当前视频规格缺少尺寸或像素格式。";
            return false;
        }
    }
    SortModes(modes);
    error.clear();
    return true;
}

bool CollectAvailableModes(IMFSourceReader* reader, IMFMediaSource* source,
    std::vector<VideoMode>& modes, std::wstring& error) {
    std::wstring ignored;
    if (CollectNativeModes(reader, modes, ignored)) {
        error.clear();
        return true;
    }
    if (CollectPresentationModes(source, modes, ignored)) {
        error.clear();
        return true;
    }
    if (CollectCurrentMode(reader, modes, ignored)) {
        error.clear();
        return true;
    }
    modes.clear();
    error = L"采集设备没有公开可枚举的视频规格；自动模式仍可尝试驱动当前格式。";
    return false;
}
}  // namespace

unsigned CaptureOutput::RefreshRateBucket(std::uint32_t numerator,
    std::uint32_t denominator) {
    if (!numerator || !denominator) return 0;
    const double rate = static_cast<double>(numerator) / denominator;
    constexpr unsigned commonRates[]{24, 25, 30, 50, 60, 75, 90, 100,
        120, 144, 165, 180, 200, 240, 360, 480};
    for (const unsigned candidate : commonRates) {
        const double tolerance = std::max(0.08, candidate * 0.0021);
        if (std::abs(rate - candidate) <= tolerance) return candidate;
    }
    return rate > 0 ? static_cast<unsigned>(std::llround(rate)) : 0;
}

std::wstring CaptureOutput::BuildModeStableKey(const CaptureNativeMode& mode) {
    std::uint32_t numerator = mode.fpsNumerator;
    std::uint32_t denominator = mode.fpsDenominator ? mode.fpsDenominator : 1;
    const std::uint32_t divisor = std::gcd(numerator, denominator);
    if (divisor) {
        numerator /= divisor;
        denominator /= divisor;
    }
    const std::uint32_t interlace = mode.interlaceMode
        ? mode.interlaceMode
        : (mode.progressive ? static_cast<std::uint32_t>(MFVideoInterlace_Progressive)
                            : static_cast<std::uint32_t>(MFVideoInterlace_Unknown));
    std::wostringstream key;
    key << L"mf1|" << mode.width << L'x' << mode.height << L'|'
        << numerator << L'/' << denominator << L'|'
        << GuidText(mode.subtype) << L"|i" << interlace;
    return key.str();
}

bool CaptureOutput::ModeMatchesStableKey(const CaptureNativeMode& mode,
    const std::wstring& stableKey) {
    if (stableKey.empty()) return false;
    const std::wstring modeKey = mode.stableKey.empty()
        ? BuildModeStableKey(mode) : mode.stableKey;
    if (CompareStringOrdinal(modeKey.c_str(), -1, stableKey.c_str(), -1, TRUE) == CSTR_EQUAL)
        return true;

    StableModeKey expected;
    if (!ParseStableModeKey(stableKey, expected) || mode.width != expected.width ||
            mode.height != expected.height || !SameGuid(mode.subtype, expected.subtype)) return false;
    const std::uint32_t interlace = mode.interlaceMode
        ? mode.interlaceMode
        : (mode.progressive ? static_cast<std::uint32_t>(MFVideoInterlace_Progressive)
                            : static_cast<std::uint32_t>(MFVideoInterlace_Unknown));
    if (interlace != expected.interlaceMode) return false;
    if (!mode.fpsNumerator && !expected.fpsNumerator) return true;
    if (!mode.fpsNumerator || !mode.fpsDenominator || !expected.fpsNumerator) return false;
    const double actualFps = mode.Fps();
    const double expectedFps = static_cast<double>(expected.fpsNumerator) /
        expected.fpsDenominator;
    const double tolerance = std::max(0.02, expectedFps * 0.0005);
    return std::abs(actualFps - expectedFps) <= tolerance;
}

bool CaptureOutput::ModeMatchesTimingStableKey(const CaptureNativeMode& mode,
    const std::wstring& stableKey) {
    StableModeKey expected;
    if (!ParseStableModeKey(stableKey, expected) || mode.width != expected.width ||
            mode.height != expected.height) return false;
    const std::uint32_t interlace = mode.interlaceMode
        ? mode.interlaceMode
        : (mode.progressive ? static_cast<std::uint32_t>(MFVideoInterlace_Progressive)
                            : static_cast<std::uint32_t>(MFVideoInterlace_Unknown));
    if (!TimingScanCompatible(interlace, mode.progressive, expected.interlaceMode)) return false;
    if (!mode.fpsNumerator && !expected.fpsNumerator) return true;
    if (!mode.fpsNumerator || !mode.fpsDenominator || !expected.fpsNumerator) return false;
    const double actualFps = mode.Fps();
    const double expectedFps = static_cast<double>(expected.fpsNumerator) /
        expected.fpsDenominator;
    const double tolerance = std::max(0.02, expectedFps * 0.0005);
    return std::abs(actualFps - expectedFps) <= tolerance;
}

void CaptureOutput::MergeModes(std::vector<CaptureNativeMode>& preferredModes,
    const std::vector<CaptureNativeMode>& additionalModes) {
    // 两个 API 对 59.94 Hz 等时序可能给出不同有理数，不能只比较 stableKey 文本。
    // 先放入 MF 列表，可确保重复项保留 MF 原始描述；DirectShow 独有档仍会补齐。
    for (const auto& candidate : additionalModes) {
        const bool duplicate = std::any_of(preferredModes.begin(), preferredModes.end(),
            [&](const CaptureNativeMode& existing) {
                const std::wstring existingKey = existing.stableKey.empty()
                    ? BuildModeStableKey(existing) : existing.stableKey;
                return ModeMatchesStableKey(candidate, existingKey);
            });
        if (!duplicate) preferredModes.push_back(candidate);
    }
    std::stable_sort(preferredModes.begin(), preferredModes.end(), AutoModeBetter);
}

int CaptureOutput::SelectMode(const std::vector<CaptureNativeMode>& modes,
    CaptureModePolicy policy, const std::wstring& modeKey) {
    if (modes.empty()) return -1;
    if (policy == CaptureModePolicy::Exact || policy == CaptureModePolicy::Preferred) {
        if (modeKey.empty()) return -1;
        int best = -1;
        for (std::size_t index = 0; index < modes.size(); ++index) {
            const bool matches = policy == CaptureModePolicy::Exact
                ? ModeMatchesStableKey(modes[index], modeKey)
                : ModeMatchesTimingStableKey(modes[index], modeKey);
            if (!matches) continue;
            if (policy == CaptureModePolicy::Exact) return static_cast<int>(index);
            if (best < 0 || AutoModeBetter(modes[index], modes[static_cast<std::size_t>(best)]))
                best = static_cast<int>(index);
        }
        return best;
    }
    // Auto 的 modeKey 是跨后端枚举得到的全局目标。当前后端存在相同时序时先用它；
    // 不存在时仍返回本后端的普通 Auto 最佳项，运行时的目标阶段会决定何时允许降级。
    if (!modeKey.empty()) {
        int target = -1;
        for (std::size_t index = 0; index < modes.size(); ++index) {
            if (!ModeMatchesTimingStableKey(modes[index], modeKey)) continue;
            if (target < 0 || AutoModeBetter(modes[index], modes[static_cast<std::size_t>(target)]))
                target = static_cast<int>(index);
        }
        if (target >= 0) return target;
    }
    std::size_t best = 0;
    for (std::size_t index = 1; index < modes.size(); ++index) {
        if (AutoModeBetter(modes[index], modes[best])) best = index;
    }
    return static_cast<int>(best);
}

bool CaptureOutput::AllowsDriverDefault(CaptureModePolicy policy) noexcept {
    return policy == CaptureModePolicy::Auto;
}

unsigned CaptureOutput::FrameSilenceTimeoutMs(const CaptureOptions& options,
    bool receivedFirstFrame) noexcept {
    return receivedFirstFrame ? options.signalTimeoutMs : options.firstFrameTimeoutMs;
}

struct CaptureOutput::Impl {
    struct ReadEvent {
        HRESULT result = S_OK;
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        std::uint64_t arrivalQpc = 0;
        ComPtr<IMFSample> sample;
    };

    struct Session {
        explicit Session(CaptureOptions value)
            : options(std::move(value)), notificationTarget(options.notifyWindow),
              notificationMessage(options.notifyMessage) {
            if (options.modePolicy == CaptureModePolicy::Auto && options.hasRequestedMode &&
                    options.requestedMode.width && options.requestedMode.height &&
                    options.requestedMode.fpsNumerator && options.requestedMode.fpsDenominator) {
                autoTargetKey = options.requestedMode.stableKey.empty()
                    ? CaptureOutput::BuildModeStableKey(options.requestedMode)
                    : options.requestedMode.stableKey;
                autoTargetOnly = !autoTargetKey.empty();
            }
            status.state = CaptureState::Starting;
            status.message = L"正在启动采集设备…";
            status.requestedPolicy = options.modePolicy;
            status.requestedModeKey = autoTargetOnly ? autoTargetKey : options.exactModeKey;
            status.requestedMode = options.requestedMode;
            status.hasRequestedMode = options.hasRequestedMode;
        }

        CaptureOptions options;
        std::atomic<HWND> notificationTarget{nullptr};
        UINT notificationMessage = 0;
        std::atomic<bool> stopping{false};
        // current 会在工作线程结束后继续保留最终错误供 UI 查询；Active() 单独依据此标记。
        std::atomic<bool> workerRunning{true};
        // 刷新消息只保留一个待处理项，UI 忙碌时不会按视频帧率堆积消息队列。
        std::atomic<bool> notificationPending{false};
        // 每次重新打开设备都会换代，迟到的旧回调不能污染新读取器的单帧邮箱。
        std::atomic<std::uint64_t> activeReaderGeneration{0};
        mutable std::mutex mutex;
        std::condition_variable wake;
        CaptureStatus status;
        CaptureDiagnosticsTracker diagnostics;
        // 只有工作线程更新内容分析器；回调只在 mutex 内维护入口覆盖计数。
        std::uint64_t mailboxSkippedFrames = 0;
        std::shared_ptr<const CaptureFrame> latest;
        // 复用少量帧缓冲；仍被 UI/呈现器持有的帧绝不改写，忙时才临时扩容。
        std::array<std::shared_ptr<CaptureFrame>, 4> framePool;
        std::size_t framePoolCursor = 0;
        // 某些驱动接受建图却只会在另一项等价原生格式上真正出帧。首次无帧后
        // 轮换候选，避免每轮重连都卡在同一项；一旦成功出帧即恢复首选顺序。
        std::size_t mediaFoundationCandidateOffset = 0;
        std::size_t directShowCandidateOffset = 0;
        // Auto 先让两个后端只尝试合并枚举得到的全局时序。两边均无法打开后，
        // 再进入普通 Auto，允许其它模式和 driver default。
        std::wstring autoTargetKey;
        bool autoTargetOnly = false;
        bool eventReady = false;
        ReadEvent event;
    };

    static CaptureModePolicy EffectiveModePolicy(const std::shared_ptr<Session>& session) {
        return session->autoTargetOnly ? CaptureModePolicy::Preferred
                                       : session->options.modePolicy;
    }

    static const std::wstring& EffectiveModeKey(const std::shared_ptr<Session>& session) {
        return session->autoTargetOnly ? session->autoTargetKey
                                       : session->options.exactModeKey;
    }

    class ReaderCallback final : public IMFSourceReaderCallback {
    public:
        ReaderCallback(std::shared_ptr<Session> value, std::uint64_t valueGeneration)
            : session(std::move(value)), generation(valueGeneration) {}

        STDMETHODIMP QueryInterface(REFIID iid, void** object) override {
            if (!object) return E_POINTER;
            if (iid == __uuidof(IUnknown) || iid == __uuidof(IMFSourceReaderCallback)) {
                *object = static_cast<IMFSourceReaderCallback*>(this);
                AddRef();
                return S_OK;
            }
            *object = nullptr;
            return E_NOINTERFACE;
        }

        STDMETHODIMP_(ULONG) AddRef() override {
            return references.fetch_add(1, std::memory_order_relaxed) + 1;
        }

        STDMETHODIMP_(ULONG) Release() override {
            const ULONG remaining = references.fetch_sub(1, std::memory_order_acq_rel) - 1;
            if (!remaining) delete this;
            return remaining;
        }

        STDMETHODIMP OnReadSample(HRESULT result, DWORD, DWORD flags,
            LONGLONG timestamp, IMFSample* sample) override {
            auto active = session;
            if (!active || active->stopping.load(std::memory_order_relaxed)) return S_OK;
            if (active->activeReaderGeneration.load(std::memory_order_acquire) != generation) return S_OK;
            {
                std::lock_guard<std::mutex> lock(active->mutex);
                // Close() 可能在第一次检查后换代读取器；锁内再确认，避免旧回调覆盖新会话事件。
                if (active->stopping.load(std::memory_order_relaxed) ||
                    active->activeReaderGeneration.load(std::memory_order_acquire) != generation) return S_OK;
                active->event.result = result;
                active->event.flags = flags;
                active->event.timestamp = timestamp;
                if (active->eventReady && active->event.sample && sample)
                    ++active->mailboxSkippedFrames;
                LARGE_INTEGER counter{};
                QueryPerformanceCounter(&counter);
                active->event.arrivalQpc = static_cast<std::uint64_t>(counter.QuadPart);
                active->event.sample = sample;
                active->eventReady = true;
            }
            active->wake.notify_all();
            return S_OK;
        }

        STDMETHODIMP OnFlush(DWORD) override {
            if (session) session->wake.notify_all();
            return S_OK;
        }

        STDMETHODIMP OnEvent(DWORD, IMFMediaEvent*) override { return S_OK; }

    private:
        ~ReaderCallback() = default;
        std::atomic<ULONG> references{1};
        std::shared_ptr<Session> session;
        std::uint64_t generation = 0;
    };

    struct OpenedReader {
        std::weak_ptr<Session> owner;
        std::uint64_t generation = 0;
        ComPtr<IMFMediaSource> source;
        ComPtr<IMFSourceReader> reader;
        ComPtr<ReaderCallback> callback;
        int width = 0;
        int height = 0;
        LONG stride = 0;
        double nominalFps = 0;
        CaptureNativeMode requestedMode;
        CaptureNativeMode actualMode;
        bool usedDriverDefault = false;

        void Close() {
            if (auto session = owner.lock()) {
                std::uint64_t expected = generation;
                session->activeReaderGeneration.compare_exchange_strong(expected,
                    generation + 1, std::memory_order_acq_rel);
            }
            reader.Reset();
            if (source) source->Shutdown();
            source.Reset();
            callback.Reset();
            width = height = 0;
            stride = 0;
            nominalFps = 0;
            requestedMode = {};
            actualMode = {};
            usedDriverDefault = false;
        }
    };

    mutable std::mutex controlMutex;
    std::shared_ptr<Session> current;
    std::thread worker;

    static void Notify(const std::shared_ptr<Session>& session,
        std::uint64_t serial, bool statusOnly) {
        if (session->notificationPending.exchange(true, std::memory_order_acq_rel)) return;
        if (!NotifyWindow(session->notificationTarget.load(std::memory_order_relaxed),
                session->notificationMessage, serial, statusOnly))
            session->notificationPending.store(false, std::memory_order_release);
    }

    static void SetStatus(const std::shared_ptr<Session>& session, CaptureState state,
        const std::wstring& message, bool clearFrame = false) {
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            changed = session->status.state != state || session->status.message != message;
            session->status.state = state;
            session->status.message = message;
            if (clearFrame) session->latest.reset();
        }
        if (changed) Notify(session, 0, true);
    }

    static bool QueryOutputFormat(IMFSourceReader* reader, OpenedReader& opened,
        std::wstring& error) {
        ComPtr<IMFMediaType> currentType;
        HRESULT result = reader->GetCurrentMediaType(kVideoStream, &currentType);
        UINT32 width = 0, height = 0;
        if (FAILED(result) || FAILED(MFGetAttributeSize(currentType.Get(), MF_MT_FRAME_SIZE,
                &width, &height)) || width == 0 || height == 0 ||
            width > kMaximumFrameWidth || height > kMaximumFrameHeight) {
            error = L"采集设备返回了不支持的画面尺寸。";
            return false;
        }
        UINT32 numerator = 0, denominator = 0;
        MFGetAttributeRatio(currentType.Get(), MF_MT_FRAME_RATE, &numerator, &denominator);
        UINT32 encodedStride = 0;
        LONG stride = 0;
        if (SUCCEEDED(currentType->GetUINT32(MF_MT_DEFAULT_STRIDE, &encodedStride))) {
            stride = static_cast<LONG>(encodedStride);
        } else if (FAILED(MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1,
                width, &stride))) {
            stride = static_cast<LONG>(width * 4);
        }
        opened.width = static_cast<int>(width);
        opened.height = static_cast<int>(height);
        opened.stride = stride;
        opened.nominalFps = denominator ? static_cast<double>(numerator) / denominator : 0;
        return true;
    }

    static bool QueryActualNativeFormat(IMFSourceReader* reader, OpenedReader& opened,
        std::wstring& error) {
        ComPtr<IMFMediaType> currentNative;
        HRESULT result = reader->GetNativeMediaType(kVideoStream,
            static_cast<DWORD>(MF_SOURCE_READER_CURRENT_TYPE_INDEX), &currentNative);
        VideoMode parsed;
        if (SUCCEEDED(result) && ReadMode(currentNative.Get(), parsed)) {
            opened.actualMode = std::move(parsed.description);
            return true;
        }

        // 专用采集卡可能拒绝 CURRENT_TYPE_INDEX，但仍可正常输出。此时用 Source Reader
        // 当前输出确认尺寸和时序；若此前已经确认原生 subtype，则保留它用于状态展示。
        ComPtr<IMFMediaType> currentOutput;
        result = reader->GetCurrentMediaType(kVideoStream, &currentOutput);
        parsed = {};
        if (SUCCEEDED(result) && ReadMode(currentOutput.Get(), parsed)) {
            if (opened.actualMode.width && opened.actualMode.height) {
                opened.actualMode.width = parsed.description.width;
                opened.actualMode.height = parsed.description.height;
                opened.actualMode.fpsNumerator = parsed.description.fpsNumerator;
                opened.actualMode.fpsDenominator = parsed.description.fpsDenominator;
                opened.actualMode.interlaceMode = parsed.description.interlaceMode;
                opened.actualMode.progressive = parsed.description.progressive;
                opened.actualMode.stableKey = CaptureOutput::BuildModeStableKey(opened.actualMode);
            } else {
                opened.actualMode = std::move(parsed.description);
            }
            error.clear();
            return true;
        }
        error = FAILED(result) ? HResultText(L"回读采集设备实际模式失败", result)
                               : L"采集设备返回的实际模式无效。";
        return false;
    }

    static HRESULT SetRgbOutput(IMFSourceReader* reader, const CaptureNativeMode* sourceMode) {
        ComPtr<IMFMediaType> rgb;
        HRESULT result = MFCreateMediaType(&rgb);
        if (FAILED(result)) return result;
        rgb->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        rgb->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        if (sourceMode && sourceMode->width && sourceMode->height) {
            MFSetAttributeSize(rgb.Get(), MF_MT_FRAME_SIZE, sourceMode->width, sourceMode->height);
            if (sourceMode->fpsNumerator && sourceMode->fpsDenominator) {
                MFSetAttributeRatio(rgb.Get(), MF_MT_FRAME_RATE,
                    sourceMode->fpsNumerator, sourceMode->fpsDenominator);
            }
            MFSetAttributeRatio(rgb.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
            rgb->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        }
        result = reader->SetCurrentMediaType(kVideoStream, nullptr, rgb.Get());
        if (SUCCEEDED(result) || !sourceMode) return result;

        // 某些 UVC/专用驱动拒绝显式尺寸，但允许只声明 RGB32，并沿用刚选中的原生模式。
        rgb.Reset();
        result = MFCreateMediaType(&rgb);
        if (FAILED(result)) return result;
        rgb->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        rgb->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        return reader->SetCurrentMediaType(kVideoStream, nullptr, rgb.Get());
    }

    static bool Open(const std::shared_ptr<Session>& session, OpenedReader& opened,
        std::wstring& error) {
        const CaptureModePolicy modePolicy = EffectiveModePolicy(session);
        const std::wstring& modeKey = EffectiveModeKey(session);
        const std::wstring mediaFoundationId = !session->options.mediaFoundationId.empty()
            ? session->options.mediaFoundationId : session->options.symbolicLink;
        if (mediaFoundationId.empty() || mediaFoundationId.compare(0, 7, L"dshow1|") == 0) {
            error = L"所选设备没有 Media Foundation 采集入口。";
            return false;
        }
        ComPtr<IMFAttributes> sourceAttributes;
        HRESULT result = MFCreateAttributes(&sourceAttributes, 3);
        if (SUCCEEDED(result)) result = sourceAttributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
        if (SUCCEEDED(result)) result = sourceAttributes->SetString(
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
            mediaFoundationId.c_str());
        if (SUCCEEDED(result)) result = sourceAttributes->SetUINT32(
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_MAX_BUFFERS, 1);
        if (SUCCEEDED(result)) result = MFCreateDeviceSource(sourceAttributes.Get(), &opened.source);
        if (FAILED(result)) {
            error = HResultText(L"找不到或无法打开所选采集设备", result);
            opened.Close();
            return false;
        }

        const std::uint64_t generation =
            session->activeReaderGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
        opened.owner = session;
        opened.generation = generation;
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            session->event = {};
            session->eventReady = false;
        }
        opened.callback.Attach(new (std::nothrow) ReaderCallback(session, generation));
        if (!opened.callback) {
            error = L"创建采集回调失败：内存不足。";
            opened.Close();
            return false;
        }
        ComPtr<IMFAttributes> readerAttributes;
        result = MFCreateAttributes(&readerAttributes, 5);
        if (SUCCEEDED(result)) result = readerAttributes->SetUnknown(
            MF_SOURCE_READER_ASYNC_CALLBACK, opened.callback.Get());
        if (SUCCEEDED(result)) result = readerAttributes->SetUINT32(
            MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        if (SUCCEEDED(result)) result = readerAttributes->SetUINT32(MF_LOW_LATENCY, TRUE);
        if (SUCCEEDED(result)) result = readerAttributes->SetUINT32(
            MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        if (SUCCEEDED(result)) result = MFCreateSourceReaderFromMediaSource(
            opened.source.Get(), readerAttributes.Get(), &opened.reader);
        if (FAILED(result)) {
            error = HResultText(L"创建设备视频读取器失败", result);
            opened.Close();
            return false;
        }

        opened.reader->SetStreamSelection(
            static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        result = opened.reader->SetStreamSelection(kVideoStream, TRUE);
        if (FAILED(result)) {
            error = HResultText(L"采集设备没有可用的视频流", result);
            opened.Close();
            return false;
        }

        std::vector<VideoMode> modes;
        std::wstring enumerationError;
        const bool hasModes = CollectAvailableModes(opened.reader.Get(), opened.source.Get(),
            modes, enumerationError);
        if (!hasModes && !CaptureOutput::AllowsDriverDefault(modePolicy)) {
            error = modePolicy == CaptureModePolicy::Exact
                ? L"Media Foundation 无法验证所选精确规格；驱动没有公开规格列表。"
                : session->autoTargetOnly
                    ? L"Media Foundation 未公开 Auto 目标时序，正在切换其它采集方式。"
                    : L"Media Foundation 无法保持所选画面规格；驱动没有公开规格列表。";
            opened.Close();
            return false;
        }
        if (hasModes) {
            std::vector<CaptureNativeMode> descriptions;
            descriptions.reserve(modes.size());
            for (const auto& mode : modes) descriptions.push_back(mode.description);
            const int selectedIndex = CaptureOutput::SelectMode(descriptions,
                modePolicy, modeKey);
            if (selectedIndex < 0) {
                if (modePolicy == CaptureModePolicy::Exact)
                    error = L"Media Foundation 没有找到所选精确规格。";
                else if (modePolicy == CaptureModePolicy::Preferred)
                    error = session->autoTargetOnly
                        ? L"Media Foundation 没有 Auto 目标时序，正在切换其它采集方式。"
                        : L"Media Foundation 没有找到分辨率、刷新率和扫描方式一致的规格。";
                else
                    error = L"采集设备没有可选择的视频模式。";
                opened.Close();
                return false;
            }
            if (selectedIndex > 0) std::rotate(modes.begin(), modes.begin() + selectedIndex,
                modes.begin() + selectedIndex + 1);
        }

        std::vector<std::size_t> modeAttemptOrder;
        if (hasModes) {
            for (std::size_t index = 0; index < modes.size(); ++index) {
                const bool eligible = modePolicy == CaptureModePolicy::Auto ||
                    (modePolicy == CaptureModePolicy::Exact
                        ? CaptureOutput::ModeMatchesStableKey(modes[index].description,
                            modeKey)
                        : CaptureOutput::ModeMatchesTimingStableKey(modes[index].description,
                            modeKey));
                if (eligible) modeAttemptOrder.push_back(index);
            }
            if (!modeAttemptOrder.empty() && modePolicy != CaptureModePolicy::Exact) {
                const std::size_t offset = session->mediaFoundationCandidateOffset %
                    modeAttemptOrder.size();
                std::rotate(modeAttemptOrder.begin(), modeAttemptOrder.begin() + offset,
                    modeAttemptOrder.end());
            }
        }

        HRESULT lastFormatError = MF_E_INVALIDMEDIATYPE;
        bool configured = false;
        if (hasModes) {
            for (const std::size_t index : modeAttemptOrder) {
                const auto& mode = modes[index];
                lastFormatError = opened.reader->SetCurrentMediaType(
                    kVideoStream, nullptr, mode.type.Get());
                if (FAILED(lastFormatError)) continue;

                // RGB 转换前先回读一次，兼容之后不支持 CURRENT_TYPE_INDEX 的专用驱动。
                ComPtr<IMFMediaType> confirmedType;
                VideoMode confirmed;
                if (SUCCEEDED(opened.reader->GetCurrentMediaType(kVideoStream, &confirmedType)) &&
                    ReadMode(confirmedType.Get(), confirmed)) {
                    opened.actualMode = std::move(confirmed.description);
                } else {
                    opened.actualMode = mode.description;
                }
                lastFormatError = SetRgbOutput(opened.reader.Get(), &mode.description);
                if (FAILED(lastFormatError)) continue;

                std::wstring queryError;
                if (QueryOutputFormat(opened.reader.Get(), opened, queryError) &&
                    QueryActualNativeFormat(opened.reader.Get(), opened, queryError)) {
                    const bool exactMismatch = modePolicy == CaptureModePolicy::Exact &&
                        !CaptureOutput::ModeMatchesStableKey(opened.actualMode,
                            modeKey);
                    const bool preferredMismatch = modePolicy == CaptureModePolicy::Preferred &&
                        !CaptureOutput::ModeMatchesTimingStableKey(opened.actualMode,
                            modeKey);
                    if (exactMismatch || preferredMismatch) {
                        error = exactMismatch
                            ? L"Media Foundation 没有采用请求的精确规格。"
                            : L"Media Foundation 改变了请求的分辨率、刷新率或扫描方式。";
                        continue;
                    }
                    opened.requestedMode = mode.description;
                    configured = true;
                    error.clear();
                    break;
                }
                error = std::move(queryError);
            }
        } else {
            // GC555 等专用驱动可能完全不枚举模式，但能用其当前/default 类型正常出帧。
            // Auto 才允许这条路径；精确模式已在上方拒绝，语义仍然严格。
            opened.usedDriverDefault = true;
            lastFormatError = SetRgbOutput(opened.reader.Get(), nullptr);
            if (SUCCEEDED(lastFormatError)) {
                std::wstring queryError;
                if (QueryOutputFormat(opened.reader.Get(), opened, queryError) &&
                    QueryActualNativeFormat(opened.reader.Get(), opened, queryError)) {
                    configured = true;
                    error.clear();
                } else {
                    error = std::move(queryError);
                }
            }
        }
        if (!configured) {
            if (error.empty()) {
                error = HResultText(hasModes ? L"采集设备无法转换为 RGB32 画面"
                                             : L"驱动默认视频格式无法打开", lastFormatError);
            }
            opened.Close();
            return false;
        }
        return true;
    }

    static std::shared_ptr<CaptureFrame> AcquireFrame(const std::shared_ptr<Session>& session) {
        for (std::size_t offset = 0; offset < session->framePool.size(); ++offset) {
            const std::size_t index = (session->framePoolCursor + offset) % session->framePool.size();
            auto& slot = session->framePool[index];
            if (!slot) slot = std::make_shared<CaptureFrame>();
            if (slot.use_count() == 1) {
                session->framePoolCursor = (index + 1) % session->framePool.size();
                return slot;
            }
        }
        // 校准截图等调用方可能短暂同时持有四帧；替换池槽不会破坏仍在使用的旧帧。
        auto fresh = std::make_shared<CaptureFrame>();
        session->framePool[session->framePoolCursor] = fresh;
        session->framePoolCursor = (session->framePoolCursor + 1) % session->framePool.size();
        return fresh;
    }

    static bool CopySample(const std::shared_ptr<Session>& session, IMFSample* sample,
        int width, int height, LONG fallbackStride,
        std::shared_ptr<CaptureFrame>& frame, std::wstring& error) {
        if (!sample || width <= 0 || height <= 0) {
            error = L"采集设备返回了空视频帧。";
            return false;
        }
        const std::size_t rowBytes = static_cast<std::size_t>(width) * 4;
        if (rowBytes > std::numeric_limits<std::size_t>::max() / static_cast<std::size_t>(height)) {
            error = L"采集画面尺寸超出内存限制。";
            return false;
        }
        const std::size_t totalBytes = rowBytes * static_cast<std::size_t>(height);
        ComPtr<IMFMediaBuffer> buffer;
        DWORD bufferCount = 0;
        HRESULT result = sample->GetBufferCount(&bufferCount);
        if (SUCCEEDED(result) && bufferCount == 1) result = sample->GetBufferByIndex(0, &buffer);
        else result = sample->ConvertToContiguousBuffer(&buffer);
        if (FAILED(result)) result = sample->ConvertToContiguousBuffer(&buffer);
        if (FAILED(result)) {
            error = HResultText(L"读取采集帧缓冲失败", result);
            return false;
        }

        auto next = AcquireFrame(session);
        next->width = width;
        next->height = height;
        next->stride = static_cast<int>(rowBytes);
        next->serial = 0;
        next->capturedMs = 0;
        next->nativeSubtype = {};
        next->sampleTimestamp100ns = 0;
        next->arrivalQpc = 0;
        next->storage = CaptureFrameStorage::CpuBgra32;
        next->bgra.resize(totalBytes);

        auto copyRows = [&](const BYTE* scanline, LONG pitch,
                const BYTE* bufferStart, std::size_t bufferLength) {
            if (!scanline || std::abs(static_cast<long long>(pitch)) <
                    static_cast<long long>(rowBytes)) return false;
            const auto begin = reinterpret_cast<std::uintptr_t>(bufferStart);
            const auto end = begin <= std::numeric_limits<std::uintptr_t>::max() - bufferLength
                ? begin + bufferLength : 0;
            for (int y = 0; y < height; ++y) {
                const BYTE* row = scanline + static_cast<std::ptrdiff_t>(y) * pitch;
                if (bufferStart) {
                    const auto address = reinterpret_cast<std::uintptr_t>(row);
                    if (!end || address < begin || address > end || rowBytes > end - address) return false;
                }
                std::memcpy(next->bgra.data() + static_cast<std::size_t>(y) * rowBytes,
                    row, rowBytes);
            }
            return true;
        };

        ComPtr<IMF2DBuffer2> surface2;
        if (SUCCEEDED(buffer.As(&surface2))) {
            BYTE* scanline = nullptr;
            BYTE* bufferStart = nullptr;
            LONG pitch = 0;
            DWORD bufferLength = 0;
            result = surface2->Lock2DSize(MF2DBuffer_LockFlags_Read,
                &scanline, &pitch, &bufferStart, &bufferLength);
            if (FAILED(result) || !copyRows(scanline, pitch, bufferStart, bufferLength)) {
                if (SUCCEEDED(result)) surface2->Unlock2D();
                error = L"采集帧的二维缓冲布局无效。";
                return false;
            }
            surface2->Unlock2D();
        } else {
            ComPtr<IMF2DBuffer> surface;
            if (SUCCEEDED(buffer.As(&surface))) {
            BYTE* scanline = nullptr;
            LONG pitch = 0;
            result = surface->Lock2D(&scanline, &pitch);
            if (FAILED(result) || !copyRows(scanline, pitch, nullptr, 0)) {
                if (SUCCEEDED(result)) surface->Unlock2D();
                error = L"采集帧的二维缓冲布局无效。";
                return false;
            }
            surface->Unlock2D();
            } else {
            BYTE* bytes = nullptr;
            DWORD maximum = 0, currentLength = 0;
            result = buffer->Lock(&bytes, &maximum, &currentLength);
            if (FAILED(result) || !bytes) {
                error = HResultText(L"锁定采集帧失败", result);
                return false;
            }
            const LONG absoluteStride = std::max<LONG>(static_cast<LONG>(rowBytes),
                static_cast<LONG>(std::abs(static_cast<long long>(fallbackStride))));
            const std::size_t required = static_cast<std::size_t>(absoluteStride) * height;
            if (currentLength < required) {
                buffer->Unlock();
                error = L"采集帧数据长度不足。";
                return false;
            }
            const BYTE* firstRow = bytes;
            LONG step = absoluteStride;
            if (fallbackStride < 0) {
                firstRow += static_cast<std::size_t>(height - 1) * absoluteStride;
                step = -absoluteStride;
            }
            for (int y = 0; y < height; ++y) {
                std::memcpy(next->bgra.data() + static_cast<std::size_t>(y) * rowBytes,
                    firstRow + static_cast<std::ptrdiff_t>(y) * step, rowBytes);
            }
            buffer->Unlock();
            }
        }
        frame = std::move(next);
        return true;
    }

    static bool CopyDirectShowFrame(const std::shared_ptr<Session>& session,
        const directshow::FrameView& source, std::shared_ptr<CaptureFrame>& frame,
        std::wstring& error) {
        if (!source.bytes || source.width <= 0 || source.height <= 0 || source.stride <= 0 ||
                source.width > static_cast<int>(kMaximumFrameWidth) ||
                source.height > static_cast<int>(kMaximumFrameHeight)) {
            error = L"DirectShow 返回了无效的视频帧。";
            return false;
        }
        const std::size_t rowBytes = static_cast<std::size_t>(source.width) * 4;
        const std::size_t stride = static_cast<std::size_t>(source.stride);
        if (stride < rowBytes || stride > std::numeric_limits<std::size_t>::max() /
                static_cast<std::size_t>(source.height)) {
            error = L"DirectShow 视频帧的行跨度无效。";
            return false;
        }
        const std::size_t required = stride * static_cast<std::size_t>(source.height);
        if (source.length < required || rowBytes > std::numeric_limits<std::size_t>::max() /
                static_cast<std::size_t>(source.height)) {
            error = L"DirectShow 视频帧数据长度不足。";
            return false;
        }

        auto next = AcquireFrame(session);
        next->width = source.width;
        next->height = source.height;
        next->stride = static_cast<int>(rowBytes);
        next->serial = 0;
        next->capturedMs = 0;
        next->nativeSubtype = source.nativeMode.subtype;
        next->sampleTimestamp100ns = source.sampleTimestamp100ns;
        next->hasSampleTimestamp = source.hasSampleTimestamp;
        next->mailboxSkippedFrames = source.mailboxSkippedFrames;
        next->arrivalQpc = source.arrivalQpc;
        next->storage = CaptureFrameStorage::CpuBgra32;
        next->bgra.resize(rowBytes * static_cast<std::size_t>(source.height));
        for (int y = 0; y < source.height; ++y) {
            const int sourceY = source.bottomUp ? source.height - 1 - y : y;
            std::memcpy(next->bgra.data() + static_cast<std::size_t>(y) * rowBytes,
                source.bytes + static_cast<std::size_t>(sourceY) * stride, rowBytes);
        }
        frame = std::move(next);
        return true;
    }

    static void Publish(const std::shared_ptr<Session>& session,
        std::shared_ptr<CaptureFrame> frame, double nominalFps) {
        const std::uint64_t now = GetTickCount64();
        static const double qpcFrequency = [] {
            LARGE_INTEGER value{};
            QueryPerformanceFrequency(&value);
            return static_cast<double>(value.QuadPart);
        }();
        const double arrivalMs = frame->arrivalQpc && qpcFrequency > 0
            ? static_cast<double>(frame->arrivalQpc) * 1000.0 / qpcFrequency
            : static_cast<double>(now);
        // 在发布前检查原始视频，不把软件光标、HUD 或重复 Present 算成画面更新。
        // 采样位于现有工作线程，不持有 UI 状态锁，也不创建额外视频队列。
        session->diagnostics.Observe(*frame, arrivalMs);
        std::uint64_t serial = 0;
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            serial = session->status.frameSerial + 1;
            frame->serial = serial;
            frame->capturedMs = now;
            session->latest = std::move(frame);
            session->status.state = CaptureState::Running;
            const bool autoCompatibilityMode =
                session->options.modePolicy == CaptureModePolicy::Auto &&
                !session->autoTargetKey.empty() && !session->autoTargetOnly &&
                session->status.hasActualMode &&
                !CaptureOutput::ModeMatchesTimingStableKey(session->status.actualMode,
                    session->autoTargetKey);
            if (session->status.backend == CaptureBackend::DirectShow) {
                session->status.directShowError.clear();
                session->status.message = autoCompatibilityMode
                    ? L"正在输出兼容规格 · DirectShow 兼容模式；需要固定高刷请选择对应帧率优先"
                    : L"正在输出采集画面 · DirectShow 兼容模式";
            } else {
                session->status.mediaFoundationError.clear();
                if (autoCompatibilityMode) {
                    session->status.message =
                        L"正在输出兼容规格；需要固定高刷请选择对应帧率优先";
                } else {
                    session->status.message =
                        session->status.requestedPolicy == CaptureModePolicy::Auto &&
                            !session->status.hasRequestedMode
                        ? L"正在输出采集画面（驱动当前格式）"
                        : L"正在输出采集画面";
                }
            }
            session->status.width = session->latest->width;
            session->status.height = session->latest->height;
            session->status.nominalFps = nominalFps;
            session->status.diagnostics = session->diagnostics.Snapshot();
            session->status.measuredFps = session->status.diagnostics.deliveredFps;
            session->status.frameSerial = serial;
            session->status.lastFrameMs = now;
        }
        Notify(session, serial, false);
    }

    struct AttemptResult {
        bool stopped = false;
        bool established = false;
        bool producedFrame = false;
        std::wstring error;
    };

    static void BeginAttempt(const std::shared_ptr<Session>& session,
        CaptureBackend backend, const std::wstring& message, bool fallback) {
        session->diagnostics.Reset();
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            if (session->status.backend != CaptureBackend::None &&
                    session->status.backend != backend) ++session->status.backendSwitchCount;
            session->status.backend = backend;
            session->status.fallbackUsed = session->status.fallbackUsed || fallback;
            // 带全局目标的 Auto 与其它明确策略都保留调用方请求；没有目标的 Auto
            // 才由各后端自行报告选择结果。
            if (session->options.modePolicy == CaptureModePolicy::Auto &&
                    !session->options.hasRequestedMode) {
                session->status.requestedMode = {};
                session->status.hasRequestedMode = false;
            } else {
                session->status.requestedMode = session->options.requestedMode;
                session->status.hasRequestedMode = session->options.hasRequestedMode;
            }
            // 新一轮尚未收到/协商出画面，不能继续展示上一后端的尺寸、帧率和掉帧口径。
            // frameSerial 保持单调递增，方便 UI 合并刷新消息；下一帧会在旧序号后继续发布。
            session->status.width = 0;
            session->status.height = 0;
            session->status.nominalFps = 0;
            session->status.measuredFps = 0;
            session->status.diagnostics = {};
            session->mailboxSkippedFrames = 0;
            session->status.lastFrameMs = 0;
            session->status.actualMode = {};
            session->status.hasActualMode = false;
            session->status.droppedFrames = 0;
            session->status.dropCountAvailable = false;
            session->status.requestedModeKey = !session->autoTargetKey.empty()
                ? session->autoTargetKey : session->options.exactModeKey;
            session->status.state = fallback ? CaptureState::Reconnecting : CaptureState::Starting;
            session->status.message = message;
            session->latest.reset();
        }
        Notify(session, 0, true);
    }

    static AttemptResult RunMediaFoundationAttempt(const std::shared_ptr<Session>& session,
        bool fallback) {
        AttemptResult attempt;
        BeginAttempt(session, CaptureBackend::MediaFoundation,
            fallback ? L"正在重新尝试标准采集方式…" : L"正在打开采集设备…", fallback);
        OpenedReader opened;
        if (!Open(session, opened, attempt.error)) return attempt;
        attempt.established = true;

        {
            std::lock_guard<std::mutex> lock(session->mutex);
            session->status.state = CaptureState::WaitingSignal;
            session->status.message = opened.usedDriverDefault
                ? L"设备未公开规格，已采用驱动当前格式；正在等待视频信号…"
                : L"标准采集方式已连接，正在等待视频信号…";
            session->status.width = opened.width;
            session->status.height = opened.height;
            session->status.nominalFps = opened.nominalFps;
            session->status.measuredFps = 0;
            if (!session->status.hasRequestedMode) {
                session->status.requestedModeKey = opened.requestedMode.stableKey;
                session->status.requestedMode = opened.requestedMode;
                session->status.hasRequestedMode = !opened.requestedMode.stableKey.empty();
            }
            session->status.actualMode = opened.actualMode;
            session->status.hasActualMode = !opened.actualMode.stableKey.empty();
        }
        Notify(session, 0, true);

        HRESULT request = opened.reader->ReadSample(kVideoStream, 0, nullptr, nullptr, nullptr, nullptr);
        if (FAILED(request)) attempt.error = HResultText(L"请求 Media Foundation 采集帧失败", request);
        std::uint64_t lastFrameAt = GetTickCount64();
        while (attempt.error.empty() && !session->stopping.load(std::memory_order_relaxed)) {
            ReadEvent event;
            bool gotEvent = false;
            {
                std::unique_lock<std::mutex> lock(session->mutex);
                session->wake.wait_for(lock, std::chrono::milliseconds(50), [&] {
                    return session->eventReady || session->stopping.load(std::memory_order_relaxed);
                });
                if (session->eventReady) {
                    event = std::move(session->event);
                    session->event = {};
                    session->eventReady = false;
                    gotEvent = true;
                }
            }
            if (session->stopping.load(std::memory_order_relaxed)) break;
            const std::uint64_t now = GetTickCount64();
            if (gotEvent) {
                if (FAILED(event.result) || (event.flags &
                        (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM))) {
                    attempt.error = FAILED(event.result)
                        ? HResultText(L"Media Foundation 读取采集设备失败", event.result)
                        : L"Media Foundation 视频流已结束。";
                    break;
                }
                if (event.flags & (MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED |
                        MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED)) {
                    std::wstring formatError;
                    if (!QueryOutputFormat(opened.reader.Get(), opened, formatError) ||
                            !QueryActualNativeFormat(opened.reader.Get(), opened, formatError)) {
                        attempt.error = formatError;
                        break;
                    }
                    const CaptureModePolicy modePolicy = EffectiveModePolicy(session);
                    const std::wstring& modeKey = EffectiveModeKey(session);
                    const bool exactMismatch = modePolicy == CaptureModePolicy::Exact &&
                        !CaptureOutput::ModeMatchesStableKey(opened.actualMode, modeKey);
                    const bool preferredMismatch = modePolicy == CaptureModePolicy::Preferred &&
                        !CaptureOutput::ModeMatchesTimingStableKey(opened.actualMode, modeKey);
                    if (exactMismatch || preferredMismatch) {
                        attempt.error = exactMismatch
                            ? L"Media Foundation 已离开请求的精确规格。"
                            : L"Media Foundation 已改变请求的分辨率、刷新率或扫描方式。";
                        break;
                    }
                    std::lock_guard<std::mutex> lock(session->mutex);
                    session->status.actualMode = opened.actualMode;
                    session->status.hasActualMode = true;
                    session->status.width = opened.width;
                    session->status.height = opened.height;
                    session->status.nominalFps = opened.nominalFps;
                }
                if (event.sample) {
                    std::shared_ptr<CaptureFrame> frame;
                    if (!CopySample(session, event.sample.Get(), opened.width, opened.height,
                            opened.stride, frame, attempt.error)) break;
                    frame->nativeSubtype = opened.actualMode.subtype;
                    frame->sampleTimestamp100ns = event.timestamp;
                    LONGLONG sampleTime = 0;
                    frame->hasSampleTimestamp = SUCCEEDED(event.sample->GetSampleTime(&sampleTime));
                    if (frame->hasSampleTimestamp) frame->sampleTimestamp100ns = sampleTime;
                    {
                        std::lock_guard<std::mutex> lock(session->mutex);
                        frame->mailboxSkippedFrames = session->mailboxSkippedFrames;
                    }
                    frame->arrivalQpc = event.arrivalQpc;
                    lastFrameAt = now;
                    attempt.producedFrame = true;
                    Publish(session, std::move(frame), opened.nominalFps);
                }
                request = opened.reader->ReadSample(kVideoStream, 0, nullptr, nullptr, nullptr, nullptr);
                if (FAILED(request))
                    attempt.error = HResultText(L"继续读取 Media Foundation 采集帧失败", request);
            }
            // 某些驱动会持续返回格式/流标志却不给 sample；超时必须以最后一帧为准，
            // 不能因为收到了非画面事件就无限等待。
            if (attempt.error.empty()) {
                const std::uint64_t checkedAt = GetTickCount64();
                const std::uint64_t silentFor = checkedAt >= lastFrameAt
                    ? checkedAt - lastFrameAt : 0;
                const std::uint64_t timeout = CaptureOutput::FrameSilenceTimeoutMs(
                    session->options, attempt.producedFrame);
                if (silentFor >= timeout) {
                    attempt.error = attempt.producedFrame
                        ? L"Media Foundation 视频流长时间无帧。"
                        : L"Media Foundation 已打开设备，但没有收到首帧。";
                }
            }
        }
        opened.Close();
        attempt.stopped = session->stopping.load(std::memory_order_relaxed);
        return attempt;
    }

    static AttemptResult RunDirectShowAttempt(const std::shared_ptr<Session>& session,
        bool fallback) {
        AttemptResult attempt;
        BeginAttempt(session, CaptureBackend::DirectShow,
            fallback ? L"标准采集方式未取得画面，正在尝试 DirectShow 兼容模式…"
                     : L"正在打开 DirectShow 兼容模式…", fallback);
        directshow::CaptureSession capture;
        directshow::OpenOptions options;
        options.stableId = session->options.directShowId;
        options.modePolicy = EffectiveModePolicy(session);
        options.exactModeKey = EffectiveModeKey(session);
        options.candidateOffset = session->directShowCandidateOffset;
        directshow::OpenResult opened;
        std::uint64_t lastFrameAt = GetTickCount64();
        const auto onFrame = [&](const directshow::FrameView& source, std::wstring& frameError) {
            std::shared_ptr<CaptureFrame> frame;
            if (!CopyDirectShowFrame(session, source, frame, frameError)) return false;
            const std::uint64_t now = GetTickCount64();
            lastFrameAt = now;
            attempt.producedFrame = true;
            Publish(session, std::move(frame), opened.nominalFps);
            return true;
        };
        if (!capture.Open(options, onFrame, opened, attempt.error)) return attempt;
        attempt.established = true;
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            session->status.state = CaptureState::WaitingSignal;
            session->status.message = opened.usedDriverDefault
                ? L"DirectShow 已采用驱动当前格式，正在等待视频信号…"
                : L"DirectShow 兼容模式已连接，正在等待视频信号…";
            session->status.width = opened.width;
            session->status.height = opened.height;
            session->status.nominalFps = opened.nominalFps;
            session->status.measuredFps = 0;
            if (!session->status.hasRequestedMode) {
                session->status.requestedModeKey = opened.requestedMode.stableKey;
                session->status.requestedMode = opened.requestedMode;
                session->status.hasRequestedMode = !opened.requestedMode.stableKey.empty();
            }
            session->status.actualMode = opened.actualMode;
            session->status.hasActualMode = !opened.actualMode.stableKey.empty();
        }
        Notify(session, 0, true);
        if (!capture.Run(attempt.error)) return attempt;
        lastFrameAt = GetTickCount64();

        while (attempt.error.empty() && !session->stopping.load(std::memory_order_relaxed)) {
            capture.WaitForFrame(50);
            if (session->stopping.load(std::memory_order_relaxed)) break;
            std::wstring pumpError;
            const auto pump = capture.Pump(pumpError);
            if (pump == directshow::PumpResult::Failure) {
                attempt.error = pumpError;
                break;
            }
            if (capture.PollFailure(attempt.error)) break;
            const std::uint64_t now = GetTickCount64();
            const std::uint64_t silentFor = now >= lastFrameAt ? now - lastFrameAt : 0;
            const std::uint64_t timeout = CaptureOutput::FrameSilenceTimeoutMs(
                session->options, attempt.producedFrame);
            if (silentFor >= timeout) {
                attempt.error = attempt.producedFrame
                    ? L"DirectShow 视频流长时间无帧。"
                    : L"DirectShow 已打开设备，但没有收到首帧。";
            }
        }
        capture.Close();
        attempt.stopped = session->stopping.load(std::memory_order_relaxed);
        return attempt;
    }

    static void DeviceWorker(const std::shared_ptr<Session>& session) {
        CaptureApiScope captureApis;
        std::wstring comError;
        if (!captureApis.StartCom(comError)) {
            SetStatus(session, CaptureState::Error, comError, true);
            return;
        }

        const bool mediaFoundationConfigured = !session->options.mediaFoundationId.empty();
        const bool hasDirectShow = !session->options.directShowId.empty();
        std::wstring mediaFoundationStartupError;
        const bool hasMediaFoundation = mediaFoundationConfigured &&
            captureApis.StartMediaFoundation(mediaFoundationStartupError);
        if (mediaFoundationConfigured && !hasMediaFoundation) {
            // MF 初始化失败只禁用当前线程的 MF 路径；DirectShow 仍可使用已经就绪的 COM。
            std::lock_guard<std::mutex> lock(session->mutex);
            session->status.mediaFoundationError = mediaFoundationStartupError;
        }
        if (!hasMediaFoundation && !hasDirectShow) {
            SetStatus(session, CaptureState::Error,
                mediaFoundationStartupError.empty()
                    ? L"所选设备没有可用的 Windows 采集入口。"
                    : mediaFoundationStartupError,
                true);
            return;
        }

        CaptureBackend preferred = hasMediaFoundation
            ? CaptureBackend::MediaFoundation : CaptureBackend::DirectShow;
        CaptureBackend next = preferred;
        bool mediaFoundationTried = false;
        bool directShowTried = false;
        unsigned mediaFoundationTargetRetries = 0;
        unsigned directShowTargetRetries = 0;
        while (!session->stopping.load(std::memory_order_relaxed)) {
            // fallbackUsed 表示实际采用兼容引擎，而不是普通重连；单后端重试不应误报回退。
            const bool fallback = next == CaptureBackend::DirectShow &&
                mediaFoundationConfigured;
            AttemptResult result = next == CaptureBackend::MediaFoundation
                ? RunMediaFoundationAttempt(session, fallback)
                : RunDirectShowAttempt(session, fallback);
            if (result.stopped) break;
            bool retryTargetCandidate = false;
            {
                std::lock_guard<std::mutex> lock(session->mutex);
                if (next == CaptureBackend::MediaFoundation) {
                    session->status.mediaFoundationError = result.error;
                    mediaFoundationTried = true;
                    if (result.producedFrame) session->mediaFoundationCandidateOffset = 0;
                    else if (result.established) {
                        ++session->mediaFoundationCandidateOffset;
                        if (session->autoTargetOnly && mediaFoundationTargetRetries++ == 0) {
                            mediaFoundationTried = false;
                            retryTargetCandidate = true;
                        }
                    }
                } else {
                    session->status.directShowError = result.error;
                    directShowTried = true;
                    if (result.producedFrame) session->directShowCandidateOffset = 0;
                    else if (result.established) {
                        ++session->directShowCandidateOffset;
                        if (session->autoTargetOnly && directShowTargetRetries++ == 0) {
                            directShowTried = false;
                            retryTargetCandidate = true;
                        }
                    }
                }
            }
            if (result.producedFrame) preferred = next;

            if (retryTargetCandidate) {
                // 建图成功但无首帧时，再给下一项等价原生格式一次机会；不存在目标的
                // Open 失败不会进入这里，仍会立即转到另一个后端。
                std::unique_lock<std::mutex> lock(session->mutex);
                session->wake.wait_for(lock, std::chrono::milliseconds(120), [&] {
                    return session->stopping.load(std::memory_order_relaxed);
                });
                continue;
            }

            const CaptureBackend alternative = next == CaptureBackend::MediaFoundation
                ? CaptureBackend::DirectShow : CaptureBackend::MediaFoundation;
            const bool alternativeAvailable = alternative == CaptureBackend::MediaFoundation
                ? hasMediaFoundation : hasDirectShow;
            const bool completeRound = (!hasMediaFoundation || mediaFoundationTried) &&
                (!hasDirectShow || directShowTried);
            if (!completeRound && alternativeAvailable) {
                next = alternative;
                // 部分专用采集驱动在 Shutdown/Stop 返回后仍需很短时间释放独占端点。
                // 用可取消等待完成后端交接，避免紧接着打开另一 API 时得到假性占用错误。
                std::unique_lock<std::mutex> lock(session->mutex);
                session->wake.wait_for(lock, std::chrono::milliseconds(120), [&] {
                    return session->stopping.load(std::memory_order_relaxed);
                });
                continue;
            }

            if (completeRound && session->autoTargetOnly) {
                // 两个后端都已验证过全局目标，再允许 Auto 选择其它模式。目标在当前后端
                // 根本不存在时 Open 会立即返回，因此不会先卡进可用但较低的 60 Hz。
                session->autoTargetOnly = false;
                mediaFoundationTried = directShowTried = false;
                mediaFoundationTargetRetries = directShowTargetRetries = 0;
                next = preferred;
                std::unique_lock<std::mutex> lock(session->mutex);
                session->wake.wait_for(lock, std::chrono::milliseconds(120), [&] {
                    return session->stopping.load(std::memory_order_relaxed);
                });
                continue;
            }

            {
                std::lock_guard<std::mutex> lock(session->mutex);
                ++session->status.reconnectCount;
                session->status.state = CaptureState::Reconnecting;
                // 保留最后一次后端给出的具体 Open/协商/首帧错误。完整的 MF 与 DS
                // 错误分别保存在诊断字段，不能再用笼统的 HDMI/占用猜测覆盖根因。
                session->status.message = result.error.empty()
                    ? L"采集方式未返回具体错误；正在重试…"
                    : result.error + L" 正在重试…";
                session->latest.reset();
            }
            Notify(session, 0, true);
            std::unique_lock<std::mutex> lock(session->mutex);
            session->wake.wait_for(lock,
                std::chrono::milliseconds(session->options.reconnectIntervalMs),
                [&] { return session->stopping.load(std::memory_order_relaxed); });
            lock.unlock();
            mediaFoundationTried = directShowTried = false;
            mediaFoundationTargetRetries = directShowTargetRetries = 0;
            // 普通 Auto 一轮仍失败后，下一轮先重新探测全局目标，兼容信号时序恢复。
            session->autoTargetOnly = !session->autoTargetKey.empty();
            next = preferred;
        }
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            session->status.backend = CaptureBackend::None;
        }
        SetStatus(session, CaptureState::Stopped, L"采集输出已停止", true);
    }

    static void Worker(const std::shared_ptr<Session>& session) {
        DeviceWorker(session);
    }

    static void ReportWorkerFailure(const std::shared_ptr<Session>& session,
        const wchar_t* message) noexcept {
        try {
            SetStatus(session, CaptureState::Error, message, true);
        } catch (...) {
            // 内存耗尽时连错误文案分配也可能失败，至少保留可查询的错误状态。
            try {
                std::lock_guard<std::mutex> lock(session->mutex);
                session->status.state = CaptureState::Error;
                session->latest.reset();
            } catch (...) {}
        }
    }

    void StopUnlocked() {
        auto oldSession = current;
        if (!oldSession) {
            if (worker.joinable()) worker.detach();
            return;
        }
        oldSession->stopping.store(true, std::memory_order_relaxed);
        oldSession->notificationTarget.store(nullptr, std::memory_order_relaxed);
        oldSession->wake.notify_all();
        current.reset();
        if (!worker.joinable()) return;
        const HANDLE handle = static_cast<HANDLE>(worker.native_handle());
        const DWORD wait = WaitForSingleObject(handle, kStopWaitMs);
        if (wait == WAIT_OBJECT_0) worker.join();
        else worker.detach();
    }
};

CaptureOutput::CaptureOutput() : impl(std::make_unique<Impl>()) {}

CaptureOutput::~CaptureOutput() {
    Stop();
}

bool CaptureOutput::EnumerateDevices(std::vector<CaptureDevice>& devices, std::wstring& error) {
    devices.clear();
    error.clear();
    CaptureApiScope captureApis;
    if (!captureApis.StartCom(error)) return false;

    std::wstring mediaFoundationError;
    bool mediaFoundationEnumerated = false;
    if (captureApis.StartMediaFoundation(mediaFoundationError)) {
        ComPtr<IMFAttributes> attributes;
        HRESULT result = MFCreateAttributes(&attributes, 1);
        if (SUCCEEDED(result)) result = attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
        IMFActivate** activations = nullptr;
        UINT32 count = 0;
        if (SUCCEEDED(result)) result = MFEnumDeviceSources(attributes.Get(), &activations, &count);
        if (FAILED(result)) {
            mediaFoundationError = HResultText(L"枚举 Media Foundation 视频设备失败", result);
        } else {
            mediaFoundationEnumerated = true;
            for (UINT32 index = 0; index < count; ++index) {
                wchar_t* friendlyName = nullptr;
                wchar_t* symbolicLink = nullptr;
                UINT32 friendlyLength = 0, linkLength = 0;
                const HRESULT nameResult = activations[index]->GetAllocatedString(
                    MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &friendlyName, &friendlyLength);
                const HRESULT linkResult = activations[index]->GetAllocatedString(
                    MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                    &symbolicLink, &linkLength);
                if (SUCCEEDED(nameResult) && SUCCEEDED(linkResult) && symbolicLink && *symbolicLink) {
                    CaptureDevice device;
                    device.friendlyName.assign(friendlyName ? friendlyName : L"视频采集设备",
                        friendlyName ? friendlyLength : 6);
                    device.symbolicLink.assign(symbolicLink, linkLength);
                    device.mediaFoundationId = device.symbolicLink;
                    device.mediaFoundationAvailable = true;
                    devices.push_back(std::move(device));
                }
                if (friendlyName) CoTaskMemFree(friendlyName);
                if (symbolicLink) CoTaskMemFree(symbolicLink);
                activations[index]->Release();
            }
            CoTaskMemFree(activations);
        }
    }

    std::vector<directshow::Device> directShowDevices;
    std::wstring directShowError;
    const bool directShowEnumerated = directshow::EnumerateDevices(directShowDevices,
        directShowError);
    if (directShowEnumerated) {
        for (const auto& source : directShowDevices) {
            auto existing = source.devicePath.empty() ? devices.end() :
                std::find_if(devices.begin(), devices.end(), [&](const CaptureDevice& device) {
                    return SameDevicePath(device.mediaFoundationId, source.devicePath);
                });
            if (existing != devices.end()) {
                existing->directShowId = source.stableId;
                existing->directShowAvailable = true;
                continue;
            }
            CaptureDevice device;
            device.friendlyName = source.friendlyName;
            device.symbolicLink = L"dshow1|" +
                (source.devicePath.empty() ? source.stableId : source.devicePath);
            device.directShowId = source.stableId;
            device.directShowAvailable = true;
            devices.push_back(std::move(device));
        }
    }

    std::sort(devices.begin(), devices.end(), DeviceLess);
    devices.erase(std::unique(devices.begin(), devices.end(), [](const auto& left, const auto& right) {
        return CompareStringOrdinal(left.symbolicLink.c_str(), -1,
            right.symbolicLink.c_str(), -1, TRUE) == CSTR_EQUAL;
    }), devices.end());
    if (!mediaFoundationEnumerated && !directShowEnumerated) {
        error = mediaFoundationError;
        if (!directShowError.empty()) {
            if (!error.empty()) error += L"；";
            error += directShowError;
        }
        return false;
    }
    error.clear();
    return true;
}

bool CaptureOutput::EnumerateModes(const std::wstring& symbolicLink,
    std::vector<CaptureNativeMode>& modes, std::wstring& error) {
    CaptureDevice device;
    device.symbolicLink = symbolicLink;
    constexpr wchar_t directShowPrefix[] = L"dshow1|";
    if (symbolicLink.compare(0, std::size(directShowPrefix) - 1, directShowPrefix) == 0) {
        device.directShowId = symbolicLink.substr(std::size(directShowPrefix) - 1);
        device.directShowAvailable = true;
    } else {
        device.mediaFoundationId = symbolicLink;
        device.directShowId = symbolicLink;
        device.mediaFoundationAvailable = true;
        device.directShowAvailable = true;
    }
    return EnumerateModes(device, modes, error);
}

bool CaptureOutput::EnumerateModes(const CaptureDevice& device,
    std::vector<CaptureNativeMode>& modes, std::wstring& error) {
    modes.clear();
    error.clear();
    if (device.symbolicLink.empty() && device.mediaFoundationId.empty() &&
            device.directShowId.empty()) {
        error = L"请先选择视频采集设备。";
        return false;
    }
    CaptureApiScope captureApis;
    if (!captureApis.StartCom(error)) return false;

    std::wstring mediaFoundationError;
    std::vector<CaptureNativeMode> mediaFoundationModes;
    const std::wstring mediaFoundationId = !device.mediaFoundationId.empty()
        ? device.mediaFoundationId
        : (device.mediaFoundationAvailable ? device.symbolicLink : std::wstring());
    if (!mediaFoundationId.empty() && captureApis.StartMediaFoundation(mediaFoundationError)) {
        ComPtr<IMFAttributes> sourceAttributes;
        HRESULT result = MFCreateAttributes(&sourceAttributes, 3);
        if (SUCCEEDED(result)) result = sourceAttributes->SetGUID(
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
        if (SUCCEEDED(result)) result = sourceAttributes->SetString(
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
            mediaFoundationId.c_str());
        if (SUCCEEDED(result)) result = sourceAttributes->SetUINT32(
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_MAX_BUFFERS, 1);

        ComPtr<IMFMediaSource> source;
        if (SUCCEEDED(result)) result = MFCreateDeviceSource(sourceAttributes.Get(), &source);
        ComPtr<IMFSourceReader> reader;
        if (SUCCEEDED(result)) result = MFCreateSourceReaderFromMediaSource(source.Get(),
            nullptr, &reader);
        if (SUCCEEDED(result)) {
            reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
            result = reader->SetStreamSelection(kVideoStream, TRUE);
        }
        if (SUCCEEDED(result)) {
            std::vector<VideoMode> nativeModes;
            if (CollectAvailableModes(reader.Get(), source.Get(), nativeModes,
                    mediaFoundationError)) {
                mediaFoundationModes.reserve(nativeModes.size());
                for (auto& mode : nativeModes)
                    mediaFoundationModes.push_back(std::move(mode.description));
            }
        } else {
            mediaFoundationError = HResultText(L"Media Foundation 无法读取设备规格", result);
        }
        reader.Reset();
        if (source) source->Shutdown();
        source.Reset();
    }

    std::wstring directShowId = device.directShowId;
    if (directShowId.empty() && device.directShowAvailable) directShowId = device.symbolicLink;
    std::vector<CaptureNativeMode> directShowModes;
    std::wstring directShowError;
    if (!directShowId.empty())
        directshow::EnumerateModes(directShowId, directShowModes, directShowError);

    // MF 仍是同规格时的真相源，但无论它是否返回条目，都补入 DirectShow 独有档。
    modes = std::move(mediaFoundationModes);
    MergeModes(modes, directShowModes);
    if (!modes.empty()) {
        error.clear();
        return true;
    }
    error = mediaFoundationError;
    if (!directShowError.empty()) {
        if (!error.empty()) error += L"；";
        error += directShowError;
    }
    if (error.empty()) error = L"采集设备没有公开可用的视频规格。";
    return false;
}

bool CaptureOutput::Start(const CaptureOptions& options, std::wstring& error) {
    error.clear();
    CaptureOptions normalized = options;
    if (!normalized.backendIdsResolved &&
            normalized.mediaFoundationId.empty() &&
            normalized.directShowId.empty() && !normalized.symbolicLink.empty()) {
        constexpr wchar_t directShowPrefix[] = L"dshow1|";
        if (normalized.symbolicLink.compare(0, std::size(directShowPrefix) - 1,
                directShowPrefix) == 0) {
            normalized.directShowId = normalized.symbolicLink.substr(
                std::size(directShowPrefix) - 1);
        } else {
            normalized.mediaFoundationId = normalized.symbolicLink;
            // DirectShow 的 DevicePath 通常与 MF 符号链接一致；旧调用方仍可获得自动回退。
            normalized.directShowId = normalized.symbolicLink;
        }
    }
    if (!normalized.backendIdsResolved &&
            normalized.directShowId.empty() &&
            !normalized.mediaFoundationId.empty()) {
        normalized.directShowId = normalized.mediaFoundationId;
    }
    if (normalized.symbolicLink.empty() &&
            normalized.mediaFoundationId.empty() && normalized.directShowId.empty()) {
        error = L"请先选择视频采集设备。";
        return false;
    }
    if (normalized.modePolicy != CaptureModePolicy::Auto &&
            normalized.exactModeKey.empty()) {
        error = normalized.modePolicy == CaptureModePolicy::Exact
            ? L"精确采集模式缺少稳定标识。"
            : L"帧率优先模式缺少画面规格标识。";
        return false;
    }
    if (normalized.firstFrameTimeoutMs < 250 || normalized.firstFrameTimeoutMs > 60000 ||
        normalized.signalTimeoutMs < 250 || normalized.signalTimeoutMs > 60000 ||
        normalized.reconnectIntervalMs < 250 || normalized.reconnectIntervalMs > 60000) {
        error = L"首帧等待、断流超时和重连间隔必须在 250～60000 毫秒之间。";
        return false;
    }
    std::lock_guard<std::mutex> operation(impl->controlMutex);
    impl->StopUnlocked();
    auto session = std::make_shared<Impl::Session>(std::move(normalized));
    impl->current = session;
    try {
        impl->worker = std::thread([session] {
            try {
                Impl::Worker(session);
            } catch (const std::bad_alloc&) {
                Impl::ReportWorkerFailure(session, L"视频帧内存不足，采集已停止。");
            } catch (const std::exception&) {
                Impl::ReportWorkerFailure(session, L"视频采集线程异常，采集已停止。");
            } catch (...) {
                Impl::ReportWorkerFailure(session, L"视频采集线程发生未知异常，采集已停止。");
            }
            session->workerRunning.store(false, std::memory_order_release);
        });
    } catch (const std::exception&) {
        impl->current.reset();
        error = L"创建视频采集线程失败。";
        return false;
    }
    return true;
}

void CaptureOutput::Stop() {
    std::lock_guard<std::mutex> operation(impl->controlMutex);
    impl->StopUnlocked();
}

CaptureStatus CaptureOutput::Status() const {
    std::shared_ptr<Impl::Session> session;
    {
        std::lock_guard<std::mutex> operation(impl->controlMutex);
        session = impl->current;
    }
    if (!session) return {CaptureState::Stopped, L"采集输出已停止"};
    std::lock_guard<std::mutex> lock(session->mutex);
    return session->status;
}

bool CaptureOutput::Active() const {
    std::lock_guard<std::mutex> operation(impl->controlMutex);
    return impl->current && impl->current->workerRunning.load(std::memory_order_acquire);
}

std::shared_ptr<const CaptureFrame> CaptureOutput::LatestFrame() const {
    std::shared_ptr<Impl::Session> session;
    {
        std::lock_guard<std::mutex> operation(impl->controlMutex);
        session = impl->current;
    }
    if (!session) return {};
    std::lock_guard<std::mutex> lock(session->mutex);
    return session->latest;
}

void CaptureOutput::AcknowledgeNotification() {
    std::shared_ptr<Impl::Session> session;
    {
        std::lock_guard<std::mutex> operation(impl->controlMutex);
        session = impl->current;
    }
    if (session) session->notificationPending.store(false, std::memory_order_release);
}

RECT CaptureOutput::FitRect(const RECT& client, int frameWidth, int frameHeight) {
    RECT result{client.left, client.top, client.left, client.top};
    const long long clientWidth = static_cast<long long>(client.right) - client.left;
    const long long clientHeight = static_cast<long long>(client.bottom) - client.top;
    if (clientWidth <= 0 || clientHeight <= 0 || frameWidth <= 0 || frameHeight <= 0) return result;
    long long width = clientWidth;
    long long height = clientHeight;
    if (clientWidth * frameHeight > clientHeight * frameWidth) {
        width = clientHeight * frameWidth / frameHeight;
    } else {
        height = clientWidth * frameHeight / frameWidth;
    }
    result.left = client.left + static_cast<LONG>((clientWidth - width) / 2);
    result.top = client.top + static_cast<LONG>((clientHeight - height) / 2);
    result.right = result.left + static_cast<LONG>(width);
    result.bottom = result.top + static_cast<LONG>(height);
    return result;
}

RECT CaptureOutput::Paint(HDC dc, const RECT& client, COLORREF background) const {
    HBRUSH brush = CreateSolidBrush(background);
    if (brush) {
        FillRect(dc, &client, brush);
        DeleteObject(brush);
    }
    const auto frame = LatestFrame();
    if (!dc || !frame || frame->width <= 0 || frame->height <= 0 || frame->bgra.empty()) {
        return {client.left, client.top, client.left, client.top};
    }
    const RECT destination = FitRect(client, frame->width, frame->height);
    BITMAPINFO bitmap{};
    bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap.bmiHeader.biWidth = frame->width;
    bitmap.bmiHeader.biHeight = -frame->height;
    bitmap.bmiHeader.biPlanes = 1;
    bitmap.bmiHeader.biBitCount = 32;
    bitmap.bmiHeader.biCompression = BI_RGB;
    const int oldMode = SetStretchBltMode(dc, HALFTONE);
    POINT oldOrigin{};
    SetBrushOrgEx(dc, 0, 0, &oldOrigin);
    StretchDIBits(dc, destination.left, destination.top,
        destination.right - destination.left, destination.bottom - destination.top,
        0, 0, frame->width, frame->height, frame->bgra.data(), &bitmap,
        DIB_RGB_COLORS, SRCCOPY);
    SetBrushOrgEx(dc, oldOrigin.x, oldOrigin.y, nullptr);
    SetStretchBltMode(dc, oldMode);
    return destination;
}

}  // namespace qpointer
