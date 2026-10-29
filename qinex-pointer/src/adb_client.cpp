#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <algorithm>
#include <cctype>
#include <sstream>
#include <stdexcept>
#include "adb_client.h"
#include "embedded_runtime.h"

namespace qpointer::android {
namespace {
DWORD ServerPid(int port) {
    DWORD bytes = 0; GetExtendedTcpTable(nullptr, &bytes, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0);
    std::vector<unsigned char> storage(bytes);
    if (!bytes || GetExtendedTcpTable(storage.data(), &bytes, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) != NO_ERROR) return 0;
    const auto* table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(storage.data());
    for (DWORD i = 0; i < table->dwNumEntries; ++i) if (ntohs(static_cast<u_short>(table->table[i].dwLocalPort)) == port &&
        (table->table[i].dwLocalAddr == htonl(INADDR_LOOPBACK) || table->table[i].dwLocalAddr == INADDR_ANY)) return table->table[i].dwOwningPid;
    return 0;
}
// -1 表示没有监听，0 表示监听者并非可核验的 ADB 服务。
int ServerVersion(int port) {
    WSADATA data{}; if (WSAStartup(MAKEWORD(2, 2), &data)) return 0;
    SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct Guard { SOCKET s; ~Guard() { closesocket(s); WSACleanup(); } } guard{socket};
    if (socket == INVALID_SOCKET) return 0;
    // Windows 对无监听端口的异步 connect 可能重试后才拒绝；先用独占 bind
    // 判断真正空闲端口，既不依赖拒绝延迟，也不占用其它程序的监听地址。
    SOCKET probe = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (probe != INVALID_SOCKET) {
        BOOL exclusive = TRUE; setsockopt(probe, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
        sockaddr_in local{}; local.sin_family = AF_INET; local.sin_port = htons(static_cast<u_short>(port)); local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        const bool free = bind(probe, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0;
        closesocket(probe); if (free) return -1;
    }
    DWORD timeout = 600; setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(static_cast<u_short>(port)); addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    u_long nonblocking = 1; ioctlsocket(socket, FIONBIO, &nonblocking);
    if (connect(socket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr))) {
        const auto error = WSAGetLastError();
        if (error == WSAECONNREFUSED) return -1;
        if (error != WSAEWOULDBLOCK) return 0;
        fd_set writable, exceptional; FD_ZERO(&writable); FD_ZERO(&exceptional); FD_SET(socket, &writable); FD_SET(socket, &exceptional);
        timeval limit{0, 600000}; if (select(0, nullptr, &writable, &exceptional, &limit) <= 0) return 0;
        int socketError = 0, length = sizeof(socketError);
        if (getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&socketError), &length)) return 0;
        if (socketError) return socketError == WSAECONNREFUSED ? -1 : 0;
    }
    nonblocking = 0; ioctlsocket(socket, FIONBIO, &nonblocking);
    const std::string query = "000chost:version";
    if (send(socket, query.data(), static_cast<int>(query.size()), 0) != static_cast<int>(query.size())) return 0;
    const auto receive = [&](size_t n) {
        std::string value(n, 0); size_t got = 0;
        while (got < n) { int count = recv(socket, value.data() + got, static_cast<int>(n - got), 0); if (count <= 0) return std::string(); got += count; }
        return value;
    };
    if (receive(4) != "OKAY" || receive(4) != "0004") return 0;
    const auto version = receive(4);
    if (version.size() != 4 || !std::all_of(version.begin(), version.end(), [](unsigned char c) { return std::isxdigit(c); })) return 0;
    return std::stoi(version, nullptr, 16);
}
}
std::vector<Device> ParseDevices(const std::string& output) {
    std::vector<Device> devices; std::istringstream lines(output); std::string line;
    bool listing = false;
    while (std::getline(lines, line)) {
        line = Trim(line);
        if (line.rfind("List of devices attached", 0) == 0) { listing = true; continue; }
        if (!listing || line.empty()) continue;
        // devices 的短列表使用制表符，devices -l 则使用空格对齐。
        // 两种格式都按空白拆词，先核验状态以排除 ADB 的其它输出。
        std::istringstream tokens(line); Device device; tokens >> device.serial >> device.state;
        if (device.serial.empty() || device.state.empty()) continue;
        if (device.state == "no") {
            std::string next; tokens >> next;
            if (next != "permissions") continue;
            device.state = "no permissions";
        }
        static const std::vector<std::string> states{"device", "unauthorized", "offline", "authorizing",
            "connecting", "recovery", "sideload", "rescue", "bootloader", "host", "unknown", "no permissions"};
        if (std::find(states.begin(), states.end(), device.state) == states.end()) continue;
        std::string token;
        while (tokens >> token) if (token.rfind("model:", 0) == 0) { device.model = token.substr(6); std::replace(device.model.begin(), device.model.end(), '_', ' '); }
        devices.push_back(std::move(device));
    }
    return devices;
}
std::string DescribeDevices(const std::vector<Device>& devices) {
    if (devices.empty()) return "ADB 已运行，但未发现手机。请确认 USB 调试已开启、手机已解锁，并使用数据线直连电脑；检查 Windows 的 Android ADB 接口驱动，也可使用无线调试。";
    size_t authorized = 0, unauthorized = 0, offline = 0;
    for (const auto& device : devices) {
        if (device.state == "device") ++authorized;
        else if (device.state == "unauthorized" || device.state == "authorizing") ++unauthorized;
        else ++offline;
    }
    std::string summary = "发现 " + std::to_string(devices.size()) + " 台设备：已授权 " + std::to_string(authorized) +
        "，待授权 " + std::to_string(unauthorized) + "，离线或不可用 " + std::to_string(offline) + "。";
    if (unauthorized) summary += "请解锁手机并允许 USB 调试；没有弹窗时可在手机撤销 USB 调试授权后重新连接。";
    if (offline) summary += "离线设备请检查连接后重新插拔或重新连接无线地址。";
    if (authorized) summary += "请选择目标手机，再点击检查手机。";
    return summary;
}
std::string AdbClient::ConnectionDescription() const {
    if (executor) return "ADB 执行器：模拟设备测试";
    return "本机 ADB 服务：127.0.0.1:" + std::to_string(port) +
        (owned ? "（本工具启动）" : "（复用已有服务）") + "\n内嵌 ADB 路径：" + ToUtf8(executable.wstring());
}
bool ValidEndpoint(const std::string& s) {
    const auto colon = s.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= s.size() || s.size() > 300) return false;
    for (size_t i = 0; i < colon; ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (!(std::isalnum(c) || c == '.' || c == '-' || c == ':' || c == '[' || c == ']')) return false;
    }
    unsigned port = 0;
    for (size_t i = colon + 1; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        port = port * 10 + (s[i] - '0'); if (port > 65535) return false;
    }
    return port != 0;
}
void AdbClient::Initialize(const std::atomic<bool>& cancel) {
    if (executor) return;
    const auto runtimeExecutable = EnsureRuntime("adb", cancel) / L"adb/adb.exe";
    if (!executable.empty()) {
        if (ServerVersion(port) == 41) return;
        if (ownedProcess) CloseHandle(ownedProcess); ownedProcess = nullptr; owned = false; executable.clear();
    }
    executable = runtimeExecutable;
    port = 5037;
    int version = ServerVersion(port);
    if (version == 41) return;
    if (version != -1) {
        port = 5057;
        while (port < 5067 && ServerVersion(port) != -1) ++port;
        if (port == 5067) { executable.clear(); throw std::runtime_error("找不到空闲 ADB 服务端口；请检查已有调试程序"); }
    }
    // -H 会让 ADB 将目标视为远程服务并拒绝自动启动；本机启动必须使用
    // -L 固定监听地址，后续客户端再通过 -H/-P 连接已核验的服务。
    auto started = RunProcess(executable,
        {L"-L", L"tcp:localhost:" + std::to_wstring(port), L"start-server"},
        cancel, 20000, {}, false);
    if (!started.Ok() || ServerVersion(port) != 41) {
        executable.clear(); throw std::runtime_error("ADB 服务无法启动或版本不兼容：" + started.output);
    }
    ownedPid = ServerPid(port);
    ownedProcess = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, ownedPid);
    if (ownedProcess) {
        std::wstring path(32768, 0); DWORD length = static_cast<DWORD>(path.size());
        if (QueryFullProcessImageNameW(ownedProcess, 0, path.data(), &length)) {
            path.resize(length); std::error_code error;
            owned = std::filesystem::equivalent(path, executable, error) && !error;
        }
    }
}
ProcessResult AdbClient::Run(const std::vector<std::wstring>& args, const std::atomic<bool>& cancel,
    unsigned long timeout, const std::string& input) {
    if (executor) return executor(args, cancel, timeout, input);
    std::vector<std::wstring> all{L"-H", L"127.0.0.1", L"-P", std::to_wstring(port)};
    all.insert(all.end(), args.begin(), args.end()); return RunProcess(executable, all, cancel, timeout, input, false);
}
ProcessResult AdbClient::Shell(const std::string& serial, const std::string& command,
    const std::atomic<bool>& cancel, bool root, unsigned long timeout) {
    if (serial.empty()) throw std::runtime_error("尚未选择手机");
    const auto line = root ? "su -c " + ShellQuote(command) : command;
    return Run({L"-s", ToWide(serial), L"shell", ToWide(line)}, cancel, timeout);
}
void AdbClient::Shutdown() {
    if (owned && ownedProcess && WaitForSingleObject(ownedProcess, 0) == WAIT_TIMEOUT && ServerPid(port) == ownedPid) {
        std::atomic<bool> cancel{false}; Run({L"kill-server"}, cancel, 3000);
    }
    if (ownedProcess) CloseHandle(ownedProcess); ownedProcess = nullptr; owned = false;
}
}
