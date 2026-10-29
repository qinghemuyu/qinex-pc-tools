#pragma once
#include <atomic>
#include <filesystem>
#include <string>

namespace qpointer::android {
// 离线验证使用线程局部目录，避免测试图片、签名和备份进入真实用户资料。
class ScopedDataDirectory {
    std::filesystem::path previous;
public:
    explicit ScopedDataDirectory(const std::filesystem::path& path);
    ~ScopedDataDirectory();
};
std::filesystem::path DataDirectory();
std::filesystem::path RuntimeDirectory();
std::string FileHash(const std::filesystem::path& path);
std::string StringHash(const std::string& content);
// 依赖以摘要为版本；只释放所需分组，校验已存在文件并原子替换损坏项。
std::filesystem::path EnsureRuntime(const std::string& group, const std::atomic<bool>& cancel);
void AtomicWrite(const std::filesystem::path& path, const std::string& content);
}
