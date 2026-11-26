# 电视

Asuka 应用可以运行在 Apple TV 和 Android TV（包括 Google TV）上，使用的是电视自己的原生控件，就像在手机上使用手机的控件一样。电视没有触摸屏：遥控器把焦点从一个视图移到另一个视图，按下中间的确认键就会按下获得焦点的视图。这些都由平台自己的焦点引擎完成，作用在应用的视图上：tvOS 上是 UIKit 的焦点引擎，Android 电视上是 Android 的。

电视上跑的是同一份代码。Apple TV 的应用由 iOS 应用的代码构建而成，所以在那里 `@platform == .iOS`，和 iPad 一样；Android TV 上跑的就是 Android 应用，手机和电视共用一个构建。`environment().tv` 会告诉你应用正运行在电视上，方便你为隔着一个房间观看的距离把界面做得更大、更简洁。

## Apple TV

```sh
asuka dev --platform tvos .                # 在 Apple TV 模拟器上运行，保存即重新加载
asuka build --platform tvos --simulator .  # 用于模拟器的发布版 .app
asuka build --platform tvos .              # 用于 Apple TV 设备的 .ipa，位于 dist/tvos
```

`asuka dev --platform tvos` 会在 Apple TV 模拟器上运行应用：优先使用已经启动的那台，否则启动一台 Apple TV 4K（用 `--device` 可以指定其他模拟器）。tvOS 模拟器随 Xcode 的 tvOS 平台一起安装：在 Xcode › Settings › Components 中安装，或者运行 `xcodebuild -downloadPlatform tvOS`。在模拟器里，方向键就是遥控器的方向键，Return 是中间的确认键，Escape 是 Menu（返回）键；选择 Window › Show Apple TV Remote 可以显示遥控器本身。

`asuka build --platform tvos` 会用该应用 ID 对应的 tvOS 描述文件签名（可以用 `--profile` 指定；默认是 App Store 描述文件，也可以用 `--distribution` 选择），就像 iOS 构建使用 iOS 描述文件一样。用 Transporter 把 `.ipa` 上传到 App Store Connect，作为同一条应用记录（或另一条）里的 Apple TV 应用。应用要求 tvOS 17 及以上。

