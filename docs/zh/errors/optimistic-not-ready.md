# optimistic-not-ready

对还没有就绪的 load 调用了 `optimistic`。

应用运行时由核心报告，只在开发构建中检查（`asuka dev`、`asuka run`、`asuka test`），代码里的每个位置只报告一次；发布构建不做这项检查。

## 原因

乐观改动是 load 所显示内容的函数。load 在等待第一个结果时（pending）或失败时，没有可以改动的内容：这次调用什么也不做。

## 如何修正

等它 `.ready` 之后再改动它显示的内容：把这个操作放在 `.ready` 分支里，或者先检查 `x.value`。

## 错误示例

```asuka
async fn fetchCount() -> int {
  await sleep(10ms)
  return 1
}

component Counter {
  let count = load fetchCount()
  Column {
    Text("{count.value ?? 0}")
    Button("Add") {
      on press {
        count.optimistic(n => n + 1)
        count.reload()
      }
    }
  }
}

test "a change before the answer" {
  let app = mount(Counter())
  app.tap("Add")
  await app.idle()
  app.expect("1")
}
```

## 正确示例

```asuka
async fn fetchCount() -> int {
  await sleep(10ms)
  return 1
}

component Counter {
  let count = load fetchCount()
  Column {
    match count {
      .pending => Text("…")
      .failed(e) => Text(e.message)
      .ready(n) => Button("{n}") {
        on press {
          count.optimistic(c => c + 1)
          count.reload()
        }
      }
    }
  }
}

test "a change of what it shows" {
  let app = mount(Counter())
  await app.idle()
  app.tap("1")
  await app.idle()
  app.expect("1")
}
```
