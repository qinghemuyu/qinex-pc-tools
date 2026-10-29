#include <windows.h>
#include <shlobj.h>
#include <bcrypt.h>
#include <fstream>
#include <stdexcept>
#include <vector>
#include "embedded_runtime.h"
#include "runtime_manifest.h"

namespace qpointer::android {
namespace {
thread_local std::filesystem::path scopedDirectory;
class Hash {
    BCRYPT_ALG_HANDLE algorithm{}; BCRYPT_HASH_HANDLE hash{};
public:
    Hash() {
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
            BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0) throw std::runtime_error("无法初始化 SHA-256");
    }
    ~Hash() { if (hash) BCryptDestroyHash(hash); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); }
    void Add(const unsigned char* data, size_t size) {
        if (BCryptHashData(hash, const_cast<PUCHAR>(data), static_cast<ULONG>(size), 0) < 0) throw std::runtime_error("SHA-256 失败");
    }
    std::string Finish() {
        unsigned char digest[32]; if (BCryptFinishHash(hash, digest, sizeof(digest), 0) < 0) throw std::runtime_error("SHA-256 失败");
        const char* hex = "0123456789abcdef"; std::string value;
        for (auto byte : digest) { value += hex[byte >> 4]; value += hex[byte & 15]; } return value;
    }
};
}
std::filesystem::path DataDirectory() {
    if (!scopedDirectory.empty()) { std::filesystem::create_directories(scopedDirectory); return scopedDirectory; }
    PWSTR directory = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &directory))) throw std::runtime_error("无法获取当前用户目录");
    std::filesystem::path path(directory); CoTaskMemFree(directory);
    path /= L"QInEX/Pointer/android"; std::filesystem::create_directories(path); return path;
}
ScopedDataDirectory::ScopedDataDirectory(const std::filesystem::path& path) : previous(scopedDirectory) { if (!path.empty()) scopedDirectory = std::filesystem::absolute(path); }
ScopedDataDirectory::~ScopedDataDirectory() { scopedDirectory = previous; }
std::string FileHash(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary); if (!stream) return {};
    Hash hash; unsigned char buffer[65536];
    while (stream) { stream.read(reinterpret_cast<char*>(buffer), sizeof(buffer)); hash.Add(buffer, static_cast<size_t>(stream.gcount())); }
    if (!stream.eof()) return {}; return hash.Finish();
}
std::string StringHash(const std::string& content) {
    Hash hash; hash.Add(reinterpret_cast<const unsigned char*>(content.data()), content.size()); return hash.Finish();
}
void AtomicWrite(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    const auto temporary = std::filesystem::path(path.wstring() + L"." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetCurrentThreadId()) + L".tmp");
    { std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
      file.write(content.data(), static_cast<std::streamsize>(content.size())); file.flush();
      if (!file) throw std::runtime_error("无法写入备份或工具文件"); }
    HANDLE pending = CreateFileW(temporary.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_WRITE_THROUGH, nullptr);
    if (pending == INVALID_HANDLE_VALUE) throw std::runtime_error("无法同步备份或工具文件");
    const BOOL flushed = FlushFileBuffers(pending); CloseHandle(pending);
    if (!flushed) throw std::runtime_error("备份或工具文件同步失败");
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("无法提交备份或工具文件");
}
std::filesystem::path EnsureRuntime(const std::string& group, const std::atomic<bool>& cancel) {
    const auto root = RuntimeDirectory();
    std::filesystem::create_directories(root);
    const std::wstring mutexName = L"Local\\QInEXPointerRuntime-" + std::wstring(kRuntimeHash);
    HANDLE mutex = CreateMutexW(nullptr, FALSE, mutexName.c_str());
    if (!mutex) throw std::runtime_error("无法锁定工具缓存");
    struct Guard { HANDLE h; bool held = false; ~Guard() { if (held) ReleaseMutex(h); CloseHandle(h); } } guard{mutex};
    while (!cancel.load()) {
        const auto wait = WaitForSingleObject(mutex, 100);
        if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) { guard.held = true; break; }
        if (wait == WAIT_FAILED) throw std::runtime_error("工具缓存锁失败");
    }
    if (!guard.held) throw std::runtime_error("操作已取消");
    bool found = false;
    for (const auto& entry : kRuntimeFiles) {
        if (group != entry.group) continue; found = true;
        if (cancel.load()) throw std::runtime_error("操作已取消");
        const auto path = root / entry.path;
        if (FileHash(path) == entry.hash) continue;
        HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(entry.id), RT_RCDATA);
        HGLOBAL data = resource ? LoadResource(nullptr, resource) : nullptr;
        const auto size = resource ? SizeofResource(nullptr, resource) : 0;
        const auto bytes = data ? static_cast<const char*>(LockResource(data)) : nullptr;
        if (!bytes || !size) throw std::runtime_error("程序内嵌依赖缺失，请重新构建程序");
        AtomicWrite(path, std::string(bytes, size));
        if (FileHash(path) != entry.hash) throw std::runtime_error("工具文件摘要不匹配");
    }
    if (!found) throw std::runtime_error("内嵌依赖分组缺失");
    return root;
}
std::filesystem::path RuntimeDirectory() { return DataDirectory().parent_path() / L"runtime" / kRuntimeHash; }
}
