# 非同期処理

待つ必要のある処理（ネットワーク、ファイル、タイマー）は `async` で、それを待つのは `await` です。

```asuka
async fn loadThread(id: string) throws -> Thread {
  let (messages, people) = try await all(api.messages(id), api.people(id))
  let html = await spawn { renderMarkdown(messages) }
  return Thread(messages, people, html)
}
```

- 非同期関数を呼ぶには `await` が必要です。ハンドラー、タスク、テストでは `await` できますが、コンポーネントの本体とその中の `let` ではできません。クロージャは、非同期関数として渡されるか、本体で `await` していれば非同期になります。`async () => …` と書けば明示できます（[関数とクロージャ](functions.md)）。
- **メインスレッド。** すべてのコードはメインスレッドで動きます。`await` で待っている間は、ほかの処理が動けます。状態を変更できるのはメインスレッドだけです。
- `all(a, b)` は複数の呼び出しを同時に実行し、すべての結果を待ちます。`race(a, b)` は最初に終わった結果を取り、残りをキャンセルします。ほかに `sleep(300ms)` と `withTimeout(5s) { … }` があります。`withTimeout` の値はブロックの値で（`let page = try await withTimeout(5s) { return try await fetch(url) }`）、時間切れになるとエラーを投げます。これらは構造化されていて、キャンセルした処理が止まり、その `defer` が実行されてから戻るかエラーを投げます。`await never()` はタスクが終わるまで待つだけです。コンポーネントが表示されている間、何かを持ち続けるタスク（`defer { release() }; await never()`）や、読み手がいなくなったときにだけ終わるストリームに使います。
- `spawn { … }` はブロックを別のスレッドで実行します。メインスレッドを止めてしまうような処理（パース、画像処理）に使います。中で使えるのは値だけで、状態、ストア、ビュー、ネイティブのオブジェクト、C のポインター、そしてそれらを持つファイルの `let`（データベースなど）は使えません。使っていればコンパイラーが教えてくれます。Web では、ページの Web Worker の一つで実行されます。Worker はファイルの値だけを自分で作り、それ以外は作りません。
- `for await message in socket.messages { … }` はストリーム、つまり `Stream<T>` を読みます。時間とともに届く値です。`Stream(send => { … })` で作ります。ストリームが最初に読まれたときにブロックがタスクとして動き出し、`send(x)` のたびに値が一つ届きます。ブロックが戻るとストリームは終わり、ブロックが投げたエラーでストリームも失敗します。ループを抜けると（`break`、エラー、オーナーがなくなる）ブロックはキャンセルされます。ストリームの読み手は一つだけです。

  ```asuka
  fn levels() -> Stream<float> = Stream(send => {
    try await battery.watch(changed: level => send(level))
  })
  ```
- `live(initial, source)` は、ストリームが最新に保つ値で、それを読む関数として使います。バインディングの中で読むと、最初にそう読まれたときから最後の読み手がいなくなるまで `source()` のストリームに従います（そのあとストリームはキャンセルされ、誰も見ていないのにプラットフォームを監視し続けることはありません）。それ以外の場所で読むと、最後に分かっている値になります。ネットワークの状態のように、プラットフォームが変えるものをパッケージが提供するときに使います。

  ```asuka
  let level = live(1.0, levels)
  pub fn batteryLevel() -> float = level()
  ```
- **キャンセル。** タスクは、それを始めたものに属します。それがなくなると（ユーザーが画面を離れたときなど）、タスクはキャンセルされます。そのとき待っている `await` でタスクは終わり、`defer` が実行され、エラーは報告されません。

## 読み込むデータ

`load` は非同期の結果をコンポーネントの中に保持します。

```asuka
screen Profile(id: string) {
  let user = load api.user(id)   // id が変わると読み込み直す

  match user {
    .pending => Spinner()
    .failed(e) => ErrorView(e) { on retry { user.reload() } }
    .ready(u) => ProfileCard(u)
  }
}
```

`Loading<T>` は `.pending`、`.ready(value)`、`.failed(error)` のいずれかです。`.value` は最新の値で（読み込み直している間も残るので、画面がちらつきません）、`.reload()` はもう一度実行します。引数が読んでいるものが変わると再び実行され、古くなりかけている実行はキャンセルされます。
