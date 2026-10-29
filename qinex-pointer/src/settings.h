#pragma once
#include <windows.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <string>
#include <tuple>
#include "hotkey_utils.h"
#include "pointer_core.h"

namespace Messages {
inline bool anime = false;
inline const wchar_t* Pick(const wchar_t* normal, const wchar_t* cute) { return anime ? cute : normal; }
}

// 采集规格选择需要单独持久化。旧版只保存 mode key，无法区分普通帧率档和
// 高级精确规格；缺少该字段的旧配置统一迁移为 Preferred。
enum class CaptureModeSelection {
    Auto = 0,
    Preferred = 1,
    Exact = 2
};

// 3、4 都由本程序创建独立全屏输出窗口；只有 3 需要打开采集设备。
inline constexpr int kDisplayModeCapture = 3;
inline constexpr int kDisplayModeFusion = 4;
inline bool IsCaptureDisplayMode(int mode) noexcept { return mode == kDisplayModeCapture; }
inline bool IsFusionDisplayMode(int mode) noexcept { return mode == kDisplayModeFusion; }
inline bool IsOwnedFullscreenDisplayMode(int mode) noexcept {
    return IsCaptureDisplayMode(mode) || IsFusionDisplayMode(mode);
}

// 便携配置使用 UTF-16 路径和 Win32 INI，独立程序没有运行时下载依赖。
struct Settings {
    int source = 0, port = 6533, mode = 0, monitor = 0;
    std::wstring sourceIp = L"127.0.0.1", windowTitle;
    // 采集设备使用后端中立的稳定身份；旧设备仍沿用 MF 符号链接，友好名只用于离线回显。
    // 输出屏优先保存 DisplayConfig 的显示器设备接口路径，旧版 GDI 名称仍可迁移读取。
    std::wstring captureDeviceId, captureDeviceName, captureMonitorId;
    // 模式键始终保存采集卡报告的稳定键，绝不保存枚举下标。普通档把它作为
    // 分辨率/刷新率/扫描方式目标，高级精确规格还会锁定原生像素格式。
    std::wstring captureModeKey;
    CaptureModeSelection captureModeSelection = CaptureModeSelection::Auto;
    // 普通档位用于恢复用户意图和界面选择。
    int captureFpsPreference = 0;
    // 0=自动同步，1=极限低延迟（允许撕裂）。
    int capturePresentMode = 0;
    // 融合输出使用独立显示器、底色、呈现策略和校准，切换采集模式时互不覆盖。
    std::wstring fusionMonitorId;
    COLORREF fusionBackgroundColor = RGB(0, 255, 0);
    int fusionPresentMode = 0;
    qpointer::Affine fusionCalibration;
    qpointer::Geometry geometry;
    qpointer::Rect windowRegion{0, 0, 1, 1};
    // 0..2 保持旧版箭头/圆点/十字编号；3..7 与 APP 对齐扩展圆环、涟漪、
    // 锁定框、轨迹和自定义图片。自定义热点始终是图片内归一化坐标。
    int shape = 0, size = 28, delayMs = 0, timeoutMs = 2000;
    COLORREF color = RGB(0, 205, 230);
    bool foregroundOnly = false, anime = false;
    bool setupComplete = false, simpleMapping = false, resumeOnLaunch = false;
    // 快捷键只持久化 Win32 virtual-key 与 MOD_* 修饰位；MOD_NOREPEAT 仅注册时添加。
    int pauseHotkey = VK_F10, pauseHotkeyModifiers = MOD_CONTROL | MOD_ALT;
    int exitHotkey = VK_F11, exitHotkeyModifiers = 0;
    int rangeHotkey = VK_F8, rangeHotkeyModifiers = 0;
    int hudHotkey = VK_F7, hudHotkeyModifiers = 0;
};

// 来源只包含接收身份，画面与外观变化不会被误判为切换设备。
inline bool SameSource(const Settings& a, const Settings& b) {
    return std::tie(a.source, a.sourceIp, a.port) == std::tie(b.source, b.sourceIp, b.port);
}

