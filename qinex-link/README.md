# QInEX-Link

Windows x64 的 P4 Mode C 键鼠转发工具。使用 Raw Input 与键鼠 Hook 捕获输入，经 UART 交给 P4 的映射引擎；提供共享/独占、回报率档位、串口重连、后台性能保护、可选任务栏状态与 kmboxNET 输入服务。

## 使用

1. P4 设为上位机直驱 Mode C，电脑连接板子的 UART 串口。
2. 打开 Link，选择串口并连接，等待设备状态确认。
3. 从 1000Hz 默认档开始验证转发；按需要启用独占。
4. 默认 Scroll Lock 切换独占收放；`Ctrl+Alt+Q` 紧急释放独占并停止转发。

程序以当前连接的 Mode C 与激活状态作为转发条件。使用高权限应用时，可由用户按需以管理员身份运行。详细性能与环境说明见 [后台性能说明](后台性能说明.txt)。

设置保存为 EXE 同目录的 `link.json`；日志按用户开关保存到 `logs/`。首次运行使用默认设置。3Mbps 提速默认关闭，协商失败自动回落 921600。软件档位不保证手机实际达到对应交付频率。

## 内置 PC 光标

主窗口顶部「设备连接」「光标设置」切换同一窗口内的内容，Ctrl+Tab 也可切换。设备连接页显示串口、转发、链路统计与日志；光标设置保留全部桌面功能。两部分共用官网黑绿配色、细网格与透明细边框分区，主操作采用荧光绿，P4 标识采用金色。右键统一托盘的「PC 光标与画面…」可连接、断开、暂停和操作全屏。切页和隐藏主窗口后接收继续，退出 Link 时统一清理光标与采集。无需另开 `QInEX-Pointer.exe`。首次配置与旧设置迁移见 [合并版使用](../docs/合并版使用.md)。

## 构建与维护

从仓库根目录运行 `./build.ps1 -Project Link -Test`，或在本目录运行 `./build.ps1 -Test`。构建不依赖 P4 源码、Android SDK 或私有 SDK。

参见 [构建指南](../docs/构建指南.md)、[串口协议](../docs/protocols/link-uart.md)、[kmboxNET 接入](../docs/protocols/kmnet.md) 和 [本机协作协议](../docs/protocols/desktop-ipc.md)。

原创部分适用 [PolyForm Noncommercial 1.0.0](../LICENSE.txt)，商业用途需另行授权；第三方署名见 [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt)。
