#include "netserver.h"
#include "app.h"
#include "capture.h"
#include "performance.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <thread>
#include <atomic>
#include <random>
#pragma comment(lib, "ws2_32.lib")

namespace {

// ── kmboxNET 协议(与板端 kmnet.cpp / kmboxNet.h 一致,小端可直接 cast)──
constexpr uint32_t CMD_CONNECT      = 0xaf3c2828;
constexpr uint32_t CMD_MOUSE_MOVE   = 0xaede7345;
constexpr uint32_t CMD_MOUSE_LEFT   = 0x9823AE8D;
constexpr uint32_t CMD_MOUSE_MIDDLE = 0x97a3AE8D;
constexpr uint32_t CMD_MOUSE_RIGHT  = 0x238d8212;
constexpr uint32_t CMD_MOUSE_WHEEL  = 0xffeead38;
constexpr uint32_t CMD_MOUSE_AUTO   = 0xaede7346;
constexpr uint32_t CMD_KEYBOARD_ALL = 0x123c2c2f;
constexpr uint32_t CMD_BAZERMOVE    = 0xa238455a;
constexpr uint32_t CMD_MONITOR      = 0x27388020;
constexpr uint32_t CMD_MASK_MOUSE   = 0x23234343;
constexpr uint32_t CMD_UNMASK_ALL   = 0x23344343;

#pragma pack(push, 1)
struct KmHead  { uint32_t mac, rand, indexpts, cmd; };
struct KmMouse { int32_t button, x, y, wheel; int32_t point[10]; };
struct KmKbd   { int8_t ctrl, resvel; int8_t button[10]; };
struct StdMouse { uint8_t report_id, buttons; int16_t x, y, wheel; };
struct StdKbd   { uint8_t report_id, buttons; uint8_t data[10]; };
#pragma pack(pop)

std::thread s_rxThread, s_monThread;
std::atomic<bool> s_stop{false};
SOCKET s_sock = INVALID_SOCKET;
uint32_t s_key[4] = {0};

// monitor 目标
std::atomic<bool> s_monOn{false};
sockaddr_in s_monAddr{};
HANDLE s_monEvt = nullptr;

int16_t clamp16(int v) { return (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v)); }

// XXTEA 解密(与板端逆运算一致:n=32 u32=128B,6 轮,DELTA=0x9E3779B9)
void xxteaDecrypt(uint32_t* v, int n, const uint32_t* key) {
    const uint32_t DELTA = 0x9E3779B9u;
    uint32_t y, z, sum;
    unsigned p, e, rounds = 6;
    sum = rounds * DELTA;
    y = v[0];
    do {
        e = (sum >> 2) & 3;
        for (p = n - 1; p > 0; p--) {
            z = v[p - 1];
            y = v[p] -= (((z >> 5) ^ (y << 2)) + ((y >> 3) ^ (z << 4))) ^ ((sum ^ y) + (key[(p & 3) ^ e] ^ z));
        }
        z = v[n - 1];
        y = v[0] -= (((z >> 5) ^ (y << 2)) + ((y >> 3) ^ (z << 4))) ^ ((sum ^ y) + (key[(p & 3) ^ e] ^ z));
        sum -= DELTA;
    } while (--rounds);
}

void deriveKey() {
    // 与板端一致:key[0] = bswap32(boxMac),其余 0
    s_key[0] = _byteswap_ulong(g.netMac);
    s_key[1] = s_key[2] = s_key[3] = 0;
}

