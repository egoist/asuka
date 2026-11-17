# スタイル

すべての要素がスタイルのプロパティを受け取ります。まずレイアウト（`direction`、`justify`、`align`、`gap`、`padding`、`margin`、`width`、`height`、`grow`、`shrink`、`absolute`、`top`、`grid` など）、次に見た目（`background`、`radius`、`border`、`opacity`、`shadow`、`font`、`color`、`lines` など）です。詳しくは[レイアウトとスタイル](../layout.md)を参照してください。

`style` は再利用するプロパティをまとめて宣言します。`style:` で 1 つのスタイル、またはスタイルのリストを適用します。

```asuka
style card {
  padding: 16; radius: 12; gap: 8
  background: .secondaryBackground
}

style pink { background: #ff2d55 }

View { style: [card, pink]; opacity: 0.9 }
```

2 つの項目が同じプロパティを設定するときは、後の項目が優先されます。ブロック自身のプロパティは、`style:` をブロックのどこに書いても、リスト全体より優先されます。スタイルも同じ書き方でほかのスタイルを適用でき、自身のプロパティがそれらより優先されます。

```asuka
style largeCard { style: card; padding: 24 }
```

スタイルは `fn` と同じようにパラメーターを取れます（デフォルト値も、名前付きの引数も使えます）。パラメーターは使われる場所でバインディングになります。`chip(tint)` は `tint` に追従し、それを読むプロパティだけが更新されます。

```asuka
style chip(tint: Color, size: float = 13) {
  padding: [4, 10]; radius: 999
  background: tint.opacity(0.15); color: tint; font: Font(size: size)
}

Text(tag) { style: chip(.green) }
Text(count) { style: [card, chip(tint, size: 15)] }
```

リストの項目には `if` や `match` も書けます。選ばれた分岐のスタイルが適用され、別の分岐に移ると、それらが設定していた各プロパティは、リストでそれより前にある項目の値（なければプラットフォームのデフォルト）に戻ります。ここでの `if` には `else` が要りません。`match` はすべての場合を網羅する必要がなく、名前を束縛できません。分岐に `[]` と書くと何も適用しません。

```asuka
View {
  style: [
    card,
    if selected { chip(.accent) },
    if m.urgent { urgent } else if m.read { read },
    match m.kind { .note => note, .task => [task, bold], _ => [] },
  ]
}
```

スタイルに書けるのは、スタイルのプロパティ、`style:`、バリアント（後述）、`use` です。`pub style` は別のパッケージからも使えます。`style: theme.card` と書き、その値は宣言したパッケージの中で評価されます。スタイルは自分自身を適用できません。ほかのスタイルを経由する場合も同じです。

スタイルを何も指定しない要素は、プラットフォーム純正のものと同じ見た目になります。

**テーマ。** スタイルの値はそのパッケージの `let` や `state` を読めるので、デザイントークンはそのどちらかになります（`pub let hairline = 2.0`）。`state` なら、変えると読んでいるすべての場所が変わります。スタイルは上位のコンポーネントが提供するストアを `use` することもでき、スタイルが適用される場所で探されます。アプリの一部が独自のものを提供すれば、そこには独自のトークンが使われます。

```asuka
pub store Theme(hairline: float = 2) {
  pub state border = hairline
}

pub style framed { use t: Theme; border: t.border; borderColor: .separator }

app {
  provide Theme()
  Stack { Settings() }
}

component Settings {
  provide Theme(hairline: 1)   // この中では細く
  Column { style: framed }
}
```

**デフォルトのスタイル。** 要素（またはスタイルのプロパティを受け取るコンポーネント）の名前を付けたスタイルは、そのデフォルトのスタイルになります。パッケージの中で書かれたその種類のすべての要素に、`style:` のリストの下で適用されます。ほかのパッケージの要素や、そのコンポーネントの中の要素は、元の見た目のままです。

```asuka
style Button { radius: 8 }
style View { borderColor: .separator }
```

コンテナ（`View`、`Row`、`Column`、`SafeArea`）には**バリアント**を書けます。インタラクションの状態の名前を付けたスタイルのプロパティのまとまりで、コンテナがその状態にある間、元のスタイルの上に重ねられます。CSS の `:hover` や `:active` にあたるものです。状態は `hovered`、`pressed`、`dragging`（ドラッグされている間）、`dragOver`（受け付けるドラッグが上にある間）の 4 つで、この順に重なるため、`pressed` は `hovered` より優先されます。バリアントが変えるのは書いたプロパティだけで、その値はほかのプロパティと同じくバインディングです。