// 使用明确的字段比较保留双精度精度；不比较结构体填充字节。
inline bool SameSettings(const Settings& a, const Settings& b) {
    const auto rect = [](const qpointer::Rect& x, const qpointer::Rect& y) {
        return std::tie(x.x, x.y, x.w, x.h) == std::tie(y.x, y.y, y.w, y.h);
    };
    const auto& x = a.geometry; const auto& y = b.geometry;
    const auto& ca = x.calibration; const auto& cb = y.calibration;
    const auto& fa = a.fusionCalibration; const auto& fb = b.fusionCalibration;
    return SameSource(a, b) &&
        std::tie(a.mode, a.monitor, a.windowTitle, a.captureDeviceId, a.captureDeviceName, a.captureMonitorId,
            a.captureModeKey, a.captureModeSelection, a.captureFpsPreference, a.capturePresentMode,
            a.fusionMonitorId, a.fusionBackgroundColor, a.fusionPresentMode,
            a.shape, a.size, a.delayMs, a.timeoutMs,
            a.color, a.foregroundOnly, a.anime, a.setupComplete, a.simpleMapping, a.resumeOnLaunch,
            a.pauseHotkey, a.pauseHotkeyModifiers, a.exitHotkey, a.exitHotkeyModifiers,
            a.rangeHotkey, a.rangeHotkeyModifiers, a.hudHotkey, a.hudHotkeyModifiers) ==
        std::tie(b.mode, b.monitor, b.windowTitle, b.captureDeviceId, b.captureDeviceName, b.captureMonitorId,
            b.captureModeKey, b.captureModeSelection, b.captureFpsPreference, b.capturePresentMode,
            b.fusionMonitorId, b.fusionBackgroundColor, b.fusionPresentMode,
            b.shape, b.size, b.delayMs, b.timeoutMs,
            b.color, b.foregroundOnly, b.anime, b.setupComplete, b.simpleMapping, b.resumeOnLaunch,
            b.pauseHotkey, b.pauseHotkeyModifiers, b.exitHotkey, b.exitHotkeyModifiers,
            b.rangeHotkey, b.rangeHotkeyModifiers, b.hudHotkey, b.hudHotkeyModifiers) &&
        rect(x.target, y.target) && rect(x.content, y.content) && rect(x.crop, y.crop) && rect(a.windowRegion, b.windowRegion) &&
        std::tie(x.frameWidth, x.frameHeight, x.rotation, x.mirrorX, x.mirrorY, x.stretch) ==
        std::tie(y.frameWidth, y.frameHeight, y.rotation, y.mirrorX, y.mirrorY, y.stretch) &&
        std::tie(ca.a, ca.b, ca.c, ca.d, ca.e, ca.f) == std::tie(cb.a, cb.b, cb.c, cb.d, cb.e, cb.f) &&
        std::tie(fa.a, fa.b, fa.c, fa.d, fa.e, fa.f) == std::tie(fb.a, fb.b, fb.c, fb.d, fb.e, fb.f);
}

inline std::wstring ExeDirectory() {
    std::wstring p(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, p.data(), static_cast<DWORD>(p.size()));
    p.resize(n);
    return std::filesystem::path(p).parent_path().wstring();
}

inline std::wstring IniPath() { return ExeDirectory() + L"\\pointer.ini"; }

inline std::wstring ReadIni(const wchar_t* key, const std::wstring& fallback, const std::wstring& file = IniPath()) {
    wchar_t value[1024]{};
    GetPrivateProfileStringW(L"pointer", key, fallback.c_str(), value, 1024, file.c_str());
    return value;
}

inline double ReadNumber(const wchar_t* key, double fallback, const std::wstring& file = IniPath()) {
    try {
        size_t used = 0;
        auto value = ReadIni(key, std::to_wstring(fallback), file);
        double n = std::stod(value, &used);
        return used == value.size() && std::isfinite(n) ? n : fallback;
    } catch (...) { return fallback; }
}

