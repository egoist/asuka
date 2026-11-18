# reload-arguments

`f` が受け取らない引数で `reload f(…)` した。

コンパイラーが報告するエラーです。直すまでプログラムはビルドできません。

## 理由

`reload f(a)` は、引数が `a` と等しい `f` の load に届きます。引数は `f` のパラメーターとして比べられるので、`f` の呼び出しと同じように検査されます。

## 直し方

`f` が受け取る引数を、load が呼ぶのと同じ形で渡すか、何も渡しません。`reload f` は `f` のすべての load に届きます。

## 誤った例

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

## 正しい例

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
