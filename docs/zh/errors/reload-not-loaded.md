# reload-not-loaded

`reload` 的对象没有被任何 load 调用。

编译器报告的错误：修正之前程序无法构建。

## 原因

`reload f(…)` 会让操作数是 `f` 的调用的那些 load 再请求一次：调用本身就是键，按它调用的函数来区分。程序里没有任何 `load` 调用的函数，或者不是函数的东西（闭包、某个值的方法），都不会触及任何 load。

## 如何修正

重新加载某个 `load` 调用的函数（`reload api.notes`），或者通过句柄重新加载一个 load（`x.reload()`）。

## 错误示例

```asuka
async fn fetchNotes() -> [string] {
  await sleep(1ms)
  return []
}

async fn fetchTags() -> [string] {
  await sleep(1ms)
  return []
}

component Notes {
  let notes = load fetchNotes()
  Button("Refresh") {
    on press { await reload fetchTags() }
  }
}
```

## 正确示例

```asuka
async fn fetchNotes() -> [string] {
  await sleep(1ms)
  return []
}

component Notes {
  let notes = load fetchNotes()
  Button("Refresh") {
    on press { await reload fetchNotes() }
  }
}
```
