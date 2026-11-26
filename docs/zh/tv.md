# 电视

Asuka 应用可以运行在 Apple TV 和 Android TV（包括 Google TV）上，使用的是电视自己的原生控件，就像在手机上使用手机的控件一样。电视没有触摸屏：遥控器把焦点从一个视图移到另一个视图，按下中间的确认键就会按下获得焦点的视图。这些都由平台自己的焦点引擎完成，作用在应用的视图上：tvOS 上是 UIKit 的焦点引擎，Android 电视上是 Android 的。

电视上跑的是同一份代码。Apple TV 的应用由 iOS 应用的代码构建而成，所以在那里 `@platform == .iOS`，和 iPad 一样；Android TV 上跑的就是 Android 应用，手机和电视共用一个构建。`environment().tv` 会告诉你应用正运行在电视上，方便你为隔着一个房间观看的距离把界面做得更大、更简洁。

## Apple TV

```sh
asuka dev --platform tvos .                # 在 Apple TV 模拟器上运行，保存即重新加载
asuka build --platform tvos --simulator .  # 用于模拟器的发布版 .app
asuka build --platform tvos .              # 用于 Apple TV 设备的 .ipa，位于 dist/tvos
asuka publish --platform tvos .            # 构建并上传到 TestFlight
```

`asuka dev --platform tvos` 会在 Apple TV 模拟器上运行应用：优先使用已经启动的那台，否则启动一台 Apple TV 4K（用 `--device` 可以指定其他模拟器）。tvOS 模拟器随 Xcode 的 tvOS 平台一起安装：在 Xcode › Settings › Components 中安装，或者运行 `xcodebuild -downloadPlatform tvOS`。在模拟器里，方向键就是遥控器的方向键，Return 是中间的确认键，Escape 是 Menu（返回）键；选择 Window › Show Apple TV Remote 可以显示遥控器本身。

`asuka build --platform tvos` 会用该应用 ID 对应的 tvOS 描述文件签名（可以用 `--profile` 指定；默认是 App Store 描述文件，也可以用 `--distribution` 选择），就像 iOS 构建使用 iOS 描述文件一样。iPhone 应用的描述文件不能给 Apple TV 应用签名，所以要先在 developer.apple.com 上另建一个（Profiles › + › tvOS）。应用要求 tvOS 17 及以上。

