// link.json(exe 旁)读写 + 开机自启注册表
#pragma once

namespace settings {
void load();
void save();
void applyAutostart();   // 按 g.autostart 写/删 HKCU Run 键
}