// 旧版可能只保存稳定规格键，没有单独保存普通帧率档。从键中的有理帧率
// 恢复 1..1000 Hz 桶；解析失败保持 0，不凭显示名或枚举序号猜测。
inline int CaptureFpsPreferenceFromModeKey(const std::wstring& key) {
    const std::size_t first = key.find(L'|');
    const std::size_t second = first == std::wstring::npos ? first : key.find(L'|', first + 1);
    const std::size_t third = second == std::wstring::npos ? second : key.find(L'|', second + 1);
    const std::size_t slash = second == std::wstring::npos ? second : key.find(L'/', second + 1);
    if (second == std::wstring::npos || third == std::wstring::npos ||
            slash == std::wstring::npos || slash >= third) return 0;
    const auto parse = [&](std::size_t begin, std::size_t end, std::uint64_t& value) {
        if (begin >= end) return false;
        value = 0;
        for (std::size_t index = begin; index < end; ++index) {
            if (key[index] < L'0' || key[index] > L'9') return false;
            const unsigned digit = static_cast<unsigned>(key[index] - L'0');
            if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) return false;
            value = value * 10 + digit;
        }
        return true;
    };
    std::uint64_t numerator = 0, denominator = 0;
    if (!parse(second + 1, slash, numerator) || !parse(slash + 1, third, denominator) ||
            !numerator || !denominator) return 0;
    const double rate = static_cast<double>(numerator) / static_cast<double>(denominator);
    for (const int candidate : {24, 25, 30, 50, 60, 75, 90, 100,
            120, 144, 165, 180, 200, 240, 360, 480}) {
        const double tolerance = std::max(0.08, candidate * 0.0021);
        if (std::abs(rate - candidate) <= tolerance) return candidate;
    }
    if (!std::isfinite(rate) || rate < 1 || rate > 1000) return 0;
    const long long rounded = std::llround(rate);
    return rounded >= 1 && rounded <= 1000 ? static_cast<int>(rounded) : 0;
}

