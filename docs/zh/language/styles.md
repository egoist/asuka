# 样式

每个元素都接收样式 prop：先是布局（`direction`、`justify`、`align`、`gap`、`padding`、`margin`、`width`、`height`、`grow`、`shrink`、`absolute`、`top`、`grid` ……），然后是外观（`background`、`radius`、`border`、`opacity`、`shadow`、`font`、`color`、`lines` ……）。详见[布局与样式](../layout.md)。

`style` 声明一组可以复用的 prop，用 `style:` 应用一个样式，或者一个样式列表：

```asuka
style card {
  padding: 16; radius: 12; gap: 8
  background: .secondaryBackground
}

style pink { background: #ff2d55 }

View { style: [card, pink]; opacity: 0.9 }
```

两个条目设置了同一个 prop 时，后面的优先；块里自己写的 prop 又优先于整个列表，不管 `style:` 写在块的什么位置。样式也用同样的写法应用别的样式，它自己的 prop 优先于被应用的样式：

```asuka
style largeCard { style: card; padding: 24 }
```

样式可以像 `fn` 一样接收参数（可以有默认值，也可以按名字传）。参数在用到它的地方是一个绑定：`chip(tint)` 会跟随 `tint` 变化，而且只有读到它的 prop 会更新。

```asuka
style chip(tint: Color, size: float = 13) {
  padding: [4, 10]; radius: 999
  background: tint.opacity(0.15); color: tint; font: Font(size: size)
}

Text(tag) { style: chip(.green) }
Text(count) { style: [card, chip(tint, size: 15)] }
```

列表的条目也可以是 `if` 或 `match`：它选中的分支里的样式生效；换到另一个分支时，那些样式设置过的每个 prop 都会退回到列表中排在它前面的条目所给的值（没有的话就是平台默认值）。在这里，`if` 不需要 `else`；`match` 不必覆盖所有情况，也不能绑定名字，分支写 `[]` 表示什么都不应用。

```asuka
View {
  style: [
    card,
    if selected { chip(.accent) },
    if m.urgent { urgent } else if m.read { read },
    match m.kind { .note => note, .task => [task, bold], _ => [] },
  ]
}
```

样式里可以写样式 prop、`style:`、变体（见下文）和 `use`。`pub style` 也能在别的包里使用：写作 `style: theme.card`，其中的值在声明它的包里求值。样式不能应用它自己，经过别的样式间接应用也不行。

不设置任何样式的元素，看起来就和平台自己的一样。

**主题。** 样式的值可以读取所在包的 `let` 和 `state`，所以一个设计令牌（token）就是其中之一（`pub let hairline = 2.0`）；如果是 `state`，改了它，所有读到它的地方都会跟着变。样式还可以 `use` 上层组件提供的 store，在应用样式的地方查找：应用里提供了自己那份的部分，就有自己的一套令牌。

```asuka check
pub store Theme(hairline: float = 2) {
  pub state border = hairline
}

pub style framed { use t: Theme; border: t.border; borderColor: .separator }

app {
  provide Theme()
  Stack { Settings() }
}

component Settings {
  provide Theme(hairline: 1)   // 这里面细一些
  Column { style: framed }
}
```

**默认样式。** 以元素（或接收样式 prop 的组件）命名的样式，就是它的默认样式：本包里写的每个该类元素都会应用它，位于 `style:` 列表之下。别的包里的元素，以及别的包的组件内部的元素，保持它们自己的样子。

```asuka check
style Button { radius: 8 }
style View { borderColor: .separator }
```

容器（`View`、`Row`、`Column`、`SafeArea`）可以写**变体**：以交互状态命名的一组样式 prop，在容器处于该状态时叠加到它的样式上，相当于 CSS 的 `:hover` 和 `:active`。状态有四种：`hovered`、`pressed`、`dragging`（正被拖动时）和 `dragOver`（它接受的拖动正停在它上方时），按这个顺序叠加，所以 `pressed` 会盖过 `hovered`。变体只覆盖它写出的 prop，它的值和普通 prop 一样是绑定：

```asuka
View {
  padding: 16; radius: 12; background: .secondaryBackground
  hovered { borderColor: .accent }
  pressed { background: .separator; opacity: 0.8 }
  on press { open() }
}
```

只有当某处用到了某个状态，视图才会上报它。因此没有写 `pressed` 变体的视图，仍然保留平台自己的按下效果。变体里也可以写 `style:`（`pressed { style: raised }`），它位于变体自己的 prop 之下；样式里也可以写变体，应用到容器上时生效（其他元素会忽略）：

```asuka check
style button {
  padding: [8, 16]; radius: 8; background: .accent
  pressed { opacity: 0.7 }
}
```

组件还可以声明自己的变体（`variant checked = checked`，见[组件](components.md#组件声明的变体)），样式里可以像写交互状态那样写它们的代码块：应用到一个组件声明了该变体的元素上时，代码块在变体成立期间叠加上去，位于交互状态之上；用在其他元素上则什么都不做。名字拼错会报错：样式只能写本包组件的变体，以及其他包里 `pub` 的变体。

```asuka
// 用于声明了 selected 变体的组件（例如列表框的选项）。
style option {
  padding: [6, 10]; radius: 6
  pressed { opacity: 0.8 }
  selected { background: .accent; color: .onAccent }
}
```

组件的变体位于调用方的 prop 之上：调用方设置的 `background` 会替换组件自己的，而组件的 `pressed` 依然会让它变暗。其他视图可以通过名字跟随一个视图的状态（见[给视图命名](#给视图命名)）：写成 `View as card { … }`，然后在任意绑定里读 `card.hovered`（或 `card.pressed`、`card.dragging`、`card.dragOver`），子视图可以这样做（整组悬停），兄弟视图也可以。与样式无关的反应则交给处理器：`on pressedChange(p)`、`on hoverChange(h)`。

## 给视图命名

有些事不是给视图设置一个值，而是要对视图做点什么：播放视频、在网页里运行脚本、往输入框里插入文本、滚动列表。`Ref` 就是视图的句柄：在任意元素上写 `ref: r`，它的视图就放进 `r`；写成 `Element as name`，则会为所在代码块的剩余部分声明一个这样的句柄（写在只显示一个元素的组件上时，指的是那个元素的视图）。

```asuka check
component Player {
  let video = Ref()

  Video("intro.mp4") { ref: video }
  Button("Start over") {
    on press {
      video.seek(0)
      video.play()
    }
  }
}
```

只有视图显示期间，Ref 才持有它；在那之前和之后都是空的，这时命令什么也不做，状态都是 false。Ref 的状态会被追踪，读取它的绑定会跟着它变化：

| 适用于 | Ref 的成员 |
|---|---|
| 任意视图 | `hovered`、`pressed`、`dragging`、`dragOver`（bool，会被追踪）；`focus()`、`blur()` |
| 组件的（`Checkbox as box`） | 它的变体：`box.checked`（bool，会被追踪） |
| `Scroll`、`List` | `scrollTo(x, y, animated: true)` |
| `Video` | `play()`、`pause()`、`seek(seconds)` |
| `WebView` | `try await eval(script)`（脚本的值，以 JSON 表示）、`run(script)`（不等待结果）、`goBack()`、`goForward()`、`reload()`、`stopLoading()` |
| `TextField` | `insert(text)`、`format(.bold, on)`、`toggleFormat(.italic)`、`setLink(url)`（作用于富文本输入框的选区） |

Ref 可以传来传去（`device.shareFiles(paths, from: button)`、`accessibility.focus(field)`），也可以放在文件顶层的 `let` 里，供多个组件的视图使用。
