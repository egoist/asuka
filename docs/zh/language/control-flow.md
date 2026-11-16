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

元组模式（`(true, 0)`）用各自的模式分别匹配元组的每个元素。

`for` 可以遍历数组、map 的 `(key, value)` 对，或者一个区间：`a..<b` 是从 `a` 到 `b` 但不包括 `b` 的整数，`a..b` 则包括 `b`（`0..<3` 就是 `[0, 1, 2]`；`for` 遍历区间时不会真的创建数组）。`while cond { … }` 在 cond 成立时一直循环；`break` 跳出循环，`continue` 直接进入下一轮。

`if let` 和 `while let` 取出可选值里的值，或者匹配一个模式，只有取得到时才执行代码块：`if let u = user { … }`、`while let job = queue.pop() { … }`、`if let .ready(v) = state { … }`。它们的条件可以是一串用逗号分隔的子句，`let` 和布尔条件可以任意交替。每个子句都能用到前面子句绑定的名字；所有子句都成立时才执行代码块（子句按顺序求值，遇到第一个不成立的就停下）：

```asuka
if let user = session.user, user.isAdmin, let team = teams[user.teamId] {
  Text("{team.name}'s admin")
} else {
  Text("Not an admin")
}
```

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

## 特定平台的代码

`@platform` 是应用构建时所针对的平台，类型是 `Platform`：`.iOS`、`.android`、`.macOS`、`.windows`、`.linux` 或 `.web`。它是一个常量，编译应用时就已确定：它不会被追踪，凡是能写值的地方都能用，包括文件顶层和组件主体。由它决定的分支中，某个平台不会走的那一支不会进入该平台的构建：代码、字符串和视图都不会。

```asuka
let saveKey = if @platform == .macOS { "⌘S" } else { "Ctrl+S" }

component Toolbar {
  prop link: string

  Row {
    if @platform == .iOS || @platform == .android {
      Button("Share") { on press { share(link) } }
    }
    match @platform {
      .macOS | .windows | .linux => Text("Save with {saveKey}")
      _ => Text("Saved as you go")
    }
  }
}
```

这适用于 `if` 及其 `else if`、`match @platform`（它不必覆盖 `.unknown`，因为 `@platform` 不会是这个值）、对 `@platform` 的比较做的 `&&`、`||` 和 `!`，以及视图中的 `if`：另一个分支的视图根本不会被创建。不过无论为哪个平台构建，每个分支都会被检查：平台的 API 在每个平台上都是同样的声明，所以 Linux 分支里的错误，在为 iOS 构建时同样会报出来。如果是某个平台专有的声明，单独放在一个文件里通常更清楚，见[特定平台的文件](packages.md#特定平台的文件)。

`environment().platform` 是视图运行所在的平台，由系统给出（测试里由 `mount(…, platform:)` 给出）。它是被追踪的，供视图使用。要决定为某个平台构建哪些代码，用 `@platform`。
