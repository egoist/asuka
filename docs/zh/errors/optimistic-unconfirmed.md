# optimistic-unconfirmed

乐观改动随它的任务一起撤销了：它的 load 没有一次运行来确认它。

应用运行时由核心报告，只在开发构建中检查（`asuka dev`、`asuka run`、`asuka test`），代码里的每个位置只报告一次；发布构建不做这项检查。

## 原因

`x.optimistic(f)` 会一直显示改动，直到 load 再次给出结果。如果创建它的任务结束时，load 既没有在运行，也没有被要求运行（没有 `reload`），就永远不会有东西来确认它：留着它会挡住之后的每一个结果，所以它被撤销，画面收回刚才显示的改动。

## 如何修正

在任务结束之前再次请求这个 load：`await reload f(…)`，或者 `x.reload()`。

## 错误示例

```asuka
async fn fetchCount() -> int {
  await sleep(10ms)
  return 1
}

async fn save(n: int) {
  await sleep(10ms)
}

component Counter {
  let count = load fetchCount()
  Column {
    Text("{count.value ?? 0}")
    Button("Add") {
      on press {
        count.optimistic(n => n + 1)
        await save(2)
      }
    }
  }
}

test "a change nothing confirms" {
  let app = mount(Counter())
  await app.idle()
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

async fn save(n: int) {
  await sleep(10ms)
}

component Counter {
  let count = load fetchCount()
  Column {
    Text("{count.value ?? 0}")
    Button("Add") {
      on press {
        count.optimistic(n => n + 1)
        await save(2)
        count.reload()
      }
    }
  }
}

test "a change its load's answer replaces" {
  let app = mount(Counter())
  await app.idle()
  app.tap("Add")
  await app.idle()
  app.expect("1")
}
```