`asuka publish --platform tvos` 会构建 `.ipa` 并上传到 App Store Connect，和 `--platform ios` 一样，使用同一个密钥和同样的选项（`--groups`、`--review`、`--notes`、`--file`），见[发布到应用商店](shipping.md#发布到应用商店)。Apple TV 应用是 iPhone 应用那条记录下的一个平台，共用 bundle ID 和 App 内购买项目：请先在 App Store Connect 中给这条记录加上 tvOS（在应用页面选择 Add Platform › tvOS）。

Apple TV 上的应用图标是 5:3 的分层图像：Asuka 用 `asuka.toml` 的 `icon` 生成它，把图标放在其角落颜色的背景中间，两层分开，这样焦点移到图标上时 tvOS 会让它们倾斜出视差效果。顶部栏（top shelf）的图片（应用图标在主屏幕第一排时，显示在它上方的图片）也用同样的方式生成。`[tv]` 的 `banner` 可以为两者提供你自己的图片，`background` 则指定图标背后的颜色（[asuka.toml](asuka-toml.md#tv)）。

tvOS 没有的东西，tvOS 构建里也就没有：分享目标（share target）、小组件和实时活动（Live Activities）、App Shortcuts、推送通知的改写；以及网页视图（tvOS 没有 WebKit）、会显示出来的通知（只有应用图标上的角标）、照片图库和照片选择器、通讯录、日历和邮件编写界面。调用它们的包在那里会抛出 `unsupported`，和任何缺少这些功能的平台一样；请检查 `environment().tv`，在电视上不用它们。哪些包能用，见 [Apple TV 上的包](#apple-tv-上的包)。

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

## Apple TV 上的包

标准库的包在 Apple TV 上的表现和在 iPhone 上一样，带原生代码的那些（`std/http`、`std/websocket`、`std/sqlite`、`std/store`、`std/svg`）也是如此。平台 SDK 的包中，凡是 tvOS 具备相应服务的，都能在 Apple TV 上运行，只是有些包的功能比在手机上少：

| 包 | 在 Apple TV 上 |
|---|---|
| `platform/analytics` | 和 iPhone 上一样（Firebase，需要配置 `ios.googleServices`） |
| `platform/appearance`、`platform/audio`、`platform/crash`、`platform/document`、`platform/icloud`、`platform/imageedit`、`platform/network`、`platform/periodic`、`platform/speech`、`platform/splash`、`platform/tracking`、`platform/vision` | 和 iPhone 上一样 |
| `platform/appicon` | 和 iPhone 上一样：`alternateIcons` 中的每个图标也会像应用图标那样做成分层图标 |
| `platform/appleauth` | 和 iPhone 上一样：“通过 Apple 登录”的面板和按钮 |
| `platform/bluetooth` | 只能作为中心设备（central）：扫描、连接、读写；`serve` 和 `advertise` 会抛出 `unsupported` |
| `platform/device` | 有 `info`、`openURL`、`openSettings`、`keepAwake`；没有浏览器、照片选择器和文档选择器（抛出 `unsupported`），也没有剪贴板：读不到任何内容，复制会抛出 `unsupported` |
| `platform/googleauth` | 使用 `signInWithCode`：电视上显示一个网址和一个代码，用户在手机或电脑上输入，这就是 Google 为电视提供的登录方式；`signIn` 会抛出错误 |
| `platform/integrity` | App Attest，需要真实的 Apple TV（模拟器上不可用） |
| `platform/js` | 用 JavaScriptCore 运行，没有 WebKit：语言本身和内置对象都有，计时器也有，但没有 DOM 和 `fetch` |
| `platform/location` | 只能在使用期间定位，每次给出一个位置：`positions()` 每五分钟重新获取一次；没有后台更新，区域监测由包在应用运行期间自行计算 |
| `platform/maps` | Apple 地图，不能旋转或倾斜 |
| `platform/notify` | 只有角标：`request` 申请角标权限，`setBadge` 设置角标；`schedule` 和 `show` 会抛出 `unsupported` |
| `platform/nowplaying` | 和 iPhone 上一样：控制中心和遥控器的播放、暂停键 |
| `platform/power` | 没有电池（`battery` 为 false）：只有设备的热状态 |
| `platform/purchase` | 和 iPhone 上一样使用 StoreKit；`manageSubscriptions`、`requestRefund` 和 `redeemCode` 会抛出 `unsupported`（tvOS 没有这些面板，订阅在“设置”里管理） |
| `platform/push` | 有设备令牌；推送只会设置角标，或者作为数据交给应用（`received`），不会有被点开的推送 |
| `platform/screen` | 电视的屏幕；没有可设置的亮度 |
| `platform/secure` | 钥匙串；`protected` 的机密会得到 `SecureError.notAvailable`：Apple TV 没有密码、面容 ID 或触控 ID |

其余的包因为 tvOS 缺少相应能力，不会为 tvOS 构建：`platform/ads`（Google 的广告 SDK 不支持 tvOS）、`agerange`、`biometric`（`check` 会告诉你没有生物识别）、`calendar`、`camera` 和 `record`（Apple TV 自己没有摄像头和麦克风）、`compose`、`contacts`、`dialog`、`health`、`liveactivity`、`nfc`、`pay`、`pdf`、`photos`、`preview`、`print`、`review`（tvOS 没有评分提示）、`scan`、`sensors`、`sharesheet`、`shortcuts`、`transcribe`、`webauth`、`webview` 和 `widget`；还有桌面专用的包（`tray`、`dock`、`hotkey` 等），这一点和 iPhone 一样。调用它们会抛出 `unsupported`，或者回答没有这项功能（`biometric.check`）。

## tvOS 上的原生包


只要原生包的 `Package.swift` 在 platforms 中列出了 tvOS（或者一个平台都没列），它的 Swift 就会为 tvOS 构建：

```swift
platforms: [.iOS("16.0"), .tvOS("17.0"), .macOS("11.0")],
```

没有列出 tvOS 的包不会进入 tvOS 构建，它的模块在那里会抛出 `unsupported`，和包完全没有代码的平台一样（[原生代码](native.md#ios-和-macosswift)）。在 tvOS 的 UIKit 与 iOS 不同的地方，包的 Swift 用 `#if os(tvOS)` 区分；`Package.swift` 旁边的 `tvOS.entitlements` 写明它对电视应用的权限要求。
