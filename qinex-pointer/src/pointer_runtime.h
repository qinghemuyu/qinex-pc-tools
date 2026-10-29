#pragma once
#include <windows.h>

// 由创建窗口的 UI 线程调用；Link 和独立 Pointer 共用同一套实现及配置。
namespace qpointer::desktop {
inline constexpr UINT kExitHostMessage = WM_APP + 90;
inline constexpr UINT kShowPointerMessage = WM_APP + 91;
inline constexpr UINT kShowDeviceMessage = WM_APP + 92;
inline constexpr UINT kThemeChangedMessage = WM_APP + 93;
inline constexpr wchar_t kEmbeddedWindowProperty[] = L"QInEX.Pointer.EmbeddedMain.v1";
bool Initialize(HINSTANCE instance, HWND host = nullptr, int show = SW_HIDE, const RECT* bounds = nullptr);
void Resize(const RECT& bounds);
void HideSettings();
bool ResumeRequested();
bool ProcessMessage(MSG& message);
void ShowSettings();
void ShowMenu(POINT position);
bool CanExit();
void Shutdown();
}
