# 错误

可能失败的函数要标注 `throws`，调用它时需要 `try`：

```asuka
enum MailError: Error { offline, server(code: int) }

async fn send(m: Message) throws {
  if !network.online { throw MailError.offline }
  …
}

on press {
  do {
    try await send(draft)
    dismiss()
  } catch MailError.offline {
    showOffline = true
  } catch e {
    alert("Couldn't send", message: e.message)
  }
}

let n = try? parse(text)   // 抛出错误时为 none
```

在表达式后面写 `catch`，可以给它一个备用值：正常时得到表达式的值；表达式里有地方抛出错误时，得到 `catch` 代码块的值（和 `if` 一样，是它的最后一个表达式），除非这个代码块直接离开：

```asuka
let port = try parsePort(text) catch e {
  log(e.message)
  80
}
let size = try await fileSize(path) catch { return }
try save(draft) catch { failed = true }   // 作为语句：不需要提供值
```

它的子句和 `do` 的一样：`catch`、`catch e`、`catch e: ParseError`、`catch MailError.server(code)`，可以连写多个，由第一个接住错误的子句处理。没有子句接住的错误会继续往外抛，所以所在的函数必须标注 `throws`，这和 `try` 的规则相同。如果子句能接住所有错误，整个表达式就不会抛出错误，因此可以用在 `try` 不能出现的地方：组件的 `let` 里（`let port = try parsePort(text) catch { 80 }`，会随着 `text` 的变化保持更新），或者不允许抛出错误的闭包里。它也可以配合 `try await` 使用，以及配合作为值的 `throw`：`let user = cache[id] ?? throw MailError.missing(id)`。

错误的 `message` 是它的 `message` 字段、它的 `fn message() -> string`，或者这个错误按语言的写法写出的样子：类型名、成员名和负载，其中每个值的写法都和插值时写其他值内部的部分一样（`CountError.tooMany(limit: 2)`、`ParseError(line: 3, text: "x")`、`MailError.offline`），与调试器显示的一致，也像调试器那样截短过长的部分（超过 200 字节、字符串超过 120 字节、列表超过 10 项、嵌套超过 3 层的部分写成 `…`）。消息在读取时才生成。`Error("…")` 用一条消息创建一个错误。`catch` 永远不会捕获 task 的取消：取消会一直走到 task 的结尾，并在途中执行各个 `defer`。

`Error` 也可以像 `catch` 那样按错误类型的成员来匹配：`match e { MailError.offline => …; MailError.status(code) => …; _ => e.message }`（`Error` 可能是任何错误，所以匹配要以 `_` 结尾）。

处理函数或 task 没有捕获的错误，会交给包住这个组件的最近的 `Boundary`；如果没有，就交给应用自己的处理（开发时显示出来，发布版本中记录到日志）：

```asuka
Boundary {
  Feed()
  on error(e) { Text("Something went wrong: {e.message}") }
}
```
