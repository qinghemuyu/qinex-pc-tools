#include <windows.h>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include "android_tools_service.h"
#include "android_cursor_package.h"
#include "embedded_runtime.h"

namespace qpointer::android {
namespace {
std::string Require(ProcessResult result, bool trim = true) {
    if (result.cancelled) throw std::runtime_error("操作已取消；已完成的手机修改请重新检查");
    if (result.timedOut) throw std::runtime_error("工具调用超时；手机可能已执行操作，请重新检查");
    if (!result.Ok()) throw std::runtime_error(Trim(result.output).empty() ? "手机命令失败" : Trim(result.output));
    if (result.truncated) throw std::runtime_error("工具输出过长，无法完整核验");
    return trim ? Trim(result.output) : result.output;
}
}
std::filesystem::path PhoneDirectory(const Phone& phone) {
    if (phone.identity.empty()) throw std::runtime_error("未取得稳定手机身份，不能备份或修改手机");
    // 文件名只使用身份的摘要，设备输出不能构造本地路径。
    const auto digest = StringHash(phone.identity);
    return DataDirectory() / L"devices" / digest;
}
std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary); if (!file) throw std::runtime_error("手机备份无法读取");
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
std::string SerializePhone(const Phone& p) {
    std::ostringstream stream;
    stream << std::quoted(p.identity) << '\n' << std::quoted(p.fingerprint) << '\n'
        << std::quoted(p.user) << '\n' << std::quoted(p.force) << '\n'
        << p.setting.has_value() << '\n' << std::quoted(p.setting.value_or("")) << '\n';
    return stream.str();
}
Phone DeserializePhone(const std::string& s) {
    std::istringstream stream(s); Phone p; bool present; std::string setting;
    if (!(stream >> std::quoted(p.identity) >> std::quoted(p.fingerprint) >> std::quoted(p.user)
        >> std::quoted(p.force) >> present >> std::quoted(setting))) throw std::runtime_error("手机备份损坏");
    if (present) p.setting = setting; return p;
}
namespace {
void Journal(const Phone& phone, const std::string& stage) {
    AtomicWrite(PhoneDirectory(phone) / L"operation.txt", SerializePhone(phone) + stage + '\n');
}
void VerifyPackage(const Phone& phone) {
    const auto directory = PhoneDirectory(phone);
    if (!SamePhone(DeserializePhone(ReadFile(directory / L"package-phone.txt")), phone)) throw std::runtime_error("安装包不对应当前手机或系统，请重新生成");
    std::istringstream input(ReadFile(directory / L"package-hashes.txt")); std::string zip, apk; input >> zip >> apk;
    if (zip.empty() || apk.empty() || FileHash(directory / L"cursor.zip") != zip || FileHash(directory / L"QInEXStylus.apk") != apk) throw std::runtime_error("安装包摘要不一致，请重新生成");
}
bool Digits(const std::string& s) { return !s.empty() && s.size() < 10 && s.find_first_not_of("0123456789") == std::string::npos; }
std::filesystem::path TouchFeedbackBackup(const Phone& phone) {
    return PhoneDirectory(phone) / (L"touch-feedback-" + ToWide(phone.user) + L".txt");
}
std::optional<std::string> ReadTouchFeedbackBackup(const std::string& content, const Phone& phone) {
    if (!SamePhone(DeserializePhone(content), phone)) throw std::runtime_error("触摸反馈备份对应的手机、系统或用户不一致");
    const auto value = FindSetting(content, "show_touches");
    if (!value || (*value != "absent" && *value != "0" && *value != "1")) throw std::runtime_error("触摸反馈备份损坏");
    return *value == "absent" ? std::nullopt : value;
}
void SaveTouchFeedbackBackup(const Phone& phone) {
    if (!phone.showTouchesKnown) throw std::runtime_error("未能读取原触摸反馈设置，请重新检查手机后操作");
    const auto file = TouchFeedbackBackup(phone);
    if (std::filesystem::exists(file)) ReadTouchFeedbackBackup(ReadFile(file), phone);
    else AtomicWrite(file, SerializePhone(phone) + "show_touches=" + phone.showTouches.value_or("absent") + '\n');
}
}
std::optional<std::string> FindSetting(const std::string& list, const std::string& key) {
    std::istringstream lines(list); std::string line;
    while (std::getline(lines, line)) if (line.rfind(key + "=", 0) == 0) {
        if (!line.empty() && line.back() == '\r') line.pop_back(); return line.substr(key.size() + 1);
    }
    return std::nullopt;
}
std::string Property(const std::string& list, const std::string& key) {
    const auto prefix = "[" + key + "]: ["; const auto begin = list.find(prefix);
    if (begin == std::string::npos) return {};
    const auto start = begin + prefix.size(), end = list.find("]", start);
    return end == std::string::npos ? std::string() : list.substr(start, end - start);
}
bool SamePhone(const Phone& expected, const Phone& actual) {
    return !expected.identity.empty() && expected.identity == actual.identity && expected.user == actual.user && expected.fingerprint == actual.fingerprint;
}
std::vector<std::string> RestoreCommands(const Phone& p) {
    if (!Digits(p.user)) throw std::runtime_error("备份用户身份无效");
    const auto prefix = "settings --user " + p.user;
    return {p.setting ? prefix + " put secure stylus_pointer_icon_enabled " + ShellQuote(*p.setting)
                       : prefix + " delete secure stylus_pointer_icon_enabled",
        "setprop " + std::string(kForceProperty) + " " + ShellQuote(p.force.empty() ? "0" : p.force)};
}
ToolsService::~ToolsService() { Stop(); }
void ToolsService::Stop() { cancel.store(true); if (worker.joinable()) worker.join(); adb.Shutdown(); }
ToolsState ToolsService::Snapshot() const { std::lock_guard<std::mutex> lock(mutex); return state; }
void ToolsService::Progress(const std::string& text) {
    { std::lock_guard<std::mutex> lock(mutex); state.detail = ToWide(text); }
    PostMessageW(owner, kToolsMessage, 0, 0);
}
bool ToolsService::Start(HWND window, Request request) {
    { std::lock_guard<std::mutex> lock(mutex); if (state.busy) return false; }
    if (worker.joinable()) worker.join(); cancel.store(false); owner = window;
    { std::lock_guard<std::mutex> lock(mutex); state.busy = true; state.failed = false;
      state.action = request.action; ++state.generation; state.detail = L""; }
    worker = std::thread([this, request = std::move(request)]() mutable {
        ScopedDataDirectory scope(dataRoot);
        try { Execute(std::move(request)); }
        catch (const std::exception& error) { std::lock_guard<std::mutex> lock(mutex); state.failed = true; state.detail = ToWide(error.what()); }
        { std::lock_guard<std::mutex> lock(mutex); state.busy = false; }
        PostMessageW(owner, kToolsMessage, 0, 0);
    });
    return true;
}
ProcessResult ToolsService::Command(const Phone& p, const std::string& command, bool root, unsigned long timeout) {
    return adb.Shell(p.serial, command, cancel, root, timeout);
}
Phone ToolsService::Inspect(const std::string& serial, bool root) {
    Phone p; p.serial = serial;
    const auto props = Require(Command(p, "getprop"));
    p.identity = Property(props, "ro.serialno");
    if (p.identity.empty()) p.identity = Property(props, "ro.boot.serialno");
    if (p.identity == "unknown" || p.identity == "null") p.identity.clear();
    if (!p.identity.empty()) p.identity = "serial:" + Property(props, "ro.product.brand") + ":" + p.identity;
    p.model = Property(props, "ro.product.model"); p.fingerprint = Property(props, "ro.build.fingerprint");
    p.force = Property(props, kForceProperty);
    const auto sdk = Property(props, "ro.build.version.sdk");
    if (!Digits(sdk)) throw std::runtime_error("无法取得 Android 版本");
    p.sdk = std::stoi(sdk); p.user = Require(Command(p, "am get-current-user"));
    if (!Digits(p.user)) throw std::runtime_error("无法取得当前 Android 用户");
    p.setting = FindSetting(Require(Command(p, "settings --user " + p.user + " list secure"), false), "stylus_pointer_icon_enabled");
    const auto touches = Command(p, "settings --user " + p.user + " get system show_touches", root, 5000);
    if (touches.Ok() && !touches.truncated) {
        const auto value = Trim(touches.output);
        if (value == "0" || value == "1" || value == "null") {
            p.showTouchesKnown = true;
            if (value != "null") p.showTouches = value;
        }
    }
    const auto buildSetting = Command(p, "cmd overlay lookup --user " + p.user + " android android:bool/config_enableStylusPointerIcon", false, 5000);
    if (buildSetting.Ok() && !buildSetting.truncated) {
        const auto value = Trim(buildSetting.output);
        if (value == "true") p.stylusBuildEnabled = true;
        else if (value == "false") p.stylusBuildEnabled = false;
    }
    if (p.identity.empty()) {
        const auto id = Require(Command(p, "settings --user " + p.user + " get secure android_id"));
        if (!id.empty() && id != "null") p.identity = "android-id:" + id;
    }
    p.overlays = Require(Command(p, "cmd overlay list --user 0"));
    if (root) {
        if (Require(Command(p, "id -u", true)) != "0") throw std::runtime_error("手机没有授予 ROOT 权限");
        p.root = true; auto version = Command(p, "magisk -v", true);
        p.magisk = version.Ok() && !Trim(version.output).empty();
    }
    return p;
}
void ToolsService::SaveBaseline(const Phone& phone) {
    const auto file = PhoneDirectory(phone) / (L"settings-" + ToWide(phone.user) + L".txt");
    if (std::filesystem::exists(file)) {
        if (!SamePhone(DeserializePhone(ReadFile(file)), phone)) throw std::runtime_error("系统或用户发生变化，请先处理旧设置备份");
    } else AtomicWrite(file, SerializePhone(phone));
}
void ToolsService::RestoreSettings(const Phone& phone, bool root) {
    const auto original = DeserializePhone(ReadFile(PhoneDirectory(phone) / (L"settings-" + ToWide(phone.user) + L".txt")));
    if (!SamePhone(original, phone)) throw std::runtime_error("备份对应的手机、系统或用户不一致");
    Journal(phone, "restoring-settings");
    const auto commands = RestoreCommands(original);
    Require(Command(phone, commands[0], root));
    // 原属性为空时，仅当本工具已设置强制值才写 0，不能通用删除 persist 属性。
    if (phone.force != original.force) Require(Command(phone, commands[1], root));
    const auto verified = Inspect(phone.serial, false);
    const auto expectedForce = original.force.empty() && phone.force != original.force ? "0" : original.force;
    if (verified.setting != original.setting || verified.force != expectedForce) throw std::runtime_error("原设置恢复读回不一致，请重新检查");
    Journal(phone, "settings-restored-reboot-pending");
}
void ToolsService::Execute(Request request) {
    if (request.action == Action::Dependencies) {
        Progress("释放依赖清单与许可信息…");
        EnsureRuntime("adb", cancel); EnsureRuntime("tools", cancel); EnsureRuntime("java", cancel);
        Progress("依赖清单和许可信息已准备，可在依赖目录查看。"); return;
    }
    Progress("准备内嵌 ADB…"); adb.Initialize(cancel);
    if (request.action == Action::Refresh || request.action == Action::Connect || request.action == Action::Pair) {
        if (request.action != Action::Refresh) {
            if (!ValidEndpoint(request.endpoint)) throw std::runtime_error("请输入手机显示的主机地址:端口");
            if (request.action == Action::Pair) {
                if (request.pairingCode.size() != 6 || !Digits(request.pairingCode)) throw std::runtime_error("请输入六位无线配对码");
                Require(adb.Run({L"pair", ToWide(request.endpoint)}, cancel, 45000, request.pairingCode + "\n"));
            } else {
                const auto connected = Require(adb.Run({L"connect", ToWide(request.endpoint)}, cancel));
                if (connected.find("connected to") == std::string::npos) throw std::runtime_error(connected);
            }
        }
        Progress("ADB 服务已就绪，正在刷新设备列表…");
        const auto output = Require(adb.Run({L"devices", L"-l"}, cancel), false);
        auto devices = ParseDevices(output);
        const auto detail = std::string(request.action == Action::Pair ? "配对完成；请使用手机的连接地址连接。\n" : "") +
            DescribeDevices(devices) + "\n\n" + adb.ConnectionDescription() + "\n\nADB 设备列表原文：\n" + output;
        { std::lock_guard<std::mutex> lock(mutex); state.devices = std::move(devices); }
        Progress(detail); return;
    }
    Progress("正在检查选定手机…");
    auto previous = Snapshot().phone;
    bool checkedRoot = previous && previous->serial == request.serial && previous->root && request.useRoot;
    Phone phone = Inspect(request.serial, request.action == Action::Root || checkedRoot);
    if (previous && previous->serial == phone.serial && SamePhone(*previous, phone)) {
        phone.framework = previous->framework; phone.frameworkPath = previous->frameworkPath;
        phone.frameworkHash = previous->frameworkHash; phone.resources = previous->resources; phone.overlayables = previous->overlayables;
        phone.root = phone.root || previous->root; phone.magisk = phone.magisk || previous->magisk;
    }
    { std::lock_guard<std::mutex> lock(mutex); if (previous && !SamePhone(*previous, phone)) state.package.clear(); state.phone = phone;
      if (state.package.empty() && !phone.identity.empty()) {
          const auto package = PhoneDirectory(phone) / L"cursor.zip";
          if (std::filesystem::is_regular_file(package)) state.package = package;
      } }
    const auto publishPhone = [&](Phone p) {
        if (SamePhone(phone, p)) { p.root = phone.root; p.magisk = phone.magisk; p.framework = phone.framework;
            p.frameworkHash = phone.frameworkHash; p.resources = phone.resources; p.overlayables = phone.overlayables; p.frameworkPath = phone.frameworkPath; }
        std::lock_guard<std::mutex> lock(mutex); state.phone = std::move(p);
    };
    switch (request.action) {
    case Action::Inspect:
        Progress("手机检查完成。配置值与实际显示分别确认；资源替换前请分析资源。"); break;
    case Action::Root:
        Progress(phone.magisk ? "ROOT 与 Magisk 已确认。后续写入仍由你选择对应操作。" : "ROOT 已确认；没有取得可用 Magisk，不能使用模块安装。"); break;
    case Action::EnableTouchFeedback:
    case Action::DisableTouchFeedback:
    case Action::RestoreTouchFeedback: {
        std::optional<std::string> expected;
        if (request.action == Action::RestoreTouchFeedback) {
            if (!std::filesystem::is_regular_file(TouchFeedbackBackup(phone))) throw std::runtime_error("当前手机与用户没有触摸反馈恢复记录；请先通过本工具开启或关闭反馈");
            expected = ReadTouchFeedbackBackup(ReadFile(TouchFeedbackBackup(phone)), phone);
        } else {
            SaveTouchFeedbackBackup(phone);
            expected = request.action == Action::EnableTouchFeedback ? "1" : "0";
        }
        const auto prefix = "settings --user " + phone.user;
        const auto command = expected ? prefix + " put system show_touches " + *expected
                                      : prefix + " delete system show_touches";
        Journal(phone, "touch-feedback-writing");
        const auto written = Command(phone, command, checkedRoot);
        if (!written.Ok() || written.truncated) {
            // 命令失败也尽可能更新真实状态；取消和断线时不掩盖原始错误。
            try { publishPhone(Inspect(phone.serial, checkedRoot)); } catch (...) {}
            Journal(phone, "touch-feedback-write-unconfirmed");
            Require(written);
        }
        auto verified = Inspect(phone.serial, checkedRoot); publishPhone(verified);
        if (!verified.showTouchesKnown || verified.showTouches != expected) {
            Journal(phone, "touch-feedback-readback-mismatch");
            throw std::runtime_error("触摸反馈读回与目标值不一致，未确认成功；请重新检查手机，原设置备份已保留");
        }
        Journal(phone, "touch-feedback-verified");
        if (request.action == Action::RestoreTouchFeedback) {
            Progress("触摸反馈原设置已恢复并读回；通常即时生效，请在手机确认显示。");
        } else if (expected == std::optional<std::string>("1")) {
            Progress("触摸反馈已开启并读回为 1，通常即时生效。连接 P4 并仅移动光标确认悬停圆点；同时会显示手指触摸点。关闭开发者总开关时系统可能重置此设置。");
        } else {
            Progress("触摸反馈已关闭并读回为 0，通常即时生效。依赖该反馈的悬停圆点也可能隐藏；可恢复原反馈设置。");
        }
        break;
    }
    case Action::Enable: {
        SaveBaseline(phone); Journal(phone, "enabling-settings");
        if (phone.sdk >= 35) {
            Require(Command(phone, "settings --user " + phone.user + " put secure stylus_pointer_icon_enabled 1", checkedRoot));
            if (FindSetting(Require(Command(phone, "settings --user " + phone.user + " list secure"), false), "stylus_pointer_icon_enabled") != std::optional<std::string>("1")) throw std::runtime_error("用户开关写入读回不一致");
            if (phone.stylusBuildEnabled == std::optional<bool>(true) && phone.force != "1") {
                // Android 15+ 的正常路径同时要求框架资源与用户开关为真。
                // ROM 已允许时无需写调试属性；读回仍不能证明实际图标可见。
                auto verified = Inspect(phone.serial, false); publishPhone(verified);
                if (verified.setting != std::optional<std::string>("1") || verified.stylusBuildEnabled != std::optional<bool>(true)) throw std::runtime_error("用户开关或 ROM 配置读回发生变化，请重新检查手机");
                Journal(phone, "user-setting-enabled-visual-check-pending");
                Progress("ROM 允许手写笔光标，用户开关已开启并读回；无需写入强制属性。请连接 P4 并移动手写笔光标确认显示；系统可能选择空图标。"); break;
            }
        }
        if (phone.force != "1") {
            const auto forced = Command(phone, "setprop " + std::string(kForceProperty) + " 1", checkedRoot);
            if (!forced.Ok()) {
                const auto verified = Inspect(phone.serial, false); publishPhone(verified);
                Journal(phone, "user-setting-written-force-denied");
                const auto result = phone.sdk >= 35 && verified.setting == std::optional<std::string>("1")
                    ? "用户开关已写入并读回为 1，但强制属性被手机系统拒绝。" : "强制属性被手机系统拒绝。";
                const auto build = verified.stylusBuildEnabled == std::optional<bool>(false)
                    ? "ROM 关闭了手写笔光标支持，用户开关不能覆盖该限制。"
                    : "ROM 支持状态尚未确认，用户开关不代表实际图标已显示。";
                throw std::runtime_error(std::string(result) + build + "零售系统打开开发者选项不一定允许写入；已有 ROOT 时可检查授权后重试，或恢复原设置。\n\n" + forced.output);
            }
        }
        auto verified = Inspect(phone.serial, false); publishPhone(verified);
        if (verified.force != "1") throw std::runtime_error("强制属性读回不是 1，开启未确认");
        Journal(phone, "force-enabled-reboot-pending"); Progress("强制配置已写入并读回为 1。请重启手机，连接 P4 后确认手写笔悬停图标；后续无需重复写入。"); break;
    }
    case Action::RestoreSetting:
        RestoreSettings(phone, checkedRoot); publishPhone(Inspect(phone.serial, false));
        Progress("原设置已恢复。请重启手机后检查；原属性缺失时撤销强制值为 0，系统默认行为由 ROM 决定。"); break;
    case Action::Reboot:
        Journal(phone, "reboot-requested"); Require(adb.Run({L"-s", ToWide(phone.serial), L"reboot"}, cancel));
        { std::lock_guard<std::mutex> lock(mutex); state.phone.reset(); }
        Progress("已请求手机重启。无线端口可能变化，请重新连接、选定手机并检查。"); break;
    case Action::Analyze: {
        Progress("拉取手机系统资源并检查可替换的图标…");
        auto paths = Require(Command(phone, "pm path android"));
        if (paths.rfind("package:/", 0) != 0 || paths.find('\n') != std::string::npos) throw std::runtime_error("无法唯一识别手机 framework-res.apk");
        phone.frameworkPath = paths.substr(8);
        const auto directory = PhoneDirectory(phone) / L"framework"; std::filesystem::create_directories(directory);
        const auto temporary = directory / L"framework-res.tmp.apk";
        Require(adb.Run({L"-s", ToWide(phone.serial), L"pull", ToWide(phone.frameworkPath), temporary.wstring()}, cancel, 120000));
        phone.frameworkHash = FileHash(temporary); if (phone.frameworkHash.empty()) throw std::runtime_error("资源缓存摘要失败");
        phone.framework = directory / (ToWide(phone.frameworkHash) + L".apk");
        if (std::filesystem::exists(phone.framework)) std::filesystem::remove(temporary);
        else std::filesystem::rename(temporary, phone.framework);
        const auto runtime = EnsureRuntime("tools", cancel);
        phone.resources = Require(RunProcess(runtime / L"tools/aapt2.exe", {L"dump", L"resources", L"--no-values", phone.framework.filename().wstring()}, cancel, 30000, {}, true, phone.framework.parent_path()));
        phone.overlayables = Require(RunProcess(runtime / L"tools/aapt2.exe", {L"dump", L"overlayable", phone.framework.filename().wstring()}, cancel, 30000, {}, true, phone.framework.parent_path()));
        publishPhone(phone);
        Progress("资源已分析。请选择与手机实际悬停图标对应的路径；资源存在不能证明系统使用它。默认空图标的路径不能靠 RRO 显示。"); break;
    }
    case Action::Build: {
        if (!request.confirmedVisible) throw std::runtime_error("请先确认手机已显示悬停图标，再选择该图标的资源路径");
        if (phone.framework.empty() || FileHash(phone.framework) != phone.frameworkHash) throw std::runtime_error("请先重新分析手机资源");
        auto package = BuildCursorPackage(phone, request, cancel, [this](const std::string& text) { Progress(text); });
        { std::lock_guard<std::mutex> lock(mutex); state.package = package; }
        Progress("安装包已生成并校验；尚未安装到手机。可导出，或选择 Magisk 安装。安装后的资源权限和实际显示仍需检查。"); break;
    }
    case Action::InstallMagisk: {
        if (!checkedRoot || !phone.root || !phone.magisk) throw std::runtime_error("请先检查 ROOT，并勾选使用已授权 ROOT");
        const auto directory = PhoneDirectory(phone), package = directory / L"cursor.zip";
        VerifyPackage(phone);
        {
            std::istringstream record(ReadFile(directory / L"package-framework.txt")); std::string path, digest; std::getline(record, path); std::getline(record, digest);
            if (path.empty() || path[0] != '/' || digest.size() != 64) throw std::runtime_error("目标资源记录缺失，请重新分析并生成安装包");
            std::istringstream checked(Require(Command(phone, "sha256sum " + ShellQuote(path)))); std::string actual; checked >> actual;
            if (actual != digest) throw std::runtime_error("手机系统资源已变化，请重新分析并生成安装包");
        }
        SaveBaseline(phone); Journal(phone, "module-staging");
        const std::string module = "/data/adb/modules/" + std::string(kModuleId);
        Require(Command(phone, "for d in /data/adb/modules/" + std::string(kModuleId) + " /data/adb/modules_update/" + kModuleId + "; do [ -L \"$d\" ] && exit 32; if [ -d \"$d\" ]; then grep -qx 'id=" + kModuleId + "' \"$d/module.prop\" && grep -qx 'author=QInEX Pointer' \"$d/module.prop\" || exit 32; fi; done", true));
        // 只备份本工具固定身份；已有其它程序同名模块拒绝覆盖。
        const auto backup = Command(phone, "rm -f /data/local/tmp/qinex-stylus-previous.tgz /data/local/tmp/qinex-stylus-readable.tgz; if [ -d " + module + " ]; then grep -qx 'id=" + std::string(kModuleId) + "' " + module + "/module.prop && grep -qx 'author=QInEX Pointer' " + module + "/module.prop || exit 32; tar -czf /data/local/tmp/qinex-stylus-previous.tgz -C /data/adb/modules " + kModuleId + "; fi", true);
        Require(backup);
        auto exists = Command(phone, "test -f /data/local/tmp/qinex-stylus-previous.tgz", true);
        if (exists.Ok()) {
            Require(Command(phone, "cp /data/local/tmp/qinex-stylus-previous.tgz /data/local/tmp/qinex-stylus-readable.tgz; chmod 644 /data/local/tmp/qinex-stylus-readable.tgz", true));
            Require(adb.Run({L"-s", ToWide(phone.serial), L"pull", L"/data/local/tmp/qinex-stylus-readable.tgz", (directory / L"previous-module.tgz").wstring()}, cancel));
            AtomicWrite(directory / L"previous-module.sha256", FileHash(directory / L"previous-module.tgz"));
            AtomicWrite(directory / L"previous-module-phone.txt", SerializePhone(phone));
        }
        Require(adb.Run({L"-s", ToWide(phone.serial), L"push", package.wstring(), L"/data/local/tmp/qinex-stylus-cursor.zip"}, cancel));
        AtomicWrite(directory / L"image-installation.txt", "magisk");
        Journal(phone, "module-installing");
        const auto installed = Command(phone, "magisk --install-module /data/local/tmp/qinex-stylus-cursor.zip", true, 90000);
        if (!installed.Ok()) {
            // 取消也执行必要收尾：失败候选只影响本工具模块，保留旧模块与备份。
            std::atomic<bool> cleanupCancel{false};
            const auto cleanup = adb.Shell(phone.serial, "d=/data/adb/modules_update/" + std::string(kModuleId) + "; if [ -d \"$d\" ]; then grep -qx 'author=QInEX Pointer' \"$d/module.prop\" || exit 32; touch \"$d/disable\"; fi", cleanupCancel, true);
            Journal(phone, cleanup.Ok() ? "install-failed-candidate-disabled" : "install-failed-recovery-required");
            throw std::runtime_error("模块安装未完成。" + std::string(cleanup.Ok() ? "失败候选已停用，旧模块与备份已保留。" : "收尾失败，请在 Magisk 中停用本工具模块。") + "\n" + installed.output);
        }
        Journal(phone, "module-installed-reboot-pending"); Progress("Magisk 安装命令已完成。请重启手机后检查覆盖状态与实际显示；旧模块备份已保留。"); break;
    }
    case Action::InstallOverlay: {
        VerifyPackage(phone); const auto directory = PhoneDirectory(phone);
        {
            std::istringstream record(ReadFile(directory / L"package-framework.txt")); std::string path, digest; std::getline(record, path); std::getline(record, digest);
            if (path.empty() || path[0] != '/' || digest.size() != 64) throw std::runtime_error("目标资源记录缺失，请重新分析并生成安装包");
            std::istringstream checked(Require(Command(phone, "sha256sum " + ShellQuote(path)))); std::string actual; checked >> actual;
            if (actual != digest) throw std::runtime_error("手机系统资源已变化，请重新分析并生成安装包");
        }
        if (ReadFile(directory / L"package-policy.txt") != "public") throw std::runtime_error("目标资源不允许普通公开覆盖，请选择符合系统策略的 Magisk 路径");
        SaveBaseline(phone);
        AtomicWrite(directory / L"image-installation.txt", "ordinary");
        Journal(phone, "overlay-installing");
        const auto installed = adb.Run({L"-s", ToWide(phone.serial), L"install", L"-r", (directory / L"QInEXStylus.apk").wstring()}, cancel, 90000);
        if (!installed.Ok()) throw std::runtime_error("覆盖 APK 安装失败；签名冲突时需先在原电脑恢复或卸载本工具覆盖包。\n" + installed.output);
        Journal(phone, "overlay-installed-enable-pending");
        Require(Command(phone, "cmd overlay enable --user 0 " + std::string(kOverlayPackage)));
        const auto dump = Require(Command(phone, "cmd overlay dump " + std::string(kOverlayPackage)));
        if (dump.find("STATE_ENABLED") == std::string::npos) throw std::runtime_error("覆盖包未进入启用状态，可能被 idmap 策略拒绝；可恢复原光标。\n" + dump);
        Journal(phone, "overlay-enabled-visual-check-pending"); Progress("普通覆盖包已启用。请检查实际图片；系统缓存未刷新时可重启手机。"); break;
    }
    case Action::RestoreImage: {
        const auto directory = PhoneDirectory(phone), installation = directory / L"image-installation.txt";
        const auto method = std::filesystem::exists(installation) ? Trim(ReadFile(installation)) : std::string();
        if (!method.empty() && method != "ordinary" && method != "magisk") throw std::runtime_error("光标安装记录损坏，请查看诊断详情");
        if (method == "magisk" && !checkedRoot) throw std::runtime_error("本工具记录为 Magisk 安装，请检查并勾选 ROOT 后恢复");
        // 普通覆盖的恢复跟随实际安装记录，不因后来勾选 ROOT 而遗漏 APK。
        // 导出的模块可由用户手动安装，此时仍允许按已授权 ROOT 恢复自有模块。
        if (method == "ordinary" || !checkedRoot) {
            if (!SamePhone(DeserializePhone(ReadFile(directory / L"package-phone.txt")), phone)) throw std::runtime_error("未找到对应手机的本工具安装记录");
            Journal(phone, "ordinary-overlay-removing");
            const auto disabled = Command(phone, "cmd overlay disable --user 0 " + std::string(kOverlayPackage));
            const auto removed = adb.Run({L"-s", ToWide(phone.serial), L"uninstall", ToWide(kOverlayPackage)}, cancel);
            if (!removed.Ok()) throw std::runtime_error("普通覆盖包恢复未完成。Magisk 模块需先检查并勾选 ROOT，再恢复。\n" + disabled.output + "\n" + removed.output);
            Journal(phone, "ordinary-overlay-removed");
            if (std::filesystem::exists(installation)) std::filesystem::remove(installation);
            Progress("本工具普通覆盖包已移除；请检查手机图标，手写笔开启设置保留。"); break;
        }
        if (!phone.magisk) throw std::runtime_error("请先取得 Magisk ROOT 权限");
        Journal(phone, "module-disabling");
        Require(Command(phone, "for d in /data/adb/modules/" + std::string(kModuleId) + " /data/adb/modules_update/" + kModuleId + "; do if [ -d \"$d\" ]; then grep -qx 'author=QInEX Pointer' \"$d/module.prop\" || exit 32; touch \"$d/disable\" \"$d/remove\" || exit; fi; done", true));
        Journal(phone, "image-restored-reboot-pending"); Progress("本工具模块已标记停用和移除。请重启手机检查，手写笔开启设置仍保留。"); break;
    }
    case Action::RestorePrevious: {
        if (!checkedRoot || !phone.magisk) throw std::runtime_error("恢复上一模块需要检查并勾选 Magisk ROOT");
        const auto directory = PhoneDirectory(phone), archive = directory / L"previous-module.tgz";
        if (!SamePhone(DeserializePhone(ReadFile(directory / L"previous-module-phone.txt")), phone) ||
            FileHash(archive) != Trim(ReadFile(directory / L"previous-module.sha256"))) throw std::runtime_error("上一模块备份不对应当前手机或摘要校验失败");
        Journal(phone, "previous-module-restoring");
        Require(adb.Run({L"-s", ToWide(phone.serial), L"push", archive.wstring(), L"/data/local/tmp/qinex-stylus-restore.tgz"}, cancel));
        const std::string script = "for d in /data/adb/modules/" + std::string(kModuleId) + " /data/adb/modules_update/" + kModuleId + "; do [ -L \"$d\" ] && exit 32; if [ -d \"$d\" ]; then grep -qx 'author=QInEX Pointer' \"$d/module.prop\" || exit 32; fi; done; b=/data/adb/qinex_pointer_backup/restore-$(date +%s); mkdir -p \"$b\" || exit; if [ -d /data/adb/modules/" + kModuleId + " ]; then mv /data/adb/modules/" + kModuleId + " \"$b/active\" || exit; fi; if [ -d /data/adb/modules_update/" + kModuleId + " ]; then mv /data/adb/modules_update/" + kModuleId + " \"$b/pending\" || exit; fi; tar -xzf /data/local/tmp/qinex-stylus-restore.tgz -C /data/adb/modules";
        Require(Command(phone, script, true)); Journal(phone, "previous-module-restored-reboot-pending");
        Progress("上一模块备份已恢复，待更新候选已保留到手机备份目录。请重启并检查。"); break;
    }
    case Action::Verify: {
        auto dump = Command(phone, "cmd overlay dump " + std::string(kOverlayPackage));
        std::string detail = "手写笔强制属性：" + (phone.force.empty() ? "未设置" : phone.force) + "\n";
        detail += "ROM 手写笔光标资源配置：" + std::string(phone.stylusBuildEnabled ? (*phone.stylusBuildEnabled ? "启用" : "关闭") : "未能确认") + "\n";
        detail += "触摸反馈 show_touches（当前用户 " + phone.user + "）：" + (phone.showTouchesKnown ? phone.showTouches.value_or("未设置，默认关闭") : "未能确认") + "\n";
        detail += "当前覆盖包：\n" + dump.output;
        if (!dump.Ok()) detail += "\n覆盖包尚未识别或查询失败。";
        else detail += "\n请在手机实际移动光标确认图片；查询成功不等于图片已显示。";
        const auto operation = PhoneDirectory(phone) / L"operation.txt";
        if (std::filesystem::exists(operation)) detail += "\n本工具最近操作阶段：\n" + ReadFile(operation);
        if (!phone.framework.empty()) {
            const auto profileIndex = request.profile;
            if (profileIndex) {
                const auto profile = SelectProfile(profileIndex, phone.resources);
                for (const auto& icon : profile.icons) {
                    const auto lookup = Command(phone, "cmd overlay lookup --user 0 --verbose android android:drawable/" + icon);
                    detail += "\n" + icon + "：\n" + lookup.output;
                }
            }
        }
        if (checkedRoot) {
            const auto module = Command(phone, "d=/data/adb/modules/" + std::string(kModuleId) + "; if [ -d \"$d\" ]; then ls \"$d\"; cat \"$d/overlay-result.txt\" 2>/dev/null; fi", true);
            detail += "\nMagisk 本工具模块：\n" + module.output;
        }
        Progress(detail); break;
    }
    default: break;
    }
}
}
