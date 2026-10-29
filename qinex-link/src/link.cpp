#include "link.h"
#include "app.h"
#include "performance.h"
#include <setupapi.h>
#include <devguid.h>
#include <initguid.h>
#include <winioctl.h>   // GUID_DEVINTERFACE_COMPORT
#include <thread>
#include <cstdio>
#include <cerrno>
#include <climits>
#include <cstdlib>

namespace {

constexpr int BASE_BAUD = 921600;
constexpr int LINK_BAUD = 3000000;

std::atomic<HANDLE> s_port{INVALID_HANDLE_VALUE};
std::mutex  s_txMx;
std::thread s_worker;
std::atomic<bool> s_stop{false};
std::wstring s_portName;

std::wstring toW(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

bool setBaud(int baud) {
    HANDLE port = s_port.load(std::memory_order_acquire);
    if (port == INVALID_HANDLE_VALUE) return false;
    DCB dcb{}; dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(port, &dcb)) return false;
    dcb.BaudRate = baud;
    dcb.ByteSize = 8; dcb.Parity = NOPARITY; dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fOutxCtsFlow = dcb.fOutxDsrFlow = FALSE;
    // DTR/RTS 都不置位:CH343 自动下载电路里"仅 RTS 有效"才会拉复位,双低是安全稳态。
    // 首次打开驱动瞬态仍可能复位板子一次(等心跳阶段能兜住)。
    dcb.fDtrControl = DTR_CONTROL_DISABLE;
    dcb.fRtsControl = RTS_CONTROL_DISABLE;
    return SetCommState(port, &dcb) != 0;
}

bool openPort() {
    std::wstring path = L"\\\\.\\" + s_portName;
    HANDLE port = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              OPEN_EXISTING, 0, nullptr);
    if (port == INVALID_HANDLE_VALUE) return false;
    s_port.store(port, std::memory_order_release);
    if (!setBaud(BASE_BAUD)) {
        s_port.store(INVALID_HANDLE_VALUE, std::memory_order_release);
        CloseHandle(port);
        return false;
    }
    // ⚠️ 读必须**非阻塞立即返回**(MAXDWORD + 双 0 = 只取缓冲区现有字节)。
    // 历史 bug:曾用 ReadTotalTimeoutConstant=50 的阻塞读——同步(非 overlapped)句柄上
    // Win32 一次只放行一个 I/O,读线程抱着 50ms 的 ReadFile 窗口时,发送线程的 WriteFile
    // (键鼠帧/心跳/PING)全被挡在门外;锁交接不公平,读线程返回后瞬间又发起下一窗,
    // 写侧可能连等 2~3 个窗口 → RTT 虚高到 100ms+,转发也跟着抖。
    // 非阻塞读 + 空转 Sleep(1) 轮询后,写路径永不被读串行化,RTT 回落到真实线上延迟。
    COMMTIMEOUTS to{};
    to.ReadIntervalTimeout        = MAXDWORD;
    to.ReadTotalTimeoutConstant   = 0;
    to.ReadTotalTimeoutMultiplier = 0;
    to.WriteTotalTimeoutConstant  = 100;
    SetCommTimeouts(port, &to);
    SetupComm(port, 1 << 16, 1 << 16);
    PurgeComm(port, PURGE_RXCLEAR | PURGE_TXCLEAR);
    return true;
}

void closePort() {
    std::lock_guard<std::mutex> lk(s_txMx);
    HANDLE port = s_port.exchange(INVALID_HANDLE_VALUE, std::memory_order_acq_rel);
    if (port != INVALID_HANDLE_VALUE) {
        CancelIoEx(port, nullptr);
        CloseHandle(port);
    }
}

void notifyUi() { if (g.hwnd) PostMessage(g.hwnd, MSG_LINKUPD, 0, 0); }

// ---- 板端文本行解析 ----

// 在 line 里找 " key=" 并取其后整数;找不到返回 fallback
int kvInt(const std::string& line, const char* key, int fallback) {
    std::string pat = std::string(" ") + key + "=";
    size_t p = line.find(pat);
    if (p == std::string::npos) return fallback;
    const char* first = line.c_str() + p + pat.size();
    char* end = nullptr;
    errno = 0;
    long long value = strtoll(first, &end, 10);
    if (end == first || errno == ERANGE || value < INT_MIN || value > INT_MAX) return fallback;
    return static_cast<int>(value);
}

