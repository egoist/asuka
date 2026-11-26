# テスト

アプリのテストは、コードの隣に書く `test` ブロックです。`mount` はデバイスもウィンドウもディスプレイも使わずにコンポーネントをマウントし、テストはそれをユーザーと同じように、画面に表示されているものを手がかりに操作します。

```asuka
component Counter {
  state count = 0
  Column {
    Text("Count: {count}")
    Button("Add") { on press { count += 1 } }
  }
}

test "Add counts" {
  let app = mount(Counter())
  app.tap("Add")
  app.tap("Add")
  app.expect("Count: 2")
}
```

```sh
asuka test                    # このディレクトリのアプリまたはパッケージ
asuka test --run Add          # 名前に Add を含むテスト
asuka test path/to/app
```

```
ok   Add counts
FAIL Remove empties the list (app.asuka:41)
     the screen doesn't show "No items"
     View 0,0 390x844
       Text 0,0 390x20 "Item 1"
asuka: 1 of 2 tests failed
```

テストは CLI が動くところならどこでも、CI でも、シミュレーターやエミュレーターなしで実行できます。`asuka test` はパッケージをテストと一緒にコンパイルし、このコンピューター上で実行します。初回の実行ではテストランナーをビルドするので、C コンパイラーが必要です。テストが失敗すると、終了コード 1 で終了します。プラットフォームの実際のビューを使う場合は、後述の[プラットフォームでのテスト](#プラットフォームでのテスト)を参照してください。

`asuka run file.asuka` は、ファイルを一つだけコンパイルして同じように実行します。テストがあればまずそれを実行し、次にそのアプリを動かして、画面に表示されているものを出力します。アプリを作らずに、コンポーネントや関数を試すのに使えます。

## 仕組み

`mount` は、ビューをプラットフォームの代役にマウントします。代役はプラットフォームのビューを作る代わりに、アプリが作るビューを保持します。それ以外はすべて、実際に動くときのアプリそのままです。状態、エフェクト、タスク、Asuka のレイアウト、ナビゲーション、行をバインドするリスト、シート、アラートが動きます。

アプリは iPhone（iOS、390 × 844 ポイント）で動いているものとして実行されます。`asuka test --platform android`（または `mac`、`windows`、`linux`、`web`）を付けると、別のプラットフォームで動いていることになり、プラットフォームごとに変わるアプリの動作を確かめられます。テストもそのプラットフォーム向けにコンパイルされ、各パッケージのそのプラットフォーム向けのファイルが含まれ、`@platform` もそのプラットフォームになります。テストは `asuka dev` と同じく開発ビルドで、[`@debug`](language/control-flow.md#開発ビルド向けのコード) は true です。`asuka test --release` を付けると、出荷するアプリと同じく false になります。

プラットフォームと異なるのは、テキストの測り方です。フォントに関係なく、1 文字 8 ポイント、1 行 20 ポイントとして測ります。フレームは Asuka の実際のレイアウトから得られるので、ビュー同士の位置関係は正しく保たれますが、ポイント単位の正確なサイズは実機と一致しません。

時間が進むのは、ほかに実行できるものが何もないときだけです。テスト内の `sleep(5s)` やタスクのタイマーは時間を使いません。来ることのない何かを待つテストは、すぐに失敗します（「the test never finished」）。

アプリの実行中にコアが報告するもの（意味もなく別の load を待った load、何も示さない長い保持など。[エラーとそのコード](development.md#エラーとそのコード)を参照）は、`asuka test` が起きた場所でコードと一緒に表示します。`asuka test --json` は、それと各テストの結果（プロパティテストが失敗したときはその入力も）、各ベンチマークの結果、各ファイルのカバレッジを 1 行ずつの JSON で書き出します。

`asuka test` は例によるテストのほかにも、生成した入力で[プロパティテスト](#プロパティテスト)を実行し、それを[ファジング](#ファジング)し、値を[スナップショット](#スナップショット)と比べ、[ベンチマーク](#ベンチマーク)を計測し、[カバレッジ](#カバレッジ)を測ります。アプリでも、パッケージでも、コマンドラインのプログラムでも同じように使えます。

## 画面に表示されているもの

テストでは、ユーザーに見えるテキストで対象を指定します。テキスト、ボタンのタイトル、フィールドのテキストやプレースホルダー、アクセシビリティラベル、画面のタイトル、タブ、アラートのボタンです。テキストは完全一致で照合されます。

```asuka
app.expect("Inbox")       // 表示されていなければテストを失敗させる
app.expectNo("Message 1") // 表示されていればテストを失敗させる
print(app.snapshot())     // 画面に表示されているもの（1 行に 1 ビュー）
```

対象になるのは、ユーザーに見えるものだけです。

- 各スタックのいちばん上の画面（その下の画面は見えません）
- 選択されているタブのページと、タブバーにあるすべてのタブのタイトル（とバッジ）
- 分割ビューのサイドバー：各行き先のタイトルとバッジ
- ほかのものの上に表示されているシート、アラート、ポップオーバー
- リストの高さに収まる行。Asuka はプラットフォームに求められたときにリストの行をバインドします。`asuka test` もコレクションビューと同じように行を求めます
- 非表示のものは含みません

チェックが失敗すると、メッセージに画面の内容が含まれます。表示されている各ビューが、種類、フレーム、テキストとともに並ぶので、代わりに何が表示されていたかがわかります。また、`asuka test` はテストの場所（`app.asuka:41`）を示します。

## ユーザーの操作

各操作は、アプリが落ち着くのを待ってから戻ります。操作で変わったものはレイアウトされ、リストはバインドされ、アプリのタスクがポストした処理は実行済みになります。

```asuka
app.tap("Add")                      // ボタン、行、タブ、トグル、セグメント、メニューの項目
app.type("ada@example.com", into: "Email")  // そのプレースホルダーかラベルを持つフィールド
app.type("milk", into: "Search notes")      // 画面のバーの検索フィールド（プレースホルダーで探す）
app.tapLink("https://asuka.dev")     // RichText の中の、そこへのリンク
app.share([SharedItem(kind: .link, text: "https://asuka.dev")])  // ほかのアプリが共有：onShare が受け取る
await app.idle()                    // 実行するものがなくなるまで：タスク、タイマー、読み込み
```

タップは、指が触れる場所に届きます。行の中のテキストをタップすると行が、ボタンの中のテキストをタップするとボタンがタップされます。メニューを持つビューをタップするとメニューが開き、次の `tap` でその中から選びます。

## プロパティテスト

パラメーターのあるテストはプロパティテストです。パラメーターの型のどんな値に対しても成り立つ性質を書き、`asuka test` が値を生成して実行します。

```asuka check
struct Span {
  start: int
  end: int
}

fn merge(a: Span, b: Span) -> Span {
  return Span(start: min(a.start, b.start), end: max(a.end, b.end))
}

test "a merge covers both spans" (a: Span, b: Span) {
  let m = merge(a, b)
  expect(m.start <= a.start && m.start <= b.start)
  expect(m.end >= a.end && m.end >= b.end)
}

test "reversing twice gives the list back" (xs: [int], s: string) {
  expect(xs.reversed().reversed() == xs)
  expect(s.reversed().reversed() == s)
}
```

各プロパティテストは 100 個の入力で実行されます。失敗する入力が見つかると、`asuka test` はそれを縮小（shrink）します。より単純な入力（短いリストや文字列、小さな数、`none`、列挙型の最初のケース）を順に試し、まだ失敗するもののうちいちばん単純なものを、それを見つけたシードと一緒に表示します。

```
FAIL a page holds two rows at most (pages.asuka:9)
     expected page.count() <= 2 (pages.asuka:10): got 3 and 2
     input (case 4 of seed 0, shrunk in 3 steps):
       rows = ["", "", ""]
     rerun it: asuka test --run 'a page holds two rows at most' --seed 0
```

入力はシードから作られます。`--seed` で別のシードを指定しないかぎりシードは 0 なので、どの実行でも（CI でも）同じ入力が試され、失敗は再現します。`--seed random` は新しい入力を試し、使ったシードを表示します。`--cases 1000` でより多くの入力を試せます。生成される値は次のとおりです。

| 型 | 値 |
| --- | --- |
| `int`、`Duration` | 小さな数がもっとも多く、次に ±1000 までの数、境界の値（0、±1、255、65535、32 ビットと 64 ビットの上限と下限）、任意の値 |
| `float` | 整数、小数、任意の有限の値、そして ±0、正負の無限大、NaN |
| `bool` | 両方 |
| `string` | 英字がもっとも多く、次に ASCII の任意の文字、ほかの文字体系、絵文字、結合文字、NUL、改行、引用符、波かっこ。最大 64 文字 |
| `bytes`、`[T]`、`Set<T>`、`[string: T]` | 最大 64 要素で、要素の少ないものがもっとも多い |
| `T?` | `none` か `T` の値 |
| struct、列挙型 | 各フィールドの値。各ケースとその値。自分自身を含む型は、ある深さで入れ子が止まる |

パラメーターはデータでなければなりません。ビュー、クロージャー、ストアには生成できる値がありません（[test-parameter](errors/test-parameter.md)）。ほかに必要なものは、受け取った値からテストの中で作ります。性質の対象外の入力では、テストから早めに return します：`if xs.isEmpty() { return }`。

## ファジング

```sh
asuka test --fuzz                       # 各プロパティテストを 10 秒ずつ
asuka test --fuzz --run parse --fuzztime 5m
asuka test --fuzz --fuzztime 100000x    # それぞれ 100,000 個の入力
```

`--fuzz` はプロパティテストだけを実行し、`--fuzztime`（時間、または入力の数）が尽きるまで、それぞれを新しい入力で実行し続けます。各入力がパッケージのどのコードに到達したかを見て、ほかの入力が到達しなかったコードに届いた入力を変化させ続けるので、ランダムな入力ではまず見つからないもの（パーサーの深い分岐、フォーマットの細かなケース）を見つけられます。失敗した入力は縮小して表示し、JSON として `testdata/fuzz/<test>/` に保存します。

```json
{
  "s": "abcd"
}
```

これはコミットしてください。`asuka test` は毎回、生成する入力の前に、テストの保存済みの入力を実行します。そのため、一度直したものは直ったままになります。このファイルは自分で書くこともできます。テストのパラメーターを名前で並べた JSON オブジェクトで、JSON に数として書けない浮動小数点数は `NaN`、`Infinity`、`-Infinity` と書きます。

## スナップショット

`expectSnapshot(value)` は、値を出力したものをその値のために保存されたスナップショットと比べ、異なればテストを失敗させます。最初の実行でスナップショットが `testdata/snapshots/`（ソースファイルのテストごとに 1 ファイル）に書き出されるので、それをコミットします。

```asuka
test "the list shows today's todos" {
  expectSnapshot(store.today())
  let app = mount(TodoList())
  expectSnapshot(app.snapshot())   // 画面に表示されているもの
}
```

```
snapshot "the list shows today's todos 1"
  [
    Todo(title: "Buy milk", done: false, tags: ["errands"]),
    Todo(title: "Call Ada", done: true, tags: []),
  ]
```

`expectInlineSnapshot(value)` はスナップショットを呼び出しの中に置きます。最初の実行で、`asuka fmt` と同じ書式でそこに書き込まれます。

```asuka
expectInlineSnapshot(total(cart))
// 最初の実行のあと：
expectInlineSnapshot(total(cart), "42.5")
```

文字列はそのままがスナップショットになります。ほかの値は、言語での書き方で出力されます：`"a"`（エスケープ付き）、`1.0`、`1500ms`、`[1, 2]`、`["a": 1]`、`Set([1])`、`Todo(title: "Milk", done: false)`、`.circle(radius: 2.0)`、`none`。マップのエントリーとセットの要素は並べ替えられるので、追加した順番には左右されません。1 行に収まらないときは、要素ごとに 1 行になります。

値がスナップショットと異なると、テストは失敗し、どう違うかを示します。

```
FAIL the list shows today's todos (todos.asuka:12)
     expectSnapshot (todos.asuka:13): what it shows isn't its snapshot (asuka test --update writes what it shows)
     - the snapshot
     + what it shows
       [
     -   Todo(title: "Buy milk", done: false, tags: ["errands"]),
     +   Todo(title: "Buy oat milk", done: false, tags: ["errands"]),
         Todo(title: "Call Ada", done: true, tags: []),
       ]
```

`asuka test --update`（`-u`）は、代わりにテストが示したものを書き込みます。すべてのテストが実行されて成功したときは、どのテストも使わなくなったスナップショットも削除します。CI（`CI` が設定されている環境）では、まだ保存されていないスナップショットは書き込まれず、テストが失敗します。プロパティテストではスナップショットを使えません。生成したすべての値で実行されるからです。

## ベンチマーク

`bench` はベンチマークで、その処理にかかる時間を計測します。

```asuka check
fn fib(n: int) -> int {
  if n < 2 { return n }
  return fib(n - 1) + fib(n - 2)
}

bench "fib 20" {
  keep(fib(20))
}

bench "sorting 1,000 ints" {
  var xs: [int] = []
  for i in 0..<1000 {
    xs.push(i * 7919 % 1000)
  }
  measure {
    keep(xs.sorted())
  }
}
```

```sh
asuka test --bench                 # すべてのベンチマーク
asuka test --bench --run sort      # 名前に sort を含むもの
```

```
bench fib 20                    109µs/op ±2%          0 allocs/op        0 B/op  (10 × 520)
bench sorting 1,000 ints       14.7µs/op ±4%          3 allocs/op    15.7 KB/op  (10 × 3,964)
```

`--bench` は各ベンチマークの本体を何度も実行します。まず 1 回、そのあとは回ごとに回数を増やし、1 回の実行が `--benchtime`（1s）の 10 分の 1 に達するまで続けます。それからその回数で 10 回実行し、1 回あたりの時間の中央値、もっとも速い回と遅い回が中央値からどれだけ離れているか、1 回あたりに割り当てたオブジェクトの数とバイト数を表示します。`measure { … }` より前は準備で、時間に含まれません。`measure` がなければ本体全体を計測します。`keep(x)` の値は `x` です。計算の結果を渡してください。結果を誰も読まない処理は、コンパイラーが省いてしまうことがあります。

ベンチマークはリリースビルドと同じく C にコンパイルして実行され（`@debug` は false）、アプリが出荷するコードそのものを計測します。`--vm` を付けると、`asuka dev` が使う VM で実行します。時間は計測するだけで、チェックには使いません。ベンチマークが失敗するのは、それ自身のチェック（`expect`）やエラーのときだけです。また、通常の `asuka test` は各ベンチマークをテストとして 1 回だけ実行し、まだ動くことを確かめます。`--benchtime 100x` は、ちょうど 100 回の反復を 1 回だけ実行します。

ビューをマウントするベンチマークは、反復ごとに画面まで届いたもの、つまり実機がこなすことになる処理も表示します。プラットフォームに送られた変更のバッチ、その操作の数とバイト数、プラットフォームが計測したビューの数、レイアウトし直したビューの数です。アプリはユーザーと同じように操作する（`app.tap`）か、フレームの中で動かします（`app.frame` と、アプリ側の `onFrame`）。そうすれば、タップのときと同じく 1 回の反復の変更が 1 つのバッチになります。

```asuka
bench "a new row" {
  let app = mount(Inbox())
  measure { app.tap("Add") }
}
```

```
bench a new row                 41.2µs/op ±3%         84 allocs/op     6.1 KB/op  (10 × 2,410)  host: 1 batches, 9 ops, 312 B, 2 measures; 6 nodes laid out
```

ベンチマークの中では、`app.ops()` に計測部分が送った操作は含まれません。

変更の効果を見るには、ある実行の結果を残しておいて比べます。

```sh
asuka test --bench --json > before.json
# … コードを変更する …
asuka test --bench --compare before.json
```

すると各行の末尾に、変更前の時間と変化の割合（`was 24.1µs/op: -15.8%`）が付きます。2 回の実行のばらつきの範囲が重なるときは「within noise」と表示され、その差がノイズかもしれないことを示します。

## カバレッジ

```sh
asuka test --cover                            # ファイルごとの要約
asuka test --cover-lcov coverage/lcov.info    # エディターや CI 向け
asuka test --cover-html coverage.html         # ファイルごとに、実行されたところ
```

```
coverage: 85.7% of lines (12 of 14), 75.0% of branches (6 of 8)
  todos.asuka   85.7%  lines 12/14  branches 6/8  not run: 31, 40-41
```

`--cover` は、テストの実行中にパッケージのコードのどの行が実行されたか（パッケージ自身のコードだけで、テストの本体やインポートしたものは含みません）と、各分岐を数えます。分岐とは `if` の各分岐（書かれていない `else` も含む）と `match` の各アームです。行の中で文が 1 つでも実行されれば、その行は実行されたことになります。その行の中で実行されなかった部分は、分岐のデータでわかります。どこからも呼ばれない関数は、一度も実行されなかったものとして数えます。プロジェクトのプログラム（`[bin.<name>]`）は、それぞれ自身のテストで計測します。

`--cover-lcov` は lcov 形式で書き出します。エディター（VS Code の Coverage Gutters）や CI サービス（Codecov、Coveralls）が読み込めます。`--cover-html` は、ソースを表示し、実行されたところを緑、実行されなかったところを赤で示すページを書き出します。`--cover` を付けなければ、何も数えません。

## プラットフォームのサービスとネイティブパッケージ

[ネイティブパッケージ](native.md)のインターフェースはプラットフォームが実装するもので、`asuka test` にはありません。呼び出すと、そのサービスがないプラットフォームと同じように例外が投げられます。コンポーネントが使うものはストアか `provide` で渡し、テストでは独自のものを渡します。

```asuka
test "archiving removes the row" {
  let app = mount(Inbox(), provide: Mailbox.fake([sample]))
  await app.idle()
  app.tap("Archive")
  app.expectNo("Hello")
}
```

`provide:` にはストアを 1 つ渡します。ビューが複数のストアを使うときは、リストで渡せます：`mount(Inbox(), provide: [Mailbox.fake([sample]), Session()])`。

## プラットフォームでのテスト

`asuka test` がテストするのは、アプリの振る舞い、つまり何が表示され、ユーザーが操作したときに何が起きるかです。それを各プラットフォームがどう描画するかはプラットフォームの領分です。各プラットフォームでアプリを確かめるには、次の方法があります。

- `asuka dev --platform ios|android|web` で、シミュレーター、エミュレーター、実機、ブラウザーでアプリを動かし、保存するたびにリロードします。
- `asuka test --gui` で、そこでアプリのフロー（ユーザーがアプリで行う一連の操作）を実行します。[GUI テスト](#gui-テストフロー)を参照してください。
- デスクトップでは、`ASUKA_SNAPSHOT=out.png` を指定すると、ウィンドウが表示されたらフォーカスを奪わずに PNG に書き出して終了します（`ASUKA_DUMP=1` でビューツリーも隣に書き出します）。`ASUKA_SCRIPT` を使うと、先に操作（`tap:Add`、`type:hello`）を実行できます。[開発](development.md#スナップショットとスクリプト)を参照してください。
- Linux では、Xvfb（`xvfb-run -a dbus-run-session`）の下で実行すれば、ディスプレイのない CI でも同じことができます。

## GUI テスト：フロー

フローは、ユーザーがアプリで行う一連の操作で、プラットフォームの開発用アプリの上で動かします。シミュレーター、エミュレーター、スマートフォン、ヘッドレス Chrome、またはこのコンピューターです。フローはそれぞれアプリの `flows` フォルダーにあるファイルで、一行に一ステップを書きます。

```
# flows/note.flow：シートでメッセージにメモを書く。
tap Message 20
see No note yet.
tap Add a note
type hello
tap Save
see Note: hello
back
notsee Note: hello
```

`asuka test --gui` は、`asuka dev` と同じようにアプリを動かしてフローを実行します。どのフローも、すべての `state` が初期値の状態から始まり、結果がフローごとに表示されます。

```
asuka test --gui --platform ios       # シミュレーター（--device で選ぶ）
asuka test --gui --platform android   # エミュレーターかスマートフォン
asuka test --gui --platform web       # ヘッドレス Chrome
asuka test --gui                      # このコンピューター
```

| ステップ | 動作 |
|---|---|
| `tap <text>` | そのテキストを表示しているものを押します。ボタン、トグル、押せるビュー（行など）、またはテキストフィールドで、テキストフィールドならその後の入力を受け取ります。 |
| `type <text>` | 最後にタップしたテキストフィールド（なければ最初のもの）に、ユーザーが打つのと同じように入力します。 |
| `submit` | そのフィールドでリターンを押します。 |
| `back` | 戻るボタンと同じように、いちばん上の画面から戻ります。 |
| `tab <title>` | タブバーでタップするのと同じように、そのタイトルのタブを選びます。 |
| `scroll` | 画面上のリストを 1 ページ下へスクロールします。 |
| `scrollto <text>` | 何かがそのテキストを表示するまで、リストを下へスクロールします。 |
| `see <text>` | 画面上の何かがそのテキストを表示しています。 |
| `notsee <text>` | 画面上の何もそれを表示していません。 |
| `wait <time>` | 待ちます（`500ms`、`2s`）。 |

テキストは、ビューが表示しているもの、そのラベル（アイコンだけのボタン）、フィールドのプレースホルダー（`tap New note`）のいずれかで、全体で照合します。`see Message 1` は `Message 19` には一致しません。テキストの中の `*` は任意の文字に一致するので、変わるテキストに使います（`see Count: *`）。リストは画面上の行にだけビューを作るので、もっと下の行は `scroll` か `scrollto` で画面に入れると表示されます。

失敗したステップは、通るか `--timeout`（10s）が過ぎるまでやり直します。アプリがまだ読み込み中だったり、ネットワークを待っていたりするためです。時間が過ぎるとフローは失敗し、そのステップの行と、画面に表示されているものが示されます。いちばん上の画面の下にある画面や、選ばれていないタブは、表示されているとは見なしません。`--run text` は、名前にそのテキストを含むフローだけを実行します。ステップは、プラットフォームのイベントが届くときと同じように Asuka 自身のビューを通るので、フローはどこでも同じように動き、表示されるのはプラットフォームのウィジェットです。
