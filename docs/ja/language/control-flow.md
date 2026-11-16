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

タプルパターン（`(true, 0)`）は、タプルの要素ごとにそれぞれのパターンでマッチさせます。

`for` で回せるのは、配列、マップの `(key, value)` の組、範囲です。`a..<b` は `a` から `b` の手前までの整数、`a..b` は `b` を含む整数です（`0..<3` は `[0, 1, 2]`。範囲を `for` で回しても配列は作られません）。`while cond { … }` は cond が成り立つあいだ繰り返します。`break` はループを抜け、`continue` は次の周回に進みます。

`if let` と `while let` は、オプショナルの中の値を取り出すか、パターンにマッチさせ、取り出せたときだけブロックを実行します（`if let u = user { … }`、`while let job = queue.pop() { … }`、`if let .ready(v) = state { … }`）。条件は、カンマで区切った節の並びにもできます。`let` とブール値の条件は、どんな順番で並べてもかまいません。各節では、それより前の節で束縛した名前を使えます。ブロックが実行されるのはすべての節が成り立ったときです（節は順に評価され、成り立たない節が出たところで止まります）。

```asuka
if let user = session.user, user.isAdmin, let team = teams[user.teamId] {
  Text("{team.name}'s admin")
} else {
  Text("Not an admin")
}
```

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

## 特定のプラットフォーム向けのコード

`@platform` は、アプリのビルド先のプラットフォームを表す `Platform` 型の値です（`.iOS`、`.android`、`.macOS`、`.windows`、`.linux`、`.web`）。これはコンパイル時に決まる定数で、追跡されません。ファイルのトップレベルやコンポーネントの本体も含め、値を書ける場所ならどこでも使えます。`@platform` で決まる分岐のうち、あるプラットフォームが通らない側は、そのプラットフォームのビルドに含まれません。コードも、文字列も、ビューも含まれません。

```asuka
let saveKey = if @platform == .macOS { "⌘S" } else { "Ctrl+S" }

component Toolbar {
  prop link: string

  Row {
    if @platform == .iOS || @platform == .android {
      Button("Share") { on press { share(link) } }
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