int kvIntRange(const std::string& line, const char* key, int fallback, int minValue, int maxValue) {
    int value = kvInt(line, key, fallback);
    return value >= minValue && value <= maxValue ? value : fallback;
}

void parseLine(const std::string& line) {
    if (line.rfind("STATUS: Mode ", 0) == 0) {
        char mode = line[13];
        g.boardMode = mode;
        g.lastStatusMs = GetTickCount64();
        size_t up = line.find(" usb=");
        g.usbMounted = (up != std::string::npos && line.compare(up + 5, 7, "mounted") == 0);
        // 启动验证:STATUS 行格式 + act 字段 = P4 身份指纹(与 APP 的 /api/appauth 同定位:
        //   防"没 P4 的人白嫖工具",不防自建假串口设备伪造心跳——离线场景没有密码学可依)。
        //   act 来自板子 NVS 的出厂离线激活 token 校验结果。会话内见过一次 act=1 即解锁。
        int act = kvInt(line, "act", -1);
        int prevAct = g.boardAct.exchange(act);
        if (act == 1 && !g.verified) {
            g.verified = true;
            g.log(L"启动验证通过:P4 已授权,功能解锁");
        } else if (act == 0 && prevAct != 0) {
            g.log(L"P4 未授权，请在板载页“系统 → 设备授权”完成授权");
        }
        g.boardUkm  = kvInt(line, "ukm", 0);
        g.boardFps  = kvInt(line, "fps", 0);
        g.boardDps  = kvInt(line, "dps", 0);
        g.boardGmaxUs = kvInt(line, "gmax", 0);
        // dmax 仅是诊断量，不参与转发。旧固件在未 armed 的异常完成回调上可能吐出
        // uint32 回绕值；在 PC 端丢弃 >1s 的非物理样本，USB 固件发送路径保持原样。
        g.boardDmaxUs = kvIntRange(line, "dmax", 0, 0, 1000000);
        g.boardUq      = kvInt(line, "uq", 0);
        g.boardUqMax   = kvInt(line, "uqmax", 0);
        g.boardUdrop   = kvInt(line, "udrop", 0);
        int ue = kvInt(line, "uerr", 0);
        g.boardUerr = ue;
        if (g.uerrBase < 0) g.uerrBase = ue;             // 会话首采 = 基线
        int sess = ue - g.uerrBase.load();
        g.uerrSession = sess < 0 ? 0 : sess;             // 板子重启会使 ue 变小,夹到 0
        g.boardBaud = kvInt(line, "baud", g.boardBaud.load());
        notifyUi();
        return;
    }
    if (line.rfind("PONG ", 0) == 0) {
        uint64_t ts = _strtoui64(line.c_str() + 5, nullptr, 10);
        uint64_t now = nowUs();
        if (ts && now > ts && now - ts < 5000000) {
            g.rttX10 = (int)((now - ts) / 100);   // ms×10
        }
        return;
    }
    // 其余可读行(KMLINK 回执 / INFO / 告警)进日志条
    if (line.rfind("KMLINK", 0) == 0 || line.rfind("KMV2", 0) == 0 ||
        line.rfind("INFO:", 0) == 0 ||
        line.rfind("SELFTEST", 0) == 0 || line.rfind("QCFG", 0) == 0 ||
        line.rfind("W (", 0) == 0 || line.rfind("E (", 0) == 0) {
        g.log(toW(line));
    }
}

// 读串口并按行切分;返回是否读到东西。expect 非空时,命中该前缀的行会写入 *hit。
std::string s_acc;
bool pump(const char* expect = nullptr, std::string* hit = nullptr) {
    HANDLE port = s_port.load(std::memory_order_acquire);
    if (port == INVALID_HANDLE_VALUE) return false;
    uint8_t buf[4096];
    DWORD got = 0;
    if (!ReadFile(port, buf, sizeof(buf), &got, nullptr) || got == 0) return false;
    for (DWORD i = 0; i < got; i++) {
        char c = (char)buf[i];
        if (c == '\n') {
            if (!s_acc.empty() && s_acc.back() == '\r') s_acc.pop_back();
            if (!s_acc.empty()) {
                if (expect && hit && s_acc.rfind(expect, 0) == 0) *hit = s_acc;
                parseLine(s_acc);
            }
            s_acc.clear();
        } else if (s_acc.size() < 512) {
            s_acc += c;
        } else {
            s_acc.clear();   // 高波特率乱码兜底
        }
    }
    return true;
}

