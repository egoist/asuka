# 组件

组件是界面中的一块，参数在头部声明，其余所有内容都写在主体里：

```asuka
component MessageRow(message: Message) {
  prop compact = false            // 一个具名 prop，带默认值
  prop accessory: View? = none    // 一个插槽：由调用方传入的视图
  event archive                   // 它对外报告的事件
  state expanded = false          // 它自己的状态

  Row {
    gap: 12; padding: [12, 16]
    Avatar(message.from)
    Column {
      grow: 1
      Text(message.subject) { font: .headline; lines: 1 }
      if !compact { Text(message.preview()) { color: .secondaryLabel } }
    }
    accessory
  }
  contextMenu {
    Button("Archive") { role: .destructive; on press { emit archive } }
  }
}
```

使用组件的方式和使用内置元素完全一样。头部参数放在圆括号里，其余一切都放在代码块里：

- prop：`name: value`
- 处理函数：`on name { … }`
- 双向绑定：`bind name: target`
- 样式：`style: card`，或一个列表 `style: [card, pink]`
- 子元素

```asuka
MessageRow(m) {
  compact: true
  accessory: Badge(3)
  on archive { mailbox.archive(m.id) }
}
```

每样东西都只有一个位置：头部参数不能在代码块里设置，prop 也不能放在圆括号里传。

接收子元素的组件要声明 `prop children: View`，调用方代码块里的内容就会放到这里。具名插槽在代码块里写作 `name { … }`（`toolbar { … }`）。

`emit name(args)` 触发一个事件，由调用方的 `on name(args) { }` 来处理。没有人处理的事件什么也不做；把事件名当作值读取，得到的是调用方有没有处理它（`if click { … }`：比如托盘图标，调用方不接管点击时，点击就打开它的菜单）。

只显示一个元素的组件（主体里的视图就是那一个元素，或另一个同样如此的组件）也接受样式属性：调用方给的样式落在那个元素上，并覆盖组件自己在那里设置的值。

```asuka
component Card(title: string) {
  Column {
    padding: 16; radius: 12; background: .secondaryBackground
    Text(title)
  }
}

Card("Plan") { padding: 8; width: 240 }   // padding 为 8，radius 为 12
```

## 组件的主体是声明

组件的主体**不是**运行一次就被抛到脑后的代码。主体里的每一行都声明了一件始终成立的事：

- `state x = …` 是组件自己拥有的状态。
- `let y = …` 由它读取的内容派生而来，并会一直保持最新（只在被读取时才计算，而且只有当它读过的内容变化后才会再次计算）。
- 每个元素的每个 prop 值都以同样的方式保持最新：`count` 变化时，`Text("Count: {count}")` 跟着变化，其他东西都不会变。
- `if`、`for` 和 `match` 会按照条件显示对应的内容，现在如此，之后也一样。

`count` 变化时，重新运行的只有读取了它的部分：那个 `Text` 的文字，以及那个 `if`。没有任何组件会重新运行，平台的视图也只在确实有变化的地方才会被改动。

## 处理函数与语句

代码在这些地方运行：处理函数（`on press { … }`）、函数、`task` 和 `effect` 代码块，以及测试。在这些地方读取状态，得到的是它的当前值；写入状态，读取它的地方就会更新：

```asuka
on press {
  count += 1
  todos.push(Todo(title: draft))
  todos[0].done = true      // 写入 todos：只有读取了 todos[0] 的行会更新
  draft = ""
}
```

一个处理函数里的多次写入，会在它结束时（或者执行到 `await` 时）一起显示出来。

语句有：`let`、`var`、`let … else`、赋值（`=`、`+= -= *= /= %=`、`&= |= ^= <<= >>=`、`??=`）、`if` 和 `if let`、`for x in xs`（数组或集合）、`for (k, v) in m`、`for (i, x) in xs.enumerated()`、`while` 和 `while let`、`match`、`return`、`break`、`continue`、`throw`、`do { } catch { }`、`defer { }`、`emit`（见[控制流](control-flow.md#控制流)），以及 `fn` 声明（见[函数与闭包](functions.md#函数与闭包)）。

## 双向绑定

控件既要显示一个值，也要修改它。`bind` 把状态交给控件，两件事就都能做：

```asuka
state query = ""
state notify = true

TextField { bind text: query; placeholder: "Search" }
Toggle { bind on: notify; title: "Notifications" }
```

不用 `bind` 时，控件只负责报告变化：`TextField { text: name; on change(t) { name = t.trim() } }`。

组件的 prop 也可以这样绑定，只要组件用 `event <prop>Change` 报告它：一个有 `prop value` 和 `event valueChange(value: T)` 的组件，写 `bind value: when` 就会把状态传进去，并在组件发出变化时写回：

```asuka
component Rating {
  prop value = 0
  event valueChange(value: int)

  Stepper {
    value: float(value)
    on change(v) { emit valueChange(int(v)) }
  }
}

component Review {
  state stars = 3

  Rating { bind value: stars }
}
```

## 视图列表

```asuka
for todo in todos key todo.id {
  TodoRow(todo) { on delete { todos.removeWhere(t => t.id == todo.id) } }
}
```

`for` 为每个键保留一行。某一项变化时，对应的行只更新它自己的绑定。新增一项会创建一行，删除一项会销毁它的那一行。移动了位置的行只会被移动，不会重建。直接放在 `List` 下面时，行只在显示在屏幕上时才存在，所以一个有 100,000 项的列表，开销也只是屏幕上可见的那些。

## Effect 与 task

```asuka
effect { storage.set("draft", draft) }        // 立即运行，之后 draft 每次变化时再运行
task { try? await mailbox.refresh() }         // 组件显示期间运行一次
task(query) { results = await search(query) } // query 变化时再运行一次
```

effect 会立即运行，之后每当它读取过的内容变化时再次运行。task 是异步的（见下文）：组件出现时启动；圆括号里的内容变化时重新启动（并取消上一次运行）；组件消失时被取消。`app` 代码块里也可以写它们：它们属于应用本身，在应用运行期间一直有效（比如一个刷新循环，或者一个跟随应用状态的设置：`effect { setColorScheme(scheme) }`）。
