#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <locale>
#include <sstream>
#include <string>
#include <thread>

namespace {
std::atomic<bool> stopping{false};

BOOL WINAPI ConsoleHandler(DWORD event) {
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) return FALSE;
    stopping.store(true, std::memory_order_relaxed);
    return TRUE;
}

struct Options {
    std::string ip = "127.0.0.1";
    int port = 6533;
    double duration = 10.0;
    int hz = 60;
    std::string pattern = "circle";
    std::string state;
};

void PrintHelp() {
    std::cout <<
        "QInEX Pointer 测试发送器：通过 UDP 发送 JSON v2 光标状态。\n"
        "用法：QInEX-Pointer-Simulator.exe [选项]\n"
        "  --ip IPv4          接收电脑地址，默认 127.0.0.1\n"
        "  --port 端口        1～65535，默认 6533\n"
        "  --duration 秒      大于 0 且不超过 3600，默认 10\n"
        "  --pattern 模式     circle（圆周）/grid（九点）/still（中心静止）\n"
        "  --hz 频率          1～1000，默认 60 Hz\n"
        "  --state 状态       单次 show/hide/press/release/heartbeat\n"
        "  --help             显示帮助\n"
        "单次状态不能与 --pattern/--duration/--hz 同时使用。\n"
        "show、press、release 在中心发送完整 move；hide、heartbeat 发送控制消息。\n"
        "circle 每圈 4 秒，第 1～2.5 秒按下拖动，其余时间释放移动。\n"
        "grid 每秒切换一个九点位置，still 保持中心；两者保持释放状态。\n"
        "连续测试结束或按 Ctrl+C 会清除按下并发送 hide。\n"
        "单次状态发出后立即退出；press 保留按下，需再用 release 或 hide 清除。\n"
        "此工具只测试接收和绘制，不向手机注入真实触摸。\n"
        "示例：QInEX-Pointer-Simulator.exe --pattern still --duration 10\n";
}

bool ParseInteger(const std::string& input, int minimum, int maximum, int& value) {
    if (input.empty()) return false;
    for (char c : input) if (c < '0' || c > '9') return false;
    errno = 0;
    char* end = nullptr;
    const long parsed = std::strtol(input.c_str(), &end, 10);
    if (errno == ERANGE || end == input.c_str() || *end != '\0' ||
        parsed < minimum || parsed > maximum) return false;
    value = static_cast<int>(parsed);
    return true;
}

bool ParseOptions(int argc, char** argv, Options& options) {
    bool continuousOption = false;
    for (int index = 1; index < argc; ++index) {
        const std::string key = argv[index];
        if (index + 1 >= argc) {
            std::cerr << "选项缺少取值：" << key << '\n';
            return false;
        }
        const std::string value = argv[++index];
        if (key == "--ip") options.ip = value;
        else if (key == "--port") {
            if (!ParseInteger(value, 1, 65535, options.port)) {
                std::cerr << "端口必须是 1～65535 的整数。\n";
                return false;
            }
        } else if (key == "--duration") {
            continuousOption = true;
            std::istringstream input(value);
            input.imbue(std::locale::classic());
            input >> std::noskipws >> options.duration;
            if (!input || !input.eof() || !std::isfinite(options.duration) ||
                options.duration <= 0.0 || options.duration > 3600.0) {
                std::cerr << "持续秒数必须大于 0 且不超过 3600。\n";
                return false;
            }
        } else if (key == "--hz") {
            continuousOption = true;
            if (!ParseInteger(value, 1, 1000, options.hz)) {
                std::cerr << "频率必须是 1～1000 的整数。\n";
                return false;
            }
        } else if (key == "--pattern") {
            continuousOption = true;
            options.pattern = value;
            if (value != "circle" && value != "grid" && value != "still") {
                std::cerr << "轨迹只支持 circle、grid、still。\n";
                return false;
            }
        } else if (key == "--state") {
            options.state = value;
            if (value != "show" && value != "hide" && value != "press" &&
                value != "release" && value != "heartbeat") {
                std::cerr << "状态只支持 show、hide、press、release、heartbeat。\n";
                return false;
            }
        } else {
            std::cerr << "未知选项：" << key << '\n';
            return false;
        }
    }
    if (!options.state.empty() && continuousOption) {
        std::cerr << "--state 不能与连续测试选项同时使用。\n";
        return false;
    }
    return true;
}

std::string MoveJson(double x, double y, bool down = false, bool show = true) {
    // 固定 C locale，确保系统小数点习惯不会产生不合法的 JSON。
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::fixed << std::setprecision(5)
        << "{\"type\":\"move\",\"nx\":" << x << ",\"ny\":" << y
        << ",\"show\":" << (show ? "true" : "false")
        << ",\"down\":" << (down ? "true" : "false") << '}';
    return output.str();
}

struct Sender {
    bool started = false;
    SOCKET socket = INVALID_SOCKET;
    sockaddr_in destination{};