// ⚠️ 必须整帧写出,绝不能吐半个帧。
//   串口写超时(COMMTIMEOUTS.WriteTotalTimeoutConstant)的行为是:WriteFile 返回 **TRUE**
//   但只写了一部分字节。原来这里发现 wr != len 就直接返回 false —— 而调用方(sender 的三个
//   发帧函数)从来不看返回值,于是半个帧已经上线、剩下半个被丢弃 → 板端解析流**永久失步**:
//   协议是裸二进制无分隔符,dx/dy 的数据字节完全可能等于 0xF6/0xF7,失步后解析器就在数据
//   字节上认帧头,一路吐垃圾帧;8 位 XOR 只有约 1/256 的拦截漏网率 → 板子既 uerr 狂涨
//   (面板"校验错误"一直红),又时不时把垃圾键码喂进映射 → 用户看到的"一直乱触发按键"。
//   典型诱因:KMLINK 3M 协商失败回落 921600(≈92KB/s),却还跑 8000Hz 档
//   (旧 F6 位移帧 64KB/s；KMV2 的 F8 为 72KB/s)。
bool writeRaw(const void* data, int len) {
    std::lock_guard<std::mutex> lk(s_txMx);
    HANDLE port = s_port.load(std::memory_order_acquire);
    if (port == INVALID_HANDLE_VALUE) return false;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    int sent = 0;
    for (int tries = 0; tries < 4 && sent < len; tries++) {
        DWORD wr = 0;
        if (!WriteFile(port, p + sent, (DWORD)(len - sent), &wr, nullptr)) break;
        sent += (int)wr;      // 超时只写了一部分 → 下一轮把剩余补完,宁可多阻塞也不撕帧
    }
    if (sent != len) { g.txTrunc++; return false; }   // 补不完(串口真堵死/掉线):计数上报
    return true;
}

// 鼠标协议能力协商。新固件回 KMV2 OK 后使用 F8(gap_125us)；老固件不认识该文本行，
// 350ms 超时后继续使用 F6，保证 Link 与现场存量固件交叉升级也不会失去鼠标。
bool enableMouseV2() {
    g.mouseV2 = false;
    s_acc.clear();
    static const char cmd[] = "KMV2\n";
    if (!writeRaw(cmd, (int)sizeof(cmd) - 1)) return false;
    ULONGLONG deadline = GetTickCount64() + 350;
    std::string hit;
    while (GetTickCount64() < deadline && !s_stop) {
        if (!pump("KMV2", &hit)) Sleep(1);
        if (!hit.empty()) {
            bool ok = hit.find("OK") != std::string::npos;
            g.mouseV2 = ok;
            if (ok) g.log(L"板端已启用鼠标时间颗粒重放(KMV2)");
            return ok;
        }
    }
    g.log(L"板端不支持 KMV2，兼容使用旧鼠标协议");
    return false;
}

