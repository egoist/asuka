# Asuka 文档

<!-- repository-only:start -->
[English](../README.md) · [日本語](../ja/README.md)
<!-- repository-only:end -->

Asuka 让你在 iOS、Android、macOS、Windows、Linux 和 Web 上，用各平台的原生控件构建应用。应用用 **Asuka** 编写，这是一门专为界面设计的语言：组件声明自己要显示什么，状态说明哪些东西会变。状态变化时，只有读取了它的那些属性会被发送给平台的视图，不需要对整棵树做 diff。

```asuka
component Counter {
  state count = 0

  SafeArea {
    align: .center; justify: .center; gap: 16

    Text("Count: {count}") { font: .largeTitle }
    Button("Add") {
      variant: .filled
      on press { count += 1 }
    }
  }
}

app { Counter() }

test "counts taps" {
  let app = mount(Counter())
  app.tap("Add")
  app.expect("Count: 1")
}
```

## 工作原理

Asuka 负责运行应用的组件和状态、布局（flexbox 和 grid，遵循 CSS 规范），以及需要等待的工作（异步函数、task、定时器），并创建和更新各平台自己的视图，每帧只更新发生变化的部分：

| 平台 | 使用的视图 |
| --- | --- |
| iOS | UIKit |
| Android | Android Views 和 Material 3 |
| macOS | AppKit |
| Windows | Win32 和通用控件，绘制成 Windows 11 的样式 |
| Linux | GTK 4 和 libadwaita |
| Web | 页面的 DOM 和浏览器自带的控件 |

一个文本输入框，在各平台上分别就是 `UITextField`、`EditText`、`NSTextField`、`EDIT` 控件、`GtkEntry` 或 `<input>`；页面的导航栈则分别对应 `UINavigationController`、带返回手势的 Material 顶部应用栏、Mac 窗口中带返回按钮的工具栏，或 libadwaita 的导航视图。因此，一个不设置任何样式的应用，看起来就和该平台上的其他应用一样，无障碍、文本输入和滚动也都和它们表现一致。

开发时，`asuka dev` 运行你的应用：可以跑在 iOS 模拟器、Android 模拟器或手机上，也可以直接跑在电脑上，或者跑在浏览器里。保存后几毫秒内改动就会到达应用，而应用的状态和页面都会保留下来。发布构建则用平台自己的编译器把应用预先编译好：没有解释器，没有垃圾回收，也就没有停顿。

同一个应用可以在所有平台上运行。在手机上，它铺满整个屏幕；在桌面端，它是一个用户可以调整大小的窗口，还可以打开更多窗口，并拥有带键盘快捷键的菜单栏（在手机上会被忽略）。如果某个包提供的功能在某个平台上并不存在（比如 Mac 上的传感器或主屏幕小组件），它在那个平台上会告诉你“不可用”，应用可以据此检查，在缺少这项功能时照常运行。

