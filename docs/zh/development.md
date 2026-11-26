# 开发

`asuka dev` 会运行你的应用，并在你保存时重新加载。它可以运行在 iOS 模拟器或 iPhone、Android 模拟器或手机、你的 Mac、PC 或 Linux 电脑上，也可以运行在浏览器里。本页介绍它运行的开发 App、重载时会保留什么，以及设备、资源和日志、编辑器和你可能遇到的问题。如果想在没有设备或显示器的情况下测试应用界面，请看[测试](testing.md)。

## asuka dev

```sh
asuka dev [flags] [dir]                     # 当前电脑：macOS、Windows 或 Linux
asuka dev --platform ios [flags] [dir]       # iOS 模拟器
asuka dev --platform ios --device iphone [dir]  # 连接着的 iPhone（或 iPad）
asuka dev --platform android [flags] [dir]   # Android 模拟器或手机
asuka dev --platform web [flags] [dir]       # 浏览器
```

`--platform` 可以是 `ios`、`android`、`mac`、`windows`、`linux` 和 `web` 之一；`asuka build` 也接受同样的参数。不指定时，应用会为你当前使用的电脑运行（或构建）。每次只能运行一个平台：要同时跑两个平台，就开两个终端。它运行的是开发构建：[`@debug`](language/control-flow.md#开发构建的代码) 为 true。

`dir` 是应用所在的目录，默认是当前目录，里面有应用的 `.asuka` 文件（其中一个包含 `app` 块）和它的 `asuka.toml`。可用的参数有：

- `--platform`：见上文。
- `--lang`：本次运行使用某种语言（如 `de`、`zh-Hans`），或者使用伪区域设置：`en-XA`（带重音符号且更长）或 `ar-XB`（从右到左）。`--dir rtl` 可以让任何应用按从右到左布局。参见 [多语言](i18n.md)。
- `--once`：只启动一次应用，不监听文件变化。
- `-v`：打印 Asuka 执行的命令。

在手机上还有：

- `--device`：在 iOS 上可以是模拟器的名称或 UDID（默认使用已启动的 iPhone 模拟器，没有的话就选一个较新的 iPhone Pro 并启动它），也可以是已连接的 iPhone 或 iPad 的名称或 UDID，或者写 `iphone`，表示当前连接着的那一台（参见 [设备](#设备)）；在 Android 上是 adb 序列号或 AVD 名称（默认使用 adb 列出的第一台设备，没有的话就启动第一个 AVD）。
- `--team`：在 iPhone 上运行时，为开发 App 签名的 Apple 开发者团队。

在浏览器中还有 `--port`、`--lan`（向局域网提供服务，供手机浏览器访问）和 `--no-open`，详见 [Web](web.md#asuka-dev---platform-web)。

## 开发 App

开发时，应用运行在一个**开发 App** 里。它是应用的外壳（名称、ID、图标、`Info.plist` 或清单文件、原生包的代码），`asuka dev` 会为平台构建一次，然后安装并启动它。之后 `asuka dev` 把你的应用发送给它；每次保存都会重新编译（只需几毫秒）并发送新版本，开发 App 原地加载它，保留应用的状态，并报告重载的结果：

```
asuka: compiled in 3.1 ms
asuka: reloaded in 2.2 ms, 4 states kept
```

如果保存的代码无法编译，会以 `file:line:col: message` 的形式打印错误，并把它们显示在应用上方（见[应用上方的错误](#应用上方的错误)）；开发 App 继续运行原来的版本，下一次能够编译的保存会从那里接着来。应用其余部分的改动（`asuka.toml`、资源、原生包的 Swift、Kotlin 或 JavaScript 代码）会让开发 App 重新构建，有变化时重新安装，然后再次启动。

## 在桌面端

在 macOS、Windows 和 Linux 上，`asuka dev` 在一个窗口中运行开发 App。第一次运行时会为这台电脑构建开发 App，需要稍等片刻；之后就能立即启动。构建需要一个 C 编译器：Mac 上是 Xcode 的命令行工具；Windows 和 Linux 上是 Zig、clang 或 gcc（可以用 `ASUKA_CC` 指定）。

应用直接从原位置读取 `assets/`，所以修改资源后不需要复制。应用的数据（SQLite 数据库、文件）存放在 Asuka 缓存中该应用自己的 `data` 目录里（参见 [从头开始](#常见问题)），而不是已安装应用的数据目录，所以 `asuka dev` 和发布版本的数据互不共享。应用的输出和错误会打印到终端。

**macOS。** 开发 App 是 Asuka 缓存中的一个 `.app`，和发布版本的一样：有 `Info.plist`（其中的名称、bundle ID、URL scheme、权限用途说明和语言都来自 `asuka.toml`）和图标，并使用临时签名（ad hoc）。因此在系统看来，它就是这个应用本身，和从访达打开时一样：程序坞（Dock）显示的是它的名称和图标，通知归它自己所有，隐私权限提示（相机、定位、通讯录）也是以它的名义、带着它的用途说明弹出的，而不是以终端的名义。需要描述文件（provisioning profile）授权的包（推送、通过 Apple 登录、App Attest）只能在用描述文件签名的发布构建中使用，参见 [发布](shipping.md#macos)。

**Windows。** `asuka dev` 只能在 Windows 本机上运行；`asuka build --platform windows` 则可以在任何系统上构建（借助 Zig），生成一个包含应用的 `.exe`。开发 App 是一个控制台程序，所以输出会打印到终端；发布版本没有控制台。

**Linux。** 在 Linux 上，`asuka dev` 基于 GTK 4 和 libadwaita 运行开发 App，因此需要先安装它们（参见 [快速上手](getting-started.md#准备工作)），它运行在你会话的显示服务器上，Wayland 或 X11 都可以。在 Mac 或 PC 上，`asuka dev --platform linux` 只有在 `ASUKA_LINUX_RUN` 指定了一条命令时才能运行：这条命令要能在一台带显示器、并且能以相同路径访问本机文件的 Linux 机器上运行程序。这通常是一台虚拟机，例如 `ASUKA_LINUX_RUN="orb -m ubuntu"`（OrbStack）或 `ASUKA_LINUX_RUN="limactl shell default"`（Lima）。Asuka 会针对那台机器的架构构建开发 App，这需要 Zig；开发 App 通过网络连接 `asuka dev`：地址是这台电脑的第一个地址，或者 `ASUKA_DEV_HOST` 指定的地址。

## 重载时会保留什么

在每个平台上，重载都会保留应用中所有的 `state`，你不需要为此写任何代码。每个状态单元按它所在组件在视图树中的位置和它的名称保存（例如 `Inbox.query`，位于显示它的那个页面和那一行之下），新模块中位置相同、名称相同的状态单元就从这个值开始。值会按字段名和枚举成员名转换成新模块中的类型：新增的字段取默认值，删掉的字段被丢弃。如果状态的类型变了，而且无法转换（比如原来是 `int`，现在是 `string`），它会从初始值重新开始。视图会由新模块重新创建，显示它们原来的状态：屏幕看起来和之前一样，只是多了你的改动。

以下内容不会保留：

- 你修改了初始值的状态会从新的初始值开始（比如把 `state count = 0` 改成 `state count = 10`），其他状态仍然保留各自的值。只调整初始值的写法（空格、换行）不算修改。
- 被重命名的状态，或者被移到另一个组件里的状态，都会从头开始；所在组件被移到视图树中其他位置的状态也是如此。
- `let` 的值不会保留：它们会根据所读取的内容重新计算。
- task 和 effect 会在新模块中重新开始（保存时正在运行的 `task` 会被取消，然后运行新的那个）。

想让应用从头开始，在运行 `asuka dev` 的终端里按 **r**：每个开发 App 都会重新加载应用，不保留任何状态，所有 `state` 都从初始值开始，就像刚启动时一样，但开发 App 本身不会重新启动（`asuka: restarted in 2.0 ms`）。停止 `asuka dev` 再重新运行，应用同样会从零开始。无论哪种方式，应用写入的文件（SQLite 数据库、文件）都会保留下来：它们位于应用的数据目录中，在设备上，或者在桌面端的 Asuka 缓存中。

## 应用上方的错误

开发过程中出的错不只打印在终端里，还会直接显示在设备或窗口上，盖在应用上方：

- **没有被捕获的错误**：handler、effect 或 task 抛出、外面没有 `Boundary` 包着的错误，或者应用的 `init` 抛出的错误。覆盖层会显示错误消息、源码中抛出它的那几行，以及一路调用到那里的调用栈（最内层在最前）。
- **无法编译的保存**：显示第一个错误、出错位置附近的源码（用 `^` 标出有问题的部分）、该错误的说明页，以及其余的错误。只要有一次保存能够编译，它就会自动消失；它会优先于应用自己的错误显示，因为后者来自你正在改的那部分代码。

点 **Dismiss**（或按 Escape、Android 的返回键）会回到应用原来的样子：抛出错误的 handler 或 effect 停在了那里，其他一切照常运行。对于应用自己的错误，还会提供 **Reload**，效果和按 **r** 一样，让应用从头开始，所有 `state` 回到初始值。如果在显示一个错误时又出现了更多错误，覆盖层仍然显示第一个，并注明后面还有多少个：其余的通常都是由它引起的。一次能够编译的保存会清掉旧代码产生的那些应用错误。

覆盖层用各平台自己的视图绘制，盖在应用的页面、sheet 和 alert 之上，所以即使出问题的正是应用自己的视图，它也照样能显示出来。它只在 `asuka dev` 中出现：在构建出来的应用里，没有被捕获的错误只会记录到日志，要向用户展示错误，请用 `Boundary`。

## 重载

Asuka 会监听应用所在的目录及其本地依赖（`asuka.toml` 中的 `file:`）的目录：

- `.asuka` 文件：编译后原地重载；
- `asuka.toml` 和 `asuka.lock`、`assets/` 下的文件、语言目录（catalog）、原生包的 Swift、Kotlin 和 JavaScript 代码、`Info.plist` 和授权文件（entitlements）：重新构建并启动开发 App（应用从零开始）。

以 `.` 开头的目录以及 `testdata`、`build`、`dist` 和 `node_modules` 会被跳过。

## 设备

**你的电脑**：在 Mac、PC 或 Linux 上直接运行 `asuka dev` 即可。

**iOS 模拟器**不需要额外设置：它和 Mac 共享网络。

**Android 模拟器或手机**：`asuka dev` 通过 adb 连接模拟器或通过 USB 连接的手机。对于手机，在开发者选项中打开 USB 调试，连上电脑并同意弹出的授权提示，然后确认 `adb devices` 能列出它。`asuka dev --platform android` 使用 adb 列出的第一台设备，不论是手机还是模拟器；连接了多台设备时，用 `--device` 加序列号指定。

**iPhone**（或 iPad），需要 Xcode 26 或更新版本：把它连上 Mac，解锁后点“信任”；再在它的“设置 › 隐私与安全性”中打开“开发者模式”（手机连接过 Xcode 之后才会出现这个选项，打开后手机会重启）。之后运行 `asuka dev --platform ios --device iphone` 就会在它上面运行；连着多台设备时，用 `--device` 加上设备名称或 UDID 指定（`xcrun devicectl list devices` 可以列出它们）。

- **签名**：开发 App 由你的团队用开发证书签名。团队依次取自 `--team`、`ASUKA_TEAM_ID`、`asuka.toml` 的 `ios.team`，都没有的话就用钥匙串里 Apple Development 证书所属的团队。第一次运行时，Asuka 会让 Xcode 生成所需的开发描述文件，顺带把这台手机注册到团队，并打开应用用到的功能（推送、App Groups 等）。这一步使用 Xcode “设置 › 账户”中登录的 Apple ID；如果设置了 `ASUKA_ASC_KEY_ID`（或 `ios.appStoreConnect`），则改用 [App Store Connect API 密钥](shipping.md)。团队已有的描述文件如果合适（包括通配符描述文件），就直接使用。
- **连接到 Mac**：插着数据线时，Mac 会像 Xcode 一样经数据线连接开发 App，手机不需要连接任何网络。没有数据线时，开发 App 通过网络连接 `asuka dev`，依次尝试 Mac 的各个地址（或 `ASUKA_DEV_HOST` 指定的地址），端口在空闲时为 8791；iOS 会询问一次是否允许这个应用查找本地网络中的设备，请选择允许，之后也可以在手机的“设置 › App › 本地网络”中打开。无论哪种方式，`asuka dev` 重新启动后开发 App 都会自己重新连上，从主屏幕再次打开的开发 App 也能找到它。如果开发 App 一直没有连上，Asuka 会告诉你该检查什么。

**手机浏览器**：`asuka dev --platform web --lan` 会向局域网提供应用，并打印可以打开的地址，第一个还附有二维码，用手机相机扫一下即可。

## 资源

在桌面端，开发 App 直接从原位置读取 `assets/`。在手机上，资源有变化时会被复制进开发 App。在浏览器中，则由开发服务器提供这些资源。修改资源会让开发 App 重新构建并启动。

## 日志

开发 App 会把它的输出和错误发送给 `asuka dev`，由后者打印到终端：在每个平台上都包括 `print`、未捕获的错误（附带一路调用到那里的调用栈，应用上也会显示，见[应用上方的错误](#应用上方的错误)），以及 Asuka 自己的警告和错误。开发 App 无法加载某个模块时，会说明原因（`asuka: the player: ...`）。在浏览器中，页面的错误也会打印到终端。

## 检查器

`asuka dev` 会打印它的检查器的地址：

```
asuka: the inspector (the app's views): http://127.0.0.1:52817/
```

在同一台电脑的浏览器中打开它（或在运行 `asuka dev` 的终端里按 **i**），就能以树的形式查看正在运行的应用的视图：每个视图的元素、文字，以及它在父视图中的 frame。把指针移到一个视图上或点击它，应用会在设备上把它高亮出来；点击还会显示它的详情，按 Escape 则取消选择。可以折叠不需要的部分，也可以按元素或文字筛选。这棵树每秒跟随应用更新一次（关掉 Live 可让它保持不动）。同时连接了多台设备时（例如一台 iPhone 和一个模拟器），可以在顶部选择其中一台。表单（sheet）、弹出框和其他窗口各自是一棵树，排在应用的树之后；列表的单元格则是列表的子视图。

## 错误及其代码

每个错误和警告都有一个代码，也就是它所报告的那条规则的名称。终端会把代码连同帮助（如果有的话）和代码的页面一起显示：

```
app.asuka:14:9: error[write-in-binding]: can't assign to count here
  = help: what the UI follows is computed, not changed; change state in a handler (on press { … }), a fn, a task or an effect
  = see https://asuka.egoist.dev/docs/errors/write-in-binding
```

`asuka explain write-in-binding` 会在终端里用你的语言（`LANG`）显示这个页面，内容与所装的版本一致：这条规则为什么存在、违反时如何修正，以及一个违反它的程序和一个不违反它的程序。[错误代码](errors/README.md)列出了全部代码。

`asuka check --json` 和 `asuka test --json` 则改为每行输出一个 JSON 对象，供工具和编程智能体使用：`code`、`severity`、`file`、`range`（行号和列号从 1 开始）、`message`、`help` 和 `url`；只在部分平台上出现的诊断还有 `platforms`。编辑器也会显示代码，并链接到它的页面。

在 `asuka dev`、`asuka run` 或 `asuka test` 下运行时，核心还会报告它在状态中发现的问题，同样带有代码：不断互相触发的 effect（`effect-loop`，附带 effect 所在的行和它最后写入的状态）、等待了另一个它并不使用的 load 的 load（`load-waterfall`）、显示上一次结果已达两秒却没有任何地方读取 `loading` 的 load（`long-hold`）、两次让 1,000 多个绑定重新运行的写入（`wide-fanout`），以及让绑定在同一次更新里再运行一次的 effect 写入（`effect-in-effect-pass`）。代码里的每个位置只报告一次，随开发 App 的日志一起输出。发布构建不做这些检查；崩溃报告会带上 panic 的代码。

## 编辑器

`asuka lsp` 是 Asuka 的语言服务器，为 `.asuka` 文件提供：输入时的诊断、补全、带 `///` 文档的悬停提示、跳转到定义、查找引用、重命名、格式化（与 `asuka fmt` 相同）、符号、签名帮助和语义高亮。它会带着你尚未保存的修改检查正在编辑的包，以及导入它的包。

VS Code 的扩展在 `editors/vscode` 中：它为这门语言加上语法高亮，并运行 `asuka lsp`（使用 `PATH` 中的 `asuka`，或 `asuka.server.path` 指定的那个）。

```sh
cd editors/vscode
npm install
npx @vscode/vsce package
code --install-extension asuka-0.1.0.vsix
```

Zed 的扩展在 `editors/zed` 中：借助 Asuka 的 tree-sitter 语法（`editors/tree-sitter-asuka`）提供语法高亮、大纲、括号匹配、缩进和文本对象，在每个 `test`、`bench` 和 `app` 旁边显示运行按钮，并运行 `asuka lsp`。它使用 `PATH` 中的 `asuka`，或 Zed 设置里 `lsp.asuka.binary.path` 指定的那个；两者都没有时，会下载 Asuka 最新 release 的命令行工具。安装方法：在命令面板中执行 **zed: install dev extension**，选择 `editors/zed` 目录；Zed 会用 Rust 构建它，Rust 需通过 [rustup](https://rustup.rs) 安装。

```json
{
  "lsp": {
    "asuka": {
      "binary": { "path": "/path/to/asuka", "arguments": ["lsp", "--verbose"] }
    }
  }
}
```

任何带 LSP 客户端的编辑器也都能使用它：运行 `asuka lsp`，它通过标准输入和输出使用 LSP 3.17，处理 `.asuka` 文件，以 `asuka.toml` 所在的目录为根。`editors/vscode` 的 README 里有 Neovim 和 Helix 的设置，它们的语法高亮可以使用 `editors/tree-sitter-asuka` 这份 tree-sitter 语法。

## 快照与脚本

桌面应用支持几个环境变量，可以让应用显示某个状态并截取下来，方便你肉眼检查改动或在 CI 中使用。用这些变量运行发布构建的可执行文件，或者运行 `asuka dev`（它会把自己的环境变量传给开发 App）：

```sh
ASUKA_SNAPSHOT=shot.png ./MyApp.app/Contents/MacOS/MyApp            # 窗口显示后把它保存为 PNG，然后退出
ASUKA_SNAPSHOT=shot.png ASUKA_DUMP=1 asuka dev                      # 同时把视图树写入 shot.png.txt
ASUKA_SCRIPT="tap:Add;tap:Add" ASUKA_SNAPSHOT=shot.png asuka dev    # 先执行这些步骤
```

- `ASUKA_SNAPSHOT` 会在窗口显示后（以及脚本执行完后）把窗口保存为 PNG，然后退出；设置 `ASUKA_SNAPSHOT_STAY=1` 可以让应用继续运行。在 macOS 上，截图取自窗口服务器合成的窗口图像（连同其上的弹出面板），材质和 Liquid Glass 都和屏幕上一样；文件名为 `*.content.png` 时只保存内容，不含标题栏。
- `ASUKA_DUMP` 会在截图旁边写出视图树：每个视图的类型、frame（位置和尺寸）以及显示的内容。
- `ASUKA_SCRIPT` 会在截图前执行一系列以 `;` 分隔的步骤：`tap:<text>`（点按显示该文字的视图：按钮标题、文本、标签页、侧边栏的行、分段控件的段）、`type:<text>`（在获得焦点的输入框中输入）、`back`、`key:<shortcut>`（如 `ctrl+shift+k`）、`menu:<item>`（菜单栏中的菜单项）、`escape`、`close`（关闭当前窗口）、`wait:<duration>`（如 `500ms`）、`waitfor:<file>`（等到该文件出现：由测试决定何时继续）和 `snap:<file>`。

## 构建缓存

构建时编译和下载的内容都保存在 Asuka 的缓存中，由这台电脑上的所有应用共享：构建第二个应用，或者小改动之后再构建时，只会编译新的部分。某个平台的第一次构建会下载 Asuka 为该平台预先构建好的库，只下载一次，来源是命令行工具对应的 GitHub release，下载后会用命令行工具自带的校验和核对（你这台电脑自己的那份，安装脚本和 `asuka upgrade` 会提前下载好）；代理设置（`HTTPS_PROXY`、`NO_PROXY`）同样生效。要使用镜像，把 `ASUKA_PREBUILT_URL` 设为存放 release 中 `asuka-prebuilt-*.tar.gz` 文件的位置：可以是一个 URL，其中的 `{version}` 会替换成命令行工具的版本（如果设置了 `ASUKA_PREBUILT_TOKEN`，会作为 bearer token 发送给它）；在不联网的电脑上，也可以是你把这些文件复制进去的目录。其中的 Swift 库只能配合构建它们的 Xcode 或更新版本使用；如果你的 Xcode 更旧，它们会改为在你的电脑上编译一次。

- `asuka cache stats` 显示缓存的位置、大小，以及构建时在其中命中所需内容的次数。
- `asuka cache clean` 清空缓存；之后的构建会重新编译所有内容。
- 缓存会保持在 10 GB 以内，优先删除最久没用到的内容。用 `ASUKA_CACHE_MAX` 设置别的上限（`ASUKA_CACHE_MAX=20G`、`500M`；`0` 表示不设上限），用 `ASUKA_CACHE` 把它放到别的位置。

## 常见问题

- **`cbuild: no C compiler`**：安装系统的 C 编译器（Xcode 命令行工具、`build-essential`、LLVM），或者安装 Zig 并设置 `ASUKA_CC=zig`。`building for the web needs Zig` 和 `cross-compiling for ... needs Zig`：安装 [Zig](https://ziglang.org/download)，它可以为 Web 和其他系统构建。
- **`no iOS simulator SDK (install Xcode)`**：安装 Xcode，打开一次，然后在它的 Components 设置中添加 iOS 模拟器运行时。`no iPhone simulator; add one in Xcode` 和 `no simulator "..."` 表示没有该名称的模拟器：可以用 `xcrun simctl list devices` 列出所有模拟器。
- **iPhone 没有准备好**：`... isn't paired with this Mac`（连上数据线，解锁后点“信任”）、`... isn't reachable`（连上数据线，或在 Mac 所在的 Wi-Fi 下唤醒它）、`... has Developer Mode off`（“设置 › 隐私与安全性 › 开发者模式”）、`... is locked`（解锁手机）。明明连着手机却提示 `no iPhone or iPad is connected` 时，检查 `xcrun devicectl list devices` 能否列出它。
- **`Xcode couldn't make the development profiles`**：在 Xcode 中登录团队的 Apple ID（Xcode › 设置 › 账户），或者设置 App Store Connect API 密钥；如果 bundle ID 已经属于另一个团队，就需要在 `asuka.toml` 里换一个 `id`。`the keychain has Apple Development certificates of N teams`：用 `--team` 选一个团队。
- **`the player hasn't connected yet`**：插上数据线并解锁手机；或者让手机连上 Mac 所在的网络，允许它访问本地网络，并在 Mac 的防火墙中允许 `asuka` 接受连接（“系统设置 › 网络 › 防火墙”）。
- **`no Android SDK: install Android Studio or set ANDROID_HOME`**：Asuka 会依次在 `ANDROID_HOME`、`ANDROID_SDK_ROOT` 中查找，然后是 Mac 上的 `~/Library/Android/sdk`、Windows 上的 `%LOCALAPPDATA%\Android\Sdk` 和 Linux 上的 `~/Android/Sdk`。
- **`no Android NDK`**：在 Android Studio 的 SDK Manager 中安装一个（SDK Tools 下的 NDK）。Asuka 会选用最新的、带有你这台电脑可用版本的 NDK（任何 Windows 电脑上都是 `windows-x86_64`，Arm 电脑也一样）。
- **`no Android device or emulator`**：在 Android Studio 的 Device Manager 中创建一个 AVD，或者连接一部手机。对于 Asuka 自己启动的模拟器，它最多会等四分钟让模拟器完成启动（超时会报 `the emulator didn't boot`）。
- **Java**：Gradle 需要 17 到 24 版的 JDK：`JAVA_HOME` 是其中之一时优先用它，否则使用 Android Studio 自带的 JDK，无论 Android Studio 是用安装程序还是 JetBrains Toolbox 装的（在 Windows 上，位于 Program Files 或 `%LOCALAPPDATA%\Programs` 下），再不然就用电脑上装的其他 JDK 中符合版本的最新一个。一个都没有时，构建会直接说明：把 `JAVA_HOME` 设成其中一个即可。
- **第一次 Android 构建很慢**：Gradle 要下载它自己和 Android 的库，这只发生一次。
- **手机上装有发布构建**：为了安装开发 App，由 `asuka build` 安装的应用（签名密钥不同）会被卸载，其数据也会一起删除。
- **`asuka dev --platform windows runs the app on this computer`**：`asuka dev` 只能在 Windows 上运行 Windows 应用。在其他系统上可以用 `asuka build --platform windows` 构建，再把生成的文件夹复制到 PC 上。
- **Linux 上提示 `... isn't installed`**：应用需要 GTK 4.14 和 libadwaita 1.5 或更高版本，某些包还需要各自的库：`WebView` 需要 WebKitGTK 6.0，视频和音频需要 GStreamer，安全存储需要 libsecret。错误信息会指出缺少的是哪个，用发行版的包管理器安装即可。
- **Linux 应用打不开**：它需要一个显示服务器。通过 SSH 或在 CI 中运行时，请在 Xvfb 下运行（`xvfb-run -a`），对于需要 D-Bus 的功能，还要加上会话总线（`dbus-run-session`）。
- **从头开始**：Asuka 的构建产物、缓存以及各应用的开发数据都存放在 Asuka 的缓存目录中：Mac 上是 `~/Library/Caches/asuka`，Linux 上是 `~/.cache/asuka`，Windows 上是 `%LocalAppData%\asuka`。用 `asuka cache clean` 清空它，就能在没有任何数据的情况下全部重新构建。

## asuka.toml

`asuka dev` 和 `asuka build` 读取的是同一个 `asuka.toml`。每个键的说明参见 [asuka.toml 参考](asuka-toml.md)，发布时它们如何配合参见 [发布](shipping.md#asukatoml)。