inline Settings LoadSettings(const std::wstring& file = IniPath()) {
    Settings s;
    auto number = [&](const wchar_t* key, double fallback) { return ReadNumber(key, fallback, file); };
    auto integer = [&](const wchar_t* key, int fallback, int low, int high) {
        const double n = number(key, fallback);
        return n >= low && n <= high && n == std::floor(n) ? static_cast<int>(n) : fallback;
    };
    s.source = integer(L"source", 0, 0, 1);
    s.port = integer(L"port", 6533, 1, 65535);
    s.sourceIp = ReadIni(L"sourceIp", L"127.0.0.1", file);
    s.mode = integer(L"mode", 0, 0, kDisplayModeFusion);
    s.monitor = integer(L"monitor", 0, 0, 64);
    s.windowTitle = ReadIni(L"windowTitle", L"", file);
    s.captureDeviceId = ReadIni(L"captureDeviceId", L"", file);
    s.captureDeviceName = ReadIni(L"captureDeviceName", L"", file);
    s.captureMonitorId = ReadIni(L"captureMonitorId", L"", file);
    s.captureModeKey = ReadIni(L"captureModeKey", L"", file);
    // 老版本没有 captureModeSelection：非空 mode key 原来既可能来自普通帧率档，
    // 也可能来自精确规格。按更易成功的普通档迁移，用户再次勾选精确规格后会明确写 2。
    const int legacyModeSelection = s.captureModeKey.empty()
        ? static_cast<int>(CaptureModeSelection::Auto)
        : static_cast<int>(CaptureModeSelection::Preferred);
    s.captureModeSelection = static_cast<CaptureModeSelection>(
        integer(L"captureModeSelection", legacyModeSelection,
            static_cast<int>(CaptureModeSelection::Auto),
            static_cast<int>(CaptureModeSelection::Exact)));
    s.captureFpsPreference = integer(L"captureFpsPreference", 0, 0, 1000);
    if (s.captureModeSelection == CaptureModeSelection::Preferred) {
        // stable key 是实际采集时序的真相源；修正旧配置中缺失或与 key 不一致的 UI 桶。
        s.captureFpsPreference = CaptureFpsPreferenceFromModeKey(s.captureModeKey);
    }
    if (s.captureModeKey.empty()) s.captureModeSelection = CaptureModeSelection::Auto;
    if (s.captureModeSelection == CaptureModeSelection::Auto) {
        s.captureModeKey.clear();
        s.captureFpsPreference = 0;
    }
    s.capturePresentMode = integer(L"capturePresentMode", 0, 0, 1);
    s.fusionMonitorId = ReadIni(L"fusionMonitorId", L"", file);
    s.fusionBackgroundColor = static_cast<COLORREF>(integer(
        L"fusionBackgroundColor", RGB(0, 255, 0), 0, 0xffffff));
    s.fusionPresentMode = integer(L"fusionPresentMode", 0, 0, 1);
    s.fusionCalibration = {
        number(L"fusionA", 1), number(L"fusionB", 0), number(L"fusionC", 0),
        number(L"fusionD", 0), number(L"fusionE", 1), number(L"fusionF", 0)};
    s.shape = integer(L"shape", 0, 0, 7);
    s.size = integer(L"size", 28, 12, 144);
    s.delayMs = integer(L"delayMs", 0, 0, 2000);
    s.timeoutMs = integer(L"timeoutMs", 2000, 600, 15000);
    // 损坏文件的单项值即使各自合法，也不能恢复出无法启动的延迟/超时组合。
    if (s.timeoutMs <= s.delayMs + 500) s.timeoutMs = s.delayMs + 1000;
    s.color = static_cast<COLORREF>(integer(L"color", RGB(0, 205, 230), 0, 0xffffff));
    s.foregroundOnly = integer(L"foregroundOnly", 0, 0, 1) != 0;
    s.anime = integer(L"anime", 0, 0, 1) != 0;
    // 旧配置保留完整几何模型；只对用户明确启用的简易框选使用新映射方式。
    s.setupComplete = integer(L"setupComplete", 0, 0, 1) != 0;
    s.simpleMapping = integer(L"simpleMapping", 0, 0, 1) != 0;
    s.resumeOnLaunch = integer(L"resumeOnLaunch", 0, 0, 1) != 0;
    constexpr int kModifierMask = qpointer::kHotkeyModifierMask;
    auto hotkey = [&](const wchar_t* key, const wchar_t* modifierKey,
            int defaultKey, int defaultModifiers, bool required) {
        int virtualKey = integer(key, defaultKey, 0, 0xff);
        int modifiers = integer(modifierKey, defaultModifiers, 0, kModifierMask) & kModifierMask;
        if (!qpointer::IsUsableHotkey({virtualKey, modifiers}, required)) {
            virtualKey = defaultKey;
            modifiers = defaultModifiers;
        }
        if (!virtualKey) modifiers = 0;
        return std::pair<int, int>{virtualKey, modifiers};
    };
    // 旧版仅允许关闭或 Ctrl+Alt+F9/F10/F11。只有 modifier 字段存在时才按
    // 新格式解释任意虚拟键，防止损坏的旧值突然变成一个有效全局快捷键。
    if (ReadIni(L"pauseHotkeyModifiers", L"", file).empty()) {
        const int legacy = integer(L"pauseHotkey", VK_F10, 0, 0xff);
        s.pauseHotkey = legacy == 0 || (legacy >= VK_F9 && legacy <= VK_F11)
            ? legacy : VK_F10;
        s.pauseHotkeyModifiers = s.pauseHotkey ? MOD_CONTROL | MOD_ALT : 0;
    } else {
        std::tie(s.pauseHotkey, s.pauseHotkeyModifiers) = hotkey(
            L"pauseHotkey", L"pauseHotkeyModifiers", VK_F10, MOD_CONTROL | MOD_ALT, false);
    }
    std::tie(s.exitHotkey, s.exitHotkeyModifiers) = hotkey(
        L"exitHotkey", L"exitHotkeyModifiers", VK_F11, 0, true);
    std::tie(s.rangeHotkey, s.rangeHotkeyModifiers) = hotkey(
        L"rangeHotkey", L"rangeHotkeyModifiers", VK_F8, 0, false);
    std::tie(s.hudHotkey, s.hudHotkeyModifiers) = hotkey(
        L"hudHotkey", L"hudHotkeyModifiers", VK_F7, 0, false);
    // 手工改坏配置时也不能让多个动作争抢同一个全局热键。
    std::array<qpointer::HotkeyBinding, 4> bindings{{
        {s.pauseHotkey, s.pauseHotkeyModifiers}, {s.exitHotkey, s.exitHotkeyModifiers},
        {s.rangeHotkey, s.rangeHotkeyModifiers}, {s.hudHotkey, s.hudHotkeyModifiers}}};
    bool duplicate = false;
    for (size_t index = 0; index < bindings.size(); ++index) {
        if (!bindings[index].virtualKey) continue;
        if (qpointer::IsReservedHotkey(bindings[index]))
            duplicate = true;
        for (size_t prior = 0; prior < index; ++prior) {
            if (bindings[index] == bindings[prior]) duplicate = true;
        }
    }
    if (duplicate) bindings = {{{VK_F10, MOD_CONTROL | MOD_ALT},
        {VK_F11, 0}, {VK_F8, 0}, {VK_F7, 0}}};
    std::tie(s.pauseHotkey, s.pauseHotkeyModifiers) =
        std::pair<int, int>{bindings[0].virtualKey, bindings[0].modifiers};
    std::tie(s.exitHotkey, s.exitHotkeyModifiers) =
        std::pair<int, int>{bindings[1].virtualKey, bindings[1].modifiers};
    std::tie(s.rangeHotkey, s.rangeHotkeyModifiers) =
        std::pair<int, int>{bindings[2].virtualKey, bindings[2].modifiers};
    std::tie(s.hudHotkey, s.hudHotkeyModifiers) =
        std::pair<int, int>{bindings[3].virtualKey, bindings[3].modifiers};
    auto& g = s.geometry;
    g.target = {number(L"targetX", 0), number(L"targetY", 0), number(L"targetW", 1920), number(L"targetH", 1080)};
    g.frameWidth = number(L"frameW", 1920); g.frameHeight = number(L"frameH", 1080);
    g.content = {number(L"contentX", 0), number(L"contentY", 0), number(L"contentW", 1), number(L"contentH", 1)};
    g.crop = {number(L"cropX", 0), number(L"cropY", 0), number(L"cropW", 1), number(L"cropH", 1)};
    g.rotation = integer(L"rotation", 0, 0, 270);
    if (g.rotation % 90 != 0) g.rotation = 0;
    g.mirrorX = integer(L"mirrorX", 0, 0, 1) != 0; g.mirrorY = integer(L"mirrorY", 0, 0, 1) != 0;
    g.stretch = integer(L"stretch", 0, 0, 1) != 0;
    g.calibration = {number(L"a", 1), number(L"b", 0), number(L"c", 0), number(L"d", 0), number(L"e", 1), number(L"f", 0)};
    s.windowRegion = {number(L"windowX", 0), number(L"windowY", 0), number(L"windowW", 1), number(L"windowH", 1)};
    if (!(s.windowRegion.w > 0 && s.windowRegion.h > 0 && s.windowRegion.x >= 0 && s.windowRegion.y >= 0 && s.windowRegion.x + s.windowRegion.w <= 1.00001 && s.windowRegion.y + s.windowRegion.h <= 1.00001)) s.windowRegion = {0, 0, 1, 1};
    std::string error;
    if (!qpointer::ValidateGeometry(g, error)) g = qpointer::Geometry{};
    qpointer::Geometry fusionValidation;
    fusionValidation.calibration = s.fusionCalibration;
    if (!qpointer::ValidateGeometry(fusionValidation, error)) s.fusionCalibration = {};
    if (s.source == 1) s.port = 6533;
    return s;
}

