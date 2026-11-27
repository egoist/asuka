# Apple Watch

Asuka 应用可以运行在 Apple Watch 上，使用的是手表自己的控件，就像在手机上使用手机的控件一样：按钮、开关、滑块、步进器、选择器和列表都是 SwiftUI 的，导航和提醒框也由 watchOS 自己绘制，能滚动的内容用数码表冠来滚动。Asuka 在手表上的布局方式和在其他地方一样，所以为手机写的界面到了手腕上还是同一个界面，只是缩放到合适的大小。

手表上跑的是同一份代码。手表应用由 iOS 应用的代码构建而成，所以在那里 `@platform == .iOS`，和 Apple TV 一样，你为 iOS 写的文件和分支都可以照常使用。`environment().watch` 会告诉你应用正运行在手表上，方便你只显示更少、更大的内容。

## 运行

```sh
asuka dev --platform watchos .                # 在手表模拟器上运行，保存即重新加载
asuka build --platform watchos --simulator .  # 用于模拟器的发布版 .app
asuka build --platform watchos .              # 独立手表应用的 .ipa，位于 dist/watchos
asuka publish --platform watchos .            # 构建并上传到 App Store Connect，供 TestFlight 使用
```

`asuka dev --platform watchos` 会在 Apple Watch 模拟器上运行应用：优先使用已经启动的那台，否则使用最新的 Apple Watch Series 模拟器，没有的话就新建一台并启动它（用 `--device` 可以指定其他模拟器）。保存后应用会重新加载，状态保持不变，和在手机上一样。手表模拟器随 Xcode 的 watchOS 平台一起安装：在 Xcode › Settings › Components 中安装，或者运行 `xcodebuild -downloadPlatform watchOS`。在模拟器里，滚动鼠标滚轮或在触控板上滑动，就相当于转动数码表冠。

`asuka dev` 只能在手表模拟器上运行，不能在真实的手表上运行：watchOS 不允许应用建立重新加载所需的、连到你电脑的连接。要在手表上试用应用，请用发布版构建，通过 TestFlight 安装。

## 构建与发布

