# 组件

组件是界面中的一块，参数在头部声明，其余所有内容都写在主体里：

```asuka
component MessageRow(message: Message) {
  prop compact = false            // 一个具名 prop，带默认值
  prop accessory: View? = none    // 一个插槽：由调用方传入的视图
  event archive                   // 它对外报告的事件
  state expanded = false          // 它自己的状态

  Menu {
    longPress: true               // 上下文菜单：长按或右键点击
    actions: [.button(title: "Archive", role: .destructive, run: () => { emit archive })]
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

会 `await` 的处理函数属于声明它的组件，也就是接收事件的那个组件。它会一直运行，直到执行完毕或那个组件被移除为止；不管事件是谁触发的，也不管触发方后来怎样。比如弹出页的 `on done { shown = false; try await save(draft) }` 会先关掉弹出页，再继续保存。它在声明它的位置运行：`push` 推入的是那里的导航栈，它抛出而没有处理的错误交给那里外层的 `Boundary`。在 async 函数里，`emit` 会等处理函数执行完，并拿到它抛出的错误；在其他地方（不 `await` 的处理函数、普通的 `fn`），`emit` 只是启动它，然后继续往下执行。取消正在等待的代码，只会停止等待，不会停止处理函数。

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

主体是一个基于 `@platform`（或 `@debug`）、带 `else` 的 `if`，并且每个分支都是一个元素的组件也一样：每次构建只编译其中一个分支，调用方的样式就落在它上面。

```asuka
component Player(source: string) {
  if @platform == .windows || @platform == .linux {
    CanvasPlayer(source)
  } else {
    NativePlayer(source)
  }
}

Player("intro.json") { width: 200 }
```

## 组件声明的变体

只显示一个元素的组件，可以给自己的状态起名字，交给调用方去设置样式：`variant name = condition`，一个像 `let` 一样随时保持最新的 bool。调用方在代码块里写 `name { … }`，条件成立时，里面的样式属性就叠加到那个元素上，就像 `View` 被按下时叠加 `pressed { … }` 一样：

```asuka
pub component Checkbox {
  prop checked = false
  event checkedChange(checked: bool)
  prop disabled = false
  prop children: View

  pub variant checked = checked
  pub variant disabled = disabled

  View {
    role: .button
    on press { if !disabled { checked = !checked } }
    children
  }
}

