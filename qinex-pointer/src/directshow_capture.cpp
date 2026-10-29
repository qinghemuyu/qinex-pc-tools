#include "directshow_capture.h"

#include <dshow.h>
#include <dvdmedia.h>
#include <objbase.h>
#include <oleauto.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <limits>
#include <mutex>
#include <new>
#include <sstream>
#include <utility>

#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

namespace qpointer::directshow {
namespace {
using Microsoft::WRL::ComPtr;

// Sample Grabber 属于 DirectShow 的系统组件，但新版 Windows SDK 已不再附带 qedit.h。
// 在这里保留公开 COM 契约即可使用系统组件，无需随程序携带额外 DLL。
struct __declspec(uuid("0579154A-2B53-4994-B0D0-E773148EFF85"))
IQpSampleGrabberCallback : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SampleCB(double sampleTime, IMediaSample* sample) = 0;
    virtual HRESULT STDMETHODCALLTYPE BufferCB(double sampleTime, BYTE* buffer, long length) = 0;
};

struct __declspec(uuid("6B652FFF-11FE-4FCE-92AD-0266B5D7C78F"))
IQpSampleGrabber : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetOneShot(BOOL oneShot) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMediaType(const AM_MEDIA_TYPE* type) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetConnectedMediaType(AM_MEDIA_TYPE* type) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetBufferSamples(BOOL bufferSamples) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentBuffer(long* size, long* buffer) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentSample(IMediaSample** sample) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCallback(IQpSampleGrabberCallback* callback, long method) = 0;
};

constexpr GUID kClsidSampleGrabber{
    0xC1F400A0, 0x3F08, 0x11D3, {0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37}};
constexpr GUID kClsidNullRenderer{
    0xC1F400A4, 0x3F08, 0x11D3, {0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37}};
constexpr std::uint32_t kInterlaceProgressive = 2;
constexpr std::uint32_t kInterlaceUpperFirst = 3;
constexpr std::uint32_t kInterlaceLowerFirst = 4;
constexpr unsigned kMaximumFrameWidth = 8192;
constexpr unsigned kMaximumFrameHeight = 4320;

bool SameGuid(const GUID& left, const GUID& right) {
    return InlineIsEqualGUID(left, right) != FALSE;
}

bool EqualInsensitive(const std::wstring& left, const std::wstring& right) {
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
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
    if (EqualInsensitive(left, right)) return true;
    const std::wstring leftKey = DeviceHardwareKey(left);
    const std::wstring rightKey = DeviceHardwareKey(right);
    return !leftKey.empty() && !rightKey.empty() && EqualInsensitive(leftKey, rightKey);
}

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

std::wstring GuidText(const GUID& value) {
    wchar_t text[40]{};
    swprintf_s(text, L"{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
        value.Data1, value.Data2, value.Data3, value.Data4[0], value.Data4[1],
        value.Data4[2], value.Data4[3], value.Data4[4], value.Data4[5],
        value.Data4[6], value.Data4[7]);
    return text;
}

