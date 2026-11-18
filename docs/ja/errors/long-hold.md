# long-hold

load が数秒間、何も示さずに古いコンテンツを保っている。

アプリの実行中にコアが報告します。検査するのは開発ビルド（`asuka dev`、`asuka run`、`asuka test`）だけで、コードの場所ごとに 1 回だけ報告されます。リリースビルドでは検査しません。

## 理由

再実行された load は、新しい答えが届くまで表示していたものを保ちます（`.ready` のまま）。まとめた load の呼び出しのうち答えたものも、遅い呼び出しが答えるまで答えを保ちます。画面は落ち着きますが、答えに数秒かかり、`.loading` を読むものがないと、ユーザーは次の答えが来ることを知らないまま前の答えを見続けます。

## 直し方

保っているあいだ `x.loading` を読む（コンテンツを薄くする、リングを表示する）か、その場所にスピナーを表示します。

## 誤った例

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

## 正しい例

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