Checkbox {
  bind checked: agree
  padding: 4; radius: 4; border: 1
  checked { background: .accent; borderColor: .accent }
  disabled { opacity: 0.4 }
  Text("I agree")
}
```

- 变体的名字自成一个命名空间：可以和某个 prop 或 state 同名，比如这里的 `checked`。但不能用交互状态的名字（`pressed`），也不能用关键字。
- 变体叠加在元素上所有其他样式之上：元素自己的样式、调用方的样式，以及交互状态。两个变体同时成立时，后声明的胜出（这里 `disabled` 压过 `checked`）。
- `pub variant` 可以由其他包的调用方设置样式；没有 `pub` 的变体只属于声明它的包。
- 样式里也可以写变体的代码块（`style box { checked { … } }`）：应用到一个声明了这个变体的组件所显示的元素上时，代码块随变体生效；用在其他地方则什么都不做。
- 当组件显示的元素是容器时，调用方在组件的代码块里也可以写交互状态（`pressed { … }`、`hovered { … }`）。
- `Checkbox as box` 给它显示的那个元素的视图起名字：`box.checked` 会读取这个变体，并被追踪，就像 `card.hovered` 读取交互状态一样。

## 组件的主体是声明

组件的主体**不是**运行一次就被抛到脑后的代码。主体里的每一行都声明了一件始终成立的事：

- `state x = …` 是组件自己拥有的状态。如果它的值读取了会变化的内容，它就会跟随那些内容（见[跟随的状态](#跟随的状态)）。
- `let y = …` 由它读取的内容派生而来，并会一直保持最新（只在被读取时才计算，而且只有当它读过的内容变化后才会再次计算）。
- 每个元素的每个 prop 值都以同样的方式保持最新：`count` 变化时，`Text("Count: {count}")` 跟着变化，其他东西都不会变。
- `if`、`for` 和 `match` 会按照条件显示对应的内容，现在如此，之后也一样。

`count` 变化时，重新运行的只有读取了它的部分：那个 `Text` 的文字，以及那个 `if`。没有任何组件会重新运行，平台的视图也只在确实有变化的地方才会被改动。

## 跟随的状态

状态的值说明它从什么开始。如果这个值读取了会变化的内容（参数、prop、其他状态、`let`），状态就会跟随它：在被写入之前，状态就是这个值；它读过的内容变化后，状态又会回到这个值。

```asuka
component Edit(title: string) {
  state draft = title          // 用户输入之前是 title；title 变化后又回到 title
  state first = peek(title)    // 只读一次 title：一个初始值

  TextField { bind text: draft }
}
```

`state x(a, b) = e` 只跟随 `a` 和 `b`：其中任何一个变化时重新计算 `e`，不管 `e` 还读了别的什么。

```asuka
let results = items.filter(i => i.name.contains(query))
state highlighted(results) = 0                  // 结果变化时回到第一项
```

在状态自己的值里，状态的名字表示它当前的值，类型是可选值（第一次为 `none`）。这样可以在值仍然成立时保留它，不成立时再替换掉；这种状态需要写明类型：

```asuka
// 选中项还在结果里时保持不变，不在时移到第一个结果。
state selected: string? = if let s = selected, results.any(r => r.id == s) { s } else { results.first()?.id }
```

如果同一个处理函数里既写入了状态，又改变了它跟随的内容，以后发生的那一个为准。它跟随的内容刚变化时就去读它，读到的已经是新值。

`peek(e)` 就是 `e`，只是读取时不跟随它；凡是会被跟随的值里都能用：`Text("{count} (from {peek(count)})")` 会在 `count` 变化时更新，而第二次读取保持原样。处理函数、task、`effect(…)` 的主体和测试本来就不跟随任何东西，所以在这些地方写 `peek` 是错误。

## 处理函数与语句

代码在这些地方运行：处理函数（`on press { … }`）、函数、`task` 和 `effect` 代码块，以及测试。在这些地方读取状态，得到的是它的当前值；写入状态，读取它的地方就会更新：

```asuka
on press {
  count += 1
  todos.push(Todo(title: draft))
  todos[0].done = true      // 写入 todos[0].done：只有读取它（或 todos[0]、整个 todos）的地方会更新
  draft = ""
}
```

一个处理函数里的多次写入，会在它结束时（或者执行到 `await` 时）一起显示出来。

语句有：`let`、`var`、`let … else`、赋值（`=`、`+= -= *= /= %=`、`&= |= ^= <<= >>=`、`&+= &-= &*=`、`??=`）、`if` 和 `if let`、`for x in xs`（数组或集合）、`for (k, v) in m`、`for (i, x) in xs.enumerated()`、`while` 和 `while let`、`match`、`return`、`break`、`continue`、`throw`、`do { } catch { }`、`defer { }`、`emit`（见[控制流](control-flow.md#控制流)），以及 `fn` 声明（见[函数与闭包](functions.md#函数与闭包)）。

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

```asuka check
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

组件也可以给这样的 prop 赋值，就像它是自己的 state 一样：在处理函数、fn、effect 和 task 里赋值都行（通过元素的 `bind` 写入也算）。赋值的效果取决于调用方：

- 调用方绑定了它（`bind open: shown`）：赋值会发出 `openChange`，由它写入调用方的状态，prop 继续读这个状态。如果调用方不写入，prop 就保持原样。
- 调用方没有绑定（传了一个值，或者什么都没传）：赋的值从此归组件自己所有；如果调用方处理了 `openChange`，仍然会收到通知。调用方传入的值一旦变化，就会替换掉组件自己的值。

```asuka
component Disclosure(title: string) {
  prop open = false
  event openChange(open: bool)
  prop children: View

  Column {
    Button(title) { on press { open = !open } }
    if open { children }
  }
}

Disclosure("Details") { Text("…") }                  // 组件自己的
Disclosure("Details") { bind open: shown; Text("…") } // 调用方的
```

prop 只能整体改变：写 `open = …` 或 `items = items + [x]`，不能写 `items.push(x)`。没有对应 change 事件的 prop 不能赋值。

## 视图列表

```asuka
for todo in todos key todo.id {
  TodoRow(todo) { on delete { todos.removeWhere(t => t.id == todo.id) } }
}
```

`for` 为每个键保留一行。某一项变化时，对应的行只更新它自己的绑定。新增一项会创建一行，删除一项会销毁它的那一行。移动了位置的行只会被移动，不会重建。直接放在 `List` 下面时，行只在显示在屏幕上时才存在，所以一个有 100,000 项的列表，开销也只是屏幕上可见的那些。

