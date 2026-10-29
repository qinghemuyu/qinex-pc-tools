// PS/2 Set-1 扫描码(Raw Input MakeCode)→ USB HID usage 映射,全键位含数字区。
// 板端 uart_km 0xF7 帧是 boot-protocol 风格(mod + 6 键),与 usb_host 解码同构,
// 所以这里必须用扫描码而不是 VK ——VK 分不清主键区数字与数字小键盘(108 配列)。
#pragma once
#include <cstdint>

// 修饰键返回 0 并置 modBit(HID boot 协议 mod 字节的位);普通键返回 usage、modBit=0。
// 未识别返回 0 且 modBit=0。
inline uint8_t scanToHid(uint16_t make, bool e0, bool e1, uint8_t* modBit) {
    *modBit = 0;
    if (e1) return 0x48;                       // Pause(E1 前缀序列)
    if (e0) {
        switch (make) {
            case 0x1D: *modBit = 0x10; return 0;   // RCtrl
            case 0x38: *modBit = 0x40; return 0;   // RAlt
            case 0x5B: *modBit = 0x08; return 0;   // LGUI(Win)
            case 0x5C: *modBit = 0x80; return 0;   // RGUI
            case 0x1C: return 0x58;   // 小键盘 Enter
            case 0x35: return 0x54;   // 小键盘 /
            case 0x37: return 0x46;   // PrintScreen
            case 0x47: return 0x4A;   // Home
            case 0x48: return 0x52;   // ↑
            case 0x49: return 0x4B;   // PageUp
            case 0x4B: return 0x50;   // ←
            case 0x4D: return 0x4F;   // →
            case 0x4F: return 0x4D;   // End
            case 0x50: return 0x51;   // ↓
            case 0x51: return 0x4E;   // PageDown
            case 0x52: return 0x49;   // Insert
            case 0x53: return 0x4C;   // Delete
            case 0x5D: return 0x65;   // Menu / App
            default:   return 0;
        }
    }
    switch (make) {
        case 0x1D: *modBit = 0x01; return 0;   // LCtrl
        case 0x2A: *modBit = 0x02; return 0;   // LShift
        case 0x38: *modBit = 0x04; return 0;   // LAlt
        case 0x36: *modBit = 0x20; return 0;   // RShift

        case 0x01: return 0x29;   // Esc
        case 0x02: return 0x1E;   // 1
        case 0x03: return 0x1F;   // 2
        case 0x04: return 0x20;   // 3
        case 0x05: return 0x21;   // 4
        case 0x06: return 0x22;   // 5
        case 0x07: return 0x23;   // 6
        case 0x08: return 0x24;   // 7
        case 0x09: return 0x25;   // 8
        case 0x0A: return 0x26;   // 9
        case 0x0B: return 0x27;   // 0
        case 0x0C: return 0x2D;   // -
        case 0x0D: return 0x2E;   // =
        case 0x0E: return 0x2A;   // Backspace
        case 0x0F: return 0x2B;   // Tab
        case 0x10: return 0x14;   // Q
        case 0x11: return 0x1A;   // W
        case 0x12: return 0x08;   // E
        case 0x13: return 0x15;   // R
        case 0x14: return 0x17;   // T
        case 0x15: return 0x1C;   // Y
        case 0x16: return 0x18;   // U
        case 0x17: return 0x0C;   // I
        case 0x18: return 0x12;   // O
        case 0x19: return 0x13;   // P
        case 0x1A: return 0x2F;   // [
        case 0x1B: return 0x30;   // ]
        case 0x1C: return 0x28;   // Enter
        case 0x1E: return 0x04;   // A
        case 0x1F: return 0x16;   // S
        case 0x20: return 0x07;   // D
        case 0x21: return 0x09;   // F
        case 0x22: return 0x0A;   // G
        case 0x23: return 0x0B;   // H
        case 0x24: return 0x0D;   // J
        case 0x25: return 0x0E;   // K
        case 0x26: return 0x0F;   // L
        case 0x27: return 0x33;   // ;
        case 0x28: return 0x34;   // '
        case 0x29: return 0x35;   // `
        case 0x2B: return 0x31;   // 反斜杠
        case 0x2C: return 0x1D;   // Z
        case 0x2D: return 0x1B;   // X
        case 0x2E: return 0x06;   // C
        case 0x2F: return 0x19;   // V
        case 0x30: return 0x05;   // B
        case 0x31: return 0x11;   // N
        case 0x32: return 0x10;   // M
        case 0x33: return 0x36;   // ,
        case 0x34: return 0x37;   // .
        case 0x35: return 0x38;   // /
        case 0x37: return 0x55;   // 小键盘 *
        case 0x39: return 0x2C;   // Space
        case 0x3A: return 0x39;   // CapsLock
        case 0x3B: return 0x3A;   // F1
        case 0x3C: return 0x3B;   // F2
        case 0x3D: return 0x3C;   // F3
        case 0x3E: return 0x3D;   // F4
        case 0x3F: return 0x3E;   // F5
        case 0x40: return 0x3F;   // F6
        case 0x41: return 0x40;   // F7
        case 0x42: return 0x41;   // F8
        case 0x43: return 0x42;   // F9
        case 0x44: return 0x43;   // F10
        case 0x45: return 0x53;   // NumLock
        case 0x46: return 0x47;   // ScrollLock
        case 0x47: return 0x5F;   // 小键盘 7
        case 0x48: return 0x60;   // 小键盘 8
        case 0x49: return 0x61;   // 小键盘 9
        case 0x4A: return 0x56;   // 小键盘 -
        case 0x4B: return 0x5C;   // 小键盘 4
        case 0x4C: return 0x5D;   // 小键盘 5
        case 0x4D: return 0x5E;   // 小键盘 6
        case 0x4E: return 0x57;   // 小键盘 +
        case 0x4F: return 0x59;   // 小键盘 1
        case 0x50: return 0x5A;   // 小键盘 2
        case 0x51: return 0x5B;   // 小键盘 3
        case 0x52: return 0x62;   // 小键盘 0
        case 0x53: return 0x63;   // 小键盘 .
        case 0x56: return 0x64;   // ISO 反斜杠(102 键)
        case 0x57: return 0x44;   // F11
        case 0x58: return 0x45;   // F12
        default:   return 0;
    }
}
