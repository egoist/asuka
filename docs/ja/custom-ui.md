# 独自の見た目

Asuka には、アプリの画面を作る方法が二つあります。一つの画面で両方を混ぜて使うこともできます。

- **プラットフォームのコントロール**（`Button`、`Toggle`、`Picker`、`TextField`、`Sheet` など）：各プラットフォーム純正のウィジェットです。スタイルを何も設定しなければ、そのプラットフォームのアプリと同じ見た目になります。iOS では UIKit、Android では Material 3 です。[コンポーネント](components.md)を参照してください。
- **`std/base`**：振る舞いだけを持ち、見た目を持たないコンポーネントです。セレクト、メニュー、ダイアログ、スイッチ、タブ、スライダーが、それぞれ自分の状態、キーボードとフォーカス、スクリーンリーダー、ウィンドウの上での位置を扱います。表示されるのはアプリが付けたスタイルだけなので、スマートフォンでもデスクトップでもブラウザーでも同じ見た目になります。

アプリに独自のデザインがあるときは `std/base` を選びます。Web のデザインシステムのように、ブランドの色や形、動きをすべてのコントロールに付け、どこでも同じにしたい場合です。コンボボックス、トグルグループ、範囲スライダー、ウィンドウ内のメニューバー、トーストのように、プラットフォームによっては持っていないコントロールも使えます。

描画以上のことをするコントロールは、プラットフォームのものを使い続けてください。iOS の日付ピッカーにはホイールがあり、テキストフィールドには自動入力とシステムのテキスト編集があり、共有シートはプラットフォームのほかのアプリにつながります。`std/base` の `Input` の中身はスタイルを付けた `TextField` なので、入力、選択、キーボードはプラットフォームのままです。

## 画面全体の例

`std/base` は Asuka に付属しています。`import std/base` と書けば、`base.Select`、`base.Switch`、`base.Dialog` などとして使えます。次の例は、独自のテーマで作った設定カードです。

```asuka check
import std/base

/// アプリの見た目：すべてのスタイルがここから色と角丸を読みます。
store Theme(dark: bool) {
  pub let ink: Color = if dark { #f2f2f7 } else { #1c1c1e }
  pub let paper: Color = if dark { #18181c } else { #ffffff }
  pub let line: Color = if dark { #3a3a44 } else { #d1d1d6 }
  pub let accent: Color = #6d5dfc
  pub let corner: float = 10
}

style card {
  use t: Theme
  padding: 16
  gap: 12
  radius: t.corner
  background: t.paper
  border: 1
  borderColor: t.line
}

style field {
  use t: Theme
  direction: .row
  align: .center
  gap: 8
  padding: [8, 12]
  radius: t.corner
  border: 1
  borderColor: t.line
  focusVisible {
    border: 2
    borderColor: t.accent
  }
}

style option {
  use t: Theme
  padding: [6, 10]
  radius: t.corner - 4
  highlighted { background: t.accent.opacity(0.15) }
}

style track {
  use t: Theme
  width: 46
  height: 28
  radius: 14
  padding: 3
  background: t.line
  checked { background: t.accent }
}

style thumb {
  width: 22
  height: 22
  radius: 11
  background: #ffffff
  checked { margin: Edges(start: 18) }
}

style primary {
  use t: Theme
  padding: [8, 16]
  radius: 999
  background: t.accent
  pressed { opacity: 0.7 }
}

component Settings {
  use t: Theme
  state units = "metric"
  state sync = true
  state confirming = false

  Column {
    style: card

    base.Select {
      bind value: units
      labels: ["metric": unitName("metric"), "imperial": unitName("imperial")]
      base.SelectTrigger {
        style: field
        base.SelectValue {
          color: t.ink
          grow: 1
        }
        Symbol("chevron.down") { color: t.ink }
      }
      popup {
        base.SelectPopup {
          style: card
          padding: 4
          gap: 0
          for u in ["metric", "imperial"] key u {
            base.SelectItem(u) {
              style: option
              label: unitName(u)
              Text(unitName(u)) { color: t.ink }
            }
          }
        }
      }
    }

    Row {
      gap: 10
      align: .center
      base.Switch {
        style: track
        bind checked: sync
        label: "Sync"
        base.SwitchThumb { style: thumb }
      }
      Text("Sync") { color: t.ink }
    }

    base.Dialog {
      bind open: confirming
      base.DialogTrigger {
        style: primary
        Text("Reset…") { color: #ffffff }
      }
      backdrop { View { background: #00000066 } }
      popup {
        base.DialogPopup {
          style: card
          width: 300
          base.DialogTitle("Reset settings?") {
            color: t.ink
            font: .headline
          }
          base.DialogDescription("Units and sync go back to their defaults.") { color: t.ink }
          Row {
            gap: 8
            justify: .end
            base.DialogClose {
              style: option
              Text("Cancel") { color: t.ink }
            }
            base.Button {
              style: primary
              on press {
                units = "metric"
                sync = true
                confirming = false
              }
              Text("Reset") { color: #ffffff }
            }
          }
        }
      }
    }
  }
}

fn unitName(u: string) -> string = if u == "metric" { "Metric" } else { "Imperial" }

component Screen {
  provide Theme(dark: environment().dark)

  SafeArea {
    padding: 20
    Settings()
  }
}

app { Screen() }

test "units and a reset" {
  let app = mount(Screen())
  app.tap("Metric")
  app.tap("Imperial")
  app.expect("Imperial")
  app.tap("Reset…")
  app.expect("Reset settings?")
  app.tap("Reset")
  app.expectNo("Reset settings?")
  app.expect("Metric")
}
```

