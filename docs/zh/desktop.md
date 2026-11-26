# 桌面端

除了窗口和菜单栏（见[导航](navigation.md#窗口)），桌面端还有许多手机上没有的东西：托盘图标和程序坞（Dock）菜单、浮出面板、在后台常驻的应用、全局快捷键、面板与启动器、查找已安装的应用、鼠标穿透窗口、文件对话框与文件管理器、应用打开的文件、单实例、退出、最近使用的文档、基于文档的应用、登录时启动、自动更新、“正在播放”与媒体键、显示器、电源、应用图标上的进度，以及系统外观。如果想跟踪文件和文件夹的变化（比如应用正在展示的文件夹，或其他应用保存的文档），请参阅[监视文件](data.md#监视文件stdwatch)，这项功能在手机和 Web 上也有。

上面每一项都是平台 SDK（或标准库）里的一个包，或是 prelude 里的一个元素或函数，在 Mac、Windows 和 Linux 上都能用。手机或浏览器上没有某项功能时，会明确告诉你不支持（抛出 `unsupported`、`notAvailable` 错误，或者 `supported()` 为 false），或者什么也不显示，这样应用就可以只在支持的平台上提供它。

## 桌面应用是怎么构成的

在 Mac 上，应用是 `Name.app` 里的一个可执行文件，和 iOS 应用一样，原生包的 Swift 也构建在里面。它只针对一种架构：Apple 芯片，或者用 `--arch amd64` 构建的 Intel（不会把两者合成一个通用二进制，那样体积会翻倍）。在 Windows 和 Linux 上，一个应用就是一个可执行文件；在 Windows 上，应用的图标、清单和版本信息也直接打包进去。在任何系统上都能构建 Windows 和 Linux 应用（借助 Zig），也不需要安装 GTK。每个桌面应用都带有来自 `asuka.toml` 的应用设置（`asuka-app.json`：名称、ID、版本、强调色、URL scheme、文件类型、更新）：

| | 应用 | 设置 |
|---|---|---|
| macOS（`Name.app`） | `Contents/MacOS/Name` | `Contents/Resources/asuka-app.json` |
| Windows（一个文件夹） | `Name.exe` | `asuka-app.json`，与它放在一起 |
| Linux（一个安装前缀） | `bin/<exe>` | `share/<exe>/asuka-app.json` |

`asuka dev` 则在开发 App 中运行应用，每次保存都会立即生效（参见[开发](development.md#在桌面端)）。

## 托盘图标与程序坞菜单

`platform/tray` 的 `Tray` 在显示期间往托盘里放一个图标（它在所在的位置不显示任何东西）：在 Mac 上是菜单栏里的 `NSStatusItem`，在 Windows 上是通知区域里的图标，在 Linux 上是 StatusNotifierItem（KDE、Xfce、Cinnamon、MATE、Budgie 都支持，GNOME 需要安装 AppIndicator 扩展）。

```asuka
import platform/tray

component Focus {
  state minutes = 25
  tray.Tray("timer") {
    symbol: "moon.zzz"
    title: "{minutes}:00" // 在图标旁显示
    tooltip: "Focus timer"
    menu: [
      .button(title: "Start", run: () => start()),
      .submenu(title: "Length", items: [
        .button(title: "25 minutes", checked: minutes == 25, run: () => { minutes = 25 }),
        .button(title: "50 minutes", checked: minutes == 50, run: () => { minutes = 50 }),
      ]),
      .section(title: "", items: [.button(title: "Quit", run: () => quit())]),
    ]
  }
  Timer()
}

fn start() {}

component Timer {
  Text("25:00")
}
```

它的 id 在应用的各个图标中标识它（Mac 会把它放回用户按住 ⌘ 拖到的位置）。它的 `image`（一张资源图片或一个文件，绘制为 16 点高）或 `symbol`（在 Mac 上是模板图像，会跟随菜单栏的外观变化；`template: true` 让图片也成为模板图像）、`title`（Mac 上，以及会显示标签文字的 Linux 面板上可见）、`tooltip` 和 `menu` 都是绑定，变化时会重新发送；显示它的组件消失时，图标也随之消失。没有 `on click` 时，点击图标会打开菜单；有 `on click` 时，左键点击触发 `click`，右键点击才打开菜单。

它的 `menu` 和 `Menu` 的一样是一组 `Action`：`.button`（`disabled`、`checked`、`.destructive` 角色）、`.submenu`，以及用分隔线隔开的 `.section`。`tray.DockMenu { items: […] }` 在 Mac 上往程序坞图标的菜单里添加菜单项；在 Windows 上，这些菜单项会成为跳转列表（jump list）里的“任务”，点击后由正在运行的应用处理；在 Linux 上则成为启动器的快捷列表（quicklist）。同时显示多个时，以最后一个的菜单项为准。

`examples/tray` 是一个常驻在菜单栏里的计时器。

## 浮出面板

浮出面板（flyout）是一种从托盘图标上“垂下来”的窗口，就像 Mac 菜单栏附加功能弹出的面板，或者 Windows 通知区域弹出的面板那样；它也可以从某个窗口中的一块矩形区域弹出。它就是一个带有 `flyout` 的 `Window`：

```asuka
import platform/tray

component Power {
  state open = false
  tray.Tray("power") {
    symbol: "bolt.fill"
    menu: [.button(title: "Quit", run: () => quit())] // 右键点击时显示
    on click { open = !open }
  }
  Window {
    bind open: open
    flyout: Flyout(anchor: tray.anchor("power"), placement: .bottom)
    fitContent: true
    minWidth: 260
    maxHeight: 480
    Panel()
  }
}

component Panel {
  Text("Charging")
}
```

- 它没有窗口边框，也没有任务栏按钮。在 Mac 上，如果没有设置 `background`，它会在内容背后显示菜单那种材质效果，带圆角和阴影，所以这时不要再给内容设置背景；Windows 11 会给它加上圆角，并在窗口底色上投下阴影；Linux 两者都不画。
- `placement` 决定它出现在锚点的哪一侧，以及如何与锚点对齐（`.bottom`：在下方，居中对齐）。如果显示器上那一侧放不下，它会先翻到另一侧（Mac 菜单栏图标的下方、屏幕底部任务栏图标的上方），再平移到显示器范围之内，最后缩小以适应显示器。`gap` 让它和锚点之间保持一段距离。
- `anchor` 是一个绑定：`tray.anchor(id)` 是以这个 id 显示的托盘图标，`Anchor(x:, y:, width:, height:)` 则表示声明这个浮出面板的窗口中的一块矩形，单位是点，从窗口内容区的左上角算起（可以取自某个视图的 frame）；这种情况下，浮出面板是那个窗口的子窗口，会跟着它移动。锚点变化时，它会重新摆放。
- 它一出现就接收键盘输入，但不会把应用切到前台。当用户点击别处、切换到其他应用，或按下内容没有处理的 Escape 时，它会关闭，效果和用户主动关闭它一样（`open` 变为 false、触发 `close`，或先询问 `closeRequest`）。
- 在它上面弹出对话框：在它的内容里声明一个 `modal` 的 `Window`，这个窗口会显示在浮出面板之上，并以它为中心居中；对话框关闭之前，浮出面板会保持打开，对话框关闭后键盘输入回到浮出面板。在它上面弹出警告框时，它同样会保持打开。
- `fitContent` 让它的大小适应内容（见下文），内容变大或变小时，会重新依锚点摆放。

| | 托盘浮出面板 | 依据什么定位 |
|---|---|---|
| macOS | 一个无边框、不激活应用的 `NSPanel`，处在菜单的窗口层级，背后是菜单的材质 | 用户点击的那个状态栏项的副本（每块显示器的菜单栏上都有一份） |
| Windows | 一个无边框、置顶的工具窗口，带 Windows 11 的圆角 | 图标所在的矩形（`Shell_NotifyIconGetRect`），并考虑任务栏位于哪条边 |
| Linux | 一个没有窗口装饰、不出现在任务栏里的窗口 | 在 X11 下，依据面板报告的图标点击位置；在 Wayland 下，由于不允许应用自行摆放窗口，由桌面决定位置 |
| Web | 页面中的一个面板；锚点为托盘图标时位于页面右上角 | 它所在窗口中的一块矩形，位于页面内 |

`examples/tray` 的图标会打开一个这样的浮出面板，里面是计时器，上面还会弹出一个对话框。

## 大小适应内容的窗口

`fitContent: true`（`Window` 的键，主窗口也有）让窗口的大小适应它显示的内容，并随内容变化而调整：比如一个浮出面板里的节点列表不断变长，或者一个启动器的搜索结果时多时少。窗口的宽度取内容中最宽一行的宽度（max-content），并限制在 `minWidth` 与 `maxWidth` 之间，达到 `maxWidth` 时文字就会换行；高度取内容在这个宽度下的高度，并限制在 `minHeight` 与 `maxHeight` 之间，超出时由其中的 `Scroll` 负责滚动。Asuka 在布局每一帧时都会按这个尺寸布局内容，窗口也在同一轮里采用这个尺寸，所以窗口一出现就是正确的大小，不会先以别的尺寸出现。浮出面板会重新依锚点摆放；普通窗口或面板则保持顶边不动，向下伸展。用户无法手动调整它的大小。在 Web 上，主窗口就是标签页，它总是铺满整个页面。

## 后台应用

在 `asuka.toml` 中设置 `keepRunning = true`，应用会在最后一个窗口关闭后继续运行：主窗口只是被隐藏（绑定了的 `open` 会变为 false），再次显示它就能回来（在 Mac 上，点击程序坞图标也可以；在 Linux 上，再次启动应用也可以）。设置 `background = true` 还会让应用启动时不显示任何窗口，在 Mac 上也不显示程序坞图标，适合只住在菜单栏或托盘里、以 `Tray` 作为入口的应用。它打开的 `Window` 照常显示。这类应用通过菜单退出：托盘图标菜单里的 `quit()`，或程序坞的“退出”。

这样的应用不需要主窗口：如果 app 代码块在窗口之外什么都不显示（只有托盘图标、菜单栏，以及它的菜单打开的 `Window`），它就没有主窗口，它打开的那些窗口就是它全部的窗口（见[没有主窗口的应用](navigation.md#没有主窗口的应用)）。`examples/tray` 就是这样一个应用。

## 全局快捷键

`hotkey.Hotkey` 注册的按键，即使其他应用在前台也能被你的应用收到，有效期与它显示的时间相同（它本身不显示任何内容）：

```asuka
import platform/hotkey

component Launcher {
  state problem = ""

  Column {
    hotkey.Hotkey(Shortcut(key: "space", alt: true)) {
      description: "Show the launcher"
      on press { window.main().focus() }
      on error(e) { problem = e.message }
    }
    if !problem.isEmpty() { Text(problem) }
  }
}
```

每次按下快捷键时，`on press` 都会在主线程上执行；快捷键本身跟随它的绑定（比如用户在设置里自选的按键）：绑定变化时会重新注册。`on error(e)` 会告诉你失败的原因：`.taken`（这组按键已被其他应用占用）、`.notAllowed`（用户拒绝了，或者桌面环境不认识这个应用）、`.notAvailable`、`.noKey`。`window.main()` 是主窗口的句柄，在哪里调用都一样：`focus()` 把它切到前台，`show()`、`hide()` 等其他方法也都作用在它上面。

- **macOS**：使用 Carbon 的热键，不需要辅助功能权限。系统自带的快捷键会被视为已占用。按下快捷键不会把应用切到前台，需要的话请对某个窗口调用 `focus()`。
- **Windows**：使用 `RegisterHotKey`；按下快捷键后，应用可以把自己切到前台。
- **Linux**：在 X11 下，由应用自己抓取按键。在 Wayland 下，由桌面通过 GlobalShortcuts portal（GNOME 48、KDE Plasma 6）绑定按键：桌面可能会请用户确认或换一组按键，并且要求应用已经和它的 desktop entry 一起安装。`on press` 中显示的窗口会凭这次按键的激活令牌（activation token）获得焦点。
- 手机和 Web 上没有全局快捷键（`.notAvailable`）。

## 面板与启动器

面板是启动器、命令面板或快速输入框所用的窗口，就像 Spotlight 和 Raycast 那样：用全局快捷键从任何地方都能呼出；它浮在所有应用的窗口之上，出现在用户当前所在的空间（Space）或虚拟桌面上，也能盖在全屏应用上面；它不出现在任务栏、程序坞的窗口列表和应用切换器中；在 Mac 上，它接收键盘输入却不会把应用切到前台，因此用户原本所在的应用会保留自己的菜单栏，面板消失后键盘输入也会回到那个应用。它就是一个设置了 `panel: true` 的窗口，通常作为启动器的主窗口，搭配在后台运行的应用（`background = true`）使用：

```asuka
import platform/hotkey

state shown = false

app {
  Window {
    bind open: shown // 在快捷键显示它之前保持隐藏
    panel: true
    fitContent: true
    minWidth: 640
    maxWidth: 640
    maxHeight: 440
    on closeRequest { shown = false } // 按 Escape 或点击别处时：隐藏它
    Launcher()
  }
}

component Launcher {
  hotkey.Hotkey(Shortcut(key: "space", alt: true)) {
    description: "Show the launcher"
    on press { shown = !shown }
  }
  Search()
}

component Search {
  TextField { placeholder: "Search" }
}
```

- 它没有窗口边框。在 Mac 上，如果没有设置 `background`，它会在内容背后显示弹出框（popover）的材质效果，带圆角和阴影；Windows 11 会给它加上圆角；Linux 把它画成普通窗口的内容。
- 没有设置 `x` 和 `y`（或 `stateKey`）时，它每次显示都会出现在鼠标指针所在的显示器上，水平居中，位于从上往下三分之一处；`w.setPosition` 和 `w.center()` 则会把它固定到某个位置。配合 `fitContent` 时，随着结果出现，它会从顶边向下伸展。
- 当用户按下内容没有处理的 Escape（在文本框里按的也算），或者它失去键盘焦点（点击了别处、切换到其他应用）时，它会关闭，效果和用户主动关闭它一样。更好的做法是改为隐藏它（如上面的 `on closeRequest`），这样它下次能立刻以原样出现；何况对于保持运行的应用，主窗口关闭时本来也只是被隐藏。
- 显示它（`open` 为 true，或 `w.show()`）会让它接收键盘输入；`w.focus()` 的效果相同，两者都不会把应用切到前台。

| | 面板 |
|---|---|
| macOS | 一个带 nonactivating 样式的 `NSPanel`，出现在每个空间中并能盖在全屏应用上（`canJoinAllSpaces`、`fullScreenAuxiliary`），不参与窗口轮换，浮动显示；永远不会成为主窗口 |
| Windows | 一个无边框、置顶的工具窗口，属于一个隐藏窗口（因此没有任务栏按钮），带 Windows 11 的圆角；它会抢到前台，而全局快捷键的那次按键恰好允许它这样做 |
| Linux | 一个没有窗口装饰的窗口；在 X11 下是一个保持置顶的工具窗口，出现在每个工作区，不出现在任务栏和工作区切换器中；在 Wayland 下，如果应用通过 gtk4-layer-shell（`LD_PRELOAD`）运行，且合成器支持该协议（KDE Plasma、Sway、Hyprland、COSMIC），它是一个 overlay 层的 layer-shell surface，否则就是一个普通窗口（由 GNOME 决定位置，也不会让它保持置顶） |
| Web | 页面上方的一个面板，位于从上往下三分之一处 |

`examples/launcher` 是一个自制的 Spotlight：按 ⌥Space（Alt+Space）会弹出一个带搜索框的面板，随着输入列出已安装的应用及其图标，按 Enter 打开第一个。

## 查找应用

`platform/apps` 会像启动器那样列出并搜索已安装的应用：

```asuka
import platform/apps

component Apps {
  state installed: [apps.App] = []
  state query = ""
  state found: [apps.App] = []
  state icon = ""

  task { installed = (try? await apps.list()) ?? [] }
  task(installed, query) {
    found = (try? await apps.search(installed, query)) ?? []
    // 一个 PNG，可用于 Image。
    if let first = found.first() { icon = (try? await apps.icon(first, 32)) ?? "" }
  }

  Column {
    TextField { bind text: query }
    for a in found key a.id {
      Button(a.name) { on press { try? await apps.open(a) } }
    }
    if !icon.isEmpty() { Image(icon) }
  }
}
```

- `list()` 返回每个应用的 `name`（与系统中显示的一致，使用用户的语言）、`id` 和 `path`，按名称排序；它在主线程之外读取磁盘。每次显示面板时都重新列一遍，因为期间可能有应用被安装或卸载。
- `search` 按用户输入的内容为应用排序，忽略大小写和重音符号，优先级依次为：名称完全相同、名称以输入开头、某个单词以输入开头或首字母缩写匹配（"vsc" 匹配 Visual Studio Code）、名称中任意位置包含输入，最后是名称中按顺序包含输入的各个字母。
- `icon` 把应用图标按指定的尺寸（单位是点）绘制成 PNG 文件，存在应用的缓存目录中供下次复用，并返回它的路径。
- `open` 启动这个应用，如果它已在运行，就把它切到前台。请先隐藏面板，这样在新打开的应用接管键盘之前，键盘输入会留在用户原本所在的应用里。

| | 查找位置 | ID | 图标 | 打开方式 |
|---|---|---|---|---|
| macOS | `/Applications`、`/System/Applications`、`~/Applications` 以及它们下一级的文件夹，外加 Finder | bundle identifier | workspace 为它提供的图标 | `NSWorkspace` |
| Windows | “开始”菜单中的应用（shell 的 Applications 文件夹），包括打包应用 | AppUserModelID | shell 为它提供的图像 | shell |
| Linux | 菜单中会显示的 desktop entry（XDG 的，以及 Flatpak 和 Snap 的），依据 `OnlyShowIn`、`NoDisplay`、`TryExec` 筛选 | desktop file id | 从图标主题中取 entry 指定的图标 | GIO，并使用显示器的启动上下文 |

手机和 Web 上会抛出 `apps.AppsError.unsupported`。

## 鼠标穿透窗口

`clickThrough: true`（或 `w.setClickThrough(true)`）让鼠标穿过窗口，作用到它背后的东西上，比如另一个应用的窗口或桌面。它适合做悬浮层、HUD 或桌面宠物，通常再配上 `transparent` 和 `alwaysOnTop`。读取 `w.clickThrough` 可以跟随它的状态。开启期间，窗口不接收点击、悬停和滚轮事件，只有设置了 `takesClicks: true` 的视图上方例外，比如 HUD 上的按钮、用户可以拖动的宠物。要让用户操作整个窗口，就把它关掉。

```asuka
state hud = true

app {
  Window {
    bind open: hud
    transparent: true
    alwaysOnTop: true
    clickThrough: true
    width: 240
    height: 80
    Meters()
  }
}

component Meters {
  Row {
    gap: 8; align: .center
    Text("CPU 12%")
    View {
      takesClicks: true
      Button("Hide") { on press { hud = false } }
    }
  }
}
```

它在 Mac 上是 `ignoresMouseEvents`，在 Windows 上是带 `WS_EX_TRANSPARENT` 的分层窗口，在 Linux 上是一个输入区域（`gdk_surface_set_input_region`），其中只有接收点击的视图；在 Mac 和 Windows 上，鼠标移到这些视图上方时，窗口就会接收鼠标。在 Web 上，页面中的窗口会让点击穿透到页面上；但没有办法让点击穿出浏览器。

## 文件对话框

`platform/dialog` 显示系统自带的文件对话框，给你的是用户文件的真实路径，而不是副本：

```asuka
import platform/dialog
import std/fs
import platform/shell

async fn export(notes: string) throws {
  let md = [dialog.Filter(name: "Markdown", types: ["text/markdown"], extensions: ["md"])]
  if let path = try await dialog.save(name: "Notes.md", button: "Export", filters: md) {
    try fs.writeText(path, notes)
    try? await shell.reveal(path)
  }
}
```

有这几种对话框：`dialog.open`（用 `multiple` 可以选多个文件，`filters` 可以按 MIME 类型如 `image/*` 或按扩展名筛选）、`dialog.save`（可以给一个建议的 `name`；覆盖已有文件前对话框会先询问）、`dialog.chooseFolder` 和 `dialog.chooseFolders`。`title`、`button` 和 `start`（一个文件夹，或其中的一个文件）用来配置它们。用户取消时，既没有路径（`none` 或空列表），也没有错误。

在 Mac 上，它们以弹出面板（sheet）的形式附着在窗口上；在 Windows 上是通用项目对话框（common item dialog），对所属窗口模态；在 Linux 上是 GTK 的文件对话框，在 Flatpak 中则走桌面的 portal。在浏览器中，open 和 chooseFolder 会把选中的内容复制到应用的文件里；save 给出的文件会被写入用户选择的文件（Chrome、Edge），或者作为下载保存。在 iOS 和 Android 上，open 是系统的文档选择器（得到的是副本，和 `device.pickFiles` 一样），而 save 和 chooseFolder 会抛出 `unsupported` 错误。

测试可以自己回答这些对话框：`dialog.fake()` 安装一个 `FakeDialogs`，它的 `opens`、`saveTo` 和 `folder` 就是用户选中的内容（none 或 `[]` 表示取消），`shown` 列出被请求过的对话框（`"save Untitled.txt"`）。

## 文件管理器

`platform/shell` 能对文件做文件管理器会做的那些事：`shell.reveal(path)` 在文件所在的文件夹中选中并显示它（Finder、资源管理器，或桌面环境的文件管理器）；`shell.open(path)` 用用户为该类型设置的应用打开文件（没有这样的应用时抛出 `shell.ShellError.noApp`），对文件夹则用文件管理器打开；`shell.trash(path)` 把文件移到废纸篓或回收站，永远不会直接彻底删除（做不到时就抛出错误）；`shell.beep()` 播放系统提示音。手机和 Web 上没有文件管理器：reveal 和 trash 会抛出 `unsupported` 错误，而在 Web 上 open 会在新标签页中显示文件。如果要在应用内显示文件的内容，请使用 `platform/preview`。

## 打开文件

在 `asuka.toml` 的 `fileTypes` 中声明应用能打开的文件类型（见[发布](shipping.md)），然后用 `app` 块的 `on files(paths)` 处理它们（或者在组件体里用 `onOpenFiles`，在组件存在期间有效）：

```asuka
state opened: [string] = []

app {
  on files(paths) {
    for p in paths { opened.push(p) }
  }
  Column {
    for p in opened key p { Text(p) }
  }
}
```

一起打开的文件会在同一次调用中以绝对路径的形式送达，来源包括：在 Finder、资源管理器或“文件”应用中双击、“打开方式”、把文件拖到程序坞图标上、选择一个最近使用的文档，以及在 Windows 和 Linux 上通过命令行传入的文件。应用启动时就带着的文件，会等到第一个处理函数注册后再送达。在 iOS 上，路径指向文档的原始位置，在应用退出前都可以读取；在 Android 和 Web 上，路径指向应用在缓存目录中保存的副本。

## 单实例

在 Windows 和 Linux 上，设置了 `id` 的应用只会运行一个实例：再次启动它时，会把正在运行的那个切到前台，`app` 块的 `on secondInstance(args, dir)` 会收到这次启动的参数和工作目录，其中的文件和链接也会分别交给 `on files(paths)` 和 `on openURL(url)`：

```asuka
state newWindow = false

app {
  on secondInstance(args, dir) {
    if args.contains("--new-window") { newWindow = true }
  }
  Window { Main() }
}
component Main {
  Text(if newWindow { "A new window" } else { "Main" })
}
```

组件里的 `onSecondInstance(fn([string], string))` 效果相同，在组件存在期间有效。如果希望每次都启动一个新实例，就在 `asuka.toml` 中设置 `singleInstance = false`。Mac 总是只运行一个实例，并且只传递文件，不传递参数，所以它在 Mac 上不会触发。

## 退出

`quit()` 退出应用，效果和用户选择“退出”相同。无论退出是怎么触发的（⌘Q、程序坞、关闭最后一个窗口、`quit()`、Ctrl+C 或 SIGTERM、注销），`app` 块的 `on quit(e)` 都会先执行，调用 `e.cancel()` 可以让应用继续运行，以便询问用户是否保存未保存的更改；`e.canceled()` 说明是否已有处理函数取消了它。如果 `e.sessionEnding` 为 true，退出就无法取消：请立即保存，不要弹出对话框。

```asuka
state unsaved = false
state confirmQuit = false

app {
  on quit(e) {
    if unsaved && !e.sessionEnding {
      e.cancel()
      confirmQuit = true // 先询问，保存后再调用一次 quit()
    }
  }
  Window { Editor() }
}
component Editor {
  Text("Editor")
}
```

组件里的 `onQuit(fn(QuitEvent))` 效果相同，在组件存在期间有效。关闭单个窗口时，会先询问该窗口的守卫（见[导航](navigation.md#关闭窗口前先询问)）。

`relaunch()` 以同样的方式退出（处理函数可以取消它），并在应用退出后用同样的参数重新启动它，适用于更新之后，或者修改了某个在启动时才生效的设置之后。手机和 Web 上的应用不会自行退出，两者都会被忽略。

## 自动更新

`platform/update` 的 `update.UpdateWindow()` 在应用主窗口中放一次，就得到一个 Sparkle 式的更新器：它会在启动后不久以及之后每天检查一次新版本；有新版本时显示更新说明，并提供“安装更新”“跳过此版本”和“稍后提醒”三个选项；下载时显示进度条（如果有针对当前版本的增量包，就只下载变化的部分）；校验新版本是否用应用的密钥签名；安装完成后提供“立即重新启动”。把 `update.AppMenu()` 放进 `MenuBar`，会在 Mac 上的“关于”之后加入“检查更新…”菜单项（在其他平台上则放在“帮助”菜单中）；`update.CheckForUpdates()` 是单独的这一个菜单项。如果想自己做更新界面，可以使用 `update.check()`、`Release.install(progress:)`、`update.relaunch()` 和更新器的状态（`update.updater`）。

```asuka
import platform/update

app {
  Window {
    title: "Notes"
    update.UpdateWindow()
    MenuBar { update.AppMenu() }
    Text("Notes")
  }
}
```

`asuka update keygen` 生成密钥对；`asuka.toml` 中的 `updates` 指定发布的位置（GitHub Releases、S3 存储桶、任意 HTTPS 目录，或者你自己的服务器）；`asuka publish --platform mac`（或 `windows`、`linux`）负责构建、签名和上传（见[发布](shipping.md#自动更新)）。下载更新和把它放到位由应用自己完成：Mac 应用的新 bundle 会等应用退出后再替换；Windows 和 Linux 则在应用运行时就替换它的文件。通过系统软件包、Flatpak、Snap 或应用商店安装的应用由它们负责更新，`await update.unsupported()` 会反映这一点；手机上返回 `.notAvailable`。在 Web 上，如果页面比网站上的版本旧，会提示用户重新加载。

## 最近使用的文档

用户每打开或保存一个文档，就调用一次 `recent.add(path)`（`platform/recent`）。Mac 会把它们列在“文件”菜单的“最近使用”和程序坞菜单中，Windows 列在跳转列表中，Linux 则记入用户的最近文件。用户选中其中一个时，会通过 `on files(paths)` 打开它。`await recent.list()` 按从新到旧的顺序返回它们，并跳过已经不存在的文件；`recent.clear()` 清除本应用自己添加的记录。

## 基于文档的应用

`platform/document` 用来编写每个窗口各显示一个文件的应用，就像文本编辑器那样：它提供文档的模型，以及围绕文档的窗口和页面。描述好文档的类型和它的编辑器：

```asuka
import platform/document

let types = [document.Type(name: "Plain Text", extensions: ["txt"])]

app {
  document.Documents(types) { editor: doc => Editor(doc) }
}

component Editor(doc: document.Document<string>) {
  if let text = doc.content {
    TextField {
      text: text
      multiline: true
      grow: 1
      on change(t) { doc.edit(t) }
    }
  }
}
```

在 `asuka.toml` 的 `fileTypes` 中列出同样的类型，系统才会把你的应用作为这些文件的打开方式之一。`examples/textedit` 就是这样一个编辑器，另外加了一行状态栏，以及一个在文件被其他应用修改时出现的提示条。

`Document` 是一个 store：它的 `content`（文件读出之前为 none）和 `state`（`.loading`、`.saved`、`.edited`、`.saving`、`.conflict`、`.failed`，失败原因在 `error` 里）。`doc.edit(c)` 修改内容，最后一次修改片刻之后自动保存；`save()` 立即保存，`saveSoon()` 供不能等待的代码使用（按键、菜单项）。如果文件在读出之后被其他程序修改过，就会产生冲突：`saveAnyway()` 保留应用的内容，`revert()` 采用文件的内容。它的 `file` 是文档所在的文件（未命名文档为 ""），`name` 是它的名字；`saveAs(path)`、`rename(name)` 和 `moveTo(dir)` 改变它的位置，`unsaved()` 表示它是否有文件里还没有的更改，`discard()` 放弃这些更改。`document.open(path, read:, write:)` 可以为任意类型的内容创建文档，由你自己的 read 和 write 函数在字节和内容之间转换；`Documents` 编辑的是文本。

用户会得到这些：

- **在桌面上，每个文档一个窗口**：窗口以文件命名；在 Mac 上标题栏显示文件图标，有未保存的更改时关闭按钮里显示一个圆点。应用的主窗口（`Documents` 就是它的内容）在启动时隐藏。在 Windows 和 Linux 上，最后一个窗口关闭时应用随之退出，和记事本一样；Mac 上的应用继续运行，菜单栏里的新建和打开…仍可使用。
- **“文件”菜单**：Mac 上与“文本编辑”相同：新建（⌘N）打开一个未命名文档，打开…（⌘O）显示系统的对话框，最近使用显示系统记录的最近文档，然后是关闭（⌘W）、存储（⌘S）、复制（⇧⌘S）、存储为…（⌥⇧⌘S）、重新命名…、移到…和复原到上次存储的版本。Windows 和 Linux 上与记事本和 GNOME 文本编辑器相同：新建、打开…、最近使用（其中是最近的文档和“清除菜单”）、保存、另存为…（Ctrl+Shift+S）、还原、关闭和退出。每一项都作用于当前正在使用的窗口中的文档。这些菜单项会跟随应用的语言（英语、德语、西班牙语、法语、日语和中文；其他语言显示英语）。
- **自动保存**：用户停止输入片刻后，文档会自动保存，从不询问。无法原地保存的文档（未命名的、被其他应用修改过的、文件不可写的）会把更改保存在 `document.recoveryDir()` 中的恢复副本里，退出应用时也会保留在那里：下次启动时，这些文档会以“已编辑”的状态重新打开。
- **在更改可能丢失时询问**：关闭有更改的未命名文档时，会询问是否保留它（存储…、删除、取消）；关闭保存失败或遇到其他应用修改的文档时，会询问是否保存（存储、不存储、取消）；保存时如果文件已被其他应用修改，会询问是“仍然存储”还是“复原”；复原到上次存储的版本之前也会先询问。移到…在要替换同名文件之前会询问。
- **在手机和浏览器上**，同一时间只有一个文档：应用打开后先显示它的文档列表，带有新建；打开某个文档时，文档页面会推入到列表之上，并在用户编辑时保存。在列表的某一行上滑动，可以复制、重新命名（会询问新名称）或删除（会先询问）该文档。

## 登录时启动

把 `try await login.set(true)`（`platform/login`）作为一个设置项提供给用户，并用 `await login.enabled()` 读回当前状态。在 macOS 13 及以上，它是一个登录项（仅限打包好的应用；在用户于“系统设置”中允许之前，会抛出 `LoginError.needsApproval`）；在 Windows 上是一个 Run 注册表项；在 Linux 上是一个 autostart 条目（在 Flatpak 中通过 Background portal）。`await login.openedAtLogin()` 可以区分这次启动是否是登录时的自动启动，这样应用就可以在后台悄悄启动。

## “正在播放”与媒体键

`nowplaying.NowPlaying` 在显示期间，把应用正在播放的内容放进系统的媒体控件中，这些控件上的按钮以及键盘上的媒体键（播放/暂停、下一首、上一首）都会交给应用的处理函数：在 Mac 上是控制中心和菜单栏里的“正在播放”；在 Windows 上是音量浮出面板和快速设置中的媒体控件（通过应用窗口的 SystemMediaTransportControls）；在 Linux 上是一个 MPRIS 播放器，可以由 GNOME 和 KDE 的媒体控件以及 `playerctl` 控制。它和手机上用的是同一个包，在手机上对应锁屏界面的媒体控件（见[设备与服务](device.md#正在播放与媒体键platformnowplaying)）：

```asuka
import platform/audio
import platform/nowplaying

component Spring {
  event next

  let music = audio.Player("music/spring.mp3")

  nowplaying.NowPlaying {
    title: "Spring"
    artist: "Vivaldi"
    // 从它获取播放状态和进度；播放、暂停、跳转都作用在它上面。
    player: music
    on next { emit next }
  }
}
```

## 显示器、电源与应用图标

`screen.displays()` 跟踪所有显示器（它是被跟踪的：在绑定里读取时，就会在被读取期间一直监视显示器）：每块显示器的位置、工作区、缩放比例、刷新率，以及哪块是主显示器；`screen.nearest` 和 `screen.matching` 用来挑出某个点或某个窗口所在的那块显示器。Wayland 没有工作区的概念，在那里工作区就是显示器的整个范围。

`power.status()` 跟踪电池状态、低电量模式和散热状态。`power.suspends()`、`resumes()`、`locks()` 和 `unlocks()` 是电脑睡眠、唤醒以及屏幕锁定、解锁的事件流，`power.idleTime()` 则给出用户已经离开了多久。手机上有锁定和解锁，但没有睡眠和空闲时间；Web 上只有电池状态。

```asuka
import platform/power
import platform/screen

component Status {
  let p = power.status()
  let displays = screen.displays()

  task {
    for await _ in power.suspends() { print("going to sleep") }
  }

  Column {
    Text(if p.battery { "Battery {(p.level * 100).fixed(0)}%" } else { "No battery" })
    Text(displays.map(d => "{d.name} {d.bounds.width}x{d.bounds.height}@{d.scale}x").joined(separator: "\n"))
  }
}
```

`dock.setProgress` 在程序坞图标、任务栏按钮，或支持 Unity launcher API 的 Linux dock 上显示任务进度（GNOME 自带的 dash 不显示）。`dock.requestAttention` 会在应用处于后台时让图标跳动或闪烁。图标上的数字角标则由 `notify.setBadge` 设置。

`appearance.current()` 提供系统的强调色（一个 `Color?`）、“增强对比度”和“降低透明度”设置（会被跟踪），供应用自己绘制的内容使用；Asuka 显示的视图本身已经会跟随这些设置。
