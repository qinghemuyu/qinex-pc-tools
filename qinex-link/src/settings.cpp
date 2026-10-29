#include "settings.h"
#include "app.h"
#include <cstdio>
#include <string>

namespace {

std::wstring cfgPath() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring p = exe;
    size_t s = p.find_last_of(L"\\/");
    return p.substr(0, s + 1) + L"link.json";
}

// 极简 JSON 取值(配置结构自己写自己读,不需要完整解析器)
int jInt(const std::string& j, const char* key, int fallback) {
    std::string pat = std::string("\"") + key + "\":";
    size_t p = j.find(pat);
    if (p == std::string::npos) return fallback;
    return atoi(j.c_str() + p + pat.size());
}
std::string jStr(const std::string& j, const char* key) {
    std::string pat = std::string("\"") + key + "\":\"";
    size_t p = j.find(pat);
    if (p == std::string::npos) return "";
    size_t b = p + pat.size();
    size_t e = j.find('"', b);
    return (e == std::string::npos) ? "" : j.substr(b, e - b);
}

}  // namespace

namespace settings {

void load() {
    FILE* f = _wfopen(cfgPath().c_str(), L"rb");
    if (!f) return;
    std::string j;
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) j.append(buf, n);
    fclose(f);

    std::string port = jStr(j, "port");
    g.portName.assign(port.begin(), port.end());   // COM 名纯 ASCII
    g.tier      = jInt(j, "tier", 1000);
    g.exclusive = jInt(j, "exclusive", 0) != 0;
    g.stopOnRelease = jInt(j, "stopOnRelease", 0) != 0;
    g.hotkeyVk  = (UINT)jInt(j, "hotkey", VK_SCROLL);
    g.link3m    = jInt(j, "link3m", 0) != 0;
    g.autostart = jInt(j, "autostart", 0) != 0;
    g.taskbarWidget = jInt(j, "taskbarWidget", 1) != 0;
    g.winX      = jInt(j, "x", -100000);
    g.winY      = jInt(j, "y", -100000);
    g.netEnable = jInt(j, "netEnable", 0) != 0;
    g.netPort   = jInt(j, "netPort", 6234);
    g.netLan    = jInt(j, "netLan", 0) != 0;   // 无 UI 开关:默认只听 127.0.0.1,确需局域网才手改
    g.logSave   = jInt(j, "logSave", 0) != 0;
    g.netMac    = (unsigned)strtoul(([&]{ std::string s = jStr(j, "netMac"); return s.empty() ? "0" : s; }()).c_str(), nullptr, 16);
}

void save() {
    RECT r{};
    if (g.hwnd && !IsIconic(g.hwnd)) { GetWindowRect(g.hwnd, &r); g.winX = r.left; g.winY = r.top; }
    std::string port;
    for (wchar_t c : g.portName) port += (char)(c & 0x7F);   // COM 名纯 ASCII
    FILE* f = _wfopen(cfgPath().c_str(), L"wb");
    if (!f) return;
    fprintf(f,
        "{\n"
        "  \"port\":\"%s\",\n"
        "  \"tier\":%d,\n"
        "  \"exclusive\":%d,\n"
        "  \"stopOnRelease\":%d,\n"
        "  \"hotkey\":%u,\n"
        "  \"link3m\":%d,\n"
        "  \"autostart\":%d,\n"
        "  \"taskbarWidget\":%d,\n"
        "  \"netEnable\":%d,\n"
        "  \"netPort\":%d,\n"
        "  \"netLan\":%d,\n"
        "  \"netMac\":\"%08X\",\n"
        "  \"logSave\":%d,\n"
        "  \"x\":%d,\n"
        "  \"y\":%d\n"
        "}\n",
        port.c_str(), g.tier, g.exclusive ? 1 : 0, g.stopOnRelease ? 1 : 0,
        g.hotkeyVk.load(),
        g.link3m ? 1 : 0, g.autostart ? 1 : 0, g.taskbarWidget ? 1 : 0,
        g.netEnable ? 1 : 0, g.netPort, g.netLan ? 1 : 0, g.netMac,
        g.logSave ? 1 : 0, g.winX, g.winY);
    fclose(f);
}

void applyAutostart() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    if (g.autostart) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring v = L"\"" + std::wstring(exe) + L"\"";
        RegSetValueExW(key, L"QInEX-Link", 0, REG_SZ,
                       (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, L"QInEX-Link");
    }
    RegCloseKey(key);
}

}  // namespace settings
