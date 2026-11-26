# 核心概念

一个 Asuka 应用由两样东西构成：声明要显示什么的组件，以及说明什么会变化的状态。本页介绍它们如何配合：应用本身、组件及其状态、状态变化时哪些部分会更新、effect 与 task、条件显示与列表渲染、owner 与清理、主线程，以及环境信息。语法的完整说明见 [Asuka 语言](language-tour.md)。

## 应用

`app { … }` 是应用的根：

```asuka
component Greeting(name: string) {
  Text("Hello, {name}")
}

app {
  SafeArea {
    padding: 16
    gap: 12
    Text("Hello") { font: .largeTitle }
    Greeting("Ada")
  }
}
```

在手机上，根会铺满整个屏幕，一直延伸到刘海和底部的 Home 指示条下方；在桌面端，它会铺满整个窗口。`SafeArea` 能让内容避开遮挡它的东西，包括键盘（见[布局与样式](layout.md#安全区域键盘与窗口)）。

应用的启动方式因平台而异：

- **iOS 和 Android**：由平台启动应用，应用再挂载根。
- **macOS、Windows 和 Linux**：应用打开主窗口，把根挂载进去，一直运行到应用退出。
- **Web**：页面加载应用，由应用把根挂载到页面中。

在 `asuka dev` 下，每次保存都能在几毫秒内到达正在运行的应用，应用的状态和视图也都会保留下来。

## 组件

组件是界面中的一块。参数在头部声明；props、事件、状态以及视图都写在主体里：

```asuka
component Counter(label: string) {
  prop step = 1
  event changed(count: int)
  state count = 0

  Row {
    gap: 12
    Text("{label}: {count}")
    Button("Add") {
      on press {
        count += step
        emit changed(count)
      }
    }
  }
}
```

组件的主体是**声明**，而不是运行一次就被抛到脑后的代码。`Text("{label}: {count}")` 说明的是这段文字现在是什么、以后也是什么。因此你不可能一不小心只读取了一次 `count`，结果显示出一个永远不会更新的值。

## 状态

`state` 是界面会跟随的值：

```asuka
state count = 0
state draft = ""
state todos: [Todo] = []
```

写入它（`count += 1`、`todos.push(t)`、`todos[0].done = true`）会更新读取了它的地方，而且只更新这些地方。写入一个与当前值相同的值，什么也不会发生。

如果状态的值读取了会变化的内容，状态就会跟随它：`state draft = title` 在被写入之前就是 `title`，`title` 变化后又会回到 `title`。`state draft = peek(title)` 只读一次 `title`；`state page(query) = 1` 每当 `query` 变化时回到 1（见[跟随的状态](language/components.md#跟随的状态)）。

组件里的状态属于这个组件：每个实例都有自己的一份，并随实例一起消失。写在文件顶层的 `state` 属于整个应用：全应用只有一份。**store** 把状态和修改它的函数组织在一起，供多个视图共享：

```asuka
pub store Cart {
  state items: [Item] = []
  let total = items.sum(i => i.price)

  pub fn add(i: Item) { items.push(i) }
}
```

### 派生值

在组件、store 或文件中，`let` 由其他值派生出一个值：

```asuka
state first = "Ada"
state last = "Lovelace"
let full = "{first} {last}"
let left = todos.count(t => !t.done)
```

它在被读取时才计算，之后只有当它读过的内容发生变化时才会重新计算；读取两次，也只计算一次。它读取的就是代码里写的那些部分：`todos.count(t => !t.done)` 读取每一项的 `done` 和总项数，所以给某一项改名不会让 `left` 重新计算。`let` 不能写入状态。

## 哪些部分会更新

每个读取了状态的 prop 都是一个独立的绑定。当它读过的状态变化时，这个绑定会重新运行，并把这一个 prop 发送给原生视图；其他任何东西都不会运行。

```asuka
component Score {
  state score = 0

  Column {
    Text("Score: {score}")          // score 变化时更新
    Text("Keep going")              // 只发送一次
    Button("Point") { on press { score += 1 } }
  }
}
```

按下 *Point* 会修改 `score`。第一个 `Text` 的绑定重新运行，发送新的文字。组件不会重新运行，第二个 `Text` 不会被碰到，也没有需要比较的树：变化直接从状态传到读取它的那一个 prop。

绑定读取了状态的哪一部分，就只跟随那一部分。`Text(user.name)` 跟随的是用户的名字，修改 `user.avatar` 不会让它重新运行。`todos[1].done = true` 只会让读取这个 `done` 的地方（以及读取整个列表的地方）重新运行，读取其他项、或者这一项标题的地方都不受影响；`todos.count()` 跟随的是项数，`cart[sku]` 跟随那个键，`picked.contains(id)` 跟随那个成员。整体读取的值（传给函数、参与比较）会被整体跟随。整体替换一个值（`todos = fetched`）时，只有读取了真正变化部分的地方才会重新运行。

一个处理函数里的多次写入，会在它结束时（或者执行到 `await` 时）一起显示出来：三次写入，只更新一次。

## Effect 与 task

`effect` 会立即运行一段代码，之后每当它读取过的内容变化时再次运行；如果在圆括号里写了值，就只在这些值变化时再次运行：

```asuka
effect { storage.set("draft", draft) }
effect(selected) { analytics.log("selected", selected) }
effect() { analytics.log("opened") }   // 只在组件建立时运行一次
```

effect 不能写入它跟随的状态（否则它会在自己写入之后再运行一次）：编译器会报错，并给出替代的写法。

effect 建立的东西，由它主体里的 `cleanup { … }` 撤销。cleanup 会在 effect 再次运行之前执行，也会在 effect 随组件一起消失时执行：

```asuka
effect(roomId) {
  let room = roomId
  chat.join(room)
  cleanup { chat.leave(room) }
}
```

cleanup 读取状态时，读到的是它运行那一刻的值，那时 effect 跟随的东西已经变了。所以要把这次运行用到的值存进 `let`，就像这里的 `room`；cleanup 读取它所在 effect 跟随的东西时，编译器会给出警告。

`task` 是组件在显示期间进行的异步工作：组件出现时启动，组件消失时被取消。如果在圆括号里写了值，这些值变化时它会重新启动，并取消上一次运行。圆括号里的值会被比较：`task(user.id)` 在 id 变化时才重新启动，用户改名时不会。

```asuka
task { messages = try await api.inbox() }
task(query) {
  await sleep(300ms)
  results = try await api.search(query)
}
```

`load` 把一个异步结果连同它的错误、以及是否正在加载一起保存为状态。一旦有了结果，下一次加载期间这个结果会继续留在屏幕上（见 [Asuka 语言](language/async.md#需要加载的数据)）。

## 条件显示与列表渲染

`if` 在条件成立时显示视图；`match` 从多种情况中显示一种；`for … key` 为每一项显示一行：

```asuka
if loggedIn {
  Profile(user)
} else {
  Button("Log in") { on press { present(Login()) } }
}

match status {
  .loading => Spinner()
  .failed(e) => Text(e.message)
  .ready(items) => ItemList(items)
}

for todo in todos key todo.id {
  TodoRow(todo)
}
```

`for` 为每个键保留一行。某一项变化时，对应的行只更新它自己的绑定；新增一项会创建一行，删除一项会销毁它的那一行；移动了位置的行只会被移动，不会重建。遍历会变化的内容时，`for` 需要写 `key`；范围、集合和常量列表则按它们本身的值作为行的键。

遍历状态的 `for` 跟随的是各项的顺序，而不是各项的内容：修改某一项的某个字段，既不会让 `for` 重新运行，也不会影响其他行，只有这一行里读取了该字段的绑定会重新运行。行里的项传递下去之后（`TodoRow(todo)`）仍然指向这一项：`TodoRow` 里的 `todo.title` 跟随的就是这一项的标题。用项的某个字段作为键（`key todo.id`）开销最小：`for` 会把所有的键作为一个整体来跟随。

### 出错时

处理函数或 task 没有捕获的错误，会交给包住它的最近的 `Boundary`。`Boundary` 会改为显示它为这个错误给出的内容，而不是原来的内容：

```asuka
Boundary {
  Feed()
  on error(e) { Text("Something went wrong: {e.message}") }
}
```

如果没有 `Boundary`，就由应用自己报告这个错误：开发时开发 App 会把它显示出来，发布版本中则记录到日志。

### 长列表

直接放在 `List` 里的 `for` 只为屏幕上可见的行构建视图，并在滚动时复用它们，所以一个有 100,000 项的列表，开销也只是屏幕上可见的那些。详见[组件](components.md#列表)。

## owner 与清理

组件创建的一切（它的状态、绑定、effect 和 task）都属于一个 **owner**（所有者），也就是这个组件实例的 owner。每个 `if` 分支、`for` 行、`match` 情况、导航栈中的页面、标签页和弹出面板，也都在父级 owner 之下拥有自己的 owner。销毁一个 owner 时，会停止它的绑定和 effect，取消它的 task，并销毁它下面的所有 owner。

以下情况会销毁 owner：

- `if` 切换分支时：旧分支的 owner；
- `for` 中某一项的键离开数组时：该行的 owner；
- `match` 切换情况时：旧情况的 owner；
- 页面被弹出时（返回按钮、滑动手势、`pop()`）：该页面的 owner；
- 弹出面板关闭时：其内容的 owner。

组件主体里的 `cleanup { … }` 会在它的 owner 被销毁时运行；effect 主体里的 cleanup 也会在这时运行，此外还会在 effect 每次再次运行之前运行。store 和 app 的主体里也可以写 cleanup。多个 cleanup 按注册的相反顺序运行，并且在它们的 owner 创建的东西都停下之后才运行：先是 effect 的 cleanup，再是主体的。effect 的 cleanup 撤销这次运行注册的东西，主体的 cleanup 则告诉别人组件已经消失：

```asuka
component Tab(value: string) {
  use tabs: TabsContext
  effect(value) {
    let v = value
    tabs.add(v)
    cleanup { tabs.remove(v) }
  }
  cleanup { tabs.closed(value) }
  Text(value)
}
```

被取消的 task 会在它正在等待的那个 `await` 处结束，并在结束途中运行它的 `defer`：这里就是停止它所启动的东西的地方。

```asuka
component Clock {
  state now = time.now()

  task {
    let ticker = time.ticker(1s)
    defer { ticker.stop() }
    for await t in ticker.ticks { now = t }
  }

  Text(now.format("HH:mm:ss")) { font: .title.semibold() }
}
```

用 `if showClock { Clock() }` 显示它时，时钟被隐藏后 ticker 就会停止，再次显示时则会启动一个新的 ticker。

## 主线程

组件、处理函数、effect、task 以及所有对状态的写入，都运行在平台的主线程上：在 iOS 和 Android 上是 UI 线程；在 macOS、Windows 和 Linux 上是进程的第一个线程；在浏览器中是页面所在的线程。不要拖住主线程：主线程忙碌时，应用无法绘制、滚动或响应输入。

需要等待的工作（网络、文件、定时器）用 `async` 表示：`await` 让主线程在等待期间可以继续运行别的工作，而 `await` 之后的代码会回到主线程上运行，所以可以写入状态。

```asuka
on press {
  loading = true
  defer { loading = false }
  do {
    let page = try await http.get("https://example.com").text()
    status = "{page.count()} characters"
  } catch e {
    status = e.message
  }
}
```

需要长时间计算的工作（解析、图片处理）放进 `spawn { … }`，它会在另一个线程上运行。这段代码只能使用值，不能使用状态或视图（如果用了，编译器会报错）；它的结果通过 `await` 回到主线程：

```asuka
let html = await spawn { markdown.render(source) }
```

## 环境

`environment()` 是平台提供的、关于组件显示环境的信息：在绑定或 `let` 中读取它，它就会随屏幕旋转、键盘、深色模式和文字大小的变化而更新。它反映的是组件所在页面的环境（导航栈中的页面、标签页、弹出面板或根）。

```asuka
let landscape = environment().width > environment().height
```

它包含以下字段：

- `width`、`height`：屏幕的尺寸，单位为点。这指的是应用的窗口，而不是设备：在分屏模式下（iPad 的分屏浏览、台前调度，Android 的分屏），它是应用所占的那部分；在桌面端或浏览器中，它是窗口的内容区域，会随用户调整窗口大小而变化。
- `safeTop`、`safeRight`、`safeBottom`、`safeLeft`：安全区域的边距。桌面窗口没有安全区域，它们都是 0。
- `keyboard`：屏幕键盘遮住了底部多少高度。
- `scale`：每个点对应的像素数：手机上是 2 或 3，Retina 屏的 Mac 上是 2，Windows 和 Linux 上是显示器的缩放比例（150% 时为 1.5）。
- `fontScale`：用户设置的文字大小（iOS 的动态字体、Android 的字体缩放、Windows 的文本大小、GNOME 的文本缩放）；1 是默认值。
- `dark`：是否开启了深色模式。
- `reduceMotion`：用户是否要求减少动态效果。见[动效](motion.md)。
- `screenReader`：VoiceOver、TalkBack、讲述人或 Orca 是否已开启。见[无障碍](accessibility.md#屏幕阅读器是否开启)。
- `widthClass`、`heightClass`：窗口在每个方向上给应用留出了多少空间，按平台的分类方式表示为 `.compact`、`.medium` 或 `.expanded`。如果要决定是否在内容旁边放一个侧边栏，或者在列表旁边显示详情，应该根据它们来判断，而不是自己定一个宽度阈值。iOS 上报的是 UIKit 的尺寸类别，只有 compact 和 regular 两种（regular 对应 expanded）；Android 上报的是 Material 的窗口尺寸类别。macOS、Windows、Linux 和 Web 会根据窗口尺寸套用 Material 的划分。
- `locale`、`languages`、`timeZone`：用户的区域格式、用户能阅读的语言，以及用户的时区。大多数应用通过[文案目录和格式化函数](i18n.md)间接使用它们。
- `platform`：应用所运行的系统：`.iOS`、`.android`、`.macOS`、`.windows`、`.linux` 或 `.web`。
- `tv`：应用是否运行在电视上（Apple TV，此时 `platform` 是 `.iOS`；以及 Android TV），由遥控器移动焦点来操作，而不是触摸。见[电视](tv.md)。

语义颜色和字体会自动跟随深色模式和文字大小变化；它们覆盖不到的部分，再去读取环境信息。

```asuka
if environment().widthClass == .expanded {
  Row { grow: 1; Sidebar(); Detail() }
} else {
  ItemList()
}
```

平台的原生视图本身就会呈现出应有的样子；对于应用自己绘制的部分，可以根据 `platform` 做分支，好让它们遵循各平台的设计规范。比如 [`examples/usage`](../../examples/usage) 在 iOS 上绘制 UIKit 风格的嵌入式分组列表，在 Android 上则绘制 Material 3 风格的列表。

## 持久化的值

状态只在应用运行期间存在。store 模块（`import std/store`）可以在多次启动之间保留设置之类的小数据：`read` 读出保存的值（没有的话就是默认值），`write` 把值保存下来。用一个 effect 就能把状态的每一个值都保存下来：

```asuka
import std/store

state units = store.read("units", "metric")
effect { store.write("units", units) }
```

值以 JSON 的形式保存在平台自己的偏好设置里，所以数字、字符串、数组和结构体都可以。关于 store 模块和数据库，详见[数据](data.md)。在 `asuka dev` 下，所有状态的值也都会在每次保存后保留，不需要写任何代码（见[快速上手](getting-started.md#运行)）。
