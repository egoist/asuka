# Asuka のツアー

Asuka のアプリは Asuka で書きます。各プラットフォーム自身のウィジェットを使うインターフェースのために作られた、小さな言語です。静的型付けで、リリースビルドでは事前にコンパイルされ、開発中は保存するとその場で再読み込みされます。このページでは、言語に出会う順に見ていきます。それぞれの全体は[言語リファレンス](language/README.md)にあります。

```asuka
component Counter {
  state count = 0

  Column {
    gap: 12; padding: 24
    Text("Count: {count}") { font: .title }
    Button("Add") { on press { count += 1 } }
  }
}

app { Counter() }
```

ファイルは宣言でできています：コンポーネント、関数、型、状態。`app { }` がアプリのルートで、それ以外の順番は自由です。

## 値

```asuka
fn values() -> string {
  let name = "Asuka"               // 定数：型は推論される
  var visits = 0                   // 変数
  visits += 1
  let greeting = "Hello, {name}"   // {…} で値を文字列に埋め込む
  let ratio: float = 2.5
  let tags = ["new", "work"]       // 配列：[string]
  let ages = ["ana": 31, "bo": 28] // マップ：[string: int]
  let nickname: string? = none     // オプショナル：文字列か none
  let shown = nickname ?? name     // none のときの代わりの値
  return "{greeting} {visits} {ratio} {tags.count()} {ages["ana"] ?? 0} {shown}"
}
```

`var` は関数とハンドラーの中で使います。ファイルのトップレベルにあるものは、`state` でない限り変わりません。値は常にコピーされ、共有されることはありません。配列のコピーを変えても元の配列はそのままです。文字列、配列、マップには思いつくメソッドがそろっています（`count()`、`contains`、`map`、`filter`、`sorted`、`split` など）。[値と型](language/types.md)を参照してください。

## 関数

```asuka
fn area(width: float, height: float = 1) -> float {
  return width * height
}

fn double(x: int) -> int = x * 2     // 式ひとつ

let a = area(3, height: 4)           // 順番に、そのあと名前で
let squares = [1, 2, 3].map(x => x * x)
```

引数は順番に渡し、そのあと名前で渡します（`area(3, height: 4)`）。既定値のある引数は省略できます。`x => …` はクロージャです。[関数とクロージャ](language/functions.md)を参照してください。

## 構造体と列挙型

```asuka
struct Message {
  subject: string
  read: bool = false

  fn preview() -> string = subject.slice(0, 40)
}

enum Filter {
  all
  unread
  from(sender: string)
}

fn label(f: Filter) -> string = match f {
  .all => "Everything"
  .unread => "Unread"
  .from(sender) => "From {sender}"
}

let m = Message(subject: "Hello")
let n = label(.from(sender: "ana"))
```

構造体はフィールドとメソッドからなり、列挙型のケースは値を持てます。`match` はすべてのケースを扱う必要があります（漏れていれば、どれが足りないかをコンパイラーが教えてくれます）。`if`、`match`、ブロックも値です。[宣言](language/declarations.md)と[制御フロー](language/control-flow.md)を参照してください。

## コンポーネントと状態

```asuka
component Greeting(name: string) {
  prop excited = false                // prop と、その既定値
  state draft = ""                    // コンポーネント自身の状態

  Column {
    gap: 8
    Text(if excited { "Hi, {name}!" } else { "Hi, {name}" })
    TextField { bind text: draft; placeholder: "Reply" }
    if !draft.isEmpty() {
      Button("Send") { on press { draft = "" } }
    }
  }
}

app { Greeting("Ana") { excited: true } }
```

コンポーネントのヘッダーは引数を受け取り、ブロックには prop、状態、ビューが入ります。状態が変わると、それを読んでいるところだけが更新され、差分の計算はありません。`bind` は双方向バインディング、`on press` はイベントの処理で、`if` と `for` がどのビューがあるかを決めます。[コンポーネント](language/components.md)を参照してください。複数のコンポーネントで共有する状態は [ストア](language/stores.md) です。

## 非同期処理とエラー

```asuka
enum LoadError: Error {
  offline
  server(code: int)
}

async fn fetchCount(online: bool) throws -> int {
  if !online { throw LoadError.offline }
  await sleep(1s)
  return 42
}

component Badge {
  state count = 0
  state problem = ""

  task {
    do {
      count = try await fetchCount(online: true)
    } catch LoadError.offline {
      problem = "You're offline"
    } catch e {
      problem = e.message
    }
  }

  Text(if problem.isEmpty() { "{count}" } else { problem })
}
```

`async` の処理は画面を止めずに進み、`task { }` はそれを始めたコンポーネントのものです。コンポーネントがなくなるとキャンセルされます。失敗しうる関数は `throws` と書き、呼び出す側は `try` を付けます（エラーのときに none でよければ `try?`）。[非同期処理](language/async.md)と[エラー](language/errors.md)を参照してください。

## パッケージ

```asuka
import std/time

let today = time.now().format("EEE d MMM")
```

`pub` を付けた宣言はほかのパッケージから見えます。標準ライブラリ（`std/…`）とプラットフォーム SDK（`platform/…`）は CLI に同梱されているので、import するだけで使えます。`tray_linux.asuka` という名前のファイルは Linux でだけパッケージに含まれ、`@platform == .linux` は定数なので、あるプラットフォーム向けのコードはほかのプラットフォームのビルドに入りません。[ファイル、パッケージ、インポート](language/packages.md)、[標準ライブラリとプラットフォーム SDK](stdlib.md)、[特定のプラットフォーム向けのコード](language/control-flow.md#特定のプラットフォーム向けのコード)を参照してください。

## テスト

```asuka
component Counter {
  state count = 0
  Button("Add {count}") { on press { count += 1 } }
}

test "adding counts" {
  let app = mount(Counter())
  app.tap("Add 0")
  app.expect("Add 1")
}
```

`asuka test` はデバイスなしでパッケージの `test` ブロックを実行します。`mount` がビューを表示し、テストはタップや入力をして、表示されている内容を確認します。[テスト](language/tests.md)を参照してください。

## 次に読むもの

- [言語リファレンス](language/README.md)：型とそのメソッドから、スタイル、ナビゲーション、ネイティブコードまで、それぞれの全体。
- [コンポーネント](components.md)：アプリを形作る要素。
- [基本概念](concepts.md)：何が更新されるか、オーナー、メインスレッド。