    ~Sender() {
        if (socket != INVALID_SOCKET) closesocket(socket);
        if (started) WSACleanup();
    }

    bool Open(const Options& options) {
        WSADATA data{};
        const int error = WSAStartup(MAKEWORD(2, 2), &data);
        if (error != 0) {
            std::cerr << "网络初始化失败：" << error << '\n';
            return false;
        }
        started = true;
        destination.sin_family = AF_INET;
        destination.sin_port = htons(static_cast<u_short>(options.port));
        if (InetPtonA(AF_INET, options.ip.c_str(), &destination.sin_addr) != 1) {
            std::cerr << "接收地址必须是有效的 IPv4。\n";
            return false;
        }
        socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket == INVALID_SOCKET) {
            std::cerr << "创建 UDP socket 失败：" << WSAGetLastError() << '\n';
            return false;
        }
        return true;
    }

    bool Send(const std::string& message) {
        if (sendto(socket, message.data(), static_cast<int>(message.size()), 0,
            reinterpret_cast<const sockaddr*>(&destination), sizeof(destination)) !=
            static_cast<int>(message.size())) {
            std::cerr << "发送 UDP 失败：" << WSAGetLastError() << '\n';
            return false;
        }
        return true;
    }
};
}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc == 2 && std::string(argv[1]) == "--help") {
        PrintHelp();
        return 0;
    }
    Options options;
    if (!ParseOptions(argc, argv, options)) return 2;
    Sender sender;
    if (!sender.Open(options)) return 1;

    if (!options.state.empty()) {
        std::string message;
        if (options.state == "hide" || options.state == "heartbeat") {
            message = "{\"type\":\"" + options.state + "\"}";
        } else {
            message = MoveJson(0.5, 0.5, options.state == "press");
        }
        if (!sender.Send(message)) return 1;
        std::cout << "已发送到 " << options.ip << ':' << options.port << "：" << message << '\n';
        return 0;
    }

    SetConsoleCtrlHandler(ConsoleHandler, TRUE);
    std::cout << "正在向 " << options.ip << ':' << options.port << " 发送 "
        << options.pattern << "，" << options.hz << " Hz，持续 " << options.duration << " 秒。\n";
    using Clock = std::chrono::steady_clock;
    const auto started = Clock::now();
    const auto finishAt = started + std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(options.duration));
    const auto interval = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(1.0 / options.hz));
    auto nextSend = started;
    bool success = true;
    uint64_t sent = 0;
    double lastX = 0.5;
    double lastY = 0.5;
    while (!stopping.load(std::memory_order_relaxed)) {
        const auto now = Clock::now();
        const double elapsed = std::chrono::duration<double>(now - started).count();
        if (elapsed >= options.duration) break;
        double x = 0.5;
        double y = 0.5;
        bool down = false;
        if (options.pattern == "circle") {
            constexpr double pi = 3.14159265358979323846;
            const double phase = elapsed * 2.0 * pi / 4.0;
            x += 0.35 * std::cos(phase);
            y += 0.35 * std::sin(phase);
            const double cycleTime = std::fmod(elapsed, 4.0);
            down = cycleTime >= 1.0 && cycleTime < 2.5;
        } else if (options.pattern == "grid") {
            const int point = static_cast<int>(elapsed) % 9;
            x = 0.1 + 0.4 * (point % 3);
            y = 0.1 + 0.4 * (point / 3);
        }
        if (!sender.Send(MoveJson(x, y, down))) {
            success = false;
            break;
        }
        lastX = x;
        lastY = y;
        ++sent;
        nextSend += interval;
        if (nextSend < now) nextSend = now + interval;
        // 低频时也分段等待，让 Ctrl+C 至多约 20 ms 后进入结束流程。
        const auto wakeAt = nextSend < finishAt ? nextSend : finishAt;
        while (!stopping.load(std::memory_order_relaxed) && Clock::now() < wakeAt) {
            const auto remaining = wakeAt - Clock::now();
            const auto slice = std::chrono::duration_cast<Clock::duration>(std::chrono::milliseconds(20));
            std::this_thread::sleep_for(remaining < slice ? remaining : slice);
        }
    }
    // 结束时间可能正好位于按下区间，先补发完整释放隐藏状态，再发送 hide。
    // 两条消息均可独立结束显示，避免只依赖最后一次正常轨迹采样释放。
    const bool cleared = sender.Send(MoveJson(lastX, lastY, false, false));
    if (!cleared) success = false;
    const bool hidden = sender.Send("{\"type\":\"hide\"}");
    if (!hidden) success = false;
    SetConsoleCtrlHandler(ConsoleHandler, FALSE);
    std::cout << "测试结束，已发送 " << sent << " 个位置状态。"
        << (hidden ? "已发送隐藏消息。\n" : "隐藏消息发送失败。\n");
    return success ? 0 : 1;
}
