#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace qpointer {

// Start/Stop 由同一管理线程调用；Drain/Dropped 可与后台接收并发。
// 接收模块只负责搬运数据，不解释协议，不把发送端临时源端口作为绑定依据。
class UdpReceiver {
public:
    struct Datagram {
        std::string payload;
        std::string sourceIp;
        uint64_t receivedMs = 0;
    };

    UdpReceiver();
    ~UdpReceiver();
    UdpReceiver(const UdpReceiver&) = delete;
    UdpReceiver& operator=(const UdpReceiver&) = delete;
    UdpReceiver(UdpReceiver&&) = delete;
    UdpReceiver& operator=(UdpReceiver&&) = delete;

    // 空 sourceIp 接受任意 IPv4 来源；端口范围为 1～65535。
    // 失败时停止接收并填充中文错误；重复 Start 会先关闭旧监听。
    bool Start(uint16_t port, const std::string& sourceIp, std::string& error);
    // 不从调用线程关闭正在接收的 socket；后台最多等待一次 50 ms 的 select。
    void Stop();
    std::vector<Datagram> Drain();
    // 可选的合并通知：队列从空闲转为有数据时只投递一条消息，主线程处理后必须确认。
    void SetNotification(HWND window, UINT message);
    void AcknowledgeNotification();
    // 统计本次 Start 后超长数据报与本地队列溢出；来源过滤不计为丢包。
    uint64_t Dropped() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}  // namespace qpointer
