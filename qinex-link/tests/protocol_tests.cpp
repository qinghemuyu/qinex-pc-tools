// 固定字节向量验证公开协议；无需连接 P4 或安装输入钩子。
#include "protocol.h"
#include <iostream>
#include <stdexcept>

void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
int main() {
    try {
        Check(link_protocol::Mouse(1, 1, -2, -1) == std::array<uint8_t, 8>{0xF6, 1, 1, 0, 0xFE, 0xFF, 0xFF, 0x08}, "F6 固定向量");
        Check(link_protocol::TimedMouse(1, 1, -2, -1, 125) == std::array<uint8_t, 9>{0xF8, 1, 1, 0, 0xFE, 0xFF, 0xFF, 1, 0x07}, "F8 固定向量");
        const uint8_t keys[6]{4, 5, 0, 0, 0, 0};
        Check(link_protocol::Keyboard(2, keys) == std::array<uint8_t, 10>{0xF7, 2, 4, 5, 0, 0, 0, 0, 0, 0xF4}, "F7 固定向量");
        const uint8_t released[6]{};
        Check(link_protocol::Keyboard(0, released) == std::array<uint8_t, 10>{0xF7, 0, 0, 0, 0, 0, 0, 0, 0, 0xF7}, "全抬键盘");
        const auto clamped = link_protocol::Mouse(0, 50000, -50000, 300);
        Check(clamped[2] == 0xff && clamped[3] == 0x7f && clamped[4] == 0 && clamped[5] == 0x80 && clamped[6] == 127, "输入饱和范围");
        Check(link_protocol::TimedMouse(0, 0, 0, 0, 0)[7] == 0, "重新锚定");
        Check(link_protocol::TimedMouse(0, 0, 0, 0, 1)[7] == 1, "最小时间单位");
        Check(link_protocol::TimedMouse(0, 0, 0, 0, 187)[7] == 1 && link_protocol::TimedMouse(0, 0, 0, 0, 188)[7] == 2, "时间量化边界");
        Check(link_protocol::TimedMouse(0, 0, 0, 0, 65535)[7] == 255, "时间量化上限");
        Check(link_protocol::Ping(0x0807060504030201ULL) == std::array<uint8_t, 9>{0xFE, 1, 2, 3, 4, 5, 6, 7, 8}, "PING 小端且无校验尾");
        std::cout << "Link protocol checks passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
