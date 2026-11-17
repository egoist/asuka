# コンポーネント

コンポーネントは UI の部品です。パラメーターはヘッダーで宣言し、それ以外はすべて本体に書きます。

```asuka
component MessageRow(message: Message) {
  prop compact = false            // デフォルト値付きの名前付きプロパティ
  prop accessory: View? = none    // スロット：呼び出し側が渡すビュー
  event archive                   // コンポーネントが知らせるイベント
  state expanded = false          // コンポーネント自身の状態

  Row {
    gap: 12; padding: [12, 16]
    Avatar(message.from)
    Column {
      grow: 1
      Text(message.subject) { font: .headline; lines: 1 }
      if !compact { Text(message.preview()) { color: .secondaryLabel } }
    }
    accessory
  }
  contextMenu {
    Button("Archive") { role: .destructive; on press { emit archive } }
  }
}
```

使い方は組み込みの要素と同じです。ヘッダーのパラメーターは丸括弧の中に、それ以外はすべてブロックの中に書きます。

- プロパティ：`name: value`
- ハンドラー：`on name { … }`
- 双方向バインディング：`bind name: target`
- スタイル：`style: card`、またはリストで `style: [card, pink]`
- 子

```asuka
MessageRow(m) {
  compact: true
  accessory: Badge(3)
  on archive { mailbox.archive(m.id) }
}
```

どれも書く場所は一つだけです。ヘッダーのパラメーターをブロックの中で設定することも、プロパティを丸括弧の中で渡すこともできません。

子を受け取るコンポーネントは `prop children: View` を宣言します。呼び出し側のブロックの中身がそこに入ります。名前付きのスロットは、ブロックの中に `name { … }` と書きます（`toolbar { … }`）。

`emit name(args)` はイベントを発生させ、呼び出し側の `on name(args) { }` がそれを処理します。誰も処理しないイベントは何もしません。イベント名を値として読むと、呼び出し側がそれを処理しているかどうかになります（`if click { … }`：たとえばトレイアイコンは、呼び出し側がクリックを引き受けなければクリックでメニューを開きます）。

ひとつの要素だけを表示するコンポーネント（本体のビューがその要素ひとつ、またはそうなっている別のコンポーネント）は、スタイルのプロパティも受け取ります。呼び出し側が渡したスタイルはその要素に付き、コンポーネント自身がそこに設定した値より優先されます。

```asuka
component Card(title: string) {
  Column {
    padding: 16; radius: 12; background: .secondaryBackground
    Text(title)
  }
}

Card("Plan") { padding: 8; width: 240 }   // padding は 8、radius は 12
```

## コンポーネントが宣言するバリアント

要素を一つだけ表示するコンポーネントは、自分の状態に名前を付けて、呼び出し側がスタイルを当てられるようにできます。`variant name = condition` と書くと、`let` と同じように常に最新に保たれる bool になります。呼び出し側はブロックに `name { … }` と書き、条件が成り立つあいだ、そのスタイルプロパティがその要素に重ねられます。`View` が押されているあいだ `pressed { … }` が重ねられるのと同じです。

```asuka
pub component Checkbox {
  prop checked = false
  event checkedChange(checked: bool)
  prop disabled = false
  prop children: View

  pub variant checked = checked
  pub variant disabled = disabled

  View {
    role: .button
    on press { if !disabled { checked = !checked } }
    children
  }
}

Checkbox {
  bind checked: agree
  padding: 4; radius: 4; border: 1
  checked { background: .accent; borderColor: .accent }
  disabled { opacity: 0.4 }
  Text("I agree")
}
```

- バリアントの名前は独自の名前空間にあります。ここの `checked` のように、プロパティや状態と同じ名前にできます。ただしインタラクションの状態の名前（`pressed`）やキーワードは使えません。
- バリアントは、その要素のほかのすべてのスタイルより優先されます。要素自身のスタイル、呼び出し側のスタイル、インタラクションの状態のどれよりも上です。二つが同時に成り立つときは、あとで宣言したほうが勝ちます（ここでは `disabled` が `checked` に勝ちます）。
- `pub variant` はほかのパッケージの呼び出し側もスタイルを当てられます。`pub` でないバリアントは、宣言したパッケージの中だけのものです。
- スタイルにもバリアントのブロックを書けます（`style box { checked { … } }`）。そのバリアントを宣言しているコンポーネントの要素に適用すると、ブロックはバリアントに追従します。それ以外の場所では何もしません。
- コンポーネントが表示する要素がコンテナなら、呼び出し側はコンポーネントのブロックにインタラクションの状態（`pressed { … }`、`hovered { … }`）も書けます。
- `Checkbox as box` は、表示している要素のビューに名前を付けます。`box.checked` でバリアントを読め、`card.hovered` で状態を読むのと同じく追跡されます。

## コンポーネントの本体は宣言

コンポーネントの本体は、一度実行したら忘れられるコードでは**ありません**。一行一行が、ずっと成り立ち続けることを宣言しています。

