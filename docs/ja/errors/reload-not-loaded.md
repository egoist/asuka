# reload-not-loaded

どの load も呼んでいないものを `reload` した。

コンパイラーが報告するエラーです。直すまでプログラムはビルドできません。

## 理由

`reload f(…)` は、オペランドが `f` の呼び出しである load にもう一度求めさせます。呼び出しそのものが、呼ぶ関数によって区別されるキーです。プログラムのどの `load` も呼んでいない関数や、関数でないもの（クロージャー、値のメソッド）は、どの load にも届きません。

## 直し方

`load` が呼ぶ関数を reload する（`reload api.notes`）か、load をハンドルで読み込み直します（`x.reload()`）。

## 誤った例

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

## 正しい例

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