bool IsFourccSubtype(const GUID& value) {
    static constexpr BYTE suffix[8]{0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
    return value.Data2 == 0 && value.Data3 == 0x0010 &&
        std::memcmp(value.Data4, suffix, sizeof(suffix)) == 0;
}

std::wstring FourccText(const GUID& subtype) {
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
    if (SameGuid(subtype, MEDIASUBTYPE_RGB32)) return L"RGB32";
    if (SameGuid(subtype, MEDIASUBTYPE_ARGB32)) return L"ARGB32";
    if (SameGuid(subtype, MEDIASUBTYPE_NV12)) return L"NV12";
    if (SameGuid(subtype, MEDIASUBTYPE_YUY2)) return L"YUY2";
    if (SameGuid(subtype, MEDIASUBTYPE_UYVY)) return L"UYVY";
    if (SameGuid(subtype, MEDIASUBTYPE_MJPG)) return L"Motion JPEG";
    if (SameGuid(subtype, MEDIASUBTYPE_H264)) return L"H.264";
    return fourcc.empty() ? GuidText(subtype) : fourcc;
}

bool KnownUncompressedSubtype(const GUID& subtype) {
    return SameGuid(subtype, MEDIASUBTYPE_RGB32) || SameGuid(subtype, MEDIASUBTYPE_ARGB32) ||
        SameGuid(subtype, MEDIASUBTYPE_RGB24) || SameGuid(subtype, MEDIASUBTYPE_NV12) ||
        SameGuid(subtype, MEDIASUBTYPE_YUY2) || SameGuid(subtype, MEDIASUBTYPE_UYVY) ||
        SameGuid(subtype, MEDIASUBTYPE_YV12) || SameGuid(subtype, MEDIASUBTYPE_IYUV);
}

int GrabberConversionPreference(const GUID& subtype) {
    // Sample Grabber 最终接收 RGB32。Windows 自带的 DirectShow 转换链对 RGB 和
    // packed YUV 最稳定；NV12 通常需要额外的 DMO/MFT 包装，放在同规格候选之后。
    if (SameGuid(subtype, MEDIASUBTYPE_RGB32)) return 8;
    if (SameGuid(subtype, MEDIASUBTYPE_ARGB32)) return 7;
    if (SameGuid(subtype, MEDIASUBTYPE_RGB24)) return 6;
    if (SameGuid(subtype, MEDIASUBTYPE_YUY2) || SameGuid(subtype, MEDIASUBTYPE_UYVY)) return 5;
    if (SameGuid(subtype, MEDIASUBTYPE_MJPG)) return 3;
    if (SameGuid(subtype, MEDIASUBTYPE_NV12)) return 2;
    if (SameGuid(subtype, MEDIASUBTYPE_H264)) return 1;
    return 0;
}

void FreeMediaType(AM_MEDIA_TYPE& type) {
    if (type.cbFormat && type.pbFormat) CoTaskMemFree(type.pbFormat);
    if (type.pUnk) type.pUnk->Release();
    type = {};
}

struct MediaTypeDeleter {
    void operator()(AM_MEDIA_TYPE* type) const noexcept {
        if (!type) return;
        FreeMediaType(*type);
        CoTaskMemFree(type);
    }
};
using OwnedMediaType = std::unique_ptr<AM_MEDIA_TYPE, MediaTypeDeleter>;

struct VideoMode {
    OwnedMediaType type;
    CaptureNativeMode description;
};

const BITMAPINFOHEADER* BitmapHeader(const AM_MEDIA_TYPE& type,
    REFERENCE_TIME* frameInterval = nullptr, DWORD* interlaceFlags = nullptr,
    bool* scanKnown = nullptr) {
    if (frameInterval) *frameInterval = 0;
    if (interlaceFlags) *interlaceFlags = 0;
    if (scanKnown) *scanKnown = false;
    if (SameGuid(type.formattype, FORMAT_VideoInfo) &&
            type.cbFormat >= sizeof(VIDEOINFOHEADER) && type.pbFormat) {
        const auto* info = reinterpret_cast<const VIDEOINFOHEADER*>(type.pbFormat);
        if (frameInterval) *frameInterval = info->AvgTimePerFrame;
        return &info->bmiHeader;
    }
    if (SameGuid(type.formattype, FORMAT_VideoInfo2) &&
            type.cbFormat >= sizeof(VIDEOINFOHEADER2) && type.pbFormat) {
        const auto* info = reinterpret_cast<const VIDEOINFOHEADER2*>(type.pbFormat);
        if (frameInterval) *frameInterval = info->AvgTimePerFrame;
        if (interlaceFlags) *interlaceFlags = info->dwInterlaceFlags;
        if (scanKnown) *scanKnown = true;
        return &info->bmiHeader;
    }
    return nullptr;
}

bool ReadMode(const AM_MEDIA_TYPE& type, CaptureNativeMode& mode) {
    if (!SameGuid(type.majortype, MEDIATYPE_Video)) return false;
    REFERENCE_TIME interval = 0;
    DWORD interlaceFlags = 0;
    bool scanKnown = false;
    const BITMAPINFOHEADER* bitmap = BitmapHeader(type, &interval, &interlaceFlags,
        &scanKnown);
    if (!bitmap || bitmap->biWidth == 0 || bitmap->biHeight == 0) return false;
    const auto width = static_cast<unsigned>(std::abs(static_cast<long long>(bitmap->biWidth)));
    const auto height = static_cast<unsigned>(std::abs(static_cast<long long>(bitmap->biHeight)));
    if (!width || !height || width > 16384 || height > 16384) return false;

    mode = {};
    mode.width = width;
    mode.height = height;
    if (interval > 0 && interval <= std::numeric_limits<std::uint32_t>::max()) {
        mode.fpsNumerator = 10000000;
        mode.fpsDenominator = static_cast<std::uint32_t>(interval);
    }
    mode.subtype = type.subtype;
    mode.subtypeFourcc = FourccText(type.subtype);
    mode.subtypeName = SubtypeName(type.subtype, mode.subtypeFourcc);
    mode.compressed = !KnownUncompressedSubtype(type.subtype);
    if (!scanKnown) {
        // VIDEOINFOHEADER 没有扫描方式字段，必须保留 Unknown；把缺失信息写成
        // Progressive 会让同一高刷规格在 MF/DirectShow 之间产生假性不匹配。
        mode.progressive = false;
        mode.interlaceMode = 0;
    } else if (interlaceFlags & AMINTERLACE_IsInterlaced) {
        mode.progressive = false;
        mode.interlaceMode = interlaceFlags & AMINTERLACE_Field1First
            ? kInterlaceUpperFirst : kInterlaceLowerFirst;
    } else {
        mode.progressive = true;
        mode.interlaceMode = kInterlaceProgressive;
    }
    mode.stableKey = CaptureOutput::BuildModeStableKey(mode);
    return true;
}

std::wstring ReadBagString(IPropertyBag* bag, const wchar_t* name) {
    if (!bag) return {};
    VARIANT value;
    VariantInit(&value);
    std::wstring result;
    if (SUCCEEDED(bag->Read(name, &value, nullptr)) && value.vt == VT_BSTR && value.bstrVal)
        result = value.bstrVal;
    VariantClear(&value);
    return result;
}

std::wstring MonikerName(IMoniker* moniker) {
    ComPtr<IBindCtx> context;
    LPOLESTR name = nullptr;
    if (!moniker || FAILED(CreateBindCtx(0, &context)) ||
            FAILED(moniker->GetDisplayName(context.Get(), nullptr, &name)) || !name) return {};
    std::wstring result = name;
    CoTaskMemFree(name);
    return result;
}

bool EnumerateMonikers(const std::function<bool(IMoniker*, const Device&)>& visitor,
    std::wstring& error) {
    ComPtr<ICreateDevEnum> deviceEnumerator;
    HRESULT result = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&deviceEnumerator));
    if (FAILED(result)) {
        error = HResultText(L"初始化 DirectShow 设备枚举失败", result);
        return false;
    }
    ComPtr<IEnumMoniker> monikers;
    result = deviceEnumerator->CreateClassEnumerator(CLSID_VideoInputDeviceCategory,
        &monikers, 0);
    if (result == S_FALSE) {
        error.clear();
        return true;
    }
    if (FAILED(result)) {
        error = HResultText(L"枚举 DirectShow 视频设备失败", result);
        return false;
    }

    ComPtr<IMoniker> moniker;
    ULONG fetched = 0;
    while (monikers->Next(1, &moniker, &fetched) == S_OK) {
        Device device;
        device.stableId = MonikerName(moniker.Get());
        ComPtr<IPropertyBag> bag;
        if (SUCCEEDED(moniker->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&bag)))) {
            device.friendlyName = ReadBagString(bag.Get(), L"FriendlyName");
            device.devicePath = ReadBagString(bag.Get(), L"DevicePath");
        }
        if (device.friendlyName.empty()) device.friendlyName = L"DirectShow 视频采集设备";
        if (!device.stableId.empty() && !visitor(moniker.Get(), device)) break;
        moniker.Reset();
    }
    error.clear();
    return true;
}

bool BindDevice(const std::wstring& stableId, ComPtr<IBaseFilter>& source,
    std::wstring& error) {
    if (stableId.empty()) {
        error = L"DirectShow 设备标识为空。";
        return false;
    }
    bool found = false;
    HRESULT bindResult = E_FAIL;
    const bool enumerated = EnumerateMonikers([&](IMoniker* moniker, const Device& device) {
        if (!EqualInsensitive(device.stableId, stableId) &&
                !SameDevicePath(device.devicePath, stableId)) return true;
        found = true;
        bindResult = moniker->BindToObject(nullptr, nullptr, IID_PPV_ARGS(&source));
        return false;
    }, error);
    if (!enumerated) return false;
    if (!found) {
        error = L"DirectShow 找不到所选采集设备。";
        return false;
    }
    if (FAILED(bindResult) || !source) {
        error = HResultText(L"DirectShow 无法打开所选采集设备", bindResult);
        return false;
    }
    return true;
}

