# テスト

```asuka
test "archiving removes the row" {
  let app = mount(Inbox(), provide: Mailbox.fake([sample]))
  await app.idle()
  app.tap("Archive")
  app.expectNo("Hello")
}
```

`expect(cond)` は cond が false ならテストを失敗させ、何がどこで失敗したかを示します（比較なら両辺の値も示します）。JSON は `std/json` で扱います。`let notes: [Note] = try json.decode(text)` は求められている型として読み込み（`json.decode<[Note]>(text)` とも書けます）、`json.encode(notes)` は JSON に書き出します。`json.Value`（任意の JSON）が求められるところでは、JSON で表せる値は書き出したとおりの `json.Value` になります。型の混ざった値のマップは `[string: json.Value]` になり（`["port": port, "tls": true]`）、数値は数値のままです。ジェネリック関数も自分の `T` を JSON として読み書きできます。その場合、呼び出しごとに `T` は JSON で表せる型でなければならず、コンパイラーが呼び出し箇所でそれをチェックします。

`asuka test` は、パッケージのテストをデバイスなしで実行します。プラットフォームと同じようにビューをレイアウトし、テキストは 1 文字あたり固定の幅で測ります。また、時間はほかに実行できるものが何もないときにしか進みません（テストの中の `sleep(5s)` には時間がかかりません）。`mount` はアプリを返すので、それを操作して確かめます（`tap`、`longPress`、`type(_, into:)`、`submit`、`toggle`、`slide`、`select`、`swipe(_, action:)`、`refresh`、`scrollTo`、`back`、`dismiss`、`pickDate`、`pickRange`、`remove`、`chooseSuggestion`、`pickColor`、`expect`、`expectNo`、`texts()`、`disabled(text)`、`until`、`waitFor`、`idle`）。`in(.navigationBar)`、`in(.sidebar)`、`nextTo(label)` で、テキストが指すものを絞り込めます。ルーターがあれば、`mount(…, at: "/inbox/1")`、`href`、`history()`、`goBack()`、`goForward()` も使えます。詳しくは[テスト](../testing.md)を参照してください。

テストはどのファイルにも書けますが、パッケージの**テストファイル**にも書けます。ファイル名が `_test.asuka` で終わるものです（`parser_test.asuka`。1 つのプラットフォーム用なら `tray_linux_test.asuka`）。テストファイルの宣言とインポート（テスト用のデータやヘルパー）は、そのパッケージ自身のテストを実行するとき（`asuka test`、`asuka check`、エディター）にだけパッケージの一部になります。ビルドにはそれらも、`test`、`bench`、`preview` のブロック（[プレビュー](components.md#プレビュー)）も含まれず、このパッケージをインポートするほかのパッケージからも見えません。そのため、テストにしか要らないパッケージをテストファイルでインポートしても、アプリには入りません。

パラメーターのあるテストはプロパティテストで、`asuka test` がパラメーターの値を生成して実行します。失敗した入力は、まだ失敗するいちばん単純なものに縮小されます。`bench "name" { … }` はベンチマークで、`asuka test --bench` が計測します。その中の `measure { … }` が計測される部分で、準備のあとに書きます。`keep(x)` は、結果が最適化で省かれないようにします。`expectSnapshot(value)` と `expectInlineSnapshot(value, "…")` は、値を出力したものをスナップショットと比べます。スナップショットは `asuka test` が書き込みます。

```asuka check
fn fib(n: int) -> int {
  if n < 2 { return n }
  return fib(n - 1) + fib(n - 2)
}

test "fib grows" (n: int) {
  if n < 2 || n > 30 { return }
  expect(fib(n) >= fib(n - 1))
}

test "the first ones" {
  expectInlineSnapshot([fib(1), fib(2), fib(3), fib(10)], "[1, 1, 2, 55]")
}

bench "fib 20" {
  keep(fib(20))
}
```
