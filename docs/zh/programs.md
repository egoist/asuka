# 程序

除了应用和库，Asuka 还能写**程序**：没有窗口、也没有视图的命令行工具。只要一个包的文件里声明了 `fn main()`，它就是程序，就像 Go 的 `package main`、Cargo 的 `src/main.rs`。程序的构建方式和应用一样：同一门语言、同一套标准库（文件、JSON、时间、SQLite、HTTP……），并编译成 macOS、Windows 和 Linux 上的原生可执行文件。

```asuka
import std/os

fn main() {
  let name = os.args().first() ?? "world"
  print("Hello, {name}!")
}
```

## 创建程序

```sh
asuka new --bin greet   # asuka.toml，以及带 main 和一个测试的 main.asuka
cd greet
asuka run -- Ada        # Hello, Ada!
asuka test              # 运行它的测试块
asuka build             # 生成可执行文件 dist/greet
```

`asuka new` 创建应用，`asuka new --lib` 创建库，`asuka new --bin` 创建程序。一个应用项目也可以自带程序，放在各自的目录里，见[应用自带的程序](#应用自带的程序)。

## main

程序从 `fn main()` 开始。它不接受参数（参数通过 `os.args()` 读取），也不返回值（退出码用 `os.exit(code)` 指定）；它可以是 `async` 的，也可以抛出错误。带 `fn main` 的包不能有 `app` 块，也不能声明组件：程序不显示视图。只有构成程序的那个包才有入口；它导入的包里如果也有 `main`，那只是一个普通函数。

```asuka
import std/fs

async fn main() throws {
  let text = try await spawn { try fs.readText("notes.txt") }
  print("{text.lines().count()} lines")
}
```

程序的运行过程如下：

- 先创建各个包的值（它们的 `let`），然后运行 `main`。`async` 的 `main` 作为一个 task 运行：和在应用里一样，`await` 等待时其他工作照常进行，不会占住主线程。
- `main` 返回时，程序以退出码 0 结束。在此之前，它启动后仍在运行的东西（正在读取的 stream、某个包的 task）会先被取消，它们的 `defer` 会执行。
- `main` 抛出（或没有捕获）的错误会让程序以 1 结束：`error: ` 加上错误的消息写到 stderr，后面跟着抛出的位置，最内层的函数在前：

  ```text
  error: bad name fail
      check (main.asuka:18)
      main (main.asuka:8)
  ```

- `os.exit(code)` 立即以 `code` 结束程序：stdout 和 stderr 会被刷新，但 `defer` 和其他 task 不会执行。
- `print` 向 stdout 写一行；`os.eprint` 写到 stderr。

## 进程：std/os

| 函数 | 作用 |
|---|---|
| `os.args()` | 参数，不含程序本身 |
| `os.program()` | 程序启动时的名字或路径（`argv[0]`） |
| `os.env(name)` | 环境变量的值，没有设置时为 `none` |
| `os.environ()` | 所有环境变量，`[string: string]` |
| `os.cwd()` | 工作目录 |
| `os.exit(code)` | 立即结束进程（类型是 `never`） |
| `os.readLine()` | 读取 stdin 的一行（不含行尾），读到末尾时为 `none`；会等待这一行 |
| `os.readAll()` | stdin 剩下的全部内容，`bytes` |
| `os.lines()` | 逐行读取 stdin（一个 `Stream<string>`），在另一个线程上读，程序可以同时做别的事 |
| `os.eprint(text)` | 向 stderr 写一行 |
| `os.writeTo(.stdout, text)`、`writeBytesTo` | 原样写出文本或字节，不加行尾，目标是 `.stdout` 或 `.stderr` |
| `os.terminal(.stdout)` | 某个流是否是终端（有人在看），而不是管道或文件：只在终端上输出颜色和提示 |

```asuka
import std/os

async fn main() throws {
  var total = 0
  for await line in os.lines() {
    total += line.toInt() ?? 0
  }
  print(total)
  if os.env("DEBUG") != none {
    os.eprint("summed stdin")
  }
}
```

在手机和 Web 上，应用的 `os.args()` 为空，stdin 也是空的，`os.exit` 会抛出错误：那里的应用不能结束自己的进程。桌面应用的 `os.args()` 是它自己进程的参数。

## 命令行参数：std/flag

`flag.parse<T>(os.args())` 根据编译器对 `T` 的了解，把命令行读成一个 `T`：不需要另外维护一张和字段对应的参数表。

```asuka
import std/flag
import std/os

enum Unit { lines, words, bytes }

/// Counts what's in files.
struct Options {
  /// -v Say each file's count.
  verbose: bool = false
  /// -u What to count.
  unit: Unit = .lines
  /// How many files at once.
  jobs: int = 4
  /// Names to leave out.
  skip: [string] = []
  /// The files to count.
  files: [string]
}

fn main() {
  let o = flag.parse<Options>(os.args())
  print("{o.unit} in {o.files.count()} files, {o.jobs} at once")
}
```

- **有默认值**的字段是选项：`dryRun` 对应 `--dry-run`。它的文档注释就是帮助里的那一行；如果注释的第一个词是 `-x`（一个字母或数字），那就是它的短名。
- `bool` 是开关：`--verbose`、`-v`；默认开启的开关（`color: bool = true`）用 `--no-color` 关闭，也可以写 `--color=false`。
- `int`、`float`、`string` 和没有附带值的枚举成员（`--unit words`：成员名的 kebab-case 写法）需要一个值：`--jobs 8`、`--jobs=8`、`-j8`、`-j 8`。数组可以反复给出（`--skip a --skip b`）；可选类型可以省略。
- 短开关可以合写：`-vf` 就是 `-v -f`，`-vu words` 把值交给 `-u`。
- **没有默认值**的字段是位置参数，顺序和字段一致：数组接收剩下的全部参数（所以它是最后一个），可选类型可以省略（放在不能省略的之后）。
- `--` 结束选项：之后的内容都是位置参数，不管是否以 `-` 开头。
- `-h` 和 `--help` 打印帮助并以 0 退出；`--version` 打印 asuka.toml 里的 `version` 并以 0 退出。
- 用法出错时，打印问题所在和用法，并以 2 退出：

  ```text
  error: unknown option --jbos (did you mean --jobs?)

  Usage: wc [options] [<files>...]

  For more, try 'wc --help'.
  ```

结构体的文档注释是帮助的第一行：

```text
Counts what's in files.

Usage: wc [options] [<files>...]

Arguments:
  [<files>...]  The files to count.

Options:
  -v, --verbose                   Say each file's count.
  -u, --unit <lines|words|bytes>  What to count. (default: lines)
      --jobs <n>                  How many files at once. (default: 4)
      --skip <skip>...            Names to leave out.
  -h, --help                      Show this help.
      --version                   Show the version.
```

解析器处理不了的类型，会在对应字段处报编译错误：没有直接写出来的默认值（比如函数的返回值）、map、把 `-h` 用作短名、没有默认值的开关。`flag.tryParse<T>(args)` 做 `parse` 的工作，但不打印也不退出：它抛出 `flag.FlagError`（`.help(text)`、`.version(text)`、`.usage(problem, usage, program)`）；`flag.help<T>()` 返回帮助文本。

### 子命令

如果一个枚举的成员各自附带一个选项结构体，它就是一组子命令，就像 `git add` 和 `git commit`：用 `flag.parse<Command>(args)`，或者在结构体里放一个这个枚举类型的字段，它出现在结构体自己的选项之后。成员的文档注释是它在帮助里的那一行；`tool add --help` 显示这个子命令自己的帮助。

```asuka
import std/flag
import std/os

struct AddOptions {
  /// -f Add ignored files too.
  force: bool = false
  /// The files to add.
  paths: [string]
}

struct CommitOptions {
  /// -m The commit's message.
  message: string = ""
}

/// A tiny version control.
enum Command {
  /// Add files to the index.
  add(AddOptions)
  /// Record the index.
  commit(CommitOptions)
  /// Show the working tree's state.
  status
}

struct Cli {
  /// -q Say less.
  quiet: bool = false
  command: Command
}

fn main() {
  let cli = flag.parse<Cli>(os.args())
  match cli.command {
    .add(o) => print("adding {o.paths}")
    .commit(o) => print("committing: {o.message}")
    .status => print("on main")
  }
}
```

## 运行其他程序：std/command

`std/command` 用来运行其他程序。参数是一个列表，原样传过去，绝不经过 shell：带空格或 `;` 的文件名就是一个参数。会阻塞的操作（读管道、等待结束）都在其他线程上进行，主线程不会等待。

```asuka
import std/command

async fn main() throws {
  let out = try await command.run("git", ["status", "--short"])
  print(out.text())

  let p = try command.start("ping", ["-c", "3", "example.com"])
  for await line in p.lines() {
    print("> {line}")
  }
  print("ping exited with {await p.wait()}")
}
```

- `command.run(program, args, cwd:, env:, clearEnv:, stdin:, check:)` 运行一个程序直到结束，返回 `Output`：`code`、`stdout` 和 `stderr`（字节；`text()` 和 `errorText()` 得到文本，还有 `ok()`）。`stdin:` 的内容会交给它；`check: true` 时，非 0 的退出码会变成 `CommandError.failed`。
- `command.status(program, args, …)` 让程序直接使用当前程序的 stdin、stdout 和 stderr（它打印的内容会显示出来），返回退出码。
- `command.start(program, args, …, stdin:, stdout:, stderr:)` 启动程序并返回一个 `Process`：`pid`，`stdout()` 和 `stderr()`（字节流）或 `lines()` 和 `errorLines()`，`write(text)` 和 `closeStdin()`，`wait()`，`output()`（读完全部输出再取退出码），以及 `kill(.term)`。每个流是一个 `Stdio`：`.piped`（stdout 和 stderr 的默认值）、`.inherit` 或 `.null`（stdin 的默认值）。
- 程序名不是路径时，会在 `PATH` 上查找；`env:` 叠加在当前程序的环境变量之上（`clearEnv: true` 时只用它）。找不到程序时抛出 `CommandError.notFound`。
- 等待程序（`run`、`output`、`wait`）的 task 被取消时，那个程序也会被结束。
- 在 macOS 和 Linux 上，被信号结束的程序，退出码是 128 加上信号的编号。在 Windows 上，程序按 `CreateProcess` 的规则查找（会补上 `.exe`）；`.bat` 或 `.cmd` 要通过 `cmd /c` 作为程序来运行。

这里没有 `command.shell("…")`：shell 会解释传给它的文本里的引号、`$` 和 `;`，文件名就是这样变成命令的。确实需要执行一行 shell 命令时，就显式地运行 shell：`command.run("sh", ["-c", line])`。

它可以在程序和桌面应用（macOS、Windows、Linux）里使用；手机和 Web 上的应用不能运行其他程序（`CommandError.unsupported`）。

## 运行

`asuka run` 直接运行程序的 `main`，不需要等待构建：`--` 之后的参数都交给程序，退出时带上程序的退出码。

```sh
asuka run -- --jobs 8 a.txt b.txt
asuka run --bin notes -- add "Buy milk"   # asuka.toml 里 [bin] 中的一个程序
```

`asuka dev` 运行程序，并在你每次保存后重新运行（就像 `cargo watch -x run`）：保存时如果上一次还没结束，会先停掉它。对于没有 `fn main` 的文件或包，`asuka run` 还是和以前一样，运行它的测试块并显示它的应用画面。

## 构建

`asuka build` 把程序编译成本机的可执行文件，放在 `dist/` 里：

```sh
asuka build                          # dist/greet
asuka build --target linux-amd64     # dist/linux-amd64/greet
asuka build --target windows-arm64   # dist/windows-arm64/greet.exe
```

`--target` 由 `darwin`、`linux` 或 `windows` 加上 `amd64` 或 `arm64` 组成；本机以外的系统和桌面应用一样，用 Zig 构建。可执行文件在系统允许的范围内是独立的：在 Linux 上它是静态链接的（musl，需要 Zig），除非它用到的东西要加载系统的库（std/http 的 libsoup）；在 Windows 上只依赖系统自带的 DLL；在 macOS 上只依赖系统的库。

带平台代码的包在每个桌面系统上的程序里都能用：`std/http` 在 macOS 上用 URLSession、在 Windows 上用 WinHTTP、在 Linux 上用 libsoup 发请求，和在应用里一样。只是没有窗口。SQLite（`std/sqlite`）和在应用里一样编译进可执行文件。

### 应用自带的程序

应用可以自带程序，就像 VS Code 自带 `code` 命令：在终端里操作应用数据的命令。一个包不能同时有 `app { }` 和 `fn main()`，所以每个程序放在自己的目录里，并在 asuka.toml 中声明：

```toml
[bin.notes]
path = "cli"
```

程序和其他包一样，用相对路径导入项目里的包（`import ../model`）。`asuka run --bin notes` 运行它（只有一个程序时可以省略 `--bin`），`asuka test` 会把各个程序的测试和应用的测试一起运行，`asuka build --bin notes` 单独构建它。应用自带的程序把数据保存在应用保存的地方（std/fs 的 `appDir`，以及其中的 SQLite 数据库），所以两者读写的是同一份数据；开发时，`asuka run --bin` 使用 `asuka dev` 的那份。

桌面应用的构建会把程序放进应用里：

| 构建 | 位置 | 加到用户的 PATH |
|---|---|---|
| macOS | `Notes.app/Contents/Helpers/notes`，通用二进制，和应用一起签名 | 由用户自己（或应用提供选项）创建链接：`ln -s /Applications/Notes.app/Contents/Helpers/notes /usr/local/bin/notes` |
| Linux | 安装前缀的 `bin/notes`，和应用的可执行文件在一起，tar 包和 Flatpak 里都有 | 随应用安装到 `~/.local/bin` 或 `/usr/bin`；Flatpak 中用 `flatpak run --command=notes <id>` 运行 |
| Windows | 应用 `.exe` 旁边的 `notes.exe` | 由安装程序把应用的文件夹加入 `PATH` |

手机和 Web 会忽略 `[bin]`。[examples/notes](../../examples/notes) 就是一个自带 `notes` 命令的应用，这个命令可以列出、添加和勾选应用里的笔记。

### 由其他工具链构建的程序

由其他工具链构建的程序（用 Go 写的服务、用 Rust 写的工具，由应用启动）也以同样的方式放进应用。它的 `[bin]` 写的不是目录，而是构建它的命令，应用为哪些目标构建，这个命令就针对每个目标运行一次（通用 Mac 应用的两个架构会用 `lipo` 合并）。占位符用各个工具链自己的名字表示目标：Go 用 `{goos}` 和 `{goarch}`，Rust 用目标三元组 `{target}`；`{os}` 和 `{arch}` 是 Asuka 自己的名字（`macos`、`x64`），供你自己的脚本使用。不能写到 `{out}` 的构建（比如 cargo）用 `output` 说明程序留在哪里（完整列表见 [asuka.toml](asuka-toml.md#bin)）：

```toml
[bin.notes-server]
build = ["go", "build", "-o", "{out}", "./cmd/server"]
env = { GOOS = "{goos}", GOARCH = "{goarch}", CGO_ENABLED = "0" }

[bin.notes-index]
build = ["cargo", "build", "--release", "--manifest-path", "index/Cargo.toml", "--target", "{target}"]
output = "index/target/{target}/release/notes-index{exe}"
```

在目标系统上构建时，`{target}` 是该系统自己的三元组（Windows 上是 `x86_64-pc-windows-msvc`）；交叉构建时，是不需要那个系统的 SDK 就能构建的三元组（`x86_64-pc-windows-gnu`，即 MinGW；`x86_64-unknown-linux-musl`，静态链接）。Rust 的目标用 `rustup target add` 安装，交叉构建还需要对应的链接器。

如果占位符给出的参数不是你想要的，`build` 可以运行你自己的脚本，由脚本在运行时决定参数：目标在它的环境变量里（`ASUKA_OS`、`ASUKA_ARCH`、`ASUKA_EXE`、`ASUKA_TARGET`、`ASUKA_GOOS`、`ASUKA_GOARCH`），程序写到 `ASUKA_OUT`。下面这个脚本在任何地方都构建静态链接的 Linux 程序，并为 Mac 开启一个 feature：

```toml
[bin.notes-index]
build = ["sh", "index/build.sh"]
```

```sh
#!/bin/sh
# index/build.sh: 在项目目录中运行，每个目标一次。
set -e
target=$ASUKA_TARGET
features=
case $ASUKA_OS in
  linux)
    case $ASUKA_ARCH in
      x64) target=x86_64-unknown-linux-musl ;;
      arm64) target=aarch64-unknown-linux-musl ;;
    esac ;;
  macos) features="--features metal" ;;
esac
cargo build --release --manifest-path index/Cargo.toml --target "$target" $features
cp "index/target/$target/release/notes-index$ASUKA_EXE" "$ASUKA_OUT"
```

脚本在构建应用的机器上运行，而不是在应用运行的机器上：在 Mac 上构建 Windows 版时，用的是 Mac 的 `sh`。要在 Windows 上构建，就写 Windows 能运行的东西：`["pwsh", "-File", "index/build.ps1"]`（输出路径是 `$env:ASUKA_OUT`），或者任何语言写的程序，比如 `["go", "run", "./tools/build-index"]`。

应用用 `command.bundled(name)` 找到自己的程序：构建里的路径，没有时（手机、Web、测试）为 none。`asuka dev` 启动时会构建它们，应用也用同样的方式找到：

```asuka
import std/command

async fn startServer() throws -> command.Process {
  let server = command.bundled("notes-server") ?? "notes-server" // 否则用 PATH 上的
  return try command.start(server, ["serve"])
}
```

## 测试

和应用一样，程序的测试块用 `asuka test` 运行。`main` 是一个函数：测试里用 `os.fake` 像真正的进程那样运行它，`os.fake` 提供参数、环境变量和 stdin，并保存它写出的内容：

```asuka
import std/os

fn main() {
  let name = os.args().first() ?? "world"
  print("Hello, {name}!")
}

test "greets whom it's given" {
  let o = os.fake(args: ["Ada"], env: ["LANG": "en"], stdin: "")
  expect(await o.run(main) == 0)
  expect(o.stdout() == "Hello, Ada!\n")
}
```

`o.run(main)` 返回程序本该结束时的退出码：0；抛出错误时为 1（消息在 `o.stderr()` 里）；或者 `os.exit` 给出的值，这时 `os.exit` 只结束测试里的 `main`，而不是整个进程。没有 fake 时，测试里的 `os.args()` 和 stdin 都是空的。`command.fake()` 替其他程序作答，这样它们就不会真的运行：

```asuka
import std/command

test "reads git's branch" {
  let git = command.fake()
  git.answer("git", ["branch", "--show-current"], stdout: "main\n")
  let out = try await command.run("git", ["branch", "--show-current"])
  expect(out.text() == "main\n")
  expect(git.calls == [["git", "branch", "--show-current"]])
}
```

## 各平台支持情况

| | macOS | Windows | Linux | 手机、Web |
|---|---|---|---|---|
| 程序（`asuka build`） | 支持 | 支持 | 支持 | 不支持 |
| `os.args`、`os.exit`、stdin | 支持 | 支持 | 支持 | 应用里为空，`exit` 会抛出错误 |
| `std/command` | 支持 | 支持 | 支持 | `unsupported` |
| `std/flag` | 支持 | 支持 | 支持 | 支持（它能解析任何列表） |