enum class StreamPinKind {
    Capture,
    Preview,
    Any
};

struct StreamPinSelection {
    StreamPinKind kind = StreamPinKind::Capture;
    int ordinal = 0;
};

struct ModeCandidate {
    CaptureNativeMode description;
    StreamPinSelection pin;
    std::size_t equivalentOrdinal = 0;
};

const GUID* PinCategory(StreamPinKind kind) {
    if (kind == StreamPinKind::Capture) return &PIN_CATEGORY_CAPTURE;
    if (kind == StreamPinKind::Preview) return &PIN_CATEGORY_PREVIEW;
    return nullptr;
}

bool FindStreamPin(ICaptureGraphBuilder2* builder, IBaseFilter* source,
    const StreamPinSelection* preferred, ComPtr<IPin>& pin,
    ComPtr<IAMStreamConfig>& config, StreamPinSelection& selected) {
    if (!builder || !source) return false;
    pin.Reset();
    config.Reset();
    const auto tryPin = [&](StreamPinKind kind, int ordinal, bool requireConfig) {
        ComPtr<IPin> candidate;
        const HRESULT result = builder->FindPin(source, PINDIR_OUTPUT, PinCategory(kind),
            &MEDIATYPE_Video, TRUE, ordinal, &candidate);
        if (FAILED(result) || !candidate) return false;
        ComPtr<IAMStreamConfig> candidateConfig;
        candidate.As(&candidateConfig);
        if (requireConfig && !candidateConfig) return false;
        pin = std::move(candidate);
        config = std::move(candidateConfig);
        selected.kind = kind;
        selected.ordinal = ordinal;
        return true;
    };
    if (preferred) {
        return tryPin(preferred->kind, preferred->ordinal, false);
    }

    // 多路采集滤镜可能公开多个同类别输出 pin。枚举和设置格式时必须记住具体
    // pin；稍后若仅凭 PIN_CATEGORY_CAPTURE 再搜索一次，驱动可能返回另一条流。
    for (const StreamPinKind kind : {StreamPinKind::Capture, StreamPinKind::Preview,
            StreamPinKind::Any}) {
        for (int ordinal = 0;; ++ordinal) {
            ComPtr<IPin> candidate;
            const HRESULT result = builder->FindPin(source, PINDIR_OUTPUT, PinCategory(kind),
                &MEDIATYPE_Video, TRUE, ordinal, &candidate);
            if (FAILED(result) || !candidate) break;
            ComPtr<IAMStreamConfig> candidateConfig;
            candidate.As(&candidateConfig);
            if (candidateConfig) {
                pin = std::move(candidate);
                config = std::move(candidateConfig);
                selected.kind = kind;
                selected.ordinal = ordinal;
                return true;
            }
        }
    }
    // 少数旧驱动没有 IAMStreamConfig；自动模式仍可连接其第一条视频输出。
    return tryPin(StreamPinKind::Capture, 0, false) ||
        tryPin(StreamPinKind::Preview, 0, false) ||
        tryPin(StreamPinKind::Any, 0, false);
}

bool SameStreamPin(const StreamPinSelection& left, const StreamPinSelection& right) {
    return left.kind == right.kind && left.ordinal == right.ordinal;
}

void AppendStreamPin(std::vector<StreamPinSelection>& pins,
    const StreamPinSelection& candidate) {
    if (std::none_of(pins.begin(), pins.end(), [&](const StreamPinSelection& existing) {
            return SameStreamPin(existing, candidate);
        })) pins.push_back(candidate);
}

void CollectConfiguredStreamPins(ICaptureGraphBuilder2* builder, IBaseFilter* source,
    std::vector<StreamPinSelection>& pins) {
    pins.clear();
    if (!builder || !source) return;
    const auto collectKind = [&](StreamPinKind kind) {
        for (int ordinal = 0;; ++ordinal) {
            ComPtr<IPin> pin;
            const HRESULT result = builder->FindPin(source, PINDIR_OUTPUT, PinCategory(kind),
                &MEDIATYPE_Video, TRUE, ordinal, &pin);
            if (FAILED(result) || !pin) break;
            ComPtr<IAMStreamConfig> config;
            pin.As(&config);
            if (config) AppendStreamPin(pins, {kind, ordinal});
        }
    };

    // 采集卡可能同时公开主采集流、预览流以及同类别的多路输出。规格枚举与开流必须
    // 覆盖每条有 IAMStreamConfig 的流，不能让 FindPin 的首项决定整个设备的能力。
    collectKind(StreamPinKind::Capture);
    collectKind(StreamPinKind::Preview);
    // 少数旧驱动没有 pin category。仅在标准类别均不可用时遍历未分类视频输出，避免
    // 把同一条 capture/preview pin 通过 Any 再加入一次。
    if (pins.empty()) collectKind(StreamPinKind::Any);
}

bool PreferredModeOrder(const CaptureNativeMode& left, const CaptureNativeMode& right) {
    if (std::abs(left.Fps() - right.Fps()) > 0.001) return left.Fps() > right.Fps();
    const std::uint64_t leftPixels = static_cast<std::uint64_t>(left.width) * left.height;
    const std::uint64_t rightPixels = static_cast<std::uint64_t>(right.width) * right.height;
    if (leftPixels != rightPixels) return leftPixels > rightPixels;
    const int leftConversion = GrabberConversionPreference(left.subtype);
    const int rightConversion = GrabberConversionPreference(right.subtype);
    if (leftConversion != rightConversion) return leftConversion > rightConversion;
    return left.stableKey < right.stableKey;
}

bool CollectModes(IAMStreamConfig* config, std::vector<VideoMode>& modes,
    std::wstring& error) {
    modes.clear();
    if (!config) {
        error = L"DirectShow 设备没有公开视频规格接口。";
        return false;
    }
    int count = 0, capabilitySize = 0;
    HRESULT result = config->GetNumberOfCapabilities(&count, &capabilitySize);
    if (FAILED(result) || count < 0 || capabilitySize <= 0 || capabilitySize > 65536) {
        error = FAILED(result) ? HResultText(L"读取 DirectShow 规格数量失败", result)
                               : L"DirectShow 返回的规格描述无效。";
        return false;
    }
    std::vector<BYTE> capabilities(static_cast<std::size_t>(capabilitySize));
    for (int index = 0; index < count; ++index) {
        AM_MEDIA_TYPE* raw = nullptr;
        result = config->GetStreamCaps(index, &raw, capabilities.data());
        OwnedMediaType type(raw);
        CaptureNativeMode description;
        if (SUCCEEDED(result) && type && ReadMode(*type, description)) {
            // 相同尺寸、帧率和像素格式仍可能有不同的 format block。尤其是
            // FORMAT_VideoInfo 与 FORMAT_VideoInfo2 的可连接性不同，内部协商时
            // 必须全部保留；只在向 UI 返回描述时去重。
            modes.push_back({std::move(type), std::move(description)});
        }
    }
    if (modes.empty()) {
        error = L"DirectShow 设备没有报告可用的视频规格。";
        return false;
    }
    std::stable_sort(modes.begin(), modes.end(), [](const VideoMode& left, const VideoMode& right) {
        return PreferredModeOrder(left.description, right.description);
    });
    error.clear();
    return true;
}

