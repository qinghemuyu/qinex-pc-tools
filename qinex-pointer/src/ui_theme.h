#pragma once

#include <windows.h>

namespace qpointer {

// 官网同系控制台配色；只影响操作界面，不改变采集画面、融合背景或光标配置。
struct UiPalette {
    COLORREF background, sidebar, panel, field, border;
    COLORREF text, secondary, disabled, accent, accentText, selected, hover;
};

inline UiPalette PointerPalette(bool anime) {
    if (anime) return {
        RGB(237, 243, 239), RGB(246, 249, 247), RGB(249, 251, 250),
        RGB(236, 243, 239), RGB(190, 208, 199), RGB(13, 31, 22),
        RGB(69, 94, 80), RGB(112, 134, 122), RGB(0, 123, 72),
        RGB(255, 255, 255), RGB(219, 239, 227), RGB(224, 237, 230)
    };
    // 以用户提供的官网截图为参照：近黑、低亮度绿、白字与荧光绿操作色。
    return {
        RGB(5, 10, 7), RGB(7, 14, 10), RGB(10, 19, 14),
        RGB(13, 25, 18), RGB(36, 52, 43), RGB(246, 249, 247),
        RGB(158, 179, 167), RGB(100, 124, 111), RGB(0, 245, 139),
        RGB(2, 24, 13), RGB(12, 41, 26), RGB(23, 46, 33)
    };
}

}  // namespace qpointer
