# 设备与服务

Asuka 提供了一系列包，让应用用上视图之外的设备能力：相机、条码、图像内容识别、照片和文件、文件预览、打印和 PDF、邮件和短信编辑界面、定位、通知和推送、声音、正在播放（Now Playing）和录音、网络状态、共享的媒体库、后台任务、接收其他应用的分享、JavaScript、登录和生物识别、屏幕、地图、应用商店提供的服务、广告、崩溃报告，以及数据分析。本页逐一介绍这些包：它们能做什么、API 怎么用、会给应用带来哪些权限，以及在各个平台上的表现。

每个包在所有平台上都是同一套 API，iOS 和 Android 几乎支持其中的全部功能。在 macOS、Windows 和 Linux 上，Asuka 会用桌面系统自己的服务来实现各个包；在浏览器中则用 Web API。如果某个平台根本没有对应的能力，包的表现就和在一台缺少该功能的手机上一样：返回这个包的 `notAvailable` 错误，返回一个说明缺了什么的错误，或者调用了也什么都不做。每个包的小节都会说明它能做什么、在各平台上分别靠什么实现。每个桌面平台和浏览器也各有专门的一节（[在 Mac 上](#在-mac-上)、[在 Windows 上](#在-windows-上)、[在 Linux 上](#在-linux-上)、[在浏览器中](#在浏览器中)），介绍应用在那里需要做哪些配置，并用一张表列出它与 iOS 和 Android 的不同之处。

## 权限与用途说明

应用导入某个包时，会自动获得这个包所需的一切：Android 清单里的权限、iOS Info.plist 里的用途说明（purpose string），以及平台代码本身。没有导入的应用就不会带上这些，所以一个照片应用不会去申请麦克风权限。Mac 应用会得到同样的用途说明，以及它们所需的 entitlement；Windows、Linux 和 Web 则不需要声明任何东西（见下文各自的小节）。

| 包 | Android | iOS |
| --- | --- | --- |
| `platform/camera` | `CAMERA`、`RECORD_AUDIO` | `NSCameraUsageDescription`、`NSMicrophoneUsageDescription` |
| `platform/location` | `ACCESS_COARSE_LOCATION`、`ACCESS_FINE_LOCATION`、`ACCESS_BACKGROUND_LOCATION`、`FOREGROUND_SERVICE_LOCATION`、`RECEIVE_BOOT_COMPLETED`；这个包的前台服务及其广播接收器（用于后台定位更新和区域） | `NSLocationWhenInUseUsageDescription`、`NSLocationAlwaysAndWhenInUseUsageDescription`、`location` 后台模式；调用了 `requestPrecise` 的应用另有 `NSLocationTemporaryUsageDescriptionDictionary` |
| `platform/notify` | `POST_NOTIFICATIONS` | |
| `platform/network` | `ACCESS_NETWORK_STATE` | |
| `platform/library` | `WRITE_EXTERNAL_STORAGE`（Android 9 及更早版本） | `NSPhotoLibraryAddUsageDescription`；应用的 Documents 目录会出现在“文件”App 中 |
| `platform/background` | `FOREGROUND_SERVICE`、`FOREGROUND_SERVICE_DATA_SYNC`、`POST_NOTIFICATIONS`、`WAKE_LOCK`；一个前台服务 | |
| `platform/scan` | ML Kit 的条码模型（约 2 MB） | |
| `platform/vision` | ML Kit 的文字、人脸和条码模型（约 30 MB） | |
| `platform/photos` | `READ_MEDIA_IMAGES`、`READ_MEDIA_VISUAL_USER_SELECTED`；`READ_EXTERNAL_STORAGE`（Android 12 及更早版本） | `NSPhotoLibraryUsageDescription` |
| `platform/push` | Firebase Cloud Messaging、`POST_NOTIFICATIONS` | `aps-environment` entitlement；`remote-notification` 后台模式 |
| `platform/analytics` | Firebase Analytics（`INTERNET`、`ACCESS_NETWORK_STATE`、`WAKE_LOCK`、`AD_ID`）；`android.googleServices` 的设置 | Firebase Analytics，`ios.googleServices` 指定的文件，从应用启动时开始 |
| `platform/appleauth` | | 通过 Apple 登录（Sign in with Apple）的 entitlement |
| `platform/pay` | Google Pay 的库，启用它的 API，以及它的回调服务 | Apple Pay 的 entitlement，带有 `asuka.toml` 的 `applePay.merchantIds` |
| `platform/biometric` | `USE_BIOMETRIC` | `NSFaceIDUsageDescription` |
| `platform/record` | `RECORD_AUDIO`、`FOREGROUND_SERVICE_MICROPHONE`；一个前台服务 | `NSMicrophoneUsageDescription` |
| `platform/nowplaying` | `FOREGROUND_SERVICE`、`FOREGROUND_SERVICE_MEDIA_PLAYBACK`；一个前台服务 | `audio` 后台模式 |
| `platform/liveactivity` | `POST_NOTIFICATIONS`、`POST_PROMOTED_NOTIFICATIONS` | `NSSupportsLiveActivities` 和小组件扩展（仅限调用了它的应用） |
| `platform/transcribe` | `RECORD_AUDIO` | `NSSpeechRecognitionUsageDescription` |
| `platform/screen` | `DETECT_SCREEN_CAPTURE` | |
| `platform/tracking` | | `NSUserTrackingUsageDescription` |
| `platform/review` | Play 的应用内评价库 | |
| `platform/integrity` | Play Integrity | App Attest 的 entitlement |
| `platform/agerange` | Play Age Signals | Declared Age Range 的 entitlement |
| `platform/purchase` | Play Billing、`com.android.vending.BILLING` | StoreKit 2（仅限调用了它的应用）；不需要 entitlement |
| `platform/icloud` | （无：Android 上没有 iCloud） | 根据应用调用的功能，加上键值存储的 entitlement、iCloud Documents 和应用的容器、`NSUbiquitousContainers`（见 [iCloud](icloud.md)） |
| `platform/maps` | Google 的 Maps SDK | |
| `platform/periodic` | WorkManager | `fetch` 和 `processing` 后台模式；各任务的标识符 |
| `platform/sensors` | `ACTIVITY_RECOGNITION`（计步器） | `NSMotionUsageDescription`（计步器） |
| `platform/speech` | 文字转语音引擎的可见性声明（`<queries>`） | |
| `platform/contacts` | `READ_CONTACTS`（用于 `list`） | `NSContactsUsageDescription` |
| `platform/calendar` | `READ_CALENDAR`、`WRITE_CALENDAR`（用于 `calendars`、`events`） | `NSCalendarsFullAccessUsageDescription`、`NSCalendarsWriteOnlyAccessUsageDescription`、`NSCalendarsUsageDescription` |
| `platform/compose` | 处理 `mailto:` 和 `smsto:` 的应用的可见性声明（`<queries>`） | `LSApplicationQueriesSchemes` 中的 `mailto` |
| `platform/nfc` | `NFC`；`uses-feature android.hardware.nfc`（非必需）；`asuka.toml` 中 `nfc` 的过滤器（见 [NFC 标签](nfc.md)） | `NFCReaderUsageDescription`；Core NFC 的 entitlement；`asuka.toml` 中 `nfc` 的 AID 和 FeliCa 系统代码 |
| `platform/bluetooth` | `BLUETOOTH_SCAN`（`neverForLocation`）、`BLUETOOTH_CONNECT`、`BLUETOOTH_ADVERTISE`；调用了 `backgroundScan` 时还有后台扫描的接收器（见[蓝牙](bluetooth.md#构建时添加的内容)） | `NSBluetoothAlwaysUsageDescription`；声明了 `bluetooth.background` 时还有 `bluetooth-central`、`bluetooth-peripheral`（调用了 `backgroundScan` 时有 `bluetooth-central`），以及创建 iOS 要恢复的管理器的启动类 |

`platform/device`、`platform/audio`、`platform/splash`、`platform/imageedit`、`platform/widget`、`platform/js`、`platform/secure`、`platform/preview` 和 `platform/print` 不需要任何权限。

要在发布构建中使用某个 entitlement，必须先在应用的 App ID 上开启对应的 capability（在 developer.apple.com 的 Identifiers 页面），这样描述文件（provisioning profile）里才会包含它。如果描述文件缺了某一项，`asuka build --platform ios` 会停下来并告诉你缺的是什么；模拟器构建则所有 entitlement 都有。

Asuka 写入的用途说明是通用的（"To take a photo."）。iOS 和 macOS 向用户请求权限时会显示这段文字，所以最好在 `asuka.toml` 的 `ios.usage` 里写上你自己的说明：

```toml
[ios.usage]
NSCameraUsageDescription = "To photograph your receipts."
NSLocationWhenInUseUsageDescription = "To find stores near you."
```

每个包都是在第一次真正需要时才询问用户，而不是在应用启动时。需要等待的调用（等用户回应、等定位结果、等平台处理）都是 `async` 的：在事件处理函数、task 或另一个 async 函数里 `await` 它们，等待期间应用照常运行（见[异步](language/async.md#异步)）。平台上随时间变化的东西（网络、某项权限、账户）则会被追踪：它要么是一个供绑定读取的函数，要么是一个供 task 读取的 `Stream`。

## 在 Mac 上

Mac 应用（无论是 `asuka dev` 还是 `asuka build` 生成的）的 Info.plist 里有同样的用途说明（定位的那条还会额外写成 `NSLocationUsageDescription`）。它的签名带有 hardened runtime 为这些请求所需的 entitlement（相机、麦克风、定位、通讯录、日历、照片）；缺了它们，Developer ID 构建的应用就拿不到相应的权限。推送的 `aps-environment`、通过 Apple 登录、App Attest、Apple Pay 和 iCloud 这几项 entitlement 只能由描述文件授予：对于用到它们的应用，`asuka build` 会嵌入一个描述文件（优先取 `asuka.toml` 的 `mac.profile`，其次是 `ASUKA_MAC_PROFILE`，最后是本机已安装的最佳匹配项）。没有描述文件时，应用照样能构建，CLI 会告诉你哪些功能将无法使用：在 Mac 上，注册推送和通过 Apple 登录都会失败，App Attest 会报告不受支持。`asuka dev` 使用 ad hoc 签名，不带描述文件，所以开发时也是同样的情况。Mac App Store 构建（`--appstore`）运行在沙盒中，可以访问网络以及用户选择的文件。详见[打包与发布](shipping.md)。

这些包在 macOS 上的工作方式与 iOS 相同，用的也是同一套框架（AVFoundation、PhotoKit、Core Location、MapKit、Vision、Speech、UserNotifications、AuthenticationServices），只有下面这些 Mac 与之不同的地方例外：

| 包 | 在 Mac 上 |
| --- | --- |
| `platform/device` | `pickPhotos` 和 `pickFiles`：一个以 sheet 形式附着在窗口上的打开面板（无需权限），选中的文件会复制到应用的缓存目录。`browse` 在用户的浏览器中打开页面。`info`：`model` 为 `"Mac"`，`system` 为 `"macOS"` |
| `platform/camera` | `take()`：在窗口上弹出一个面板，带实时预览、“拍照”和“取消”按钮（AppKit 没有相机界面）。`camera.View` 的“后置”摄像头就是内置摄像头（Mac 没有后置摄像头），或者外接的那个；在其中扫码需要 macOS 13 |
| `platform/record` | 没有音频会话：其他应用的声音会照常播放；`background` 不起作用，因为 Mac 应用在后台本来就能录音 |
| `platform/nowplaying` | 和 iOS 一样使用 `MPNowPlayingInfoCenter` 和 `MPRemoteCommandCenter`：对应控制中心和菜单栏里的“正在播放”、键盘上的媒体键以及 AirPods。Mac 不需要音频会话，也不需要后台模式 |
| `platform/contacts`、`platform/calendar` | `add` 会在窗口上弹出一个警告框来确认（macOS 没有新建联系人或日程的界面），然后保存；保存前会先请求访问权限 |
| `platform/photos` | 和 iOS 一样使用 PhotoKit；Mac 没有“有限访问”的照片库，所以 `manageLimited` 什么也不做 |
| `platform/library` | 照片和视频存入“照片”App；`.downloads` 和 `.music` 分别存到用户的 `~/Downloads` 和 `~/Music` 下指定的文件夹 |
| `platform/biometric` | Touch ID 的提示框，或者输入用户密码（设置了 `noPasscode` 时除外） |
| `platform/push` | 和 iOS 一样使用 APNs。需要描述文件（见上文） |
| `platform/appleauth` | 和 iOS 一样，使用 Apple 提供的按钮和面板。需要描述文件（见上文） |
| `platform/pay` | 和 iOS 一样，使用 Apple Pay 的支付面板和按钮：带触控 ID 的 Mac 可以直接付款，否则用附近的 iPhone 或 Apple Watch 付款。需要描述文件（见上文） |
| `platform/integrity` | 和 iOS 一样使用 App Attest。需要描述文件（见上文）：没有的话 `supported()` 为 false |
| `platform/background` | 任务会阻止 App Nap 拖慢应用；没有通知，所以 `update` 不起作用 |
| `platform/periodic` | `NSBackgroundActivityScheduler`：任务只在应用运行期间执行（系统不会为了它们启动应用）；需要接通电源的任务，会在 Mac 使用电池时等待 |
| `platform/screen` | `lock`、`setBars` 和 `setBrightness` 什么也不做；`preventCapture` 让应用的窗口不出现在截图和录屏中；`screenshots()` 不会报告任何截图 |
| `platform/appicon` | `set` 在应用运行期间更换程序坞里的图标，下次启动时也会换上；访达里仍是应用自己的图标 |
| `platform/transcribe` | 只在本机识别；应用的 bundle 需要它的用途说明，这一项由 `asuka build` 写入 |
| `platform/sensors` | Mac 没有运动传感器：`available` 为 false，各个流和 `steps` 都会抛出 `SensorError.notAvailable` |
| `device.haptic` | 触控板的反馈（需要 Force Touch 触控板）：`.selection` 对应“对齐”反馈，其他类型对应“级别变化”反馈 |
| `platform/widget`、`platform/liveactivity`、`platform/agerange` | Mac 上不支持：`widget.set` 什么也不做，`liveactivity.start` 抛出 "liveactivity: not available"，`agerange.request` 抛出 `AgeRangeError.notAvailable` |
| `platform/purchase` | StoreKit 2，编译进应用的 Swift 库；导入了这个包的应用，构建时会生成这个库（见[应用内购买](purchases.md)） |
| `platform/icloud` | 和 iOS 一样，编译进应用的 Swift 库。需要带 iCloud 的描述文件（见上文）：没有的话报错 "icloud: not available"（见 [iCloud](icloud.md)） |
| `platform/splash` | Mac 应用没有启动屏：`hold` 和 `hide` 什么也不做 |
| `platform/analytics` | 和 iOS 一样使用 Firebase，用同一个 `GoogleService-Info.plist`（与应用 ID 相同的 Apple 应用） |
| `onShare`、快捷操作 | 仅限 iOS 和 Android：Mac 应用不会出现在其他应用的分享菜单里，`asuka.toml` 的 `shortcuts` 也不会被添加 |

## 在 Windows 上

Windows 应用不需要声明任何东西：`asuka build --platform windows` 不会写入任何 capability，Windows 也不要求桌面应用申请权限。不过，“设置”里的隐私开关（隐私和安全性）对相机、麦克风和定位依然有效：如果某个开关是关着的，相机和录制会抛出各自的 `notAllowed` 错误，定位则抛出 `LocationError.denied`。

这些包在 Windows 上通过系统自己的服务实现（Media Foundation、WIC、WinRT、Windows Hello、SAPI、凭据管理器），只有下面这些 Windows 与之不同的地方例外；Windows 完全不具备的能力，包的表现就和在缺少该功能的设备上一样：

| 包 | 在 Windows 上 |
| --- | --- |
| `platform/device` | `pickPhotos` 和 `pickFiles`：窗口上弹出的打开对话框，选中的文件会复制到应用的缓存目录。`browse` 在用户的浏览器中打开页面。剪贴板里的图片是 PNG。`keepAwake` 让显示器保持常亮。`info`：`model` 为 `"PC"`，`system` 为 `"Windows"`，`systemVersion` 为 `"10.0.<build>"` |
| `platform/secure` | 凭据管理器中用户的普通凭据，名为 `<app>/<key>`（每个值最多 2560 字节） |
| `platform/camera` | Media Foundation 的捕获引擎。`take()`：窗口上弹出一个对话框，带实时预览、“拍照”和“取消”按钮（对应 Enter 和 Escape 键）。`camera.View` 选用 Windows 标为前置或后置的摄像头，找不到就用第一个；设置了 `scan` 时，它每秒取四帧，用 Asuka 自带的识别器读取其中的条码（与 `scan.file` 的做法相同）。“设置”中的相机开关关闭时：`CameraError.notAllowed` |
| `platform/record`（视频） | MPEG-4 封装（`.mp4`）的 H.264 和 AAC，使用默认麦克风，除非设置了静音 |
| `platform/audio` | MCI（DirectShow 的播放功能）；URL 会先下载到应用的缓存目录 |
| `platform/record`（录音） | 默认麦克风，44.1 kHz 的 AAC（语音为单声道，音乐为立体声）；`background` 不起作用 |
| `platform/nowplaying` | 应用窗口的 SystemMediaTransportControls（没有窗口的应用会用一个自己的隐藏窗口）：对应 Windows 10 的音量浮出面板、Windows 11 快速设置里的媒体控件、锁屏以及键盘上的媒体键。跳过操作对应它的快进和快退按钮；它没有切换操作（播放/暂停键发送的是播放或暂停） |
| `platform/speech` | SAPI 的语音，即系统已安装的那些（设置 > 时间和语言 > 语音） |
| `platform/network` | 网络列表管理器（Network List Manager）：Wi-Fi、蜂窝网络或有线网络；按流量计费的连接为 `metered`，接近或超出流量上限时为 `constrained` |
| `platform/location` | WinRT 的 Geolocator。`placeName` 查询 OpenStreetMap 的 Nominatim（Windows 自带的地理编码器需要 Bing Maps 密钥）：每秒最多发一次请求，查询结果保存在应用的缓存目录中。区域监控用 `GeofenceMonitor`，仅在应用运行期间有效；后台定位更新根据位置计算 |
| `platform/biometric` | Windows Hello 的提示框；`check()` 返回的类型就是传感器的类型；Hello 总会提供 PIN 选项，取消按钮的文字也由它自己决定 |
| `platform/webauth` | 在应用窗口之上打开一个内嵌 WebView2 的窗口（使用应用的 Web 配置文件；设置 `ephemeral` 时使用一个用完即弃的配置文件） |
| `platform/js` | 一个从不显示的 WebView2 |
| `platform/contacts`、`platform/calendar` | Windows 只向打包（packaged）应用开放通讯录和日历：`request` 返回 `.denied`；`add` 会在用户的联系人或日历应用中打开一个 vCard 或日程文件，并返回 false（Windows 不会告知是否已保存） |
| `platform/photos` | “图片”文件夹（无需申请权限）；截图就是其中“屏幕截图”文件夹里的图片 |
| `platform/library` | 用户的“下载”“视频”“音乐”和“图片”文件夹下指定的文件夹 |
| `platform/vision` | Windows 的 OCR（支持哪些语言取决于“设置 > 时间和语言”中安装了哪些 OCR 语言）及其人脸检测器；条码的识别方式与 `platform/scan` 相同 |
| `platform/scan` | Windows 没有供应用使用的条码识别器，因此用的是 Asuka 自带的识别器：能识别 QR 码、Data Matrix、EAN-13 和 EAN-8、UPC-A 和 UPC-E、Code 128 和 Code 39（不支持 Aztec、PDF417、Code 93、Codabar 和 ITF） |
| `platform/imageedit` | 用 Windows 的编解码器读取图片（装了相应扩展时支持 HEIC 和 WebP）；视频帧通过 Media Foundation 获取 |
| `platform/maps` | Windows 没有供桌面应用使用的地图控件（它自带的那个需要 Bing Maps 密钥），所以由 Asuka 自己绘制 OpenStreetMap 的瓦片：每次下载两张，在应用的缓存目录中保留一周，地图上标注“© OpenStreetMap contributors”；支持拖动、滚轮或双指缩放、双击，以及 + 和 - 键。`.satellite` 和 `.hybrid` 显示的也是街道图（没有免费的卫星影像），`showsUser` 不显示任何东西，这两点与 Linux 相同 |
| `platform/background` | 任务会阻止电脑进入睡眠；没有通知，所以 `update` 不起作用 |
| `platform/periodic` | 应用运行期间用定时器执行（系统不会为了它们启动应用）；需要接通电源的任务，会在电脑使用电池时等待 |
| `platform/screen` | `lock`、`setBars` 和 `setBrightness` 什么也不做；`preventCapture` 让应用的窗口不出现在截图和录屏中；`screenshots()` 不会报告任何截图 |
| `platform/appicon` | `set` 更换应用窗口和任务栏按钮的图标，下次启动时也会换上；开始菜单和可执行文件仍是应用自己的图标 |
| `platform/tracking` | 没有提示框：“设置”中的广告 ID 开着就是允许，否则就是拒绝 |
| `platform/push`、`platform/widget`、`platform/review`、`platform/integrity` | 这些能力只属于打包（应用商店）应用，或者依赖 Apple 和 Google 的服务：`push.register` 会抛出错误，`widget.set` 什么也不做，`review.request` 不显示任何东西，`integrity.supported()` 为 false |
| `platform/liveactivity` | 这是手机的能力：`enabled` 为 false，`start` 抛出 "liveactivity: not available" |
| `platform/purchase` | Microsoft Store，仅限从商店安装的应用；否则 `available()` 为 false，其余调用抛出 `PurchaseError.notAvailable`（见[应用内购买](purchases.md)） |
| `platform/pay` | Windows 上没有能为应用付款的钱包：`available` 返回 `.none`，`pay` 抛出 `PayError.notAvailable`，`PayButton` 什么也不显示 |
| `platform/appleauth`、`platform/agerange`、`platform/icloud`、`platform/sensors` | Windows 上不支持：返回不可用，与在缺少这些功能的设备上一样 |
| `platform/transcribe` | 用 Windows 的桌面语音识别器在本机识别：英语的随 Windows 附带，其他语言的随该语言包的语音识别功能安装；`check` 会告诉你哪些可用。`listen` 使用默认麦克风 |
| `device.haptic` | 电脑没有触感反馈：什么也不做 |
| `platform/splash` | Windows 应用没有启动屏：`hold` 和 `hide` 什么也不做 |
| `platform/analytics` | GA4 的 Measurement Protocol，用 API 密钥发送到 `asuka.toml` 里的 Web 数据流：在主线程之外批量发送，客户端 ID 和尚未发出的事件保存在应用的数据目录里（[数据分析](#数据分析platformanalytics)） |
| `onShare`、快捷操作 | 仅限 iOS 和 Android |

## 在 Linux 上

Linux 应用同样不需要声明任何东西。凡是 Asuka 通过 XDG 桌面门户（portal）来做的事情（文件、相机、定位、通知、邮件、打印、打开文件和链接、阻止系统休眠），都由门户按它自己的方式询问用户，而且在 Flatpak 沙盒内外都能正常工作。`asuka build --platform linux` 生成的 Flatpak 清单只允许应用访问显示、GPU、网络和门户；播放或录制声音的应用（用了 `Video`、`platform/audio`、`platform/record`、`platform/transcribe`）还可以访问声音服务器。包要打开、而 GNOME 运行时里没有的库（`platform/pdf` 的 Poppler、`platform/transcribe` 的 PocketSphinx），清单会一并构建。Asuka 不经门户、直接访问的那些服务（Speech Dispatcher、提供通讯录和日历的 Evolution Data Server、提供连接类型的 NetworkManager、iio-sensor-proxy、Video4Linux 摄像头），需要你在清单里加上开放它们的 `finish-args`；否则包的表现就和在没有这项服务的系统上一样。定时通知需要沙盒外的 systemd：在 Flatpak 中，它只会在应用运行时显示（见[各平台的情况](#各平台的情况)）。

在 Linux 上，Asuka 访问桌面服务时，有门户就走门户，此外还用到 GLib、GTK，以及桌面系统通常自带的一些库。每个库都在第一次用到时才加载：如果系统里没有，就会返回一个说明缺了什么的错误。

| 包 | 在 Linux 上 |
| --- | --- |
| `platform/device` | `pickPhotos` 和 `pickFiles`：GTK 的文件对话框（在沙盒中是门户提供的），选中的文件会复制到应用的缓存目录。剪贴板里的图片及剪贴板的变化通过 GDK 的剪贴板获取。`browse` 在用户的浏览器中打开页面。`keepAwake` 阻止会话进入空闲状态。`info`：`model` 为 `"PC"`，`system` 为 `"Linux"`，`systemVersion` 为发行版的版本（取自 `/etc/os-release`） |
| `platform/secure` | 密钥环（通过 libsecret：GNOME Keyring、KWallet、KeePassXC），每个键存为一项，标签为应用名；没有 libsecret 或密钥环时，调用会失败并说明原因 |
| `platform/network` | GNetworkMonitor（在沙盒中是门户提供的）；Wi-Fi、以太网还是蜂窝网络，由 NetworkManager 提供；从不报告受限 |
| `platform/location` | 定位门户（背后是 GeoClue，由它询问用户），没有门户时直接使用 GeoClue；装了 geocode-glib 时，`placeName` 用它来查询。GeoClue 没有地理围栏：区域和后台定位更新都在应用运行期间根据位置计算 |
| `platform/speech` | Speech Dispatcher（没在运行时会启动它）：使用它的语音、语速、音调和音量 |
| `platform/audio`、`platform/record` | GStreamer：用 AAC 编码器（FDK、libav 的等等）录音，默认音源即麦克风 |
| `platform/nowplaying` | 有内容显示时，在会话总线上注册一个 MPRIS 播放器，名为 `org.mpris.MediaPlayer2.<app>.instance<pid>`：对应 GNOME 和 KDE 的媒体控件与锁屏、媒体键以及 `playerctl`。如果应用支持跳过，MPRIS 的 `Seek` 就是跳过，否则是跳转播放位置；封面图是一个 `file://` 或 http(s) URL |
| `platform/library` | 用户的文件夹（XDG 的 Pictures、Videos、Music、Downloads）下指定的文件夹 |
| `device.shareFiles` | 没有分享面板：文件会复制到剪贴板，并弹出一条提示（toast） |
| `platform/camera`、`platform/record` | 优先用相机门户（只询问用户一次；使用 PipeWire 的摄像头，需要 `gstreamer1.0-pipewire`），否则用第一个 Video4Linux 摄像头，二者都通过 GStreamer。`take()`：窗口上弹出一个对话框，带实时预览、“拍照”和“取消”按钮。`camera.View`：镜像显示的预览画面，填满它的区域；前置和后置是同一个摄像头（电脑的摄像头本来就朝着用户）。视频优先录成 MPEG-4（x264 或 OpenH264 提供的 H.264，加上 AAC），否则录成 WebM；扫码需要 GStreamer 的 `zbar` 元素。没有摄像头时为 `CameraError.noCamera`；被拒绝时为 `.notAllowed` |
| `platform/scan`、`platform/vision` | 和 Windows 上一样，用 Asuka 自带的识别器识别条码：QR、Data Matrix、EAN、UPC、Code 128、Code 39（不支持 Aztec、PDF417、Code 93、Codabar 和 ITF）。用 Tesseract（`libtesseract5` 加上对应语言的数据）识别文字；没有人脸检测，`faces` 始终为空。没有 Tesseract 时，`vision.analyze` 的结果里没有文字 |
| `platform/imageedit` | 用 GdkPixbuf 的加载器解码（装了相应加载器时支持 HEIF 和 WebP），并摆正方向；各项编辑操作由 Asuka 完成；视频帧来自 GStreamer |
| `platform/webauth` | 窗口上弹出一个内嵌 WebKitGTK 网页视图的对话框（设置 `ephemeral` 时使用独立的会话）；关闭对话框即取消 |
| `platform/js` | 一个从不显示的 WebKitGTK 网页视图 |
| `platform/maps` | 用 libshumate（`libshumate-1.0-1`）显示 OpenStreetMap 瓦片地图；`.satellite` 和 `.hybrid` 显示的也是街道图；没有用户位置图层。没有 libshumate 时，视图会显示相应说明，并报告 `error` |
| `platform/background` | 任务会阻止系统挂起；没有通知，`update` 不起作用 |
| `platform/periodic` | 应用运行期间按定时器执行任务（系统不会为了它们启动应用）；需要接通电源的任务会在使用电池时等待，需要网络的任务会在离线时等待 |
| `platform/screen` | `lock`、`setBars`、`preventCapture` 和 `setBrightness` 什么也不做；`screenshots()` 不会报告任何截图 |
| `platform/appicon` | `set` 抛出 `notAvailable`：启动器显示的是 desktop 文件里的图标 |
| `platform/sensors` | iio-sensor-proxy（适用于二合一设备、平板）：光线传感器（单位为 lux），以及由加速度计得出的方向，表示为沿朝上那条轴的 1 g，随设备转动而变化。没有其他传感器：`available` 为 false，各个流和 `steps` 都会抛出 `SensorError.notAvailable` |
| `platform/contacts`、`platform/calendar` | Evolution Data Server 中的通讯录和日历（来自 GNOME 通讯录、GNOME 日历和在线账户）：有它时访问权限为 `.allowed`（日历为 `.full`）；`pick` 是一个带搜索框的联系人对话框；`add` 先确认，再保存到默认通讯录或该日程所属的日历。没有它时：通讯录的访问权限为 `.denied`，日历为 `.writeOnly`；`add` 会在用户的联系人或日历应用中打开一个 vCard 或 iCalendar 文件，并返回 false；`contacts.pick` 会抛出错误 |
| `platform/photos` 的照片库、`platform/appleauth`、`platform/widget`、`platform/liveactivity` | Linux 上都没有：照片库的访问权限为 `.denied`；`appleauth.available()` 为 false，它的按钮什么也不显示；`liveactivity.start` 抛出 "liveactivity: not available"；`widget.set` 什么也不做。`platform/splash` 也没有启动屏可以保持 |
| `platform/purchase` | 没有应用商店：`available()` 为 false，其余调用抛出 `PurchaseError.notAvailable` |
| `platform/icloud` | 没有 iCloud："icloud: not available" |
| `platform/biometric`、`platform/integrity`、`platform/agerange`、`platform/push` | Linux 上都没有：返回不可用或不受支持。`platform/review` 什么也不做；`platform/tracking` 返回 `.allowed` |
| `platform/pay` | Linux 上没有能为应用付款的钱包：`available` 返回 `.none`，`pay` 抛出 `PayError.notAvailable`，`PayButton` 什么也不显示 |
| `platform/crash` | Asuka 的崩溃输出，其中也包括原生代码中的崩溃（GTK 代码里收到的信号、GLib 的 abort），这类崩溃报告为 `"native"`；不会读取系统自己的崩溃记录（systemd-coredump、apport） |
| `platform/analytics` | GA4 的 Measurement Protocol，和 Windows 一样 |
| `device.haptic` | 桌面设备没有触感反馈：什么也不做 |
| `onShare`、快捷操作 | 仅限 iOS 和 Android |

## 在浏览器中

网页不需要声明任何东西：某个包第一次需要用到相机、麦克风、定位或通知时，浏览器会询问用户。这些 API 大多要求页面处于安全上下文，也就是通过 `https` 或从 `localhost` 提供（`asuka dev --platform web` 的地址满足要求，`--lan` 的地址则不满足）。这些包调用的都是 Web API；详细说明见 [Web](web.md) 一页，那里也介绍了用于通知和 Web Push 的 service worker。

| 包 | 在浏览器中 |
| --- | --- |
| `platform/device` | `pickPhotos` 和 `pickFiles`：浏览器的文件选择器，选中的文件会复制到缓存中。`browse`：打开新标签页。剪贴板：可以读取应用自己复制的内容，在用户允许页面读取剪贴板时，也能读取其他应用复制的内容。`keepAwake`：使用 Screen Wake Lock。`info` 的信息来自 user agent（Chromium 的 client hints 能提供系统版本和 Android 手机的型号） |
| `platform/secure` | 值经过加密（AES-GCM）后存在 IndexedDB 中，密钥由浏览器保管、无法导出；这并不是钥匙串（见 [Web](web.md)） |
| `platform/camera`、`platform/record` | `take()` 在手机上打开相机，否则显示一个带实时画面和快门的对话框；`camera.View` 是放在 `<video>` 中的摄像头画面，支持 `capture()` 和录制，浏览器有 `BarcodeDetector` 时还能扫码 |
| `platform/scan`、`platform/vision` | 用 `BarcodeDetector` 识别条码；人脸和文字只在浏览器支持时才能识别 |
| `platform/imageedit` | 用 canvas 处理：输出 JPEG 或 PNG；远程图片需要 CORS |
| `platform/photos` | 网页无法读取照片库：访问权限为 `.denied`（要选照片请用 `device.pickPhotos`） |
| `platform/library` | `save` 会把文件下载下来 |
| `platform/location` | Geolocation API；Web 上没有 `placeName`（浏览器没有地理编码器）。区域和后台定位更新根据位置计算，只在页面打开期间有效 |
| `platform/network` | 是否在线取自 `navigator.onLine`；浏览器提供相关信息时（Android 上的 Chrome）能区分 Wi-Fi、蜂窝网络和以太网，蜂窝网络视为按流量计费，开启省流量模式（Data Saver）时视为受限 |
| `platform/notify`、`platform/push` | 通过应用的 service worker 使用 Notification API 和 Web Push（`push.register` 的 `service` 为 `"webpush"`）；应用没有任何页面打开时，定时通知无法显示 |
| `platform/audio`、`platform/record` | 每个播放器对应一个 audio 元素（受浏览器的自动播放规则约束：要在用户点按时开始播放）；录音用 `MediaRecorder`，浏览器支持时录成 MPEG-4 封装的 AAC，否则录成 WebM 封装的 Opus |
| `platform/nowplaying` | Media Session API（`navigator.mediaSession`）：对应浏览器的媒体控件、媒体键和手机锁屏，在页面播放声音时显示。它没有切换操作（按键发送的是播放或暂停）；浏览器不支持的操作（有些浏览器不支持停止）会被略去 |
| `platform/speech` | `speechSynthesis`，使用浏览器提供的语音 |
| `platform/sensors` | Generic Sensor API（Chrome），或设备运动和方向事件（Safari 会在用户点按时询问），坐标轴和单位与 Android 一致；没有气压计和计步器 |
| `platform/biometric` | 通过 WebAuthn 使用平台验证器（Touch ID、Face ID、Windows Hello、Android 的屏幕锁定）：第一次 `authenticate` 会为该站点创建一个通行密钥（passkey）。提示框由浏览器提供，所以 `reason` 和 `cancel` 不会显示 |
| `platform/webauth` | 一个弹出窗口，回到与应用同源的页面时即完成（自定义 scheme 无法打开页面） |
| `platform/js` | 一个隐藏的沙盒 iframe |
| `platform/contacts`、`platform/calendar` | 浏览器支持 Contact Picker 时（Android 上的 Chrome）可以用 `contacts.pick`；`add` 会下载一个 vCard 或 `.ics` 文件；Web 上无法读取通讯录和日历 |
| `platform/screen` | `lock` 只在全屏时或已安装的应用中有效；隐藏系统栏就是进入全屏；没有 `preventCapture`、`screenshots()` 和 `setBrightness` |
| `platform/appicon` | `set` 更换页面（标签页）的图标，下次访问时也会换上；已安装的 Web 应用仍用清单里的图标 |
| `platform/background` | 页面打开期间一直在运行；任务不会带来额外效果，也没有通知 |
| `platform/tracking` | 没有提示框：浏览器发送了 Global Privacy Control 或 Do Not Track 时为 `.denied`，否则为 `.allowed` |
| `device.haptic` | 浏览器支持时（Android 上的 Chrome）使用 `navigator.vibrate` |
| `platform/review`、`platform/integrity`、`platform/agerange`、`platform/appleauth`、`platform/icloud`、`platform/periodic`、`platform/widget`、`platform/liveactivity` | Web 上不支持：返回一个说明这一点的错误，或者该包的 `notAvailable` |
| `platform/purchase` | 在 Trusted Web Activity 里通过 Digital Goods API 使用 Google Play；其他情况下没有应用商店（`PurchaseError.notAvailable`） |
| `platform/maps` | 和 Windows 一样，由 Asuka 绘制 OpenStreetMap 的瓦片；页面获得定位权限后，`showsUser` 才会生效 |
| `platform/pay` | 浏览器支持 Apple Pay 时（Safari）通过 Payment Request API 使用它，商户验证由应用的服务器完成（`onValidateMerchant`）；其他浏览器通过 Google 的 Web API（pay.js）使用 Google Pay；两者都使用各自的官方按钮。支付面板只在点击时显示 |
| `platform/crash` | Asuka 的致命错误，以及未捕获的 JavaScript 错误；后者作为非致命的 `"exception"` 报告 |
| `platform/analytics` | 从 Google 加载的 gtag.js，发送到 `asuka.toml` 里的 Web 数据流；关闭数据收集用的是 gtag 自己的开关，并保存在 `localStorage` 里 |
| `platform/splash` | `hold` 和 `hide` 什么也不做 |
| `onShare`、快捷操作 | 仅限 iOS 和 Android |

## 相机：platform/camera

`camera.View` 实时显示相机拍到的画面：在 iOS 和 Mac 上是 AVFoundation 的预览，在 Android 上是 CameraX，在浏览器中是 `getUserMedia`，在 Windows 上是 Media Foundation，在 Linux 上是 GStreamer。`camera.capture()` 从屏幕上的这个视图拍一张照片；`camera.take()` 则打开系统的相机界面（iOS 的相机界面、Android 的相机应用；桌面端是一个带预览的弹出面板，浏览器中是文件选择器里的相机），返回照片文件，用户取消时返回 `none`。照片是放在应用缓存目录里的 JPEG，想保留就把它移走。

```asuka
import platform/camera

component Shutter {
  state facing: camera.Facing = .back
  state last = ""
  state status = ""

  Column {
    camera.View {
      facing: facing
      grow: 1
      on error(m) { status = m }
    }
    Row {
      gap: 12
      Button("Flip") { on press { facing = if facing == .back { .front } else { .back } } }
      Button("Take") { on press { last = try await camera.capture() } }
    }
    Text(status)
  }
}
```

视图第一次显示时会请求相机权限。如果被拒绝，或者设备没有摄像头（比如 iOS 模拟器），视图会报告 `error`，`capture` 会抛出 `CameraError.notAllowed` 或 `.noCamera`；屏幕上没有 `camera.View` 时抛出 `.noView`。导入 `platform/camera` 的应用在 iOS 上会自动带上相机和麦克风的用途说明（在 asuka.toml 的 `[ios.usage]` 里可以写你自己的），在 Android 上会带上 `CAMERA` 和 `RECORD_AUDIO` 权限。

`camera.access()` 告诉你应用能不能用相机，不会询问用户：`.notAsked`、`.denied` 或 `.allowed`。`camera.request()` 会在用户还没被问过时询问，并给出用户的回答。请在用户做需要相机的事时、先说明原因再询问；用户拒绝之后，系统不会再问：`device.openSettings()` 可以带用户去打开它的地方。

```asuka check
import platform/camera
import platform/device

component ScanButton {
  state why = ""

  Column {
    gap: 8
    Button("Scan a receipt") {
      on press {
        if camera.access() == .denied {
          why = "The camera is off for this app"
          return
        }
        if (try? await camera.request()) == .allowed {
          why = ""
        }
      }
    }
    if !why.isEmpty() {
      Text(why)
      Button("Open Settings") { on press { let _ = device.openSettings() } }
    }
  }
}
```

iOS 和 Mac 给出用户的选择（被策略禁止时为 `.denied`）；Android 给出是否已授权，以及应用是否问过；Windows 不会询问桌面应用，所以除非在设置里对桌面应用关闭了相机，否则都是 `.allowed`；Linux 上是相机门户对这个应用的回答（没有门户时为 `.allowed`）；浏览器给出它对这个网站的权限。`record.access()` 和 `record.request()` 对麦克风也一样。

## 录制视频：platform/record

处于视频模式的 `camera.View` 可以录像：`record.start()` 录下它显示的画面，除非指定 `mute`，否则同时录下麦克风的声音，直到调用 `record.stop()`（或到达 `maxDuration`），然后返回文件，即应用缓存目录里的一个 MPEG-4 视频。`camera.record` 和 `camera.stopRecording` 的作用相同。

```asuka
import platform/camera
import platform/record

component Clip {
  state recording = false
  state file = ""

  Column {
    camera.View { mode: .video; grow: 1 }
    Button(if recording { "Stop" } else { "Record" }) {
      on press {
        if recording {
          record.stop()
        } else {
          recording = true
          defer { recording = false }
          file = try await record.start(maxDuration: 30s)
        }
      }
    }
  }
}
```

在 iOS 和 Mac 上用的是 AVCaptureMovieFileOutput，在 Android 上是 CameraX 的 `VideoCapture`，在浏览器中是 `MediaRecorder`（写不了 MPEG-4 时用 WebM），在 Windows 上是 Media Foundation 的 sink writer，在 Linux 上是 GStreamer。

## 条码和二维码：platform/scan

打开 `scan` 的 `camera.View` 会实时识别画面里的码，每识别到一个就通过 `code` 报告一次（码一直在画面里时只报告一次）。`codes` 限定要找的种类，不写就是所有种类。种类的名字是 `"qr"`、`"aztec"`、`"datamatrix"`、`"pdf417"`、`"ean13"`、`"ean8"`、`"upca"`、`"upce"`、`"code128"`、`"code39"`、`"code93"`、`"codabar"` 和 `"itf"`。

`scan.file(path)` 则识别图片文件（拍的或选的照片）里的码：在 iOS 和 Mac 上用 Vision，在 Android 上用 ML Kit（模型打包在应用里，约 2 MB，所以没有 Google Play 服务也能用），在浏览器中用 `BarcodeDetector`（浏览器支持时），在 Windows 和 Linux 上用 Asuka 自带的识别器（这两个平台没有应用可以依靠的识别器；支持 QR、Data Matrix、EAN、UPC、Code 128、Code 39）。

```asuka
import platform/camera
import platform/scan

component Scanner {
  state found = ""

  Column {
    camera.View {
      scan: true
      codes: ["qr", "ean13"]
      grow: 1
      on code(c) { found = "{c.kind}: {c.value}" }
    }
    Text(found)
  }
}

async fn read(photo: string) throws -> [string] {
  var out: [string] = []
  for c in try await scan.file(photo, kinds: ["qr"]) { out.push(c.value) }
  return out
}
```

## 识别图像内容：platform/vision

`vision.analyze(path)` 在设备上找出图片里的内容：逐行、逐词的文字，人脸，以及条码，每一项都带有它的 `Box`（以图片左上角为原点、按图片宽高比例表示的位置，图片已摆正；乘以视图的尺寸就得到点）。只请求你需要的（`text:`、`faces:`、`codes:`），知道文字的语言时也告诉它（`languages: ["ja"]`）。数据不会离开设备。

```asuka
import platform/vision

async fn words(photo: string) throws -> [string] {
  let r = try await vision.analyze(photo, faces: false, codes: false)
  return r.lines.map(l => l.text)
}
```

在 iOS 和 Mac 上用的是 Vision 框架，在 Android 上是 ML Kit 及其打包的模型（约 30 MB），在浏览器中是 Shape Detection API（浏览器支持时），在 Windows 上是系统的 OCR 和人脸检测，在 Linux 上是 Tesseract（已安装时）；Windows 和 Linux 上的条码由 Asuka 自带的识别器识别。文字和人脸用的都是各平台自己的识别（没有一个各平台都能共用的识别器），所以不同平台识别出的结果可能不同。`platform/scan` 读出码的内容，这个包则告诉你码在哪里。

## 照片库：platform/photos

`platform/photos` 在用户许可后读取用户的照片库：iOS 和 Mac 上用 PhotoKit，Android 上用 MediaStore，Windows 上读取“图片”文件夹。Linux 和 Web 上没有应用可以读取的照片库，访问权限为 `.denied`。这个包适合以照片库为主界面的应用。如果只是想让用户挑几张照片，用 `device.pickPhotos` 就够了，而且它不需要任何权限：能满足需求时请优先使用它。Google Play 会要求读取照片库的应用在 Play 管理中心说明理由。所有调用都是 async 的。

```asuka
import platform/photos

component Screenshots {
  state thumbs: [string] = []

  task {
    if (try? await photos.request()) == .denied { return }
    let assets = (try? await photos.list(screenshots: true, limit: 60)) ?? []
    for a in assets {
      thumbs.push((try? await photos.thumbnail(a.id, size: 300)) ?? "")   // 一个文件，可交给 Image 显示
    }
  }

  Scroll {
    for (i, t) in thumbs.enumerated() key i { Image(t) { width: 100; height: 100 } }
  }
}

async fn original(a: photos.Asset) throws -> string {
  let f = try await photos.export(a.id)   // 原图，以文件形式导出
  return f.path
}
```

- `photos.current()` 不询问用户，直接告诉你当前的授权状态：`.notAsked`、`.denied`、`.limited`（只能访问用户选定的照片：iOS 14、Android 14 起）或 `.allowed`。`photos.request()` 会在第一次调用时询问用户。`photos.manageLimited()` 让处于 `.limited` 状态的用户修改允许访问的照片，并在用户改完后返回：这时请重新获取列表。
- `photos.list(screenshots:, offset:, limit:)` 按从新到旧的顺序返回，从 `offset` 开始，最多 `limit` 个（默认 100）。每个 `Asset` 包含 `id`、`width`、`height`、`created`（一个 `time.Time`）和 `screenshot`。设置 `screenshots: true` 后只返回截图：iOS 本身知道哪些是截图；Android 上则以 Screenshots 文件夹中的图片为准。
- `photos.thumbnail(id, size:)` 返回一个短边至少 `size` 像素的 JPEG 文件，并保存在应用的缓存目录中，下次直接复用。
- `photos.export(id)` 按照片库中的原始格式（HEIC、JPEG、PNG）把完整的图片复制到一个文件，返回它的 `path` 以及它在照片库中的 `name`。iCloud 中的照片会先下载。

## 照片、浏览器与剪贴板：platform/device

`platform/device` 汇集了平台在视图之外提供的各种能力。

```asuka
import platform/device

component Gallery {
  state photos: [string] = []

  Column {
    Button("Pick photos") {
      on press { photos = (try? await device.pickPhotos(max: 3)) ?? [] }
    }
    Button("Help") {
      on press { try? await device.browse("https://example.com/help") }
    }
    for p in photos key p { Image(p) }
  }
}
```

- `device.pickPhotos(max:)` 显示系统的照片选择器（iOS 上是 PHPicker，Android 上是 Photo Picker；桌面上是打开对话框，浏览器中是文件选择器），最多可选 `max` 张图片（1 到 255）。它不需要任何权限：由用户自己挑选，应用只能看到被选中的图片。它返回这些图片的文件（可以直接交给 `Image` 显示），用户取消时则为空。这些文件是放在应用临时目录或缓存目录中的副本，要保留的话请把它移走。
- `device.browse(url)` 在应用内、覆盖在当前界面之上显示一个网页（SFSafariViewController、Custom Tab）。URL 必须是 http 或 https。而 `device.openURL` 则会离开应用、跳转到浏览器。在桌面上，它会在用户的浏览器中打开页面；在浏览器中，则打开一个新标签页。
- `device.clipboardText()` 读取剪贴板里的文本，没有文本时返回 `none`。应用第一次读取别的应用放进剪贴板的内容时，iOS 会询问用户。写入剪贴板请用 `device.copy(text)`。
- `device.keepAwake(on)` 在 `on` 为 true 时让屏幕保持常亮，适合视频、菜谱、地图这类场景。需要常亮的界面关闭时，记得把它关掉。
- `device.pickFiles(types:, multiple:)` 显示系统的文档选择器（iOS 的“文件”浏览器、Android 的存储选择器、桌面上的打开对话框、浏览器中的文件选择器；都不需要权限）。`types` 是 MIME 类型（`"application/pdf"`、`"image/*"`；不填则可以选任何文件），`multiple` 允许用户选择多个文件。它为每个文件返回一个 `device.File`：`path` 是复制到应用缓存目录中的副本，另外还有 `name`、`type` 和 `size`；用户取消时则为空。
- `device.clipboardHas()` 不读取剪贴板，就能告诉你里面装的是 `text`、`image`、`url` 还是富文本（`html`）（iOS 不会因此询问用户）：这样你可以只在有东西可粘贴时才显示“粘贴”按钮。`device.clipboardImage()` 把剪贴板里的图片读到一个文件中，格式与复制时相同；`device.copyImage(path)` 把一个图片文件放进剪贴板。`device.clipboardHTML()` 以 HTML 形式读取富文本（浏览器、邮件或文档里复制的内容，带着粗体、链接和列表）；`device.copyHTML(html, text:)` 把富文本放进剪贴板，同时附上 `text`（默认是这段 HTML 自身的文字），给只能粘贴纯文本的应用。`device.clipboardChanges()` 是一个流，每当剪贴板变化时给出它当前的内容（iOS 和 Android 会主动通知；Mac 和 Windows 则每秒查询两次；浏览器能看到页面内的复制，在用户允许页面读取剪贴板时，也能看到其他应用的复制）。
- `device.info()` 返回设备和应用的信息：`model`（"iPhone"、"Pixel 9"、"Mac"、"PC"）、`system`（"iOS"、"Android"、"macOS"、"Windows"、"Linux"）、`systemVersion`，以及 `appVersion` 和 `build`，即 `asuka.toml` 中的 `version` 和 `build`。和 expo-application 一样，`source` 表示应用从哪里来（`.appStore`、`.testFlight`、`.playStore`、`.otherStore`（比如 Microsoft Store 或 Flathub）、`.direct`（下载或侧载的 APK）、`.development`（开发者的构建）），`installedAt` 和 `updatedAt` 是应用安装的时间和当前版本安装的时间（Web 上没有，`source` 为 `.unknown`）：可以用来显示测试版横幅，或者提示下载来的副本去商店安装。

```asuka
import platform/device

component Player {
  let info = load device.info()

  task {
    device.keepAwake(true)
    defer { device.keepAwake(false) }
    await sleep(1h)
  }

  Text(if let i = info.value { "{i.model}, {i.system} {i.systemVersion}" } else { "" })
}
```

## 网页登录：platform/webauth

`webauth.open(url)` 在系统自带的登录面板中显示登录页面：iOS 上是 `ASWebAuthenticationSession`，Android 上是 Custom Tab。这个面板属于浏览器，所以用户已有的登录会话、通行密钥和密码自动填充都能用，那些拒绝应用内 `WebView` 的登录服务（比如 Google 登录）也不会拒绝它。做 OAuth 时请用它，而不是 `WebView`。

Mac 上同样有 `ASWebAuthenticationSession`。Windows 和 Linux 没有系统级的登录面板，Asuka 会在窗口之上用应用自己的网页视图（WebView2、WebKitGTK）显示页面。这个视图使用应用的 Web 配置文件，但没有用户浏览器中的登录会话；拒绝内嵌网页视图的登录服务，在这里同样会拒绝。浏览器则会在弹出窗口中打开页面，页面回到与应用同源的地址时即完成（自定义 scheme 无法打开页面）。

```asuka
import platform/webauth

async fn signIn(authorizeURL: string) throws -> string? {
  do {
    // 形如 myapp://…?code=…：接下来用其中的 code 换取令牌。
    return try await webauth.open(authorizeURL, callbackScheme: "myapp")
  } catch webauth.WebAuthError.canceled {
    return none // 用户关掉了面板
  }
}
```

登录流程有两种结束方式：

- **通过回调 scheme。** 页面跳转到 `callbackScheme`（应用自己的 scheme）下的 URL 时，面板关闭，`open` 返回这个 URL。在 Android 上，这个 scheme 必须就是应用的 scheme，即 asuka.toml 中的 `scheme`；URL 会交给 `open`，而不是应用的 `openURL` 事件。
- **由应用自己结束。** 不设置 scheme 时，页面会重定向到一个应用能收到通知的地方：如果 OAuth 服务器允许重定向到回环地址（`http://localhost:port/callback`，见 RFC 8252），它就会把 code 发给应用在该端口上运行的服务器；让两者竞争（`race(server.next(), webauth.open(url))`），服务器先收到时就会取消 `open`，面板随之关闭。在 Android 上，取消会让应用回到标签页之上。

用户关闭面板时，`open` 会抛出 `webauth.WebAuthError.canceled`。`ephemeral: true` 会请求一个私密会话，不带浏览器中的 cookie（iOS 的 `prefersEphemeralWebBrowserSession`；浏览器支持时用 Android 的临时 Custom Tab；Windows 和 Linux 上是一个用完即弃的配置文件）。

## 复制、分享和打开：device.copy、device.share、device.openURL

`platform/device` 中有三个函数可以把文本交给系统处理。

```asuka
import platform/device

component Links {
  Column {
    Button("Copy link") { on press { device.copy("https://example.com/s/42") } }
    Button("Share") { on press { device.share("https://example.com/s/42") } }
    Button("Open in browser") { on press { device.openURL("https://example.com") } }
    Button("Call") { on press { device.openURL("tel:+15555550100") } }
  }
}
```

- `device.copy(text)` 把文本放进剪贴板。
- `device.share(text)` 显示系统的分享面板，用户可以把文本发给某个应用、联系人，或者通过 AirDrop、Nearby Share 发送。URL 会以链接的形式分享。Mac 显示它的共享服务选择器，Windows 显示它的分享界面，浏览器在支持时显示 Web Share 面板；Linux 没有分享面板，所以会把文本复制到剪贴板并弹出一条提示（不支持 Web Share 的浏览器也是这样复制的）。
- `device.openURL(url)` 请系统打开一个 URL：网址在浏览器中打开，其他 scheme（`tel:`、`mailto:`、其他应用的 scheme）由能处理它的应用打开。
- `device.openSettings()` 打开系统设置里属于这个应用的页面，用户可以在那里打开之前拒绝过的权限（相机、通知、定位）：iOS 上是应用自己的设置页，Android 上是它的“应用信息”；Mac 和 Windows 没有单个应用的页面，打开的是“隐私与安全性”。没有可打开的页面时（Linux、Web）返回 false。

`device.shareFiles(paths, mime:, uti:, title:, from:)` 显示用于分享文件的面板：发送一段视频、把 PDF 存到“文件”、用 AirDrop 传照片。它是 async 的，面板消失后才返回，无论用户有没有分享；如果面板无法显示（比如文件不存在），它会抛出错误。

```asuka
import platform/device

component ShareReport(report: string) {
  state status = ""
  let button = Ref()

  Column {
    Button("Share") {
      ref: button
      on press {
        do {
          try await device.shareFiles([report], mime: "application/pdf", title: "Send the report", from: button)
        } catch e {
          status = e.message
        }
      }
    }
    Text(status)
  }
}
```

它的选项与 expo-sharing 的一致：

| 选项 | |
| --- | --- |
| `mime` | 文件的类型，分享面板上的应用据此判断能否处理。Android 会把 intent 的类型设为它。在 iOS 上，`uti` 为空时用它作为类型。默认：根据第一个文件的扩展名推断。 |
| `uti` | iOS：文件的统一类型标识符（Uniform Type Identifier，如 `com.adobe.pdf`），优先级高于 `mime`。 |
| `title` | 面板的标题：Android 上是选择器的标题，在 iOS 上则作为邮件的主题。 |
| `from` | 面板以弹出框（popover）形式出现时（iPad、Mac）所指向的视图的 `Ref`，通常就是触发分享的那个按钮。默认：屏幕中央。 |

在 iOS 上，如果文件的扩展名与它的类型对不上，会像 expo-sharing 那样换一个相符的文件名再分享：作为 JPEG 分享的 `image.pdf` 会以 `image.pdf.jpeg` 的名字发出，它是指向原文件的链接，而不是副本。在 Android 上，文件通过应用的 FileProvider 传递；不在应用自身目录中的文件，会先复制到缓存目录。Mac 上用的是共享服务选择器。Windows 上用的是分享界面（界面一出现 `device.shareFiles` 就返回，因为 Windows 不会告知它何时关闭）。Linux 没有分享面板：文件会放进剪贴板，与分享文本时一样。浏览器能用 Web Share 面板分享文件时就用它，否则会逐个下载这些文件。

如果想让应用自己能通过 URL 被打开，请参阅[打包与发布](shipping.md#asukatoml)中 `asuka.toml` 的 `scheme` 和 `links`，以及 prelude 的 `onOpenURL`（或 app 块中的 `on openURL(url)`）。

## 预览文件：platform/preview

`preview.show(sources)` 用系统的查看器显示文件，就像邮件应用显示附件那样：PDF、图片、视频、音频、Office 和 iWork 文档、文本都可以。来源可以是文件路径、资源路径（与 `Image` 接受的格式相同），也可以是 http(s) URL；对于 URL，会先把文件下载到应用的缓存目录，沿用原来的文件名（或者根据服务器返回的类型补上扩展名）。

```asuka
import platform/preview

component Invoice {
  prop path: string
  state status = ""

  Column {
    Button("Open the invoice") {
      on press {
        do {
          try await preview.show([path, "docs/terms.pdf"])
        } catch preview.PreviewError.noViewer {
          status = "Nothing on this device opens it"
        } catch e {
          status = e.message
        }
      }
    }
    Text(status)
  }
}
```

`start:` 是最先显示的文件的索引；`edit: true` 允许用户用“标记”（Markup）功能在 PDF 和图片上标注，并保存回文件（iOS）。查看器关闭后它才返回；如果没有任何东西能显示这些文件，它会抛出 `preview.PreviewError.noViewer`，也可能抛出其他错误（文件不存在、下载失败）。

| | 查看器 | 何时返回 |
| --- | --- | --- |
| iOS | 覆盖在界面上的快速查看（Quick Look，`QLPreviewController`）：可以左右滑动切换文件，支持分享、标记和搜索 | 查看器关闭时 |
| Android | 对 `start` 指定的文件发起 `ACTION_VIEW`，在用户为该类型选择的应用（Drive 的 PDF 查看器、相册）中打开；文件通过应用的 FileProvider 传递，只授予读取这一个文件的权限 | 用户返回应用时 |
| macOS | 快速查看面板（`QLPreviewPanel`），用其中的箭头在文件之间切换 | 面板关闭时 |
| Windows | 文件的默认应用（`ShellExecuteEx`），或者“你要如何打开这个文件？”对话框 | 交给系统之后 |
| Linux | 文件的默认应用（`GtkFileLauncher`，在沙盒中由门户打开） | 交给系统之后 |
| Web | 新标签页，适用于浏览器能直接显示的内容（图片、视频、音频、文本、PDF、HTML）；其他类型为 `noViewer` | 打开之后 |

Android 没有系统级的查看器，也没有能在多个文件之间翻页的查看器：它只打开一个文件，就像 Android 上的邮件应用逐个打开附件那样。在 iOS 上，`preview.show` 目前还不能指定查看器从哪个视图展开（即像 `device.shareFiles` 的 `from:` 那样传一个 `Ref`）。

## 显示 PDF：platform/pdf

`pdf.View` 把 PDF 显示在应用的界面里，和其他视图并列：页面从上往下排，适应视图的宽度，可以滚动浏览，双指缩放。`source` 可以是文件路径、资源的路径（和 `Image` 的写法一样），或者 http(s) URL（先下载下来）。

```asuka check
import platform/pdf

component Manual {
  state page = 0
  state pages = 0

  Column {
    grow: 1
    pdf.View {
      source: "docs/Manual.pdf"
      bind page: page
      grow: 1
      on loaded(n) { pages = n }
    }
    Row {
      justify: .spaceBetween
      padding: 12
      Button("Previous") { on press { page = max(0, page - 1) } }
      Text("{page + 1} of {pages}")
      Button("Next") { on press { page = min(pages - 1, page + 1) } }
    }
  }
}
```

`page` 是当前显示的页，从 0 开始：设置它会滚动到那一页；绑定之后，它会跟随用户的滚动（`pageChange` 也会告诉你）。显示出来之后，`loaded` 给出页数；显示不了时，`error` 说明原因（文件不存在、不是 PDF、已加密、下载失败）。

| | 用什么显示 |
| --- | --- |
| iOS、macOS | PDFKit 的 `PDFView` |
| Android | `PdfRenderer` 渲染的页面，放在一个能滚动、能缩放（双击、双指）的列表里 |
| Windows | `Windows.Data.Pdf` 渲染的页面 |
| Linux | Poppler（`libpoppler-glib8`）渲染的页面，前提是发行版提供了它 |
| Web | 浏览器自带的查看器；没有查看器的浏览器（大多数手机上的）会显示一个打开它的链接，`loaded` 给出的页数是 0 |

如果想把文件交给系统的查看器，见 `platform/preview`；要生成 PDF，用 `print.pdf`。

## 打印与 PDF：platform/print

`platform/print` 可以为文件或 HTML 标记显示系统的打印对话框，其中包含可用的打印机和各种选项（份数、页码范围、纸张、方向；Android 和 Mac 上还有“存储为 PDF”）；它还能把 HTML 排版成分页的 PDF。对话框关闭后，负责打印的调用会返回一个 `print.Outcome`。这个包的名字与 prelude 的 `print` 相同：如果还要打印文本，导入时给它换个名字。

```asuka
import platform/print as printing

async fn printReceipt(receipt: string) throws -> string {
  let r = try await printing.html(receipt, job: "Receipt")
  return if r == .printed { "Sent to the printer" } else { "" }
}
```

- `print.file(source, job:, orientation:)` 打印 PDF 或图片（可以是路径、资源或 http(s) URL，与 `preview.show` 接受的一样）。其他类型会抛出 `print.PrintError.unsupported`，但 Windows 和 Linux 例外：Windows 上由文件所属的应用来打印，Linux 上由打印机决定接受什么（即 CUPS 能打印的内容）。
- `print.html(markup, job:, orientation:)` 打印 HTML 标记，分页方式与浏览器打印网页相同：CSS 的 `@media print` 和 `break-before` 都会生效。图片和样式要用绝对 URL（`https:`、`data:`）。
- `print.text(s, job:, mono:, orientation:)` 打印纯文本，就像应用里显示的报告或笔记那样：行和空行保持原样，过长的行会自动换行，`job` 作为标题显示在最前面；`mono: true` 会改用平台的等宽字体，适合按列排版的文本。
- `print.webView(view, job:, orientation:)` 打印 `WebView` 当前显示的页面（包括用户和页面脚本对它做过的改动），按它的浏览器打印网页的方式分页：`view` 是装着这个 WebView 的 `Ref`（用 `ref:` 放进去）。如果 Ref 里没有应用正在显示的 WebView，会抛出 `print.PrintError.unsupported`。

```asuka
import platform/print as printing

let page = Ref()

component Article(url: string) {
  Screen {
    title: "Article"
    toolbar {
      Button("Print") {
        icon: "printer"
        on press { try? await printing.webView(page, job: "Article") }
      }
    }
    WebView(url: url) {
      ref: page
      grow: 1
    }
  }
}
```

`job` 是打印队列中的任务名（默认依次取文件名、页面标题、应用名），`orientation` 是对话框初始的纸张方向（`.portrait`、`.landscape`）。

`Outcome` 可能是 `.printed`（任务已进入打印机队列，或已从对话框存储为 PDF）、`.cancelled`，或者在平台不告知结果时为 `.unknown`。打印任务失败会抛出错误。`print.PrintError.unavailable` 表示设备无法打印。

| | 对话框 | 文件 | HTML | 结果 |
| --- | --- | --- | --- | --- |
| iOS | `UIPrintInteractionController`，iPad 上以弹出框显示 | PDF 和图片（`printingItem`） | WebKit 的打印格式化器 | 已打印或已取消 |
| Android | `PrintManager` 的打印界面 | PDF 原样写入；图片缩放到页面大小并摆正方向（与 `PrintHelper` 的做法相同） | WebView 的打印适配器 | 已打印、已取消或失败（取决于任务状态） |
| macOS | `NSPrintOperation` 的面板，以 sheet 形式附着在窗口上，带预览 | PDFKit（PDF；图片放在一页上） | WebKit 的打印操作 | 已打印或已取消 |
| Windows | WebView2 提供的：系统打印对话框 | 交给文件关联的应用打印（shell 的 `print` 动作），不限类型 | WebView2 | 未知 |
| Linux | GTK 的打印对话框（打印文件时用打印门户的对话框） | 直接交给打印机（CUPS：PDF、图片） | WebKitGTK 的打印操作 | 已打印或已取消 |
| Web | 浏览器的打印对话框 | 在浏览器的查看器中打印 PDF，以及图片 | 一个隐藏的 iframe | 未知 |

在 Android 和 iOS 上，宽大于高的图片默认以横向开始，这与它们打印照片时的行为一致。WebView 的页面用 WebView 自己的打印功能来打印：iOS 上是它的打印格式化器，Android 上是它的打印适配器，Mac 和 Linux 上是 WebKit 的打印操作，Windows 上是 WebView2 的打印对话框。在浏览器中，与应用同源的页面（比如 `html:` 字符串生成的页面）会从它的 iframe 完整地打印出来；其他网站的页面，应用的页面无法访问其内容，因此打印的是只留下这个 WebView 的应用页面，内容就是它当前显示的样子。`platform/print` 目前还不能指定 iPad 上的弹出框指向哪个视图。

### 用 HTML 生成 PDF

`print.pdf(markup, page:, landscape:, margins:)` 把 HTML 标记排版成分页的文档，排版效果与 `print.html` 打印出来的相同，只是不弹出对话框。它会在应用的缓存目录中写出一个 PDF，文件名取自页面的 `<title>`（如 "Receipt.pdf"；没有标题时为 "Document.pdf"），并返回文件路径，方便你拿去分享、发邮件或移到别处。PDF 中的文字仍然是文字，可以搜索，也可以选中。

```asuka
import platform/print as printing
import platform/preview

async fn receiptPDF(receipt: string) throws {
  let path = try await printing.pdf(receipt, page: printing.Page.a4, margins: printing.Margins(top: 48, right: 48, bottom: 48, left: 48))
  try await preview.show([path])
}
```

| 参数 | |
| --- | --- |
| `page` | 纸张大小，以点（pt）为单位：`Page.a3`、`a4`、`a5`、`letter`、`legal`，或者自定义尺寸。默认：用户所在地区通用的纸张（`print.regionPage(environment().locale)`），美洲使用 Letter 的国家和地区为 Letter（依据 CLDR 的纸张尺寸数据），其他地方为 A4。 |
| `landscape` | 将页面转为横向，即以较长的一边作为宽度。 |
| `margins` | 每页内容四周的留白，以点为单位。默认：每边半英寸（36 点）。 |

iOS 用 WebKit 排版（`UIPrintPageRenderer` 配合网页视图的打印格式化器），Android 用 WebView 的打印适配器，Mac 用 WebKit 的打印操作，Windows 用 WebView2（Chromium 的 `Page.printToPDF`），Linux 用 WebKitGTK（打印到文件）。浏览器只能通过打印对话框生成 PDF：所以在浏览器中 `pdf` 会抛出 `unsupported` 错误，而 `print.html` 可以让用户自己选择“存储为 PDF”。

## 邮件与短信：platform/compose

`platform/compose` 会打开系统的邮件和短信编辑界面，预先填好内容，再交给用户发送；用户操作完后，它返回一个 `compose.Result`：`.sent`、`.saved`（存为草稿）、`.cancelled`，或者在邮件交给了其他应用、对方不告知结果时为 `.unknown`。

```asuka
import platform/compose

async fn mailReceipt(receiptPDF: string) -> string {
  do {
    let r = try await compose.mail(
      to: ["support@example.com"],
      subject: "Order 1042",
      body: "<p>Hello,</p><p>The receipt is attached.</p>",
      html: true,
      attachments: [compose.Attachment(source: receiptPDF, name: "Receipt 1042.pdf")],
    )
    return if r == .sent { "Sent" } else { "" }
  } catch e {
    return if e.message == "compose: unavailable" { "Set up a mail account to send it" } else { e.message }
  }
}

async fn onMyWay() throws {
  let _ = try await compose.message(to: ["+15555550100"], body: "On my way")
}
```

`compose.mail` 接受 `to`、`cc` 和 `bcc` 三组地址，以及 `subject`、`body`（`html` 为 true 时是 HTML 标记）和 `attachments`。`compose.message` 接受 `to`（电话号码或 iMessage 地址）、`subject`（在“信息”开启了主题栏时使用，否则放在正文前面）、`body` 和 `attachments`。`Attachment` 包含 `source`（路径、资源或 http(s) URL）、`type`（默认根据扩展名推断）以及收件人看到的 `name`（默认是文件名）。如果邮件无法承载 HTML（`mailto:` 链接、Simple MAPI），正文会改为纯文本发送，转换方式与 `compose.plainText(html)` 相同。

`compose.check()` 告诉你设备能做哪些事：`mail` 表示有邮件账户或邮件应用；`mailComposer` 表示有 iOS 自带的邮件编辑界面，它能告知用户最终做了什么，也支持附件；`text` 表示能发短信；`textAttachments` 表示短信能带附件。设备做不到的调用会抛出 "compose: unavailable"。

| | 邮件 | 短信 | 结果 |
| --- | --- | --- | --- |
| iOS | 覆盖在应用之上的“邮件”编辑界面（`MFMailComposeViewController`）；没有配置“邮件”账户时，用 `mailto:` 链接交给用户设置的邮件应用（不带附件） | “信息”的编辑界面（`MFMessageComposeViewController`） | 已发送、已保存、已取消；使用链接时为未知 |
| Android | 只交给邮件应用：`ACTION_SENDTO` 加 `mailto:`；带附件时用 `ACTION_SEND`，并以 `mailto:` 作为 selector，文件通过应用的 FileProvider 传递；HTML 正文既以 span 的形式放进 `EXTRA_TEXT`，也放进 `EXTRA_HTML_TEXT` | 默认短信应用：`smsto:` 加 `sms_body`；带附件时用 `ACTION_SEND` | 未知，在用户返回应用后返回 |
| macOS | 用 `mailto:` 链接交给用户的邮件应用；带附件时用邮件共享服务（`NSSharingService`），它不支持抄送和密送（设置了会抛出 "compose: unavailable"） | “信息”（`NSSharingService`） | 未知，或已取消 |
| Windows | `mailto:` 链接；带附件时用 Simple MAPI（`MAPISendMailW`：支持 Outlook、Thunderbird；新版 Outlook 和 Windows 的“邮件”应用不支持，此时为不可用） | 不可用 | 未知，或已取消（MAPI） |
| Linux | 邮件门户（邮件应用的编辑界面，带上附件），其次是 `xdg-email`，最后是 `mailto:` 链接 | 不可用 | 未知，或已取消（门户） |
| Web | `mailto:` 链接；带附件时用 Web Share 面板 | `sms:` 链接；带附件时用 Web Share 面板 | 未知 |

iOS 模拟器上既没有“邮件”账户，也没有“信息”：请在真机上试用这些编辑界面。

## 定位：platform/location

`platform/location` 提供设备的当前位置，以及移动过程中的位置变化。它需要用户许可，并会在第一次使用时请求。iOS 和 Mac 上用 Core Location；Android 上用系统的位置管理器，不依赖 Google Play 服务；Windows 上用 WinRT 的 Geolocator，Linux 上用定位门户（或 GeoClue），浏览器中用 Geolocation API。应用不在前台时，它也能在后台持续跟踪设备位置，并替应用监控区域，见下文。

`location.current()` 如果还没请求过权限，会先请求，然后返回设备当前的位置。如果用户没有允许，它会抛出 `LocationError.denied`。

```asuka
import platform/location

component Where {
  state where = ""

  Column {
    Button("Where am I?") {
      on press {
        do {
          let p = try await location.current()
          where = "{p.latitude.fixed(4)}, {p.longitude.fixed(4)} (within {p.accuracy.fixed(0)} m)"
        } catch location.LocationError.denied {
          where = "Allow location in Settings"
        } catch e {
          where = e.message
        }
      }
    }
    Text(where)
  }
}
```

`location.positions()` 是一个流：先给出当前位置，之后设备每移动一次就再给出一个。在 task 中读取它，它就会随组件一起停止：

```asuka
import platform/location

component Track {
  state here = location.Position()

  task {
    do {
      for await p in location.positions() { here = p }
    } catch e {
      print(e.message)
    }
  }

  Text("{here.latitude.fixed(5)}, {here.longitude.fixed(5)}")
}
```

`location.placeName(latitude, longitude)` 返回某个地点的名称（"Mission District, San Francisco"），由平台的地理编码器提供，而地理编码器要请求自己的服务器：所以只在用户确实需要地名时才调用。Linux 上在装了 geocode-glib 时使用它；Windows 和浏览器没有地理编码器，在那里会抛出错误。

`location.Position` 包含 `latitude`、`longitude`、`accuracy`（单位为米：设备所在范围的半径）、`altitude`（海拔，单位为米；未知时为 0）、`speed` 和 `course`（未知时为负数），以及 `time`（一个 `time.Time`）。区域穿越事件包含 `region`、`kind`、`at`（系统给出时的位置）和 `time`。

### 权限级别

用户可以只在使用应用期间允许定位（`.whenInUse`），也可以始终允许（`.always`）。区域监控，以及应用没有运行时的事件，都需要 `.always`。用户还可以选择给出精确位置还是大致位置（误差在几公里以内：即 iOS 的“精确位置”开关和 Android 的“大致位置”）。`location.authorization()` 返回用户允许了什么，并且会被追踪：在绑定中读取它时，它会跟随用户在“设置”中所做的修改。

```asuka
import platform/location

component Allowed {
  let auth = location.authorization()

  Column {
    Text("Allowed: {auth.level}; precise: {auth.precise}{if auth.off { "; location services are off" } else { "" }}")
    Button("Allow always") { on press { let _ = try? await location.requestAlways() } }
    Button("Location settings") { on press { try? await location.openSettings() } }
  }
}
```

- `location.request()` 在第一次调用时请求 `.whenInUse`；变成 `.denied` 之后再请求，就什么也不会弹出：只能由用户在“设置”中修改（`location.openSettings()`）。
- `location.requestAlways()` 请求 `.always`（在 `.whenInUse` 之后）。iOS 只会询问一次；Android 11 及以后的版本会把用户带到应用的定位设置页面，让用户选择“始终允许”。
- `location.requestPrecise(purpose)` 向只允许了大致位置的用户请求精确位置：在 iOS 上只是临时的（`purpose` 是 asuka.toml 中 `ios.locationPurposes` 的一个键），在 Android 上则会一直有效。

桌面应用一旦获得允许就是 `.always`：反正它在不可见时也会继续运行。Linux 和浏览器在应用请求位置之前不会给出任何信息，所以在拿到第一个位置之前，它们都是 `.notAsked`。

### 后台定位更新

`location.startUpdates(…)` 开始一组定位更新：应用离开前台后它们仍会继续，应用重新启动后也依然有效（在 iOS 和 Android 上，系统会为这些更新启动应用），直到调用 `location.stopUpdates()` 为止。`location.updates()` 是这些更新的流，每一项是一个 `Batch`（`positions`，从旧到新排列）：请在应用启动时就读取它，这样系统为某一批更新启动应用时，这批更新才有人接收。

```asuka
import platform/location

async fn track() throws {
  try await location.startUpdates(
    accuracy: .high,
    distance: 20,
    activity: .fitness,
    delay: 30s,
    indicator: true,
    notification: location.Notification(title: "Tracking your route", text: "Your run is being recorded"),
  )
}

async fn followUpdates() throws {
  for await b in location.updates() {
    let done = b.hold()   // 发送期间让应用保持运行
    defer { done() }
    print("{b.positions.count()} positions")
  }
}
```

`accuracy` 在精确度和耗电之间权衡（`.high`、`.navigation`、`.balanced`、`.low`、`.passive`）；`distance` 是两次更新之间设备移动的距离；`interval` 是更新的频率（仅 Android）；`activity` 用来调整 iOS 的更新方式（配合 `autoPause` 时，也调整它的自动暂停）；`delay` 让更新攒在一起送达，应用被唤醒的次数就更少；`significant` 只接收显著的位置变化（约 500 米），几乎不耗电，而且在 iOS 上，即使用户关掉了应用，这些变化也会启动它。`indicator` 在应用于后台接收更新时，显示 iOS 的蓝色指示器。在 Android 上，`notification` 会运行一个定位类型的前台服务，它就是这个服务的通知：有了它，应用不在前台时更新也能按设定的频率送达。`location.updating()` 表示更新是否正在运行（它们可能是在之前某次启动时开始的）。

在桌面端和浏览器中，这些更新就是应用运行期间监听到的位置。

### 区域

`location.monitor(region)` 替应用监控一个圆形区域，直到调用 `location.stopMonitoring(id)` 为止：区域的转换通过 `location.crossings()` 送达，应用在后台或者没有运行时也会收到（iOS 和 Android 会启动应用）。如果开始监控时设备已经在区域内，会收到一个 `.enter`。在 iOS 和 Android 上需要 `.always`（否则抛出 `LocationError.needsAlways`）；超出系统的数量限制时，会抛出 `tooManyRegions`。

```asuka
import platform/location

async fn watchHere() throws {
  let p = try await location.current()
  try await location.monitor(location.Region(id: "here", latitude: p.latitude, longitude: p.longitude, radius: 150, events: [.enter, .exit]))
}

async fn followCrossings() throws {
  for await c in location.crossings() {
    print("{c.kind} {c.region}")
  }
}
```

`location.supported()` 说明系统自己能做什么：`regions`（iOS、Android、macOS 和 Windows 由系统监控区域；在 Linux 和浏览器中，则由这个包在应用运行期间根据位置计算）、`maxRegions`（iOS 和 macOS 上为 20）、`dwell`（是否会收到 `.dwell`），以及 `relaunch`（系统是否会为事件启动应用）。

### 应用没有运行时的事件

系统会保留每一批更新和每一次区域转换，直到应用读取了它为止：应用被结束时恰好到来的事件，会在下次启动时再交给应用。一个事件会让应用继续运行几秒钟；如果读取事件之后还有工作要做（比如把它发送到服务器），`b.hold()` 和 `c.hold()` 可以让应用一直运行，直到调用它们返回的函数为止（或者系统给的时间用完：iOS 上约 30 秒，Android 上约 10 秒）。

### 测试

`location.fake()` 让这个包使用一个 `FakeLocation`，而不是平台的定位服务：`answer` 是用户被询问时给出的回答；`move(latitude, longitude)` 把设备移到某个地方（读取位置的一方会看到新位置，更新运行期间会送来一批更新，设备进入或离开的区域会产生转换）；`allow(level)` 相当于用户在“设置”中修改了授权；`pending` 统计 `hold()` 还没结束的事件。

```asuka
import platform/location

test "a region, crossed" {
  let loc = location.fake()
  loc.answer = .always
  loc.move(35.6812, 139.7671)
  let _ = try await location.requestAlways()
  try await location.monitor(location.Region(id: "here", latitude: 35.6812, longitude: 139.7671, radius: 150))
  loc.move(35.70, 139.7671) // 向北 2 公里：一次离开
}
```

## 通知：platform/notify

`platform/notify` 用来显示本地通知，可以立即显示，也可以在应用设定的时间显示，到时应用是否在运行都没关系。点按通知会用它的 URL 打开应用。它还提供了消息类应用所需的各种功能，同样适用于 `platform/push` 的通知：操作按钮和直接在通知里输入的回复、会话分组（thread）、Android 的通知渠道、对话中的消息、角标、当前显示的通知，以及应用打开时哪些通知要显示。

```asuka
import platform/notify

async fn remindToWater() throws {
  if !(try await notify.request()) {
    return
  }
  try await notify.schedule(notify.Notification(
    id: "water",
    title: "Water the plants",
    body: "It's been a day.",
    url: "myapp://plants",
    after: 24h,
  ))
}
```

- `notify.request()` 第一次调用时会请求用户允许通知，并返回通知是否已被允许：可能是用户当时的回答，也可能是之后在“设置”中修改过的结果。
- `notify.schedule(n)` 安排 `n` 在 `n.after` 之后显示（至少一秒）；`notify.show(n)` 则立即显示。如果 `id` 与某条等待中或正在显示的通知相同，就会替换掉它。两者都是异步的。
- `notify.cancel(id)` 移除一条通知，无论它还在等待还是已经显示。

`Notification` 的 `url` 应该使用应用自己的 scheme（`asuka.toml` 中的 `scheme`）：用户点按通知时，应用的 `openURL` 会收到这个 URL。其他字段如下：

| 字段 | 作用 |
|---|---|
| `subtitle` | 标题下方的一行（iOS）；在 Android 上是应用名旁边的子文本（sub text） |
| `data` | 应用自己的键值数据，会在 `showing`、`responses` 和 `present` 中原样返回 |
| `category` | 某个类别的 ID，决定显示哪些操作按钮（见下文） |
| `thread` | 把它和同一会话的其他通知归为一组：iOS 上是 thread，Android 上是一个通知组，组的摘要通知由 Asuka 维护 |
| `summary` | Android 通知组摘要下方的那行文字（"3 new messages"）；iOS 会自己生成 |
| `channel` | 它所属的 Android 渠道（见下文）；iOS 没有渠道 |
| `badge` | 通知显示时应用图标上的数字（iOS、macOS）；在 Android 上是通知的数字 |
| `silent` | 不发出声音 |
| `image` | 一张图片，来源与 `Image` 相同（资源、文件、http(s) URL）：在 iOS 上作为附件，在 Android 上作为大图（big picture） |

**iOS** 在应用打开时也会以横幅形式显示通知，除非 `present` 另有指定。**Android** 用非精确闹钟（inexact alarm）发出定时通知（因此不需要精确闹钟权限），所以通知可能会比设定的时间稍晚一点出现。

### 操作与回复

类别（category）代表一种通知以及它的按钮。请在应用启动时设置好它的类别，并且要赶在引用这些类别的通知到来之前（推送也一样：iOS 上是 `aps.category`，Android 上是 data 中的 `"category"`）。类别设置在应用多次启动之间会一直保留。

```asuka
import platform/notify

component Chats {
  task {
    notify.setCategories([notify.Category(id: "message", actions: [
      notify.Action(id: "reply", title: "Reply", icon: "arrowshape.turn.up.left",
        reply: notify.Reply(button: "Send", placeholder: "Message")),
      notify.Action(id: "read", title: "Mark as Read"),
      notify.Action(id: "delete", title: "Delete", destructive: true, unlock: true),
      notify.Action(id: "open", title: "Open", open: true),
    ])])

    for await r in notify.responses() {
      match r.action {
        "" => openChat(r.notification.thread)   // 点按了通知本身
        "reply" => {
          let done = r.defer()                   // 发送需要一点时间
          defer { done() }
          try? await send(r.notification.thread, r.reply)
        }
        "read" => notify.removeThread(r.notification.thread)
        _ => {}
      }
    }
  }

  Text("Chats")
}

// 应用自己的函数。
fn openChat(thread: string) {}
async fn send(thread: string, text: string) throws {}

app { Chats() }
```

- `Action` 包含 `id` 和 `title`，以及 `icon`（一个 SF Symbol；iOS 15 和 macOS 12 会显示它，Android 显示操作时不带图标）、`destructive`（iOS 会把它显示成红色）、`unlock`（先解锁设备：iOS，以及 Android 12 及更高版本）、`open`（打开应用）和 `reply`（一个文本框，可以设置发送按钮的标题和占位文字）。
- `notify.responses()` 是用户操作的流：可能是点按通知本身（`action` 为 `""`），也可能是某个操作；`reply` 是用户输入的文本，`notification` 是被响应的那条通知（也包括 `platform/push` 的通知）。在有人读取这个流之前就到达的响应（启动或唤醒应用的那一个）会等待第一个读取者，所以请在应用启动时、在根组件的一个 task 中读取它。
- 没有设置 `open` 的操作会在后台运行：应用会运行（如果应用没在运行，会在没有界面的情况下启动它：运行它的根组件和那个 task），但不会切换到前台。如果读取响应之后还有工作要做，就调用 `r.defer()`，并在工作完成时调用它返回的函数：系统会让应用一直运行到那时，或者运行到时间用完为止（iOS 约 30 秒，Android 约 10 秒）。

**Android** 会像 iOS 一样，在后台操作结束后移除对应的通知，除非应用在处理完成前再次显示了它（调用 `show`，并使用同一个 `id`）。用户回复之后，Android 要求重新发布这条通知（在此之前，通知上的加载动画会一直转）：对于对话类通知，Asuka 会以用户的名字把回复追加进去；其他通知则直接移除。

### 会话分组与当前显示的通知

`thread` 把一个聊天的通知归为一组：iOS 会把它们叠在一起，Android 会把它们放进一个带摘要通知的组里，并由 Asuka 随着通知的增减来维护这个摘要。

- `notify.showing()` 按从新到旧的顺序，列出应用当前显示着的通知（在通知中心或通知栏中），包括推送来的通知：每个 `Delivered` 包含 `id`、`title`、`subtitle`、`body`、`url`、`data`、`category`、`thread`、`time`（一个 `time.Time`），推送通知还有 `remote`。
- `notify.remove(ids)`、`notify.removeThread(thread)`（适用于已读的聊天）和 `notify.removeAll()` 用来移除通知。

### 应用打开时显示哪些通知

```asuka
import platform/notify

component Chat(id: string) {
  effect(id) {
    notify.present(n => if n.thread == id { .hide } else { .banner })   // 这个聊天正显示在屏幕上
    cleanup { notify.stopPresenting() }
  }

  Text("Chat")
}
```

`notify.present(decide)` 让 `decide` 决定应用在前台时收到的每条通知该如何显示：`.banner`（和应用关闭时一样）、`.quiet`（只进入通知中心或通知栏，没有横幅也没有声音），或者 `.hide`。它会在通知到达时被调用，并且必须立即给出结果：Asuka 最多只等一秒，超时就直接显示。`notify.stopPresenting()` 恢复为全部显示。

### 对话中的消息

消息会以平台展示消息的方式显示，并带上发送者的头像：在 iOS 上是通讯通知（communication notification），在 Android 上是对话（显示在通知栏的“对话”区域，并附带该对话的快捷方式）。

```asuka
import platform/notify

async fn whosIn() throws {
  let ana = notify.Person(id: "u7", name: "Ana", image: "https://example.com/ana.jpg")
  let chat = notify.Conversation(id: "climbing", title: "Climbing", group: true)
  try await notify.show(notify.Notification(body: "Who's in for Saturday?", category: "message").from(ana, chat))
}
```

- `n.from(sender, c)` 把 `n` 变成一条消息：`sender` 在 `c` 中说了 `n.body`。它的 `id` 和 `thread` 默认为对话的 `id`（一对一对话的 ID 就是发送者的 ID），`title` 默认为发送者的名字（群组则为群组的标题）。
- `Person` 包含 `id`（每次都要相同）、`name`、`image`（来源与 `Image` 相同）和 `bot`。`Conversation` 包含 `id`、群组的 `title` 和 `image`、`group`、`me`（用户本人在对话中显示的身份；Android 用它来标注用户自己的回复，默认为 "You"），以及 `earlier`（之前的消息，每条是一个 `Message`：`from`、`text`、`time`，`time` 是 `time.Time?`，未知时为 none）。
- **Android** 把一个对话显示为一条通知，所有消息都在其中：新消息会追加到当前显示着的那条对话通知里（或者由 `earlier` 设定它前面的内容）。**iOS** 则把每条消息显示为一条独立的通知，归入该对话的分组。
- **iOS** 需要在 App ID 上开启 Communication Notifications capability：调用了 `from` 的应用会自动获得相应的 entitlement 以及 Info.plist 中的 `NSUserActivityTypes`（`INSendMessageIntent`），发布构建的描述文件也必须包含这项 capability。否则消息会以普通通知的形式显示。**macOS** 则总是把消息显示为普通通知。

### Android 的通知渠道

渠道（channel）代表一类通知，用户可以在应用的设置中关闭某个渠道或把它设为静音。Android 按照通知所在渠道的声音、振动和重要程度来显示它；iOS 没有渠道，会忽略这些设置。

```asuka
import platform/notify

fn makeChannels() {
  notify.setChannelGroups([notify.ChannelGroup(id: "chats", name: "Chats")])
  notify.setChannels([
    notify.Channel(id: "messages", name: "Messages", importance: .high, group: "chats"),
    notify.Channel(id: "digest", name: "Daily digest", importance: .low, noBadge: true),
  ])
}
```

- `notify.setChannels(cs)` 会创建渠道，为之前创建过的渠道更新名称，并删除之前由它创建、但这次不在 `cs` 中的渠道。渠道的 `importance`（`.default`；`.high`：悬浮通知；`.low`：没有声音；`.min`）、`silent`、`noVibration` 和 `noBadge` 只在创建时生效，之后就交由用户来修改。`notify.setChannelGroups(gs)` 对渠道组做同样的事（删除一个组会连同删除其中的渠道）。
- 没有指定 `channel`、或者指定的渠道不存在的通知，会归入默认渠道（"Notifications"），这个渠道始终保留。推送通过 FCM 的 `android.notification.channel_id` 指定渠道，数据消息则通过 `"channel"` 指定。

### 角标

`notify.setBadge(n)` 设置应用图标上的数字；传入 `0` 则去掉角标。**iOS** 和 **macOS**（显示在程序坞上）会照你设定的数字显示。**Android** 没有自己的角标：支持角标的启动器会统计应用的通知数量（或者只显示一个圆点），所以通知被移除后（`remove`、`removeThread`）角标会随之减少，而 `setBadge` 什么也不做；通知的 `badge` 是启动器可能会显示的数字。设置了 `noBadge` 的渠道中的通知不计入角标。

### 各平台的情况

| | iOS | Android | macOS | Windows | Linux | Web |
|---|---|---|---|---|---|---|
| 显示、定时、取消 | ✓ | ✓ | ✓ | ✓（toast） | ✓（定时：用 systemd 用户定时器，无论应用是否运行） | ✓（定时：仅在应用有页面打开时） |
| 操作、回复 | ✓ | ✓ | ✓ | ✓（最多五个按钮） | 支持操作（回复会打开应用） | 浏览器显示按钮时可用（Chrome、Edge：两个）；支持内联回复的浏览器可以回复 |
| 后台操作 | ✓（约 25 秒） | ✓（约 9 秒） | ✓ | ✓（约 25 秒；会为此启动应用） | 仅在应用运行时 | 在应用的页面中运行（由 worker 打开一个页面） |
| 会话分组 | ✓ | ✓（一个组及其摘要） | ✓ | 不分组；支持 `removeThread` | ✓ | 不分组；支持 `removeThread` |
| 渠道 | 忽略 | ✓ | 忽略 | 忽略 | 忽略 | 忽略 |
| 对话 | ✓（通讯通知） | ✓（MessagingStyle、快捷方式） | 普通通知 | 普通通知 | 普通通知 | 普通通知，以发送者头像作为图标 |
| 角标 | ✓ | 由启动器决定 | ✓（程序坞） | 任务栏按钮上的叠加图标 | 启动器 API 提供的角标（Dash to Dock、Plasma） | 已安装应用的角标（`navigator.setAppBadge`） |
| 列出当前显示的通知、移除 | ✓ | ✓ | ✓ | ✓（通知中心中的） | 仅限应用本次运行中显示过的 | ✓（service worker 显示的） |
| `present` | ✓ | ✓ | ✓ | ✓（定时通知除外） | ✓ | ✓ |

**macOS** 需要应用的 bundle，而 `asuka dev` 和 `asuka build` 都是从 bundle 启动应用的：单独运行的可执行文件没有 bundle ID，这些调用会抛出说明原因的错误。**Windows** 显示 toast 通知，可以立即显示，也可以定时显示，支持图片，类别中的操作显示为按钮，回复显示为文本框。应用第一次使用通知时会注册自己：在开始菜单中创建一个快捷方式，带上应用的 AppUserModelID（即 asuka.toml 中的 `id`），并在用户的注册表中登记一个 COM 激活器（也就是应用的可执行文件）。这样，点按通知或按钮时才能把响应送到应用；如果应用没在运行，Windows 会替它启动。如果是后台操作，应用会保持不可见，并在响应处理完后退出。角标是画在应用任务栏按钮上的一个数字（打包应用则使用系统的角标）。无论 `present` 怎么决定，Windows 都会照常显示定时的 toast。**Linux** 上，如果应用有 desktop 文件（或运行在沙盒中），通知会以 GNotification 的形式发送，否则直接发给桌面的通知服务器（GNOME Shell 会拒绝没有 desktop 文件的应用）；应用运行期间，点按通知和按钮都能传到应用。定时通知是一个 systemd 用户定时器（`systemd-run --user`）：到时它会以无窗口的方式运行应用的可执行文件来显示通知，如果应用正在运行就交给应用处理，用户点按时则启动应用；没有 systemd 时（Flatpak、`asuka dev`），定时通知只在应用运行时显示。**浏览器**通过应用的 service worker 显示通知，`platform/push` 则通过 Web Push 实现：详见 [Web](web.md)。

## 播放声音：platform/audio

`audio.play(source)` 把一段声音播放一次：可以是应用 assets 里的文件、文件路径或 http(s) URL。`audio.Player(source)` 是一个 store，持有一段声音，创建后即可播放：`play`、`pause`、`seek(seconds:)`、`setVolume`、`setRate`（保持音高，适合语音）、`setLoop`，以及表示当前进度的 `status()`（`position`、`duration`、`playing`、`ended`）。创建它的组件消失时，或者调用 `release()` 时，它会停止并释放这段声音。

```asuka
import platform/audio

component Episode(url: string) {
  state player = audio.Player(url)
  state status = audio.Status()

  task {
    while true {
      status = await player.status()
      await sleep(250ms)
    }
  }

  Column {
    Text("{status.position.fixed(0)} / {status.duration.fixed(0)} s")
    Button(if status.playing { "Pause" } else { "Play" }) {
      on press { if status.playing { player.pause() } else { player.play() } }
    }
    Button("Ding") { on press { audio.play("ding.wav") } }
  }
}
```

在 iOS 和 Mac 上用的是 AVPlayer，在 Android 上是 Media3 的 ExoPlayer，在浏览器中是 `<audio>`，在 Windows 上是 MCI，在 Linux 上是 GStreamer 的 `playbin`。只有当应用声明自己正在播放媒体时，锁屏后声音才会继续播放：见下面的“正在播放”。

### 正在播放与媒体键：platform/nowplaying

`nowplaying.NowPlaying` 在它显示期间告诉系统应用正在播放什么：iOS 锁屏和控制中心里的播放器，Android 的媒体通知（一个前台服务，所以能一直播放下去），Mac 的“正在播放”小组件，Windows 的媒体浮出控件（SMTC），Linux 上的 MPRIS（GNOME 的媒体控件），浏览器中的 Media Session API。它的事件就是系统发来的命令：媒体键、耳机按键、锁屏上的按钮。给它一个 `player`，它就会跟着播放器的进度走，并且自己去播放和暂停。

```asuka
import platform/audio
import platform/nowplaying

component Playing(url: string) {
  state player = audio.Player(url)

  Column {
    nowplaying.NowPlaying {
      title: "Episode 12"
      artist: "The Show"
      artwork: "cover.jpg"
      player: player
      skipInterval: 15
      on next { player.seek(0) }
    }
    Button("Play") { on press { player.play() } }
  }
}
```

不给 `player` 时，就自己提供 `state`（`.playing`、`.paused`）、`duration` 和 `position`，并处理 `play`、`pause`、`seek(position)`、`skipForward(seconds)` 和 `skipBackward(seconds)`。同时显示了好几个时，系统显示的是最后出现的那个。导入它的应用会自动带上 iOS 的 `audio` 后台模式和 Android 的前台服务权限。

## 网络状态：platform/network

`platform/network` 反映设备的网络状况：是否联网，走的是 Wi-Fi 还是蜂窝网络，以及连接是否计费。你可以用它实现“仅在 Wi-Fi 下下载大文件”，或者提示用户应用当前处于离线状态。它在 iOS 和 Mac 上基于 NWPathMonitor，在 Android 上基于 ConnectivityManager，在 Windows 上基于 Network List Manager，在 Linux 上基于 GNetworkMonitor 和 NetworkManager，在浏览器中基于 `navigator.onLine`；浏览器只有在能拿到信息的地方（Android 上的 Chrome）才会告诉你连接类型。

`network.status()` 返回网络状态，并且会被追踪：在绑定中读取它时，它会跟随网络的变化。只有在有东西读取它的期间，才会去监听网络。

```asuka
import platform/network

component NetworkBadge {
  let net = network.status()

  Text(if !net.connected { "Offline" } else if net.metered || net.constrained { "{net.type}: large downloads wait for Wi-Fi" } else { "{net.type}" })
}
```

`network.Status` 包含：

- `connected`：是否已联网。
- `type`：`.none`、`.wifi`、`.cellular`、`.ethernet` 或 `.other`（只有 VPN、蓝牙等）；转成文本时就是它的名字（`"wifi"`）。
- `metered`：连接会产生费用或有流量限制，比如移动数据或手机热点。
- `constrained`：用户要求应用少用流量（iOS 的低数据模式、Android 的流量节省程序、浏览器的省流量模式）；在 Windows 上，也可能是连接接近或超出了流量上限。在 Linux 上永远为 false。

在平台第一次给出结果之前，以及在平台不提供网络信息的地方，它的值都是已连接（`connected`），`type` 为 `.other`。

`network.current()` 返回此刻的网络状态，不会被追踪；`network.changes()` 则是网络状态的流：先给出当前状态，之后每次变化时再给出一次。

## 保存到媒体库：platform/library

`platform/library` 把文件保存到用户和其他应用都能找到的地方：视频存进相册，歌曲存进音乐，任何文件都可以存进“下载”。

```asuka
import platform/library

async fn keep(file: string) -> string {
  do {
    let _ = try await library.save(file, collection: .movies, folder: "My App", move: true)
    return "Saved"
  } catch e {
    return "Couldn't save: {e.message}"
  }
}
```

`library.save(path, collection:, name:, folder:, mime:, move:)` 在主线程之外把文件复制到指定的集合中（大视频可能要花一些时间），并返回文件存到了哪里。第一次保存时，它会请求所需的权限。

- `collection`：`.downloads`（默认）、`.movies`、`.music` 或 `.pictures`。
- `name`：文件保存后的名字（默认沿用原名）。如果重名，会自动加上编号。
- `folder`：在该集合中专门存放应用文件的文件夹；留空则直接放进集合本身。
- `mime`：文件类型（默认根据扩展名判断，即 `library.mimeOf(name)`）。
- `move`：保存成功后删除原文件。适合下载到应用缓存里的文件，免得多留一份副本。

各集合对应的位置：

| 集合 | Android | iOS | macOS | Windows、Linux |
| --- | --- | --- | --- | --- |
| `.downloads` | “下载”（Downloads） | 应用的 Documents，在“文件”App 中可见 | `~/Downloads` | 用户的“下载”文件夹 |
| `.movies` | 相册中的视频 | “照片” | “照片” | 用户的“视频”文件夹 |
| `.music` | “音乐”（Music） | 应用的 Documents，在“文件”App 中可见 | `~/Music` | 用户的“音乐”文件夹 |
| `.pictures` | 相册中的图片 | “照片” | “照片” | 用户的“图片”文件夹 |

在 Windows 和 Linux 上，以及在 Mac 的 `~/Downloads` 和 `~/Music` 中，文件会放在用户对应文件夹下的 `folder` 里，返回的是文件路径；Linux 上的这些文件夹遵循 XDG 规范。浏览器则不管是哪个集合，都会直接下载文件，文件会出现在浏览器存放下载内容的位置。

**Android** 从 Android 10 开始使用 MediaStore 的集合，不需要任何权限，返回的是一个 `content://` URI。在 Android 10 之前，它会先请求存储权限，再写入共享文件夹。

**iOS** 会请求向“照片”添加内容的权限（仅限添加：应用无法读取照片图库），返回的是该资源的标识符；`folder` 在这里不起作用。其他集合会保存到应用 Documents 下的 `folder` 中，返回的是文件路径；Asuka 会让 Documents 在“文件”App 中可见。

## 后台任务：platform/background

`platform/background` 让应用在不可见时继续工作，比如用户发起的、要花上几分钟的下载、上传或导出。

`background.start(title:, text:, progress:, actions:, onAction:)` 启动一个任务：在调用任务的 `end()` 之前，应用会一直运行，并显示一条通知，展示任务的标题、文字、进度和按钮。用户点按通知上的按钮时，`onAction` 会收到这个按钮的 ID。`update(…)` 用来显示新的内容，比如随着工作推进更新进度。请在应用显示在屏幕上、用户请求这项工作时启动任务。

```asuka
import platform/background

component Export {
  state status = ""
  state cancelled = false

  Column {
    Button("Export") {
      on press {
        cancelled = false
        let buttons = [background.Action(id: "cancel", title: "Cancel")]
        let job = background.start(title: "Exporting", progress: -1, actions: buttons, onAction: a => {
          if a == "cancel" { cancelled = true }
        })
        defer { job.end() }
        var i = 1
        while i <= 10 && !cancelled {
          await sleep(1s)   // 实际要做的工作
          let p = float(i) / 10
          job.update(title: "Exporting", text: "{(p * 100).fixed(0)}%", progress: p, actions: buttons)
          i += 1
        }
        status = if cancelled { "Cancelled" } else { "Exported" }
      }
    }
    Text(status)
  }
}
```

- `title` 和 `text`。
- `progress`：0 到 1；负数表示正在进行，但不知道进度。
- `actions`：通知上的按钮，每个按钮是一个 `background.Action`（由 `id` 和 `title` 组成）。

每次 `update` 都会替换全部内容，所以别忘了把 `actions` 也重新传一遍。实际的工作就是应用自己的异步代码：任务存续期间，它会一直进行下去。[`examples/downloader`](../../examples/downloader) 在任务中执行下载，并支持暂停、继续和取消。

**Android** 会在几分钟内停掉转入后台的应用。任务会让应用在一个前台服务（数据同步类型）中继续运行，每个任务对应一条常驻通知，显示进度和最多三个按钮，同时保持 CPU 唤醒。在 Android 13 及以上版本，如果用户还没有允许通知，启动任务时会请求通知权限；不管用户是否允许，工作都会继续。Android 不允许处于后台的应用启动任务。Android 15 规定数据同步服务每天最多运行六小时。Google Play 会在 Play Console 中询问应用的数据同步前台服务是做什么用的：上传之前请先完成声明。

**iOS** 会在任务存续期间，给转入后台的应用多留大约 30 秒，并且不显示通知：`update` 在 iOS 上什么也不做，`onAction` 也永远不会被调用。时间用完后，iOS 会结束任务，工作可能因此中断。

**桌面端**的应用在不可见时本来就会继续运行：任务的作用是防止系统降低它的速度或把它挂起（Mac 上的 App Nap，Windows 和 Linux 上的睡眠），同样不显示通知：`update` 什么也不做，`onAction` 也永远不会被调用。**浏览器**中，只要页面开着就会一直运行，有没有任务都一样。

## 接收其他应用的分享：onShare

在组件体中写 `onShare(items => …)`，就能在组件存续期间接收其他应用通过分享面板分享给这个应用的内容：比如来自 YouTube 或浏览器的链接、备忘录里的一段文字、相册里的照片、“文件”里的 PDF。`app` 块也可以把它当作事件来接收：`on share(items) { … }`。这是 iOS 和 Android 才有的功能：在桌面端和浏览器中，应用不会出现在其他应用的分享菜单里，也不会收到任何内容。`asuka.toml` 的 `shareTarget` 决定应用可以接收哪些内容：

```toml
shareTarget = true
```

这样写表示接收文本和链接。写成表时还能接收文件，按 MIME 类型指定，一次最多 `max` 个（默认 1 个）：

```toml
[shareTarget]
types = ["text", "image/*", "video/*"]
max = 10
```

类型可以是 `"text"`（文本和网页链接），也可以是 MIME 类型：`"image/*"`、`"video/*"`、`"audio/*"`、`"application/pdf"`，`"*/*"` 表示任意文件。

```asuka
component Shared {
  state link = ""
  state photos: [string] = []

  onShare(items => {
    for it in items {
      match it.kind {
        .link => { link = it.text }
        .image => { photos.push(it.path) }
        _ => {}
      }
    }
  })

  Column {
    Text(link)
    for p in photos key p { Image(p) }
  }
}
```

每个 `SharedItem` 都有一个 `kind`：`.text`、`.link`、`.image`、`.video`、`.audio` 或 `.file`。文本或链接放在 `text` 里。文件则位于 `path`，那是应用缓存中属于应用自己的一份副本：要么把它移到应用存放数据的地方，要么用完后删掉。文件还带有 `name`（它在来源应用中的名字，如 `IMG_0042.HEIC`）、`mime` 类型和 `size`（未知时为 -1）。各项的顺序与来源应用给出的顺序一致。从浏览器分享的网页会变成两项：标题（一条文本）和链接。

在有人接收之前就到达的分享（比如启动应用的那一次）会交给第一个 `onShare`（或应用的 `on share`）。组件的 `onShare` 会在组件消失时停止。如果要为分享的内容打开应用的某个页面，请看[导航](navigation.md)。

**Android** 会根据所列类型生成的 intent filter，带着分享的内容直接打开应用本身（`ACTION_SEND`；`max` 大于 1 时还有 `ACTION_SEND_MULTIPLE`）。在 `onShare` 拿到文件之前，应用会趁着访问权限还在，先把文件复制到自己的缓存中。

**iOS** 上，应用里包含一个分享扩展，它会像 Android 那样带着分享的内容打开应用，自己不显示任何界面。扩展的激活规则由所列类型生成：文本、链接、图片、视频和任意文件使用 Apple 提供的键，`max` 依然有效；其他类型（音频、PDF）则对附件类型做谓词判断，这时 `max` 不起作用。扩展会接收传给它的任何内容，所以请检查 `kind` 和 `mime`。文件通过 App Group（`group.<app ID>`）从扩展传给应用，真机构建所用的描述文件必须包含这个 App Group（见[发布](shipping.md#ios)）。

在 `asuka dev` 的热重载模式下，开发 App 会接收任何内容的分享，不管应用的 `shareTarget` 怎么设置。

链接指向的是什么（网页、图片还是视频），需要应用自己用 `std/http` 去弄清楚，相当于用 Asuka 实现 expo-sharing 的 “resolved” 负载。

### 应用自己的分享面板：platform/sharesheet

默认情况下，分享会打开应用。应用也可以有自己的分享面板：像“保存到 Raindrop”或“添加到备忘录”那样，在发起分享的应用上方显示一个页面，用户保存之后就回到原来所在的地方。借助 `platform/sharesheet`，app 块可以在应用作为分享面板运行时显示这个面板：

```asuka
import platform/sharesheet

component SaveLink {
  state link = ""
  state note = ""

  onShare(items => {
    for it in items {
      if it.kind == .link {
        link = it.text
      }
    }
  })

  Column {
    padding: 16; gap: 12; grow: 1
    if link.isEmpty() {
      ProgressRing()
    } else {
      Text(link)
    }
    TextField { bind text: note; placeholder: "Note" }
    Row {
      gap: 12
      Button("Cancel") {
        on press { sharesheet.cancel() }
      }
      Button("Save") {
        variant: .filled
        on press {
          save(link, note)
          sharesheet.done()
        }
      }
    }
  }
}

component Root {
  Text("Links")
}

// 应用自己的函数。
fn save(link: string, note: string) {}

app {
  if sharesheet.active() {
    SaveLink()
  } else {
    Root()
  }
}
```

应用作为自己的分享面板运行时，`sharesheet.active()` 为 true：这时根视图就是面板，应用的其余部分不会创建。面板和应用一样，通过 `onShare` 接收分享的内容；内容还没到齐时（比如视频还在复制），可以先显示一个加载指示器。面板通过 `sharesheet.done()`、`sharesheet.cancel()` 或 `sharesheet.openApp()` 关闭；`openApp()` 会带着分享的内容打开应用，交给应用的 `onShare`，和没有面板时一样。面板会出现在哪些内容的分享菜单里，仍由 `asuka.toml` 的 `shareTarget` 决定（见上文）；如果应用用了 `active()` 却没有设置 `shareTarget`，就不会出现在任何分享菜单里（CLI 会给出提示）。

面板是一个独立的根视图，运行在独立的进程中，这和 iOS 运行分享扩展的方式一致：在 **iOS** 上，它就是应用的分享扩展，由 iOS 以弹出面板的形式显示，运行的是它与应用共用的框架中的应用代码；在 **Android** 上，它是运行在 `:share` 进程中的一个 Activity，以 Material 底部面板的形式显示在另一个应用上方（向下拖动、点按面板外部或返回都会取消分享）。面板关闭时，进程也随之结束。文件的 `path` 只在面板关闭之前有效：应用要保留的内容，请先复制出来。

应用保存的数据在面板里也一样能访问：它的文件（`std/fs` 的应用目录，以及其中的 SQLite 数据库）、`std/storage` 的值、`platform/secure`。Android 上两个进程共享应用的文件。iOS 上，应用第一次带着面板运行时，它的文件和值会一次性移到 App Group（`group.<app ID>`）中，钥匙串访问组也会与扩展共享；真机构建所用的描述文件需要开启 App Groups 功能（见[发布](shipping.md#ios)）。应用运行期间，不会收到面板写入数据的通知：等它回到前台时再重新读取。

应用用到了 `sharesheet.active` 时，Asuka 会构建这个面板，取代那个只负责打开应用的扩展。这时在 iOS 上，应用的代码会打包成一个框架，应用和它的扩展都加载这个框架，就像 Xcode 让扩展共享代码那样：应用不会因为有了面板而变大。在桌面平台和浏览器中，应用不会出现在其他应用的分享菜单里：`active()` 在这些平台上为 false。

在测试中，`sharesheet.fake()` 会让应用作为分享面板运行（`mountApp()` 显示的就是面板），并记下面板是怎样关闭的（`closed`）；挂载后的应用调用 `device.share(items)`，就相当于另一个应用分享了这些内容：

```asuka
import platform/sharesheet

component SaveLink {
  state link = ""

  onShare(items => {
    link = items[0].text
  })

  Column {
    Text(link)
    Button("Save") {
      on press { sharesheet.done() }
    }
  }
}

app {
  if sharesheet.active() {
    SaveLink()
  } else {
    Text("Links")
  }
}

test "the sheet saves" {
  let sheet = sharesheet.fake()
  let app = mountApp()
  app.share([SharedItem(kind: .link, text: "https://asuka.dev")])
  app.expect("https://asuka.dev")
  app.tap("Save")
  expect(sheet.closed == .done)
}
```

## 运行 JavaScript：platform/js

`platform/js` 在屏幕之外、用平台自带的引擎运行 JavaScript：在一个从不显示的网页视图里（iOS 和 Mac 上是 WKWebView，Android 上是 WebView，Windows 上是 WebView2，Linux 上是 WebKitGTK），在浏览器中则是一个隐藏的沙箱 iframe。它适用于那些只有 JavaScript 版本的代码，比如某个网站的播放器里用来给视频 URL 签名的函数。

`js.eval(script)` 在一个全新的独立页面中运行脚本（语句的执行方式与 `<script>` 中相同），并以 JSON 返回它的值：也就是最后一条语句的值；如果脚本返回一个 promise，则是 promise 解析出的结果（`undefined` 会变成 `null`）。抛出的错误或被 reject 的 promise 会成为 `js.eval` 抛出的错误，带有脚本给出的消息。脚本运行完，页面也就销毁了：一个脚本设置的东西，下一个脚本看不到。等待期间它不会占住主线程。

```asuka
import platform/js
import std/json

async fn doubled() throws -> [int] {
  let v = try await js.eval("const xs = [1, 2, 3].map(x => x * 2); xs")   // "[2,4,6]"
  return try json.decode<[int]>(v)
}

async fn later() throws -> string {
  let v = try await js.eval("new Promise(done => setTimeout(() => done('later'), 100))", timeout: 10s)
  return try json.decode<string>(v)
}
```

脚本最多可以运行 `timeout` 指定的时间（默认一分钟）；超时的脚本会抛出 `js.JsError.timeout`。取消 task（比如启动它的页面消失了）会释放它的页面。

如果要在用户能看到的页面里运行脚本，请使用 `WebView`（见[网页](components.md#网页)）。

## 推送通知：platform/push

`push.register()` 会（在第一次调用时）请求用户允许通知，向 Apple（APNs）或 Google（Firebase Cloud Messaging）的推送服务注册设备，然后返回一个 `Registration`：其中有设备的 `token`、所用的 `service`（`"apns"`、`"fcm"`），以及通知是否已获允许（`allowed`）。把这个令牌发给你的服务器，由服务器向它发送通知；令牌可能会变，所以每次启动都要注册一次。Mac 和 iOS 一样通过 APNs 注册（需要描述文件，见[在 Mac 上](#在-mac-上)）。在 Web 上用的是 Web Push（服务为 `"webpush"`，令牌就是浏览器的推送订阅，见 [Web](web.md)）。Windows 和 Linux 没有可供应用使用的推送服务：`register` 在这两个平台上会抛出错误。

```asuka
import platform/push

component Root {
  task {
    if let r = try? await push.register() {
      try? await saveDeviceToken(r.service, r.token)
    }
  }
  task {
    for await m in push.opened() { openThread(m.data["thread"] ?? "") }
  }

  Text("Inbox")
}

// 应用自己的函数。
async fn saveDeviceToken(service: string, token: string) throws {}
fn openThread(id: string) {}

app { Root() }
```

- `push.received()` 是应用运行期间到达的消息的流。如果消息带有标题或正文，它也会以通知的形式显示出来，应用在前台时也是如此。数据消息（静默推送）只会送到这里。
- `push.opened()` 是用户点按的通知的流，其中也包括启动应用的那一条，它会等待第一个读取者：所以请在应用启动时就读取它。和 `platform/notify` 的通知一样，消息数据中的 `"url"` 也会交给应用的 `openURL`。
- `push.Message` 包含 `id`、`title`、`body` 和 `data`（iOS 上是负载中 `aps` 以外的键；Android 上是消息的 data）。

iOS 的发布构建需要在 App ID 上开启 Push Notifications 功能；在模拟器上也能拿到令牌。Android 需要一个 Firebase 项目：把 Android 应用添加进去，下载它的 `google-services.json`，然后在 `asuka.toml` 中指定这个文件：

```toml
[android]
googleServices = "google-services.json"
```

Asuka 会像 Google 的 Gradle 插件那样，把其中的配置写进应用。缺少这个文件时，`register` 会抛出错误，并告诉你该怎么做。

### 推送中的类别、分组和渠道

推送可以使用 `platform/notify` 的类别（及其操作和 `notify.responses()`）、分组、渠道、角标和 `present`，只需在负载中指定：

- **iOS**：`aps` 中的 `category`、`thread-id` 和 `badge`，与 APNs 的定义一致。
- **Android**：通知消息中的 `android.notification.channel_id` 和 `image`；数据消息中的 `"title"`、`"body"`、`"subtitle"`、`"category"`、`"thread"`、`"summary"`、`"channel"`、`"image"`、`"badge"` 和 `"url"`。带有 `"title"` 或 `"body"` 的数据消息会显示为通知（由 Asuka 构建，包括其中的操作）；两者都没有的则是静默消息（只交给 `received()`）。应用在后台时收到的通知消息由 Firebase 显示，既没有操作也不会分组：需要这些功能时，请改发数据消息。

`push.Message` 也包含这些字段：`subtitle`、`category`、`thread`、`channel`、`image`、`badge`、`summary`。

### 在推送显示之前改写它

`push.rewrite(f)` 让应用在每条推送显示之前修改它：解密端到端加密的负载，设置标题和正文，附上图片，把它变成一条带有发送者头像的会话消息，设置它的分组和类别。在应用启动时调用：

```asuka
import platform/push
import platform/notify

async fn reveal(m: push.Message) throws -> push.Message? {
  let box = m.data["box"] ?? ""
  if box.isEmpty() {
    return none   // 不是我们的：按原样显示
  }
  let msg = try await decrypt(box)   // 密钥保存在 platform/secure 中
  var out = m
  out.body = msg.text
  out.category = "message"
  return out.from(notify.Person(id: msg.fromID, name: msg.fromName, image: msg.avatar),
    notify.Conversation(id: m.data["chat"] ?? ""))
}

component Root {
  task {
    push.rewrite(reveal)
  }

  Text("Chats")
}

// 应用自己的类型和函数。
struct Plain {
  text: string
  fromID: string
  fromName: string
  avatar: string
}

async fn decrypt(box: string) throws -> Plain {
  return Plain(text: box, fromID: "ana", fromName: "Ana", avatar: "")
}

app { Root() }
```

`f` 返回要显示的推送；返回 `none` 或抛出错误时，推送会按收到时的样子显示。推送一条一条地改写，顺序与到达的顺序一致。`image` 会被下载并附在通知上；`from` 把它变成一条会话消息，和 `notify.Notification` 的 `from` 一样。调用 `push.stopRewriting()` 之后，推送又会按原样显示。

**iOS** 在 Notification Service Extension 中改写推送，`asuka build` 会为用到 `push.rewrite` 的应用生成这个扩展：`aps` 中带有 `"mutable-content": 1`（以及 `alert`，改写失败时显示的就是它）的每条推送，iOS 都会启动它。它会像在后台启动那样，不带屏幕地运行应用；等应用调用了 `push.rewrite`，改写才会执行。所以要让根视图在启动时做的事情尽量轻量，或者让扩展里的根视图只负责调用它（在扩展中 `push.inExtension()` 为 true：`if push.inExtension() { Rewrites() } else { Root() }`）。iOS 给它大约 30 秒和 24 MB 内存，超时后推送会按原样显示。应用保存的数据在扩展里也一样：它的文件和 `std/storage` 的值都在两者共享的 App Group 中，钥匙串项（`platform/secure`）在应用的访问组中。发布构建需要一个对应扩展 bundle ID 的描述文件，这个 ID 是应用的 ID 加上 `.notification`（可以用 `asuka.toml` 的 `ios.notificationProfile` 指定；默认选用已安装的最匹配的那个），并且要开启 App Groups 功能、包含应用的 App Group。在 iOS 上，改写无法隐藏一条推送。模拟器只会为来自 APNs 的推送运行这个扩展，`xcrun simctl push` 发出的推送不会经过它。

**Android** 在应用的进程中改写 FCM 的数据消息：如果应用没有运行，会先不带屏幕地启动它，在大约十秒之内完成改写，然后显示通知。改写之后既没有标题也没有正文的数据消息不会显示。应用在后台时收到的通知消息由 Firebase 直接显示，不会经过改写：请改发数据消息。

**Web** 上由前台的页面在 service worker 显示推送之前改写它；没有打开的页面时，推送按原样显示。

在测试中，`push.fakeRewrite(m)` 返回一条到达的推送最终会显示的样子：改写后的推送，或者 `m` 本身。

## 通过 Apple 登录：platform/appleauth

`appleauth.SignInButton` 是 Apple 官方的登录按钮（标题、标志和配色都是 Apple 的，并使用用户的语言），`appleauth.signIn` 会弹出系统的登录面板。如果应用的账号支持用其他公司的账号登录，App Store 就要求它同时提供“通过 Apple 登录”。

```asuka
import platform/appleauth

component SignIn {
  state status = ""

  Column {
    appleauth.SignInButton {
      type: .continueWithApple
      variant: .automatic   // 浅色模式下为黑色，深色模式下为白色
      height: 44
      on press {
        do {
          let c = try await appleauth.signIn(scopes: [.fullName, .email], nonce: "n-0S6_WzA2Mj")
          try await signInWithApple(c.identityToken, c.authorizationCode)
        } catch appleauth.AppleAuthError.canceled {
        } catch e {
          status = e.message
        }
      }
    }
    Text(status)
  }
}

// 由应用自己实现：它的服务器向 Apple 验证令牌。
async fn signInWithApple(token: string, code: string) throws {}
```

`Credential` 的 `user` 是账号的稳定 ID；把 `identityToken`（由 Apple 签名的 JWT）和 `authorizationCode` 发给你的服务器，由服务器向 Apple 验证。`email`、`givenName` 和 `familyName` 只在用户第一次登录这个应用时提供：务必在那时保存下来。`AppleAuthError.canceled` 表示用户关闭了登录面板。`appleauth.credentialState(user)` 告诉你用户是否仍在这个应用上使用“通过 Apple 登录”（如果是 `.revoked`，就让用户退出登录）。Android 上没有这项功能：`SignInButton` 什么也不显示，`available()` 为 false，`signIn` 会抛出 `AppleAuthError.notAvailable`；Windows、Linux 和 Web 上也是如此。在这些平台上，可以通过网页完成“通过 Apple 登录”（`platform/webauth`）。Mac 和 iOS 一样支持这项功能，但需要描述文件（见[在 Mac 上](#在-mac-上)）。在测试中，`appleauth.fake()` 会替用户作答，答案由它的 `credential`、`cancel` 和 `states` 决定。

## 通过 Google 登录：platform/googleauth

`googleauth.signIn(clients, scopes:, nonce:)` 用用户的 Google 账号登录，和 Google 自己的 SDK 一样，给出一个 `GoogleUser`：它的 `id`、`email`、`name`、`givenName`、`familyName` 和 `picture`，给应用服务器用的 `idToken`，以及在个人资料之外请求的 `scopes` 对应的 `accessToken`。返回 `none` 表示用户取消了。

```asuka check
import platform/googleauth

let clients = googleauth.Clients(
  ios: "1234-ios.apps.googleusercontent.com",
  web: "1234-web.apps.googleusercontent.com",
  webRedirect: "https://example.com/",
  desktop: "1234-desktop.apps.googleusercontent.com",
  desktopSecret: "GOCSPX-example"
)

component SignIn {
  state who = ""

  Column {
    gap: 12
    Button("Sign in with Google") {
      on press {
        do {
          if let user = try await googleauth.signIn(clients) {
            who = user.email
            // Send user.idToken to the app's server, which verifies it.
          }
        } catch e {
          who = e.message
        }
      }
    }
    Text(who)
  }
}
```

各平台用各自的方式登录，各用一个在 Google Cloud 控制台创建的对应类型的 OAuth 客户端（在 `Clients` 里给出）：

| | 方式 | 客户端 |
| --- | --- | --- |
| Android | Credential Manager 的账号选择器（系统的）；scopes 通过 Google 的 Authorization API 请求 | ID 令牌用 Web 客户端的 ID（`web`）；还必须存在一个带应用包名和签名证书的 Android 客户端 |
| iOS、macOS | 在系统的登录面板（`platform/webauth`）里打开 Google 的登录页面，使用 PKCE | iOS 客户端（`ios`）；它的 ID 反过来写就是回调的 scheme |
| Windows、Linux | 在 `platform/webauth` 的窗口里打开登录页面，回到应用自己在 127.0.0.1 上的服务器（RFC 8252） | 桌面客户端及其密钥（`desktop`、`desktopSecret`：已安装应用的密钥，并不保密） |
| Web | 一个弹出窗口，回到应用的页面 | Web 客户端（`web`），`webRedirect` 是它已授权的重定向 URI 之一 |
| 电视（Apple TV、Android TV） | `signInWithCode`：应用显示一个网址和一个代码，用户在手机或电脑上输入（Google 为电视和输入受限设备提供的登录方式） | “电视和输入受限设备”类型的客户端及其密钥（`tv`、`tvSecret`） |

ID 令牌是 Google 签名的 JWT：应用的服务器要先验证它（用 Google 的公钥验证签名，检查 `aud` 是应用的客户端、`iss`、`exp`，以及服务器交给应用的 `nonce`），才能相信它所说的身份。`Clients` 里没有当前平台的客户端时，会抛出 `GoogleAuthError.notConfigured`。`googleauth.signOut()` 让 Android 的选择器下次重新询问；其他平台上，会话由浏览器保留。

电视上没有可以弹出的登录面板，所以在那里 `signIn` 会抛出错误：请改用 `googleauth.signInWithCode(clients, show:, scopes:)` 登录。它的 `show` 会收到要显示给用户的 `DeviceCode`（`url`、`code`，以及代码的有效时长 `expires`），然后一直等待用户在另一台设备上输入代码。用户在那边同意后，它返回账号；用户拒绝时返回 `none`；代码在用户输入前过期，则抛出 `GoogleAuthError.expired`，再调用一次就能拿到新的代码。它在任何平台上都能用，只要提供 `tv` 客户端；Google 允许它请求个人资料的 scopes，以及少数其他 scopes（应用自己创建的 Drive 文件、YouTube）。

```asuka check
import platform/googleauth

let clients = googleauth.Clients(tv: "1234-tv.apps.googleusercontent.com", tvSecret: "GOCSPX-example")

component TVSignIn {
  state prompt = ""

  Column {
    gap: 24
    Button("通过 Google 登录") {
      on press {
        do {
          let user = try await googleauth.signInWithCode(clients, show: c => {
            prompt = "请在手机上打开 {c.url}，输入 {c.code}"
          })
          if let u = user {
            prompt = u.email
            // 把 u.idToken 发给应用的服务器，由服务器验证。
          }
        } catch e {
          prompt = e.message
        }
      }
    }
    Text(prompt) { font: .title2 }
  }
}
```

## 钱包支付：platform/pay

`platform/pay` 用平台自带的钱包收款：在 iOS、macOS 和 Safari 里是 Apple Pay，在 Android 和其他浏览器里是 Google Pay。它适用于实体商品和服务（订单、打车、门票、捐款）；在应用里出售的数字内容要走应用商店的内购。用户在钱包的支付面板里付款，应用拿到钱包给出的支付令牌（token），交给自己的支付处理商（Stripe、Adyen、Braintree 等），由服务器完成扣款。Asuka 自己从不扣款。

```asuka
import platform/pay

fn request() -> pay.Request = pay.Request(
  country: "US",
  currency: "USD",
  items: [pay.Item(label: "Tea set", price: .parse("40.00")!), pay.Item(label: "Tax", price: .parse("3.20")!, kind: .tax)],
  total: pay.Item(label: "Asuka Tea", price: .parse("43.20")!),
  shipping: [.name, .address, .email],
  apple: pay.Apple(merchantId: "merchant.com.example.tea"),
  google: pay.Google(gateway: "stripe", gatewayParams: ["stripe:version": "2018-10-31", "stripe:publishableKey": "pk_test_123"]),
  onShippingContact: c => {
    // 这个地址对应的各项明细和总额。
    if c.address.countryCode != "US" {
      throw pay.Invalid(field: .unserviceable, message: "We ship in the United States only.")
    }
    return pay.Update(total: pay.Item(price: .parse(if c.address.state == "CA" { "43.20" } else { "40.00" })!))
  },
  onAuthorize: p => {
    try await charge(p.token)   // 正常返回：付款成功；抛出的错误会显示在面板里
  },
)

component Checkout {
  state placed = ""
  let wallet = load pay.available(request())

  Column {
    if wallet.value?.canPay ?? false {
      pay.PayButton {
        type: .buy
        variant: .automatic   // 浅色模式下为黑色，深色模式下为白色
        height: 48
        on press {
          do {
            let p = try await pay.pay(request())
            placed = "Thanks, {p.shipping.name}!"
          } catch pay.PayError.canceled {
          } catch e {
            placed = e.message
          }
        }
      }
    }
    Text(placed)
  }
}

// 由应用自己实现：它的服务器用这个令牌扣款。
async fn charge(token: string) throws {}
```

`pay.available(request)` 告诉你钱包对这个请求所接受的卡能做什么：`wallet` 是哪个钱包，`canPay` 表示能付款（这时才显示按钮），`hasCard` 表示用户有一张它接受的卡。只在 `canPay` 成立时显示 `PayButton`。它就是钱包自己的按钮（`PKPaymentButton`、Google 的 `PayButton`，以及 Web 上 Safari 和 Google 提供的按钮），两家的品牌规范都要求应用不要自己画：按钮上的文字（由 `type` 决定：`.buy`、`.checkout`、`.book`、`.donate`、`.order`、`.subscribe`、`.plain`，以及 Apple 独有的其他几种）和标志都使用用户的语言；`variant` 在浅色背景上用 `.black`，在深色背景上用 `.white`，在白色背景上用 `.whiteOutline`，也可以用 `.automatic`。按钮的高度由钱包决定（iOS 上 44 点，Mac 上 30 点，Google 的是 48），宽度由你给定。Apple Pay 里还没有卡时，Apple 的规范是显示 `.setUp` 按钮，点击后调用 `pay.setUp()` 添加一张卡（也可以照常显示购买按钮，它的面板同样能添加卡）；Google Pay 的面板会自己引导用户添加卡。

`pay.pay(request)` 显示支付面板，面板关闭后返回 `Payment`；用户取消时抛出 `PayError.canceled`（已有另一个面板正在显示时抛出 `.busy`）。金额是 [`std/decimal`](stdlib.md) 的 `Decimal`，精确表示，以货币主单位计（`price: .parse("12.75")!`；Google Pay 最多两位小数；折扣用负数），每个平台拿到的都是同一个数（Apple 的 `NSDecimalNumber`，Google 的文本）。这个包的第一个版本用字符串表示金额（`amount: "12.75"`）；在下一个版本之前，`price` 为零时仍会读取它。`total` 的标签是商家名称，和用户账单上显示的一致（Apple Pay 的面板上写作“Pay Asuka Tea”）；`items` 是总额上方的各行明细，其中的 `kind` 供 Google Pay 的面板使用。请求里还可以指定接受哪些卡（`networks`、`cards`、`cardCountries`），收货和账单联系信息里要用户提供哪些（`.name`、`.email`、`.phone`、`.address`），`shippingType` 和 `shippingMethods`（默认选中第一项），Google Pay 可以配送到哪些国家（`shippingCountries`），以及 Apple Pay 的优惠码（`coupons`）。设置 `recurring` 表示这是一笔订阅付款，包括常规扣款和试用期；`deferred` 表示稍后扣款（比如酒店）；`automaticReload` 表示储值余额的自动充值。钱包会把今后的扣款显示给用户，给出的令牌是用于这些扣款的商户令牌（merchant token）。

这些回调都是异步的，运行期间面板会等待（最多大约 30 秒），所以回调里可以请求应用的服务器：用户选择收货地址时调用 `onShippingContact`（付款之前，钱包只提供城市、州（省）、邮编和国家，足够计算税费和可选的配送方式），此外还有 `onShippingMethod`、`onPaymentMethod`（比如对某张卡加收手续费）和 `onCoupon`。每个回调要么返回一个 `Update`（新的明细、总额或配送方式；为 none 的保持不变），要么抛出 `pay.Invalid(field:, message:)`，面板会按它的字段显示出来，字段比如 `.shippingPostalCode`、`.unserviceable`（应用不配送的地址）、`.couponInvalid`；它还可以带上在此期间要显示的更新。一个 `Update` 的 `errors` 可以同时给出多个错误。错误信息要写给用户看。用户付款时（面容 ID、触控 ID 或输入 PIN），`onAuthorize` 会收到 `Payment`：把其中的 `token` 发给服务器，由服务器扣款。它抛出的错误会显示在面板里，用户可以重试；正常完成时面板打勾关闭，`pay` 返回这笔付款，其中包括卡片、联系人信息和所选的配送方式。

这个令牌就是各家支付处理商的 SDK 所发送的内容。Apple Pay 的令牌是 `PKPaymentToken` 的 `paymentData`，一个 JSON 对象（Stripe 把它作为 `pk_token` 接收，Braintree 和 Adyen 作为 Apple Pay 令牌接收），它用商户 ID 的支付处理证书签名，这个证书由支付处理商持有。Google Pay 的令牌由请求中的 `google.gateway` 按网关的参数生成（Stripe 的 publishable key，Adyen 或 Braintree 的商户账号）；也可以用 `directKey` 加密给应用自己的服务器。

Apple Pay 需要一个商户 ID（在 developer.apple.com 的 Identifiers 里创建 Merchant ID），并配上支付处理商提供的支付处理证书，写进 `asuka.toml`：

```toml
[applePay]
merchantIds = ["merchant.com.example.tea"]
```

导入了这个包的应用会获得带有这些商户 ID 的 in-app-payments entitlement；发布构建的描述文件要求应用的 App ID 开启 Apple Pay 能力，并关联同样的商户 ID（Mac 应用也需要描述文件，见[在 Mac 上](#在-mac-上)）。iOS 模拟器里的支付面板用测试卡付款，返回的令牌任何支付处理商都不会扣款。在 Web 上，Apple Pay 要求网站的域名已为该商户 ID 注册，并且要提供 `onValidateMerchant`：由应用的服务器用商户身份证书向 Apple 请求商户会话（merchant session）。Google Pay 的 `TEST` 环境不需要任何账号，返回的是测试令牌；`pay.Google(production: true)` 需要 Google Pay & Wallet Console 里的商户 ID（`merchantId`），并且应用要在那里通过审核。导入了这个包的 Android 应用会带上 Google Pay 的库（`play-services-wallet`），在 manifest 里启用它的 API，并注册它的面板所回调的服务；没导入的应用则不会带上这些。

在 iOS 和 Mac 上，它使用 PassKit 的 `PKPaymentAuthorizationController` 和 `PKPaymentButton`。在 Android 上使用 Google Pay API：`isReadyToPay`，通过 Activity Result API 调用 `loadPaymentData`，动态价格更新和付款授权则通过它的回调服务完成。在浏览器里，如果浏览器支持 Apple Pay（Safari，以及 iPhone 和 iPad 上的浏览器），就通过 Payment Request API 的 Apple Pay 支付方式；其他浏览器使用 Google 自己的 Web API（从 pay.google.com 加载的 `pay.js`），它在所有浏览器里都和 Android 使用同样的请求和回调。要在按钮的点击里调用 `pay`：浏览器只在响应点击时显示支付面板。Windows 和 Linux 上没有可供应用使用的钱包：`available` 返回 `.none`，`pay` 会抛出 `PayError.notAvailable`，`PayButton` 什么也不显示。

测试中用 `pay.fake()` 扮演用户，它返回一个 `FakeWallet`：当 `pay` 正在显示面板时（`sheet` 不为 none），它可以修改地址、配送方式或优惠码，然后付款（`authorize`）或关闭面板（`cancel`），应用的回调会照常作答：

```asuka
import platform/pay

test "the tax, then paid" {
  let w = pay.fake()
  let app = mount(Checkout())
  await app.idle()
  app.tap("PayButton")
  await app.idle()
  w.changeShippingContact(pay.Contact(address: pay.PostalAddress(state: "CA", countryCode: "US")))
  await app.idle()
  expect(w.sheet!.total.price.string() == "43.2")
  w.authorize(pay.Payment(token: "tok_visa", shipping: pay.Contact(name: "Ana")))
  await app.idle()
  app.expect("Thanks, Ana!")
}

component Checkout {
  state placed = ""

  pay.PayButton {
    on press {
      let p = try await pay.pay(pay.Request(
        total: pay.Item(label: "Asuka Tea", price: .parse("40.00")!),
        shipping: [.address],
        onShippingContact: c => pay.Update(total: pay.Item(price: .parse(if c.address.state == "CA" { "43.20" } else { "40.00" })!)),
      ))
      placed = "Thanks, {p.shipping.name}!"
    }
  }
  Text(placed)
}
```

在测试中，原生组件显示为它的名字，点按它就会触发它的 `press`。[examples/checkout](../../examples/checkout) 就是用这种方式卖一套茶具。

## 面容 ID 与指纹：platform/biometric

`biometric.authenticate(reason:)` 显示系统的身份验证提示（Face ID、Touch ID、Android 的 BiometricPrompt；Mac 上是 Touch ID，Windows 上是 Windows Hello，浏览器中是通行密钥的 WebAuthn 提示）。如果确认是设备的主人，它就正常返回；否则抛出 `BiometricError.canceled`、`.notAvailable`（既没有生物识别也没有设置设备密码，或者已被锁定）或 `.failed`。等待期间它不会占住主线程。

```asuka
import platform/biometric

component Lock {
  state unlocked = false

  Button(if unlocked { "Unlocked" } else { "Unlock" }) {
    on press {
      do {
        try await biometric.authenticate(reason: "Unlock your notes")
        unlocked = true
      } catch biometric.BiometricError.canceled {
      } catch e {
        unlocked = false
      }
    }
  }
}
```

- `reason` 说明为什么要验证（在 iOS 上显示为副标题，在 Android 上显示为标题）；`cancel:` 是取消按钮的标题。
- 用户也可以改用设备的密码、PIN 或图案，除非设置了 `noPasscode: true`，即只允许生物识别（在 Android 上还要求是强生物识别，也就是 Class 3）。
- `biometric.check()` 告诉你设备具备哪些能力：它的 `kind`（`.face`、`.fingerprint`、`.iris`、`.optic`）、是否已录入（`enrolled`），以及是否设置了密码（`passcode`）。

Linux 没有用来验证设备主人的系统提示：`check` 会报告什么都没有，`authenticate` 会抛出 `.notAvailable`。

## 录音：platform/record

`record.audio()` 录下麦克风的声音，直到调用 `record.stopAudio()`（或到达 `maxDuration`），然后返回文件：应用缓存目录里（或 `path` 指定的位置）的一个 AAC 编码的 MPEG-4 文件（.m4a），可以用 `platform/audio` 播放。第一次录音时会请求麦克风权限。`pauseAudio` 和 `resumeAudio` 暂停和继续；`audioStatus()` 是当前的状态（`level` 从 0 到 1，可以做电平表；`elapsed` 是已录时长）。`quality: .music` 会保留比人声所需更多的细节。

```asuka
import platform/record

component Memo {
  state recording = false
  state file = ""

  Button(if recording { "Stop" } else { "Record" }) {
    on press {
      if recording {
        record.stopAudio()
      } else {
        recording = true
        defer { recording = false }
        file = try await record.audio(maxDuration: 60s)
      }
    }
  }
}
```

`background: true` 让它在应用不可见时继续录音：在 iOS 上，应用需要在 asuka.toml 的 `ios.backgroundModes` 里加上 `"audio"`；在 Android 上，它是一个前台服务，通知里显示 `title`，并带一个停止按钮。在 iOS 和 Mac 上用的是 AVAudioRecorder，在 Android 和浏览器中是 `MediaRecorder`，在 Windows 上是 Media Foundation，在 Linux 上是 GStreamer。

## 语音转文字：platform/transcribe

`transcribe.file(path)` 在设备上把一段录音转成文字，声音不会离开设备。它的 `Result` 包含 `text`、`segments`（每个词或短语的开始时间和时长，单位是秒，平台提供时才有）以及识别出的 `language`。`transcribe.check(language)` 可以在录音之前告诉你设备能不能转写。

```asuka
import platform/transcribe

async fn caption(recording: string) throws -> string {
  let can = try await transcribe.check("en-US")
  if !can.onDevice { return "" }
  let r = try await transcribe.file(recording, language: "en-US")
  return r.text
}
```

在 iOS 和 Mac 上用的是 Speech 框架，并要求只在设备上处理；在 Android 上是系统的设备端识别器（识别录音要 Android 13 及以上，`listen` 要 Android 12 及以上）；在浏览器中，是浏览器自己在设备上运行的识别器（Chrome 139 及以上），录音会被播放给它听，所以识别一个文件大约需要它本身的时长，结果也没有 `segments`；在 Windows 上是桌面语音识别器（SAPI 的听写：英语的随 Windows 附带，其他语言的随该语言包的语音识别功能安装）。在 Linux 上是各发行版打包的识别器 PocketSphinx（Debian 和 Ubuntu 上的 `pocketsphinx-en-us`），支持英语。设备做不到时（较旧的 Android、没有这种识别器的浏览器、Windows 上没有识别器的语言、没有安装 PocketSphinx 或使用其他语言的 Linux），它会抛出 `TranscribeError.notAvailable`，而不是把声音发到服务器；语言模型还在下载时抛出 `.downloading`（浏览器这时会开始安装它）。

`transcribe.listen(language)` 用于听写：它监听麦克风，边说边给出识别结果，每次一个 `Heard`，其中的 `text` 是到目前为止的全部文字（最后几个词可能还会变）。最后一个是 `final` 的：调用了 `transcribe.finish()`，或者识别器不再听到说话时就会给出。停止读取它的任务会立刻停止监听，之后的内容不再保留。第一次使用时会请求麦克风和语音识别的权限；它与 `file` 一样在设备上运行，支持的平台也相同。

```asuka check
import platform/transcribe

component Dictation {
  state on = false
  state text = ""

  task(on) {
    if !on { return }
    for await h in transcribe.listen() {
      text = h.text
    }
    on = false
  }

  Column {
    Text(text)
    Button(if on { "Done" } else { "Dictate" }) {
      on press {
        if on { transcribe.finish() } else { on = true }
      }
    }
  }
}
```

## 屏幕：platform/screen

```asuka
import platform/screen

component Player {
  state note = ""

  task {
    screen.lock(.landscape)               // 视频播放器
    screen.setBars(style: .light, hideStatus: true)
    screen.preventCapture(true)           // 票券上的二维码
    defer {
      screen.lock(.any)
      screen.setBars()
      screen.preventCapture(false)
    }
    for await _ in screen.screenshots() { note = "That screenshot shows your balance" }
  }

  Text(note)
}
```

- `screen.lock(o)` 把应用转到方向 `o` 并保持不变：`.portrait`、`.portraitUp`、`.landscape`、`.landscapeLeft`、`.landscapeRight`；`.any` 表示解除锁定。只能使用 `asuka.toml` 允许的方向。处于分屏浏览（Split View）或台前调度（Stage Manager）中的 iPad，以及 Android 16 的大屏设备，会忽略这个设置。
- `screen.setBars(style:, hideStatus:, hideNavigation:)` 设置状态栏的样式（`.auto` 跟随深色模式；`.light` 是浅色图标，适合下方是深色画面的情况；`.dark` 是深色图标），还可以隐藏状态栏或 Android 的导航栏（从屏幕边缘滑动会让它们短暂出现）。安全区域会随之更新。
- `screen.preventCapture(on)` 让应用不出现在截图和录屏中。Android 会把窗口设为安全窗口：截图和录屏都是黑屏，最近任务里也显示为空白。iOS 无法阻止截图：它只能在屏幕被录制、镜像或通过 AirPlay 投放时把应用遮住。
- `screen.screenshots()` 是用户每次截屏组成的流（iOS；Android 14 及以上）。
- `screen.setBrightness(level)` 让应用在前台时以 `level`（0 到 1）的亮度显示屏幕，比如把登机牌的二维码调到 1，方便扫码；传 `none` 则交还给系统亮度。应用离开前台时恢复用户原来的亮度，回到前台时再用应用的亮度：Android 设置的是窗口自己的亮度，iOS 设置的是屏幕亮度，并在离开时把用户的亮度放回去。`screen.brightness()` 是当前的亮度。

桌面窗口既没有方向，也没有系统栏：`lock` 和 `setBars` 在桌面端什么也不做；显示器的亮度由用户自己决定（`setBrightness` 什么也不做，`brightness()` 是 `none`）。在 Mac 和 Windows 上（Linux 不行），`preventCapture` 会让应用的窗口不出现在截图和录屏中；桌面系统不会把用户截屏的事告诉应用。浏览器只有在全屏或作为已安装的应用运行时才能锁定方向，靠进入全屏来隐藏系统栏，而防截屏、截屏通知和亮度设置它都不支持。

这些设置作用于整个应用，而不是某个页面：页面如果改了其中某项，离开时要把它恢复原样（在页面的 task 里用 `defer`）。

### 截取屏幕

`screen.capture(display)` 把一个显示器当前显示的内容截成 PNG 文件，存在应用的缓存目录里（`Image` 可以显示它，也可以分享或保存）：不传参数时截主显示器，也可以传 `screen.displays()` 中的一个。

```asuka check
import platform/screen

component Grab {
  state shot = ""
  state note = ""

  Column {
    gap: 12
    Button("Capture the screen") {
      on press {
        do {
          shot = try await screen.capture()
        } catch screen.CaptureError.notAllowed {
          note = "Allow screen recording in System Settings"
        } catch e {
          note = e.message
        }
      }
    }
    if !shot.isEmpty() {
      Image(shot) { height: 200 }
    }
    Text(note)
  }
}
```

| | 截取的内容 |
| --- | --- |
| macOS | 用 ScreenCaptureKit 截取显示器；第一次截取时会请求“录屏”权限（系统设置 › 隐私与安全性），用户拒绝则是 `notAllowed` |
| Windows | 显示器上的像素，不需要任何授权 |
| Linux | 截图门户（portal）给出的截图，第一次会询问用户 |
| Web | 用户在浏览器的选择框里选的屏幕或窗口（请在按下按钮时调用） |
| iOS、Android | 应用自己的窗口：应用在前台时，屏幕上显示的就是它 |

用户拒绝时抛出 `CaptureError.notAllowed`，平台做不到时（没有这个显示器、浏览器不支持截取屏幕、Linux 上没有门户）抛出 `.notAvailable`。

## 应用图标：platform/appicon

`asuka.toml` 列出应用除了自己的图标之外还能显示的图标，每个平台的构建会像生成应用图标一样生成它们：

```toml
icon = "icon.png"

[alternateIcons]
dark = "icons/dark.png"
classic = "icons/classic.png"
```

```asuka check
import platform/appicon

component IconPicker {
  state shown = appicon.current() ?? "default"

  Column {
    gap: 8
    for name in ["default"] + appicon.names() key name {
      Button(name) {
        on press {
          try? await appicon.set(if name == "default" { none } else { name })
          shown = appicon.current() ?? "default"
        }
      }
    }
    Text("Showing {shown}")
  }
}
```

- `appicon.names()` 是 `alternateIcons` 里的名字，`appicon.current()` 是应用当前显示的那个（`none` 表示应用自己的图标）。
- `appicon.set(name)` 换上其中一个，`set(none)` 换回应用自己的图标。选择会一直保留，重新启动和更新后都不变。构建里没有的名字会抛出 `AppIconError.unknown`。
- iOS 会更换主屏幕上的图标，并由系统弹出提示告诉用户，应用无法省略这个提示；应用在后台时调用的，会在回到前台时生效。Android 在应用离开屏幕时更换启动器里的图标（应用显示时更换可能会关闭应用）：应用抽屉里显示新图标，主屏幕上指向旧图标的快捷方式会被移除。

## 启动屏：platform/splash

`asuka.toml` 的 `splash` 就是启动屏：一张图片（正方形 PNG，比如 logo），宽 `size` 点（默认 120，最大 160），居中显示在纯色背景上，浅色和深色模式可以分别设置。

```toml
[splash]
image = "logo.png"
size = 96
background = "#4A63B5"
darkBackground = "#1B2440"
```

它从用户点按图标开始显示，一直到应用的第一帧出现为止。在 iOS 上，先显示系统的启动屏，然后在应用上方覆盖一份一模一样的副本，直到第一帧准备好；在 Android 12 及以上是系统的启动画面（图片位于图标的圆形区域中央），更早的版本则用窗口背景来显示。桌面端没有启动屏。在 Web 上，如果 asuka.toml 设置了 `background`，页面加载时是启动屏的颜色（不含图片），直到应用的第一帧，无论是否调用了 `hold`。`hold` 和 `hide` 在桌面端和 Web 上什么也不做。如果想在应用加载期间一直显示启动屏，就在启动时（在根组件中）调用 `splash.hold()`，加载完成后再调用 `splash.hide()`：

```asuka
import platform/splash

component Root {
  state ready = false

  task {
    splash.hold()
    await loadSession()
    ready = true
    splash.hide()
  }

  if ready { Home() }
}

async fn loadSession() { await sleep(300ms) } // 由应用自己实现：加载首屏需要的内容
component Home { Text("Signed in") }
```

## 地图：platform/maps

`maps.View` 是一张地图：在 iOS 和 Mac 上是 Apple 地图，在 Android 上是 Google 地图，在 Windows 和浏览器中是 OpenStreetMap 的瓦片，在 Linux 上是 libshumate 的瓦片。绑定它的 `region`（中心点和跨越的度数），就能移动地图，也能跟上用户的拖动；`markers` 是地图上的标记，`markerPress`、`press` 和 `regionChange` 是用户的操作。

```asuka
import platform/maps

component Places {
  state region = maps.Region(center: maps.Coordinate(lat: 37.77, lng: -122.42), latDelta: 0.08, lngDelta: 0.08)
  state picked = ""

  Column {
    maps.View {
      bind region: region
      markers: [maps.Marker(id: "hq", at: maps.Coordinate(lat: 37.79, lng: -122.39), title: "HQ", color: .blue)]
      mapType: .muted
      grow: 1
      on markerPress(id) { picked = id }
    }
    Text(picked)
  }
}
```

`mapType` 可以是 `.standard`、`.satellite`、`.hybrid` 或 `.muted`，`showsUser` 显示用户的位置（先用 `platform/location` 请求权限）。OpenStreetMap 的地图显示的是街道（在浏览器中 `.muted` 会用更柔和的颜色），用户位置也只在浏览器中显示。在 Android 上，导入这个包的应用会带上 Google 的 Maps SDK 一起构建，还需要在 asuka.toml 的 `android.mapsKey` 里填一个 Maps SDK for Android 的密钥。

## 后台刷新：platform/periodic

`platform/periodic` 会在应用不在屏幕上时，时不时运行一下应用的代码，比如同步数据、拉取新内容。`periodic.schedule` 请求系统去运行一个任务；`periodic.runs(name)` 是这个任务每次运行组成的流，在应用根组件的一个 task 里读取。

```asuka
import platform/periodic

component Root {
  task {
    periodic.schedule("sync", every: 1h, network: true)
    for await run in periodic.runs("sync") {
      do {
        try await syncInbox()
        run.finish(true)
      } catch {
        run.finish(false)   // Android 会再试一次
      }
    }
  }

  Text("Inbox")
}

// 由应用自己实现。
async fn syncInbox() throws {}

app { Root() }
```

什么时候运行由系统决定，依据是 `every`（至少 15 分钟）、电量、网络以及用户使用应用的习惯；实际间隔可能比设定的长得多，用户也可以关掉这项功能（iOS 的“后台 App 刷新”）。设置 `network` 后只在联网时运行，设置 `charging` 后只在充电时运行；再次调度同一个任务会更改它的设置。每个 `Run` 都有自己的 `budget`，也就是这次运行可以用的时间，比系统给的略少一点：iOS 上大约 25 秒（设置了 `charging` 时属于处理任务，可以有几分钟），Android 上为 9 分钟（WorkManager）。`run.finish(ok)` 表示这次运行结束：传 `false` 会请 Android 稍后重试。所有运行都由同一个流读取：流里来了别的任务的运行，会被立即结束。因此一个应用只用一个任务，并在一个地方读取它的运行。

当系统在应用关闭的情况下运行任务时，应用会在没有界面的情况下启动：它的根组件会运行，读取 `runs` 的那个 task 也会运行，并拿到这次启动所要执行的那次运行（这次运行会一直等到有读取者为止）。如果这时用户打开了应用，界面会正常出现。`periodic.cancel(name)` 用来停止一个任务。

桌面系统不会为了执行任务而启动应用：在 macOS（`NSBackgroundActivityScheduler`）、Windows 和 Linux 上，任务只在应用运行期间按定时器执行，需要耗电的任务在使用电池时会推迟。浏览器在页面关闭后不会运行任何任务：`schedule` 在浏览器中什么也不做。

## 应用商店服务：platform/review、platform/tracking、platform/integrity、platform/agerange

`review.request()` 请求应用商店弹出评分提示（App Store 的评分提示、Google Play 的应用内评价）。是否显示由商店按照自己的配额决定，而且不会告诉你结果；请在用户顺利完成某件事之后再请求，不要通过按钮触发。如果想做一个“给我们评分”按钮，请用 `device.openURL(review.appStoreURL(id))` 或 `review.playStoreURL(id)`。

`tracking.request()` 显示 iOS 的 App 跟踪透明度（App Tracking Transparency）提示，应用在读取广告标识符或与广告网络共享数据之前必须先发起这个请求；它返回 `.allowed`、`.denied`、`.restricted` 或 `.notAsked`。`tracking.current()` 只读取当前状态，不会发起请求。Android 没有这样的提示：结果是 `.allowed`。

`platform/integrity` 向你的服务器证明：请求确实来自运行在真实设备上的你的应用。iOS 上用的是 App Attest：`integrity.generateKey()` 生成一个密钥（保存好它的 ID），`integrity.attestKey(id, challenge)` 让 Apple 针对服务器给出的 challenge 为这个密钥出具证明（只需一次），之后每个请求都用 `integrity.assertion(id, data)` 签名。Android 上用的是 Play Integrity：启动时调用 `integrity.prepare(cloudProjectNumber:)`，之后每个请求调用 `integrity.token(requestHash:)`。服务器再拿这些返回结果去 Apple 或 Google 验证。在 iOS 模拟器上，`integrity.supported()` 为 false。

`agerange.request(13, 18)` 在不获取出生日期的前提下询问用户的年龄段，以满足那些要求应用识别未成年用户的法律：它基于 Apple 的 Declared Age Range（iOS 26）和 Play 的 Age Signals。平台可能会请用户本人或其家长同意分享。返回的 `Range` 有一个 `status`：`.shared`（附带年龄下限 `lower` 和上限 `upper`，0 表示不设限）、`.declined`，或者 `.verificationRequired`（Play）；它的 `source` 说明年龄是怎么得知的。平台无法回答时，得到的是 `AgeRangeError.notAvailable`。

```asuka
import platform/agerange
import platform/review
import platform/tracking

async fn afterFirstOrder() throws {
  try? await review.request()
  if try await tracking.request() == .allowed { print("personalized ads") }
  let r = try await agerange.request(13, 18)
  if r.status == .shared && r.lower >= 18 { print("an adult") }
}
```

手机以外的平台：Mac 支持 App Store 的评分提示、跟踪提示和 App Attest（需要描述文件，见[在 Mac 上](#在-mac-上)），但不支持 Declared Age Range。Windows、Linux 和 Web 没有商店的评分提示（`review.request` 什么也不做），没有设备认证（`integrity.supported` 为 false），也无法获取年龄段（`.notAvailable`）。在这些平台上，跟踪也不需要弹出提示：Windows 在“设置”中开启了广告 ID 时返回 `.allowed`；Linux 返回 `.allowed`；浏览器在发送了全局隐私控制（Global Privacy Control）或 Do Not Track 时返回 `.denied`。

应用内购买 `platform/purchase` 有单独的一页：[应用内购买](purchases.md)。iCloud 的键值存储和文档 `platform/icloud` 也有：[iCloud](icloud.md)。

## 运动与传感器：platform/sensors

`platform/sensors` 读取设备的运动和环境传感器，每个传感器都是一个流，只在被读取时运行：

```asuka
import platform/sensors

component Level {
  state roll = 0.0

  task {
    for await m in sensors.motion(every: 50ms) { roll = m.attitude.roll }
  }

  Text("{(roll * 180 / 3.14159).fixed(0)}°")
}
```

- `sensors.accelerometer(every:)`、`gyroscope(every:)` 和 `magnetometer(every:)` 大约每隔给定的间隔给出一个 `Vector`（`x`、`y`、`z`、`time`）：加速度的单位是 g，旋转的单位是弧度每秒，磁场的单位是微特斯拉（已校准）。
- `sensors.motion(every:)` 给出融合后的设备运动数据：`gravity` 和用户施加的 `acceleration`（单位为 g）、`rotation`，以及 `attitude`。`attitude` 包括一个把设备坐标轴转换到东-北-天（East-North-Up）坐标系的 `matrix`、`heading`（相对磁北的角度；没有磁力计时为 -1），以及 `pitch` 和 `roll`。
- `sensors.pressure(every:)` 对应气压计（单位为百帕，同时给出海拔变化），`sensors.light(every:)` 给出环境光照度，单位为勒克斯（Android、Linux），`sensors.stepUpdates()` 给出开始监听以来的步数。`sensors.steps(from:, to:)`（时间采用 `time.now()` 给出的形式）读取 iOS 的历史记录（最近一周）；在其他平台上会抛出 `SensorError.notAvailable`。
- `sensors.available(sensor)` 告诉你设备有没有某个传感器（`Sensor.all` 列出了所有传感器）；iOS 模拟器和 Android 模拟器上的传感器都很少。Mac 和 PC 没有这些传感器；Linux 在二合一设备和平板上有光线传感器，以及由加速度计得出的屏幕方向（iio-sensor-proxy）；浏览器有 Generic Sensor API 或设备运动事件，但没有气压计和计步器。读取设备上没有的传感器时，流会抛出 `notAvailable`。
- `sensors.simulate(json)` 给没有运动传感器的 iOS 模拟器送入一个读数，用 Core Motion 的术语描述（`kind` 为 0 表示加速度计，为 3 表示设备运动；还有 `acceleration`、`gravity`、`user`、`rotation`、`field`）。

各平台给出的数值是一致的：设备坐标轴采用 Android 和 W3C 的定义（x 指向屏幕右侧，y 沿屏幕向上，z 垂直于屏幕向外）；加速度是加速度计实际测得的值（设备屏幕朝上平放时，读数为 z = +1 g；iOS 的数据会转换成同样的方向）；姿态在每个平台上都用同样的方式计算。每秒最多 200 个读数；读数在主线程上送达。

在测试中，`sensors.fake()` 给这个包换上一个 `FakeSensors`：`present` 是设备拥有的传感器，读数通过它发出（`accelerate(x, y, z)`、`rotate`、`move(gravity, user:, rotation:, field:)`、`press(hPa)`、`shine(lux)`、`walk(steps)`），每个读数都会送到读取对应传感器的流，频率不会超过这些流请求的间隔。

```asuka
import platform/sensors

test "lying face up" {
  let s = sensors.fake()
  s.accelerate(0, 0, 1)
}
```

## 健康数据：platform/health

`platform/health` 在用户允许的前提下读写用户的健康数据：iOS 上是 HealthKit，Android 上是 Health Connect。`health.request(read:, write:)` 请求读写哪些种类（`.steps`、`.distance`、`.activeEnergy`、`.floors`、`.heartRate`、`.restingHeartRate`、`.oxygenSaturation`、`.respiratoryRate`、`.weight`、`.height`、`.bodyFat`、`.water`、`.sleep`、`.mindfulness`），`health.samples(kind, from:, to:, limit:)` 按从新到旧读取某一种类的样本，`health.total` 对可以累加的种类求和（手机和手表都记录到的不会重复计算），`health.write` 写入一条。

```asuka check
import platform/health
import std/time

component Steps {
  state steps = 0.0
  state note = ""

  Column {
    gap: 8
    Text("{steps.fixed(0)} steps today")
    Text(note)
  }

  task {
    if !health.available() {
      note = "No health data on this device"
      return
    }
    do {
      if try await health.request(read: [.steps]) {
        steps = try await health.total(.steps, from: time.now().startOfDay(), to: time.now())
      }
    } catch e {
      note = e.message
    }
  }
}
```

- 每个种类都有自己的单位：米、千卡、每分钟心跳次数、千克、升；比例则是 0 到 1（血氧饱和度、体脂率）。一次睡眠或一次正念练习是一个值为 1 的样本，时长由开始和结束时间表示。
- iOS 不会告诉应用它能否读取：用户没有允许的种类读出来就是没有样本。导入这个包后，应用会带上 HealthKit 能力（发布版的描述文件需要它）和用途说明（可以在 `[ios.usage]` 里写你自己的）。
- Android 需要 Health Connect：Android 14 起内置，Android 9 到 13 需要用户安装它的应用（更早的版本没有）。Google Play 要求说明每一项健康权限的用途，所以应用要在 [asuka.toml 的 `[health]`](asuka-toml.md) 里列出它读写的种类和隐私政策；请求没有列出的种类会抛出 `HealthError.notAllowed`。
- Mac、Windows、Linux 和 Web 上没有健康数据：`available()` 为 false，调用会抛出 `HealthError.notAvailable`。

## 广告：platform/ads

`platform/ads` 在 iOS 和 Android 上展示 Google 的广告（AdMob）：一种是放在应用视图之间的横幅广告，另一种是提前加载、在合适的停顿时展示的全屏广告。应用的 AdMob 应用 ID 写在 [asuka.toml 的 `[ads]`](asuka-toml.md#ads) 里；为 iOS 或 Android 构建时，如果缺少对应平台的 ID，构建会失败并说明原因。

```asuka check
import platform/ads

let rewardUnit = ads.testUnit(.rewarded)

component Game {
  state coins = 0
  state ready = false
  state note = ""

  task {
    do {
      let c = try await ads.consent()
      if c.canRequestAds {
        try await ads.start()
        ready = true
      }
    } catch e {
      note = e.message
      ready = ads.currentConsent().canRequestAds
    }
  }

  Column {
    gap: 12
    grow: 1
    Text("{coins} coins")
    Text(note)
    Button("Watch an ad for 10 coins") {
      disabled: !ready
      on press {
        do {
          let ad = try await ads.rewarded(rewardUnit)
          if let r = try await ad.show() {
            coins += r.amount
          }
        } catch e {
          note = e.message
        }
      }
    }
    View { grow: 1 }
    if ready {
      ads.Banner(ads.testUnit(.banner)) {
        on error(message) { note = message }
      }
    }
  }
}
```

- **先征得同意。** 在法律有要求的地区（欧洲经济区、英国、瑞士以及美国的部分州），Google 要求先取得用户的同意再展示广告。`ads.consent()` 通过 Google 的 User Messaging Platform 收集同意：如果需要同意而用户还没有给出，它会弹出你在 AdMob 的“隐私权和消息”中配置的表单；返回的 `Consent` 说明现在能否请求广告（`canRequestAds`）。每次应用启动、加载广告之前都调用一次。当 `privacyOptionsRequired` 为 true 时，应用要在设置里提供重新打开表单的入口：`ads.privacyOptions()`。在模拟器上测试时，可以用 `ConsentOptions(debugGeography: .eea)` 让表单在任何地区都弹出。
- **跟踪授权。** iOS 上 App Tracking Transparency 的提示由 `platform/tracking` 负责：在征得同意之后、展示第一条广告之前请求它。用户拒绝后仍然会看到广告，只是不再是个性化广告。
- **横幅广告。** `ads.Banner(unit)` 是 Google 的锚定自适应横幅：宽度等于布局给它的宽度，高度由 Google 根据这个宽度决定。`size:` 可以改用固定尺寸（`.banner` 即 320×50，`.mediumRectangle` 即 300×250，还有 `.largeBanner`、`.fullBanner`、`.leaderboard`）。它的事件会告诉你广告何时展示（`loaded`）、为什么没有广告（`error`），以及用户何时点击了它（`click`、`open`、`close`）。把它放在页面的顶部或底部，不要挨着用户常点的地方。
- **全屏广告。** `ads.interstitial(unit)`、`ads.rewarded(unit)`、`ads.rewardedInterstitial(unit)` 和 `ads.appOpen(unit)` 用来加载一条广告；`show()` 把它覆盖在应用上展示，等用户关闭后返回：如果是看完了的激励广告，返回 `Reward`（类型和数量，以 AdMob 中广告单元的设置为准），否则返回 `none`。一条广告只能展示一次，并且一小时后过期（开屏广告是四小时），所以请在需要之前不久再加载。如果要在服务器端验证奖励，`Request` 的 `userId` 和 `customData` 会随 Google 的回调发到你的服务器。
- **设置。** `ads.configure(Settings(...))` 设定之后每个请求都会带上的内容：`testDevices`、广告面向的年龄段 `age`（`.child`、`.teen`）、`maxRating`、`personalized: false`，以及视频广告的 `muted` 和 `volume`。
- **测试广告。** `ads.testUnit(format)` 是 Google 为当前平台、该格式提供的测试广告单元：它总有广告可展示，并标明是测试广告。开发时用它们，并在 `[ads]` 中填写 Google 的示例应用 ID；点击自己的真实广告可能导致 AdMob 账号被封。模拟器总是收到测试广告；真机则要把它的 ID（SDK 加载广告时会打印在日志里）加进 `testDevices`。
- **错误。** 没有广告可展示时，加载会抛出 `AdsError.noFill`；网络问题是 `.network`，广告单元或应用 ID 不对是 `.invalidRequest`；同一条广告展示两次会抛出 `.alreadyShown`。
- **其他平台。** Mac、Windows、Linux 和 Web 上没有 AdMob：`ads.available()` 为 false，Banner 什么也不显示，`consent()` 表示不能请求广告，加载广告会抛出 `AdsError.notAvailable`。

在测试中，`ads.fake()` 会给这个包换上一个 `FakeAds`：广告立即加载成功，用户看完（`watches`）激励广告时会得到它的 `reward`，测试可以读取加载过（`loaded`）和展示过（`shown`）的广告；改变 `noFill`、`unavailable` 和 `consent` 可以得到不同的结果。

```asuka
import platform/ads

test "a reward" {
  let f = ads.fake()
  let ad = try await ads.rewarded("unit")
  expect(try await ad.show() == f.reward)
}
```

## 编辑图片和视频帧：platform/imageedit

`imageedit.edit(source, actions)` 用平台自带的编解码器编辑图片，并写成 JPEG 或 PNG：它能读取相机和照片选择器给出的格式（HEIC、JPEG、PNG、WebP ……），并按照方向信息把图片摆正，适合用来上传、缓存或交给 `Image` 显示。操作按顺序执行：`.resize(width, height)`（某一边写 0 就保持比例）、`.crop(x, y, width, height)`、`.rotate(degrees)`、`.flipHorizontal`、`.flipVertical`、`.fill(...)`（填充纯色，原来的内容就彻底没有了）和 `.pixelate(...)`。`imageedit.size(source)` 读取图片尺寸；`imageedit.videoFrame(source, at:)` 把视频的某一帧取成图片。

```asuka
import platform/imageedit

async fn thumbnail(photo: string) throws -> string {
  let r = try await imageedit.edit(photo, [.resize(1080, 0), .crop(0, 0, 1080, 1080)], quality: 0.8)
  return r.path
}

async fn poster(video: string) throws -> string {
  return (try await imageedit.videoFrame(video, at: 1s, actions: [.resize(640, 0)])).path
}
```

在 iOS 和 Mac 上用的是 Core Graphics 和 Image I/O，在 Android 上是 `Bitmap` 和 `MediaMetadataRetriever`，在浏览器中是 canvas，在 Windows 上是 WIC 和 Media Foundation，在 Linux 上是 GdkPixbuf 和 GStreamer。来源不是图片时会抛出 `EditError.noImage`。

### 把视图存成图片

`imageedit.capture(view: ref)` 会把 `Ref` 指向的视图按它现在的样子（连同里面的一切）画成应用缓存目录里的一张 PNG（也可以用 `format: .jpeg`），比如要分享的卡片、要保存的收据。由平台来画，所以控件和屏幕上看到的一样。`scale` 是每个点对应的像素数，默认是屏幕的。

```asuka check
import platform/device
import platform/imageedit

component Receipt(total: string) {
  let card = Ref()

  Column {
    View {
      ref: card
      padding: 16
      radius: 12
      background: #f2f2f7
      Text("Paid {total}")
    }
    Button("Share") {
      on press {
        do {
          let image = try await imageedit.capture(view: card)
          try await device.shareFiles(paths: [image.path], from: card)
        } catch e {
          print(e.message)
        }
      }
    }
  }
}
```

在 iOS 和 Mac 上画的是视图的图层，在 Android 上画的是它的 `View`（里面有视频或地图时，取窗口在那里的像素）。Windows 和 Linux 取窗口在视图位置上的像素（Windows 总是按屏幕的缩放比例，不管 `scale` 是多少）。浏览器会把元素复制进一张 SVG 图片里，所以来自其他网站、又不允许跨域（CORS）的图片在那里会是空白。

## 文字转语音：platform/speech

`speech.speak(text)` 用设备上对应文字语言的声音朗读（也可以用 `Options(language:, voice:, rate:, pitch:, volume:)` 指定），读完后才返回；正在朗读时又要读的内容会排队等候。`stop()` 停止朗读（正在朗读或等待的调用会抛出 `SpeechError.stopped`），`pause()` 和 `resume()` 在某个词处暂停并继续，`speaking()` 表示是否正在朗读，`voices()` 列出所有声音及其 `quality`。

```asuka
import platform/speech

component Reader(text: string) {
  Row {
    gap: 12
    Button("Read") { on press { try await speech.speak(text, options: speech.Options(rate: 1.1)) } }
    Button("Stop") { on press { speech.stop() } }
  }
}
```

在 iOS 和 Mac 上用的是 AVSpeechSynthesizer，在 Android 上是系统的 `TextToSpeech`，在浏览器中是 `speechSynthesis`，在 Windows 上是 SAPI，在 Linux 上是 Speech Dispatcher。朗读时，其他应用的声音会自动调低。

## 通讯录：platform/contacts

```asuka
import platform/contacts

component Invite {
  state who = ""

  Column {
    Button("Choose a friend") {
      on press {
        if let c = try? await contacts.pick() {
          who = "{c.name}, {c.phones.count()} phones"
        }
      }
    }
    Text(who)
  }
}
```

- `contacts.pick()` 显示系统的联系人选择器：用户选中一位联系人，应用只能看到这一位，不需要请求任何权限（在 Android 上，没有权限时只能拿到姓名）。用户取消时它返回 none。
- `contacts.add(contact)` 显示系统的新建联系人界面，并预先填好信息，由用户来保存，然后返回用户是否保存了；不需要请求权限。
- `contacts.request()` 请求访问权限（结果是 `.notAsked`、`.denied`、`.limited` 或 `.allowed`），`contacts.list(name:, limit:)` 读取通讯录（可以按姓名查找，有数量上限）：每个 `Contact` 的姓名、`organization` 和 `jobTitle`、带标签的 `phones` 和 `emails`、`birthday` 以及 `note`。在 iOS 18 的有限访问模式下，只会列出用户选择共享的联系人。没有访问权限时，`list` 会抛出“contacts: not allowed”。

在 Mac 上用的是 Contacts 框架：`pick` 是系统的选择器，以弹出框的形式从窗口顶部弹出；`add` 则在窗口上方弹出一个警告框征求用户同意（macOS 没有新建联系人界面），然后再保存。Linux 读写 Evolution Data Server 的通讯录，详见[在 Linux 上](#在-linux-上)。Windows 只向打包应用开放联系人：`request` 返回 `.denied`，`pick` 会抛出错误，`add` 会在用户的联系人应用中打开一个 vCard。浏览器在支持 Contact Picker 的地方（Android 上的 Chrome）可以选择一位联系人，`add` 会下载一个 vCard。

## 日历：platform/calendar

```asuka
import platform/calendar
import std/time

async fn addDemo() throws -> bool {
  let start = time.date(2026, 12, 1, hour: 15)
  return try await calendar.add(calendar.Event(title: "Demo", location: "Room 2", start: start, end: start + 30min))
}
```

- `calendar.add(event)` 显示系统的事件编辑器，并预先填好信息，由用户来保存事件，然后返回用户是否保存了；不需要请求权限（iOS 17 及以上）。iOS 会告诉你事件是否已保存，Android 的日历应用则不会。
- `calendar.request()` 请求访问权限（结果是 `.notAsked`、`.denied`、`.writeOnly` 或 `.full`）；拿到 `.full` 后，`calendar.calendars()` 列出所有日历，`calendar.events(from:, to:, calendars:)` 列出两个时间点之间的事件，重复事件的每一次都会单独列出。没有 `.full` 时，它们会抛出“calendar: not allowed”。
- 事件的 `start` 和 `end` 是 `time.Time`。全天事件的则以天为单位：分别是第一天的 UTC 零点，以及最后一天的次日 UTC 零点。

在 Mac 上用的是 EventKit：`add` 在窗口上方弹出一个警告框征求用户同意（macOS 没有供应用使用的事件编辑器），然后再保存。Linux 读写 Evolution Data Server 的日历。Windows 只向打包应用开放日历：`request` 返回 `.denied`，`add` 会在用户的日历应用中打开一个 iCalendar 文件（并返回 false，因为 Windows 不会告诉应用是否保存了）。浏览器无法读取日历；`add` 会下载一个 `.ics` 文件。

## 主屏幕小组件：platform/widget

`asuka.toml` 的 `widgets` 声明应用有哪些小组件；`widget.set` 则在应用的代码里为它们填充内容：

```toml
[[widgets]]
kind = "today"
name = "Today"
description = "What's next"
sizes = ["small", "medium", "rectangular"]
```

```asuka
import std/time
import platform/widget

fn showToday(meeting: time.Time) {
  widget.set("today", widget.Entry(
    title: "Next", symbol: "calendar", value: "3 tasks", body: "Call Ana at 4",
    progress: 0.4, showProgress: true, until: meeting, url: "myapp://today",
  ))
}
```

小组件会用平台的模板（或者下面所说的应用自己的布局）显示它的 `Entry`，并按尺寸排版：`title`（带上 `symbol`，以 `tint` 着色）、大号的 `value`、`body`、`progress` 进度条、到 `until`（一个 `time.Time`）为止的实时倒计时，以及 `caption`。在应用设置新的内容之前，它会一直显示当前内容，不管应用是否在运行；点按小组件会用它的 `url` 打开应用（由应用的 `openURL` 事件接收）。`widget.setAll(kind, entries)` 中的条目各自带上 `at`，就构成了一条时间线：每个条目从各自的时间开始显示。`widget.clear(kind)` 用来清空小组件。

尺寸有 `small`、`medium`、`large`，以及 iOS 的 `extraLarge`（iPad）和锁屏上的 `circular`、`rectangular`、`inline`。iOS：CLI 会把一个 WidgetKit 扩展构建进应用，并让它与应用共享一个 App Group（发布构建需要开启 App Groups 功能，还需要为扩展的 ID，即应用的 ID 加上 `.widgets`，准备一个描述文件）。Android：每种小组件对应一个 App Widget，使用 Material You 配色，尺寸按启动器的网格单元计算。桌面端和 Web 没有小组件：`widget.set` 在这些平台上什么也不做。

### 应用自己的布局

`Entry` 的 `layout` 让小组件按应用自己的布局来画，取代模板。它是一棵由小组件能显示的部件组成的树，由平台来排版和绘制（iOS 扩展里是 SwiftUI 的视图，Android 上是 App Widget 的视图）：小组件是主屏幕根据应用最后设置的内容画出来的，而不是由运行中的应用画的。`sizes` 可以为某个尺寸单独指定一个布局。

```asuka check
import std/fs
import std/time
import platform/widget

fn energy(now: time.Time) -> widget.Layout = widget.row([
  widget.image("cover").frame(width: 56, height: 56).background(.clear, radius: 12),
  widget.column([
    widget.row([widget.symbol("bolt.fill", size: 13, color: .orange), widget.text("Energy", font: .caption.bold())]),
    widget.text("72%", font: Font(size: 30, weight: .bold), lines: 1),
    widget.progress(0.72, color: .orange),
    widget.text("Updated {now.format("HH:mm")}", font: .caption2, color: .secondaryLabel),
  ], spacing: 4, align: .stretch).fill(),
], spacing: 12)

async fn showEnergy() throws {
  widget.setImage("cover", try await fs.readAsset("cover.png"))
  let now = time.now()
  widget.set("energy", widget.Entry(url: "myapp://energy", layout: energy(now), sizes: [
    .small: widget.column([
      widget.text("Energy", font: .caption.bold()),
      widget.spacer(),
      widget.gauge(0.72, label: "72", color: .orange),
      widget.timer(until: now + 25min, font: .footnote),
    ]),
  ]))
}
```

部件有：`column`、`row`（各自带 `spacing` 和 `align`）和 `layers`（一层叠在另一层上，按 `x` 和 `y` 摆放）；`text`（一个 `Font`、一个 `Color`，最多 `lines` 行）；`symbol`（一个 SF Symbol，在 Android 上是对应的 Material 符号）；`image`（`widget.setImage` 保存的图片，存放在小组件能读到的地方）；`progress`（进度条）和 `gauge`（带标签的圆环）；`timer`（倒数到某个时间，过了之后再正向计时，小组件休眠时也会走）；`spacer` 和 `divider`。每个部件都可以加上 `padding`、`background`（一种颜色，可以带圆角）、`frame`（以点为单位的尺寸）、`fill`（占满所在列或行剩下的空间）、`link`（点按小组件的这一部分时应用打开的 URL；iOS 的小尺寸小组件只会打开条目的 `url`）和 `opacity`。语义颜色（`.label`、`.secondaryLabel`、`.accent` 等）跟随主屏幕的浅色或深色模式；文字样式跟随用户设置的文字大小。

## 实时活动：platform/liveactivity

实时活动在应用之外显示正在发生的事，并随着事情的进展更新：一份正在配送的外卖、一趟行程、一场比赛的比分、一个计时器。在 iOS 上，它是一个实时活动（Live Activity，iOS 16.2 起）：显示在锁屏和灵动岛上，没有灵动岛的 iPhone 上则以横幅显示。在 Android 上，它是一条常驻通知，Android 16 会把它提升为实时更新（Live Update）：显示在通知栏和锁屏的顶部，状态栏里还有一个胶囊标签。

```asuka
import platform/liveactivity
import std/time

async fn followOrder(eta: time.Time) throws {
  let a = try await liveactivity.start(liveactivity.Content(
    title: "Order #1042", subtitle: "Ana is picking it up", value: "12 min",
    symbol: "bag.fill", until: eta, url: "myapp://orders/1042",
  ))

  // 稍后：
  a.update(liveactivity.Content(title: "Order #1042", subtitle: "2 stops away", value: "4 min", until: eta),
    alert: liveactivity.Alert(title: "Almost there", body: "Ana is 2 stops away"))
  a.end(liveactivity.Content(title: "Order #1042", subtitle: "Delivered"))
}
```

实时活动用平台的模板显示它的 `Content`，空着的字段不显示：`title` 和 `subtitle`、`value`（大号显示在一侧；在灵动岛的紧凑视图里显示；在 Android 16 上是状态栏里的胶囊标签）、以 `tint` 着色的 `symbol`、`progress` 进度条，以及一个到 `until`（一个 `time.Time`）为止的倒计时，它不需要应用运行也会走。同时设置 `since`、`until` 和 `showProgress` 时，进度条会自己往前走。点按实时活动会用它的 `url` 打开应用（由应用的 `openURL` 事件接收）。

`liveactivity.start(c, push:, relevance:)` 要在应用显示在屏幕上时调用（两个平台都不允许后台的应用启动实时活动）；`relevance` 决定应用的多个实时活动之间的先后顺序。带上 `Alert` 的更新会点亮屏幕：iOS 会展开灵动岛，并在 Apple Watch 上显示它；Android 会让通知再提醒一次。`end(c, dismiss:)` 之后，最后的内容会继续显示，直到系统把它移走（`.later`，这是默认值，最多 4 小时），或者显示到某个时间为止（`.at(t)`），也可以立即移走（`.now`）。`liveactivity.enabled()` 返回用户是否允许实时活动（Android 会在第一次 `start` 时请求通知权限）。这个包不会列出仍在运行的实时活动。

`liveactivity.events()` 告诉你实时活动发生了什么：某个活动 `.ended`（被用户、系统或服务器结束）、某个活动被服务器 `.started`，以及推送令牌（`.pushToken`、`.pushToStartToken`）；每一项都是一个 `Event`，带有对应的 `activity` 和 `token`。设置 `push: true` 后，服务器可以在应用不在前台时更新和结束实时活动：把它的推送令牌和 `id` 发给服务器即可。在 iOS 上，服务器发送一条 `liveactivity` 类型的 APNs 推送，其中的 `content-state` 就是 JSON 形式的内容（即 `Content` 的各个字段，其中 `since` 和 `until` 写作 `sinceMs` 和 `untilMs`）；在 Android 上，服务器发送一条 Firebase 数据消息，带上 `asuka.liveActivity`（`update` 或 `end`）、`asuka.id` 和 `asuka.content`。服务器也可以启动实时活动：在 iOS 17.2 上用 push-to-start 令牌（attributes 类型为 `AsukaActivityAttributes`，带上新活动的 `id`），在 Android 上把 `asuka.liveActivity` 设为 `start`。推送需要应用同时使用 `platform/push`：iOS 上需要推送通知（Push Notifications）功能，Android 上需要 Firebase。

对于调用了这个包的应用，iOS 构建会加入小组件扩展，其中带有实时活动的模板（和小组件一样用 SwiftUI 编写），并设置 `NSSupportsLiveActivities`；和小组件一样，发布构建需要为扩展的 ID 准备一个描述文件。用户可以在“设置”里为应用关闭实时活动，也可以在 Android 16 的通知设置里关闭提升显示。桌面端和 Web 没有实时活动：`enabled()` 为 false，`start` 会抛出“liveactivity: not available”。

## 快捷方式：Siri、聚焦搜索与启动器

`asuka.toml` 的 `shortcuts` 列出系统为应用提供的快捷操作。每个快捷方式都会用它的 URL 打开应用，交给应用的 `openURL` 事件处理：

```toml
[[shortcuts]]
id = "newNote"
title = "New Note"
url = "notes://new"
symbol = "square.and.pencil"
phrases = ["New note in ${app}"]
```

在 iOS 上，它们是 App Shortcuts：Siri 能听懂它们的短语（每个短语都要包含代表应用名称的 `${app}`），它们还会出现在聚焦搜索、“快捷指令”App 和操作按钮中，用户无需做任何设置（最多十个）。CLI 会把它们编译成 App Intents，并像 Xcode 那样提取它们的元数据。在 Android 上，它们是启动器快捷方式，长按应用图标时出现，图标使用强调色。桌面端和 Web 不会添加它们。

模拟器构建会像 Xcode 那样签名，使用钥匙串中的 Apple Development 身份（属于 `asuka.toml` 中指定的团队）：App Intents 只能在带有团队的应用中运行。如果没有团队，构建会采用临时（ad hoc）签名，CLI 会提示快捷方式将无法运行。

### 参数

快捷方式的 `params` 是 Siri 和“快捷指令”App 要向用户询问的值。每个参数的值经过转义后，填进 URL 中对应的 `{name}`：

```toml
[[shortcuts]]
id = "addTask"
title = "Add Task"
url = "todo://add?title={title}&list={list}"
phrases = ["Add a task in ${app}", "Add a ${list} task in ${app}"]

[[shortcuts.params]]
name = "title"
title = "Title"
prompt = "What's the task?"

[[shortcuts.params]]
name = "list"
title = "List"
type = "options"
options = [{ value = "work", title = "Work" }, { value = "home", title = "Home" }]
default = "home"
```

说“Add a task in Todo”时，Siri 会问 *What's the task?*，然后打开 `todo://add?title=Buy%20milk&list=home`；说“Add a work task in Todo”则直接问标题。参数的 `type` 可以是 `string`（默认）、`int`、`float`、`bool` 或 `options`。没有 `default` 的参数，Siri 会询问，除非它是 `optional`，这时它在 URL 中的位置留空。短语里可以引用 `options` 类型的参数（`${list}`），Siri 能听懂每个选项的标题；其他类型不能出现在短语里，因为 Siri 无法识别任意文字。在“快捷指令”App 里，每个参数都是这个操作的一个字段。在 Android 上，启动器快捷方式打开的 URL 里填的是各参数的默认值，没有默认值则留空。

### 应用自己的数据

`entity` 类型的参数接受应用自己的一项数据：一条笔记、一个播放列表、一个联系人。应用用 `platform/shortcuts` 告诉系统有哪些，Siri 和“快捷指令”App 就会按标题提供它们，即使应用没有在运行：

```toml
[[shortcuts]]
id = "markDone"
title = "Mark Done"
url = "notes://done/{note}"
phrases = ["Mark ${note} done in ${app}"]

[[shortcuts.params]]
name = "note"
title = "Note"
type = "entity"
entity = "note"
```

```asuka check
import platform/shortcuts

struct Note {
  id: int
  text: string
}

// 每次笔记变化时。
fn offer(notes: [Note]) {
  shortcuts.setEntities("note", notes.map(n => shortcuts.Entity(id: "{n.id}", title: n.text)))
}
```

说“Mark Buy milk done in Notes”会打开 `notes://done/12`：选中那一项的 `id` 填进 URL 中参数的位置。短语里可以引用 entity 参数，就像引用 `options` 参数一样。在 Android 上，每一项都是一个单独的启动器快捷方式（启动器能显示多少就有多少），打开同样的 URL。[`examples/notes`](../../examples/notes) 就用了这个功能。

## 车机屏幕：platform/car

`platform/car` 把应用显示在汽车的屏幕上，也就是 CarPlay 和 Android Auto：显示的不是应用的视图，而是平台的模板（列表、网格、标签页、信息、地图上的地点、正在播放、导航应用的地图及其导航提示），用值来描述，跟随应用的状态，并说明选中其中的行时做什么。`asuka.toml` 的 `[car]` 说明应用的类别，CLI 据此加上 CarPlay 的 entitlement 和场景，以及 Android Auto 的服务。详见[车机屏幕](car.md)。

```asuka check
import platform/car

struct Album {
  title: string
  cover: string
  tracks: [string]
}

let albums = [Album(title: "Morning", cover: "covers/morning.jpg", tracks: ["Sunrise", "Coffee"])]

fn album(a: Album) -> car.Template = .list(title: a.title, items: a.tracks.map(t => car.Item(title: t)))

component Library {
  car.Screen {
    root: .list(title: "Library", items: albums.map(a => car.Item(
      title: a.title,
      image: a.cover,
      browsable: true,
      run: () => car.push(() => album(a)),
    )))
  }

  Text(if car.connected() { "On the car's screen" } else { "Not in a car" })
}

app { Library() }
```

## 触感反馈：device.haptic

`device.haptic(kind)` 用来播放触感反馈（iOS 上是 UIFeedbackGenerator，Android 上是 `performHapticFeedback`），参数是一个 `device.Haptic`：`.selection`、`.light`、`.medium`、`.heavy`、`.success`、`.warning` 或 `.error`。Mac 会通过触控板给出反馈（需要 Force Touch 触控板：`.selection` 对应对齐反馈，其余对应层级变化反馈）；浏览器在支持的地方会振动（Android 上的 Chrome）；Windows 和 Linux 没有触感反馈，什么也不做。

```asuka
import platform/device

component Save {
  Button("Save") { on press { device.haptic(.success) } }
}
```

需要与动画配合的触感反馈，请看[动效与手势](motion.md)。

## 崩溃报告：platform/crash

导入了 `platform/crash` 的应用会为每次崩溃保留一份报告，并在下次启动时读取，发到哪里由你决定：你自己的服务器、Sentry 或 Crashlytics 都可以。Asuka 自己不会上传任何东西。

```asuka
import platform/crash

component Root {
  task {
    crash.set("user", "u42")
    for await r in crash.reports() {
      if (try? await send(r)) ?? false {   // 由你实现上传：Report 就是 JSON
        crash.delete(r.id)
      }
    }
  }

  Button("Check out") {
    on press {
      crash.breadcrumb("opened the cart")
      do {
        try await checkOut()
      } catch e {
        crash.record(e)   // 已经处理过的错误，同样会上报
      }
    }
  }
}

// 由应用自己实现。
async fn send(r: crash.Report) throws -> bool = true
async fn checkOut() throws {}

app { Root() }
```

`crash.reports()` 是一个流：先按从早到晚的顺序给出已保存的报告，之后每出现一份新报告（比如 iOS 在启动后稍晚才送达的诊断，或是记录下来的错误）就再给出一份，每份报告只给一次。报告会一直保留，直到调用 `crash.delete(id)`；超过 50 份时，最早的会被丢弃。同一份报告每次读取时的 `id` 都相同，所以服务器能够识别出重复发送的报告。`crash.record(e)` 为应用已经处理过的错误生成一份报告，`crash.breadcrumb(message)` 往报告里“应用最近做了什么”的记录中添加一条，`crash.set(key, value)` 设置报告里附带的一个值（传 `""` 会删除它）。

一份 `Report` 包含 `id`、`kind`、是否致命（`fatal`）、`time`（一个 `time.Time`）、`message`（一行文字：什么崩溃了，以及原因）、`text`（平台写下的完整报告）、`threads` 和 `context`。各平台会上报哪些内容，按 `kind` 分类如下：

| | iOS | Android | macOS | Windows | Linux | Web |
|---|---|---|---|---|---|---|
| `"panic"`：Asuka 自身的致命错误，带有 Asuka 代码的调用栈 | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| `"exception"`：未捕获的异常 | NSException（Objective-C，以及 Swift 中的 `NSException`） | JVM 的 `Throwable` | — | — | — | 未捕获的 JavaScript 错误（非致命） |
| `"native"`：信号、Mach 异常或 Windows 异常 | MetricKit | 系统的 tombstone | MetricKit；致命信号，由 Asuka 报告 | Asuka 的报告 | Asuka 的报告（原生代码中的信号） | — |
| `"hang"` | MetricKit 报告的卡顿（非致命）、看门狗 | ANR 及其 trace | MetricKit | — | — | — |
| `"oom"` | 超出内存上限（由 MetricKit 报告时） | 因内存不足被系统终止 | — | — | — | — |
| `"resource"` | CPU 和磁盘写入异常（非致命） | 资源使用过度 | MetricKit | — | — | — |
| `"nonfatal"`：`crash.record(e)` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |

在每个平台上，记录都由 Asuka 保存：Asuka 自身的致命错误会在运行结束时连同 Asuka 调用栈一起写下，未捕获的异常则在应用结束时写下（在它之前设置的处理函数，比如某个崩溃 SDK 的，随后依然会运行）。其余的则由系统在之后的某次启动时交给应用：iOS 和 macOS 的 MetricKit 诊断，以及 Android 对应用进程结束原因的记录（`getHistoricalProcessExitReasons`，Android 11 及以上；原生崩溃的 tombstone 从 Android 12 开始提供）。如果某次崩溃已经由 Asuka 报告过，系统又再报告一次（iOS 上未捕获异常导致的 SIGABRT，Android 的 `REASON_CRASH`），那么系统的这份会被排除。在 macOS、Windows 和 Linux 上，致命信号和没有被处理的异常会在下次启动时报告。

每份报告都带有 `context`：应用的版本和构建号，设备的型号和系统版本，进程及其已运行的时长，当时是否在前台，以及最近的面包屑（最多 100 条）和设置过的值。这些信息一有变化就会写入应用的数据目录，开销和一次普通写入差不多。这样一来，不管应用是怎么崩溃的，这些信息在崩溃之前就已经写到磁盘上了。生命周期的变化也会记成面包屑（“app: background”）。

报告的 `text` 是平台给出的原始内容（Asuka 的崩溃输出、调用栈、tombstone、ANR 的 trace）；`threads` 是把这些调用栈解析成的帧，崩溃的线程排在最前面。Asuka 的帧包含 `function`、`file` 和 `line`。原生帧包含所在的二进制文件 `binary`、该二进制的 UUID（Mach-O）或 build ID（ELF），即 `binaryId`，以及地址 `address` 和它在二进制中的偏移 `offset`，崩溃收集服务正是用这些信息，配合构建时产出的符号来做符号化（见[打包与发布](shipping.md#崩溃报告的符号)）。

**发送到 Sentry 或 Crashlytics。** 对于 Sentry，把每份报告转成一个事件，发到它的 envelope 端点（`https://<host>/api/<project>/envelope/`，使用你 DSN 中的 key）：`exception.values[0]` 的 `type` 为 `kind`，`value` 为 `message`，`stacktrace.frames` 为崩溃线程的帧，从最外层开始排列（这是 Sentry 的顺序，与 `Thread` 中的顺序相反），每一帧带上 `function`、`filename` 和 `lineno`，原生帧则带上 `instruction_addr` 和 `package`；再在 `debug_meta.images` 中列出这些帧所在的每个二进制文件：`{"type": "macho", "debug_id": binaryId, "image_addr": address - offset}`（Android 上为 `"elf"`），这样你用 `sentry-cli` 上传的 dSYM 或符号文件才能派上用场。`context` 放进 `contexts`、`tags` 和 `breadcrumbs`。Crashlytics 没有可以直接发送的公开端点：你可以把报告发到自己的服务器，或者在你自己的原生包里（[原生代码](native.md)）通过 Crashlytics 的 SDK 把它们记录为非致命错误。不管用哪种方式，App Store 审核都会问到崩溃数据：如果你发送了，就要如实声明（在 `asuka.toml` 的 `ios.collected` 中加上 `{type = "CrashData", purposes = ["AppFunctionality"]}`）。

**各平台的细节。** iOS 只会把 MetricKit 链接进读取报告的应用，并且把诊断交给应用本身，而不是它的分享扩展。从 iOS 17 开始，MetricKit 会给出崩溃的进程 ID，借此可以把崩溃关联到那一次运行的上下文；在此之前，则关联到崩溃之前的最后一次运行。MetricKit 在模拟器上不会送达诊断。Android 不需要额外配置；Android 11 之前没有原生崩溃和 ANR 的记录。macOS：MetricKit 从 macOS 12 开始可用；导致应用退出的未捕获 Objective-C 异常会以 MetricKit 崩溃报告的形式出现（主线程上的这类异常会被 AppKit 捕获并记入日志）。Windows 错误报告（Windows Error Reporting）照常保留它自己的记录。在浏览器中，记录保存在应用的文件中（IndexedDB），下次启动时读取。未捕获的 JavaScript 错误（无论来自 Asuka 还是应用的 `web/*.js`）会立即生成一份 `"exception"` 报告，它是非致命的（页面会继续运行），并附带本次运行的上下文；每次运行最多 20 份，同一个错误只报告一次。每个页面都会在同源的 `localStorage` 里标记自己这次运行还活着，这样应用其他开着的页面就不会去动它的记录。在 `asuka test` 下不会保存记录：`reports()` 不给出任何报告就结束，其余调用什么也不做。

**在 `asuka dev` 下**，开发 App 会像正式构建一样保存记录；开发 App 自身的原生崩溃不属于你的应用，不会报告给它；应用的构建（`asuka dev --once`、`asuka build`）则会报告所有崩溃。

## 数据分析：platform/analytics

`platform/analytics` 把用户在应用里做了什么告诉 Google Analytics：应用记录的事件和参数、用户看过的页面、用户是谁（用户 ID 和用户属性），以及用户同意了什么（意见征求模式，Consent Mode）。在 iOS、macOS 和 Android 上它使用 Google Analytics for Firebase，在 Web 上使用 gtag.js，在 Windows 和 Linux 上使用 GA4 的 Measurement Protocol，相当于 React Native 和 Flutter 在手机上用的 Firebase Analytics。

```asuka check
import platform/analytics

component Shop {
  state note = ""

  task {
    try? analytics.screen("Shop", class: "ShopScreen")
  }

  Column {
    gap: 8
    Button("Buy two pairs") {
      on press {
        do {
          try analytics.log("purchase", ["currency": "USD", "value": 20.02, "transaction_id": "T_1001"],
            items: [analytics.Item(id: "SKU_123", name: "Socks", price: 10.01, quantity: 2)])
        } catch e {
          note = e.message
        }
      }
    }
    Text(note)
  }
}

component ConsentBanner {
  Row {
    gap: 8
    Button("Accept") {
      on press {
        analytics.setConsent(analytics.Consent(analyticsStorage: true, adStorage: true, adUserData: true, adPersonalization: true))
      }
    }
    Button("Decline") {
      on press {
        analytics.setConsent(analytics.Consent(analyticsStorage: false, adStorage: false, adUserData: false, adPersonalization: false))
      }
    }
  }
}

test "buying logs a purchase" {
  let a = analytics.fake()
  let app = mount(Shop())
  app.tap("Buy two pairs")
  let p = a.logged("purchase")
  expect(p.count() == 1 && p[0].items[0]["item_id"] == .string("SKU_123"))
}
```

- `analytics.log(name, params, items:)` 记录一个事件。参数可以是字符串、数字和布尔值（布尔值以 1 或 0 发送，值为 `none` 的参数会被省略）；电商事件的商品放在 `items` 里，每件是一个 `analytics.Item`（`id`、`name`、`price`、`quantity`，其余的放在 `params` 里）。和 Firebase 一样，只有电商事件（`view_item`、`add_to_cart`、`purchase` 等，见 `analytics.ecommerceEvents`）可以带商品。使用 Google 推荐的事件（`login`、`sign_up`、`purchase`、`select_content` 等）可以直接用上 Analytics 自带的报告。
- `analytics.screen(name, class:)` 记录一个 `screen_view`。应用的页面不会被自动上报（在 iOS 和 Android 上，Firebase 只能看到 Asuka 唯一的那个 view controller 或 activity，所以它的自动上报是关闭的）：请在页面显示时调用它。
- `setUserId(id)`（用户退出登录后传 `none`）和 `setUserProperty(name, value)` 为之后的事件描述用户；`setDefaults(params)` 给之后的每个事件都加上这些参数。
- `setEnabled(false)` 关闭数据收集，本次和以后的启动都有效；`setConsent(Consent(...))` 设置意见征求模式（`true` 表示同意，`false` 表示拒绝，`none` 表示保持不变）。在应用自己设置之前，以 `asuka.toml` 的 `[analytics]` 为准（见下文）：需要先征求用户同意的应用，可以让数据收集一开始就关闭，或者一开始就拒绝相应的授权。
- `await analytics.appInstanceId()` 是 Analytics 用来识别这次安装的 ID（BigQuery 里的 `user_pseudo_id`），数据收集关闭或拒绝分析存储时为 `none`；`reset()` 会清除它以及用户的 ID 和属性，这次安装就会被当成一个新的实例。

所有平台接受的事件都是一样的，因为包会在交给平台之前先检查一遍。事件名和参数名是 1 到 40 个字母、数字或下划线，以字母开头，不能以 `firebase_`、`google_` 或 `ga_` 开头，也不能是 Analytics 自己记录的事件（`first_open`、`session_start`、`app_update` 等）。一个事件最多 25 个参数（商品算一个），最多 200 件商品，每件最多 27 个参数。用户属性名是 1 到 24 个上述字符，并且不能是 Analytics 自己设置的（`user_id`、`first_open_time` 等）。违反这些规则会抛出 `AnalyticsError`（`invalidName`、`reservedName`、`tooManyParameters`、`invalidValue`），超过 256 个字符的用户 ID 也是如此。而超过 Analytics 保存长度的文本值会被截断：参数值保留 100 个字符，用户属性值保留 36 个。

各平台的设置都写在 `asuka.toml` 里（[analytics](asuka-toml.md#analytics)）：

```toml
[ios]
googleServices = "GoogleService-Info.plist"   # iOS 和 Mac
[android]
googleServices = "google-services.json"
[analytics]
measurementId = "G-XXXXXXXXXX"   # Web、Windows 和 Linux：一个 Web 数据流的
apiSecret = "…"                   # Windows 和 Linux
enabled = false                   # 直到应用调用 setEnabled(true)
consent = { adStorage = "denied", adUserData = "denied", adPersonalization = "denied" }
```

- **iOS 和 macOS**：Firebase 项目里与应用 ID 相同的 Apple 应用的 `GoogleService-Info.plist`，构建时会打包进应用；Mac 也用同一个文件。Firebase 在应用启动时就开始工作。
- **Android**：项目的 `google-services.json`，其中要有与应用 ID 相同的 Android 应用。Firebase Analytics 会声明广告 ID 权限（`AD_ID`）：请在 Play 管理中心的广告 ID 声明里如实填写。
- **Web**：从 Google 加载的 gtag.js，发送到 `measurementId` 对应的 Web 数据流（Firebase Web 应用的 `measurementId` 就是它的数据流）。页面里不会有 API 密钥。`reset()` 会清除浏览器的客户端 ID（Analytics 的 Cookie），下次访问就是一个新实例。
- **Windows 和 Linux**：通过 Measurement Protocol 发送到同一个 Web 数据流，使用它的一个 API 密钥（在 Analytics 的“管理 > 数据流 > Measurement Protocol API 密钥”中创建；用 `ASUKA_ANALYTICS_API_SECRET` 代替 `apiSecret`，就可以不把它放进代码仓库）。事件在单独的线程上批量发送，每批最多 25 个，在第一个事件之后几秒发出，应用永远不必等待网络。这次安装的客户端 ID 在第一次启动时生成，和用户的 ID、属性以及尚未发出的事件一起保存在应用的数据目录里（尚未发出的事件会在下次启动时发送，最多保留 72 小时；离线时会稍后重试）。30 分钟内没有新事件，会话就会结束。页面会作为一次网页浏览（page view）发送，标题是页面的名字，地址是 `app://<应用 ID>/<名字>`。拒绝分析存储期间什么也不会发送。在 `asuka.toml` 里设置 `analytics.endpoint`，事件就会发到你自己的收集端（比如服务器端代码植入服务器），而不是 GA。

缺少设置的平台什么也不会发送：那里的 `available()` 为 false，各个调用什么也不做，`asuka dev` 和 `asuka build` 会提示缺少什么。在 `asuka dev` 下，Web 和桌面端会发送到 Analytics 的 DebugView；在手机上则需要打开 Firebase 的调试模式（Android 上运行 `adb shell setprop debug.firebase.analytics.app <应用 ID>`）。在测试里，`analytics.fake()` 会记录应用记录的内容而不发送（它的 `events`、`logged(name)`、`userId`、`userProperties`、`consent`）。应用发送了哪些数据，需要你在应用商店的隐私信息里自行声明（App Store Connect 的“App 隐私”、Google Play 的“数据安全”）。