bool ReadRgb32Format(const AM_MEDIA_TYPE& connected, int& width, int& height,
    int& stride, bool& bottomUp, double& fps, std::wstring& error) {
    CaptureNativeMode outputMode;
    const BITMAPINFOHEADER* bitmap = BitmapHeader(connected);
    if (!ReadMode(connected, outputMode) || !bitmap ||
            outputMode.width > kMaximumFrameWidth || outputMode.height > kMaximumFrameHeight ||
            !SameGuid(connected.subtype, MEDIASUBTYPE_RGB32)) {
        error = L"DirectShow 没有生成可用的 RGB32 视频输出。";
        return false;
    }
    width = static_cast<int>(outputMode.width);
    height = static_cast<int>(outputMode.height);
    const std::size_t minimumStride = static_cast<std::size_t>(width) * 4;
    // RGB32 每像素四字节，天然满足 DIB 的 DWORD 行对齐；biSizeImage 可能包含尾部填充，
    // 不能把它平均分配到每一行。
    stride = static_cast<int>(minimumStride);
    bottomUp = bitmap->biHeight > 0;
    fps = outputMode.Fps();
    error.clear();
    return true;
}

bool ReadConnectedFormat(IQpSampleGrabber* grabber, int& width, int& height,
    int& stride, bool& bottomUp, double& fps, std::wstring& error) {
    AM_MEDIA_TYPE connected{};
    const HRESULT result = grabber ? grabber->GetConnectedMediaType(&connected) : E_POINTER;
    if (FAILED(result)) {
        error = HResultText(L"读取 DirectShow 输出格式失败", result);
        return false;
    }
    const bool valid = ReadRgb32Format(connected, width, height, stride, bottomUp,
        fps, error);
    FreeMediaType(connected);
    return valid;
}

struct SampleSlot {
    ComPtr<IMediaSample> sample;
    double sampleTime = 0;
    std::uint64_t arrivalQpc = 0;
    std::uint64_t mailboxSkippedFrames = 0;
};

class GrabberCallback final : public IQpSampleGrabberCallback {
public:
    explicit GrabberCallback(HANDLE valueEvent) : sampleEvent(valueEvent) {}

    STDMETHODIMP QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IQpSampleGrabberCallback)) {
            *object = static_cast<IQpSampleGrabberCallback*>(this);
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
    STDMETHODIMP SampleCB(double sampleTime, IMediaSample* sample) override {
        if (!active.load(std::memory_order_acquire) || !sample) return S_OK;
        LARGE_INTEGER counter{};
        QueryPerformanceCounter(&counter);
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!active.load(std::memory_order_relaxed)) return S_OK;
            if (latest.sample) ++mailboxSkippedFrames;
            latest.sample = sample;
            latest.sampleTime = sampleTime;
            latest.arrivalQpc = static_cast<std::uint64_t>(counter.QuadPart);
            latest.mailboxSkippedFrames = mailboxSkippedFrames;
            // 与 Deactivate/CloseHandle 共用同一把锁，避免关闭时仍有回调在锁外触碰旧句柄。
            if (sampleEvent) SetEvent(sampleEvent);
        }
        return S_OK;
    }
    STDMETHODIMP BufferCB(double, BYTE*, long) override { return E_NOTIMPL; }

    bool Take(SampleSlot& output) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!latest.sample) return false;
        output = std::move(latest);
        latest = {};
        return true;
    }
    void Deactivate() {
        active.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(mutex);
            latest = {};
            if (sampleEvent) SetEvent(sampleEvent);
        }
    }

private:
    ~GrabberCallback() = default;
    std::atomic<ULONG> references{1};
    std::atomic<bool> active{true};
    std::mutex mutex;
    SampleSlot latest;
    std::uint64_t mailboxSkippedFrames = 0;
    HANDLE sampleEvent = nullptr;
};

HRESULT BuildGraph(const std::wstring& stableId, StreamPinKind preferredPin,
    ComPtr<IGraphBuilder>& graph,
    ComPtr<ICaptureGraphBuilder2>& builder, ComPtr<IBaseFilter>& source,
    ComPtr<IPin>& streamPin, ComPtr<IAMStreamConfig>& streamConfig,
    StreamPinSelection& selectedPin,
    std::wstring& error) {
    HRESULT result = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&graph));
    if (SUCCEEDED(result)) result = CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&builder));
    if (SUCCEEDED(result)) result = builder->SetFiltergraph(graph.Get());
    if (FAILED(result)) {
        error = HResultText(L"创建 DirectShow 采集图表失败", result);
        return result;
    }
    if (!BindDevice(stableId, source, error)) return E_FAIL;
    result = graph->AddFilter(source.Get(), L"QInEX Capture Device");
    if (FAILED(result)) {
        error = HResultText(L"加入 DirectShow 采集设备失败", result);
        return result;
    }
    const StreamPinSelection requested{preferredPin, 0};
    if (!FindStreamPin(builder.Get(), source.Get(),
            preferredPin == StreamPinKind::Any ? nullptr : &requested,
            streamPin, streamConfig, selectedPin)) {
        error = L"DirectShow 找不到可用的视频输出 pin。";
        return VFW_E_NOT_FOUND;
    }
    return S_OK;
}

