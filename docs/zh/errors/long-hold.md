# long-hold

load 保留旧内容已有好几秒，却没有任何提示。

应用运行时由核心报告，只在开发构建中检查（`asuka dev`、`asuka run`、`asuka test`），代码里的每个位置只报告一次；发布构建不做这项检查。

## 原因

重新运行的 load 会一直保留它显示的内容，直到新结果到来（`.ready` 保持不变）；分组 load 里已经得到结果的调用，也会把结果留着，等较慢的同组调用。这样画面不会跳动；但如果结果要好几秒才到，又没有任何地方读取 `.loading`，用户看到的就是上一次的结果，完全不知道新结果还在路上。

## 如何修正

在保留期间读取 `x.loading`（把内容调暗、显示一个转圈），或者在它的位置显示加载指示。

## 错误示例

```asuka
async fn fetchName(id: int) -> string {
  await sleep(3s)
  return "user {id}"
}

component Profile {
  state id = 1
  let user = load fetchName(id)
  Column {
    match user {
      .pending => Text("…")
      .failed(e) => Text(e.message)
      .ready(name) => Text(name)
    }
    Button("Next") { on press { id += 1 } }
  }
}

test "a long hold" {
  let app = mount(Profile())
  await app.idle()
  app.expect("user 1")
  app.tap("Next")
  await app.idle()
  app.expect("user 2")
}
```

## 正确示例

```asuka
async fn fetchName(id: int) -> string {
  await sleep(3s)
  return "user {id}"
}

component Profile {
  state id = 1
  let user = load fetchName(id)
  Column {
    match user {
      .pending => Text("…")
      .failed(e) => Text(e.message)
      .ready(name) => Text(name) { opacity: if user.loading { 0.5 } else { 1 } }
    }
    Button("Next") { on press { id += 1 } }
  }
}

test "a hold that says so" {
  let app = mount(Profile())
  await app.idle()
  app.expect("user 1")
  app.tap("Next")
  await app.idle()
  app.expect("user 2")
}
```
