# 异步

需要等待的工作（网络、文件、定时器）用 `async` 表示，等待它则用 `await`：

```asuka
async fn loadThread(id: string) throws -> Thread {
  let (messages, people) = try await all(api.messages(id), api.people(id))
  let html = await spawn { renderMarkdown(messages) }
  return Thread(messages, people, html)
}
```

- 调用异步函数需要 `await`。处理函数、task 和测试里可以使用 `await`；组件的主体及其中的 `let` 不行。闭包被传作异步函数，或者主体里有 `await` 时，就是异步的；写成 `async () => …` 可以明确标出（[函数与闭包](functions.md)）。
- **主线程。** 所有代码都在主线程上运行；`await` 让你的代码在等待时，其他工作可以先运行。状态可以在主线程上修改，而且只能在主线程上修改。
- `all(a, b)` 同时发起多个调用，并等待全部结果；`race(a, b)` 取第一个完成的结果，并取消其余的；此外还有 `sleep(300ms)` 和 `withTimeout(5s) { … }`，后者的值就是代码块的值（`let page = try await withTimeout(5s) { return try await fetch(url) }`），超时就抛出错误。它们是结构化的：被它们取消的工作会先停下、执行完自己的 `defer`，它们才返回或抛出错误。`await never()` 什么都不等，只等任务结束：适合只在读者离开时才结束的流，或者先要 await 才能持有某样东西的 task（`try await register(); defer { release() }; await never()`）。组件不需要 await 就能持有的东西，用 `cleanup { }` 撤销（见 [cleanup](components.md#cleanup)）。
- `spawn { … }` 在另一个线程上运行一段代码，用于会拖住主线程的工作（解析、图片处理）。这段代码只能使用值，不能使用状态、store、视图、原生对象或 C 指针，也不能使用保存着它们的文件级 `let`（比如一个数据库）：如果用了，编译器会报错。在 Web 上，它运行在页面的一个 Web Worker 里；Worker 只会自己生成文件里的值，不会生成其他东西。如果工作需要在多次调用之间保留某样东西（一个索引、一个它打开的数据库），就用 [worker](#worker)。
- `for await message in socket.messages { … }` 读取一个流，也就是 `Stream<T>`：随时间陆续到来的值。`Stream(send => { … })` 创建一个流：流第一次被读取时，这个代码块作为 task 开始运行，每次 `send(x)` 就是一个值；代码块返回时流结束，代码块抛出错误时流也以这个错误失败。离开循环（`break`、出错、owner 被销毁）会取消这个代码块。一个流只能有一个读取者。

  ```asuka
  fn levels() -> Stream<float> = Stream(send => {
    try await battery.watch(changed: level => send(level))
  })
  ```
- `live(initial, source)` 是一个由流保持最新的值，以读取它的函数的形式出现：在绑定中读取时，从第一次这样的读取开始跟随 `source()` 的流，直到最后一个读取者离开（随后流被取消：不会在没人需要时还监听平台）；在别处读取时，得到的是最后已知的值。包就是这样提供平台会变化的东西的，比如网络状态：

  ```asuka
  let level = live(1.0, levels)
  pub fn batteryLevel() -> float = level()
  ```
- **取消。** task 属于启动它的那个东西（组件事件的处理函数则属于声明它的组件，见[组件](components.md)）。当那个东西消失时（比如用户离开了页面），task 就会被取消：它正在等待的那个 `await` 会结束它，它的 `defer` 会运行，并且不会报告任何错误。

## Worker

`spawn` 在另一个线程上把一段代码运行一次。`worker` 则让一个值一直留在那里：它是一种类型，每个实例都运行在自己的线程上，带着只属于自己的 `var`。

```asuka check
worker SearchIndex {
  var words: [string: [int]] = [:]

  pub fn add(id: int, text: string) {
    for w in text.split(" ") {
      let k = w.lower()
      words[k] = (words[k] ?? []) + [id]
    }
  }
  pub fn find(q: string) -> [int] = words[q.lower()] ?? []
}

store Notes {
  let index = SearchIndex()
  state results: [int] = []

  pub async fn add(id: int, text: string) { await index.add(id, text) }
  pub async fn search(q: string) { results = await index.find(q) }
}
```

- worker 的 `var`、`let` 和参数（`worker Index(language: string)`）都是它自己的：只有它的 fn 会在它的线程上读取和修改它们。worker 没有 `state`（视图不会读它），也没有 props、事件和视图。
- 从外部调用时，它的 `pub fn` 是异步的：`await index.find(q)`。参数以值的形式传过去，调用按发出的顺序一个接一个地运行，结果（或者 `throws` 函数抛出的错误）也以值的形式传回来。在 worker 内部，它们就是普通函数，不会 `await`；其他 fn 只供 worker 自己使用。
- 能传过去的东西和 `spawn` 一样：只能是值，不能是状态、store、视图、闭包或原生对象；worker 的 fn 也不能读取主线程的状态。worker 可以自己创建并保存 C 句柄（一个 [extern type](../c.md#c-持有的类型)，比如它打开的数据库）：这个句柄永远不会离开 worker，也在那里释放。
- worker 属于创建它的组件或 store。组件或 store 消失时，worker 就会停止：正在运行的调用会执行完，还在排队的调用会被取消；调用方的 task 看到的是自己被取消，而不是一个错误。没有 owner 的 worker 会在不再被引用、并且交给它的调用都执行完之后停止。
- 在 Web 上，每个 worker 都是一个独立的 Web Worker。

## 需要加载的数据

`load` 把一个异步结果保存在组件里：

```asuka
screen Profile(id: string) {
  let user = load api.user(id)   // id 变化时重新运行

  match user {
    .pending => Spinner()
    .failed(e) => ErrorView(e) { on retry { user.reload() } }
    .ready(u) => ProfileCard(u) { opacity: if user.loading { 0.6 } else { 1 } }
  }
}
```

`Loading<T>` 的值是 `.pending`、`.ready(value)` 或 `.failed(error)` 之一，而且只有在第一次得到结果之前才是 `.pending`。当它的参数所读取的内容变化，或者 `.reload()` 让它再运行一次时，它会保持当前所处的情况，直到新的结果到来：`ProfileCard` 留在屏幕上，它的状态和滚动位置也都还在；那次即将过时的运行则会被取消。如果新结果和上一次相等，什么都不会变。

- `.value` 是最近一次得到的值，第一次之前为 none。某次运行失败时它也会保留，所以 `.failed` 可以在错误旁边继续显示旧内容。
- `.loading` 在有一次运行正在进行时为 true，第一次也算在内：可以用它把内容调暗，或者在角落里显示一个转圈。读取它不会跟随值的变化，`match` 也不会跟随它。

```asuka
let results = load api.search(query)

TextField { bind text: query; placeholder: "Search" }
if results.loading { ProgressRing() }
match results {
  .pending => Spinner()
  .failed(e) => Text(e.message)
  .ready(items) => List { for r in items key r.id { ResultRow(r) } }
}
```

用户输入时，列表会一直保留上一次的结果、滚动位置和各行的状态，直到新结果到来；转圈则表示有一次搜索正在进行。其他读取 `query` 的地方会立刻显示新的文字。

如果想从头来过（比如切换到另一个人时，不应该在加载期间还显示上一个人的资料），就在 `match` 的位置读取 `.loading`：

```asuka
if user.loading {
  Spinner()
} else {
  match user {
    .pending => Spinner()
    .failed(e) => ErrorView(e) { on retry { user.reload() } }
    .ready(u) => ProfileCard(u)
  }
}
```

不过更常见的情况是，内容本来就各自独立：每个 id 推入一个页面，每个 key 一行，各自有自己的 `load`。