HRESULT BuildGraph(const std::wstring& stableId, const StreamPinSelection& preferredPin,
    ComPtr<IGraphBuilder>& graph,
    ComPtr<ICaptureGraphBuilder2>& builder, ComPtr<IBaseFilter>& source,
    ComPtr<IPin>& streamPin, ComPtr<IAMStreamConfig>& streamConfig,
    StreamPinSelection& selectedPin,
    std::wstring& error) {
    HRESULT result = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&graph));
    if (SUCCEEDED(result)) result = CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&builder));
    if (SUCCEEDED(result)) result = builder->SetFiltergraph(graph.Get());
    if (FAILED(result)) {
        error = HResultText(L"创建 DirectShow 采集图表失败", result);
        return result;
    }
    if (!BindDevice(stableId, source, error)) return E_FAIL;
    result = graph->AddFilter(source.Get(), L"QInEX Capture Device");
    if (FAILED(result)) {
        error = HResultText(L"加入 DirectShow 采集设备失败", result);
        return result;
    }
    if (!FindStreamPin(builder.Get(), source.Get(), &preferredPin,
            streamPin, streamConfig, selectedPin)) {
        error = L"DirectShow 找不到先前枚举的视频输出 pin。";
        return VFW_E_NOT_FOUND;
    }
    return S_OK;
}
}  // namespace

struct CaptureSession::Impl {
    ComPtr<IGraphBuilder> graph;
    ComPtr<ICaptureGraphBuilder2> builder;
    ComPtr<IBaseFilter> source;
    ComPtr<IPin> streamPin;
    ComPtr<IAMStreamConfig> streamConfig;
    ComPtr<IBaseFilter> grabberFilter;
    ComPtr<IQpSampleGrabber> grabber;
    ComPtr<IBaseFilter> nullRenderer;
    ComPtr<IMediaControl> mediaControl;
    ComPtr<IMediaEvent> mediaEvent;
    ComPtr<GrabberCallback> callback;
    FrameCallback frameCallback;
    int width = 0;
    int height = 0;
    int stride = 0;
    bool bottomUp = false;
    CaptureNativeMode nativeMode;
    HANDLE sampleEvent = nullptr;
    bool running = false;

    void Reset() {
        if (callback) callback->Deactivate();
        if (grabber) grabber->SetCallback(nullptr, 0);
        if (mediaControl && running) mediaControl->Stop();
        running = false;
        mediaEvent.Reset();
        mediaControl.Reset();
        nullRenderer.Reset();
        grabber.Reset();
        grabberFilter.Reset();
        streamConfig.Reset();
        streamPin.Reset();
        source.Reset();
        builder.Reset();
        graph.Reset();
        callback.Reset();
        if (sampleEvent) CloseHandle(sampleEvent);
        sampleEvent = nullptr;
        frameCallback = {};
        width = height = stride = 0;
        bottomUp = false;
        nativeMode = {};
    }
};