inline bool SaveSettings(const Settings& s, const std::wstring& destination = IniPath()) {
    const auto file = destination + L".tmp";
    std::wstring contents = L"\xfeff[pointer]\r\n";
    auto put = [&](const wchar_t* key, const std::wstring& value) { contents += std::wstring(key) + L"=" + value + L"\r\n"; };
    auto number = [&](const wchar_t* key, double value) {
        std::wostringstream text;
        text.imbue(std::locale::classic());
        text << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
        put(key, text.str());
    };
    number(L"source", s.source); number(L"port", s.port); put(L"sourceIp", s.sourceIp);
    number(L"mode", s.mode); number(L"monitor", s.monitor); put(L"windowTitle", s.windowTitle);
    put(L"captureDeviceId", s.captureDeviceId); put(L"captureDeviceName", s.captureDeviceName); put(L"captureMonitorId", s.captureMonitorId);
    put(L"captureModeKey", s.captureModeKey); number(L"captureModeSelection", static_cast<int>(s.captureModeSelection));
    number(L"captureFpsPreference", s.captureFpsPreference); number(L"capturePresentMode", s.capturePresentMode);
    put(L"fusionMonitorId", s.fusionMonitorId); number(L"fusionBackgroundColor", s.fusionBackgroundColor);
    number(L"fusionPresentMode", s.fusionPresentMode);
    number(L"fusionA", s.fusionCalibration.a); number(L"fusionB", s.fusionCalibration.b); number(L"fusionC", s.fusionCalibration.c);
    number(L"fusionD", s.fusionCalibration.d); number(L"fusionE", s.fusionCalibration.e); number(L"fusionF", s.fusionCalibration.f);
    number(L"shape", s.shape); number(L"size", s.size); number(L"delayMs", s.delayMs); number(L"timeoutMs", s.timeoutMs);
    number(L"color", s.color); number(L"foregroundOnly", s.foregroundOnly); number(L"anime", s.anime);
    number(L"setupComplete", s.setupComplete); number(L"simpleMapping", s.simpleMapping);
    number(L"resumeOnLaunch", s.resumeOnLaunch);
    number(L"pauseHotkey", s.pauseHotkey); number(L"pauseHotkeyModifiers", s.pauseHotkeyModifiers);
    number(L"exitHotkey", s.exitHotkey); number(L"exitHotkeyModifiers", s.exitHotkeyModifiers);
    number(L"rangeHotkey", s.rangeHotkey); number(L"rangeHotkeyModifiers", s.rangeHotkeyModifiers);
    number(L"hudHotkey", s.hudHotkey); number(L"hudHotkeyModifiers", s.hudHotkeyModifiers);
    const auto& g = s.geometry;
    number(L"targetX", g.target.x); number(L"targetY", g.target.y); number(L"targetW", g.target.w); number(L"targetH", g.target.h);
    number(L"frameW", g.frameWidth); number(L"frameH", g.frameHeight);
    number(L"contentX", g.content.x); number(L"contentY", g.content.y); number(L"contentW", g.content.w); number(L"contentH", g.content.h);
    number(L"cropX", g.crop.x); number(L"cropY", g.crop.y); number(L"cropW", g.crop.w); number(L"cropH", g.crop.h);
    number(L"rotation", g.rotation); number(L"mirrorX", g.mirrorX); number(L"mirrorY", g.mirrorY); number(L"stretch", g.stretch);
    number(L"a", g.calibration.a); number(L"b", g.calibration.b); number(L"c", g.calibration.c);
    number(L"d", g.calibration.d); number(L"e", g.calibration.e); number(L"f", g.calibration.f);
    number(L"windowX", s.windowRegion.x); number(L"windowY", s.windowRegion.y); number(L"windowW", s.windowRegion.w); number(L"windowH", s.windowRegion.h);
    // 明确写入 UTF-16LE BOM；仅调用带 W 后缀的 INI API 创建文件仍会默认采用 ANSI。
    // 完整配置先写临时文件并刷新；替换失败时原配置保持完整。
    static_assert(sizeof(wchar_t) == 2, "Windows UTF-16 requires two-byte wchar_t");
    if (contents.size() > std::numeric_limits<DWORD>::max() / sizeof(wchar_t)) return false;
    HANDLE handle = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const DWORD bytes = static_cast<DWORD>(contents.size() * sizeof(wchar_t));
    bool ok = WriteFile(handle, contents.data(), bytes, &written, nullptr) != 0 && written == bytes;
    if (ok) ok = FlushFileBuffers(handle) != 0;
    if (!CloseHandle(handle)) ok = false;
    if (ok) ok = MoveFileExW(file.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (ok) WritePrivateProfileStringW(nullptr, nullptr, nullptr, destination.c_str());
    else DeleteFileW(file.c_str());
    return ok;
}