// KMLINK 3M 协商(验证式、可回退):发命令 → 等 OK → 切 3M → 必须在 3M 上收到干净 STATUS 才确认;
// 收不到就退回 921600(防"板子 3M / PC 921600"永久错位刷 uerr)。CH343 3M 误码高,验证是必须的。
bool negotiate() {
    s_acc.clear();
    char cmd[32];
    int n = snprintf(cmd, sizeof(cmd), "KMLINK %d\n", LINK_BAUD);
    if (!writeRaw(cmd, n)) return false;

    ULONGLONG deadline = GetTickCount64() + 1500;
    std::string hit;
    bool gotOk = false;
    while (GetTickCount64() < deadline && !s_stop) {
        if (!pump("KMLINK", &hit)) Sleep(1);   // 非阻塞读:空轮询让出 CPU
        if (!hit.empty()) { gotOk = (hit.find("OK") != std::string::npos); break; }
    }
    if (!gotOk) return false;

    // 板子回执发完再切;PC 切 3M 后清缓冲,等一条干净 STATUS 验证链路真通
    Sleep(200);
    if (!setBaud(LINK_BAUD)) return false;
    PurgeComm(s_port.load(std::memory_order_acquire), PURGE_RXCLEAR);
    s_acc.clear();
    ULONGLONG vdl = GetTickCount64() + 6500;   // STATUS 每 5s 一条,给 6.5s 裕量
    ULONGLONG nextKeepalive = 0;
    while (GetTickCount64() < vdl && !s_stop) {
        // 板端高波特率空闲 1.5s 就会回落 921600。等待 5s STATUS 期间若只读不写,
        // 两端会在验证完成前再次错开波特率;持续发合法空鼠标帧把 3M 链路保持住。
        ULONGLONG now = GetTickCount64();
        if (now >= nextKeepalive) {
            const uint8_t heartbeat[8] = {0xF6, 0, 0, 0, 0, 0, 0, 0xF6};
            if (!writeRaw(heartbeat, sizeof(heartbeat))) break;
            nextKeepalive = now + 100;
        }
        std::string st;
        if (!pump("STATUS", &st)) Sleep(1);
        if (!st.empty()) { g.log(L"链路已提速 3 Mbps(已验证)"); return true; }
    }
    // 3M 上没收到干净 STATUS → 误码/未真正切换,退回 921600 自愈
    setBaud(BASE_BAUD);
    PurgeComm(s_port.load(std::memory_order_acquire), PURGE_RXCLEAR);
    s_acc.clear();
    g.log(L"3 Mbps 验证失败,退回 921600");
    for (int i = 0; i < 17 && !s_stop; i++) Sleep(100);   // 断开请求可立即打断回落等待
    return false;
}

void worker() {
    auto perf = performance::EnableCurrentThreadHighPerformance(THREAD_PRIORITY_HIGHEST);
    if (!perf.priority || !perf.executionSpeed) {
        g.log(L"串口链路后台调度保护部分失败(优先级=" +
              std::to_wstring(perf.priorityError) + L",QoS=" +
              std::to_wstring(perf.powerError) + L"),请查看后台性能说明");
    }
    int negoRetry = 0;
    while (!s_stop) {
        switch (g.state.load()) {
        case LS_OPENING:
            if (openPort()) {
                g.log(L"串口已打开 " + s_portName + L",等待板子心跳(≤8s,STATUS 每 5s 一条)");
                g.boardMode = '?';
                g.boardAct = -1;
                g.rttX10 = -1;
                g.lastStatusMs = 0;
                g.uerrBase = -1;          // 重置会话校验错误基线
                g.uerrSession = 0;
                g.mouseV2 = false;
                g.state = LS_WAITSTATUS;
                notifyUi();
            } else {
                g.log(L"打开 " + s_portName + L" 失败,2s 后重试(被占用?接线?)");
                for (int i = 0; i < 20 && !s_stop; i++) Sleep(100);
            }
            break;

        case LS_WAITSTATUS: {
            ULONGLONG t0 = GetTickCount64();
            bool warned = false;
            while (!s_stop && g.state == LS_WAITSTATUS) {
                if (!pump()) Sleep(1);
                if (g.boardMode != '?') {
                    if (g.boardMode == 'C') {
                        // 默认稳在 921600(足够真 8K);仅用户显式开启 link3m 才尝试提速
                        if (g.link3m) { negoRetry = 0; g.state = LS_NEGOTIATE; }
                        else {
                            (void)enableMouseV2();
                            g.state = LS_CONNECTED;
                            g.connectStartMs = GetTickCount64();
                            g.log(L"已连接:上位机直驱 @ 921600(足够 8K)");
                        }
                    } else {
                        g.state = LS_CONNECTED;   // Mode A:挂着读心跳,UI 红字提示
                        g.connectStartMs = GetTickCount64();
                        g.log(L"P4 在单机模式，请开启板载页“系统 → 上位机直驱模式”");
                        g.log(L"接线：H3 拨至 DEVICE，手机插 USB-A，再重启 P4");
                    }
                    notifyUi();
                    break;
                }
                if (!warned && GetTickCount64() - t0 > 8000) {
                    warned = true;
                    g.log(L"8s 未收到 STATUS——检查:串口对不对?板子有没有上电?");
                    notifyUi();
                }
            }
            break;
        }

        case LS_NEGOTIATE:
            if (negotiate()) {
                (void)enableMouseV2();
                g.state = LS_CONNECTED;
                g.connectStartMs = GetTickCount64();
                g.log(L"已连接:上位机直驱,链路 3 Mbps");
            } else if (++negoRetry >= 2) {
                (void)enableMouseV2();
                g.state = LS_CONNECTED;   // 提速失败:稳在 921600(足够 8K,无档位限制)
                g.connectStartMs = GetTickCount64();
                g.log(L"3 Mbps 提速未成,稳在 921600(足够 8K,不影响回报率)");
            }
            notifyUi();
            break;

        case LS_CONNECTED: {
            if (!pump()) Sleep(1);   // 非阻塞读:没数据就让 1ms(timeBeginPeriod(1) 已生效,PONG 最多晚 ~1ms)
            // 看门狗:板子 STATUS 每 5s 一条,>12s 没有 = 链路断
            ULONGLONG last = g.lastStatusMs.load();
            if (last && GetTickCount64() - last > 12000) {
                g.log(L"12s 无心跳,链路中断 → 重连");
                closePort();
                g.state = LS_RECONNECT;
                notifyUi();
            }
            break;
        }

        case LS_RECONNECT:
            for (int i = 0; i < 15 && !s_stop; i++) Sleep(100);
            if (!s_stop) g.state = LS_OPENING;
            break;

        default:
            Sleep(100);
            break;
        }
    }
    closePort();
    g.state = LS_IDLE;
    g.boardMode = '?';
    g.mouseV2 = false;
    notifyUi();
}

}  // namespace