bool EnumerateDevices(std::vector<Device>& devices, std::wstring& error) {
    devices.clear();
    const bool result = EnumerateMonikers([&](IMoniker*, const Device& device) {
        const bool duplicate = std::any_of(devices.begin(), devices.end(), [&](const Device& existing) {
            return EqualInsensitive(existing.stableId, device.stableId);
        });
        if (!duplicate) devices.push_back(device);
        return true;
    }, error);
    if (!result) return false;
    std::stable_sort(devices.begin(), devices.end(), [](const Device& left, const Device& right) {
        const int byName = CompareStringOrdinal(left.friendlyName.c_str(), -1,
            right.friendlyName.c_str(), -1, TRUE);
        if (byName != CSTR_EQUAL) return byName == CSTR_LESS_THAN;
        return CompareStringOrdinal(left.stableId.c_str(), -1,
            right.stableId.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });
    return true;
}

bool EnumerateModes(const std::wstring& stableId,
    std::vector<CaptureNativeMode>& modes, std::wstring& error) {
    modes.clear();
    ComPtr<IGraphBuilder> graph;
    ComPtr<ICaptureGraphBuilder2> builder;
    ComPtr<IBaseFilter> source;
    ComPtr<IPin> streamPin;
    ComPtr<IAMStreamConfig> config;
    StreamPinSelection selectedPin;
    if (FAILED(BuildGraph(stableId, StreamPinKind::Any, graph, builder, source,
            streamPin, config, selectedPin, error))) return false;

    std::vector<StreamPinSelection> configuredPins;
    CollectConfiguredStreamPins(builder.Get(), source.Get(), configuredPins);
    if (configuredPins.empty() && config) configuredPins.push_back(selectedPin);

    std::wstring lastModeError;
    for (const StreamPinSelection& pinSelection : configuredPins) {
        ComPtr<IPin> candidatePin;
        ComPtr<IAMStreamConfig> candidateConfig;
        StreamPinSelection foundPin;
        if (!FindStreamPin(builder.Get(), source.Get(), &pinSelection,
                candidatePin, candidateConfig, foundPin) || !candidateConfig) continue;
        std::vector<VideoMode> nativeModes;
        std::wstring modeError;
        if (!CollectModes(candidateConfig.Get(), nativeModes, modeError)) {
            if (!modeError.empty()) lastModeError = std::move(modeError);
            continue;
        }
        for (auto& mode : nativeModes) {
            const bool duplicate = std::any_of(modes.begin(), modes.end(),
                [&](const CaptureNativeMode& existing) {
                    const std::wstring key = existing.stableKey.empty()
                        ? CaptureOutput::BuildModeStableKey(existing) : existing.stableKey;
                    return CaptureOutput::ModeMatchesStableKey(mode.description, key);
                });
            if (!duplicate) modes.push_back(std::move(mode.description));
        }
    }
    if (modes.empty()) {
        error = lastModeError.empty()
            ? L"DirectShow 设备没有报告可用的视频规格。" : std::move(lastModeError);
        return false;
    }
    std::stable_sort(modes.begin(), modes.end(), PreferredModeOrder);
    error.clear();
    return true;
}

CaptureSession::CaptureSession() : impl(std::make_unique<Impl>()) {}

CaptureSession::~CaptureSession() {
    Close();
}

bool CaptureSession::Open(const OpenOptions& options, FrameCallback frameCallback,
    OpenResult& openResult, std::wstring& error) {
    Close();
    openResult = {};
    if (options.stableId.empty()) {
        error = L"DirectShow 设备标识为空。";
        return false;
    }

    // 先用一张临时图读取规格及其所属 pin。真正连接时会为每次尝试重建图，
    // 避免失败的 RenderStream 留下半连接滤镜，阻断后续可用规格。
    StreamPinSelection modePin;
    HRESULT result = BuildGraph(options.stableId, StreamPinKind::Any, impl->graph,
        impl->builder, impl->source, impl->streamPin, impl->streamConfig, modePin, error);
    if (FAILED(result)) {
        Close();
        return false;
    }

    std::vector<StreamPinSelection> configuredPins;
    CollectConfiguredStreamPins(impl->builder.Get(), impl->source.Get(), configuredPins);
    if (configuredPins.empty() && impl->streamConfig) configuredPins.push_back(modePin);

    std::vector<ModeCandidate> candidateModes;
    std::wstring lastModeError;
    bool readAnyModes = false;
    for (const StreamPinSelection& pinSelection : configuredPins) {
        ComPtr<IPin> candidatePin;
        ComPtr<IAMStreamConfig> candidateConfig;
        StreamPinSelection foundPin;
        if (!FindStreamPin(impl->builder.Get(), impl->source.Get(), &pinSelection,
                candidatePin, candidateConfig, foundPin) || !candidateConfig) continue;

        std::vector<VideoMode> modes;
        std::wstring modeError;
        if (!CollectModes(candidateConfig.Get(), modes, modeError)) {
            if (!modeError.empty()) lastModeError = std::move(modeError);
            continue;
        }
        readAnyModes = true;
        for (std::size_t index = 0; index < modes.size(); ++index) {
            const bool matches = options.modePolicy == CaptureModePolicy::Exact
                ? CaptureOutput::ModeMatchesStableKey(modes[index].description,
                    options.exactModeKey)
                : options.modePolicy == CaptureModePolicy::Preferred
                    ? CaptureOutput::ModeMatchesTimingStableKey(modes[index].description,
                        options.exactModeKey)
                    : true;
            if (!matches) continue;
            std::size_t equivalentOrdinal = 0;
            for (std::size_t previous = 0; previous < index; ++previous) {
                if (CaptureOutput::ModeMatchesStableKey(modes[previous].description,
                        modes[index].description.stableKey)) ++equivalentOrdinal;
            }
            candidateModes.push_back({modes[index].description, foundPin, equivalentOrdinal});
        }
    }
    std::stable_sort(candidateModes.begin(), candidateModes.end(),
        [](const ModeCandidate& left, const ModeCandidate& right) {
            return PreferredModeOrder(left.description, right.description);
        });
    if (candidateModes.empty() && options.modePolicy != CaptureModePolicy::Auto) {
        if (!readAnyModes && !lastModeError.empty()) {
            error = options.modePolicy == CaptureModePolicy::Exact
                ? L"DirectShow 无法验证所选精确规格：" + lastModeError
                : L"DirectShow 无法验证所选画面规格：" + lastModeError;
        } else {
            error = options.modePolicy == CaptureModePolicy::Exact
                ? L"DirectShow 没有找到所选精确规格。"
                : L"DirectShow 没有找到分辨率、刷新率和扫描方式一致的兼容规格。";
        }
        Close();
        return false;
    }
    // 只跨图保存后端中立的 pin 与规格描述，及时释放临时滤镜返回的
    // AM_MEDIA_TYPE/pUnk。每次连接都会从新滤镜实例重新取得对应原生类型。
    impl->Reset();

    std::wstring lastAttemptError;
    std::size_t attemptCount = 0;
    const auto tryOpen = [&](const ModeCandidate* requestedMode,
            const StreamPinSelection& requestedPin,
            bool driverDefault, bool& fatal) {
        fatal = false;
        ++attemptCount;
        impl->Reset();
        OpenResult candidate;
        candidate.usedDriverDefault = driverDefault;
        StreamPinSelection connectedPin = requestedPin;
        std::wstring attemptError;
        HRESULT attemptResult = BuildGraph(options.stableId, requestedPin, impl->graph,
            impl->builder, impl->source, impl->streamPin, impl->streamConfig,
            connectedPin, attemptError);
        if (FAILED(attemptResult)) {
            // 某一路 pin 在重建滤镜后消失时仍应继续试其它输出；设备或图表本身
            // 无法建立才会使后续候选失去意义。
            fatal = attemptResult != VFW_E_NOT_FOUND;
            lastAttemptError = std::move(attemptError);
            return false;
        }

        if (requestedMode) {
            if (!impl->streamConfig) {
                lastAttemptError = L"DirectShow 找不到所选规格对应的视频输出 pin。";
                impl->Reset();
                return false;
            }
            // AM_MEDIA_TYPE 来自具体滤镜实例。重建图后重新从新实例取出同一稳定规格，
            // 避免把旧实例的格式对象直接交给新 IAMStreamConfig。
            std::vector<VideoMode> currentModes;
            if (!CollectModes(impl->streamConfig.Get(), currentModes, attemptError)) {
                lastAttemptError = std::move(attemptError);
                impl->Reset();
                return false;
            }
            std::size_t equivalentOrdinal = 0;
            auto currentMode = currentModes.end();
            for (auto item = currentModes.begin(); item != currentModes.end(); ++item) {
                if (!CaptureOutput::ModeMatchesStableKey(item->description,
                        requestedMode->description.stableKey)) continue;
                if (equivalentOrdinal++ == requestedMode->equivalentOrdinal) {
                    currentMode = item;
                    break;
                }
            }
            if (currentMode == currentModes.end()) {
                lastAttemptError = L"DirectShow 候选视频规格在重新连接时已不可用。";
                impl->Reset();
                return false;
            }
            attemptResult = impl->streamConfig->SetFormat(currentMode->type.get());
            if (FAILED(attemptResult)) {
                lastAttemptError = HResultText(L"DirectShow 无法设置候选视频规格", attemptResult);
                impl->Reset();
                return false;
            }
            candidate.requestedMode = currentMode->description;
        }

        attemptResult = CoCreateInstance(kClsidSampleGrabber, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&impl->grabberFilter));
        if (SUCCEEDED(attemptResult)) attemptResult = impl->grabberFilter.As(&impl->grabber);
        if (FAILED(attemptResult)) {
            fatal = true;
            lastAttemptError = HResultText(L"创建 DirectShow 帧读取组件失败", attemptResult);
            impl->Reset();
            return false;
        }
        AM_MEDIA_TYPE requestedOutput{};
        requestedOutput.majortype = MEDIATYPE_Video;
        requestedOutput.subtype = MEDIASUBTYPE_RGB32;
        // 这是 partial media type：只约束最终像素格式，不把 format type 也锁死。
        // 这样源 pin 为 FORMAT_VideoInfo2 时，Intelligent Connect 仍有机会插入转换器，
        // 再以 Sample Grabber 可接受的格式完成连接。
        attemptResult = impl->grabber->SetMediaType(&requestedOutput);
        if (SUCCEEDED(attemptResult)) attemptResult = impl->grabber->SetOneShot(FALSE);
        if (SUCCEEDED(attemptResult)) attemptResult = impl->grabber->SetBufferSamples(FALSE);
        if (SUCCEEDED(attemptResult)) attemptResult = impl->graph->AddFilter(
            impl->grabberFilter.Get(), L"QInEX RGB32 Frame Reader");
        if (SUCCEEDED(attemptResult)) attemptResult = CoCreateInstance(kClsidNullRenderer,
            nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&impl->nullRenderer));
        if (SUCCEEDED(attemptResult)) attemptResult = impl->graph->AddFilter(
            impl->nullRenderer.Get(), L"QInEX Null Renderer");
        if (FAILED(attemptResult)) {
            fatal = true;
            lastAttemptError = HResultText(L"初始化 DirectShow 帧输出失败", attemptResult);
            impl->Reset();
            return false;
        }

        // 配置和连接必须指向同一个 pin；否则规格验证的并非真正出帧的流。
        attemptResult = impl->builder->RenderStream(nullptr,
            &MEDIATYPE_Video, impl->streamPin.Get(), impl->grabberFilter.Get(),
            impl->nullRenderer.Get());
        if (FAILED(attemptResult)) {
            lastAttemptError = HResultText(L"DirectShow 无法连接候选视频规格", attemptResult);
            impl->Reset();
            return false;
        }

        bool bottomUp = false;
        double outputFps = 0;
        if (!ReadConnectedFormat(impl->grabber.Get(), candidate.width, candidate.height,
                candidate.stride, bottomUp, outputFps, attemptError)) {
            lastAttemptError = std::move(attemptError);
            impl->Reset();
            return false;
        }

        AM_MEDIA_TYPE* currentRaw = nullptr;
        if (impl->streamConfig && SUCCEEDED(impl->streamConfig->GetFormat(&currentRaw))) {
            OwnedMediaType current(currentRaw);
            if (current) ReadMode(*current, candidate.actualMode);
        }
        if (!candidate.actualMode.width && requestedMode &&
                options.modePolicy != CaptureModePolicy::Exact) {
            candidate.actualMode = requestedMode->description;
        }
        const bool constrainedMode = options.modePolicy == CaptureModePolicy::Exact ||
            options.modePolicy == CaptureModePolicy::Preferred;
        if (constrainedMode) {
            const bool matchesRequest = candidate.actualMode.width &&
                (options.modePolicy == CaptureModePolicy::Exact
                    ? CaptureOutput::ModeMatchesStableKey(candidate.actualMode,
                        options.exactModeKey)
                    : CaptureOutput::ModeMatchesTimingStableKey(candidate.actualMode,
                        options.exactModeKey));
            if (!matchesRequest) {
                lastAttemptError = options.modePolicy == CaptureModePolicy::Exact
                    ? L"DirectShow 无法确认驱动采用了请求的精确规格。"
                    : L"DirectShow 无法确认驱动保持了请求的分辨率、刷新率和扫描方式。";
                impl->Reset();
                return false;
            }
            if (candidate.width != static_cast<int>(candidate.actualMode.width) ||
                    candidate.height != static_cast<int>(candidate.actualMode.height)) {
                lastAttemptError = L"DirectShow 实际输出尺寸与请求的画面规格不一致。";
                impl->Reset();
                return false;
            }
            const double actualFps = candidate.actualMode.Fps();
            if (actualFps > 0 && outputFps > 0 &&
                    std::abs(actualFps - outputFps) > std::max(0.02, actualFps * 0.0005)) {
                lastAttemptError = L"DirectShow 实际输出刷新率与请求的画面规格不一致。";
                impl->Reset();
                return false;
            }
        }
        if (!candidate.actualMode.width) {
            candidate.actualMode.width = static_cast<unsigned>(candidate.width);
            candidate.actualMode.height = static_cast<unsigned>(candidate.height);
            candidate.actualMode.fpsNumerator = outputFps > 0
                ? static_cast<std::uint32_t>(std::llround(outputFps * 1000.0)) : 0;
            candidate.actualMode.fpsDenominator = candidate.actualMode.fpsNumerator ? 1000 : 1;
            candidate.actualMode.subtype = MEDIASUBTYPE_RGB32;
            candidate.actualMode.subtypeName = L"RGB32";
            candidate.actualMode.progressive = true;
            candidate.actualMode.interlaceMode = kInterlaceProgressive;
            candidate.actualMode.stableKey = CaptureOutput::BuildModeStableKey(
                candidate.actualMode);
        }
        candidate.nominalFps = candidate.actualMode.Fps();
        if (candidate.nominalFps <= 0) candidate.nominalFps = outputFps;

        impl->sampleEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!impl->sampleEvent) {
            fatal = true;
            lastAttemptError = L"创建 DirectShow 帧通知失败。";
            impl->Reset();
            return false;
        }
        impl->callback.Attach(new (std::nothrow) GrabberCallback(impl->sampleEvent));
        if (!impl->callback) {
            fatal = true;
            lastAttemptError = L"创建 DirectShow 视频回调失败：内存不足。";
            impl->Reset();
            return false;
        }
        impl->frameCallback = frameCallback;
        impl->width = candidate.width;
        impl->height = candidate.height;
        impl->stride = candidate.stride;
        impl->bottomUp = bottomUp;
        impl->nativeMode = candidate.actualMode;
        attemptResult = impl->grabber->SetCallback(impl->callback.Get(), 0);
        if (SUCCEEDED(attemptResult)) attemptResult = impl->graph.As(&impl->mediaControl);
        if (SUCCEEDED(attemptResult)) attemptResult = impl->graph.As(&impl->mediaEvent);
        ComPtr<IMediaFilter> mediaFilter;
        if (SUCCEEDED(attemptResult)) attemptResult = impl->graph.As(&mediaFilter);
        if (SUCCEEDED(attemptResult)) attemptResult = mediaFilter->SetSyncSource(nullptr);
        if (FAILED(attemptResult)) {
            fatal = true;
            lastAttemptError = HResultText(L"启用 DirectShow 视频回调失败", attemptResult);
            impl->Reset();
            return false;
        }
        openResult = std::move(candidate);
        error.clear();
        return true;
    };

    const std::size_t modeAttempts = candidateModes.size();
    bool fatalFailure = false;
    const std::size_t firstCandidate = modeAttempts
        ? options.candidateOffset % modeAttempts : 0;
    for (std::size_t index = 0; index < modeAttempts; ++index) {
        const std::size_t candidateIndex = (firstCandidate + index) % modeAttempts;
        bool fatal = false;
        if (tryOpen(&candidateModes[candidateIndex],
                candidateModes[candidateIndex].pin, false, fatal)) return true;
        if (fatal) {
            fatalFailure = true;
            break;
        }
    }

    // Auto 最后再让驱动选择当前格式，并覆盖已发现的各路输出及旧驱动常见入口。
    if (options.modePolicy == CaptureModePolicy::Auto && !fatalFailure) {
        std::vector<StreamPinSelection> defaultPins = configuredPins;
        AppendStreamPin(defaultPins, modePin);
        AppendStreamPin(defaultPins, {StreamPinKind::Capture, 0});
        AppendStreamPin(defaultPins, {StreamPinKind::Preview, 0});
        AppendStreamPin(defaultPins, {StreamPinKind::Any, 0});
        for (const StreamPinSelection& pin : defaultPins) {
            bool fatal = false;
            if (tryOpen(nullptr, pin, true, fatal)) return true;
            if (fatal) break;
        }
    }

    Close();
    error = lastAttemptError.empty() ? L"DirectShow 没有可连接的视频规格。" :
        (attemptCount > 1
            ? L"DirectShow 已尝试 " + std::to_wstring(attemptCount) +
                L" 种连接方式，均未成功：" + lastAttemptError
            : lastAttemptError);
    return false;
}

