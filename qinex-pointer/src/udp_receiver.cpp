#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <array>
#include <atomic>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>

#include "udp_receiver.h"

namespace qpointer {
namespace {
constexpr size_t kMaxDatagramBytes = 4096;
constexpr size_t kMaxQueuedDatagrams = 2048;

std::string SocketError(const char* operation, int code) {
    return std::string(operation) + "（WinSock 错误 " + std::to_string(code) + "）";
}
}  // namespace

struct UdpReceiver::Impl {
    SOCKET socket = INVALID_SOCKET;
    bool winsockStarted = false;
    bool filterSource = false;
    in_addr acceptedAddress{};
    std::atomic<bool> stopping{true};
    std::atomic<uint64_t> dropped{0};
    std::atomic<HWND> notifyWindow{nullptr};
    std::atomic<UINT> notifyMessage{0};
    std::atomic<bool> notificationPending{false};
    std::thread worker;
    std::mutex queueMutex;
    std::deque<Datagram> queue;

    void Receive() {
        // 多留一个字节，让长度恰好 4097 的报文也能可靠地整体丢弃。
        std::array<char, kMaxDatagramBytes + 1> buffer{};
        while (!stopping.load(std::memory_order_relaxed)) {
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(socket, &readable);
            timeval timeout{0, 50000};
            const int ready = select(0, &readable, nullptr, nullptr, &timeout);
            if (stopping.load(std::memory_order_relaxed)) break;
            if (ready == 0) continue;
            if (ready == SOCKET_ERROR) {
                // 非阻塞 UDP 的临时网络错误不终止监听，也不进入忙循环。
                Sleep(20);
                continue;
            }

            sockaddr_in from{};
            int fromLength = sizeof(from);
            const int count = recvfrom(socket, buffer.data(), static_cast<int>(buffer.size()),
                0, reinterpret_cast<sockaddr*>(&from), &fromLength);
            if (count == SOCKET_ERROR) {
                if (WSAGetLastError() == WSAEMSGSIZE) {
                    dropped.fetch_add(1, std::memory_order_relaxed);
                }
                continue;
            }
            if (static_cast<size_t>(count) > kMaxDatagramBytes) {
                dropped.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            if (filterSource && from.sin_addr.s_addr != acceptedAddress.s_addr) continue;

            char ip[INET_ADDRSTRLEN]{};
            if (InetNtopA(AF_INET, &from.sin_addr, ip, sizeof(ip)) == nullptr) continue;
            Datagram message{std::string(buffer.data(), static_cast<size_t>(count)),
                ip, GetTickCount64()};
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                if (queue.size() >= kMaxQueuedDatagrams) {
                    queue.pop_front();
                    dropped.fetch_add(1, std::memory_order_relaxed);
                }
                queue.push_back(std::move(message));
            }
            HWND target = notifyWindow.load(std::memory_order_acquire);
            const UINT messageId = notifyMessage.load(std::memory_order_relaxed);
            if (target && messageId && !notificationPending.exchange(true, std::memory_order_acq_rel) &&
                !PostMessageW(target, messageId, 0, 0)) {
                notificationPending.store(false, std::memory_order_release);
            }
        }
    }
};

UdpReceiver::UdpReceiver() : impl(std::make_unique<Impl>()) {}

UdpReceiver::~UdpReceiver() {
    Stop();
}

bool UdpReceiver::Start(uint16_t port, const std::string& sourceIp, std::string& error) {
    Stop();
    error.clear();
    impl->dropped.store(0, std::memory_order_relaxed);
    impl->notificationPending.store(false, std::memory_order_release);
    if (port == 0) {
        error = "监听端口必须在 1～65535 范围内。";
        return false;
    }

    WSADATA data{};
    const int startup = WSAStartup(MAKEWORD(2, 2), &data);
    if (startup != 0) {
        error = SocketError("初始化网络失败", startup);
        return false;
    }
    impl->winsockStarted = true;
    impl->filterSource = !sourceIp.empty();
    if (impl->filterSource && InetPtonA(AF_INET, sourceIp.c_str(), &impl->acceptedAddress) != 1) {
        error = "来源 IP 必须是有效的 IPv4 地址，例如 127.0.0.1。";
        Stop();
        return false;
    }

    impl->socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (impl->socket == INVALID_SOCKET) {
        error = SocketError("创建 UDP 监听失败", WSAGetLastError());
        Stop();
        return false;
    }
    // 独占绑定，避免同端口的另一实例悄悄分走光标数据。
    const BOOL exclusive = TRUE;
    if (setsockopt(impl->socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
        reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) == SOCKET_ERROR) {
        error = SocketError("设置 UDP 独占监听失败", WSAGetLastError());
        Stop();
        return false;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (bind(impl->socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        const int code = WSAGetLastError();
        error = "无法监听 UDP " + std::to_string(port) + "：";
        error += (code == WSAEADDRINUSE || code == WSAEACCES)
            ? "端口已被占用或系统限制绑定，请关闭占用程序或检查端口。"
            : "绑定失败，请检查网络配置。";
        error += "（WinSock 错误 " + std::to_string(code) + "）";
        Stop();
        return false;
    }
    u_long nonblocking = 1;
    if (ioctlsocket(impl->socket, FIONBIO, &nonblocking) == SOCKET_ERROR) {
        error = SocketError("设置 UDP 非阻塞接收失败", WSAGetLastError());
        Stop();
        return false;
    }

    impl->stopping.store(false, std::memory_order_relaxed);
    try {
        impl->worker = std::thread([this] { impl->Receive(); });
    } catch (const std::exception&) {
        error = "创建 UDP 接收线程失败。";
        Stop();
        return false;
    }
    return true;
}

void UdpReceiver::Stop() {
    impl->stopping.store(true, std::memory_order_relaxed);
    if (impl->worker.joinable()) impl->worker.join();
    if (impl->socket != INVALID_SOCKET) {
        closesocket(impl->socket);
        impl->socket = INVALID_SOCKET;
    }
    if (impl->winsockStarted) {
        WSACleanup();
        impl->winsockStarted = false;
    }
    std::lock_guard<std::mutex> lock(impl->queueMutex);
    impl->queue.clear();
    impl->notificationPending.store(false, std::memory_order_release);
}

std::vector<UdpReceiver::Datagram> UdpReceiver::Drain() {
    // 在锁外分配返回数组，避免批量移动期间持续占用接收锁。
    std::deque<Datagram> pending;
    {
        std::lock_guard<std::mutex> lock(impl->queueMutex);
        pending.swap(impl->queue);
    }
    std::vector<Datagram> messages;
    messages.reserve(pending.size());
    for (auto& message : pending) messages.push_back(std::move(message));
    return messages;
}

void UdpReceiver::SetNotification(HWND window, UINT message) {
    impl->notifyMessage.store(message, std::memory_order_relaxed);
    impl->notifyWindow.store(window, std::memory_order_release);
    impl->notificationPending.store(false, std::memory_order_release);
}

void UdpReceiver::AcknowledgeNotification() {
    impl->notificationPending.store(false, std::memory_order_release);
    bool hasPending = false;
    {
        std::lock_guard<std::mutex> lock(impl->queueMutex);
        hasPending = !impl->queue.empty();
    }
    HWND target = impl->notifyWindow.load(std::memory_order_acquire);
    const UINT messageId = impl->notifyMessage.load(std::memory_order_relaxed);
    if (hasPending && target && messageId &&
        !impl->notificationPending.exchange(true, std::memory_order_acq_rel) &&
        !PostMessageW(target, messageId, 0, 0)) {
        impl->notificationPending.store(false, std::memory_order_release);
    }
}

uint64_t UdpReceiver::Dropped() const {
    return impl->dropped.load(std::memory_order_relaxed);
}

}  // namespace qpointer
