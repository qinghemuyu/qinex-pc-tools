#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include "android_tools_service.h"
#include "android_cursor_package.h"
#include "embedded_runtime.h"

namespace at = qpointer::android;
void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void CheckDiscoveryWorkflow(const std::filesystem::path& dataRoot) {
    std::string output = "List of devices attached\r\nUSB123456              device product:fixture model:Real_Phone device:fixture transport_id:1\r\n10.0.0.1:37123         unauthorized transport_id:2\r\nUSB-OFFLINE            offline transport_id:3\r\n";
    at::ToolsService service([&](const std::vector<std::wstring>& args, const std::atomic<bool>&, unsigned long, const std::string&) {
        Check(args == std::vector<std::wstring>{L"devices", L"-l"}, "刷新执行了非只读设备命令");
        at::ProcessResult result; result.exitCode = 0; result.output = output; return result;
    }, dataRoot);
    const auto refresh = [&] {
        at::Request request; request.action = at::Action::Refresh;
        Check(service.Start(nullptr, request), "刷新任务未启动");
        const auto start = GetTickCount64();
        while (service.Snapshot().busy && GetTickCount64() - start < 5000) Sleep(5);
        auto snapshot = service.Snapshot(); Check(!snapshot.busy && !snapshot.failed, "刷新任务失败或超时"); return snapshot;
    };
    auto discovered = refresh();
    Check(discovered.devices.size() == 3 && discovered.devices[0].model == "Real Phone" &&
        discovered.devices[1].state == "unauthorized" && discovered.devices[2].state == "offline", "真实详细列表格式刷新丢失手机");
    const auto detail = at::ToUtf8(discovered.detail);
    Check(detail.find("待授权 1") != std::string::npos && detail.find("离线或不可用 1") != std::string::npos &&
        detail.find(output) != std::string::npos, "刷新诊断没有保留设备状态或原文");
    output = "List of devices attached\r\n\r\n";
    discovered = refresh();
    Check(discovered.devices.empty() && at::ToUtf8(discovered.detail).find("ADB 已运行，但未发现手机") != std::string::npos, "空列表仍伪装为发现设备");
    service.Stop();
    std::cout << "ADB detailed listing and refresh diagnostics checks passed\n";
}
void CheckPhoneWorkflow(const std::filesystem::path& dataRoot) {
    std::optional<std::string> setting; std::string force; unsigned writes = 0, forceAttempts = 0;
    std::optional<bool> buildEnabled = false;
    at::ToolsService service([&](const std::vector<std::wstring>& args, const std::atomic<bool>& cancel, unsigned long, const std::string&) {
        at::ProcessResult result; result.exitCode = 0;
        if (cancel.load()) { result.cancelled = true; return result; }
        if (args == std::vector<std::wstring>{L"-s", L"phone-A", L"uninstall", at::ToWide(at::kOverlayPackage)}) {
            ++writes; result.output = "Success\n"; return result;
        }
        Check(args.size() >= 4 && args[0] == L"-s" && args[2] == L"shell", "手机命令没有绑定设备");
        const bool another = args[1] == L"phone-B";
        const auto command = at::ToUtf8(args.back()); const bool root = command.rfind("su -c ", 0) == 0;
        if (command == "getprop") result.output = "[ro.serialno]: [" + std::string(another ? "B" : "A") + "]\n[ro.product.brand]: [fixture]\n[ro.product.model]: [Mock Phone]\n[ro.build.version.sdk]: [35]\n[ro.build.fingerprint]: [fixture/android-15]\n[" + at::kForceProperty + "]: [" + force + "]\n";
        else if (command == "am get-current-user") result.output = "10\n";
        else if (command.find("get system show_touches") != std::string::npos) result.output = "null\n";
        else if (command.find("list secure") != std::string::npos) result.output = setting ? "stylus_pointer_icon_enabled=" + *setting + '\n' : "other=1\n";
        else if (command.find("android:bool/config_enableStylusPointerIcon") != std::string::npos) {
            if (buildEnabled) result.output = *buildEnabled ? "true\n" : "false\n";
            else { result.exitCode = 1; result.output = "lookup unavailable\n"; }
        }
        else if (command == "cmd overlay list --user 0") result.output = "android\n";
        else if (command == "cmd overlay disable --user 0 " + std::string(at::kOverlayPackage)) ++writes;
        else if (root && command.find("id -u") != std::string::npos) result.output = "0\n";
        else if (root && command.find("magisk -v") != std::string::npos) result.output = "fixture-magisk\n";
        else if (command.find("put secure stylus_pointer_icon_enabled") != std::string::npos) { setting = command.find("'0'") != std::string::npos ? "0" : "1"; ++writes; }
        else if (command.find("delete secure stylus_pointer_icon_enabled") != std::string::npos) { setting.reset(); ++writes; }
        else if (command.find("setprop ") != std::string::npos) {
            ++forceAttempts;
            if (!root) { result.exitCode = 1; result.output = "permission denied"; }
            else { force = command.find("'0'") != std::string::npos ? "0" : "1"; ++writes; }
        } else throw std::runtime_error("模拟手机收到未预期命令：" + command);
        return result;
    }, dataRoot);
    const auto run = [&](at::Action action, bool root = false, const std::string& serial = "phone-A") {
        at::Request request; request.action = action; request.serial = serial; request.useRoot = root;
        Check(service.Start(nullptr, request), "任务未启动");
        const auto start = GetTickCount64();
        while (service.Snapshot().busy && GetTickCount64() - start < 5000) Sleep(5);
        auto snapshot = service.Snapshot(); Check(!snapshot.busy, "模拟手机任务超时"); return snapshot;
    };
    auto denied = run(at::Action::Enable);
    Check(denied.failed && setting == std::optional<std::string>("1") && force.empty(), "权限拒绝没有保留真实部分写入状态");
    Check(at::ToUtf8(denied.detail).find("用户开关已写入并读回为 1") != std::string::npos &&
        at::ToUtf8(denied.detail).find("ROM 关闭了") != std::string::npos, "部分写入诊断没有区分 ROM 资源限制");
    auto restored = run(at::Action::RestoreSetting);
    Check(!restored.failed && !setting && force.empty(), "用户开关部分写入不能撤销");
    buildEnabled = true;
    const auto beforeOrdinaryEnable = forceAttempts;
    auto ordinary = run(at::Action::Enable);
    Check(!ordinary.failed && ordinary.phone->stylusBuildEnabled == std::optional<bool>(true) &&
        setting == std::optional<std::string>("1") && force.empty() && forceAttempts == beforeOrdinaryEnable,
        "ROM 允许用户开关时仍强制写调试属性");
    restored = run(at::Action::RestoreSetting);
    Check(!restored.failed && !setting && force.empty(), "普通用户开关开启不能恢复");
    buildEnabled.reset();
    denied = run(at::Action::Enable);
    Check(denied.failed && !denied.phone->stylusBuildEnabled && at::ToUtf8(denied.detail).find("状态尚未确认") != std::string::npos,
        "ROM 查询失败被误当成允许开启");
    Check(!run(at::Action::RestoreSetting).failed, "未知 ROM 的部分写入不能恢复");
    buildEnabled = false;
    auto root = run(at::Action::Root); Check(!root.failed && root.phone->root && root.phone->magisk, "显式 ROOT 检查失败");
    auto enabled = run(at::Action::Enable, true); Check(!enabled.failed && force == "1", "ROOT 开启未读回确认");
    Check(!run(at::Action::Enable, true).failed, "重复开启失败");
    restored = run(at::Action::RestoreSetting, true);
    Check(!restored.failed && !setting && force == "0", "重复开启覆盖了最初备份");
    const auto before = writes;
    auto wrong = run(at::Action::RestoreSetting, false, "phone-B");
    Check(wrong.failed && writes == before, "跨设备恢复产生了写入");
    root = run(at::Action::Root); Check(!root.failed, "恢复图标前 ROOT 检查失败");
    const auto directory = at::PhoneDirectory(*root.phone);
    at::AtomicWrite(directory / L"package-phone.txt", at::SerializePhone(*root.phone));
    at::AtomicWrite(directory / L"image-installation.txt", "ordinary");
    const auto beforeImageRestore = writes;
    const auto imageRestored = run(at::Action::RestoreImage, true);
    Check(!imageRestored.failed && writes == beforeImageRestore + 2 &&
        !std::filesystem::exists(directory / L"image-installation.txt"), "勾选 ROOT 后遗漏普通覆盖恢复");
    service.Stop(); std::cout << "Mock phone permission, repeat-enable and restore checks passed\n";
}
void CheckTouchFeedbackWorkflow(const std::filesystem::path& dataRoot) {
    for (const auto& original : std::vector<std::optional<std::string>>{std::nullopt, "0", "1"}) {
        auto value = original;
        bool unreadable = false, denyWrite = false, ignoreWrite = false, timeoutWrite = false;
        unsigned attempts = 0, rootWrites = 0;
        std::string fingerprint = "fixture/android-16";
        const auto directory = dataRoot / at::ToWide(original.value_or("absent"));
        at::AdbExecutor executor = [&](const std::vector<std::wstring>& args, const std::atomic<bool>&, unsigned long, const std::string&) {
            Check(args.size() == 4 && args[0] == L"-s" && args[2] == L"shell", "触摸反馈命令未绑定手机");
            at::ProcessResult result; result.exitCode = 0;
            const auto command = at::ToUtf8(args.back());
            const bool root = command.rfind("su -c ", 0) == 0;
            if (command == "getprop") result.output = "[ro.serialno]: [" + std::string(args[1] == L"phone-B" ? "B" : "A") + "]\n[ro.product.brand]: [fixture]\n[ro.product.model]: [Touch Phone]\n[ro.build.version.sdk]: [36]\n[ro.build.fingerprint]: [" + fingerprint + "]\n";
            else if (command == "am get-current-user") result.output = "10\n";
            else if (command.find("list secure") != std::string::npos) result.output = "stylus_pointer_icon_enabled=0\n";
            else if (command.find("get system show_touches") != std::string::npos) {
                Check(command.find("--user 10") != std::string::npos, "触摸反馈读取了错误用户");
                result.output = unreadable ? "unexpected result\n" : value.value_or("null") + '\n';
            } else if (command.find("android:bool/config_enableStylusPointerIcon") != std::string::npos) result.output = "true\n";
            else if (command == "cmd overlay list --user 0") result.output = "android\n";
            else if (root && command.find("id -u") != std::string::npos) result.output = "0\n";
            else if (root && command.find("magisk -v") != std::string::npos) result.output = "fixture-magisk\n";
            else if (command.find("put system show_touches") != std::string::npos || command.find("delete system show_touches") != std::string::npos) {
                Check(command.find("--user 10") != std::string::npos, "触摸反馈写入了错误用户");
                ++attempts;
                if (denyWrite && !root) { result.exitCode = 1; result.output = "permission denied\n"; }
                else {
                    if (root) ++rootWrites;
                    if (!ignoreWrite) {
                        if (command.find("delete system") != std::string::npos) value.reset();
                        else value = command.find("show_touches 1") != std::string::npos ? "1" : "0";
                    }
                    if (timeoutWrite) { result.timedOut = true; result.exitCode = ERROR_TIMEOUT; }
                }
            } else throw std::runtime_error("触摸反馈操作修改了其它设置或执行了未知命令：" + command);
            return result;
        };
        auto service = std::make_unique<at::ToolsService>(executor, directory);
        const auto run = [&](at::Action action, bool useRoot = false, const std::string& serial = "phone-A") {
            at::Request request; request.action = action; request.serial = serial; request.useRoot = useRoot;
            Check(service->Start(nullptr, request), "触摸反馈任务未启动");
            const auto start = GetTickCount64();
            while (service->Snapshot().busy && GetTickCount64() - start < 5000) Sleep(5);
            auto snapshot = service->Snapshot(); Check(!snapshot.busy, "触摸反馈任务超时"); return snapshot;
        };
        unreadable = true;
        auto state = run(at::Action::Inspect);
        Check(!state.failed && !state.phone->showTouchesKnown, "未知触摸反馈状态妨碍其它手机检查");
        state = run(at::Action::EnableTouchFeedback);
        Check(state.failed && attempts == 0, "未取得原设置就写入触摸反馈");
        unreadable = false;
        state = run(at::Action::EnableTouchFeedback);
        Check(!state.failed && state.phone->showTouchesKnown && value == std::optional<std::string>("1"), "普通 ADB 未开启并读回触摸反馈");
        Check(!run(at::Action::EnableTouchFeedback).failed, "重复开启触摸反馈失败");
        state = run(at::Action::DisableTouchFeedback);
        Check(!state.failed && value == std::optional<std::string>("0"), "关闭触摸反馈失败");
        service->Stop(); service = std::make_unique<at::ToolsService>(executor, directory);
        state = run(at::Action::RestoreTouchFeedback);
        Check(!state.failed && state.phone->showTouchesKnown && value == original, "重启工具或重复开启丢失最初反馈设置");
        const auto attemptsBefore = attempts;
        fingerprint = "fixture/after-ota";
        Check(run(at::Action::RestoreTouchFeedback).failed && attempts == attemptsBefore, "系统升级后错误恢复反馈设置");
        fingerprint = "fixture/android-16";
        Check(run(at::Action::RestoreTouchFeedback, false, "phone-B").failed && attempts == attemptsBefore, "跨手机恢复写入了触摸反馈");
        denyWrite = true;
        state = run(at::Action::EnableTouchFeedback);
        Check(state.failed && value == original && at::ToUtf8(state.detail).find("permission denied") != std::string::npos && rootWrites == 0, "反馈写入拒绝被伪装成功或自动使用 ROOT");
        Check(!run(at::Action::Root).failed, "触摸反馈 ROOT 检查失败");
        state = run(at::Action::EnableTouchFeedback, true);
        Check(!state.failed && value == std::optional<std::string>("1") && rootWrites == 1, "显式 ROOT 未用于反馈写入");
        Check(!run(at::Action::RestoreTouchFeedback, true).failed && value == original, "ROOT 反馈写入不能恢复");
        denyWrite = false; ignoreWrite = true;
        state = run(original == std::optional<std::string>("1") ? at::Action::DisableTouchFeedback : at::Action::EnableTouchFeedback);
        Check(state.failed && value == original && at::ToUtf8(state.detail).find("读回") != std::string::npos, "反馈命令返回成功但未生效仍报告成功");
        ignoreWrite = false; timeoutWrite = true;
        state = run(at::Action::EnableTouchFeedback);
        Check(state.failed && state.phone->showTouches == std::optional<std::string>("1") && at::ToUtf8(state.detail).find("超时") != std::string::npos, "反馈写入超时后丢失真实状态或备份");
        timeoutWrite = false;
        Check(!run(at::Action::RestoreTouchFeedback).failed && value == original, "反馈写入超时后不能恢复");
        service->Stop();
    }
    std::cout << "Touch feedback user, backup, permission and readback checks passed\n";
}
int wmain(int argc, wchar_t** argv) {
    if (argc > 1 && std::wstring(argv[1]) == L"--echo") {
        for (int i = 2; i < argc; ++i) { const auto value = at::ToUtf8(argv[i]); std::cout << value.size() << ':' << value << '\n'; } return 0;
    }
    if (argc > 1 && std::wstring(argv[1]) == L"--sleep") { Sleep(8000); return 0; }
    if (argc > 1 && std::wstring(argv[1]) == L"--input") { std::string line; std::getline(std::cin, line); std::cout << line; return 0; }
    try {
        const auto dataRoot = argc > 1 && std::wstring(argv[1]) == L"--package" && argc == 4 ? std::filesystem::path(argv[3]) / L"data"
            : std::filesystem::temp_directory_path() / L"QInEX-Android-tests" / std::to_wstring(GetCurrentProcessId()) / L"data";
        at::ScopedDataDirectory scope(dataRoot);
        wchar_t systemDirectory[32768]; GetSystemDirectoryW(systemDirectory, 32768);
        SetEnvironmentVariableW(L"PATH", systemDirectory);
        for (const wchar_t* name : {L"JAVA_HOME", L"JAVA_TOOL_OPTIONS", L"_JAVA_OPTIONS", L"JDK_JAVA_OPTIONS"}) SetEnvironmentVariableW(name, nullptr);
        std::atomic<bool> cancel{false}; wchar_t filename[32768]; GetModuleFileNameW(nullptr, filename, 32768);
        const std::vector<std::wstring> arguments{L"", L"中文路径\\图片.png", L"with spaces\\", L"a\\\"b", L"& whoami | echo test", L"$(literal)"};
        std::vector<std::wstring> command{L"--echo"}; command.insert(command.end(), arguments.begin(), arguments.end());
        const auto result = at::RunProcess(filename, command, cancel);
        std::string expected; for (const auto& arg : arguments) { auto utf8 = at::ToUtf8(arg); expected += std::to_string(utf8.size()) + ':' + utf8 + "\r\n"; }
        Check(result.Ok() && result.output == expected, "Windows 参数未按原值到达子进程");
        Check(at::RunProcess(filename, {L"--input"}, cancel, 5000, "654321\n").output == "654321", "配对码标准输入失败");
        auto timed = at::RunProcess(filename, {L"--sleep"}, cancel, 120);
        Check(timed.timedOut && !timed.Ok(), "子进程超时未生效");
        std::thread stop([&] { Sleep(100); cancel.store(true); });
        auto cancelled = at::RunProcess(filename, {L"--sleep"}, cancel); stop.join();
        Check(cancelled.cancelled && !cancelled.Ok(), "取消未终止工具"); cancel.store(false);
        Check(at::ShellQuote("a'b; c") == "'a'\\''b; c'", "Android shell 引用不正确");
        auto devices = at::ParseDevices("List of devices attached\r\nUSB1\tdevice product:x model:My_Phone transport_id:1\r\n10.0.0.1:33001\tunauthorized\r\nUSB3\toffline\r\n");
        Check(devices.size() == 3 && devices[0].model == "My Phone" && devices[1].state == "unauthorized", "设备状态解析失败");
        devices = at::ParseDevices("* daemon started successfully\r\nList of devices attached\r\nUSB123456              device product:x model:My_Phone transport_id:1\r\n10.0.0.1:33001         unauthorized transport_id:2\r\nUSB3                   offline transport_id:3\r\n* daemon unrelated output\r\n");
        Check(devices.size() == 3 && devices[0].serial == "USB123456" && devices[0].model == "My Phone" &&
            devices[1].state == "unauthorized" && devices[2].state == "offline", "空格分隔的 ADB 详细设备列表被丢弃");
        Check(at::ParseDevices("List of devices attached\n\n").empty() && at::ParseDevices("unrelated\tdevice\n").empty(), "空列表或诊断文字被误识别为手机");
        Check(at::ValidEndpoint("192.168.1.8:37123") && at::ValidEndpoint("[::1]:37123") && !at::ValidEndpoint("host:70000") && !at::ValidEndpoint("host:12;reboot") && !at::ValidEndpoint("host:0"), "无线地址校验失败");
        Check(!at::FindSetting("other=1\n", "stylus_pointer_icon_enabled"), "未设置状态丢失");
        Check(at::FindSetting("stylus_pointer_icon_enabled=0\n", "stylus_pointer_icon_enabled") == std::optional<std::string>("0"), "原始关闭值丢失");
        at::Phone original; original.identity = "test-device"; original.user = "10"; original.fingerprint = "fingerprint";
        auto restored = at::DeserializePhone(at::SerializePhone(original));
        Check(at::SamePhone(original, restored), "备份不能往返");
        auto commands = at::RestoreCommands(original);
        Check(commands[0] == "settings --user 10 delete secure stylus_pointer_icon_enabled" && commands[1].find("'0'") != std::string::npos, "缺省原值不能恢复");
        restored.identity = "another-device"; Check(!at::SamePhone(original, restored), "跨手机备份被接受");
        restored = original; restored.fingerprint = "after-ota"; Check(!at::SamePhone(original, restored), "OTA 后错误沿用缓存");
        original.setting = "0"; Check(at::RestoreCommands(original)[0].find("put secure stylus_pointer_icon_enabled '0'") != std::string::npos, "明确关闭值未恢复");
        auto profile = at::SelectProfile(1, "resource 0x01080001 drawable/pointer_crosshair_icon\nresource 0x01080002 drawable/pointer_crosshair_large_icon\n");
        Check(profile.icons.size() == 2, "大图标路径未适配");
        auto policy = at::ResolvePolicy(profile, "name=\"Pointer\" actor=\"\"\n policies=\"product\"\n  drawable/pointer_crosshair_icon\n  drawable/pointer_crosshair_large_icon\n");
        Check(policy.targetName == "Pointer" && policy.partition == "product" && !policy.publicPolicy, "覆盖集合策略解析失败");
        bool blocked = false;
        try { at::ResolvePolicy(profile, "name=\"Pointer\" actor=\"\"\n policies=\"signature\"\n  drawable/pointer_crosshair_icon\n  drawable/pointer_crosshair_large_icon\n"); } catch (...) { blocked = true; }
        Check(blocked, "签名策略被错误接受");
        Check(at::StringHash("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 校验失败");
        const auto runtime = at::EnsureRuntime("adb", cancel);
        const auto adbVersion = at::RunProcess(runtime / L"adb/adb.exe", {L"version"}, cancel, 5000);
        Check(adbVersion.Ok() && adbVersion.output.find("35.0.2-12147458") != std::string::npos, "内嵌 ADB 离线启动失败");
        const auto beforeRepair = at::FileHash(runtime / L"adb/NOTICE.txt");
        at::AtomicWrite(runtime / L"adb/NOTICE.txt", "corrupted cache");
        at::EnsureRuntime("adb", cancel);
        Check(at::FileHash(runtime / L"adb/NOTICE.txt") == beforeRepair, "损坏依赖缓存未被修复");
        CheckDiscoveryWorkflow(dataRoot / L"discovery");
        CheckPhoneWorkflow(dataRoot);
        CheckTouchFeedbackWorkflow(dataRoot / L"touch-feedback");
        std::cout << "Android tools behavior checks passed\n";
        if (argc > 1 && std::wstring(argv[1]) == L"--adb-smoke") {
            at::AdbClient client; client.Initialize(cancel);
            const auto list = client.Run({L"devices", L"-l"}, cancel, 10000);
            Check(list.Ok(), "ADB 服务与设备发现失败");
            std::cout << "ADB service smoke passed; device count=" << at::ParseDevices(list.output).size() << '\n';
            client.Shutdown();
        }
        if (argc > 1 && std::wstring(argv[1]) == L"--package") {
            Check(argc == 4, "包验证参数缺失");
            Gdiplus::GdiplusStartupInput startup; ULONG_PTR token; Check(Gdiplus::GdiplusStartup(&token, &startup, nullptr) == Gdiplus::Ok, "GDI+ 启动失败");
            const std::filesystem::path directory(argv[3]); std::filesystem::create_directories(directory);
            const auto image = directory / L"输入图片.png";
            Gdiplus::Bitmap bitmap(128, 64, PixelFormat32bppARGB); Gdiplus::Graphics graphics(&bitmap); graphics.Clear(Gdiplus::Color(128, 255, 100, 60));
            UINT count, bytes; Gdiplus::GetImageEncodersSize(&count, &bytes); std::vector<unsigned char> data(bytes); auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(data.data()); Gdiplus::GetImageEncoders(count, bytes, codecs);
            for (UINT i = 0; i < count; ++i) if (wcscmp(codecs[i].MimeType, L"image/png") == 0) Check(bitmap.Save(image.c_str(), &codecs[i].Clsid, nullptr) == Gdiplus::Ok, "测试图片保存失败");
            at::Phone phone; phone.identity = "QInEX-automated-package-fixture"; phone.user = "0"; phone.sdk = 34; phone.fingerprint = "test-only/android-34"; phone.framework = argv[2]; phone.frameworkHash = at::FileHash(phone.framework);
            phone.resources = "resource 0x01080001 drawable/pointer_crosshair_icon\nresource 0x01080002 drawable/pointer_crosshair_large_icon\n";
            at::Request request; request.profile = 1; request.image = image; request.sizeDp = 32; request.hotspotX = request.hotspotY = 1; request.exportPath = directory / L"光标模块.zip";
            const auto package = at::BuildCursorPackage(phone, request, cancel, [](const std::string& s) { std::cout << s << '\n'; });
            Check(std::filesystem::is_regular_file(package) && at::FileHash(package) == at::FileHash(request.exportPath), "导出包不一致");
            std::cout << "Signed APK and Magisk package checks passed\n";
            // 静态 GDI+ 解码组件在进程退出时释放，测试不提前销毁其运行时。
        }
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