bool CaptureSession::Run(std::wstring& error) {
    if (!impl->mediaControl || !impl->callback) {
        error = L"DirectShow 采集会话尚未打开。";
        return false;
    }
    const HRESULT result = impl->mediaControl->Run();
    if (FAILED(result)) {
        error = HResultText(L"启动 DirectShow 视频流失败", result);
        return false;
    }
    impl->running = true;
    error.clear();
    return true;
}

bool CaptureSession::WaitForFrame(unsigned timeoutMs) const {
    return impl && impl->sampleEvent &&
        WaitForSingleObject(impl->sampleEvent, timeoutMs) == WAIT_OBJECT_0;
}

PumpResult CaptureSession::Pump(std::wstring& error) {
    error.clear();
    if (!impl->callback || !impl->frameCallback) return PumpResult::Idle;
    SampleSlot slot;
    if (!impl->callback->Take(slot)) return PumpResult::Idle;

    AM_MEDIA_TYPE* changedType = nullptr;
    const HRESULT changed = slot.sample->GetMediaType(&changedType);
    OwnedMediaType changedOwner(changedType);
    if (changed == S_OK && changedOwner) {
        // 一些采集驱动会在图启动后的第一帧再次附上已经协商好的媒体类型。
        // 该帧本身有效；若无条件重建，下一次第一帧仍会携带类型并形成死循环。
        int width = 0, height = 0, stride = 0;
        bool bottomUp = false;
        double fps = 0;
        if (!ReadRgb32Format(*changedOwner, width, height, stride, bottomUp, fps, error))
            return PumpResult::Failure;
        impl->width = width;
        impl->height = height;
        impl->stride = stride;
        impl->bottomUp = bottomUp;
        if (impl->nativeMode.width != static_cast<unsigned>(width) ||
                impl->nativeMode.height != static_cast<unsigned>(height)) {
            impl->nativeMode.width = static_cast<unsigned>(width);
            impl->nativeMode.height = static_cast<unsigned>(height);
            if (fps > 0) {
                impl->nativeMode.fpsNumerator = static_cast<std::uint32_t>(
                    std::llround(fps * 1000.0));
                impl->nativeMode.fpsDenominator = 1000;
            }
            impl->nativeMode.stableKey = CaptureOutput::BuildModeStableKey(
                impl->nativeMode);
        }
    }

    BYTE* bytes = nullptr;
    HRESULT result = slot.sample->GetPointer(&bytes);
    const long length = slot.sample->GetActualDataLength();
    if (FAILED(result) || !bytes || length <= 0) {
        error = FAILED(result) ? HResultText(L"读取 DirectShow 视频帧失败", result)
                               : L"DirectShow 返回了空视频帧。";
        return PumpResult::Failure;
    }
    FrameView frame;
    frame.bytes = bytes;
    frame.length = static_cast<std::size_t>(length);
    frame.width = impl->width;
    frame.height = impl->height;
    frame.stride = impl->stride;
    frame.bottomUp = impl->bottomUp;
    REFERENCE_TIME sampleStart = 0, sampleEnd = 0;
    frame.hasSampleTimestamp = SUCCEEDED(slot.sample->GetTime(&sampleStart, &sampleEnd));
    frame.sampleTimestamp100ns = sampleStart;
    frame.mailboxSkippedFrames = slot.mailboxSkippedFrames;
    frame.arrivalQpc = slot.arrivalQpc;
    frame.nativeMode = impl->nativeMode;
    try {
        if (!impl->frameCallback(frame, error)) {
            if (error.empty()) error = L"处理 DirectShow 视频帧失败。";
            return PumpResult::Failure;
        }
    } catch (const std::bad_alloc&) {
        error = L"DirectShow 视频帧内存不足。";
        return PumpResult::Failure;
    } catch (...) {
        error = L"处理 DirectShow 视频帧时发生异常。";
        return PumpResult::Failure;
    }
    return PumpResult::Frame;
}

