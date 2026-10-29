#pragma once
#include "tool_process.h"
#include <functional>

namespace qpointer::android {
struct Device { std::string serial, state, model; };
std::vector<Device> ParseDevices(const std::string& output);
std::string DescribeDevices(const std::vector<Device>& devices);
bool ValidEndpoint(const std::string& endpoint);
using AdbExecutor = std::function<ProcessResult(const std::vector<std::wstring>&, const std::atomic<bool>&, unsigned long, const std::string&)>;
class AdbClient {
    std::filesystem::path executable;
    int port = 5037;
    bool owned = false;
    unsigned long ownedPid = 0;
    void* ownedProcess = nullptr;
    AdbExecutor executor;
public:
    explicit AdbClient(AdbExecutor run = {}) : executor(std::move(run)) {}
    ~AdbClient() { Shutdown(); }
    // 所有调用均在工具工作线程。服务协议不兼容时另起端口，不终止第三方服务。
    void Initialize(const std::atomic<bool>& cancel);
    // 仅供诊断详情使用；与其它接口一样由工具工作线程调用。
    std::string ConnectionDescription() const;
    ProcessResult Run(const std::vector<std::wstring>& args, const std::atomic<bool>& cancel,
        unsigned long timeout = 30000, const std::string& input = {});
    ProcessResult Shell(const std::string& serial, const std::string& command,
        const std::atomic<bool>& cancel, bool root = false, unsigned long timeout = 30000);
    void Shutdown();
};
}
