# 快速上手

应用用 Asuka 语言编写（见 [Asuka 语言](language-tour.md)），显示的是各平台的原生控件。`asuka` 命令行工具用每个平台自己的工具为该平台构建应用。一条命令就能创建应用，另一条命令就能运行它，并且每次保存都会自动重新加载。

## 准备工作

你需要命令行工具，以及要运行的每个平台用来构建的工具：

- **iOS**：一台搭载 Apple 芯片、装有 Xcode 26 或更高版本的 Mac，以及 iOS 模拟器或一部 iPhone。（Asuka 不支持在 Intel Mac 上开发。）
- **Android**：一台 Mac、一台 Windows 10 或 11 电脑（x64 或 Arm）或者 Linux 电脑，装好 Android Studio，并在它的 SDK Manager 中安装 NDK（SDK Tools → NDK）；再从 Device Manager 创建一个 Android 模拟器，或者准备一部开启了 USB 调试的手机。Asuka 会在 SDK 的默认安装位置或 `ANDROID_HOME` 中查找 SDK，并使用 Android Studio 自带的 Java，用 NDK 和 Gradle 构建。在 Arm 电脑上，NDK 和 Android 模拟器都是 x64 程序，会以模拟方式运行，模拟器需要使用 x86_64 系统镜像。
- **macOS**：一台搭载 Apple 芯片、装有 Xcode 26 或更高版本（或者它的命令行工具，用 `xcode-select --install` 安装）的 Mac。构建出的应用运行在 macOS 11 或更高版本的 Apple 芯片 Mac 上；用 `--arch amd64` 构建的则运行在 Intel Mac 上。
- **Windows**：一台运行 Windows 10 或 11 的 PC，x64 或 Arm 均可，装好 LLVM 的 clang（`winget install LLVM.LLVM`），或者装好 [Zig](https://ziglang.org/download) 并设置 `ASUKA_CC=zig`。`WebView` 和 `Video` 依赖 Microsoft Edge WebView2 Runtime，Windows 11 自带，大多数 Windows 10 电脑上也已经有了。
- **Linux**：一个 C 编译器（`build-essential`、`gcc`、`clang`），以及 GTK 4.14 和 libadwaita 1.5 或更高版本（Ubuntu 24.04、Fedora 40、Debian 13 及之后的版本都已自带），Wayland 和 X11 均可：

  ```sh
  sudo apt install build-essential libgtk-4-1 libadwaita-1-0   # Debian、Ubuntu
  sudo dnf install gcc gtk4 libadwaita                         # Fedora
  sudo pacman -S base-devel gtk4 libadwaita                    # Arch
  ```

  `WebView` 需要 WebKitGTK 6.0（`libwebkitgtk-6.0-4`），`Video` 和音频需要 GStreamer；如果某个包依赖的库没有安装，它会返回一个错误，告诉你缺的是哪个库。
- **Web**：一个浏览器，以及用来构建 WebAssembly 的 [Zig](https://ziglang.org/download)。

装好 Zig 之后，`asuka build` 还可以在任何系统上构建 Windows 或 Linux 应用；Mac 应用则需要在 Mac 上构建。

## 安装命令行工具

在 macOS 和 Linux 上：

```sh
curl -fsSL https://asuka.egoist.dev/install.sh | sh
```

在 Windows 上，打开 PowerShell 运行：

```powershell
irm https://asuka.egoist.dev/install.ps1 | iex
```

安装脚本会从 GitHub release 下载命令行工具，用 release 中的校验和核对无误后，安装到 `~/.asuka/bin`（Windows 上是 `%LOCALAPPDATA%\Programs\asuka\bin`，脚本会把它加入你的 PATH），然后下载 Asuka 为你这台电脑上的应用预先构建好的库，这样第一次构建就不用再等它们下载。设置 `ASUKA_VERSION=0.3.0` 可以安装指定版本而不是最新版，设置 `ASUKA_INSTALL` 可以换一个安装目录。

`asuka upgrade` 把命令行工具更新到最新的 release（`asuka upgrade 0.3.0` 则更新到这个版本），预先构建好的库也会一并更新。asuka 每天会在后台检查一次有没有更新的 release，如果有，就在命令结束时提示你；这个检查从不让命令等待。设置 `ASUKA_NO_UPDATE_CHECK=1` 可以关掉它，在 CI 中它也不会运行。

`asuka doctor` 会告诉你这台电脑能为每个平台构建和运行什么，以及其余平台还需要安装什么：

```
$ asuka doctor
✓ iOS
  ✓ Xcode: Xcode 26.1
  ✓ a simulator: 4 iPhone simulators
✗ Android
  ✗ the Android SDK and NDK: no Android SDK: install Android Studio or set ANDROID_HOME (Android Studio › SDK Manager: SDK Tools › NDK)
...
```

标着 `!` 的项是可选的：没有模拟器也照样能构建 Android 应用。`asuka doctor -p android` 只检查一个平台，平台没准备好时命令会失败（可以作为 CI 任务的第一步）；`--json` 以 JSON 输出结果。

## 创建应用

```sh
asuka new hello
cd hello
```

`hello` 目录里有：

- `app.asuka`：应用本身，包括一个组件、显示它的 `app` 块，以及一个测试。
- `asuka.toml`：应用的名称、ID（同时用作 bundle ID、application ID 和桌面应用 ID）和版本号，以及[各应用商店和各平台](shipping.md)需要的其他信息。
- `AGENTS.md`：写给编程智能体，说明如何检查、测试和查询错误代码，并要求先读 `asuka guide`，它会把所装版本的语言写在一页里打印出来。
- `.gitignore`。

`asuka new` 能创建三种项目：应用（就是这个），加上 `--lib` 是给其他应用导入的库，加上 `--bin` 是没有窗口的命令行工具，也就是[程序](programs.md)。

## 运行

```sh
asuka dev                      # 在本机运行：Mac、PC 或 Linux
asuka dev --platform ios        # iOS 模拟器
asuka dev --platform ios --device iphone  # 连接着的 iPhone
asuka dev --platform android    # Android 模拟器或手机
asuka dev --platform web        # 你的浏览器
```

保存 `.asuka` 文件后，应用几毫秒内就会重新加载，状态都会保留下来。应用的输出和错误会显示在终端里，还有每次重载的报告（`asuka: reloaded in 2.1 ms, 3 states kept`）。

`asuka dev` 会为平台构建一次**开发 App**：它是应用的外壳（图标、名称、权限），但没有应用的任何代码。Asuka 安装并启动它，然后把你的应用发送过去；每次保存都会重新编译，开发 App 原地加载新版本。如果保存的代码无法编译，会打印错误并把它们显示在应用上方，应用继续运行原来的代码；应用没有捕获的错误也会这样显示（见[应用上方的错误](development.md#应用上方的错误)）。修改 `asuka.toml`、资源或原生代码，会让开发 App 重新构建并启动。

- **在电脑上运行**（`asuka dev`，或 `--platform mac`、`windows`、`linux`）时，开发 App 在一个窗口中运行。
- **在手机上运行**时，`--device` 用来指定 iOS 模拟器或 iPhone（名称或 UDID，写 `iphone` 则表示连接着的那台）或 Android 设备（adb 序列号或 AVD 名称）。`--once` 只启动一次应用，不监听文件变化。
- **在浏览器中运行**时，Asuka 会在 `http://localhost:8790/` 上提供应用并自动打开页面，每次保存都会原地加载到页面中。加上 `--lan` 可以把应用开放给局域网，方便用手机浏览器试用。详见 [Web](web.md)。

要在 iPhone 真机上运行，先把它连上 Mac，解锁后点“信任”，在“设置 › 隐私与安全性”中打开“开发者模式”，并在 Xcode 中添加你的 Apple ID（“设置 › 账户”）。然后运行 `asuka dev --platform ios --device iphone`：第一次它会用你的团队为开发 App 签名并安装，之后 iPhone 通过局域网连接 `asuka dev`。参见 [设备](development.md#设备)。

更多内容见[开发](development.md)：重新加载时会保留什么、设备、日志，以及你可能遇到的问题。

## 修改应用

`asuka new` 生成的 `app.asuka` 是一个计数器：

```asuka
component Counter {
  state count = 0

  SafeArea {
    justify: .center
    align: .center
    gap: 16

    Text("hello: {count}") { font: .title }
    Button("Add") {
      variant: .filled
      on press { count += 1 }
    }
  }
}

app { Counter() }

test "Add counts" {
  let app = mount(Counter())
  app.tap("Add")
  app.expect("hello: 1")
}
```

- **状态**就是 `state count = 0`：在视图中读取它（`"{count}"`）会把视图绑定到它上面；写入它（`count += 1`）会更新读取了它的地方，而且只更新那些地方。
- **组件**声明自己的状态、属性和视图；有东西变化时，组件的主体不会重新运行，重新运行的只有读取了变化内容的那些绑定。
- **样式**基于 flexbox，以点（pt）为单位：`grow`、`gap`、`padding`、`align`、`justify`，以及外观相关的 `background`、`radius`、`shadow` 等。不设置任何样式的应用，看起来就和该平台上的其他应用一样。
- **测试**就是 `test` 块：`asuka test` 不需要设备就能运行它们，并像手机一样对视图进行布局（见[测试](testing.md)）。

同一份代码就是所有平台上的应用。其余内容见 [Asuka 语言](language-tour.md)。

## 应用的文件结构

- `assets/`：图片和其他文件，例如 `Image("photo.jpg")`；字体放在 `assets/fonts` 中。
- `locales/`：应用在各语言下的文案（见[多语言](i18n.md)）。
- 应用自己的包：与应用的 `.asuka` 文件放在一起的一个 `.asuka` 文件目录，用 `import ./components` 导入。依赖写在 `asuka.toml` 的 `[dependencies]` 中（`asuka add @acme/charts`；见[包](packages.md)）。
- 原生包：声明了 `native interface` 或 `native component` 的 Asuka 包，由各平台在自己的项目中实现（Swift package、Gradle 模块、JavaScript，Windows 和 Linux 上则用 C）。详见[原生代码](native.md)。

## 下一步

- [开发](development.md)：开发循环、设备、日志。
- [测试](testing.md)：`test` 块和 `asuka test`。
- [发布](shipping.md)：为 App Store、Google Play、Mac、Windows、Linux 和 Web 构建应用。