void handle(const uint8_t* buf, int len) {
    if (len < (int)sizeof(KmHead)) return;
    const KmHead* h = (const KmHead*)buf;
    // 包头 mac 校验(原来这个字段从头到尾没被看过 → 谁发包都能注入键鼠)。这是 kmboxNET
    //   本来就有的身份位:客户端按 UI 上显示的 MAC 配置,对不上直接丢。不是密码学强度
    //   (明文可嗅探),但挡住"随手扫个端口就能操控别人游戏"。
    if (h->mac != g.netMac) return;
    const uint8_t* pl = buf + sizeof(KmHead);
    int plen = len - (int)sizeof(KmHead);

    switch (h->cmd) {
        case CMD_CONNECT:
            g.log(L"kmbox 客户端已连接");
            g.netClients = 1;
            break;
        case CMD_MOUSE_MOVE:
        case CMD_MOUSE_LEFT:
        case CMD_MOUSE_RIGHT:
        case CMD_MOUSE_MIDDLE:
        case CMD_MOUSE_WHEEL:
        case CMD_MOUSE_AUTO:
        case CMD_BAZERMOVE:
            if (plen >= (int)sizeof(KmMouse)) {
                const KmMouse* m = (const KmMouse*)pl;
                capture::injectNetMouse((uint8_t)m->button, m->x, m->y, m->wheel, true);
            }
            break;
        case CMD_KEYBOARD_ALL:
            if (plen >= (int)sizeof(KmKbd)) {
                const KmKbd* k = (const KmKbd*)pl;
                uint8_t keys[6];
                for (int i = 0; i < 6; i++) keys[i] = (uint8_t)k->button[i];
                capture::injectNetKey((uint8_t)k->ctrl, keys);
            }
            break;
        case CMD_MASK_MOUSE:
            capture::setPhysMask((uint8_t)(h->rand & 0xFF));
            break;
        case CMD_UNMASK_ALL:
            capture::setPhysMask(0);
            break;
        default:
            break;
    }
}

void rxLoop() {
    auto perf = performance::EnableCurrentThreadHighPerformance(THREAD_PRIORITY_HIGHEST);
    if (!perf.priority || !perf.executionSpeed) {
        g.log(L"网络输入线程后台调度保护部分失败,请查看后台性能说明");
    }
    uint32_t buf32[32];
    uint8_t* buf = (uint8_t*)buf32;
    while (!s_stop) {
        sockaddr_in from{}; int fl = sizeof(from);
        int n = recvfrom(s_sock, (char*)buf, 128, 0, (sockaddr*)&from, &fl);
        if (n <= 0) {
            if (s_stop) break;
            continue;   // 超时/瞬断:继续(closesocket 时 recvfrom 返回错误 → s_stop 已置)
        }
        g.netRx = g.netRx + 1;
        bool enc = (n == 128);
        if (enc) xxteaDecrypt(buf32, 32, s_key);
        if (n < (int)sizeof(KmHead)) continue;
        const KmHead* h = (const KmHead*)buf;
        if (h->mac != g.netMac) continue;   // 同 handle():先验身份再谈命令
        if (h->cmd == CMD_MONITOR) {
            uint16_t mport = (uint16_t)(h->rand & 0xFFFF);
            if (mport) {
                s_monAddr = from; s_monAddr.sin_port = htons(mport);
                s_monOn = true;
                if (s_monEvt) SetEvent(s_monEvt);   // 立即推一次当前状态
            } else {
                s_monOn = false;
            }
        } else {
            handle(buf, n);
        }
        // 回显 16B 头(客户端据 cmd+indexpts 匹配判成功)
        sendto(s_sock, (const char*)buf, sizeof(KmHead), 0, (sockaddr*)&from, fl);
    }
}

// monitor 推送:物理输入到达(capture 回调 SetEvent)才醒,聚合 ~1ms,增量发完清零。
void monLoop() {
    auto perf = performance::EnableCurrentThreadHighPerformance(THREAD_PRIORITY_ABOVE_NORMAL);
    if (!perf.priority || !perf.executionSpeed) {
        g.log(L"网络回读线程后台调度保护部分失败,请查看后台性能说明");
    }
    while (!s_stop) {
        WaitForSingleObject(s_monEvt, 200);
        if (s_stop) break;
        if (!s_monOn) continue;
        Sleep(1);   // 聚合窗口,限速 ~1kHz
        int dx, dy, wh; capture::fetchPhysMotion(dx, dy, wh);
        uint8_t btn, mod, keys[6]; capture::physState(btn, mod, keys);
        StdMouse mr{}; mr.buttons = btn; mr.x = clamp16(dx); mr.y = clamp16(dy); mr.wheel = clamp16(wh);
        StdKbd kr{}; kr.buttons = mod; memcpy(kr.data, keys, 6);
        uint8_t pkt[sizeof(StdMouse) + sizeof(StdKbd)];
        memcpy(pkt, &mr, sizeof(mr));
        memcpy(pkt + sizeof(mr), &kr, sizeof(kr));
        sendto(s_sock, (const char*)pkt, sizeof(pkt), 0, (sockaddr*)&s_monAddr, sizeof(s_monAddr));
    }
}