- `state x = …` は、コンポーネントが持つ状態です。
- `let y = …` は、読んだものから導かれる値で、常に最新に保たれます（読まれたときにだけ計算され、読んだものが変わったときにだけ計算し直されます）。
- すべての要素のすべてのプロパティの値も、同じように最新に保たれます。`Text("Count: {count}")` は `count` が変わると変わり、ほかのものは何も変わりません。
- `if`、`for`、`match` は、条件が示すものを表示します。今も、これから先も。

`count` が変わったときに再び実行されるのは、それを読んでいるもの、つまりその `Text` のテキストと `if` だけです。コンポーネントが再実行されることはなく、プラットフォームのビューも、何かが変わった箇所にしか手が入りません。

## ハンドラーと文

コードが実行されるのは、ハンドラー（`on press { … }`）、関数、`task` と `effect` のブロック、そしてテストの中です。そこで状態を読むと現在の値が得られ、書き込むとそれを読んでいるものが更新されます。

```asuka
on press {
  count += 1
  todos.push(Todo(title: draft))
  todos[0].done = true      // todos への書き込み：todos[0] を読んでいる行だけが更新される
  draft = ""
}
```

ハンドラーの中での書き込みは、ハンドラーが終わったとき（または `await` に達したとき）にまとめて画面に反映されます。

文には次のものがあります：`let`、`var`、`let … else`、代入（`=`、`+= -= *= /= %=`、`&= |= ^= <<= >>=`、`??=`）、`if` と `if let`、`for x in xs`（配列かセット）、`for (k, v) in m`、`for (i, x) in xs.enumerated()`、`while` と `while let`、`match`、`return`、`break`、`continue`、`throw`、`do { } catch { }`、`defer { }`、`emit`（[制御フロー](control-flow.md#制御フロー)を参照）、`fn` の宣言（[関数とクロージャ](functions.md#関数とクロージャ)を参照）。

## 双方向バインディング

コントロールは値を表示し、その値を変更します。`bind` で状態を渡すと、その両方を行えます。

```asuka
state query = ""
state notify = true

TextField { bind text: query; placeholder: "Search" }
Toggle { bind on: notify; title: "Notifications" }
```

`bind` を使わない場合、コントロールは変更を知らせるだけです（`TextField { text: name; on change(t) { name = t.trim() } }`）。

コンポーネントのプロパティも、コンポーネントが `event <prop>Change` でそれを知らせるなら、同じようにバインドできます。`prop value` と `event valueChange(value: T)` を持つコンポーネントに `bind value: when` と書くと、状態を渡し、コンポーネントが変更を発行したときに書き戻します。

```asuka
component Rating {
  prop value = 0
  event valueChange(value: int)

  Stepper {
    value: float(value)
    on change(v) { emit valueChange(int(v)) }
  }
}

component Review {
  state stars = 3

  Rating { bind value: stars }
}
```

コンポーネントは、こうしたプロパティに自分の状態のように代入することもできます。ハンドラー、fn、エフェクト、タスクの中で代入でき、要素の `bind` を通した書き込みも代入にあたります。代入の結果は呼び出し側によって変わります。

- バインドされている場合（`bind open: shown`）：代入すると `openChange` が発行され、それが呼び出し側の状態に書き込みます。プロパティはその状態を読み続けます。呼び出し側が書き込まなければ、プロパティは元のままです。
- バインドされていない場合（値を渡したか、何も渡していない）：代入した値はそれ以降コンポーネント自身のものになり、`openChange` を処理する呼び出し側には変更が通知されます。呼び出し側の値が変わると、それがコンポーネントの値を置き換えます。

```asuka
component Disclosure(title: string) {
  prop open = false
  event openChange(open: bool)
  prop children: View

  Column {
    Button(title) { on press { open = !open } }
    if open { children }
  }
}

Disclosure("Details") { Text("…") }                  // コンポーネント自身のもの
Disclosure("Details") { bind open: shown; Text("…") } // 呼び出し側のもの
```

プロパティは丸ごと変わります。`open = …` や `items = items + [x]` とは書けますが、`items.push(x)` とは書けません。対応する change イベントのないプロパティには代入できません。

## ビューのリスト

```asuka
for todo in todos key todo.id {
  TodoRow(todo) { on delete { todos.removeWhere(t => t.id == todo.id) } }
}
```

`for` はキーごとに行を一つ保ちます。項目が変わった行は、自分のバインディングだけを更新します。新しい項目が加わると行が作られ、項目が削除されるとその行は破棄されます。移動した行は作り直されず、移動されるだけです。`List` の直下では、行は画面に表示されている間しか存在しません。そのため 100,000 件のリストでも、コストは見えている分だけです。

## エフェクトとタスク

```asuka
effect { storage.set("draft", draft) }        // すぐに実行し、draft が変わるたびにまた実行する
task { try? await mailbox.refresh() }         // コンポーネントが表示されている間に一度だけ
task(query) { results = await search(query) } // query が変わるたびにもう一度
```

エフェクトはすぐに実行され、読んだものが変わるたびに再び実行されます。タスクは非同期です（後述）。コンポーネントが現れると始まり、丸括弧の中身が変わると前回の実行をキャンセルしてやり直し、コンポーネントがなくなるとキャンセルされます。`app` ブロックにも書けます。アプリ自身のもので、アプリが動いている間ずっと有効です（更新のループや、アプリの状態に追従する設定：`effect { setColorScheme(scheme) }`）。