遍历会变化的内容时，`for` 要用 `key` 说明靠什么区分每一项：否则项目移动时，行的状态会留在原来的位置上。有些情况不需要 `key`：

```asuka
for i in 0..<count { Dot(i) }                      // 范围：数字就是键
for tag in tags { Chip(tag) }                      // 集合：元素就是键
for day in ["Mon", "Tue", "Wed"] { Text(day) }     // 常量：没有东西会移动
for (i, x) in xs.enumerated() key i { Slot(i, x) } // 有意按位置保留行
```

## Effect 与 task

```asuka
effect { storage.set("draft", draft) }        // 立即运行，之后 draft 每次变化时再运行
effect(selected) { log(selected, user.plan) } // 立即运行，之后只在 selected 变化时再运行
task { try? await mailbox.refresh() }         // 组件显示期间运行一次
task(query) { results = await search(query) } // query 变化时再运行一次
task(user.id) { profile = try await load(user.id) } // id 变化时才重新运行，name 变化时不会
```

effect 会立即运行，之后每当它读取过的内容（包括它调用的函数读取的内容）变化时再次运行。它读取的就是代码里写到的那部分：`effect { log(user.name) }` 在名字变化时重新运行，头像变化时不会。`effect(a, b)` 只在 `a` 或 `b` 变化时再次运行，它的主体读取的东西都不会被跟随；`effect() { }` 什么都不跟随，只在组件建立时运行一次。task 是异步的（见[异步](async.md#异步)）：组件出现时启动；圆括号里的内容变化时重新启动（并取消上一次运行）；组件消失时被取消。effect 和 task 圆括号里的内容会被比较：变化后如果值仍然相等，就什么都不会发生，所以用户改名时 `task(user.id)` 会继续运行。`app` 代码块里也可以写它们：它们属于应用本身，在应用运行期间一直有效（比如一个刷新循环，或者一个跟随应用状态的设置：`effect { appearance.setColorScheme(scheme) }`）。

effect 可以写入状态，但不能写入它跟随的状态：`effect { if n > 3 { n = 3 } }` 会在自己写入之后再运行一次，晚了一轮，而在那之前处理函数已经看到 `n` 是 4 了。编译器会报错，并给出替代的写法：跟随的状态（`state n: int = min(n ?? 0, 3)`、`state selected(results) = 0`），跟随别的内容的 `effect(…)`，或者在改变它读取内容的那个处理函数里直接写入。task 可以写入它跟随的状态：它通过取消来重新启动，最终会结束。

### cleanup

```asuka
effect() {
  let id = bus.listen("refresh", e => reload())
  cleanup { bus.unlisten(id) }          // 只运行一次：组件消失时
}
effect(roomId) {
  let room = roomId
  chat.join(room)
  cleanup { chat.leave(room) }          // 下一次运行之前，以及 effect 消失时
}
cleanup { drafts.save(draft) }          // 组件消失时
```

effect 主体里的 `cleanup { … }` 会在 effect 下一次运行之前、以及 effect 消失时运行：这次运行建立了什么，它就撤销什么。组件显示期间只需建立一次的东西，写在 `effect() { }` 里：它调用的函数可以读写它所注册的地方（比如一个监听者列表），而 effect 不会去跟随这些东西。它可以写在 `if` 里（只撤销这个分支做过的事），也可以写在循环里（执行到几次就注册几次）；有多个时，后注册的先运行。写在组件、store 或 `app` 的主体里、和 effect 与 task 并列时，它会在这个实例消失时运行，排在 effect 的 cleanup 之后。

cleanup 是一个闭包：它能看到这次运行的局部变量；它不能 await，也不能抛出错误（它在 owner 消失的过程中运行，后面的 cleanup 还得接着运行；用 `try?` 或 `do { } catch { }` 处理错误）；它读取的东西不会被跟随。它读到的状态是它运行那一刻的值。在 effect 再次运行之前，那已经是 effect 跟随的东西变化之后了，所以 `cleanup { chat.leave(roomId) }` 离开的会是新房间：要把这次运行的值存进 `let`，否则编译器会给出警告（[cleanup-reads-what-it-follows](../errors/cleanup-reads-what-it-follows.md)）。它写入的东西算作它所在 effect 的写入。

处理函数、函数、task 和测试里不能写 cleanup：task 用 `defer` 撤销它启动的东西，`defer` 会在 task 结束或被取消时运行；处理函数和函数没有自己的 owner 来运行它。
