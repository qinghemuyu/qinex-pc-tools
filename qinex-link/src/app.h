// QInEX-Link 共享状态与常量(上位机直驱 PC 工具,模块与协议见 docs/架构与维护.md)
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <atomic>
#include <string>
#include <deque>
#include <mutex>
#include <cstdint>
#include <cstdio>

// 连接状态机
enum LinkState {
    LS_IDLE = 0,      // 未连接
    LS_OPENING,       // 正在打开串口
    LS_WAITSTATUS,    // 等板子 STATUS 心跳(判 Mode)
    LS_NEGOTIATE,     // KMLINK 3M 协商中
    LS_CONNECTED,     // 已连接(txReady 还要求 Mode C)
    LS_RECONNECT,     // 链路中断,重连中
};

// 自定义窗口消息(主窗口)
#define MSG_TRAY     (WM_APP + 1)
#define MSG_HOTKEY   (WM_APP + 2)   // 独占收放热键按下
#define MSG_BINDDONE (WM_APP + 3)   // 绑键完成,wParam = vk
#define MSG_LINKUPD  (WM_APP + 4)   // 链路状态变化 → 刷 UI
#define MSG_LOG      (WM_APP + 5)   // 日志更新 → 刷日志条
#define MSG_PANIC    (WM_APP + 6)   // 应急解锁(Ctrl+Alt+Q):释放独占 + 停转发
#define MSG_TASKBAR_COMMAND (WM_APP + 7) // 任务栏控制岛 → 主 UI 线程（控制岛线程不直接改配置）

enum TaskbarCommand : WPARAM {
    TBC_SHOW = 1,
    TBC_TIER_DOWN,
    TBC_FORWARD,
    TBC_TIER_UP,
    TBC_EXCLUSIVE,
};

inline uint64_t nowUs() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart * 1000000.0 / freq.QuadPart);
}

struct App {
    // ---- 持久化设置(link.json)----
    std::wstring portName;            // "COM47"
    int  tier      = 1000;            // 125/250/500/1000/2000/4000/8000;0 = 直通
    bool exclusive = false;           // 捕获模式:false 共享 / true 独占
    bool stopOnRelease = false;       // true:独占热键放行本机输入时同时停止转发
    std::atomic<UINT> hotkeyVk{VK_SCROLL}; // 独占收放热键(Hook 线程读,UI 线程改)
    bool link3m    = false;           // 尝试 KMLINK 提速 3Mbps(默认关:921600 已够真 8K,3M 仅加余量)
    bool autostart = false;
    bool taskbarWidget = true;        // Windows 11 任务栏控制岛（失败不影响主功能）
    int  winX = -100000, winY = -100000;

    // ---- kmboxNET 本机模拟(P3)----
    bool netEnable = false;           // 开本机 UDP kmbox 服务(默认关,发版初期不放开)
    int  netPort   = 6234;            // 与板端 kmnet 同默认端口
    bool netLan    = false;           // false=只绑 127.0.0.1(默认,第三方软件都在本机);
                                      // true=绑所有网卡,等于把键鼠注入入口开到局域网,慎开
    unsigned netMac = 0x00000000;     // 盒子 MAC(4B,XXTEA 密钥根;0=首次运行随机生成)

    // ---- 日志保存(用户侧排障:开着 = 每条日志实时落盘 logs\*.log)----
    bool logSave = false;

    // ---- 运行态 ----
    std::atomic<int>  state{LS_IDLE};
    std::atomic<bool> forwarding{false};
    std::atomic<bool> grabbed{false};      // 独占钩子当前是否吞本机输入
    std::atomic<bool> bindWait{false};     // 等待用户按下新热键
    std::atomic<char> boardMode{'?'};      // 'A' / 'C' / '?'
    std::atomic<int>  boardAct{-1};        // 板子激活状态(STATUS act=):-1 未知 / 0 未激活 / 1 已激活
    std::atomic<bool> verified{false};     // 本次会话启动验证:见过一次「P4 且已激活」即解锁(不落盘,每次启动重验)
    std::atomic<bool> mouseV2{false};      // 板端已确认 KMV2：鼠标改发带 gap_125us 的 F8 帧
    std::atomic<int>  boardBaud{0};
    // 固件 STATUS 的以下性能量均是 5 秒窗口累计值；UI 再换算成 Hz。
    std::atomic<int>  boardUkm{0};       // UART 收到的键鼠帧数
    std::atomic<int>  boardFps{0};       // mapping 生成触摸帧数
    std::atomic<int>  boardDps{0};       // 手机实际取走触摸报告数
    std::atomic<int>  boardGmaxUs{0};    // 生成侧最大相邻帧间隔
    std::atomic<int>  boardDmaxUs{0};    // 交付侧最大装填→取走间隔
    std::atomic<int>  boardUq{0};        // F8 鼠标重放队列当前深度
    std::atomic<int>  boardUqMax{0};     // 本次开机队列峰值
    std::atomic<int>  boardUdrop{0};     // 队列溢出丢弃累计；正常必须为 0
    std::atomic<int>  boardUerr{0};      // 板子开机以来累计(只增,重启才清)
    std::atomic<int>  uerrBase{-1};      // 本次连接首个 uerr 采样(会话基线;-1=未采)
    std::atomic<int>  uerrSession{0};    // = boardUerr - uerrBase(本次连接新增,UI 显示这个)
    std::atomic<bool> usbMounted{false};
    std::atomic<int>  rttX10{-1};          // RTT 毫秒×10;-1 = 未知
    std::atomic<ULONGLONG> lastStatusMs{0};
    std::atomic<uint32_t>  inEvents{0};    // 捕获侧事件累计(UI 差分出速率)
    std::atomic<uint32_t>  outFrames{0};   // 发送侧有效帧累计(不含心跳)
    std::atomic<uint32_t>  txTrunc{0};     // 串口写截断次数:帧被撕成两半上线 → 板端解析永久失步
                                           //   (魔数落到数据字节上 → 垃圾帧 → 板端 uerr 狂涨 + 乱触发按键)。
                                           //   正常必须恒为 0;非 0 = 链路带宽不够或串口堵死。
    std::atomic<bool> netUp{false};        // kmbox UDP 服务是否在监听
    std::atomic<int>  netClients{0};       // 最近有活动的客户端数(近似)
    std::atomic<uint32_t> netRx{0};        // 收到的 kmbox 命令包累计
    ULONGLONG connectStartMs = 0;
    HWND hwnd = nullptr;

    std::mutex logMx;
    std::deque<std::wstring> logLines;     // 最近 200 行(UI 显示环)
    FILE* logFp = nullptr;                 // 日志文件(logMx 保护;开着 = 全量落盘)
    std::wstring logPath;                  // 当前日志文件完整路径(UI 提示用)
    void log(const std::wstring& s);
    void logFileOpen();                    // 开落盘:建 logs\、开会话文件、补写已有 backlog
    void logFileClose();
};
extern App g;
