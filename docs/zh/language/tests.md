# 测试

```asuka
test "archiving removes the row" {
  let app = mount(Inbox(), provide: Mailbox.fake([sample]))
  await app.idle()
  app.tap("Archive")
  app.expectNo("Hello")
}
```

`expect(cond)` 在 cond 为 false 时让测试失败，并指出失败的是什么、在哪里（如果是比较，还会给出两边的值）。JSON 由 `std/json` 处理：`let notes: [Note] = try json.decode(text)` 按期望的类型读取 JSON（也可以写成 `json.decode<[Note]>(text)`），`json.encode(notes)` 把值写成 JSON。在需要 `json.Value`（任意 JSON）的地方，JSON 能表示的值会按它写出的样子自动成为 `json.Value`：混合类型的值组成的 map 就是一个 `[string: json.Value]`（`["port": port, "tls": true]`），其中的数字仍然是数字。泛型函数也可以把它的 `T` 当作 JSON 读写；这样每次调用时，`T` 都必须是 JSON 能表示的类型，编译器会在调用处检查这一点。

`asuka test` 不需要设备就能运行包里的测试：它会像各平台那样对视图进行布局，按每个字符固定宽度来测量文字，而且只有在没有其他东西可以运行时，时间才会前进（测试里的 `sleep(5s)` 不花任何时间）。`mount` 返回应用，供你驱动和检查：`tap`、`longPress`、`type(_, into:)`、`submit`、`toggle`、`slide`、`select`、`swipe(_, action:)`、`refresh`、`scrollTo`、`back`、`dismiss`、`pickDate`、`pickColor`、`expect`、`expectNo`、`texts()`、`disabled(text)`、`until`、`waitFor`、`idle`；`in(.navigationBar)`、`in(.sidebar)` 和 `nextTo(label)` 缩小文字所指的范围；有路由器时，还有 `mount(…, at: "/inbox/1")`、`href`、`history()`、`goBack()`、`goForward()`。详见[测试](../testing.md)。

测试可以写在任何文件里，也可以写在包的**测试文件**里：文件名以 `_test.asuka` 结尾（`parser_test.asuka`；只给一个平台的写成 `tray_linux_test.asuka`）。测试文件里的声明和导入（测试数据、辅助函数）只在运行这个包自己的测试时才属于这个包（`asuka test`、`asuka check`、编辑器）：构建出来的应用里没有它们，也没有任何 `test`、`bench` 或 `preview` 块（见[预览](components.md#预览)）；导入这个包的其他包也永远看不到它们。所以测试文件可以放心导入只有测试才用得到的包，这些包不会被带进应用。

带参数的测试是属性测试，`asuka test` 会为参数生成值来运行它；失败的输入会被缩减成仍然失败的最简单的那组。`bench "name" { … }` 是基准测试，由 `asuka test --bench` 计时；其中的 `measure { … }` 是要计时的部分，写在准备工作之后；`keep(x)` 防止计算结果被编译器优化掉。`expectSnapshot(value)` 和 `expectInlineSnapshot(value, "…")` 把一个值打印出来的样子和它的快照比较，快照由 `asuka test` 写入。

```asuka check
fn fib(n: int) -> int {
  if n < 2 { return n }
  return fib(n - 1) + fib(n - 2)
}

test "fib grows" (n: int) {
  if n < 2 || n > 30 { return }
  expect(fib(n) >= fib(n - 1))
}

test "the first ones" {
  expectInlineSnapshot([fib(1), fib(2), fib(3), fib(10)], "[1, 1, 2, 55]")
}

bench "fib 20" {
  keep(fib(20))
}
```