void monKick() { if (s_monOn && s_monEvt) SetEvent(s_monEvt); }   // monitor 关时零唤醒

bool openSocket() {
    WSADATA wsa; static bool wsaInit = false;
    if (!wsaInit) { if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false; wsaInit = true; }
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock == INVALID_SOCKET) return false;
    DWORD tv = 300;   // recv 300ms 超时,让 stop 能退出
    setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    sockaddr_in me{};
    me.sin_family = AF_INET;
    // 默认只绑回环:kmboxNET 第三方软件都跑在本机,没有理由把"键鼠注入入口"开到局域网。
    //   本服务无握手、明文包也照单执行,绑 0.0.0.0 等于同网段任何人都能往游戏里注入键鼠,
    //   甚至用 MASK 命令屏蔽掉用户自己的鼠标。确需远程的用户在 link.json 里置 netLan=1。
    me.sin_addr.s_addr = htonl(g.netLan ? INADDR_ANY : INADDR_LOOPBACK);
    me.sin_port = htons((u_short)g.netPort);
    if (bind(s_sock, (sockaddr*)&me, sizeof(me)) != 0) {
        closesocket(s_sock); s_sock = INVALID_SOCKET; return false;
    }
    return true;
}

}  // namespace

namespace netserver {

void start() {
    if (!g.netEnable) return;
    if (s_sock != INVALID_SOCKET) return;   // 已在跑
    if (g.netMac == 0) {                     // 首次:随机盒子 MAC(4B),持久化留给 settings::save
        // 用真随机源:原来是 rand()(全程没 srand,序列恒定)^ GetTickCount(),熵实际只剩
        //   开机毫秒数、可猜 —— 而这个 MAC 正是 XXTEA 的密钥根。
        std::random_device rd;
        g.netMac = (unsigned)rd() ^ ((unsigned)rd() << 16) ^ (unsigned)rd();
        if (g.netMac == 0) g.netMac = 0x51A3C7E9;
    }
    deriveKey();
    if (!openSocket()) { g.log(L"kmbox 服务启动失败(端口被占?)"); return; }
    s_stop = false;
    if (!s_monEvt) s_monEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    capture::setMonitorCallback(monKick);
    s_rxThread  = std::thread(rxLoop);
    s_monThread = std::thread(monLoop);
    g.netUp = true;
    wchar_t m[64];
    swprintf(m, 64, L"kmbox 模拟已启动 · UDP %d · MAC %08X", g.netPort, g.netMac);
    g.log(m);
}

void stop() {
    if (s_sock == INVALID_SOCKET && !s_rxThread.joinable()) return;
    s_stop = true;
    capture::setMonitorCallback(nullptr);
    if (s_monEvt) SetEvent(s_monEvt);
    if (s_sock != INVALID_SOCKET) { closesocket(s_sock); s_sock = INVALID_SOCKET; }
    if (s_rxThread.joinable())  s_rxThread.join();
    if (s_monThread.joinable()) s_monThread.join();
    s_monOn = false;
    capture::setPhysMask(0);   // 退出务必解除物理屏蔽,别把本机键鼠锁死
    g.netUp = false;
    g.netClients = 0;
}

void applyConfig() {
    stop();
    start();
}

bool isUp() { return g.netUp.load(); }

}  // namespace netserver