このカードは iOS、Android、macOS、Windows、Linux、Web のどれでも同じに描かれます。色も角丸も余白もアプリが決めたものだからです。プラットフォームごとに変わるのは、ユーザーがコントロールに期待する操作のほうです。タッチ画面のタップとマウスのクリック、デスクトップのキー、Android の「戻る」、そして各プラットフォームのスクリーンリーダーです。

## パーツ、スロット、状態

コンポーネントはいくつかのパーツでできています。ルート（`Select`、`Dialog`、`Tabs`）が状態を持ち、アプリはその中にパーツ（`SelectTrigger`、`SelectValue`、`SelectPopup`、`SelectItem`）を並べてスタイルを付けます。各パーツは要素を一つだけ表示するので、`View` と同じように `style:` とスタイルのプロパティを受け取ります。中に何を置くか（シンボル、画像、テキストの行など）はアプリが決めます。

- **値。** ルートの値は、バインドされていれば呼び出し側のもの（`bind value: units`、`bind checked: sync`、`bind open: confirming`）、されていなければルート自身のものです。変化はイベントとしても通知されます（`on valueChange(v)`、`on checkedChange(c)`、`on openChange(o)`）。
- **スロット。** ポップアップのパーツはルートの `popup` スロットに入れます。ルートはそれをウィンドウの上、トリガーの隣に表示します（ダイアログは中央に、`side:` を指定すれば端に寄せて）。`backdrop` は、ダイアログの後ろでウィンドウを覆うものです。
- **状態。** 各パーツの状態は、アプリがスタイルを付けられるバリアントです。`View` に `pressed { … }` を書くのと同じように、`checked { … }`、`open { … }`、`highlighted { … }`、`selected { … }`、`disabled { … }` と書きます。スタイルの中にも書けます（`style track { checked { … } }`）。ref からも読めます。`base.Switch as s` と書けば `s.checked` で読めます。

`asuka doc std/base` を実行すると、各パーツのプロパティ、イベント、バリアントが一覧できます。[リファレンス](std/base.md)にも同じ内容があります。

## テーマ

テーマは、スタイルが読む値（色、角丸、余白、フォント）をまとめたストアです。スタイルは `use` でそれを受け取り、`provide` で画面のコンポーネントに渡します。

```asuka
store Theme(dark: bool) {
  pub let ink: Color = if dark { #f2f2f7 } else { #1c1c1e }
  pub let accent: Color = #6d5dfc
}

style label {
  use t: Theme
  color: t.ink
}

component Screen {
  provide Theme(dark: environment().dark)
  Text("Hello") { style: label }
}
```

