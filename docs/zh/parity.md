# Asuka 与 SwiftUI、Expo 的功能对照

下面每一行都是应用经常需要的一项功能，分别列出 SwiftUI 和 Expo 的做法，以及 Asuka 提供的对应能力。除非该行另有说明，✓ 表示 Asuka 支持的所有平台（iOS、Android、Mac、Windows、Linux 和 Web）都能用；如果某个平台本身不具备某项能力（比如 Mac 没有传感器，浏览器没有主屏幕小组件），对应的包会如实告诉调用方不支持（抛出 `unsupported` 或 `notAvailable` 错误，或者 `available()` 返回 false）。至于浏览器做不到手机能做的事时各个包会如何替代，见 [Web](web.md#原生模块)。

Asuka 这一列写的是实现这项功能的元素、属性或包（`std/...` 或 `platform/...`，见[标准库与平台 SDK](stdlib.md)），以及它在各个平台上分别是什么：Asuka 显示的是各平台自己的原生控件，调用的也是各平台自己的 API——iOS 和 Mac 上用 Swift，Android 上用 Kotlin，Web 上用 JavaScript，Windows 和 Linux 上用 C。

SwiftUI 也能在 Mac 上运行。Expo 支持 iOS、Android 和 Web；React Native 则借助微软的 React Native for Windows and macOS 覆盖 Windows 和 Mac，不过这不在 Expo 的范围内。Asuka 与桌面端常用框架（Electron、Tauri、Wails）的对比，见[与桌面端框架对比](#与桌面端框架对比)；桌面应用在窗口之外还需要哪些能力，见[桌面应用的能力](#桌面应用的能力)；与自己绘制 UI 的 MyGo 的对比，见 [Asuka 与 MyGo](mygo.md)。

✓ 已支持 · ◐ 部分支持（会注明缺什么） · ✗ 尚未支持

## 布局

| | SwiftUI | Expo (React Native) | Asuka |
|---|---|---|---|
| 堆叠、间距、内边距、对齐 | `HStack`、`VStack`、`Spacer` | flexbox | ✓ `Row`、`Column`、`View`，以及 flexbox 的各个属性：`grow`、`shrink`、`gap`、`align`、`justify`、`wrap`、`padding`、`margin`（见[布局](layout.md)） |
| 覆盖层、绝对定位 | `ZStack`、`.overlay` | `position: absolute` | ✓ `absolute`，配合 `top`、`left`、`start`、`end`…… |
| 视图网格 | `Grid`、`LazyVGrid` | 社区方案 | ✓ `grid`（CSS grid：`Grid(columns:)`、`.fr`、`.repeat`、`.autoFill`；`gridColumn`、`gridRow`） |
| 滚动 | `ScrollView` | `ScrollView` | ✓ `Scroll`（纵向，或用 `horizontal` 横向） |
| 惰性列表、单元格复用 | `List`、`LazyVStack` | `FlatList` | ✓ `List`（用平台自己的：`UICollectionView`、`RecyclerView`；在 Mac、Windows、Linux 和 Web 上，由 Asuka 在平台的滚动视图里复用可见范围内的行） |
| 惰性网格 | `LazyVGrid` | `FlatList numColumns` | ✓ `List` 的 `columns`、`gap` |
| 聊天列表：从末尾开始显示，往上加载更早的消息 | `defaultScrollAnchor(.bottom)`、`scrollPosition` | `FlatList inverted`、`maintainVisibleContentPosition` | ✓ `List` 的 `anchor: .bottom`、`on startReached`；在可见内容上方插入行时，可见内容保持不动，所有列表都是如此 |
| 分区、吸顶标题 | `List` 里的 `Section` | `SectionList`、`stickySectionHeadersEnabled` | ✓ `List` 的 `section` 和 `header`（在 plain 和 bare 列表里吸顶） |
| 选择行 | `List(selection:)`、`EditButton` | 社区方案 | ✓ `List` 的 `selecting`，行的 `selected`、`on select`、`on selectBegin`（iOS 的编辑模式和双指拖动；Android 的复选框和长按；桌面端高亮选中行，浏览器里显示勾选标记） |
| 安全区域、键盘 | 自动处理 | `SafeAreaView`、`KeyboardAvoidingView` | ✓ `SafeArea`（也会为键盘留出空间）、`environment()` 的 `safeTop`……和 `keyboard` |
| 读取视图尺寸、窗口尺寸 | `GeometryReader`、尺寸类别 | `onLayout`、`useWindowDimensions` | ✓ `on layout(frame)`；`environment()` 的 `width`、`height`、`widthClass`、`heightClass` |
| 宽高比、最小和最大尺寸 | `.aspectRatio`、`.frame(min:max:)` | style | ✓ `aspectRatio`、`minWidth`、`maxWidth`、`minHeight`、`maxHeight` |

## 控件

| | SwiftUI | Expo | Asuka |
|---|---|---|---|
| 文本、富文本、链接 | `Text`、`AttributedString` | `Text` | ✓ `Text`、`RichText`（文本片段、链接）、`Selectable`；Markdown 用 `std/markdown`（由 Asuka 解析，各平台结果一致，再用平台自己的富文本控件绘制） |
| 按钮（样式、图标、菜单） | `Button`、`Menu` | `Pressable` | ✓ `Button`（`variant`：plain、filled、tinted、gray、bordered、glass、glass prominent；`icon`；`menu`）、`Menu`（点按或 `longPress`：上下文菜单）；菜单项可以 `disabled`、`checked`，可以放进 `.submenu` 或带标题的 `.section`；栏按钮也能打开菜单 |
| 文本输入框 | `TextField`、`SecureField`、`TextEditor` | `TextInput` | ✓ `TextField`（`secure`、`keyboard`、`returnKey`、用于自动填充的 `contentType`、`multiline`、`plain`） |
| 光标、在光标处插入 | `TextSelection` | `selection`、`onSelectionChange` | ✓ `bind selection`；`Ref` 的 `insert(text)` |
| 富文本编辑（比如邮件正文） | 配合 `AttributedString` 的 `TextEditor`（iOS 26） | 社区方案（10tap、pell） | ✓ `bind rich`，`Ref` 的 `toggleFormat`、`format`、`setLink`，`on formats`（分别是 `UITextView`、`EditText`、`NSTextView`、富文本编辑控件、`GtkTextView`、`contenteditable`） |
| 键盘上方的工具栏 | `.toolbar { ToolbarItemGroup(placement: .keyboard) }` | `InputAccessoryView`（iOS） | ✓ `TextField` 的 `accessory { … }`（iOS 上是 `inputAccessoryView`；Android 上是一条跟随键盘的栏；浏览器里只在触摸屏上显示） |
| 粘贴图片、键盘里的 GIF 和贴纸 | `onPasteCommand`、`PasteButton` | 社区方案（`onPaste`） | ✓ `pasteTypes`、`on paste`（粘贴的内容，或键盘直接输入的内容） |
| 开关、滑块 | `Toggle`、`Slider` | 社区方案 | ✓ `Toggle`、`Slider` |
| 分段控件 | `Picker(.segmented)` | 社区方案 | ✓ `Segmented` |
| 日期和时间 | `DatePicker` | 社区方案 | ✓ `DatePicker`、`time.Picker`（`mode`：日期、时间或两者；`inline`） |
| 进度条、加载指示器 | `ProgressView` | `ActivityIndicator` | ✓ `Progress`、`ProgressRing`（有无具体数值均可）、`Activity` |
| 滚轮选择器、菜单选择器、步进器 | `Picker`、`Stepper` | 社区方案 | ✓ `Wheel`（`UIPickerView`、`NumberPicker`；桌面端和 Web 上是弹出菜单）、`Picker`（iOS 和 Mac 上是弹出按钮，Android 上是 Material 的 exposed dropdown menu，Windows 上是按 WinUI 样式绘制的组合框，`GtkDropDown`，`<select>`）、`Stepper` |
| 颜色选择器 | `ColorPicker` | 社区方案 | ✓ `ColorPicker`（`UIColorWell`；Android 没有系统颜色选择器，所以用一个带调色板的 Material 对话框，支持不透明度；`NSColorWell`、Windows 的颜色对话框、`GtkColorDialogButton`、`<input type="color">`） |
| 图标 | SF Symbols | `@expo/vector-icons` | ✓ `Symbol`（iOS 和 Mac 上用 SF Symbols；同样的名字在 Android 和 Web 上对应 Material Symbols，在 Windows 上对应 Segoe Fluent Icons，在 Linux 上对应图标主题里的图标或 Material Symbols：这些平台支持的是一组精选图标）；SVG 图标用 `std/svg` 以矢量方式绘制 |
| 图片（远程加载、缓存、占位图） | `AsyncImage` | `expo-image` | ✓ `Image`（`placeholder`，BlurHash 和 ThumbHash 占位图（`blurHash`、`thumbHash`），淡入的 `transition`；在内存和磁盘上缓存（http(s) 图片跨启动保留）；解码用各平台自己的解码器：ImageIO、`ImageDecoder`、`NSImage`、WIC、GDK 的加载器、浏览器，所以平台能读的 WebP、HEIC 和 AVIF 都能显示；动图 GIF、APNG 和 WebP 在所有平台上都能播放，用 `animated` 控制；SVG 在 Mac、Linux 和 Web 上可用） |
| Lottie 动画 | 社区方案（lottie-ios） | `lottie-react-native` | ✓ `std/lottie` 的 `lottie.View`（lottie-ios、lottie-android、lottie-web：JSON 和 dotLottie，`playing`、`loop`、`speed`，可读可写的 `progress`，`segment`、`marker`，按 keypath 的 `overrides`）；在 Windows 和 Linux 上是 Asuka 自己在 `Canvas` 上的播放器：形状、渐变、修剪路径、中继器、星形、预合成和 Alpha 遮罩，不支持蒙版、图片和文字 |
| 视频 | `VideoPlayer` | `expo-video` | ✓ `Video`（iOS 和 Mac 上用 AVKit，Android 上用 Media3，Windows 上是 WebView2 里的 `<video>`，Linux 上是 `GtkVideo`（GStreamer），Web 上用 `<video>`） |
| 网页内容 | `WKWebView` | `react-native-webview` | ✓ `WebView`（iOS 和 Mac 上用 `WKWebView`，Android 的 `WebView`，Windows 上用 WebView2，Linux 上用 WebKitGTK：加载 URL 或 HTML，双向传递消息，在页面脚本之前运行脚本（`preload`），拦截请求，页面状态和历史记录，下载，新窗口，每一个请求，脚本的返回值，`webview.cookies(url)`；Web 上是 `<iframe>`，以上功能只对应用自己站点的页面完全可用）；`platform/js` 可以在屏幕外运行 JavaScript |
| 地图 | `Map` | `expo-maps` | ✓ `platform/maps` 的 `maps.View`（iOS 和 Mac 上用 MapKit，Android 上用 Google Maps，Windows、Linux（libshumate）和 Web 上用 OpenStreetMap 的瓦片：区域、标记、用户位置、点按） |
| 自定义绘制 | `Shape`、`Path`、`Canvas` | `react-native-svg`、Skia | ✓ `Shape` 和 `Path`（填充、描边、线帽、连接方式、虚线，以及可动画的裁剪）；`Canvas`（路径、渐变、文字、变换、裁剪：分别用 Core Graphics、Android 的 `Canvas`、GDI+、GSK、2D `<canvas>` 绘制）；`std/svg` |
| 图表 | Swift Charts | `victory-native`、社区方案 | ✓ `std/chart`（柱状图、折线图、面积图、散点图；用 `svg.Shape` 绘制，各平台一致） |
| 二维码（生成） | Core Image 的 `CIQRCodeGenerator` | `react-native-qrcode-svg`、社区方案 | ✓ `std/qrcode`（`qrcode.View(text)`、`encode` 和 `svg()`：用 Asuka 写成，各平台一致；Flutter：`qr_flutter`） |
| 着色器 | `.colorEffect(ShaderLibrary...)` | Skia 的 runtime effect | ✓ `shader` 声明（用 Asuka 编写，随应用一起编译：iOS 和 Mac 上是 Metal，Android 13 及以上是 AGSL，Web 上是 WebGL 2，Windows 上是 Direct3D 11，Linux 上是 OpenGL） |
| 原生扩展 | `UIViewRepresentable` | 原生模块 | ✓ 原生包：在 Asuka 里声明 `native interface` 或 `native component`，在 iOS 和 Mac 上用 Swift 实现，Android 上用 Kotlin，Web 上用 JavaScript，Windows 和 Linux 上用 C（见[原生代码](native.md)）；C 库用 `extern` 直接调用（见[在 Asuka 中调用 C](c.md)） |
| 分页、页码圆点 | `.scrollTargetBehavior(.paging)`、`TabView(.page)` | `react-native-pager-view` | ✓ `Scroll` 的 `paging`、`PageControl` |
| 复选框 | `Toggle`（`.checkbox`，macOS） | 社区方案 | ✓ `Toggle { checkbox: true }` |

## 导航与呈现

| | SwiftUI | Expo (expo-router) | Asuka |
|---|---|---|---|
| 导航栈：原生的推入、弹出和返回手势 | `NavigationStack` | Stack | ✓ `Stack`、`push`、`pop` |
| 标签页 | `TabView` | Tabs | ✓ `Tabs`、`Tab` |
| 侧边栏、抽屉 | `NavigationSplitView` | Drawer | ✓ `Split`、`Destination`、`Sidebar`（iPhone 上是导航栈，Android 上是抽屉；`UISplitViewController`、`NSSplitViewController`，Windows 上按 WinUI 的 `NavigationView` 窗格绘制，Linux 上是 libadwaita 的分栏视图） |
| 标签页和侧边栏行上的徽标 | `badge` | `tabBarBadge`、`drawerItem` 的计数 | ✓ `Tab` 和 `Destination` 的 `badge`（`badgeValue`、`BadgeDrawable`、抽屉里的计数；Mac 上是来源列表的计数；Fluent 的 `InfoBadge`；Linux 和 Web 上显示一个计数） |
| 栏按钮、标题、搜索 | `.toolbar`、`.searchable` | header 选项 | ✓ 页面的 `title`、`largeTitle`、`toolbar { … }`、`bind search` |
| 底部工具栏、主操作（FAB） | `ToolbarItem(placement: .bottomBar)` | — | ✓ 页面的 `bottomToolbar`、`primary`、`primaryExtended`：UIKit 的工具栏，Material 的 `BottomAppBar` 和（扩展）FAB，Mac 上窗口的工具栏，Windows 上的命令栏，Linux 上的 `GtkActionBar`，Web 上的一条栏和一个浮动按钮 |
| 带档位的弹出面板 | `.sheet`、`.presentationDetents` | modal | ✓ `Sheet`（`detents`、`grabber`） |
| 全屏模态 | `.fullScreenCover` | `presentation: "fullScreenModal"` | ✓ `Sheet { fullScreen: true }`（iOS 的 `.fullScreen`；Material 的全屏对话框；在桌面端覆盖整个窗口内容；在 Web 上是覆盖页面的 `<dialog>`） |
| 拦截返回、阻止关闭 | `.navigationBarBackButtonHidden`、`.interactiveDismissDisabled` | `BackHandler`、`usePreventRemove` | ✓ `onBack { … }`（页面、弹出面板、整个应用）、`backGesture: false` |
| 气泡弹窗 | `.popover` | — | ✓ `Popover`（iPhone 上也是气泡弹窗） |
| 警告框、操作表 | `.alert`、`.confirmationDialog` | `Alert` | ✓ `Alert`（`actionSheet`；可以放输入框，密码框用 `secure`，按钮可以通过 `disabled` 跟随输入框的内容） |
| Snackbar、Toast（带撤销的简短消息） | — | 社区方案（react-native-paper 的 `Snackbar`） | ✓ `snackbar(message, …)`（Android 上是 Material 的，Linux 上是 `AdwToast`；在 iOS、macOS、Windows 和 Web 上按平台的样子绘制） |
| 深层链接、通用链接和 App Links | `.onOpenURL` | Linking | ✓ `Router` 会自己跳转到路由对应的链接（应用的 scheme、通用链接和 App Links、通知里的 URL）；其余的交给 `on openURL(url)`；在 `asuka.toml` 里配置 `scheme` 和 `links` |
| URL 路由：路径模式、嵌套导航器、链接下的初始路由、重定向、守卫、未找到 | 配合 `Codable` 值的 `NavigationPath` | expo-router（`[id]`、`[...rest]`、`unstable_settings`、`<Redirect>`、`Stack.Protected`、`+not-found`） | ✓ `Router` 和 `Route`（`:id`、`*rest`、`initial`、`redirect`、`guard`；`layout`：导航栈、标签页、分栏视图、插槽；`sheet: true`）、`navigate`、`replace`、`router.back()`、`router.setQuery`，按钮和视图上的 `href`；状态可以从地址恢复（刷新页面、桌面窗口上次的位置、`asuka dev` 的重载）（见[路由](navigation.md#路由)） |
| 类型化链接 | `NavigationPath` 的值 | 类型化路由（生成的 `.d.ts`） | ✓ 调用一个带路由的页面就是它的地址，编译时检查：`navigate(Thread(id: 7))`、`href: Thread(id: 7)` |
| 下拉刷新、滑动操作 | `.refreshable`、`.swipeActions` | `RefreshControl` | ✓ `on refresh`、`refreshing`；两侧都支持：`swipeActions`、`leadingSwipeActions` |

## 动画

| | SwiftUI | Expo (Reanimated) | Asuka |
|---|---|---|---|
| 为变化添加动画 | `withAnimation` | `withTiming`、`withSpring` | ✓ `animate { … }`、`animate(.spring) { … }`（一个 `Animation`：时长、缓动、线性、弹簧、延迟、重复、往返）（见[动画与手势](motion.md)） |
| 变换 | `.rotationEffect`、`.scaleEffect`、`.offset` | `transform` | ✓ `transform: Transform(…)` |
| 进入和退出过渡 | `.transition` | entering/exiting | ✓ `transition`（`.opacity`、`.pop`、`.slide`，或自己定义的 `Transition(…)`） |
| 逐帧动画 | `TimelineView(.animation)` | `useFrameCallback` | ✓ `frames()`、`onFrame` |
| 阴影、渐变、模糊 | `.shadow`、`LinearGradient`、材质 | style、expo-linear-gradient、expo-blur | ✓ `shadow`、`gradient`、`material` |
| 手势 | 点按、长按、拖动、缩放、旋转 | Gesture Handler | ✓ `on press`、`on longPress`、`on pan`、`on pinch`、`on rotate`、`on swipe` |
| 硬件键盘的按键 | `.onKeyPress`、`.keyboardShortcut` | `onKeyPress`（`TextInput` 的；React Native for Windows and macOS 上是 `onKeyDown`） | ✓ `keys`（从获得焦点的视图开始往外找，优先于控件和菜单）、`useKeys`、`WindowKeys`（整个窗口的） |
| 按下和悬停状态 | `ButtonStyle` 的 `isPressed`、`.onHover` | `Pressable` 的 `pressed`、`onHoverIn`/`onHoverOut` | ✓ `pressed { … }` 和 `hovered { … }` 变体；用 `Ref` 的 `pressed` 和 `hovered` 跟随一组视图或相邻视图的状态；`on pressedChange`、`on hoverChange` |
| 拖放 | `.draggable`、`.dropDestination` | （没有内置） | ✓ `dragItems`、`dropTypes`、`on drop`（应用内拖放应用自己的值；与其他应用之间拖放文本、链接和文件；`dragging { … }` 和 `dragOver { … }` 变体） |
| 共享元素过渡 | `matchedGeometryEffect`、`.navigationTransition(.zoom)` | shared transitions | ✓ `shared`（一个视图从另一个视图的位置和大小长出来）、页面的 `zoomFrom`（iOS 18 的缩放过渡；Material 的容器变换） |
| 触感反馈 | `sensoryFeedback` | `expo-haptics` | ✓ `device.haptic(…)`（Mac 上用 Force Touch 触控板，浏览器里用 `navigator.vibrate`） |
| 减弱动态效果 | `accessibilityReduceMotion` | `useReducedMotion` | ✓ `environment().reduceMotion`（iOS 上过渡改为淡入淡出；桌面端和 Web 上 `animate` 里的变化会立即生效） |

## 数据与服务

| | SwiftUI / Apple | Expo | Asuka |
|---|---|---|---|
| 状态 | `@State`、`@Observable` | hooks | ✓ `state`、store、派生的 `let`（细粒度的 signal：一次写入只更新读取它的地方）（见[核心概念](concepts.md)） |
| 持久化的小数据 | `@AppStorage` | AsyncStorage | ✓ `std/storage` 的 `storage.Value`、`std/store`（UserDefaults、SharedPreferences、Windows 和 Linux 上的一个文件、`localStorage`） |
| SQLite | SwiftData / GRDB | `expo-sqlite` | ✓ `std/sqlite`（`migrate`，把行直接解码成 `T` 的 `query<T>`，`exec`，事务，都在主线程之外执行；`watch<T>`：实时查询，读取的表被写入或参数变化时重新运行；每个平台都是同一个 SQLite，浏览器里运行在页面的一个 worker 中）（见[数据](data.md)） |
| 安全存储 | 钥匙串 | `expo-secure-store` | ✓ `platform/secure`（钥匙串、Android Keystore、凭据管理器、libsecret；Web 上用浏览器不允许导出的密钥加密后存进 IndexedDB，并不是真正的钥匙串）；`protected: true`：要先通过 Face ID、指纹、Windows Hello 或密码验证才能读出，相当于 `requireAuthentication` |
| 文件、网络 | Foundation | `expo-file-system`、fetch | ✓ `std/fs`；`std/http`（用平台自己的客户端：URLSession、`HttpURLConnection`、WinHTTP、libsoup、`fetch`）、`http.download`、`http.stream` 和服务器发送事件（`http.events`）；`std/websocket` |
| 照片 | `PhotosPicker` | `expo-image-picker` | ✓ `device.pickPhotos(max:)` |
| 文档 | `UIDocumentPickerViewController` | `expo-document-picker` | ✓ `device.pickFiles(types:, multiple:)`；桌面端用 `platform/dialog` |
| 相机 | AVFoundation | `expo-camera` | ✓ `platform/camera`：`camera.take()`（系统相机；在 Mac、Windows 和 Linux 上是一个带实时预览的对话框）、`camera.View`（实时预览，前置或后置）、`capture()`、`record()`（AVFoundation、CameraX、Media Foundation、GStreamer、`getUserMedia`）；`platform/record` 可以录制带声音的视频 |
| 条形码、二维码 | AVFoundation、Vision | `expo-camera` 的扫码 | ✓ 在 `camera.View` 里实时识别（`scan`、`on code`），也能识别图片文件（`scan.file`）：Vision，Android 上用 ML Kit（只打包进导入了它的应用），浏览器有 `BarcodeDetector` 时用它，Windows 和 Linux 上用 Asuka 自己用 C 写的识别器（QR、Data Matrix、EAN、UPC、Code 128、Code 39） |
| 定位 | Core Location | `expo-location` | ✓ `platform/location`（Core Location、Android 的 location manager、WinRT 的 `Geolocator`、GeoClue 或定位 portal、Geolocation API；使用期间和始终，精确或大致位置；地名；后台更新和地理围栏，iOS 和 Android 上会为这些事件启动应用，并在处理期间保持运行） |
| 翻译、复数、地区格式 | String Catalogs、FormatStyle | `expo-localization`、i18next | ✓ 文案目录和 `tr.key(…)`、`std/i18n`（见[多语言](i18n.md)） |
| 从右到左布局 | `layoutDirection` | `I18nManager` | ✓ 跟随语言自动处理（`start` 和 `end` 会镜像）；对于在所有语言里方向都固定的内容，用 `layoutDirection` |
| 本地通知 | UserNotifications | `expo-notifications` | ✓ `platform/notify`（立即显示或定时显示，可带图片；iOS 和 macOS 上用 UserNotifications，Android 的通知，Windows 的 toast（带按钮和回复），Linux 的桌面通知；Web 上通过应用的 service worker 实现：应用有页面打开时定时通知会按时显示，否则在下一次打开页面时显示，因为浏览器无法定时显示通知） |
| 推送通知 | APNs | `expo-notifications` | ✓ `platform/push`（iOS 和 macOS 上用 APNs，Android 上用 Firebase Cloud Messaging，Web 上用基于 VAPID 的 Web Push：`register()` 获取 token，`received()`、`opened()`） |
| 通知操作、内联回复 | `UNNotificationCategory`、`UNTextInputNotificationAction` | `setNotificationCategoryAsync` | ✓ `notify.setCategories`、`notify.responses()`（操作和回复；`r.defer()` 让应用保持运行直到处理完；Web 上用 service worker 的通知操作，Chrome 支持时还有内联回复） |
| 通知分组、正在显示的通知、角标 | `threadIdentifier`、`getDeliveredNotifications`、`setBadgeCount` | `getPresentedNotificationsAsync`、`setBadgeCountAsync` | ✓ 通知的 `thread`（Android 上是一个分组，并维护它的摘要通知）、`notify.showing()`、`remove`、`removeThread`、`setBadge` |
| 前台时的通知 | `willPresent` | `setNotificationHandler` | ✓ `notify.present(decide)` |
| Android 通知渠道 | — | `setNotificationChannelAsync` | ✓ `notify.setChannels`、`setChannelGroups`；推送里也可以指定渠道（iOS 没有渠道） |
| 会话里的消息 | 通信通知（`INSendMessageIntent`） | 社区方案 | ✓ `Notification(…).from(person, conversation)`、`Message.from`：iOS 的通信通知，Android 的 MessagingStyle 和会话快捷方式；其他平台显示为普通通知 |
| 在推送显示之前修改它 | Notification Service Extension | 社区方案（`expo-notifications` 不支持） | ✓ `push.rewrite(f)`：应用的代码运行在 iOS 的 Notification Service Extension 里（由 `asuka build` 构建并签名），在 Android 上运行在处理数据消息的 Firebase 服务里 |
| 剪贴板、分享面板、浏览器 | `UIPasteboard`、`ShareLink`、`SFSafariViewController` | `expo-clipboard`、`expo-sharing`、`expo-web-browser` | ✓ `device.copy(text)`、`device.clipboardText()`、`clipboardHas`、`clipboardImage`、`copyImage`、`clipboardHTML`、`copyHTML`（富文本）、`clipboardChanges()`；`device.share(text)`、`device.shareFiles(paths)`（Mac 上的分享菜单、Windows 的分享界面、浏览器的分享；Linux 没有分享面板，会复制到剪贴板并弹出提示）；`device.browse(url)`（`SFSafariViewController`、Custom Tab）、`device.openURL(url)` |
| 预览文件 | `QLPreviewController`、`.quickLookPreview` | `react-native-file-viewer` | ✓ `platform/preview`（iOS 和 Mac 上用快速查看，支持多个文件和标记；Android、Windows 和 Linux 上用打开该文件的应用；Web 上在新标签页打开） |
| 在界面里显示 PDF | PDFKit 的 `PDFView` | `react-native-pdf` | ✓ `platform/pdf` 的 `pdf.View`（`source`、可绑定的 `page`、`loaded`、`pageChange`；PDFKit、Android 的 `PdfRenderer`、`Windows.Data.Pdf`、Poppler、浏览器的查看器；Flutter：`pdfx`） |
| 打印、从 HTML 生成 PDF | `UIPrintInteractionController`、`UIPrintPageRenderer` | `expo-print` | ✓ `platform/print`（打印文件、HTML、`WebView`，使用系统的打印对话框并返回结果；`print.pdf`：分页、按地区选纸张大小、页边距） |
| 邮件和短信编辑界面 | `MFMailComposeViewController`、`MFMessageComposeViewController` | `expo-mail-composer`、`expo-sms` | ✓ `platform/compose`（`compose.mail`：收件人、主题、HTML 正文、附件，iOS 上能知道用户做了什么，其他平台交给邮件应用；`compose.message`：iOS 的信息、Android 的短信应用） |
| 保持屏幕常亮、设备和安装信息 | `isIdleTimerDisabled`、`UIDevice` | `expo-keep-awake`、`expo-device`、`expo-application` | ✓ `device.keepAwake(on)`、`device.info()`（含应用来源和安装时间） |
| 网络状态 | `NWPathMonitor` | `expo-network` | ✓ `platform/network`（`network.status()`：是否在线、Wi-Fi 还是蜂窝网络、是否按流量计费、低数据模式；NWPathMonitor、ConnectivityManager、网络列表管理器、NetworkManager、Network Information API） |
| 保存到相册、下载目录 | `PHPhotoLibrary` | `expo-media-library` | ✓ `platform/library`（`library.save`：照片、Android 的相册和下载目录、音乐；桌面端的下载、图片、影片和音乐文件夹） |
| 后台任务 | `beginBackgroundTask`、`BGTaskScheduler` | `expo-task-manager`、`expo-background-task` | ✓ `platform/background`：Android 会让应用保持运行，并在通知里显示进度和按钮；iOS 大约多给 30 秒；桌面端让应用不被挂起（App Nap、睡眠）。`platform/periodic`：系统不定期运行的任务，会在没有界面的情况下启动应用（BGTaskScheduler、WorkManager；桌面端是应用运行期间的定时器） |
| 接收其他应用的分享 | 分享扩展 | `expo-sharing`（`useIncomingShare`） | ✓ `on share(items)`：文本、链接、图片、视频、音频和文件，可以是一个或多个，带名称、MIME 类型和大小（`asuka.toml` 的 `shareTarget`：据此生成 Android 的 intent filter，以及 iOS 的分享扩展和激活规则）；`platform/sharesheet`：在分享来源应用上方显示应用自己的面板（iOS 的分享扩展；Android 的一个 activity），数据与应用共享；仅 iOS 和 Android |
| 音频 | AVAudioPlayer、AVAudioRecorder | `expo-audio` | ✓ `platform/audio`（`audio.play`、`audio.Player`：播放、暂停、跳转、音量、速率、循环；AVPlayer、Media3 的 ExoPlayer、MCI、GStreamer、`<audio>`）；`platform/record`（`record.audio`：音质、暂停、电平） |
| 正在播放、锁屏控制、媒体键 | `MPNowPlayingInfoCenter`、`MPRemoteCommandCenter` | `expo-audio` 的锁屏控制、社区方案（`react-native-track-player`） | ✓ `platform/nowplaying` 的 `nowplaying.NowPlaying`（标题、艺术家、封面、时长、由系统推进的播放位置，播放、暂停、下一首、上一首、跳转；可以自动跟随一个 `audio.Player`；Android 上是 MediaSession，Windows 上是 SystemMediaTransportControls，Linux 上是 MPRIS，Web 上是 Media Session API） |
| 通过 Apple 登录 | `SignInWithAppleButton` | `expo-apple-authentication` | ✓ `platform/appleauth`（Apple 的 `SignInButton`、`signIn`、`credentialState`；iOS 和 macOS；其他平台通过 `platform/webauth`） |
| 通过 Google 登录 | GoogleSignIn | `@react-native-google-signin/google-signin`、`expo-auth-session` | ✓ `platform/googleauth`（Android 上用 Credential Manager；iOS 和 Mac 上在系统的登录面板里打开 Google 的页面，使用 PKCE；Windows 和 Linux 上用回环地址；Web 上用弹出窗口） |
| Web 登录（OAuth） | `ASWebAuthenticationSession` | `expo-auth-session`、`expo-web-browser` | ✓ `platform/webauth`（`ASWebAuthenticationSession`、Custom Tab、WebView2 窗口、WebKitGTK 对话框、弹出窗口） |
| iCloud：键值存储、iCloud 云盘 | `NSUbiquitousKeyValueStore`、ubiquity 容器、`NSMetadataQuery` | 社区方案（`react-native-cloud-storage`、`expo-icloud-storage`） | ✓ `platform/icloud`（账户状态；`icloud.Value`，存放在 iCloud 的键值存储里；应用在 iCloud 云盘里的文稿，随变化列出，附带下载和上传状态，以协调方式读写，下载、移除本地副本、解决冲突；测试用 `icloud.fake()`；iOS 和 macOS；不支持 CloudKit 数据库） |
| Apple Pay、Google Pay | PassKit（`PayWithApplePayButton`） | `@stripe/stripe-react-native` 的 Platform Pay、`react-native-payments` | ✓ `platform/pay`（钱包的支付面板和 `PayButton`；交给应用支付服务商的 token；修改配送方式、优惠券和卡片时更新总价；按字段报错；iOS、macOS 和 Safari 上用 Apple Pay，Android 和其他浏览器上用 Google Pay） |
| 面容 ID、指纹 | LocalAuthentication | `expo-local-authentication` | ✓ `platform/biometric`（LocalAuthentication、BiometricPrompt、Windows Hello；Web 上通过 WebAuthn 使用平台认证器；Linux 不支持） |
| 应用内购买、订阅 | StoreKit 2 | `expo-iap`、`react-native-iap`、RevenueCat | ✓ `platform/purchase`（带本地化价格和优惠的商品、购买、待处理的付款、从应用启动起的交易流、完成交易、权益、恢复购买、订阅状态、管理订阅/退款/兑换码面板；iOS 和 macOS 上用 StoreKit 2，Play Billing，Microsoft Store，Trusted Web Activity 里的 Digital Goods API；测试用 `purchase.fake()`）（见[应用内购买](purchases.md)） |
| 应用商店评分 | `requestReview` | `expo-store-review` | ✓ `platform/review`（App Store 的评分弹窗、Play 的应用内评价） |
| 跟踪权限 | App Tracking Transparency | `expo-tracking-transparency` | ✓ `platform/tracking` |
| 打开应用的设置页（权限被拒绝之后） | `UIApplication.openSettingsURLString` | `Linking.openSettings()` | ✓ `device.openSettings()`（iOS 上是应用的设置页，Android 上是“应用信息”，Mac 和 Windows 上是“隐私与安全性”） |
| 相机和麦克风的权限：查询或请求 | `AVCaptureDevice.authorizationStatus`、`requestAccess` | `Camera.getCameraPermissionsAsync`、`requestCameraPermissionsAsync`、`getMicrophonePermissionsAsync` | ✓ `camera.access()`、`camera.request()`、`record.access()`、`record.request()`（`.notAsked`、`.denied`、`.allowed`） |
| 应用完整性 | App Attest | `expo-app-integrity` | ✓ `platform/integrity`（App Attest、Play Integrity） |
| 年龄段 | Declared Age Range | `expo-age-range` | ✓ `platform/agerange`（Declared Age Range、Play Age Signals） |
| 传感器 | Core Motion | `expo-sensors` | ✓ `platform/sensors`（加速度计、陀螺仪、磁力计、设备运动、气压计、光线、计步器；所有平台坐标轴和单位一致；Core Motion、SensorManager、Linux 上 iio-sensor-proxy 提供的光线和方向、Generic Sensor API；Mac 和 Windows 不支持） |
| 健康数据 | HealthKit | 社区方案（`react-native-health`、Flutter 的 `health`） | ✓ `platform/health`（HealthKit、Health Connect：步数、距离、能量、心率、睡眠、体重等；样本、合计、写入） |
| 广告（AdMob） | Google Mobile Ads SDK | `react-native-google-mobile-ads`、Flutter 的 `google_mobile_ads` | ✓ `platform/ads`（自适应和固定尺寸横幅；插页式、激励、插页式激励和开屏广告；通过 Google 的 User Messaging Platform 征得同意；请求设置；Google 的测试广告单元；iOS 和 Android；测试用的 `ads.fake()`） |
| NFC 标签 | Core NFC | `react-native-nfc-manager` | ✓ `platform/nfc`（读写 NDEF，只读标签，标签 ID，原始命令；iOS 的系统面板，Android 的读卡器模式，可以打开应用的标签；Android 上 Chrome 的 Web NFC；桌面端不支持）（见 [NFC 标签](nfc.md)） |
| 低功耗蓝牙 | Core Bluetooth | 社区方案（`react-native-ble-plx`） | ✓ `platform/bluetooth`（扫描、连接、服务、读、写、通知；广播和 GATT 服务端；后台扫描和状态恢复；Core Bluetooth、Android 的 BLE、WinRT、BlueZ；Chromium 里 Web Bluetooth 的设备选择器，不支持外设角色）（见[蓝牙](bluetooth.md)） |
| 图片编辑 | Core Graphics、ImageIO | `expo-image-manipulator` | ✓ `platform/imageedit`（缩放、裁剪、旋转、翻转、填充、像素化；读入 HEIC，输出 JPEG 或 PNG，方向自动摆正；Image I/O、Android 的 `Bitmap`、WIC、GdkPixbuf、canvas） |
| 视频缩略图 | `AVAssetImageGenerator` | `expo-video-thumbnails` | ✓ `imageedit.videoFrame` |
| 把视图存成图片 | `ImageRenderer`、`drawHierarchy` | `react-native-view-shot` | ✓ `imageedit.capture(view: ref)`：PNG 或 JPEG，由平台来画（图层、Android 的 `View`，Windows 和 Linux 上取窗口的像素；Web 上是元素的 SVG 副本） |
| 识别图片中的文字、人脸和码 | Vision | 无 | ✓ `platform/vision`，同时给出每一项的位置（Vision；Android 上用 ML Kit，只打包进导入了它的应用；Windows 的 OCR 和人脸检测；Linux 上用 Tesseract 识别文字，不支持人脸；浏览器有 Shape Detection API 时用它） |
| 语音转文字 | Speech（`SFSpeechRecognizer`） | 社区方案（`expo-speech-recognition`） | ✓ `platform/transcribe`（识别一段录音，只在设备上进行：iOS 和 macOS 上用 Speech，Android 的设备端识别器（录音要 13 及以上，听写要 12 及以上），浏览器在设备上运行的识别器（Chrome 139 及以上）；Windows 的桌面语音识别器（SAPI），Linux 上发行版提供的 PocketSphinx（英语）；`transcribe.listen`：从麦克风边说边识别的听写） |
| 照片图库 | PhotoKit | `expo-media-library` | ✓ `platform/photos`：权限、列出照片、缩略图、原始文件（PhotoKit、MediaStore） |
| 文字转语音 | `AVSpeechSynthesizer` | `expo-speech` | ✓ `platform/speech`（队列、声音、音调、语速、暂停；AVSpeechSynthesizer、TextToSpeech、SAPI、Speech Dispatcher、`speechSynthesis`） |
| 通讯录 | Contacts、ContactsUI | `expo-contacts` | ✓ `platform/contacts`（`pick()` 和新建联系人界面不需要权限；访问通讯录需要权限） |
| 日历 | EventKit、EventKitUI | `expo-calendar` | ✓ `platform/calendar`（事件编辑界面不需要权限；读取日历和事件需要权限；EventKit、Calendar Provider） |
| 主屏幕小组件 | WidgetKit | `expo-widgets` | ✓ `platform/widget` 和 `asuka.toml` 的 `widgets`（每种尺寸一个模板，iOS 上还有锁屏小组件，或者应用自己的布局：列、行、层、文字、符号、图片、进度条、仪表、计时器、链接；时间线、倒计时、点按打开 URL；iOS 和 Android） |
| 实时活动 | ActivityKit | `expo-live-activity` | ✓ `platform/liveactivity`（iOS：锁屏和灵动岛；Android 16 的 Live Update；倒计时、进度、提醒，以及供服务端推送更新用的推送 token） |
| App Intents、快捷指令 | App Intents、App Shortcuts | — | ✓ `asuka.toml` 的 `shortcuts`（iOS：带 Siri 短语的 App Shortcuts，出现在聚焦搜索和快捷指令里，可以带参数：文字、数字、开关、选项，以及应用自己的数据（`platform/shortcuts` 的实体），短语里可以引用选项和实体；Android：启动器快捷方式，每项数据一个） |
| 自定义字体 | `.font(.custom)` | `expo-font` | ✓ `assets/fonts`、`Font(family:)` |
| 生命周期 | `scenePhase` | AppState | ✓ `onLifecycle` |
| 启动画面 | `UILaunchScreen` | `expo-splash-screen` | ✓ `asuka.toml` 的 `splash`；`platform/splash` 可以在应用加载期间保持它显示 |
| 更换应用图标 | `setAlternateIconName` | 社区（`expo-dynamic-app-icon`） | ✓ `asuka.toml` 的 `alternateIcons`、`platform/appicon` 的 `set`（iOS 的主屏幕；Android 的启动器，在应用离开屏幕时生效；Mac 的程序坞、Windows 的窗口和任务栏、Web 页面的图标；Linux 不支持，启动器显示的是 desktop 文件里的图标） |
| 状态栏、导航栏 | `statusBarHidden`、`preferredStatusBarStyle` | `expo-status-bar`、`expo-navigation-bar` | ✓ `screen.setBars`（`platform/screen`） |
| 锁定屏幕方向 | `supportedInterfaceOrientations` | `expo-screen-orientation` | ✓ `screen.lock` |
| 截屏 | `isCaptured`、`userDidTakeScreenshotNotification` | `expo-screen-capture` | ✓ `screen.preventCapture`（Android 上截图是黑的；iOS 上录屏时内容会被遮住；macOS 和 Windows 上截图里不包含这些窗口）、`screen.screenshots()` |
| 屏幕亮度 | `UIScreen.brightness` | `expo-brightness` | ✓ `screen.setBrightness`、`screen.brightness()`（iOS 和 Android：应用在前台时用应用的亮度，离开时恢复用户的亮度；桌面和 Web 交给用户自己调） |
| 崩溃报告 | MetricKit、Crashlytics、Sentry | Sentry、Crashlytics（`@react-native-firebase/crashlytics`） | ✓ `platform/crash`：Asuka 的致命错误（附 Asuka 调用栈）、未捕获的异常、MetricKit 记录的崩溃和卡顿、Android 的原生崩溃和 ANR、桌面端的崩溃，附带面包屑和自定义值；报告发到哪里由应用决定（没有内置服务）；`asuka build` 会生成符号文件 |

## 外观与无障碍

| | SwiftUI | Expo | Asuka |
|---|---|---|---|
| 深色模式、语义色、强调色 | 自动处理 | `useColorScheme` | ✓ 语义色、`environment().dark`、`asuka.toml` 的 `accent`（Android 上据此生成 Material 3 配色，`dynamicColor` 则取自壁纸） |
| 动态字体、字体缩放 | 自动处理 | `allowFontScaling` | ✓ 文本样式和自定义字体都会跟着缩放；`environment().fontScale` |
| 屏幕阅读器的标签、角色、隐藏 | `.accessibilityLabel`... | `accessibilityLabel`... | ✓ `label`、`hint`、`role`、`a11yHidden`；可点按的视图会被识别为按钮（见[无障碍](accessibility.md)） |
| 播报 | `AccessibilityNotification.Announcement` | `announceForAccessibility` | ✓ `accessibility.announce(text)` |
| 移动屏幕阅读器的焦点 | `@AccessibilityFocusState` | `setAccessibilityFocus` | ✓ `accessibility.focus(ref)` |
| 屏幕阅读器是否开启 | `voiceOverEnabled` | `isScreenReaderEnabled` | ✓ `environment().screenReader` |
| 自定义操作、可调节控件 | `.accessibilityAction`、`.accessibilityAdjustableAction` | `accessibilityActions` | ✓ `actions`、`on adjust`（列表的滑动操作也会成为 TalkBack 和 VoiceOver 的操作） |

## 工具链

| | Xcode / Expo | Asuka |
|---|---|---|
| 实时重载并保留状态 | 预览、Fast Refresh | ✓ `asuka dev`：开发 App 在每次保存后几毫秒内换上新版本，并保留所有 `state`；手机、桌面和浏览器都是如此（见[开发](development.md)） |
| 检查视图 | Xcode 的视图调试器、React Native DevTools、Flutter DevTools | ✓ `asuka dev` 的检查器：在浏览器中以树的形式显示正在运行的应用的视图（元素、文字、frame），指针指向的视图会在设备上高亮（见[开发](development.md#检查器)） |
| 检查开发环境 | Expo 的 doctor、`flutter doctor` | ✓ `asuka doctor`：这台电脑能为每个平台构建什么，以及其余的还需要安装什么 |
| 在手机上运行 | Xcode 的 Run、Expo 的开发构建 | ✓ `asuka dev --platform ios`、`android`，或两者同时（`ios,android`）：iOS 模拟器、Android 模拟器、Android 手机（`adb reverse`），以及通过 USB 或 Wi-Fi 连接的 iPhone（`--device iphone`，由你的团队签名） |
| 测试 | XCTest、Jest 加 React Native Testing Library | ✓ `asuka test`：`test` 块，应用无需设备即可挂载（`mount`、`app.tap`、`app.expect`），设备相关的包都有测试替身（见[测试](testing.md)） |
| 发布构建和签名 | Xcode、EAS Build | ✓ `asuka build`：应用预先编译；生成 `.ipa`、`.aab`、经过签名和公证的 `.app`（或 Mac App Store 用的 `.pkg`）、包含已签名 `.exe` 的 Windows 文件夹、附带 `.tar.gz` 和 Flatpak 清单的 Linux 安装前缀、静态网站（见[发布](shipping.md)） |
| 上传到应用商店 | Xcode、EAS Submit | ✓ `asuka publish`：TestFlight、App 审核、Google Play 的各个发布轨道；Mac App Store 和 Mac 的 TestFlight；桌面构建及其签名更新 |
| GUI 测试 | XCUITest、Detox、Maestro | ✓ `asuka test --gui`：在 iOS 模拟器、Android 模拟器、手机、无头 Chrome 或这台电脑上运行应用的流程（`flows/*.flow`：`tap Add`、`type hello`、`see Count: 1`、`back`）（见[GUI 测试](testing.md#gui-测试流程)）；桌面应用也可以执行操作步骤并截图（`ASUKA_SCRIPT`、`ASUKA_SNAPSHOT`） |
| 基准测试 | Instruments | ✓ `bench/`（与 SwiftUI 和 Compose 对比） |
| 嵌入现有应用 | brownfield（React Native 的方案） | ✓ `asuka build --platform ios --xcframework`、`asuka build --platform android --aar` |
| OTA 更新 | EAS Update | ✓ `asuka publish --update` 和 `platform/update`（应用的代码，由核心的虚拟机运行，经过签名，只发给原生指纹相同的构建；启动时崩溃会回滚） |

## 与桌面端框架对比

在 Mac、Windows 和 Linux 上，很多应用是用 web view 包一层 HTML 做出来的。这些框架和 Asuka 的对比如下：

| | Electron | Tauri | Wails | React Native for Windows and macOS | Asuka |
|---|---|---|---|---|---|
| 平台 | Mac、Windows、Linux | Mac、Windows、Linux；iOS 和 Android（Tauri 2） | Mac、Windows、Linux | Windows、Mac（配合 React Native 的 iOS 和 Android） | Mac、Windows、Linux、iOS、Android、Web |
| 界面由什么呈现 | Chromium 里的 HTML 和 CSS | 系统 web view（WKWebView、WebView2、WebKitGTK）里的 HTML 和 CSS | 系统 web view 里的 HTML 和 CSS | 平台的原生视图 | 平台的原生控件：AppKit；Win32 和通用控件，按 Windows 11 的样子绘制；GTK 4 和 libadwaita |
| 应用使用的语言 | JavaScript（页面之外还有 Node.js） | Rust，加上页面里的 JavaScript | Go，加上页面里的 JavaScript | JavaScript | Asuka（原生包里还可以有 Swift、C 或 JavaScript） |
| 随应用分发的内容 | Chromium 和 Node.js | 应用的二进制文件（web view 用系统自带的） | 应用的二进制文件（同上） | 一个 JavaScript 引擎和 React Native 的运行时 | 一个可执行文件，即预先编译好的应用；Linux 上的 GTK 4 和 libadwaita 由系统提供；除非应用显示了 `WebView`，否则不带 web view |

web view 应用在所有系统上都长得一样，完全按它的 HTML 和 CSS 呈现，并且能用上 Web 生态里的各种库。Asuka 应用则和各平台的原生应用看起来一样（不设置任何样式的应用，在 Mac 上就是一个 Mac 应用，在 Linux 上就是一个 GNOME 应用，在 Windows 上就是一个 Windows 11 应用），也能获得它们的无障碍支持、文本输入和菜单，同时所有平台共用同一套布局引擎。`asuka build` 在任何系统上都能构建 Windows 和 Linux 版本（借助 Zig，不需要安装 GTK）；Mac 版本则需要一台装了 Xcode 的 Mac。

## 桌面应用的能力

桌面应用在窗口和菜单栏之外还需要哪些能力，这里与 Electron 和 MyGo 做个对比（见[桌面端](desktop.md)）。除非该行另有说明，✓ 表示 Mac、Windows 和 Linux 都支持。

| | Electron | MyGo | Asuka |
|---|---|---|---|
| 控制窗口 | `BrowserWindow` | `Window` | ✓ `app` 块里的 `Window` 和其他 `Window`（只有这些 `Window` 的应用，比如托盘应用，没有主窗口）、`window.main()`、`window.current()`、窗口的 `Ref`（`Window as w`）：显示、隐藏、聚焦、最小化、最大化、全屏、尺寸、位置、置顶，并跟随窗口状态；`on closeRequest`、`WindowCloseGuard`；用 `stateKey` 记住窗口位置；`child` 和 `modal` 窗口；`titleBar: .hidden`（Wayland 下由合成器决定，无法设置位置、居中或置顶） |
| 托盘图标 | `Tray` | 托盘图标 | ✓ `platform/tray` 的 `tray.Tray`（`NSStatusItem`；通知区域；Linux 上的 StatusNotifierItem）：图片或符号、标题、提示、菜单、点击 |
| 子菜单、退出菜单项 | `MenuItem.submenu`、roles | roles、子菜单 | ✓ `MenuBar`、`Menu`、`MenuItem`（菜单项里嵌套菜单项就是子菜单；`role: .quit`、`.close`、`.openRecent`）；托盘和 Dock 菜单里用 `Action` 的 `.submenu` 和 `.section` |
| Dock 菜单、跳转列表任务 | `app.dock.setMenu`、`setUserTasks` | Dock 菜单、跳转列表 | ✓ `tray.DockMenu`（Dock 的菜单、跳转列表的“任务”、Linux 启动器的快捷列表） |
| 后台运行的应用 | `app.dock.hide`、在 `window-all-closed` 时不退出 | 后台应用 | ✓ `asuka.toml` 的 `keepRunning` 和 `background` |
| 浮出窗口：挂在托盘图标下的窗口 | 由应用把无边框 `BrowserWindow` 放到 `tray.getBounds()` 的位置 | 浮出窗口 | ✓ `Window` 的 `flyout`，挂在 `tray.anchor(id)` 或其窗口的某个矩形上：在显示器范围内定位并自动翻转，点击别处或按 Escape 关闭，上面可以再弹出模态对话框（Linux 上只有 X11 能定位） |
| 启动器面板 | `type: 'panel'`（macOS） | 面板 | ✓ `Window` 的 `panel`：浮在所有应用和空间之上，在 Mac 上获得键盘输入但不激活应用，按 Escape 或点击别处关闭（Linux 的 Wayland 下，有 gtk4-layer-shell 时是 layer-shell 表面） |
| 窗口大小随内容变化 | 由应用调用 `setContentSize` | `FitContent` | ✓ `fitContent`：Asuka 的布局会随内容变化调整窗口大小 |
| 鼠标穿透窗口 | `setIgnoreMouseEvents` | `SetIgnoreMouseEvents`、`SetInputRegion` | ✓ `clickThrough`、`w.setClickThrough(on)`，以及仍然接收鼠标的视图上的 `takesClicks` |
| 列出已安装的应用（给启动器用） | 无 | `InstalledApps`、`FileIcon`、`LaunchApp` | ✓ `platform/apps` 的 `list`、`search`、`icon`、`open` |
| 全局快捷键 | `globalShortcut` | 全局快捷键 | ✓ `platform/hotkey` 的 `hotkey.Hotkey`（Carbon 的热键；`RegisterHotKey`；X11 的按键抓取或 Wayland 的 GlobalShortcuts portal） |
| 打开、保存和选择文件夹对话框 | `dialog.showOpenDialog`、`showSaveDialog` | 对话框 | ✓ `platform/dialog` 的 `open`、`save`、`chooseFolder`、`chooseFolders`（Mac 上以表单形式附在窗口上，Windows 的通用文件对话框，GTK 的文件对话框或 portal；Web 上用 File System Access 选择器或文件输入框；手机上只有 `open`） |
| 在文件夹中显示、用默认应用打开、移到废纸篓 | `shell.showItemInFolder`、`openPath`、`trashItem` | shell | ✓ `platform/shell` 的 `reveal`、`open`、`trash`、`beep` |
| 文件关联 | `fileAssociations`（electron-builder）、`open-file` | 文件关联 | ✓ `asuka.toml` 的 `fileTypes`、`on files(paths)`（iOS、Android 和已安装的 Web 应用也支持） |
| 单实例、第二次启动 | `requestSingleInstanceLock`、`second-instance` | 单实例 | ✓ `on secondInstance(args, dir)`、`asuka.toml` 的 `singleInstance`（Mac 传递的是文件，不是命令行参数） |
| 退出、退出前询问 | `app.quit`、`before-quit` | 退出 | ✓ `quit()`、`relaunch()`、`on quit(e)`（`e.cancel()`、`e.sessionEnding`） |
| 应用的目录 | `app.getPath` | 目录 | ✓ `fs.appDir()`（应用的文件）、`configDir()`（设置：Windows 上是 Roaming AppData，Linux 上是 `~/.config`）、`cacheDir()`（Library/Caches、Android 的缓存目录、`~/.cache`；Web 上单独保存）和 `logDir()`（Library/Logs、`~/.local/state`），在所有平台上都放在各平台约定的位置 |
| 最近使用的文档 | `app.addRecentDocument` | 最近文档 | ✓ `platform/recent`（Mac 上的“打开最近使用”、跳转列表、Linux 上用户的最近文件） |
| 基于文档的应用 | — | 文档 | ✓ `platform/document`：`Documents`，每个文件一个窗口，“文件”菜单，自动保存，恢复副本，关闭和退出时询问是否保存，处理被其他应用修改的文件，版本（浏览所有版本…），锁定（手机和 Web 上一次只打开一个文档，原地保存） |
| 登录时启动 | `app.setLoginItemSettings` | 登录时启动 | ✓ `platform/login`（`SMAppService`，macOS 13 及以上；Run 注册表项；XDG 自启动或 Background portal） |
| 监听文件变化 | Node 的 `fs.watch`（chokidar） | `WatchFiles`、watch 插件 | ✓ `std/watch` 的 `watch.path`：重命名成对报告，原子保存视为写入，连续的变化会合并；手机和 Web 不支持 |
| 显示器 | `screen` | 显示器 | ✓ `screen.displays()`、`nearest`、`matching`（Wayland 下没有工作区域；手机和 Web 上是它们自己的屏幕） |
| 截取屏幕 | `desktopCapturer` | — | ✓ `screen.capture(display)`：显示器内容的 PNG（Mac 上用 ScreenCaptureKit，需要录屏权限；Windows 上取显示器的像素；Linux 上用截图门户；浏览器里由用户在选择框中挑选；手机上是应用自己的窗口） |
| 睡眠、唤醒、锁屏、空闲时间、电池 | `powerMonitor` | 电源 | ✓ `platform/power`（手机：电池、低电量模式、锁屏；Web：电池） |
| 应用图标上的进度和提醒 | `setProgressBar`、`flashFrame`、`dock.bounce` | Dock 和任务栏 | ✓ `dock.setProgress`、`dock.requestAttention`（Linux：支持 Unity 启动器 API 的 dock） |
| 强调色、增强对比度、降低透明度 | `systemPreferences` | 强调色和辅助功能设置 | ✓ `platform/appearance`（Linux 上没有降低透明度） |
| 正在播放和媒体键 | （无：只能在页面里用 `navigator.mediaSession`） | 正在播放 | ✓ `platform/nowplaying`（`MPNowPlayingInfoCenter`；窗口的 SystemMediaTransportControls；一个 MPRIS 播放器） |
| 自动更新：签名的版本、增量更新、Sparkle 式窗口 | `autoUpdater`（Squirrel）、electron-updater | 更新器及其插件 | ✓ `platform/update`（`update.UpdateWindow`、`update.AppMenu()`、`checkForUpdates`、`check`、`install`、`relaunch`；Ed25519 签名、增量更新、更新渠道；GitHub releases、S3 存储桶或任意 HTTPS 目录，或者你自己的服务器；`asuka publish --platform mac\|windows\|linux`）；由系统包管理器、Flatpak 或应用商店安装的应用通过它们更新；Web 上刷新页面即可 |

## 尚未支持

上面各表中没有完全缺失的功能：标为 ◐ 的行或行内的说明会写明还缺什么。
