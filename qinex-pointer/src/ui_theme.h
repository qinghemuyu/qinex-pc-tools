#pragma once

#include <windows.h>

namespace qpointer {

// 界面颜色只用于设置窗口，采集视频、融合背景和光标颜色继续使用各自配置。
struct UiPalette {
    COLORREF background, sidebar, panel, field, border;
    COLORREF text, secondary, disabled, accent, accentText, selected, hover;
};

inline UiPalette PointerPalette(bool anime) {
    if (anime) return {
        RGB(255, 247, 250), RGB(253, 236, 243), RGB(255, 253, 254),
        RGB(255, 246, 250), RGB(239, 215, 226), RGB(76, 48, 65),
        RGB(139, 102, 122), RGB(174, 148, 162), RGB(198, 70, 125),
        RGB(255, 255, 255), RGB(250, 220, 234), RGB(253, 235, 244)
    };
    // 与 P4 板载页面的深色主题保持一致。
    return {
        RGB(12, 14, 17), RGB(17, 20, 25), RGB(21, 24, 29),
        RGB(27, 31, 38), RGB(49, 55, 67), RGB(231, 234, 239),
        RGB(142, 151, 165), RGB(103, 113, 128), RGB(79, 216, 162),
        RGB(12, 40, 29), RGB(27, 53, 45), RGB(37, 43, 52)
    };
}

}  // namespace qpointer
