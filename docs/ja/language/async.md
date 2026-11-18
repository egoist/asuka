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
- `all(a, b)` は複数の呼び出しを同時に実行し、すべての結果を待ちます。`race(a, b)` は最初に終わった結果を取り、残りをキャンセルします。ほかに `sleep(300ms)` と `withTimeout(5s) { … }` があります。`withTimeout` の値はブロックの値で（`let page = try await withTimeout(5s) { return try await fetch(url) }`）、時間切れになるとエラーを投げます。これらは構造化されていて、キャンセルした処理が止まり、その `defer` が実行されてから戻るかエラーを投げます。`await never()` はタスクが終わるまで待つだけです。読み手がいなくなったときにだけ終わるストリームや、何かを持つ前に await が必要なタスク（`try await register(); defer { release() }; await never()`）に使います。await せずに持てるものは、コンポーネントの `cleanup { }` で戻します（[クリーンアップ](components.md#クリーンアップ)を参照）。
- `spawn { … }` はブロックを別のスレッドで実行します。メインスレッドを止めてしまうような処理（パース、画像処理）に使います。中で使えるのは値だけで、状態、ストア、ビュー、ネイティブのオブジェクト、C のポインター、そしてそれらを持つファイルの `let`（データベースなど）は使えません。使っていればコンパイラーが教えてくれます。Web では、ページの Web Worker の一つで実行されます。Worker はファイルの値だけを自分で作り、それ以外は作りません。呼び出しの間で何かを持ち続ける処理（インデックス、自分で開くデータベースなど）には [ワーカー](#ワーカー) を使います。
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
- **キャンセル。** タスクは、それを始めたものに属します（コンポーネントのイベントのハンドラーは、それを宣言したコンポーネントに属します。[コンポーネント](components.md)を参照）。それがなくなると（ユーザーが画面を離れたときなど）、タスクはキャンセルされます。そのとき待っている `await` でタスクは終わり、`defer` が実行され、エラーは報告されません。

## ワーカー

`spawn` はブロックを別のスレッドで一度だけ実行します。`worker` はそこに値を持ち続けます。インスタンスがそれぞれ自分のスレッドで動き、自分だけの `var` を持つ型です。

```asuka check
worker SearchIndex {
  var words: [string: [int]] = [:]

  pub fn add(id: int, text: string) {
    for w in text.split(" ") {
      let k = w.lower()
      words[k] = (words[k] ?? []) + [id]
    }
  }
  pub fn find(q: string) -> [int] = words[q.lower()] ?? []
}

store Notes {
  let index = SearchIndex()
  state results: [int] = []

  pub async fn add(id: int, text: string) { await index.add(id, text) }
  pub async fn search(q: string) { results = await index.find(q) }
}
```

- ワーカーの `var`、`let`、パラメーター（`worker Index(language: string)`）はワーカー自身のものです。読み書きするのはワーカーの fn だけで、ワーカーのスレッドで行われます。`state`（ビューが読むことはありません）、props、イベント、ビューは持ちません。
- 外から呼ぶと、ワーカーの `pub fn` は非同期です（`await index.find(q)`）。引数は値として渡り、呼び出しは呼ばれた順に一つずつ実行され、結果、または `throws` の関数が投げたエラーも値として戻ってきます。ワーカーの中では普通の関数で、`await` はしません。それ以外の fn はワーカー専用です。
- 渡せるものは `spawn` と同じで、値だけです。状態、ストア、ビュー、クロージャ、ネイティブのオブジェクトは渡せません。ワーカーの fn はメインスレッドの状態も読みません。C のハンドルは自分で作って持てます（[extern type](../c.md#c-が所有する型)、開いたデータベースなど）。それがワーカーの外に出ることはなく、解放もワーカーの中で行われます。
- ワーカーは、それを作ったコンポーネントやストアに属します。それがなくなるとワーカーは止まります。実行中の呼び出しは最後まで動き、待っている呼び出しはキャンセルされます。呼び出し元のタスクには、エラーではなく自分自身のキャンセルとして見えます。オーナーのいないワーカーは、どこからも参照されなくなり、渡された呼び出しをすべて実行し終えると止まります。
- Web では、ワーカーはそれぞれ専用の Web Worker になります。

## 読み込むデータ

`load` は非同期の結果をコンポーネントの中に保持します。

```asuka
screen Profile(id: string) {
  let user = load api.user(id)   // id が変わると実行し直す

  match user {
    .pending => Spinner()
    .failed(e) => ErrorView(e) { on retry { user.reload() } }
    .ready(u) => ProfileCard(u) { opacity: if user.loading { 0.6 } else { 1 } }
  }
}
```

`Loading<T>` は `.pending`、`.ready(value)`、`.failed(error)` のいずれかで、`.pending` になるのは最初の結果が届く前だけです。引数が読んでいるものが変わったときや、`.reload()` でもう一度実行したときは、新しい結果が届くまで今のケースのままです。`ProfileCard` は状態もスクロール位置もそのまま画面に残り、古くなりかけている実行はキャンセルされます。前回と等しい結果が届いても、何も変わりません。

- `.value` は最新の値で、最初の結果が届くまでは none です。実行が失敗しても残るので、`.failed` ではエラーと並べて前の内容を表示できます。
- `.loading` は実行中のあいだ true になります（最初の実行も含みます）。内容を薄く表示したり、隅にインジケーターを出したりするのに使います。これを読んでも値の変化には追従せず、`match` もこれには追従しません。

```asuka
let results = load api.search(query)

TextField { bind text: query; placeholder: "Search" }
if results.loading { ProgressRing() }
match results {
  .pending => Spinner()
  .failed(e) => Text(e.message)
  .ready(items) => List { for r in items key r.id { ResultRow(r) } }
}
```

ユーザーが入力しているあいだ、リストは新しい結果が届くまで前の結果を表示し続け、スクロール位置も各行の状態も保たれます。インジケーターは検索が実行中であることを示します。`query` を読んでいるほかの部分には、新しいテキストがすぐに表示されます。

最初からやり直したいとき（別の人のプロフィールを読み込んでいるあいだ、前の人のものを見せるべきではない場合など）は、`match` の代わりに `.loading` を読みます。

```asuka
if user.loading {
  Spinner()
} else {
  match user {
    .pending => Spinner()
    .failed(e) => ErrorView(e) { on retry { user.reload() } }
    .ready(u) => ProfileCard(u)
  }
}
```

もっとも、多くの場合は内容がすでにそれぞれ独立しています。id ごとにプッシュされる画面や key ごとの行が、それぞれ自分の `load` を持つ形です。