`asuka build --platform watchos` 会构建一个独立手表应用：它从手表上的 App Store 安装，不需要 iPhone 应用也能运行（watchOS 10 及以上）。它的可执行文件可以在所有装有 watchOS 10 的 Apple Watch 上运行，也就是 Series 4 及之后的机型。`dist/watchos` 中的 `.ipa` 就是 App Store Connect 接受的独立手表应用的格式：手表应用装在一个只负责携带它的小 iPhone 应用里，这个 iPhone 应用永远不会出现在手机上。（如果手表应用由 iPhone 应用携带，它会和 iPhone 应用一起构建：见[与 iPhone 应用一起](#与-iphone-应用一起)。）

签名需要你团队的两个描述文件：一个是手表应用的 watchOS 描述文件，它的 ID 是应用 ID 加上 `.watchkitapp`（例如 `dev.asuka.counter.watchkitapp`）；另一个是应用 ID 本身的 iOS 描述文件，也就是 App Store Connect 中那条记录的 ID。两者都在 developer.apple.com 上创建（Profiles › +），默认使用 App Store 分发，也可以用 `--distribution` 选择。`--profile` 或 `asuka.toml` 中 `[watch]` 的 `profile` 用来指定手表应用的描述文件（[asuka.toml](asuka-toml.md#watch)），`ios.profile` 指定 iPhone 应用的。

`asuka publish --platform watchos` 会生成这个构建并上传到 App Store Connect，和 `--platform ios` 上传 iPhone 应用一样（[发布到应用商店](shipping.md#发布到应用商店)），选项也相同：`--groups` 把构建交给 TestFlight 的测试组，`--notes` 写明新内容，`--review` 提交 App 审核，`--no-wait` 在上传完成后就结束，`--file` 上传之前构建好的 `.ipa`。独立手表应用在 App Store Connect 中是一条 iOS 应用的记录（像 iPhone 应用一样用应用的 ID 创建）：它的构建、版本和审核都属于这条记录的 iOS 平台，App Store 只在手表上提供它。

应用图标就是 `asuka.toml` 中的 `icon`，手表会把它显示成圆形：重要的内容请放在图标中间。

## 应用代码的更新

在两次 App Store 发布之间，手表应用可以像手机应用一样通过无线方式更新代码（[应用代码的更新](shipping.md#手机应用代码的更新)）：用 `asuka.toml` 中的 `[updates]` 构建，再为手表的构建发布更新。

```sh
asuka build --platform watchos --update      # 写入 dist/watchos/updates：签名后的更新及其清单
asuka publish --platform watchos --update    # 生成更新，并上传到 [updates] 指定的位置
```

手表的更新是独立的：手表构建的指纹来自手表自己的原生部分（它的宿主，以及为 watchOS 构建的包），手表上的 `platform/update` 读取的是更新源中的 `update-watchos-<指纹>.json`，所以 iPhone 和手表永远不会接收对方的更新。一处改动要发到哪些平台，就为每个平台各发布一次。更新保存在应用的目录（Application Support）中；和手机上一样，如果一个更新连续两次在启动后的最初几秒内结束，应用会回到构建自带的代码。

## 与 iPhone 应用一起

如果一个应用既有 iPhone 应用又有对应的手表应用，就在 `asuka.toml` 中写明：

```toml
[watch]
companion = true
```

这样 `asuka build --platform ios` 也会用同一份代码构建手表应用，并把它放进 iPhone 应用的 `Watch` 文件夹：只有一个 `.ipa`、App Store Connect 中只有一条记录，`asuka publish --platform ios` 会一并上传两者。安装 iPhone 应用后，配对的手表上会提示安装手表应用；手表应用也能单独从手表的 App Store 安装，iPhone 不在身边时同样可以运行。设备构建会用 `<id>.watchkitapp` 的 watchOS 描述文件（`[watch]` 的 `profile`，或最匹配的那个）为它签名，iPhone 应用则用自己的描述文件；`asuka build --platform watchos --simulator` 仍然只为模拟器单独构建手表应用。

两个应用通过 `platform/companion`（Apple 的 WatchConnectivity）互相通信，两边调用的是同样的函数：在 iPhone 上，对方是手表应用；在手表上，对方是 iPhone 应用。

```asuka check
import platform/companion

component Score {
  state score = 0
  state said = ""

  // What the other app sends; a message that waits gets an answer.
  task {
    for await m in companion.messages() {
      said = m.text
      if m.wantsReply { companion.reply(m, text: "{score}") }
    }
  }

  Column {
    Text("Theirs: {companion.context()}")
    Text(said)
    Button("Point") {
      on press {
        score += 1
        // The latest state, for the other app whenever it next runs.
        try? companion.setContext("{score}")
        // A message now, if the other app can answer.
        if companion.reachable() {
          said = (try? await companion.send("point")) ?? ""
        }
      }
    }
  }
}
```

- `companion.status()`（会被追踪）说明对方是否在：`paired`、`installed`，以及 `reachable`，即对方现在能否回复消息。`available()` 和 `reachable()` 是它的简写。
- `send(text)` 给对方发一条消息（对方必须可达），并返回对方的 `reply`。`queue(text)` 把消息留到对方下次运行时送达，即使这个应用已经退出，也会按顺序送到。`messages()` 读取这两种消息，包括在第一次读取之前就已到达的。
- `setContext(text)` 为对方保存这一端的最新状态，对方用 `context()`（会被追踪）读取：只有最新的一份会送到。
- `sendFile(path, metadata:)` 在后台发送文件；`files()` 给出收到的文件，它们已被移到应用的目录里。

消息是文本：要传的内容请用 `std/json` 编码。在没有配对设备的环境里（iPad、Android、桌面端），`available()` 为 false，调用会抛出 `CompanionError.unavailable`；测试中可以用 `companion.fake()` 装一个假的对方。要在模拟器上同时试用两个应用，先把一台手表模拟器和一台 iPhone 模拟器配对（`xcrun simctl pair`，两台都要启动），再分别安装两个应用的模拟器构建。

## 复杂功能

手表上的小组件就是它的复杂功能：显示在表盘上，也显示在智能叠放中。它们就是 `asuka.toml` 中的 `widgets`，取手表支持的尺寸：`circular`、`rectangular`、`inline`（在 iPhone 上是锁屏的尺寸），以及手表独有的 `corner`：

```toml
[[widgets]]
kind = "steps"
name = "Steps"
description = "Today's steps"
sizes = ["small", "circular", "rectangular", "corner"]
```

手表应用用 `platform/widget` 填充它们，就像 iPhone 应用填充自己的小组件一样：在手表应用的代码里调用 `widget.set("steps", widget.Entry(...))`，内容就会显示在手表的表盘上。`corner` 会显示 `symbol` 和 `value`，或者 `progress` 的仪表，沿着屏幕边缘弯曲排列；应用自己的布局（`layout`、`sizes[.corner]`）在任何尺寸下都能绘制。没有手表尺寸的小组件只属于 iPhone，`corner` 则只属于手表。

CLI 会把手表的小组件扩展（WidgetKit 的，ID 为`<手表应用的 ID>.widgets`）构建进手表应用，并让两者共享一个 App Group。设备构建会用 `<id>.watchkitapp.widgets` 的 watchOS 描述文件（`[watch]` 的 `widgetProfile`，或最匹配的那个）为它签名，两个描述文件都需要开启 App Groups 功能。

## 手表上显示什么

只要 watchOS 有对应的控件，每个元素用的就是手表自己的控件：

| 组件 | 在 Apple Watch 上 |
|---|---|
| `Text`、`RichText` | SwiftUI 的文字，使用手表的系统字体和用户设置的字号；富文本保留样式和链接颜色，但不显示内嵌组件 |
| `Button` | 手表的按钮：圆角胶囊形状，`variant: .filled` 时用强调色填充；带有自己 `background` 的 `.plain` 按钮只显示它的标签 |
| `Toggle` | 手表的开关 |
| `Slider` | 手表的滑块，两端是减号和加号；获得焦点时可以用数码表冠调节 |
| `Stepper` | 手表的步进器，数值位于减号和加号之间 |
| `TextField` | 点按后打开手表的文字输入（听写、随手写、键盘） |
| `Picker`、`Wheel`、`Segmented` | 用数码表冠转动的滚轮 |
| `DatePicker` | 手表的日期和时间选择器 |
| `Image`、`Symbol` | 图片、SF Symbol |
| `Progress`、`ProgressRing`、`Activity` | 进度条、圆环、加载指示器 |
| `Scroll`、`List` | 用数码表冠滚动的滚动视图；`List` 的每一行放在手表的圆角底板上（`appearance: .bare` 时按应用自己的样子绘制） |
| `Stack` | 手表的导航：页面标题在顶部，有返回按钮，从边缘轻扫可以返回 |
| `Tabs` | 用数码表冠从上到下翻动的页面 |
| `Sheet`、`Popover` | 覆盖整个屏幕的表单，可以用关闭按钮或轻扫关闭 |
| 提醒框、操作表 | 手表的提醒框和确认对话框 |
| 页面的栏按钮 | 第一个按钮，位于顶部的后缘角落 |

需要更大屏幕、指针或另一只手操作的东西不会显示：`WebView`、`Video`、`Canvas`、`Shader` 和 `ColorPicker` 会占据它们的空间，但什么也不画；菜单、轻扫操作、下拉刷新、拖放、选择文字、捏合、旋转、工具提示、按键和键盘焦点都不会发生。

## 适配手腕

在 Apple Watch 上 `environment().watch` 为 true。人们只是瞥一眼手表：把最重要的那一件事大大地显示出来，其余的交给手机。安全区域会让应用避开手表的圆角和时间显示，就像在手机上避开刘海一样。

```asuka check
component Today(steps: int, goal: int) {
  let watch = environment().watch

  SafeArea {
    gap: if watch { 4 } else { 12 }
    padding: if watch { 4 } else { 16 }
    Text("{steps}") { font: if watch { .title.bold() } else { .largeTitle.bold() } }
    Text("of {goal} steps") { color: .secondaryLabel }
    if !watch {
      Text("Walk a little more each day: the goal follows you.") { font: .footnote }
    }
  }
}
```

## Apple Watch 上的包

标准库的包都能在手表上运行，带原生代码的包只要 watchOS 有它们调用的功能也能运行：`std/http`（URLSession）、`std/sqlite` 和 `std/store`。`std/websocket` 不行：除了音频流之外，watchOS 不允许应用使用 WebSocket 和其他直接连接，所以它在手表上会抛出 `unsupported`。

平台 SDK 中，手表具备相应服务的包都会为手表构建：`platform/device`（设备信息、手腕上的触感、`openURL`）、`secure`、`biometric`、`health`、`location`、`audio`、`speech`、`nowplaying`、`network`、`purchase`、`appleauth`、`notify`、`push`、`crash`、`widget`（复杂功能）和 `companion`。[在 Apple Watch 上](device.md#在-apple-watch-上)列出了每个包和 iPhone 上的不同之处。其余的包在手表上会抛出 `unsupported`，和任何缺少相应功能的平台一样：请检查 `environment().watch`，在手表上不用它们。

## watchOS 上的原生包

原生包的 Swift 只有在 `Package.swift` 的平台列表中列出了 watchOS（或者一个平台都没列）时，才会为 watchOS 构建：

```swift
platforms: [.iOS("16.0"), .watchOS("10.0"), .macOS("11.0")],
```

没有列出 watchOS 的包不会进入手表的构建，它的模块在手表上会抛出 `unsupported`（[原生代码](native.md#ios-和-macosswift)）。在手表与 iOS 不同的地方，包的 Swift 用 `#if os(watchOS)` 区分：手表没有 UIKit 视图，所以包的视图（`//asuka:view`）在那里提供的是 SwiftUI 视图（`AnyView`），颜色也是 SwiftUI 的。`Package.swift` 旁边的 `watchOS.entitlements` 写明它对手表应用的权限要求。
