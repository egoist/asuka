# optimistic-not-ready

準備ができていない load に `optimistic` を呼んだ。

アプリの実行中にコアが報告します。検査するのは開発ビルド（`asuka dev`、`asuka run`、`asuka test`）だけで、コードの場所ごとに 1 回だけ報告されます。リリースビルドでは検査しません。

## 理由

楽観的な変更は load が表示するものの関数です。最初の答えの前（pending）や失敗したときは、変える表示がないので、呼び出しは何もしません。

## 直し方

`.ready` になってから表示するものを変えます。操作を `.ready` の分岐に置くか、先に `x.value` を確かめます。

## 誤った例

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

## 正しい例

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
