#pragma once

#include <windows.h>
#include <commctrl.h>

#include <array>
#include <string>

namespace qpointer {

struct HotkeyBinding {
    int virtualKey = 0;
    int modifiers = 0;

    bool operator==(const HotkeyBinding& other) const noexcept {
        return virtualKey == other.virtualKey && modifiers == other.modifiers;
    }

    bool operator!=(const HotkeyBinding& other) const noexcept {
        return !(*this == other);
    }
};

// 原生 HOTKEY 控件无法录入 Win 修饰键。这里只接受它能完整读写的三种修饰键，
// 避免打开设置窗口后把已有绑定静默改坏。
inline constexpr int kHotkeyModifierMask = MOD_ALT | MOD_CONTROL | MOD_SHIFT;

inline HotkeyBinding NormalizeHotkey(HotkeyBinding value) noexcept {
    value.virtualKey = value.virtualKey >= 0 && value.virtualKey <= 0xff ? value.virtualKey : 0;
    value.modifiers &= kHotkeyModifierMask;
    if (!value.virtualKey) value.modifiers = 0;
    return value;
}

inline bool IsModifierKey(int virtualKey) noexcept {
    return virtualKey == VK_SHIFT || virtualKey == VK_LSHIFT || virtualKey == VK_RSHIFT ||
        virtualKey == VK_CONTROL || virtualKey == VK_LCONTROL || virtualKey == VK_RCONTROL ||
        virtualKey == VK_MENU || virtualKey == VK_LMENU || virtualKey == VK_RMENU ||
        virtualKey == VK_LWIN || virtualKey == VK_RWIN;
}

// 裸功能键适合全屏操作；普通字符必须带修饰键，避免用户输入时误触。
inline bool IsUsableHotkey(HotkeyBinding value, bool required) noexcept {
    value = NormalizeHotkey(value);
    if (!value.virtualKey) return !required;
    if (IsModifierKey(value.virtualKey) || value.virtualKey == VK_ESCAPE) return false;
    return value.modifiers != 0 ||
        (value.virtualKey >= VK_F1 && value.virtualKey <= VK_F24);
}

// Link 的紧急释放判定允许额外按住 Shift，因此 Ctrl+Alt+Shift+Q 也必须保留。
inline bool IsLinkPanicHotkey(HotkeyBinding value) noexcept {
    value = NormalizeHotkey(value);
    constexpr int required = MOD_CONTROL | MOD_ALT;
    return value.virtualKey == L'Q' && (value.modifiers & required) == required;
}

// 全屏始终预注册这一固定后台逃生键，不能再把同一组合分给其它动作。
inline bool IsCaptureFallbackHotkey(HotkeyBinding value) noexcept {
    value = NormalizeHotkey(value);
    return value.virtualKey == VK_F12 &&
        value.modifiers == (MOD_CONTROL | MOD_ALT);
}

inline bool IsReservedHotkey(HotkeyBinding value) noexcept {
    return IsLinkPanicHotkey(value) || IsCaptureFallbackHotkey(value);
}

inline std::wstring VirtualKeyText(int virtualKey) {
    if (!virtualKey) return L"";
    if (virtualKey >= VK_F1 && virtualKey <= VK_F24)
        return L"F" + std::to_wstring(virtualKey - VK_F1 + 1);
    if ((virtualKey >= L'0' && virtualKey <= L'9') ||
            (virtualKey >= L'A' && virtualKey <= L'Z'))
        return std::wstring(1, static_cast<wchar_t>(virtualKey));
    switch (virtualKey) {
    case VK_SPACE: return L"Space";
    case VK_TAB: return L"Tab";
    case VK_RETURN: return L"Enter";
    case VK_BACK: return L"Backspace";
    case VK_DELETE: return L"Delete";
    case VK_INSERT: return L"Insert";
    case VK_HOME: return L"Home";
    case VK_END: return L"End";
    case VK_PRIOR: return L"Page Up";
    case VK_NEXT: return L"Page Down";
    case VK_LEFT: return L"Left";
    case VK_RIGHT: return L"Right";
    case VK_UP: return L"Up";
    case VK_DOWN: return L"Down";
    default: break;
    }
    const UINT scan = MapVirtualKeyW(static_cast<UINT>(virtualKey), MAPVK_VK_TO_VSC);
    wchar_t name[96]{};
    if (scan && GetKeyNameTextW(static_cast<LONG>(scan << 16), name,
            static_cast<int>(std::size(name))) > 0) return name;
    wchar_t fallback[16]{};
    swprintf_s(fallback, L"VK %02X", virtualKey);
    return fallback;
}

inline std::wstring HotkeyText(HotkeyBinding value, const wchar_t* disabled = L"关闭") {
    value = NormalizeHotkey(value);
    if (!value.virtualKey) return disabled;
    std::wstring text;
    auto append = [&](const wchar_t* part) {
        if (!text.empty()) text += L" + ";
        text += part;
    };
    if (value.modifiers & MOD_CONTROL) append(L"Ctrl");
    if (value.modifiers & MOD_ALT) append(L"Alt");
    if (value.modifiers & MOD_SHIFT) append(L"Shift");
    const std::wstring key = VirtualKeyText(value.virtualKey);
    if (!key.empty()) append(key.c_str());
    return text;
}

inline HotkeyBinding ReadHotkeyControl(HWND control) noexcept {
    const WORD packed = static_cast<WORD>(SendMessageW(control, HKM_GETHOTKEY, 0, 0));
    const BYTE flags = HIBYTE(packed);
    int modifiers = 0;
    if (flags & HOTKEYF_CONTROL) modifiers |= MOD_CONTROL;
    if (flags & HOTKEYF_ALT) modifiers |= MOD_ALT;
    if (flags & HOTKEYF_SHIFT) modifiers |= MOD_SHIFT;
    return NormalizeHotkey({LOBYTE(packed), modifiers});
}

inline void WriteHotkeyControl(HWND control, HotkeyBinding value) noexcept {
    value = NormalizeHotkey(value);
    BYTE flags = 0;
    if (value.modifiers & MOD_CONTROL) flags |= HOTKEYF_CONTROL;
    if (value.modifiers & MOD_ALT) flags |= HOTKEYF_ALT;
    if (value.modifiers & MOD_SHIFT) flags |= HOTKEYF_SHIFT;
    SendMessageW(control, HKM_SETHOTKEY,
        MAKEWORD(static_cast<BYTE>(value.virtualKey), flags), 0);
}

inline int CurrentHotkeyModifiers() noexcept {
    int modifiers = 0;
    if (GetKeyState(VK_CONTROL) < 0) modifiers |= MOD_CONTROL;
    if (GetKeyState(VK_MENU) < 0) modifiers |= MOD_ALT;
    if (GetKeyState(VK_SHIFT) < 0) modifiers |= MOD_SHIFT;
    return modifiers;
}

inline bool MatchesHotkeyKeyDown(HotkeyBinding binding, WPARAM virtualKey) noexcept {
    binding = NormalizeHotkey(binding);
    return binding.virtualKey != 0 && binding.virtualKey == static_cast<int>(virtualKey) &&
        binding.modifiers == CurrentHotkeyModifiers();
}

inline ULONG_PTR PackHotkeyProperty(HotkeyBinding value) noexcept {
    value = NormalizeHotkey(value);
    const ULONG_PTR packed = static_cast<ULONG_PTR>(value.virtualKey & 0xff) |
        (static_cast<ULONG_PTR>(value.modifiers & kHotkeyModifierMask) << 8);
    // +1 让“已发布但关闭”与“旧版程序没有此属性”可以区分。
    return packed + 1;
}

}  // namespace qpointer