namespace link {

std::vector<PortInfo> enumPorts() {
    std::vector<PortInfo> out;
    HDEVINFO di = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_COMPORT, nullptr, nullptr,
                                       DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (di == INVALID_HANDLE_VALUE) return out;
    SP_DEVINFO_DATA dd{}; dd.cbSize = sizeof(dd);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(di, i, &dd); i++) {
        wchar_t fr[256] = L"";
        SetupDiGetDeviceRegistryPropertyW(di, &dd, SPDRP_FRIENDLYNAME, nullptr,
                                          (PBYTE)fr, sizeof(fr), nullptr);
        std::wstring f = fr;
        size_t lp = f.rfind(L"(COM");
        if (lp == std::wstring::npos) continue;
        size_t rp = f.find(L')', lp);
        if (rp == std::wstring::npos) continue;
        out.push_back({f.substr(lp + 1, rp - lp - 1), f});
    }
    SetupDiDestroyDeviceInfoList(di);
    return out;
}

void connect(const std::wstring& port) {
    disconnect();
    s_portName = port;
    s_stop = false;
    g.state = LS_OPENING;
    s_worker = std::thread(worker);
}

void disconnect() {
    s_stop = true;
    // disconnect 从 UI 线程进入。旧实现直接 join；若工作线程正卡在同步串口 I/O，
    // 窗口消息循环会一起永久等待。先取消该句柄上的全部 I/O，并专门取消工作线程的
    // 同步调用，让 worker 能观察 s_stop、执行 closePort 后快速退出。
    HANDLE port = s_port.load(std::memory_order_acquire);
    if (port != INVALID_HANDLE_VALUE) CancelIoEx(port, nullptr);
    if (s_worker.joinable()) CancelSynchronousIo(s_worker.native_handle());
    if (s_worker.joinable()) s_worker.join();
    s_stop = false;
    g.state = LS_IDLE;
    g.boardMode = '?';
    g.boardAct = -1;         // verified 不清:会话内验过一次即长效
    g.rttX10 = -1;
    g.uerrBase = -1;
    g.uerrSession = 0;
}

void shutdown() { disconnect(); }

bool write(const uint8_t* data, int len) { return writeRaw(data, len); }

// 转发前置:已连接 + Mode C + 当前板子已激活(boardAct 看当前这块板,不认会话缓存的 verified——
// 换上一块未激活的板子必须重新拦住)
bool txReady() { return g.state == LS_CONNECTED && g.boardMode == 'C' && g.boardAct == 1; }

}  // namespace link