`environment().dark` を読めば、ユーザーがダークモードを切り替えたときに追従します。テーマが状態（ユーザーが設定で選ぶものなど）なら、それを読むすべてのスタイルが一度に切り替わります。セマンティックカラー（`.label`、`.accent`、`.separator`）も使えます。独自の形にプラットフォームの色を使いたいときに便利です。

## コンポーネント一覧

| グループ | コンポーネント |
|---|---|
| アクション | `Button`、`Toggle`、`ToggleGroup`、`Toolbar` |
| 選択 | `Checkbox`（`CheckboxGroup`）、`Switch`、`RadioGroup` と `Radio`、`Select`、`Listbox`、`Combobox` |
| 値 | `Slider`、`NumberField`、`Progress`、`Meter` |
| フォーム | `Field`（ラベル、説明、エラー）、`Input`、`Fieldset`、`Form` |
| 開閉 | `Collapsible`、`Accordion`、`Tabs` |
| ポップアップ | `Dialog`、`AlertDialog`、`Popover`、`Tooltip`、`PreviewCard`、`Menu`、`ContextMenu`、`Menubar` |
| その他 | `Avatar`、`ScrollArea`、`Separator`、トースト（`Toasts`、`ToastViewport`） |

これらは Asuka で書かれていて、`View`、`Text`、`Overlay` とスタイルのプロパティからできています。Base UI が DOM からできているのと同じです。

## キーボード、フォーカス、スクリーンリーダー

キー操作は、同じ種類のコントロールについての各プラットフォームの慣習に従います。

- 矢印キーで、メニュー、セレクト、ラジオグループ、タブリストの項目を移動します。文字を入力すると、その項目に飛びます。
- Escape でポップアップが閉じます。Android の「戻る」でも閉じます。
- ポップアップが開くとフォーカスがその中に移り、閉じるとトリガーに戻ります。ダイアログが開いているあいだはフォーカスが中にとどまり、ウィンドウのほかの部分はスクリーンリーダーから隠れます。
- Tab でコントロールのあいだを移動します。ラジオグループやタブリストは、Tab で一回止まるだけです。
- `focusVisible { … }` は、キーボードでフォーカスが移ったときだけフォーカスリングを表示します。タップでは表示しません。

各パーツはそれが何であるかに合ったロールを持つので、各プラットフォームのスクリーンリーダー（VoiceOver、TalkBack、ナレーター、Orca、ブラウザーの読み上げ）は `Switch` をスイッチとして、オンかオフかも含めて読み上げます。中にテキストのないスイッチやチェックボックスには、上の例のスイッチのように `label:` を付けます。[アクセシビリティ](accessibility.md)を参照してください。

## 動き

ポップアップの中身は、開くときに作られ、閉じるときに破棄されます。`animate { }` の中で開くか、`animate { }` の中で書き換える状態にバインドすれば、そのビューが宣言したトランジションが再生されます。フェードしながら拡大するメニューや、端からスライドしてくるドロワーなどです。[動きとジェスチャー](motion.md)を参照してください。

## テスト

`std/base` のコンポーネントもほかのビューと同じなので、`test` ブロックからユーザーと同じように操作できます。`app.tap("Metric")` でセレクトが開き、項目を `app.tap` すると選ばれ、`app.pressKey("down")` でメニューを移動し、`app.expect` で表示を確かめます。[テスト](testing.md)を参照してください。

[`examples/base`](../../examples/base) は、二つのテーマを持つ設定画面です。フォーム、セレクト、スイッチ、チェックボックス、ツールチップ、タブ、スライダー、ダイアログ、トーストが入っています。

## 自分で作る

`std/base` のコンポーネントは、どのアプリにもあるものでできています。`role`、`focusable`、`keys` とアクセシビリティのプロパティを持つ `View`、そしてウィンドウの上に表示するための `Overlay`（[Overlay](components.md#overlay)）です。`std/base` にないコントロールも、アプリ自身のパッケージで同じように書けます。状態は `pub variant` で表します。[コンポーネントが宣言するバリアント](language/components.md#コンポーネントが宣言するバリアント)を参照してください。
