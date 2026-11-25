# 制御フロー

`if` と `match` は値としても使えます。値として使う `if` には `else` が必要で、ブロックの値はその最後の式です。

```asuka
let label = if unread > 0 { "{unread} new" } else { "Inbox" }
let icon = match (online, unread) {
  (false, _) => "wifi.slash"
  (true, 0) => "tray"
  _ => "tray.full"
}
```

値から型がわからない分岐（空の `[]` や `[:]`、暗黙の `.case`）は、もう一方の分岐の型になります。`if let c = chosen { [c] } else { [] }` は `[Story]` です。

タプルパターン（`(true, 0)`）は、タプルの要素ごとにそれぞれのパターンでマッチさせます。オプショナルなタプルに対しては、ケースのパターンと同じように、値があればそのタプルにマッチします（`none` は別のアームで扱います）：`if let (name, age) = found { … }`、`let (x, y) = cursor else { return }`。

型つきの名前（`c: Circle`）は、インターフェースの値が何の型なのかを調べます。値がその型のときにマッチし、その型として束縛します。構造体のパターン（`Circle(radius)`）や型を明記した列挙型のケース（`Unit.big(n)`）も同じです。インターフェースの値に対する `match` は `_` で終わらせます。詳しくは[インターフェースとジェネリクス](functions.md#インターフェースとジェネリクス)を参照してください：`if let c: Circle = shape { … }`、`let sq: Square = shape else { return }`。

`for` で回せるのは、配列、セット、マップの `(key, value)` の組、文字列の char（`for c in name`）、遅延シーケンス（`Seq`。[値と型](types.md#遅延シーケンス)を参照）、範囲です。`a..<b` は `a` から `b` の手前までの整数、`a..b` は `b` を含む整数です（`0..<3` は `[0, 1, 2]`。範囲を `for` で回しても配列は作られません）。`while cond { … }` は cond が成り立つあいだ繰り返します。`break` はループを抜け、`continue` は次の周回に進みます。

`for` では、自分で定義した型の値も回せます。一つはイテレーターで、型に `mut fn next() -> T?` があるものです（`for` はそのコピーに対して `none` が返るまで呼び出します）。もう一つはシーケンスで、`fn iter()` がイテレーターか `Seq` を返すものです（`for` は一度だけ呼び出します）。prelude の `Iterator<T>` はそのような型を表し、制約に使えます（`fn sum<I: Iterator<int>>(xs: I) -> int`）。イテレーターは `none` で終わりを表すので、要素そのものが none になりうる（`T?` の）型は `Seq` を返します（`fn iter() -> Seq<int?> = slots.lazy()`）。

```asuka check
struct Node {
  name: string
  children: [Node] = []
}

struct Walk {
  stack: [Node]

  mut fn next() -> Node? {
    let n = stack.pop() else { return none }
    stack = stack + n.children.reversed()
    return n
  }
}

extend Node {
  fn walk() -> Walk = Walk(stack: [self])
}

fn count(root: Node) -> int {
  var n = 0
  for node in root.walk() { if node.name.hasSuffix(".asuka") { n += 1 } }
  return n
}
```

`for` や `while` の先頭の後に `as name` と書くと、ループに名前を付けられます。その内側のどのループからでも、`break name` でそのループを抜け、`continue name` でその次の周回に進めます（抜けるすべてのループの `defer` が実行されます）。クロージャーの中からは使えません。宣言して使わない名前や、宣言せずに使う名前はエラーになります。

```asuka
for row in grid as rows {
  for cell in row {
    if cell == target { found = true; break rows }
    if cell.isEmpty() { continue rows }
  }
}
```

`if let` と `while let` は、オプショナルの中の値を取り出すか、パターンにマッチさせ、取り出せたときだけブロックを実行します（`if let u = user { … }`、`while let job = queue.pop() { … }`、`if let .ready(v) = state { … }`）。条件は、カンマで区切った節の並びにもできます。`let` とブール値の条件は、どんな順番で並べてもかまいません。各節では、それより前の節で束縛した名前を使えます。ブロックが実行されるのはすべての節が成り立ったときです（節は順に評価され、成り立たない節が出たところで止まります）。

```asuka
if let user = session.user, user.isAdmin, let team = teams[user.teamId] {
  Text("{team.name}'s admin")
} else {
  Text("Not an admin")
}
```

`if var` は `if let` と同じですが、束縛する名前が変更できるコピーになります。いくつかのフィールドを変更して書き戻すコードに向いています（`if var m = draft { m.read = true; m.tags.push("seen"); draft = m }`）。

`let … else` は、値を束縛するか、その場を離れます。オプショナルが none のとき（またはパターンにマッチしないとき）は `else` のブロックが実行され、このブロックは必ず離れなければなりません。離れ方は `return`、`throw`、`break`、`continue`、`panic(…)` のいずれかです（先へ進めてしまう場合はコンパイラーがエラーにします）。この文より後では、名前はオプショナルではなく中の値を指します。

```asuka
fn summary(id: string) -> string {
  let message = cache[id] else { return "(deleted)" }
  let .ready(body) = message.body else { return message.subject }
  return "{message.subject}: {body}"
}
```

`throw`、`return`、`break`、`continue` は、値を書く場所にも置けます。値を返す代わりにその場を離れます（型は `never` で、どんな型の場所にも当てはまります）。`??` の後ろに書けば、none のときにどうするかを表せます。

```asuka
let user = cache[id] ?? throw MailError.missing(id)
for line in lines {
  let n = line.toInt() ?? continue
  total += n
}
```

`if` の分岐、`match` のアーム、クロージャの本体にも同じように書けます（`x => x ?? throw Error("empty")`）。

## ないかもしれないものの変更

`var` や状態から始まり、フィールド、添字、`?.` をたどる `?.` チェーンには、代入（`=`、`+=` など）や `mut fn` の呼び出しができます。チェーンが `none` に当たると何も起きません。何も書き込まれず（右辺の値も計算されません）、状態を読む側にも通知されません。

```asuka
inbox[id]?.read = true          // そのメッセージがなければ何もしない
session.user?.name = draft
todos.first()?.toggle()          // エラー：first() が返すのは値で、場所ではない
```

`m[k, default: v]` はマップのエントリーで、エントリーがなければ `v` です。読むときの型は `V`（`V?` ではありません）で、書き込むとき（`=`、`+=`、`mut fn`、`push`）は `m[k]` に書き込みます。`v` が計算されるのはエントリーがないときだけです。数え上げやグループ分けが一行ずつで書けます。

```asuka check
fn tally(words: [string]) -> ([string: int], [int: [string]]) {
  var counts: [string: int] = [:]
  var byLength: [int: [string]] = [:]
  for w in words {
    counts[w, default: 0] += 1
    byLength[w.count(), default: []].push(w)
  }
  return (counts, byLength)
}
```

## 特定のプラットフォーム向けのコード

`@platform` は、アプリのビルド先のプラットフォームを表す `Platform` 型の値です（`.iOS`、`.android`、`.macOS`、`.windows`、`.linux`、`.web`）。これはコンパイル時に決まる定数で、追跡されません。ファイルのトップレベルやコンポーネントの本体も含め、値を書ける場所ならどこでも使えます。`@platform` で決まる分岐のうち、あるプラットフォームが通らない側は、そのプラットフォームのビルドに含まれません。コードも、文字列も、ビューも含まれません。

```asuka check
import platform/device

let saveKey = if @platform == .macOS { "⌘S" } else { "Ctrl+S" }

component Toolbar {
  prop link: string

  Row {
    if @platform == .iOS || @platform == .android {
      Button("Share") { on press { device.share(link) } }
    }
    match @platform {
      .macOS | .windows | .linux => Text("Save with {saveKey}")
      _ => Text("Saved as you go")
    }
  }
}
```

これは `if` とその `else if`、`match @platform`（`@platform` が `.unknown` になることはないので、`.unknown` を扱う必要はありません）、`@platform` の比較に対する `&&`、`||`、`!`、そしてビューの中の `if` に当てはまります。ビューの中の `if` では、もう一方の分岐のビューは作られることすらありません。ただし、どのプラットフォーム向けにビルドしても、すべての分岐がチェックされます。プラットフォームの API はどのプラットフォームでも同じ宣言なので、Linux 向けの分岐にある誤りは、iOS 向けにビルドするときもエラーになります。あるプラットフォーム専用の宣言は、専用のファイルに分けたほうがわかりやすいことがよくあります。[特定のプラットフォーム向けのファイル](packages.md#特定のプラットフォーム向けのファイル)を参照してください。

`environment().platform` は、ビューが動いているプラットフォームをシステムが伝えるものです（テストでは `mount(…, platform:)` が伝えます）。ビューのために追跡されます。プラットフォームごとにどのコードをビルドするかは `@platform` で決めます。

## 開発ビルド向けのコード

`@debug` は、アプリが開発ビルドかどうかを表す `bool` です。`asuka dev`、`asuka run`、`asuka test` では true、`asuka build` と `asuka publish` が作るものでは false になります。これも定数で、`@platform` と同じように畳み込まれます。リリースビルドが通らない側、たとえばデバッグメニュー、ステージングサーバーのアドレス、開発中だけ必要なログは、リリースビルドに含まれません。

```asuka check
let server = if @debug { "https://staging.example.com" } else { "https://example.com" }

component About {
  prop version: string

  Column {
    Text("Version {version}")
    if @debug && @platform == .iOS {
      Text("Development build, talking to {server}")
    }
  }
}
```

`!@debug` も畳み込まれ、`@platform` の比較と `&&`、`||` で組み合わせた場合も同じです。通る側の分岐が return するなら、その後ろのコードも含まれません。`if !@debug { return cached }` と書けば、ブロックの残りはリリースビルドに入りません。`@platform` の分岐でも同じです。どのビルドでも両方の分岐がチェックされるので、開発ビルドの側で作業しているあいだに、リリースビルドの側が知らないうちに壊れることはありません。

テストは `asuka dev` と同じく開発ビルドです。`asuka test --release` を付けると、出荷するアプリと同じく `@debug` を false にしてテストを実行します。プログラムなら `asuka run --release` も同様です。ベンチマーク（`asuka test --bench`）はリリースビルドが実行するものを計測するので、`@debug` は false です。
