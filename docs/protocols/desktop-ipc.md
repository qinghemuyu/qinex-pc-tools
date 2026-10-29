# Link 与 Pointer 的本机协作

Link 内置 Pointer 与独立版均保留 Win32 窗口类、窗口属性与消息协作。实现见 Link 的 `capture.cpp`、`main.cpp`，Pointer 的 `desktop_helpers.cpp`、`main.cpp`。合并版优先选择本进程 Pointer，且只配对同进程的输出窗口；没有内置实例时回退到独立版。内置 Pointer 释放输入时直接使用宿主句柄。

| 对象 | 标识 |
| --- | --- |
| Pointer 主窗口 | `QPointerMain` |
| Pointer 全屏输出窗口 | `QPointerCaptureOutput` |
| Link 主窗口 | `QInEXLinkMain` |
| 请求放行本机输入 | 注册消息 `QInEX.Pointer.ReleaseLocalInput.v1` |
| Link 内置光标句柄 | Link 主窗口属性 `QInEX.Pointer.EmbeddedMain.v1` |

合并版的 `QPointerMain` 是 `WS_CHILD`，不能通过顶层窗口枚举发现。Link 优先读取自己的内置句柄属性，子窗口销毁时删除该属性；没有内置实例时再枚举兼容的独立版。宿主内的切页、主题刷新和退出消息定义于 `pointer_runtime.h`，不作为跨进程命令使用。

Pointer 主窗口通过下列属性发布当前热键：

| 属性 | 缺省绑定 | `WM_HOTKEY` 的 wParam |
| --- | --- | --- |
| `QInEX.Pointer.Hotkey.Pause.v1` | Ctrl+Alt+F10 | 1 |
| `QInEX.Pointer.Hotkey.Exit.v1` | F11 | 3 |
| `QInEX.Pointer.Hotkey.Range.v1` | F8 | 4 |
| `QInEX.Pointer.Hotkey.Hud.v1` | F7 | 5 |

属性值为 `packed+1`：`packed` 低 8 位为 virtual-key，修饰键位于其后（`modifiers << 8`）。属性不存在/值 0 表示旧版未发布，使用默认值；属性值 1 表示用户关闭该绑定。固定 Ctrl+Alt+F12 对应 wParam=2，用于全屏逃生，不使用可变属性。

退出、HUD、范围与全屏逃生动作需存在输出窗口；暂停可在主窗口存在时使用。Link 约 100ms 刷新一次属性缓存，捕获到相应动作后向 Pointer 发送 `WM_HOTKEY`。应保留按下/抬起的锁存关系，避免改绑或换焦点留下粘键。

Pointer 冻结校准截图后，通过注册消息请求 Link 放行本机输入，供用户选取截图触点；该路径与停止转发是不同动作。此协议服务于同一桌面会话，跨权限级别和桌面隔离可能影响消息交付；失败时必须保留可用的退出与手动恢复路径。

这些名字与 ID 属于兼容接口，改动时需共同更新两个程序和测试，并说明旧版本如何回退。
