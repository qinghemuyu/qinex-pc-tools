#include <windows.h>
#include <algorithm>
#include <stdexcept>
#include "tool_process.h"

namespace qpointer::android {
namespace {
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
}
std::wstring ToWide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring r(n, 0); if (n) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), r.data(), n); return r;
}
std::string ToUtf8(const std::wstring& s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string r(n, 0); if (n) WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), r.data(), n, nullptr, nullptr); return r;
}
std::string Trim(std::string s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}
std::wstring QuoteArgument(const std::wstring& s) {
    if (s.find(L'\0') != std::wstring::npos) throw std::runtime_error("参数包含空字符");
    std::wstring r = L"\""; size_t slashes = 0;
    for (wchar_t c : s) {
        if (c == L'\\') { ++slashes; continue; }
        r.append(slashes * (c == L'"' ? 2 : 1), L'\\'); slashes = 0;
        if (c == L'"') r += L'\\'; r += c;
    }
    r.append(slashes * 2, L'\\'); return r + L'"';
}
std::string ShellQuote(const std::string& s) {
    if (s.find('\0') != std::string::npos) throw std::runtime_error("参数包含空字符");
    std::string r = "'";
    for (char c : s) r += c == '\'' ? "'\\''" : std::string(1, c);
    return r + "'";
}
ProcessResult RunProcess(const std::filesystem::path& exe, const std::vector<std::wstring>& args,
    const std::atomic<bool>& cancel, unsigned long timeout, const std::string& input, bool ownTree,
    const std::filesystem::path& workingDirectory) {
    ProcessResult result;
    if (cancel.load()) { result.cancelled = true; return result; }
    if (!exe.is_absolute()) { result.output = "工具路径必须为绝对路径"; return result; }
    Handle read, write, inRead, inWrite, job, process, thread;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    if (!CreatePipe(&read.value, &write.value, &sa, 0) || !CreatePipe(&inRead.value, &inWrite.value, &sa, 0)) {
        result.output = "无法创建工具管道"; return result;
    }
    SetHandleInformation(read.value, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(inWrite.value, HANDLE_FLAG_INHERIT, 0);
    std::wstring command = QuoteArgument(exe.wstring());
    for (const auto& arg : args) command += L" " + QuoteArgument(arg);
    STARTUPINFOEXW start{}; start.StartupInfo.cb = sizeof(start);
    start.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    start.StartupInfo.hStdOutput = start.StartupInfo.hStdError = write.value;
    start.StartupInfo.hStdInput = inRead.value;
    SIZE_T bytes = 0; InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<unsigned char> attributes(bytes);
    start.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (!InitializeProcThreadAttributeList(start.lpAttributeList, 1, 0, &bytes)) return result;
    HANDLE inherited[] = {write.value, inRead.value};
    if (!UpdateProcThreadAttribute(start.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inherited, sizeof(inherited), nullptr, nullptr)) {
        DeleteProcThreadAttributeList(start.lpAttributeList); return result;
    }
    if (ownTree) {
        job.value = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job.value || !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
            DeleteProcThreadAttributeList(start.lpAttributeList); return result;
        }
    }
    PROCESS_INFORMATION info{};
    const auto directory = (workingDirectory.empty() ? exe.parent_path() : workingDirectory).wstring();
    const BOOL created = CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED | BELOW_NORMAL_PRIORITY_CLASS | EXTENDED_STARTUPINFO_PRESENT,
        nullptr, directory.c_str(), &start.StartupInfo, &info);
    const DWORD error = GetLastError(); DeleteProcThreadAttributeList(start.lpAttributeList);
    if (!created) { result.output = "工具启动失败，Windows 错误 " + std::to_string(error); return result; }
    process.value = info.hProcess; thread.value = info.hThread;
    if (job.value && !AssignProcessToJobObject(job.value, process.value)) {
        TerminateProcess(process.value, 1); result.output = "无法管理工具子进程"; return result;
    }
    ResumeThread(thread.value);
    CloseHandle(write.value); write.value = nullptr;
    CloseHandle(inRead.value); inRead.value = nullptr;
    if (!input.empty()) { DWORD done = 0; WriteFile(inWrite.value, input.data(), static_cast<DWORD>(input.size()), &done, nullptr); }
    CloseHandle(inWrite.value); inWrite.value = nullptr;
    const auto started = GetTickCount64();
    const auto drain = [&] {
        DWORD available = 0;
        unsigned chunks = 0;
        while (++chunks <= 128 && PeekNamedPipe(read.value, nullptr, 0, nullptr, &available, nullptr) && available) {
            char buffer[4096]; DWORD got = 0;
            if (!ReadFile(read.value, buffer, std::min<DWORD>(available, sizeof(buffer)), &got, nullptr) || !got) break;
            const size_t remaining = 16 * 1024 * 1024 - result.output.size();
            result.output.append(buffer, std::min<size_t>(got, remaining));
            if (got > remaining) result.truncated = true;
        }
    };
    for (;;) {
        drain();
        if (WaitForSingleObject(process.value, 30) == WAIT_OBJECT_0) break;
        result.cancelled = cancel.load(); result.timedOut = GetTickCount64() - started >= timeout;
        if (result.cancelled || result.timedOut) {
            if (job.value) TerminateJobObject(job.value, 1); else TerminateProcess(process.value, 1);
            WaitForSingleObject(process.value, 2000); break;
        }
    }
    drain(); GetExitCodeProcess(process.value, &result.exitCode); return result;
}
}
