# reload-arguments

`reload f(…)` 给出了 `f` 不接受的参数。

编译器报告的错误：修正之前程序无法构建。

## 原因

`reload f(a)` 会触及参数等于 `a` 的那些 `f` 的 load：比较是按 `f` 的参数进行的，所以这些参数要像调用 `f` 那样接受检查。

## 如何修正

给出 `f` 接受的参数，和它的 load 调用它的方式一致；或者不给参数：`reload f` 会触及 `f` 的每一个 load。

## 错误示例

```asuka
async fn fetchNote(id: int) -> string {
  await sleep(1ms)
  return "note {id}"
}

component Note(id: int) {
  let note = load fetchNote(id)
  Button("Refresh") {
    on press { await reload fetchNote("{id}") }
  }
}
```

## 正确示例

```asuka
async fn fetchNote(id: int) -> string {
  await sleep(1ms)
  return "note {id}"
}

component Note(id: int) {
  let note = load fetchNote(id)
  Button("Refresh") {
    on press { await reload fetchNote(id) }
  }
}
```
