# 系统功能

组件作为应用的一部分所用到的功能都来自 prelude，不需要导入：它存在期间注册的东西、它的环境、警告框，以及应用的窗口。

| 函数 | 作用 |
|---|---|
| `useKeys([Shortcut(…)])` | 组件存在期间，窗口的快捷键 |
| `onOpenURL(u => …)`、`onOpenFiles(paths => …)`、`onShare(items => …)` | 组件存在期间，应用被打开时带来的内容，或其他应用分享给它的内容 |
| `quit()`、`relaunch()` | 像用户选择“退出”那样退出应用（并重新启动它），会先询问应用；仅桌面端 |
| `onCloseRequest(fn() -> bool)`、`onQuit(e => …)`、`onSecondInstance((args, dir) => …)` | 组件存在期间：组件所在窗口关闭前的询问；应用即将退出（`e.cancel()`）；应用再次被启动 |
| `onBack { … }` | 用户从组件所在的页面、弹出面板或气泡弹窗返回时询问：`true` 表示接管（页面留下），`false` 表示放行 |
| `onLifecycle(l => …)` | 组件存在期间：接收应用进入前台、后台，以及内存警告（一个 `Lifecycle`） |
| `alert(title, message:, actions:)` | 立即显示一个警告框；`actions` 是一组 `.button`（没有时只有一个 OK）。`Alert` 元素则在它的 `open` 为 true 时显示一个 |
| `snackbar(message, action:, duration:, onDismiss:)` | 在窗口底部显示一条简短消息，可以带一个操作（撤销）；返回一个用来关闭它的闭包 |
| `environment()`、`screenEnvironment()` | 尺寸、安全区域、文字大小、平台等（会被跟踪）：页面或弹出面板自己的，以及整个屏幕的 |
| `window.main()`、`window.current()` | 主窗口的 `Ref`，以及代码所在窗口的 `Ref`：在任何地方都可以 `show()`、`hide()`、`focus()` …… |
| `window.close()` | 不询问，直接关闭代码所在的窗口 |

应用向系统服务请求的功能，在[平台 SDK](../stdlib.md) 的包里，和其他包一样导入后使用：

| 函数 | 作用 |
|---|---|
| `device.share(text)` | 为一段文本或一个链接弹出分享面板 |
| `try await device.shareFiles(paths, title:, from: ref)` | 为文件弹出分享面板，从某个视图弹出（弹出框指向的位置）；面板关闭后返回 |
| `device.haptic(.success)` | 平台的触感反馈（一个 `device.Haptic`） |
| `device.copy(text)` | 把文本放到剪贴板 |
| `device.openURL(url)` | 用能处理 url 的应用打开它 |
| `accessibility.announce(text)` | 让屏幕阅读器读出文本 |
| `accessibility.focus(ref)` | 把屏幕阅读器的焦点移到该视图 |
| `try await webview.cookies(url)` | Web 视图为 url 保存的 cookie |
| `appearance.setColorScheme(.dark)` | 应用的外观：`.light`、`.dark`，或 `.system`（跟随平台） |

```asuka check
import platform/device

component ShareLink(link: string) {
  Button("Share") {
    on press {
      device.haptic(.light)
      device.share(link)
    }
  }
}
```

应用在各个平台上包含哪些代码，由 `@platform` 决定，它是编译时就已确定的常量（见[特定平台的代码](control-flow.md#特定平台的代码)）；`environment().platform` 则是视图实际运行的平台，和环境里的其他值一样会被追踪。

`useKeys` 和这些 `on…` 注册写在组件的主体里（或 app 代码块里）。app 代码块还可以把它们当作事件来处理：`on openURL(url) { … }`、`on files(paths) { … }`、`on share(items) { … }`、`on quit(e) { … }`、`on secondInstance(args, dir) { … }`、`on foreground { … }`、`on background { … }`。它的第一个 `Window` 是主窗口；如果 app 代码块在它的 `Window` 之外什么都不显示（只有托盘图标、菜单栏），它就没有主窗口（见[导航](../navigation.md#窗口)）。

`std/embed` 在应用编译时读取包里的文件，把它们编进应用，所以每个平台都直接拥有这些文件，运行时不需要加载：

```asuka
import std/embed

struct Config { api: string; retries: int }

let welcome = embed.text("welcome.md")         // string
let logo = embed.bytes("art/logo.png")         // bytes
let config: Config = embed.json("config.json") // 编译时检查
let posts = embed.texts("posts/*.md")          // [路径: 文本]
let icons = embed.files("icons/**/*.svg")      // [路径: 字节]
```

路径是字符串字面量，相对于包的目录（`asuka.toml` 所在的目录），且必须在这个目录之内；通配符支持 `*`、`?`、`[abc]` 和 `**`（任意层目录），不会匹配名字以点开头的文件。`embed.json` 和 `json.decode` 的读法一样，只是发生在编译时：文件和类型对不上就是编译错误，并指出行和列，而不会等到运行时才出错。文件不存在、通配符一个文件也没匹配到，同样是编译错误。用 `asuka dev` 时，修改被嵌入的文件会让应用就地重新加载，就像修改代码一样。运行时由平台读取的文件（图片、字体）应放在 `assets/` 里（见 [std/embed](../std/embed.md)）。
