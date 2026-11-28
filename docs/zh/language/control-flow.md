# 控制流

`if` 和 `match` 也可以作为值使用。作为值时，`if` 必须带 `else`，代码块的值就是它的最后一个表达式：

```asuka
let label = if unread > 0 { "{unread} new" } else { "Inbox" }
let icon = match (online, unread) {
  (false, _) => "wifi.slash"
  (true, 0) => "tray"
  _ => "tray.full"
}
```

如果某个分支的值本身看不出类型（空的 `[]`、`[:]`，或者隐式的 `.case`），它就沿用另一个分支的类型：`if let c = chosen { [c] } else { [] }` 是 `[Story]`。

元组模式（`(true, 0)`）用各自的模式分别匹配元组的每个元素。匹配可选的元组时，它和成员模式一样，在有值时匹配其中的元组（`none` 要单独写一个分支）：`if let (name, age) = found { … }`、`let (x, y) = cursor else { return }`。

带类型的名字（`c: Circle`）用来判断接口值是什么类型：值是这个类型时才匹配，并以这个类型绑定它；结构体的模式（`Circle(radius)`）和写明类型的枚举成员（`Unit.big(n)`）也是这样。对接口值的 `match` 要以 `_` 结尾，详见[接口与泛型](functions.md#接口与泛型)：`if let c: Circle = shape { … }`、`let sq: Square = shape else { return }`。

`for` 可以遍历数组、集合、map 的 `(key, value)` 对、字符串的 char（`for c in name`）、惰性序列（`Seq`，见[值与类型](types.md#惰性序列)），或者一个区间：`a..<b` 是从 `a` 到 `b` 但不包括 `b` 的整数，`a..b` 则包括 `b`（`0..<3` 就是 `[0, 1, 2]`；`for` 遍历区间时不会真的创建数组）。`while cond { … }` 在 cond 成立时一直循环；`break` 跳出循环，`continue` 直接进入下一轮。

`for` 也可以遍历你自己定义的类型的值：一种是迭代器，它的类型有 `mut fn next() -> T?`（`for` 在它的一份副本上反复调用，直到得到 `none`）；另一种是序列，它的 `fn iter()` 返回一个迭代器或一个 `Seq`（`for` 只调用一次）。prelude 里的 `Iterator<T>` 就表示这样的类型，可以用作约束：`fn sum<I: Iterator<int>>(xs: I) -> int`。迭代器返回 `none` 就表示结束，所以元素本身可能是 none（`T?`）的类型应当返回 `Seq`：`fn iter() -> Seq<int?> = slots.lazy()`。

```asuka check
struct Node {
  name: string
  children: [Node] = []
}

struct Walk {
  stack: [Node]

  mut fn next() -> Node? {
    let n = stack.pop() else { return none }
    stack = stack + n.children.reversed()
    return n
  }
}

extend Node {
  fn walk() -> Walk = Walk(stack: [self])
}

fn count(root: Node) -> int {
  var n = 0
  for node in root.walk() { if node.name.hasSuffix(".asuka") { n += 1 } }
  return n
}
```

在 `for` 或 `while` 的头部后面写 `as name` 可以给循环起名字：在它里面的任何一层循环中，`break name` 都会跳出这个循环，`continue name` 都会直接进入它的下一轮（被跳出的每一层循环的 `defer` 都会执行）。不能在闭包里这样用；声明了却没用到的名字，以及用到了却没声明的名字，都会报错。

```asuka
for row in grid as rows {
  for cell in row {
    if cell == target { found = true; break rows }
    if cell.isEmpty() { continue rows }
  }
}
```

`if let` 和 `while let` 取出可选值里的值，或者匹配一个模式，只有取得到时才执行代码块：`if let u = user { … }`、`while let job = queue.pop() { … }`、`if let .ready(v) = state { … }`。它们的条件可以是一串用逗号分隔的子句，`let` 和布尔条件可以任意交替。每个子句都能用到前面子句绑定的名字；所有子句都成立时才执行代码块（子句按顺序求值，遇到第一个不成立的就停下）：

```asuka
if let user = session.user, user.isAdmin, let team = teams[user.teamId] {
  Text("{team.name}'s admin")
} else {
  Text("Not an admin")
}
```

`if var` 和 `if let` 一样，只是绑定的名字是一份可以修改的副本，适合修改多个字段再写回去的代码：`if var m = draft { m.read = true; m.tags.push("seen"); draft = m }`。

`let … else` 要么绑定一个值，要么离开。可选值为 none（或者模式不匹配）时执行 `else` 代码块，而这个代码块必须离开：`return`、`throw`、`break`、`continue` 或 `panic(…)`（如果它可能继续往下执行，编译器会报错）。在它之后，这个名字就是其中的值，而不再是可选值：

```asuka
fn summary(id: string) -> string {
  let message = cache[id] else { return "(deleted)" }
  let .ready(body) = message.body else { return message.subject }
  return "{message.subject}: {body}"
}
```

`throw`、`return`、`break` 和 `continue` 也可以出现在需要值的地方：它们不提供值，而是直接离开（它们的类型是 `never`，可以放在任何类型的位置上）。写在 `??` 后面，就表示遇到 none 时该怎么办：

```asuka
let user = cache[id] ?? throw MailError.missing(id)
for line in lines {
  let n = line.toInt() ?? continue
  total += n
}
```

它们同样可以用作 `if` 的分支、`match` 的分支，或者闭包的主体：`x => x ?? throw Error("empty")`。

## 修改可能不存在的东西

从 `var` 或状态开始、经过字段、下标和 `?.` 的 `?.` 链可以被赋值（`=`、`+=` 等），也可以调用它的 `mut fn`。链上遇到 `none` 时什么也不会发生：不会写入任何东西（右边的值甚至不会被计算），状态的读取者也不会收到通知。

```asuka
inbox[id]?.read = true          // 没有这条消息时什么也不做
session.user?.name = draft
todos.first()?.toggle()          // 错误：first() 给出的是一个值，而不是一个位置
```

`m[k, default: v]` 表示映射里的条目，没有这个条目时就是 `v`。读取时它的类型是 `V`（而不是 `V?`）；写入时（`=`、`+=`、`mut fn`、`push`）它写的是 `m[k]`，而且只有条目不存在时才会计算 `v`。计数和分组都只需要一行：

```asuka check
fn tally(words: [string]) -> ([string: int], [int: [string]]) {
  var counts: [string: int] = [:]
  var byLength: [int: [string]] = [:]
  for w in words {
    counts[w, default: 0] += 1
    byLength[w.count(), default: []].push(w)
  }
  return (counts, byLength)
}
```

## 特定平台的代码

`@platform` 是应用构建时所针对的平台，类型是 `Platform`：`.iOS`、`.android`、`.macOS`、`.windows`、`.linux` 或 `.web`。它是一个常量，编译应用时就已确定：它不会被追踪，凡是能写值的地方都能用，包括文件顶层和组件主体。由它决定的分支中，某个平台不会走的那一支不会进入该平台的构建：代码、字符串和视图都不会。

```asuka check
import platform/device

let saveKey = if @platform == .macOS { "⌘S" } else { "Ctrl+S" }

component Toolbar {
  prop link: string

  Row {
    if @platform == .iOS || @platform == .android {
      Button("Share") { on press { device.share(link) } }
    }
    match @platform {
      .macOS | .windows | .linux => Text("Save with {saveKey}")
      _ => Text("Saved as you go")
    }
  }
}
```

这适用于 `if` 及其 `else if`、`match @platform`（它不必覆盖 `.unknown`，因为 `@platform` 不会是这个值）、对 `@platform` 的比较做的 `&&`、`||` 和 `!`，以及视图中的 `if`：另一个分支的视图根本不会被创建。不过无论为哪个平台构建，每个分支都会被检查：平台的 API 在每个平台上都是同样的声明，所以 Linux 分支里的错误，在为 iOS 构建时同样会报出来。如果是某个平台专有的声明，单独放在一个文件里通常更清楚，见[特定平台的文件](packages.md#特定平台的文件)。

`environment().platform` 是视图运行所在的平台，由系统给出（测试里由 `mount(…, platform:)` 给出）。它是被追踪的，供视图使用。要决定为某个平台构建哪些代码，用 `@platform`。Apple TV 应用由 iOS 应用构建而成（在那里 `@platform` 是 `.iOS`）：是不是电视要看 `environment().tv`（[电视](../tv.md)）。Apple Watch 应用也一样，是不是手表要看 `environment().watch`（[Apple Watch](../watch.md)）。

## 开发构建的代码

`@debug` 表示应用是不是开发构建，类型是 `bool`：在 `asuka dev`、`asuka run` 和 `asuka test` 下为 true，在 `asuka build` 和 `asuka publish` 产出的构建里为 false。它同样是常量，折叠方式和 `@platform` 一样：发布构建不会走的分支，比如调试菜单、测试服务器的地址、只在开发时需要的日志，都不会进入发布构建。

```asuka check
let server = if @debug { "https://staging.example.com" } else { "https://example.com" }

component About {
  prop version: string

  Column {
    Text("Version {version}")
    if @debug && @platform == .iOS {
      Text("Development build, talking to {server}")
    }
  }
}
```

`!@debug` 同样会被折叠，它和 `@platform` 的比较用 `&&`、`||` 组合起来也一样。如果走到的分支会直接返回，它后面的代码也会被去掉：写了 `if !@debug { return cached }` 之后，这个块剩下的部分不会出现在发布构建里。`@platform` 的分支也是如此。每次构建都会检查两个分支，所以你在开发分支里工作时，发布分支不会在你看不到的地方悄悄坏掉。

测试和 `asuka dev` 一样是开发构建：`asuka test --release` 会让 `@debug` 为 false 来运行测试，和应用发布时一致；对程序来说，`asuka run --release` 也是如此。基准测试（`asuka test --bench`）测的是发布构建运行的代码，`@debug` 为 false。

## 构建给出的值

`@define("KEY")` 是构建交给程序的值，同样是常量：服务器的地址、在某个构建里打开而在另一个构建里关闭的功能、应用用于某项服务的公开密钥。构建从 `asuka.toml` 的 [`[defines]`](../asuka-toml.md#defines) 里取值，构建所用的[变体](../asuka-toml.md#variants)（`--variant staging`）里的值覆盖它们，命令行上的 `--define KEY=VALUE` 又覆盖前两者：

```toml
[defines]
API_URL = "https://api.example.com"
ANALYTICS = true

[variant.staging.defines]
API_URL = "https://staging.example.com"
ANALYTICS = false
```

它的类型就是它在 `asuka.toml` 里的值的类型：字符串、`int`、`float` 或 `bool`。只由 `--define` 给出的键是字符串；`asuka.toml` 里已有的键沿用那里的类型（`PORT = 8080` 时，`--define PORT=9000` 是 `int`）。构建没有给出的键会报错，所以拼错的键在编译时就会被发现。带默认值的写法 `@define("KEY", default)` 在构建没有给出值时取默认值，值的类型由默认值决定：默认值是字面量，可以是字符串、数字或 bool，构建给出的值必须是这个类型（`--define` 的文本会按这个类型读取：对 `@define("RETRIES", 3)` 来说，`--define RETRIES=5` 就是 `int` 5）。

```asuka check
let server = @define("API_URL", "https://api.example.com")
let retries = @define("RETRIES", 3)

component Settings {
  Column {
    Text("Talking to {server}, {retries} tries")
    if @define("ANALYTICS", true) {
      Text("Usage statistics are on")
    }
  }
}
```

`bool` 类型的值和 `@debug` 一样会被折叠，无论是在 `if` 里，还是用 `!`、`&&`、`||` 组合：构建不走的分支不会进入构建。键是字符串字面量，由字母、数字和 `_` 组成，以字母或 `_` 开头。

构建给出的值会编译进应用，拿到应用的人都能读出来。绝不要用它传递机密：服务器的 API 密钥、签名密钥、密码。服务器地址、功能开关，或者本来就是公开的密钥（客户端 ID、统计服务的衡量 ID、可公开的 publishable key）都没问题；必须保密的东西留在你的服务器上，由应用去请求。
