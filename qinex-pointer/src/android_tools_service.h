#pragma once
#include <windows.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include "adb_client.h"

namespace qpointer::android {
inline constexpr UINT kToolsMessage = WM_APP + 40;
inline constexpr char kForceProperty[] = "persist.debug.input.force_enable_stylus_pointer_icon";
inline constexpr char kOverlayPackage[] = "com.qinex.pointer.stylus";
inline constexpr char kModuleId[] = "qinex_stylus_cursor";
struct Phone {
    std::string serial, identity, model, fingerprint, user, force;
    std::optional<std::string> setting;
    // 当前用户的框架资源开关；查询不可用时保持未知，不推断厂商行为。
    std::optional<bool> stylusBuildEnabled;
    // 缺失值与查询失败分别记录；触摸反馈使用独立备份，不改手写笔开关。
    std::optional<std::string> showTouches;
    bool showTouchesKnown = false;
    int sdk = 0;
    bool root = false, magisk = false;
    std::string frameworkPath, frameworkHash, resources, overlayables, overlays;
    std::filesystem::path framework;
};
enum class Action { Refresh, Connect, Pair, Inspect, Root, Enable, RestoreSetting, Reboot,
    Analyze, Build, InstallMagisk, InstallOverlay, RestoreImage, RestorePrevious, Verify, Dependencies,
    EnableTouchFeedback, DisableTouchFeedback, RestoreTouchFeedback };
struct Request {
    Action action = Action::Refresh;
    std::string serial, endpoint, pairingCode;
    bool useRoot = false, confirmedVisible = false;
    int profile = 0, sizeDp = 32;
    std::filesystem::path image, exportPath;
    double hotspotX = .5, hotspotY = .5;
};
struct ToolsState {
    bool busy = false;
    uint64_t generation = 0;
    Action action = Action::Refresh;
    std::vector<Device> devices;
    std::optional<Phone> phone;
    std::wstring detail;
    bool failed = false;
    std::filesystem::path package;
};
// 状态按值跨线程。只投递固定消息，不将对象指针或控件句柄交给工作线程。
class ToolsService {
    mutable std::mutex mutex;
    ToolsState state;
    std::atomic<bool> cancel{false};
    std::thread worker;
    HWND owner = nullptr;
    AdbClient adb;
    std::filesystem::path dataRoot;
    void Execute(Request request);
    void Progress(const std::string& text);
    Phone Inspect(const std::string& serial, bool root);
    ProcessResult Command(const Phone& phone, const std::string& command, bool root = false, unsigned long timeout = 30000);
    void SaveBaseline(const Phone& phone);
    void RestoreSettings(const Phone& phone, bool root);
public:
    explicit ToolsService(AdbExecutor executor = {}, std::filesystem::path directory = {}) : adb(std::move(executor)), dataRoot(std::move(directory)) {}
    ~ToolsService();
    bool Start(HWND window, Request request);
    ToolsState Snapshot() const;
    void Cancel() { cancel.store(true); }
    void Stop();
};
std::optional<std::string> FindSetting(const std::string& list, const std::string& key);
std::string Property(const std::string& list, const std::string& key);
bool SamePhone(const Phone& expected, const Phone& actual);
std::vector<std::string> RestoreCommands(const Phone& original);
std::filesystem::path PhoneDirectory(const Phone& phone);
std::string SerializePhone(const Phone& phone);
Phone DeserializePhone(const std::string& content);
}
