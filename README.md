# QInEX PC Tools

QInEX 的 Windows PC 工具集。QInEX-Link 已内置完整 PC 光标、校准与采集预览功能，在同一个主窗口通过顶部「设备连接」「光标设置」切换，采用与官网一致的黑绿配色、细网格和透明细边框分区；同时保留独立 Pointer 的构建。源代码公开，欢迎用户自行构建、修复问题并贡献改进。

**许可：PolyForm Noncommercial 1.0.0。商业用途须另行取得相应权利人的许可。** 这是带非商业限制的源码公开项目；正式条款见 [LICENSE.txt](LICENSE.txt)，中文说明见 [许可说明](docs/许可说明.md)。第三方组件继续适用各自许可。

| 工具 | 作用 | 运行条件 |
| --- | --- | --- |
| [QInEX-Link](qinex-link/README.md) | 捕获 PC 键鼠，经串口转发到 P4 Mode C；支持输入独占、后台调度保护、任务栏状态和可选 kmboxNET 本机输入 | Windows x64；实际转发需要已激活、运行 Mode C 的 P4 |
| [QInEX Pointer](qinex-pointer/README.md) | 接收 UDP 光标，在桌面、采集卡预览或融合器输出中显示；支持多屏、校准、自定义光标与快捷键 | Windows x64；可用内置演示验证本机显示 |

Link 把原始键鼠交给 P4；映射与触摸输出由板端完成。内置 Pointer 接收板端或兼容发送端生成的光标位置。两条链路分别连接和配置，共用程序入口、托盘和退出流程，保留本机窗口消息与旧版本协作。使用与设置迁移见 [合并版使用](docs/合并版使用.md)。

## 快速构建

安装 Visual Studio 2022 的“使用 C++ 的桌面开发”（含 Windows SDK）、CMake 3.21+，在仓库根目录打开 PowerShell：

```powershell
.\build.ps1 -Test
```

默认构建桌面功能，产物：

```text
build/all-desktop/qinex-link/Release/QInEX-Link.exe
build/all-desktop/qinex-pointer/Release/QInEX-Pointer.exe
build/all-desktop/qinex-pointer/Release/QInEX-Pointer-Simulator.exe
```

默认 Pointer 提供 PC 光标、校准、采集卡和融合输出五个页面。需要第六个“手机工具”页面时，按 [构建指南](docs/构建指南.md) 准备 Android 依赖，再使用 `-AndroidTools`。该选项保留 ADB、系统光标设置、图片覆盖与恢复功能。

只构建一个工具：

```powershell
.\build.ps1 -Project Link -Test
.\build.ps1 -Project Pointer -Test
```

## 开始使用

- **Link**：在 P4 板载设置中选择上位机直驱 Mode C，电脑连接对应 UART 串口；在 Link 连接设备后启用转发。默认独占切换键为 Scroll Lock，紧急释放为 `Ctrl+Alt+Q`。首次测试请使用可安全点按的画面。
- **Pointer**：在 P4 的 VP 光标设置中填电脑地址，选择 JSON v2 并关闭接收端自动发现；在 Pointer 连接对应来源，选择画面输出方式，再检查四角和中心。详细操作见 [Pointer 使用说明](qinex-pointer/使用说明.md)。
- **无硬件体验**：Pointer 设备页的演示使用独立接收会话；也可以使用 [测试发送器](docs/测试与验收.md)。

## 开发资料

| 文档 | 内容 |
| --- | --- |
| [开发规范（AGENTS.md）](AGENTS.md) | 新贡献者与 AI 的工作流程、Git 管理、编码边界和验证要求 |
| [构建指南](docs/构建指南.md) | 桌面与 Android 构建、依赖准备、产物和打包 |
| [架构与维护](docs/架构与维护.md) | 模块入口、线程职责、兼容边界 |
| [Link 串口协议](docs/protocols/link-uart.md) | F6/F7/F8、能力协商、心跳和释放 |
| [Pointer UDP 协议](docs/protocols/pointer-udp.md) | JSON 字段、状态合并和显示语义 |
| [两程序协作协议](docs/protocols/desktop-ipc.md) | 快捷键属性与校准时释放输入 |
| [kmboxNET 接入](docs/protocols/kmnet.md) | 网络入口、支持范围与源码位置 |
| [测试与验收](docs/测试与验收.md) | 自动测试、模拟器与真机验证边界 |
| [贡献指南](CONTRIBUTING.md) | 问题反馈、修改与提交约定 |
| [第三方声明](THIRD_PARTY_NOTICES.md) | 组件来源与许可边界 |

维护目标是输入保真、可靠释放、低延迟和兼容性。标称回报率、PC 发送速率、板端输出计数与手机实际显示延迟是不同指标，性能反馈应附测试条件。
