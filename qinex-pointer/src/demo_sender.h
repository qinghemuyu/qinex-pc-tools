#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace qpointer {

// 仅向本机发送 JSON v2 圆周轨迹，供产品内演示与整程序回归使用。
// Start/Stop 由 UI 线程调用；后台发送线程在 Stop 返回前必定退出。
class LoopbackDemoSender {
public:
    LoopbackDemoSender();
    ~LoopbackDemoSender();
    LoopbackDemoSender(const LoopbackDemoSender&) = delete;
    LoopbackDemoSender& operator=(const LoopbackDemoSender&) = delete;
    LoopbackDemoSender(LoopbackDemoSender&&) = delete;
    LoopbackDemoSender& operator=(LoopbackDemoSender&&) = delete;

    bool Start(std::uint16_t port, std::uint32_t durationMs, std::string& error);
    void Stop();
    bool Active() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}  // namespace qpointer
