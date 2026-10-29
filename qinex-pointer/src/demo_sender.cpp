#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <iomanip>
#include <locale>
#include <mutex>
#include <sstream>
#include <thread>

#include "demo_sender.h"

namespace qpointer {
namespace {

std::string MoveJson(double x, double y, bool down, bool show) {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::fixed << std::setprecision(5)
        << "{\"type\":\"move\",\"nx\":" << x << ",\"ny\":" << y
        << ",\"show\":" << (show ? "true" : "false")
        << ",\"down\":" << (down ? "true" : "false") << '}';
    return output.str();
}

}  // namespace

struct LoopbackDemoSender::Impl {
    std::atomic<bool> stopping{false};
    std::atomic<bool> active{false};
    std::thread worker;
    std::mutex waitMutex;
    std::condition_variable wake;
    SOCKET socket = INVALID_SOCKET;
    sockaddr_in destination{};
    bool winsockStarted = false;

    bool Send(const std::string& message) const {
        return socket != INVALID_SOCKET &&
            sendto(socket, message.data(), static_cast<int>(message.size()), 0,
                reinterpret_cast<const sockaddr*>(&destination), sizeof(destination)) ==
            static_cast<int>(message.size());
    }

    void Run(std::uint32_t durationMs) {
        using Clock = std::chrono::steady_clock;
        constexpr double pi = 3.14159265358979323846;
        constexpr auto interval = std::chrono::microseconds(16667);
        const auto started = Clock::now();
        const auto finishAt = started + std::chrono::milliseconds(durationMs);
        auto nextSend = started;
        double lastX = .5, lastY = .5;
        while (!stopping.load(std::memory_order_relaxed)) {
            const auto now = Clock::now();
            if (now >= finishAt) break;
            const double elapsed = std::chrono::duration<double>(now - started).count();
            const double phase = elapsed * 2.0 * pi / 4.0;
            lastX = .5 + .35 * std::cos(phase);
            lastY = .5 + .35 * std::sin(phase);
            const double cycleTime = std::fmod(elapsed, 4.0);
            if (!Send(MoveJson(lastX, lastY, cycleTime >= 1.0 && cycleTime < 2.5, true))) break;

            nextSend += interval;
            if (nextSend < now) nextSend = now + interval;
            std::unique_lock<std::mutex> lock(waitMutex);
            wake.wait_until(lock, std::min(nextSend, finishAt), [&] {
                return stopping.load(std::memory_order_relaxed);
            });
        }
        // 无论正常结束还是主动停止，都先释放触点再隐藏，避免演示残留按下状态。
        Send(MoveJson(lastX, lastY, false, false));
        Send("{\"type\":\"hide\"}");
        active.store(false, std::memory_order_release);
    }
};

LoopbackDemoSender::LoopbackDemoSender() : impl(std::make_unique<Impl>()) {}
LoopbackDemoSender::~LoopbackDemoSender() { Stop(); }

bool LoopbackDemoSender::Start(std::uint16_t port, std::uint32_t durationMs, std::string& error) {
    Stop();
    error.clear();
    if (!port || !durationMs) {
        error = "本机演示参数无效。";
        return false;
    }
    WSADATA data{};
    const int startupError = WSAStartup(MAKEWORD(2, 2), &data);
    if (startupError != 0) {
        error = "本机演示网络初始化失败：" + std::to_string(startupError);
        return false;
    }
    impl->winsockStarted = true;
    impl->socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (impl->socket == INVALID_SOCKET) {
        error = "本机演示 UDP 创建失败：" + std::to_string(WSAGetLastError());
        Stop();
        return false;
    }
    impl->destination = {};
    impl->destination.sin_family = AF_INET;
    impl->destination.sin_port = htons(port);
    impl->destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    impl->stopping.store(false, std::memory_order_relaxed);
    impl->active.store(true, std::memory_order_release);
    try {
        impl->worker = std::thread([state = impl.get(), durationMs] { state->Run(durationMs); });
    } catch (...) {
        impl->active.store(false, std::memory_order_release);
        error = "本机演示线程启动失败。";
        Stop();
        return false;
    }
    return true;
}

void LoopbackDemoSender::Stop() {
    impl->stopping.store(true, std::memory_order_relaxed);
    impl->wake.notify_all();
    if (impl->worker.joinable()) impl->worker.join();
    impl->active.store(false, std::memory_order_release);
    if (impl->socket != INVALID_SOCKET) {
        closesocket(impl->socket);
        impl->socket = INVALID_SOCKET;
    }
    if (impl->winsockStarted) {
        WSACleanup();
        impl->winsockStarted = false;
    }
}

bool LoopbackDemoSender::Active() const {
    return impl->active.load(std::memory_order_acquire);
}

}  // namespace qpointer
