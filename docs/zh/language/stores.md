# Store

store 是状态加上修改它的函数，由使用它的视图共享：

```asuka
pub store Mailbox {
  state messages: [Message] = []
  let unread = messages.count(m => !m.read)

  pub fn markRead(id: string) {
    if let i = messages.index(m => m.id == id) { messages[i].read = true }
  }

  pub async fn refresh() throws {
    messages = try await http.get("https://example.com/inbox").json()
  }
}
```

store 的创建方式和结构体一样（`Mailbox()`），它属于创建它的那个组件或 store。`provide` 让它对下面的所有内容可用，`use` 用来取到它：

```asuka
app {
  provide Mailbox()
  Stack { Inbox() }
}

screen Inbox {
  use mailbox: Mailbox
  …
}
```

组件提供的东西只属于它显示的视图：两个并排的组件各自提供一个 `Theme`，它们看到的都是自己的那个，而不是对方的。

用可选类型 `use` 时，如果上面没有谁提供，取到的就是 none。这适合既能单独使用、也能放在一组里使用的组件，比如复选框，可以放在复选框组里，也可以不放。

```asuka
store Group {
  pub state chosen: [string] = []
}

component Choice(value: string) {
  use group: Group?

  Text(if let g = group { if g.chosen.contains(value) { "✓ {value}" } else { value } } else { value })
}
```

写在文件顶层的 `state` 属于整个应用：全应用只有一份。

store 和结构体一样可以带类型参数，由参数或期望的类型推断出来：

```asuka
store Kept<T>(key: string, initial: T) {
  pub state value: T = initial
}

let size = Kept("size", 17.0)                 // 一个 Kept<float>
let tags: Kept<[string]> = Kept("tags", [])
```

在 store 的 fn 里，`self` 就是这个 store 本身，可以把它交给它创建的东西。
