# 测试

应用的测试就是写在代码旁边的 `test` 块。`mount` 在没有设备、窗口和显示器的情况下挂载一个组件，测试像用户一样，根据屏幕上显示的内容来操作它：

```asuka
component Counter {
  state count = 0
  Column {
    Text("Count: {count}")
    Button("Add") { on press { count += 1 } }
  }
}

test "Add counts" {
  let app = mount(Counter())
  app.tap("Add")
  app.tap("Add")
  app.expect("Count: 2")
}
```

```sh
asuka test                    # 当前目录下的应用或包
asuka test --run Add          # 名称中包含 Add 的测试
asuka test path/to/app
```

```
ok   Add counts
FAIL Remove empties the list (app.asuka:41)
     the screen doesn't show "No items"
     View 0,0 390x844
       Text 0,0 390x20 "Item 1"
asuka: 1 of 2 tests failed
```

只要能运行命令行工具的地方都能运行测试，CI 中也一样，不需要 iOS 模拟器或 Android 模拟器：`asuka test` 会把包连同其中的测试一起编译，然后在这台电脑上运行；第一次运行时会构建测试运行器，这需要一个 C 编译器。有测试失败时，它以状态码 1 退出。如果要使用各平台真实的视图，参见下文的[在各平台上测试](#在各平台上测试)。

`asuka run file.asuka` 会编译单个文件并以同样的方式运行：先运行其中的测试（如果有的话），再运行它的应用，并打印屏幕上显示的内容。想试一个组件或函数时，不必为此创建一个应用。

## 工作原理

`mount` 把视图挂载到一个代替平台的替身上。它会记下应用创建的视图，但不会真的创建平台的视图。除此之外，一切都和应用真实运行时一样：它的状态、effect 和 task，Asuka 的布局、导航、列表绑定行、弹出面板（sheet）和警告框。

应用会像在 iPhone 上一样运行（iOS，390 × 844 点）。`asuka test --platform android`（或 `mac`、`windows`、`linux`、`web`）可以让它以为自己运行在另一个平台上，用来测试应用按平台区分的行为：测试也会为那个平台编译，包含各个包为该平台准备的文件，`@platform` 也是那个平台。测试和 `asuka dev` 一样是开发构建：[`@debug`](language/control-flow.md#开发构建的代码) 为 true；`asuka test --release` 会把它设为 false，和你发布的应用一致。

唯一和平台不同的是文字的测量方式：不管什么字体，每个字符 8 点宽，每行 20 点高。视图的位置来自 Asuka 真实的布局计算，所以视图之间的相对位置是可信的，但精确到点的尺寸并不是。

时间只在没有其他东西可以运行时才会推进：测试里的 `sleep(5s)`，或者某个 task 的定时器，都不会真的花时间；而一个等待某件永远不会发生的事的测试会立即失败（“the test never finished”）。

核心在应用运行时报告的问题（白白等待另一个 load 的 load、没有任何提示的长时间保留：见[错误及其代码](development.md#错误及其代码)），`asuka test` 会在发生的地方连同代码一起打印出来；`asuka test --json` 则把它们、每个测试的结果（属性测试失败时还包括失败的输入）、每个基准测试的结果以及每个文件的覆盖率，各写成一行 JSON。

除了用例测试，`asuka test` 还会用它生成的输入运行[属性测试](#属性测试)，对它们做[模糊测试](#模糊测试)，把值和[快照](#快照)比较，测量[基准测试](#基准测试)，统计[覆盖率](#覆盖率)；应用、包和命令行程序都一样适用。

## 屏幕上显示什么

测试用用户看到的文字来指称界面元素：一段文本、按钮的标题、输入框的文字或占位文字、无障碍标签、页面的标题、标签页、警告框的按钮。文字需要完全匹配。

```asuka
app.expect("Inbox")       // 不显示时测试失败
app.expectNo("Message 1") // 显示时测试失败
print(app.snapshot())     // 屏幕上显示的内容，每行一个视图
```

只有用户能看到的内容才算数：

- 每个导航栈最上面的页面（它下面的页面不显示）；
- 选中的标签页的内容，以及标签栏中每个标签页的标题（和徽标）；
- 分栏的侧边栏：每个目的地的标题和徽标；
- 显示在其余内容之上的弹出面板、警告框和弹出框（popover）；
- 列表中在其高度内放得下的行：Asuka 按平台的请求绑定列表的行，`asuka test` 也一样，就像集合视图（collection view）那样；
- 不包括任何隐藏的内容。

检查失败时，消息里会附上屏幕内容：每个显示出来的视图，连同它的类型、frame 和文字，让你看到那里实际显示的是什么；`asuka test` 还会指出测试所在的位置（`app.asuka:41`）。

## 像用户一样操作

每个操作在返回之前都会让应用稳定下来：它改变的内容已经完成布局，列表已经绑定，应用的 task 投递的工作也已经运行。

```asuka
app.tap("Add")                      // 按钮、行、标签页、开关、分段控件的段、菜单项
app.type("ada@example.com", into: "Email")  // 占位文字或标签为 Email 的输入框
app.type("milk", into: "Search notes")      // 屏幕栏里的搜索框，按它的占位文字找
app.tapLink("https://asuka.dev")     // RichText 中指向它的链接
app.share([SharedItem(kind: .link, text: "https://asuka.dev")])  // 另一个应用分享了它：onShare 会收到
await app.idle()                    // 直到没有任何东西要运行：task、定时器、加载
```

点按会落在手指会落下的地方。行里的文字会点按这一行，按钮里的文字会点按这个按钮。带菜单的视图会打开菜单，下一次 `tap` 就从菜单中选择。

## 属性测试

带参数的测试就是属性测试：它描述的是对参数类型的任意值都成立的性质，`asuka test` 会自己生成参数值来运行它。

```asuka check
struct Span {
  start: int
  end: int
}

fn merge(a: Span, b: Span) -> Span {
  return Span(start: min(a.start, b.start), end: max(a.end, b.end))
}

test "a merge covers both spans" (a: Span, b: Span) {
  let m = merge(a, b)
  expect(m.start <= a.start && m.start <= b.start)
  expect(m.end >= a.end && m.end >= b.end)
}

test "reversing twice gives the list back" (xs: [int], s: string) {
  expect(xs.reversed().reversed() == xs)
  expect(s.reversed().reversed() == s)
}
```

每个属性测试默认用 100 组输入运行。某组输入失败时，`asuka test` 会缩减（shrink）它：不断尝试更简单的输入（更短的列表和字符串、更小的数、`none`、枚举的第一个 case），留下仍然失败的最简单的那组，连同发现它的种子一起打印出来：

```
FAIL a page holds two rows at most (pages.asuka:9)
     expected page.count() <= 2 (pages.asuka:10): got 3 and 2
     input (case 4 of seed 0, shrunk in 3 steps):
       rows = ["", "", ""]
     rerun it: asuka test --run 'a page holds two rows at most' --seed 0
```

输入由种子决定，除非用 `--seed` 指定别的种子，否则种子就是 0。因此每次运行（包括 CI 里的）尝试的都是同一批输入，失败也能稳定复现。`--seed random` 会尝试一批新的输入并打印所用的种子；`--cases 1000` 则尝试更多组。各类型生成的值如下：

| 类型 | 生成的值 |
| --- | --- |
| `int`、`Duration` | 小的数最常见，其次是 ±1000 以内的数、各种边界值（0、±1、255、65535、32 位和 64 位的极值），以及任意值 |
| `float` | 整数、小数、任意有限值，以及 ±0、正负无穷和 NaN |
| `bool` | 两个值都有 |
| `string` | 字母最常见，其次是任意 ASCII 字符、其他文字、emoji、组合字符、NUL、换行、引号和花括号；最多 64 个字符 |
| `bytes`、`[T]`、`Set<T>`、`[string: T]` | 最多 64 个元素，元素少的最常见 |
| `T?` | `none` 或一个 `T` |
| struct、枚举 | 各字段的值；每个 case 及其关联值；包含自身的类型嵌套到一定深度就会停止 |

参数必须是数据：视图、闭包和 store 没有可以生成的值（[test-parameter](errors/test-parameter.md)）。其余需要的东西在测试里用参数构造出来；对于性质不涉及的输入，直接从测试里返回即可：`if xs.isEmpty() { return }`。

## 模糊测试

```sh
asuka test --fuzz                       # 每个属性测试跑 10 秒
asuka test --fuzz --run parse --fuzztime 5m
asuka test --fuzz --fuzztime 100000x    # 每个跑 100,000 组输入
```

`--fuzz` 只运行属性测试，每个测试不断用新的输入运行，直到 `--fuzztime` 用完：可以是一段时间，也可以是输入的组数。它会观察每组输入执行到了包里的哪些代码，并持续变换那些到达了其他输入没到过的代码的输入，因此能找到随机输入很难碰到的问题：解析器深处的分支、某种格式的边角情况。失败的输入会被缩减、打印出来，并以 JSON 保存到 `testdata/fuzz/<test>/` 中：

```json
{
  "s": "abcd"
}
```

请把它提交到仓库：每次 `asuka test` 都会先运行测试保存下来的输入，再运行生成的输入，这样修好的问题就不会再出现。你也可以自己编写这些文件：一个按参数名列出参数值的 JSON 对象（JSON 无法表示的浮点数写作 `NaN`、`Infinity` 或 `-Infinity`）。

## 快照

`expectSnapshot(value)` 把一个值打印出来的样子和为它保存的快照比较，不一致时测试失败。第一次运行时会把快照写到 `testdata/snapshots/` 中（每个源文件的测试对应一个文件），请把它提交到仓库：

```asuka
test "the list shows today's todos" {
  expectSnapshot(store.today())
  let app = mount(TodoList())
  expectSnapshot(app.snapshot())   // 屏幕上显示的内容
}
```

```
snapshot "the list shows today's todos 1"
  [
    Todo(title: "Buy milk", done: false, tags: ["errands"]),
    Todo(title: "Call Ada", done: true, tags: []),
  ]
```

`expectInlineSnapshot(value)` 则把快照直接保存在调用里：第一次运行时会按 `asuka fmt` 的格式写进去。

```asuka
expectInlineSnapshot(total(cart))
// 第一次运行之后：
expectInlineSnapshot(total(cart), "42.5")
```

字符串本身就是它的快照，原样保存。其他值按语言里的写法打印：`"a"`（带转义）、`1.0`、`1500ms`、`[1, 2]`、`["a": 1]`、`Set([1])`、`Todo(title: "Milk", done: false)`、`.circle(radius: 2.0)`、`none`；map 的条目和集合的元素会排序，所以与添加的先后顺序无关；一行放不下时，每个元素占一行。

值和快照不一致时，测试失败并显示差异：

```
FAIL the list shows today's todos (todos.asuka:12)
     expectSnapshot (todos.asuka:13): what it shows isn't its snapshot (asuka test --update writes what it shows)
     - the snapshot
     + what it shows
       [
     -   Todo(title: "Buy milk", done: false, tags: ["errands"]),
     +   Todo(title: "Buy oat milk", done: false, tags: ["errands"]),
         Todo(title: "Call Ada", done: true, tags: []),
       ]
```

`asuka test --update`（`-u`）会改为写入测试当前显示的内容；如果所有测试都运行并通过了，还会删除不再被任何测试使用的快照。在 CI 中（设置了 `CI` 环境变量时），尚未保存的快照不会被写入，而是让测试失败。属性测试不能使用快照，因为它要用生成的每个值运行。

## 基准测试

`bench` 就是一个基准测试：测量它所做的事花了多少时间。

```asuka check
fn fib(n: int) -> int {
  if n < 2 { return n }
  return fib(n - 1) + fib(n - 2)
}

bench "fib 20" {
  keep(fib(20))
}

bench "sorting 1,000 ints" {
  var xs: [int] = []
  for i in 0..<1000 {
    xs.push(i * 7919 % 1000)
  }
  measure {
    keep(xs.sorted())
  }
}
```

```sh
asuka test --bench                 # 所有基准测试
asuka test --bench --run sort      # 名称中包含 sort 的基准测试
```

```
bench fib 20                    109µs/op ±2%          0 allocs/op        0 B/op  (10 × 520)
bench sorting 1,000 ints       14.7µs/op ±4%          3 allocs/op    15.7 KB/op  (10 × 3,964)
```

`--bench` 会把每个基准测试的主体运行很多次：先运行一次，之后每轮运行更多次，直到一轮耗时达到 `--benchtime`（默认 1s）的十分之一；然后按这个次数再运行十轮，打印每次迭代耗时的中位数、最快和最慢的一轮与中位数相差多少，以及每次迭代分配了多少对象和字节。`measure { … }` 之前的代码用来做准备，不计入时间；没有 `measure` 时，整个主体都会计时。`keep(x)` 的值就是 `x`：把计算结果交给它，否则编译器可能会省掉那些结果没人读取的计算。

基准测试会编译成 C 运行，和发布构建一样（`@debug` 为 false），所以测到的就是应用发布出去的代码；`--vm` 则让它们在 `asuka dev` 使用的虚拟机上运行。时间只是测量，从不作为检查条件：基准测试只会因为它自己的检查（`expect`）或错误而失败。普通的 `asuka test` 会把每个基准测试当作测试运行一次，确认它仍然能正常工作。`--benchtime 100x` 则只运行一轮，恰好 100 次迭代。

挂载了视图的基准测试还会报告每次迭代有哪些改动到达了屏幕，也就是真机需要完成的工作：发给平台的改动批次，其中的操作数和字节数，平台测量过的视图数，以及布局计算过的视图数。像用户那样驱动应用（`app.tap`），或者在一帧里驱动它（`app.frame`，配合应用里的 `onFrame`），这样一次迭代的改动就会合成一批，就像一次点按那样：

```asuka
bench "a new row" {
  let app = mount(Inbox())
  measure { app.tap("Add") }
}
```

```
bench a new row                 41.2µs/op ±3%         84 allocs/op     6.1 KB/op  (10 × 2,410)  host: 1 batches, 9 ops, 312 B, 2 measures; 6 nodes laid out
```

在基准测试中，`app.ops()` 不会列出计时部分发送的操作。

想知道一次改动的效果，可以先保存一次运行结果再做比较：

```sh
asuka test --bench --json > before.json
# … 修改代码 …
asuka test --bench --compare before.json
```

这样每一行末尾都会附上之前的耗时和变化幅度（`was 24.1µs/op: -15.8%`）；如果两次运行的波动范围有重叠，还会注明“within noise”，表示这个差异可能只是噪声。

## 覆盖率

```sh
asuka test --cover                            # 每个文件一行摘要
asuka test --cover-lcov coverage/lcov.info    # 给编辑器和 CI 用
asuka test --cover-html coverage.html         # 每个文件中哪些代码运行过
```

```
coverage: 85.7% of lines (12 of 14), 75.0% of branches (6 of 8)
  todos.asuka   85.7%  lines 12/14  branches 6/8  not run: 31, 40-41
```

`--cover` 统计测试运行时，包里的代码有哪些行被执行了（只算包自己的代码，不包括测试本身的代码，也不包括它导入的包），以及每个分支的情况：`if` 的各个分支（包括没有写出来的 `else`）和 `match` 的各个分支。一行上有语句执行过，这一行就算执行过；这一行里没执行到的部分由分支数据体现。没有任何地方调用的函数算作从未执行。项目里的命令行程序（`[bin.<name>]`）由它们自己的测试来统计。

`--cover-lcov` 以 lcov 格式写出结果，编辑器（VS Code 的 Coverage Gutters）和 CI 服务（Codecov、Coveralls）都能读取；`--cover-html` 则生成一个页面，显示源代码，执行过的部分标为绿色，没执行过的标为红色。不加 `--cover` 时，不会统计任何东西。

## 平台服务与原生包

[原生包](native.md)的接口由平台实现，而 `asuka test` 没有这些实现：调用它们会抛出错误，就像在没有这项服务的平台上一样。通过 store 或 `provide` 把组件要用的东西交给它，测试则提供自己的版本：

```asuka
test "archiving removes the row" {
  let app = mount(Inbox(), provide: Mailbox.fake([sample]))
  await app.idle()
  app.tap("Archive")
  app.expectNo("Hello")
}
```

`provide:` 接受一个 store；视图用到好几个 store 时，也可以传一个列表：`mount(Inbox(), provide: [Mailbox.fake([sample]), Session()])`。

## 在各平台上测试

`asuka test` 测试的是应用的行为：显示什么，以及用户操作时会发生什么。每个平台如何绘制它，是平台自己的事。要在各平台上查看你的应用：

- `asuka dev --platform ios|android|web` 会在 iOS 模拟器、Android 模拟器、手机或浏览器中运行应用，并在保存时重新加载。
- `asuka test --gui` 会在那里运行应用的流程，也就是用户在应用里的一连串操作：参见[GUI 测试](#gui-测试流程)。
- 在桌面端，`ASUKA_SNAPSHOT=out.png` 会在窗口显示后把它保存为 PNG，不抢占焦点，然后退出（`ASUKA_DUMP=1` 还会在旁边写出视图树）。`ASUKA_SCRIPT` 会先执行一些步骤（`tap:Add`、`type:hello`）：参见[开发](development.md#快照与脚本)。
- 在 Linux 上，在 Xvfb 下（`xvfb-run -a dbus-run-session`）运行，就能在没有显示器的环境（比如 CI）中做到这一点。

## GUI 测试：流程

流程（flow）是用户在应用里的一连串操作，在某个平台的开发 App 上运行：iOS 模拟器、Android 模拟器、手机、无头 Chrome，或者这台电脑。每个流程是应用 `flows` 目录下的一个文件，每行一步：

```
# flows/note.flow：在表单里给一条消息写备注。
tap Message 20
see No note yet.
tap Add a note
type hello
tap Save
see Note: hello
back
notsee Note: hello
```

`asuka test --gui` 像 `asuka dev` 那样运行应用来执行这些流程，每个流程都从头开始，所有 `state` 都是初始值，并报告每个流程的结果：

```
asuka test --gui --platform ios       # iOS 模拟器（用 --device 选择）
asuka test --gui --platform android   # Android 模拟器或手机
asuka test --gui --platform web       # 无头 Chrome
asuka test --gui                      # 这台电脑
```

| 步骤 | 作用 |
|---|---|
| `tap <text>` | 按下显示这段文字的东西：按钮、开关、可以按的视图（比如一行），或者文本框，之后输入的内容会进入这个文本框。 |
| `type <text>` | 像用户打字那样，在最后点过的文本框里输入（没有的话就是第一个文本框）。 |
| `submit` | 在那个文本框里按回车。 |
| `back` | 像返回按钮那样，从最上面的页面返回。 |
| `tab <title>` | 像在标签栏里点它那样，选中有这个标题的标签。 |
| `scroll` | 把屏幕上的列表往下滚一页。 |
| `scrollto <text>` | 把列表往下滚，直到有东西显示这段文字。 |
| `see <text>` | 屏幕上有东西显示这段文字。 |
| `notsee <text>` | 屏幕上没有东西显示它。 |
| `wait <time>` | 等待（`500ms`、`2s`）。 |

文字可以是视图显示的内容、它的标签（只有图标的按钮）或文本框的占位文字（`tap New note`），按整体匹配：`see Message 1` 不会匹配 `Message 19`。文字里的 `*` 可以匹配任意字符，用于会变化的文字：`see Count: *`。列表只为屏幕上的行创建视图，所以更靠下的行要先用 `scroll` 或 `scrollto` 滚到屏幕上才会出现。

失败的步骤会一直重试，直到通过或超过 `--timeout`（10s），因为应用可能还在加载，或在等网络。超时后流程失败，并报告该步骤所在的行和屏幕上显示的内容。被最上面的页面挡住的页面、没有选中的标签都不算在显示。`--run text` 只运行名字里包含这段文字的流程。这些步骤经过 Asuka 自己的视图，就像平台的事件传来时那样，所以流程在每个平台上的运行方式都一样，而显示出来的是平台的控件。
