# 文件、包与导入

源文件以 `.asuka` 结尾。一个目录里的源文件构成一个**包**：同一个包的文件不需要导入，就能看到彼此的声明。加上 `pub` 的声明才对其他包可见。

```asuka
import std/http                 // 标准库
import platform/camera          // 平台 SDK
import @acme/charts               // 一个依赖（asuka add @acme/charts）
import ./components               // 与当前包并列的另一个包
import ./model.{Message, Folder}  // 直接使用这些名字，不用加包名
import ./theme as t               // 起一个更短的名字
import ../model                   // 上一级目录里的包
```

包里的名字要通过包名来使用：`http.get(url)`、`components.Card("Hi")`。包名取自路径的最后一段；最后一段不能用作名字的包（比如 `@acme/line-charts`），导入时要用 `as` 给它起一个名字。预置模块（prelude，即内置的类型、元素和函数）不需要导入。`std/...` 和 `platform/...` 这些包随命令行工具一起提供，都列在[标准库与平台 SDK](../stdlib.md)里；其他包不能以 `std/` 或 `platform/` 命名。

语句和声明在行尾结束；想在一行里写多条，用 `;` 隔开。如果一行以运算符、逗号或左括号结尾，就会接着下一行继续。

注释写作 `// …` 和 `/* … */`。`///` 注释用来给它下面的声明写文档，编辑器会显示出来。

## 特定平台的文件

文件名以某个平台的后缀结尾的文件，只有在为该平台构建应用时才属于它所在的包。单个平台的后缀是 `_ios`、`_android`、`_macos`、`_windows`、`_linux` 和 `_web`；`_desktop`（macOS、Windows 和 Linux）和 `_mobile`（iOS 和 Android）则对应一组平台。不同平台的文件可以各自声明同样的名字，包里的其他文件像使用普通声明一样使用它们：

```asuka
// feedback_mobile.asuka：iOS 和 Android
pub fn confirm() { haptic(.success) }

// feedback_desktop.asuka：macOS、Windows 和 Linux
pub fn confirm() { announce("Done") }

// feedback_web.asuka
pub fn confirm() {}
```

在每个平台上，一个名字只能声明一次：要么声明在所有平台都有的文件里，要么在每个需要它的平台各自的文件里声明。如果一个名字既在所有平台共有的文件里声明，又在某个平台的文件里声明，或者在同一个平台会同时包含的两个文件里都声明了（比如 `feedback_desktop.asuka` 和 `feedback_linux.asuka`），那么无论为哪个平台构建，都会报错。文件名只有后缀的文件（`linux.asuka`）是普通文件。

所有包都按文件名用同样的规则处理：应用自己的包、依赖、标准库都一样。构建出来的总是某一个平台的版本：`asuka dev` 和 `asuka build` 为它们运行应用的那个平台构建；`asuka test` 默认为 iOS 构建，`--platform` 可以指定别的平台；程序则为运行它的那台计算机构建（或者为 `--target` 指定的目标构建）。`asuka check` 会分别检查每个平台的文件；如果某个错误并非所有平台都有，它会注明出现在哪些平台上。比如一个名字只在部分平台的文件里声明，其他平台用到它的地方就会报错。编辑器打开某个平台的文件时，会连同该平台的其他文件一起分析；打开其他文件时按 iOS 的文件分析，并显示所有平台的错误。如果要在同一个文件里按平台写不同的代码，可以用 [`@platform`](control-flow.md#特定平台的代码)。

## 应用、程序与库

一个包要么是入口，要么是库。带 `app { }` 块的包是应用：这个块就是它显示的内容（见[导航](navigation.md#导航)）。文件里声明了 `fn main()` 的包是**程序**，也就是命令行工具：`main` 不接受参数（参数通过 `os.args()` 读取），不返回值（`os.exit(code)` 以某个退出码结束程序），可以是 `async` 的，也可以抛出错误；`main` 结束，程序也就结束。两者都没有的包是库，供其他包导入。一个包不能同时是应用和程序，程序也不能声明组件：它不显示视图。只有构成应用或程序的那个包才有入口；它导入的包里的 `main` 只是一个普通函数。详见[程序](../programs.md)。

```asuka
import std/os

fn main() {
  print("Hello, {os.args().first() ?? "world"}!")
}
```