bool CaptureSession::PollFailure(std::wstring& error) {
    error.clear();
    if (!impl->mediaEvent) return false;
    long code = 0;
    LONG_PTR first = 0, second = 0;
    while (impl->mediaEvent->GetEvent(&code, &first, &second, 0) == S_OK) {
        impl->mediaEvent->FreeEventParams(code, first, second);
        if (code == EC_ERRORABORT) {
            const HRESULT result = static_cast<HRESULT>(first);
            error = HResultText(L"DirectShow 视频流发生错误", FAILED(result) ? result : E_FAIL);
            return true;
        }
        if (code == EC_DEVICE_LOST) {
            error = L"DirectShow 采集设备已断开。";
            return true;
        }
        if (code == EC_COMPLETE || code == EC_STREAM_ERROR_STOPPED) {
            error = L"DirectShow 采集视频流已结束。";
            return true;
        }
        // EC_STREAM_ERROR_STILLPLAYING 明确表示当前流仍在播放。部分驱动会在
        // 其它未使用的 pin 报错时发送它，不能因此拆掉仍能出帧的视频图。
    }
    return false;
}

void CaptureSession::Close() {
    if (impl) impl->Reset();
}

bool CaptureSession::IsOpen() const noexcept {
    return impl && impl->graph && impl->callback;
}

}  // namespace qpointer::directshow
