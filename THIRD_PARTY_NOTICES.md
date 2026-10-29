# 第三方组件与许可

本仓库原创部分适用 [PolyForm Noncommercial 1.0.0](LICENSE.txt)。下列第三方内容保留原有许可；非商业限制不对第三方组件单独施加新的使用限制。

| 组件 | 用途 | 许可与来源 |
| --- | --- | --- |
| Taskbar-Lyrics | Link 任务栏窗口附着与布局思路参考，源码中保留署名 | MIT；完整原始声明见 [qinex-link/THIRD_PARTY_NOTICES.txt](qinex-link/THIRD_PARTY_NOTICES.txt)，[上游项目](https://github.com/mo-jinran/Taskbar-Lyrics) |
| Windows SDK / 系统接口 | Win32、Raw Input、Media Foundation、DirectShow、D3D11、DXGI 等 | 由使用者安装的 Microsoft 开发环境及 Windows 提供，按其各自条款使用 |
| Android Platform Tools | Android 构建中的 ADB 及配套 DLL | [官方来源](https://developer.android.com/tools/releases/platform-tools)；以实际工具包内 NOTICE 为准，含多种第三方许可 |
| Android Build Tools 34.0.0 | Android 光标覆盖包的 AAPT2、zipalign、apksigner 及配套文件 | [官方来源](https://developer.android.com/tools/releases/build-tools)；以 SDK 条款和包内 NOTICE 中各组件许可为准 |
| Eclipse Temurin OpenJDK 17 | Android 覆盖包签名所需的精简 Java 运行时 | GPL-2.0 with Classpath Exception 及随发行包提供的第三方声明；[官方说明](https://adoptium.net/docs/faq) |
| PolyForm Noncommercial 1.0.0 | 项目采用的许可证原文 | [官方文本](https://polyformproject.org/licenses/noncommercial/1.0.0.txt) |

## Android 完整版的依赖

源码仓库通过本地路径和下载脚本准备依赖，不将 Android/Java 二进制作为源码文件跟踪。启用 `QINEX_WITH_ANDROID_TOOLS` 时，构建脚本会把所需文件、NOTICE 与 Java `legal/` 目录内容嵌入 EXE；运行后可在手机工具的诊断详情中导出依赖清单并查看相关文件。

构建成功不等于已经完成二进制再分发许可审查。分发包含 Android 工具的 EXE 前，应核对实际版本的所有许可、保留通知，并履行适用的对应源码提供义务。Temurin 下载地址与摘要已锁定；其运行时本身的 GPL 义务应按该组件处理。Android SDK 中开源组件依其自身许可分发，其余内容适用 SDK 条款。参见 [Android SDK 条款 3.4–3.5](https://developer.android.com/studio/terms)。

更新依赖版本时同步修改版本清单、校验摘要、测试和声明，不能只替换可执行文件。应用图标、用户导入的自定义图片及外部设备厂商品牌须分别尊重对应权利。