Apple TV 上的应用图标是 5:3 的分层图像：Asuka 用 `asuka.toml` 的 `icon` 生成它，把图标放在其角落颜色的背景中间，两层分开，这样焦点移到图标上时 tvOS 会让它们倾斜出视差效果。顶部栏（top shelf）的图片（应用图标在主屏幕第一排时，显示在它上方的图片）也用同样的方式生成。`[tv]` 的 `banner` 可以为两者提供你自己的图片，`background` 则指定图标背后的颜色（[asuka.toml](asuka-toml.md#tv)）。

tvOS 没有的东西，tvOS 构建里也就没有：分享目标（share target）、小组件和实时活动（Live Activities）、App Shortcuts、推送通知的改写；以及网页视图（tvOS 没有 WebKit）、通知、照片图库、通讯录、日历和邮件编写界面。调用它们的包在那里会抛出 `unsupported`，和任何缺少这些功能的平台一样；请检查 `environment().tv`，在电视上不用它们。

标准库中带原生代码的包（`std/http`、`std/websocket`、`std/sqlite`、`std/store`、`std/svg`）在 Apple TV 上的表现和在 iPhone 上一样。平台 SDK 的包（`platform/...`）目前只为 iOS 和 Mac 构建：在 Apple TV 上，它们都会抛出 `unsupported`。

## Android TV

只要 Android 应用声明自己同时也是电视应用，Android TV 就能运行它：

```toml
[tv]
android = true
```

这样构建出来的应用就具备 Android TV 和 Google Play 需要的东西：电视启动器的 intent（`LEANBACK_LAUNCHER`）、声明不需要触摸屏和 Leanback 库（`uses-feature … required="false"`，于是 Google Play 会把同一个应用同时提供给手机和电视），以及电视启动器显示的横幅（banner），尺寸为 320 × 180，由 `icon`（或 `tv.banner`）生成。

要试一试，可以在 Android Studio 的 Device Manager 里创建一个 Android TV 模拟器（选择电视设备，以及 Android TV 或 Google TV 系统镜像），然后运行 `asuka dev --platform android --device <模拟器名称> .`。模拟器里的方向键就是 D-pad，Return 是中间键，Escape 是返回键。

## 遥控器

两种电视上都一样：

- **焦点**在能获得焦点的视图之间移动：控件和输入框，以及能被按下（`on press`）、能被调节（`on adjust`）或设置了 `focusable` 的视图。平台会像在它自己的应用里那样，选出遥控器所指方向上的那个视图。
- **获得焦点的视图会显示出来**：tvOS 的控件有自己的焦点效果；应用自己的视图会稍稍放大并投下阴影，就像 tvOS 的卡片一样。在 Android TV 上，视图会显示自己的获得焦点状态（波纹效果的、按钮的），同时也有同样的放大效果。为 `focusVisible` 设置了样式的视图会改用自己的样式，`hideFocusRing: true` 则会去掉焦点效果（[无障碍](accessibility.md#能获得焦点的视图)）。
- **确认键**（中间键，或者点按 Siri Remote 的触控板）会按下获得焦点的视图：触发 `on press`、按钮的动作、开关的切换。按住时，视图会处于 `pressed` 状态。
- **返回键**（Android TV 的返回键，Siri Remote 的返回键或 Menu 键）的行为和手机上的返回一样：关闭弹出面板、从导航栈中弹出页面，`onBack` 可以接管它（[导航](navigation.md#自己处理返回)）。在应用的第一个页面上，它会离开应用，回到电视的主屏幕。
- **按键**：视图的 `keys` 会先于焦点移动拿到遥控器的方向（`up`、`down`、`left`、`right`），还能拿到 `playpause`，也就是遥控器的播放/暂停键：用于播放器的播放和暂停、游戏的操作等（[按键](motion.md#按键)）。

```asuka check
component Player(title: string) {
  state playing = false

  View {
    padding: if environment().tv { 48 } else { 16 }
    gap: 16
    keys: [Shortcut(key: "playpause", run: () => { playing = !playing })]
    Text(title) { font: .title }
    Button(if playing { "Pause" } else { "Play" }) {
      variant: .filled
      on press { playing = !playing }
    }
  }
}
```

## 电视上的控件

电视有的控件，用的就是电视自己的控件。电视没有的：

| 组件 | Apple TV | Android TV |
|---|---|---|
| `Toggle` | 一个带勾号的按钮（关闭时是空心圆），按确认键切换；tvOS 没有开关控件 | Material 的开关 |
| `Slider` | 一条进度条，获得焦点时用左右键每次调节其范围的二十分之一；tvOS 没有滑块 | Material 的滑块，用左右键调节 |
| `Stepper` | 一个减号和一个加号，获得焦点时用左右键调节 | Material 的按钮 |
| `DatePicker` | 显示日期（或时间），用左右键每次调一天（一分钟）；tvOS 没有日期选择器 | Material 的日期和时间选择器 |
| `Picker`、`Wheel` | 一个按钮，通过它的菜单来选择 | 与手机上相同 |
| `ColorPicker` | 显示颜色，只能由应用修改 | 与手机上相同 |
| `TextField` | 在 tvOS 的全屏键盘上输入的输入框；多行输入框只占一行 | 与手机上相同，使用电视的键盘 |
| `WebView` | 无：tvOS 没有 WebKit | Android 的 `WebView` |
| `Sheet` | 占满整个屏幕，没有停靠高度（detent） | 与手机上相同 |
| `Popover` | 以弹出面板的形式显示 | 与手机上相同 |
| 页面的 `toolbar` | 它的按钮排在导航栏的开头 | 与手机上相同 |
| 页面的 `search` | 搜索框作为导航栏的标题 | 与手机上相同 |

滑动操作、下拉刷新、拖放、选择文字、双指缩放、旋转、工具提示和触感反馈都需要触摸、指针或者把设备握在手里：在电视上不会出现。

## 适配电视

在 Apple TV 和 Android TV 上，`environment().tv` 为 true。人们隔着一个房间看电视，电视也没有触摸屏，所以电视上的页面通常显示更少、更大的内容，排成让焦点在其中移动的行和网格，并且省掉需要打字的部分：

```asuka check
component Shelf(items: [string]) {
  let tv = environment().tv

  Scroll {
    horizontal: true
    Row {
      gap: if tv { 48 } else { 12 }
      padding: if tv { 64 } else { 16 }
      for item in items key item {
        View {
          padding: if tv { 32 } else { 12 }
          radius: 12
          background: .secondaryBackground
          on press { print(item) }
          Text(item) { font: if tv { .title2 } else { .body } }
        }
      }
    }
  }
}
```

安全区域让应用避开电视的边缘（有些电视会裁掉边缘）：tvOS 会在 `environment()` 的边距中报告它。

## tvOS 上的原生包

只要原生包的 `Package.swift` 在 platforms 中列出了 tvOS（或者一个平台都没列），它的 Swift 就会为 tvOS 构建：

```swift
platforms: [.iOS("16.0"), .tvOS("17.0"), .macOS("11.0")],
```

没有列出 tvOS 的包不会进入 tvOS 构建，它的模块在那里会抛出 `unsupported`，和包完全没有代码的平台一样（[原生代码](native.md#ios-和-macosswift)）。在 tvOS 的 UIKit 与 iOS 不同的地方，包的 Swift 用 `#if os(tvOS)` 区分；`Package.swift` 旁边的 `tvOS.entitlements` 写明它对电视应用的权限要求。
