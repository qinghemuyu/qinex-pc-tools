#pragma once

#include <initializer_list>
#include <string>
#include <string_view>

namespace qpointer {

// 两条绘制路径共用换行规则；只切片已有不可变文本，不在高刷绘制循环内分配字符串。
template<typename DrawLine>
int ForEachOutputHudLine(const std::wstring* status, const std::wstring* shortcuts,
        DrawLine draw) {
    int row = 0;
    for (const auto* text : {status, shortcuts}) {
        if (!text || text->empty()) continue;
        std::wstring_view remaining(*text);
        while (!remaining.empty()) {
            const auto end = remaining.find(L'\n');
            const auto line = remaining.substr(0, end);
            draw(line, row++);
            if (end == std::wstring_view::npos) break;
            remaining.remove_prefix(end + 1);
        }
    }
    return row;
}

inline int OutputHudLineCount(const std::wstring* status, const std::wstring* shortcuts) {
    return ForEachOutputHudLine(status, shortcuts, [](std::wstring_view, int) {});
}

}  // namespace qpointer