```asuka
View {
  padding: 16; radius: 12; background: .secondaryBackground
  hovered { borderColor: .accent }
  pressed { background: .separator; opacity: 0.8 }
  on press { open() }
}
```

ビューが状態を報告するのは、どこかでその状態が使われたときだけです。そのため `pressed` バリアントを持たないビューは、プラットフォーム自身の押下時の見た目をそのまま保ちます。バリアントの中でも `style:` を使えます（`pressed { style: raised }`）。バリアント自身のプロパティがそれより優先されます。スタイルにもバリアントを書けて、コンテナに適用されたときに働きます（ほかの要素では無視されます）。

```asuka
style button {
  padding: [8, 16]; radius: 8; background: .accent
  pressed { opacity: 0.7 }
}
```

コンポーネントは独自のバリアントを宣言することもでき（`variant checked = checked`。[コンポーネント](components.md#コンポーネントが宣言するバリアント)を参照）、スタイルにはインタラクションの状態と同じようにそのブロックを書けます。そのバリアントを宣言しているコンポーネントの要素に適用すると、バリアントが成り立つあいだブロックが重ねられ、インタラクションの状態より優先されます。ほかの要素では何もしません。名前を書き間違えるとエラーになります。スタイルに書けるのは、自分のパッケージのコンポーネントのバリアントと、ほかのパッケージの `pub` なバリアントだけです。

```asuka
// selected バリアントを宣言するコンポーネント（リストボックスの選択肢など）向け。
style option {
  padding: [6, 10]; radius: 6
  pressed { opacity: 0.8 }
  selected { background: .accent; color: .onAccent }
}
```

コンポーネントのバリアントは、呼び出し側のプロパティより優先されます。呼び出し側の `background` はコンポーネント自身のものを置き換えますが、コンポーネントの `pressed` は変わらずそれを暗くします。ほかのビューは名前を通してビューの状態に追従できます（[ビューに名前を付ける](#ビューに名前を付ける)を参照）。`View as card { … }` と書き、任意のバインディングで `card.hovered`（あるいは `card.pressed`、`card.dragging`、`card.dragOver`）を読みます。子ビュー（グループ全体のホバー）でも兄弟のビューでも使えます。スタイル以外の反応にはハンドラーを使います：`on pressedChange(p)`、`on hoverChange(h)`。

## ビューに名前を付ける

ビューには、値を設定するのではなく、ビューに対して何かをしたいこともあります。動画を再生する、ページでスクリプトを実行する、テキストフィールドに文字を入れる、リストをスクロールする、といった操作です。`Ref` はビューのハンドルです。任意の要素に `ref: r` と書くとそのビューが `r` に入り、`Element as name` と書くと、そのブロックの残りの部分で使えるハンドルが宣言されます（要素を一つだけ表示するコンポーネントに書くと、その要素のビューを指します）。

```asuka
component Player {
  let video = Ref()

  Video("intro.mp4") { ref: video }
  Button("Start over") {
    on press {
      video.seek(0)
      video.play()
    }
  }
}
```

Ref がビューを持つのは、ビューが表示されている間だけです。その前後は空なので、コマンドは何もせず、状態はすべて false になります。状態は追跡されるので、それを読むバインディングは状態の変化に追従します。

| 対象 | Ref のメンバー |
|---|---|
| すべてのビュー | `hovered`、`pressed`、`dragging`、`dragOver`（bool、追跡される）、`focus()`、`blur()` |
| コンポーネント（`Checkbox as box`） | そのバリアント：`box.checked`（bool、追跡される） |
| `Scroll`、`List` | `scrollTo(x, y, animated: true)` |
| `Video` | `play()`、`pause()`、`seek(seconds)` |
| `WebView` | `try await eval(script)`（スクリプトの値を JSON で）、`run(script)`（完了を待たない）、`goBack()`、`goForward()`、`reload()`、`stopLoading()` |
| `TextField` | `insert(text)`、`format(.bold, on)`、`toggleFormat(.italic)`、`setLink(url)`（リッチなフィールドの選択範囲に対して） |

Ref はほかへ渡せます（`shareFiles(paths, from: button)`、`a11yFocus(field)`）。ファイルの `let` に置けば、複数のコンポーネントのビューで使えます。
