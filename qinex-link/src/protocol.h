// Mode C 线协议的纯函数编码器；不做 I/O，不保存连接状态。
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>

namespace link_protocol {
template <size_t N> inline void Finish(std::array<uint8_t, N>& frame) {
    uint8_t checksum = 0;
    for (size_t i = 0; i + 1 < N; ++i) checksum ^= frame[i];
    frame.back() = checksum;
}
inline std::array<uint8_t, 8> Mouse(uint8_t buttons, int dx, int dy, int wheel) {
    dx = std::clamp(dx, -32768, 32767);
    dy = std::clamp(dy, -32768, 32767);
    wheel = std::clamp(wheel, -128, 127);
    std::array<uint8_t, 8> frame{0xF6, buttons,
        static_cast<uint8_t>(dx & 0xff), static_cast<uint8_t>((dx >> 8) & 0xff),
        static_cast<uint8_t>(dy & 0xff), static_cast<uint8_t>((dy >> 8) & 0xff),
        static_cast<uint8_t>(static_cast<int8_t>(wheel)), 0};
    Finish(frame);
    return frame;
}
inline std::array<uint8_t, 9> TimedMouse(uint8_t buttons, int dx, int dy, int wheel, uint16_t gapUs) {
    const auto legacy = Mouse(buttons, dx, dy, wheel);
    std::array<uint8_t, 9> frame{};
    std::copy_n(legacy.begin(), 7, frame.begin());
    frame[0] = 0xF8;
    // 零表示重新锚定，其余以 125 微秒量化，最大 31.875 毫秒。
    frame[7] = gapUs ? static_cast<uint8_t>(std::clamp<int>((gapUs + 62) / 125, 1, 255)) : 0;
    Finish(frame);
    return frame;
}
inline std::array<uint8_t, 10> Keyboard(uint8_t modifiers, const uint8_t keys[6]) {
    std::array<uint8_t, 10> frame{0xF7, modifiers};
    std::copy_n(keys, 6, frame.begin() + 2);
    Finish(frame);
    return frame;
}
inline std::array<uint8_t, 9> Ping(uint64_t timestamp) {
    std::array<uint8_t, 9> frame{0xFE};
    for (int i = 0; i < 8; ++i) frame[1 + i] = static_cast<uint8_t>(timestamp >> (8 * i));
    return frame;
}
}
