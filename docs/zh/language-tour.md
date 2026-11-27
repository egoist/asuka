# Asuka 速览

Asuka 应用用 Asuka 编写。这是一门为界面而设计的小语言，界面使用各平台自己的控件。它是静态类型的，发布版本会提前编译，开发时保存即在原处重新加载。本页按你接触这门语言的顺序来介绍它；每一部分的完整内容见[语言参考](language/README.md)。

```asuka check
component Counter {
  state count = 0

  Column {
    gap: 12; padding: 24
    Text("Count: {count}") { font: .title }
    Button("Add") { on press { count += 1 } }
  }
}

app { Counter() }
```

一个文件由声明组成：组件、函数、类型、状态。`app { }` 是应用的根，其余部分的顺序随意。

## 值

```asuka check
fn values() -> string {
  let name = "Asuka"               // 常量：类型由推断得出
  var visits = 0                   // 变量
  visits += 1
  let greeting = "Hello, {name}"   // {…} 把值放进字符串
  let ratio: float = 2.5
  let level: u8 = 200              // 固定位宽的整数：i8 … i32、u8 … u64（int 是 64 位）
  let area = 2 ** 10               // 乘方：1024
  let tags = ["new", "work"]       // 数组：[string]
  let ages = ["ana": 31, "bo": 28] // 映射：[string: int]
  let nickname: string? = none     // 可选值：一个字符串或 none
  let shown = nickname ?? name     // none 时的后备值
  return "{greeting} {visits} {ratio} {level} {area} {tags.count()} {ages["ana"] ?? 0} {shown}"
}
```

`var` 用在函数和处理函数里：文件顶层的值不会改变，除非它是 `state`。不同类型的数字不能混在一起运算：`int(level) + visits` 先把 `u8` 转换成 `int`；算术结果超出类型的范围是一个错误，而不会回绕成另一个数字。值总是被复制，从不共享：修改数组的副本不会影响原来的数组。字符串、数组和映射都有你预期的方法（`count()`、`contains`、`map`、`filter`、`sorted`、`split` ……），见[值与类型](language/types.md)。

## 函数

```asuka check
fn area(width: float, height: float = 1) -> float {
  return width * height
}

fn double(x: int) -> int = x * 2     // 只有一个表达式

let a = area(3, height: 4)           // 先按顺序，再按名字
let squares = [1, 2, 3].map(x => x * x)
```

参数先按顺序传，再按名字传（`area(3, height: 4)`）；有默认值的参数可以省略。`x => …` 是闭包。见[函数与闭包](language/functions.md)。

## 结构体与枚举

```asuka check
struct Message {
  subject: string
  read: bool = false

  fn preview() -> string = subject.slice(0, 40)
}

enum Filter {
  all
  unread
  from(sender: string)
}

fn label(f: Filter) -> string = match f {
  .all => "Everything"
  .unread => "Unread"
  .from(sender) => "From {sender}"
}

let m = Message(subject: "Hello")
let n = label(.from(sender: "ana"))

struct Counter {
  n: int = 0

  mut fn bump() { n += 1 }
}

extend string {
  fn shout() -> string = upper() + "!"
}

fn bumped() -> string {
  var c = Counter()
  c.bump()
  return "{c.n}".shout()
}
```

结构体由字段和方法组成；`mut fn` 会修改调用它的那个值（一个 `var` 或者状态）。`extend` 可以给任何类型添加方法和常量，`string` 和 `Color` 也不例外。枚举的分支可以携带值，`match` 必须覆盖每一个分支（漏了哪个，编译器会指出来）。`if`、`match` 和代码块也都是值。见[声明](language/declarations.md)和[控制流](language/control-flow.md)。

## 组件与状态

```asuka check
component Greeting(name: string) {
  prop excited = false                // 一个 prop，带默认值
  state draft = ""                    // 组件自己的状态

  Column {
    gap: 8
    Text(if excited { "Hi, {name}!" } else { "Hi, {name}" })
    TextField { bind text: draft; placeholder: "Reply" }
    if !draft.isEmpty() {
      Button("Send") { on press { draft = "" } }
    }
  }
}

app { Greeting("Ana") { excited: true } }

preview "Greeting, dark" (dark: true) { Greeting("Ana") }
```

组件的头部接收参数；它的代码块里是 prop、状态和视图。状态变化时，只有读取它的地方会更新，不做任何 diff。由会变化的内容得来的状态（`state draft = name`）在被写入之前会一直跟随它；`peek(name)` 只读取一次。`bind` 建立双向绑定，`on press` 处理事件，`if` 和 `for` 决定有哪些视图。`preview` 单独显示一个组件，使用它写明的设置：`asuka dev` 会在设备上用它代替应用来显示，构建则不包含它。见[组件](language/components.md)；多个组件共享的状态见 [Store](language/stores.md)。

## 异步与错误

```asuka check
enum LoadError: Error {
  offline
  server(code: int)
}

async fn fetchCount(online: bool) throws -> int {
  if !online { throw LoadError.offline }
  await sleep(1s)
  return 42
}

component Badge {
  state count = 0
  state problem = ""

  task {
    do {
      count = try await fetchCount(online: true)
    } catch LoadError.offline {
      problem = "You're offline"
    } catch e {
      problem = e.message
    }
  }

  Text(if problem.isEmpty() { "{count}" } else { problem })
}
```

`async` 的工作不会卡住界面，`task { }` 属于启动它的组件：组件消失时它就被取消。可能失败的函数要写 `throws`，调用它时要用 `try`（出错时想得到 none 就用 `try?`）。见[异步](language/async.md)和[错误](language/errors.md)。

组件显示这种调用的结果时用 `load`：`let n = load fetchCount(online: true)` 的值是 `.pending`、`.ready(count)` 或 `.failed(e)`，再次请求时会保留它显示的内容。`n.optimistic(f)` 立刻显示一个改动，`reload fetchCount` 让它的每一个 load 再请求一次，不管它们在哪里，而 `load (a(), b())` 让几个调用的结果一起到达。见[需要加载的数据](language/async.md#需要加载的数据)。

## 包

```asuka check
import std/time

let today = time.now().format("EEE d MMM")
```

`pub` 让声明对其他包可见。标准库（`std/…`）和平台 SDK（`platform/…`）都随命令行工具一起提供：直接 import 就能用。名为 `tray_linux.asuka` 的文件只在 Linux 上属于它所在的包，而 `@platform == .linux` 是一个常量，所以只给某个平台的代码不会出现在其他平台的构建里；`@debug` 在 `asuka dev` 下为 true，只用于开发的代码不会进入发布构建。见[文件、包与导入](language/packages.md)、[标准库与平台 SDK](stdlib.md)、[特定平台的代码](language/control-flow.md#特定平台的代码)以及[开发构建的代码](language/control-flow.md#开发构建的代码)。

## 测试

```asuka check
component Counter {
  state count = 0
  Button("Add {count}") { on press { count += 1 } }
}

test "adding counts" {
  let app = mount(Counter())
  app.tap("Add 0")
  app.expect("Add 1")
}
```

`asuka test` 不需要设备就能运行包里的 `test` 块：`mount` 显示一个视图，测试可以点击、输入，并检查显示的内容。见[测试](language/tests.md)。

## 接下来

- [语言参考](language/README.md)：每一部分的完整内容，从类型及其方法到样式、导航和原生代码。
- [组件](components.md)：构成应用的各种元素。
- [核心概念](concepts.md)：什么会更新、所有者、主线程。
