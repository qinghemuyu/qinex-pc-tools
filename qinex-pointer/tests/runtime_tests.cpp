#include <winsock2.h>
#include "app.h"
#include "pointer_runtime.h"
#include <iostream>
#include <stdexcept>
#if QINEX_WITH_ANDROID_TOOLS
#include "embedded_runtime.h"
#endif

namespace {
void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void Pump() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        Check(message.message != WM_QUIT, "嵌入光标不应结束宿主消息循环");
        if (!qpointer::desktop::ProcessMessage(message)) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
    }
}
}

int main() {
#if QINEX_WITH_ANDROID_TOOLS
    // 完整版也必须与真实手机偏好、恢复备份和运行时缓存隔离。
    qp::android::ScopedDataDirectory androidData(std::filesystem::path(ExeDirectory()) / L"android-test");
#endif
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data)) return 1;
    SOCKET sender = INVALID_SOCKET;
    HWND host = nullptr;
    int result = 0;
    try {
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        host = CreateWindowExW(0, L"STATIC", L"Pointer 测试宿主", WS_OVERLAPPED,
            0, 0, 1088, 880, nullptr, nullptr, instance, nullptr);
        Check(host != nullptr, "创建宿主失败");
        sender = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        Check(sender != INVALID_SOCKET, "创建测试 socket 失败");
        for (int cycle = 0; cycle != 2; ++cycle) {
            Check(qpointer::desktop::Initialize(instance, host), "初始化失败");
            const HWND main = app->main, overlay = app->overlay;
            Check(GetParent(main) == host && (GetWindowLongPtrW(main, GWL_STYLE) & WS_CHILD),
                "光标设置必须嵌入宿主，不能生成第二个顶层窗口");
            Check(GetPropW(host, qpointer::desktop::kEmbeddedWindowProperty) == main,
                "宿主必须发布嵌入窗口供快捷键定位");
            Check(!IsWindowVisible(main), "启动不应弹出第二个主窗口");
            app->UpdateTray(true);
            Check(!app->trayInstalled, "嵌入模式不应安装第二个托盘");
            Check(qpointer::desktop::Initialize(instance, host) && app->main == main, "重复打开应复用同一实例");
            app->paused = true; // 测试接收但不在用户桌面显示光标。
            SOCKET reservation = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            sockaddr_in address{}; address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            Check(reservation != INVALID_SOCKET, "创建端口保留 socket 失败");
            const int bound = bind(reservation, reinterpret_cast<sockaddr*>(&address), sizeof(address));
            int length = sizeof(address);
            const int named = getsockname(reservation, reinterpret_cast<sockaddr*>(&address), &length);
            closesocket(reservation);
            Check(bound == 0 && named == 0, "分配本机测试端口失败");
            app->settings.port = ntohs(address.sin_port);
            app->settings.sourceIp = L"127.0.0.1";
            Check(app->Start(), "接收未启动");
            const char packet[] = R"({"type":"move","nx":0.25,"ny":0.75,"show":true,"down":true})";
            Check(sendto(sender, packet, sizeof(packet)-1, 0, reinterpret_cast<sockaddr*>(&address), sizeof(address)) > 0, "发送失败");
            const ULONGLONG deadline = GetTickCount64() + 2000;
            while (!app->state.hasPosition && GetTickCount64() < deadline) {
                MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
                Pump();
            }
            Check(app->state.hasPosition && app->state.position.x == .25 && app->state.position.y == .75 && app->state.down,
                "合并消息循环未收到真实 UDP 报文");
            SendMessageW(main, WM_CLOSE, 0, 0);
            Pump();
            Check(IsWindow(main) && !IsWindowVisible(main) && app->running, "关闭设置不应停止接收");
            app->Stop();
            Check(!app->running && !app->state.hasPosition, "停止后应清理状态");
            Check(qpointer::desktop::CanExit(), "无草稿应允许宿主退出");
            qpointer::desktop::Shutdown();
            Check(!app && !IsWindow(main) && !IsWindow(overlay), "退出应释放所有光标窗口");
            Check(!GetPropW(host, qpointer::desktop::kEmbeddedWindowProperty), "退出必须清除宿主属性");
            Pump();
            qpointer::desktop::Shutdown(); // 重复退出必须安全。
            SOCKET probe = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            Check(probe != INVALID_SOCKET, "创建端口检查 socket 失败");
            const int reusable = bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address));
            closesocket(probe);
            Check(reusable == 0, "退出后 UDP 端口应可重新绑定");
        }
        std::cout << "嵌入生命周期、UDP 收发、隐藏设置、重复启停和端口释放通过\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        qpointer::desktop::Shutdown(); result = 1;
    }
    if (sender != INVALID_SOCKET) closesocket(sender);
    if (host) DestroyWindow(host);
    WSACleanup();
    return result;
}
