#pragma once
#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

namespace qpointer::android {
struct ProcessResult {
    unsigned long exitCode = 1;
    bool cancelled = false, timedOut = false, truncated = false;
    std::string output;
    bool Ok() const { return exitCode == 0 && !cancelled && !timedOut; }
};
// 直接启动绝对路径；双层引用分别用于 Windows argv 和 Android shell。
std::wstring QuoteArgument(const std::wstring& value);
std::string ShellQuote(const std::string& value);
ProcessResult RunProcess(const std::filesystem::path& executable,
    const std::vector<std::wstring>& arguments, const std::atomic<bool>& cancel,
    unsigned long timeoutMs = 30000, const std::string& input = {}, bool ownTree = true,
    const std::filesystem::path& workingDirectory = {});
std::wstring ToWide(const std::string& value);
std::string ToUtf8(const std::wstring& value);
std::string Trim(std::string value);
}