应用可运行在 iOS 16、Android 7（API 24）、macOS 11、Windows 10 及以上版本，以及装有 GTK 4.14 和 libadwaita 1.5 或更高版本的 Linux 上，也可以在当前主流浏览器中运行。`asuka` 命令行工具是一个独立的程序，一条命令就能装好（见[快速上手](getting-started.md#安装命令行工具)）；要为某个平台构建，需要该平台自己的工具（Xcode、Android SDK 和 NDK）以及一个 C 编译器（系统自带的，或者 Zig）。

## 指南

- [快速上手](getting-started.md)：安装命令行工具，创建一个应用，并在手机、模拟器、电脑或浏览器上运行它。
- [Asuka 速览](language-tour.md)：用一页介绍编写应用所用的语言：值、函数、结构体与枚举、组件与状态、异步、错误和测试。
- [核心概念](concepts.md)：组件与状态、哪些部分会更新、effect 与 task、owner、主线程和环境。
- [布局与样式](layout.md)：flexbox、尺寸、安全区域、颜色、文字、阴影和渐变。
- [组件](components.md)：文本、按钮、输入框、图片、列表、视频、网页视图和绘图。
- [导航](navigation.md)：导航栈、标签页、弹出面板、警告框和菜单，窗口和菜单栏，打开应用的链接，以及从其他应用分享进来的内容。
- [动效与手势](motion.md)：动画、过渡、共享元素、手势、按键和触感反馈。
- [无障碍](accessibility.md)：标签、角色、自定义操作、播报，以及可缩放的文字。
- [多语言](i18n.md)：支持复数的翻译文案，按用户习惯显示的数字和日期，从右到左的布局，以及切换语言试用。
- [数据](data.md)：跨启动保留的值、支持实时查询的 SQLite、安全存储，以及监听文件变化。
- [设备与服务](device.md)：各平台上的相机、扫码、照片与文件、预览、打印与 PDF、邮件和短信编辑界面、定位、通知与推送、音频、正在播放与录音、网络、媒体库、后台任务、登录与生物识别、屏幕与启动屏、地图、应用商店服务、传感器、图片编辑、语音、通讯录与日历、主屏幕小组件与快捷方式，以及崩溃报告。
- [应用内购买](purchases.md)：通过 App Store、Google Play 和 Microsoft Store 销售消耗型、非消耗型商品和订阅，处理交易、恢复购买、在服务器上验证，以及测试。
- [NFC 标签](nfc.md)：读写标签上的 NDEF 消息、标签的 ID 和原始命令、iOS 的系统面板与 Android 的读卡器模式、Web NFC，以及能打开应用的标签。
- [蓝牙](bluetooth.md)：低功耗蓝牙设备的扫描或选择、连接、读写和订阅；应用作为外围设备进行广播和提供服务；以及各平台上的情况。
- [iCloud](icloud.md)：跟随用户同步到每台设备的键值存储，以及应用在 iCloud Drive 中的文档：列出、读写、下载和移除本地副本、解决冲突；支持 iOS 和 macOS。
- [电视](tv.md)：Apple TV 和 Android TV：遥控器的焦点、返回键和播放/暂停键，电视上有哪些控件，以及如何为它们构建。
- [车机屏幕](car.md)：在 CarPlay 和 Android Auto 上显示应用，使用平台的模板：列表、网格、标签页、地点和正在播放，以及应用的类别和测试。
- [桌面端](desktop.md)：托盘图标和程序坞（Dock）菜单、后台运行的应用、全局快捷键、文件对话框和文件管理器、打开文件、单实例、退出、最近文档、开机启动、更新、显示器、电源，以及应用图标上的进度。
- [程序](programs.md)：用 Asuka 写的命令行工具（`fn main`）：参数、命令行选项和 stdin，运行其他程序，为各桌面系统构建可执行文件，以及应用自带的命令。
- [包](packages.md)：在应用之间共享代码：`asuka add`、npm registry、`asuka.lock` 和发布。
- [标准库与平台 SDK](stdlib.md)：`std/...` 和 `platform/...` 这些包，以及它们在各平台上分别做什么。
- [在 Asuka 中调用 C](c.md)：用 `extern` 声明调用 C（应用自己的、某个库的、系统的），`asuka.toml` 的 `[c]`，厂商预编译好的库，以及回调。
- [原生代码](native.md)：原生包——在 Asuka 里声明带类型的模块和视图，在各平台自己的项目里实现：iOS 和 macOS 用 Swift，Android 用 Kotlin，Web 用 JavaScript，Windows 和 Linux 用 C。
- [开发](development.md)：深入介绍各平台上的 `asuka dev`、设备、编辑器和常见问题。
- [测试](testing.md)：用 `test` 块测试应用的界面，不需要设备或显示器：点击、输入并检查屏幕上显示的内容；时间只在应用等待时才会推进。
- [打包与发布](shipping.md)：面向 App Store 和 Google Play 的构建、签名并公证的 Mac 应用及其磁盘映像、Windows 文件夹、Linux 的 `.deb`、`.rpm`、AppImage、压缩包与 Flatpak 清单、桌面应用的签名更新，以及 `asuka.toml` 中对应用的描述。
- [Web](web.md)：同一个应用以 WebAssembly 加页面 DOM 的方式在浏览器中运行；`asuka dev --platform web`、可部署的静态站点、嵌入到其他页面，以及目前还不支持的功能。

## 参考

- [asuka.toml](asuka-toml.md)：应用 `asuka.toml` 里的每一个键，它的类型、默认值，以及在各个平台上的作用。
- [与 SwiftUI 和 Expo 对比](parity.md)：Asuka 已经有什么，还缺什么。
- [Asuka 与 MyGo](mygo.md)：平台原生控件与框架自绘界面、各自能运行在哪里（MyGo 的手机支持即将推出），以及什么时候该选哪个。
- [语言参考](language/README.md)：按主题分页介绍整个语言：类型及其内置方法、声明、组件、store、异步、错误、样式、导航、原生代码和测试。
