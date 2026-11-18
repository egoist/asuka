# アクセシビリティ

Asuka は各プラットフォーム純正のウィジェットを表示するので、スクリーンリーダーやキーボード、文字サイズの設定に必要なもののほとんどは、コードを書かなくても最初から揃っています。このページでは、何もしなくても得られるものと、それ以外を補うためのプロパティを紹介します。扱うのは、ラベル、ロール、グループ化、カスタムアクション、調整可能なビュー、読み上げ、キーボード、文字サイズ、視差効果を減らす（Reduce Motion）、そしてそれらのテスト方法です。

Asuka は同じプロパティを各プラットフォームのアクセシビリティ API に渡し、スクリーンリーダーはそこから情報を読み取ります。

| プラットフォーム | アクセシビリティ API | スクリーンリーダー |
| --- | --- | --- |
| iOS | UIAccessibility | VoiceOver |
| Android | Android のユーザー補助フレームワーク（`AccessibilityNodeInfo`） | TalkBack |
| macOS | NSAccessibility | VoiceOver |
| Windows | UI Automation | ナレーター、NVDA |
| Linux | GTK のアクセシビリティ（AT-SPI 経由） | Orca |
| Web | ページの要素に付ける ARIA のロールと属性 | ブラウザーで使うもの：VoiceOver、NVDA、TalkBack など |

## 何もしなくても得られるもの

- **コントロールはプラットフォームのものです。** `Button`、`Toggle`、`Slider`、`TextField`、`Segmented`、`Stepper`、`DatePicker` などは、UIKit、Material、AppKit、Win32、GTK、ブラウザーそれぞれの純正のコントロールです。ほかのアプリと同じように読み上げられ、操作できます。
- **押せるビューはボタンになります。** `on press` を持つ `View` は一つの要素になります。
  - iOS：中にあるテキストとラベル付きの画像をつなげて読み上げるボタンになります。ダブルタップで押せます。
  - Android：クリック可能なビューになり、TalkBack は中のテキストをまとめて読み上げます。
  - macOS：ボタン（`AXButton`）になります。
  - Windows：中のテキストをつなげたものを名前とするボタンになり、スクリーンリーダーから押せます（UI Automation の Invoke）。
  - Linux：ボタンになります。
  - Web：`button` ロールを持つ要素になります。

  Windows、Linux、Web ではキーボードでも操作できます（[キーボード](#キーボード)を参照）。
- **文字は拡大縮小されます。** セマンティックなフォント（`.body`、`.headline` など）も、固定サイズ（`Font(size: 15)`）も、アプリ独自のフォントも、すべてのフォントがユーザーの文字サイズの設定に従います。対象は Dynamic Type、Android のフォントサイズ、Windows のテキストのサイズ、GNOME のテキストの拡大率、ブラウザーの文字サイズです。ユーザーが設定を変えると、Asuka はテキストを測り直し、それによって動くものをレイアウトし直します。Mac には文字サイズの設定がありません。
- **色は環境に合わせて変わります。** セマンティックカラー（`.label`、`.secondaryLabel`、`.background`、`.accent` など）はプラットフォームの色です（[レイアウトとスタイル](layout.md)を参照）。ダークモードに従い、iOS と macOS では「コントラストを上げる」、Linux では GNOME のハイコントラストにも従います。
- **レイアウトのためだけのビューは邪魔をしません。** イベントもアクセシビリティのプロパティも持たない `View`、`Row`、`Column` はネイティブのビューを作らないので、アクセシビリティツリーに現れることはありません。
- **画像は説明を付けるまで読み上げられません。** `label` のない `Image`、`Symbol`、`Shape` はスクリーンリーダーから隠されます。ただし Linux と Web では、よく使うシンボルは例外で、ラベルを付けない限り英語の名前（Back、Archive）で読み上げられます。

## ラベルとヒント

`label` は、スクリーンリーダーがそのビューについて読み上げる内容です。`hint` は、ラベルだけでは伝わらないときに、そのビューを使うと何が起きるかを伝えます。

```asuka
component Labeled {
  Row {
    gap: 12
    Button("") {
      icon: "paperplane"
      label: "Send"
      variant: .filled
      on press { send() }
    }
    Image("https://picsum.photos/id/1015/600/400") {
      label: "A river in a valley"
      aspectRatio: 3.0 / 2
    }
    View {
      label: "Favorite"
      role: .image
      Symbol("heart.fill") { color: .red }
    }
  }
}

fn send() {}
```

- `Button` はタイトルを読み上げます。アイコンだけのボタンには `label` を付けてください。プラットフォームが自分で名前を付けるシンボルはごく一部で、Android では英語の名前しかありません。
- `TextField` と `Toggle`（チェックボックスも含む）にも、`label: "Email"` のように `label` を指定できます。ラベルはコントロール自体に付けてください。名前は入力内容やチェック状態とは別に扱われ、編集や切り替えもそのまま行えます。Android では、テキストフィールドのラベルはアクセシビリティのヒント（hint）になり、TalkBack はそれをフィールドの名前として読み上げます。
- `Image` の `label` はその画像の説明です。`label` がなければ装飾として扱われます。[`std/svg`](stdlib.md) のアイコンにも同じように `label` を付けられます。
- `Symbol` は自分のラベルを持ちません。意味のあるシンボルは、`label` と `.image` を指定した `View` で包んでください。
- `hint` は、iOS ではアクセシビリティのヒントとして少し間を置いてから読み上げられ（ユーザーはヒントをオフにできます）、Android では状態の説明（state description）としてラベルと一緒に読み上げられます。macOS ではビューのヘルプ（`accessibilityHelp`）、Windows ではヘルプテキスト、Linux ではアクセシブルな説明（accessible description）、Web では `aria-description` になります。「メッセージを開きます」のように短くしてください。

`label` と `hint` は、ほかのすべてのプロパティと同じくバインディングです。固定のテキストでも、状態を読む式でもよく、ビューはその変化に追従します。アプリの言語も同じです（`label: tr.favorite()`）。押せるビューに `label` を付けなければ、中のテキストが読み上げられます。

## ロール

`role` は、そのビューが何であるかを示します。

| ロール | iOS | Android | macOS | Windows | Linux | Web |
| --- | --- | --- | --- | --- | --- | --- |
| `.off` | なし（デフォルト） | なし | なし | なし | なし | なし |
| `.button` | button トレイト | ボタンとして読み上げ | `AXButton` | Button | `button` | `button` |
| `.link` | link トレイト | ボタンとして読み上げ | `AXLink` | Hyperlink | `link` | `link` |
| `.header` | header トレイト | 見出し | `AXHeading` | Text（見出しレベル 1） | `heading` | `heading` |
| `.image` | image トレイト | 画像として読み上げ | `AXImage` | Image | `img` | `img` |
| `.text` | static text トレイト | なし | `AXStaticText` | Text | `label` | なし |
| `.search` | search field トレイト | なし | `AXTextField` | Edit | `search` | `searchbox` |

Windows の列は UI Automation のコントロールタイプ、Linux の列は GTK のアクセシブルロール、Web の列は ARIA のロールです。押せる `View` はもともとボタンです。iOS と macOS では、`role` を持つビューはそれ自体が一つの要素になります。そのため `label` も付けてください。付けないと、ロールだけが読み上げられます。

ビューを組み合わせて作ったコントロール（[独自の見た目](components.md#独自の見た目)を参照）には、それが何であるかに合ったロールを付けます。各プラットフォームは、それを自分の同じ種類のコントロールとして読み上げます。たとえばチェックボックスは、UIKit ではトグルボタン、Android では `CheckBox`、macOS では `AXCheckBox`、UI Automation では CheckBox、GTK と ARIA では `checkbox` になります。

| ロール | 意味 |
| --- | --- |
| `.checkbox`、`.switch` | オンかオフ。状態は `a11yChecked` で示す |
| `.radio`、`.radioGroup` | グループの選択肢の一つと、そのグループ |
| `.tab`、`.tabList`、`.tabPanel` | タブ、タブの並び、タブが表示する内容 |
| `.slider`、`.spinButton` | 範囲の中の値（`a11yRange`、`a11yValue`）。`on adjust` か矢印キーで増減する |
| `.progressBar`、`.meter` | タスクの進み具合、範囲の中の測定値 |
| `.menu`、`.menuBar`、`.menuItem`、`.menuItemCheckbox`、`.menuItemRadio` | メニュー、メニューバー、それらの項目 |
| `.listbox`、`.option`、`.comboBox` | 選択肢のリスト、選択肢、それを開くフィールドやボタン |
| `.dialog`、`.alertDialog` | アプリのウィンドウの上に出るウィンドウ。アラートは応答を求める |
| `.tooltip`、`.status` | 短い説明。フォーカスを動かさずに読み上げられるメッセージ |
| `.group`、`.toolbar`、`.separator` | ひとまとまりのビュー、ツールバー、グループ間の区切り線 |

## 状態

独自のコントロールも、プラットフォームのコントロールと同じ方法で自分の状態を伝えます。

| プロパティ | 伝える内容 |
| --- | --- |
| `a11yChecked: .checked` | チェックボックス、スイッチ、ラジオボタン、チェックできるメニュー項目の状態：`.unchecked`、`.checked`、`.mixed`（どちらでもない：一部の子がチェックされた親のチェックボックス）。デフォルトの `.off` はチェックできないことを表す |
| `a11yExpanded: .expanded` | それが開く内容が表示されているかどうか：`.collapsed`、`.expanded`（開閉するビュー、メニューのボタン、コンボボックス）。`.off` は何も開かないことを表す |
| `a11ySelected: true` | グループの中で選ばれているもの：タブ、選択肢 |
| `a11yValue: "40%"` | 値として読み上げられる言葉 |
| `a11yRange: ValueRange(min: 0, max: 100, now: 40)` | スライダー、プログレスバー、メーターの範囲 |
| `a11yModal: true` | スクリーンリーダーがその中にとどまり、ウィンドウのほかの部分はスクリーンリーダーから隠れる：ダイアログ |
| `a11yControls: list` | それが操作するビュー（`Ref`）：コンボボックスのリスト、タブのパネル |
| `a11yLabelledBy: title` | テキストでそれに名前を付けるビュー：フィールドのラベル、ダイアログのタイトル |
| `a11yDescribedBy: hint` | テキストでそれを説明するビュー：フィールドの説明やエラー |

```asuka
component Disclosure(title: string) {
  state open = false
  let panel = Ref()

  Column {
    View {
      role: .button
      a11yExpanded: if open { .expanded } else { .collapsed }
      a11yControls: panel
      on press { open = !open }
      Text(title)
    }
    if open {
      View { ref: panel; Text("Details") }
    }
  }
}
```

これらの関係は、それを扱えるプラットフォームに伝わります。Windows（UI Automation の ControllerFor、LabeledBy、DescribedBy）、Linux、Web です。iOS と Android では、別のビューでラベル付けされたビューはそのビューのテキストをラベルとして読み上げ、説明が付いたビューはその説明をヒントとして読み上げます。macOS ではタイトル要素（title element）でビューに名前を付けます。`std/base` のコンポーネントは、これらをすべて自分で設定します。

## グループ化

スクリーンリーダーは要素を一つずつたどります。テキストが三つ並んだ行は三回止まることになりますが、一回で済ませたいことがよくあります。方法は二つあります。

- **押せるビューにする。** `on press` を持つ `View` は一つの要素になり、中身をまとめて読み上げます。行はたいていこの形です。

```asuka
struct Message {
  from: string
  subject: string
}

component MessageRow(m: Message) {
  event open

  View {
    direction: .row
    gap: 12
    padding: [12, 20]
    hint: "Opens the message"
    on press { emit open }
    View {
      a11yHidden: true
      Symbol("person.fill") { color: .secondaryLabel }
    }
    Column {
      grow: 1
      gap: 2
      Text(m.from) { font: .headline }
      Text(m.subject) { color: .secondaryLabel }
    }
  }
}
```

- **`label` を付ける。** `label` を持つ `View` は、そのラベルとして読み上げられます。iOS と macOS では、中身は個別には読み上げられません。Android ではラベルがビューのコンテンツの説明（content description）になり、Windows ではビューの名前、Linux ではアクセシブルなラベル、Web では `aria-label` になります。目で見てひと目でわかる内容を、言葉にして書いてください。「4:00」ではなく「Night Drive by Ada, 4 minutes」のようにします。

```asuka
component Track(title: string, artist: string, minutes: int) {
  Row {
    gap: 8
    label: "{title} by {artist}, {minutes} minutes"
    Text(title) { font: .headline }
    Text(artist) { color: .secondaryLabel }
    Text("{minutes}:00") { color: .secondaryLabel }
  }
}
```

## 装飾

`a11yHidden` は、ビューとその中身すべてをスクリーンリーダーから隠します。飾りや、同じことを言うテキストの隣にあるアイコン、「保存しました」と読み上げるトーストの中のチェックマークなどに使います。

```asuka
component Saved {
  Row {
    gap: 6
    View {
      a11yHidden: true
      Symbol("checkmark") { color: .green }
    }
    Text("Saved")
  }
}
```

## 見出し

スクリーンリーダーのユーザーは見出しから見出しへ移動します（VoiceOver のローター、TalkBack の見出し単位のナビゲーション、NVDA と Orca の H キー、ブラウザーのスクリーンリーダーの見出し一覧）。セクションのタイトルは見出しとしてマークしてください。`role: .header` を指定した `RichText` か、`label` とこのロールを指定した `View` を使います。[`std/markdown`](stdlib.md) は見出しを最初からマークします。

```asuka
component Headings {
  Column {
    gap: 12
    RichText([Span(text: "Recent")]) {
      role: .header
      font: .title2.bold()
    }
    View {
      label: "Settings"
      role: .header
      Text("Settings") { font: .title2 }
    }
  }
}
```

`RichText` の中のリンクはそれぞれが独立した要素で、スクリーンリーダーから見つけて実行できます。

## カスタムアクション

目の見えるユーザーがスワイプや長押し、行の中の小さなボタンで行う操作に、スクリーンリーダーのユーザーはアクションを通じてたどり着きます。VoiceOver ではアクションのローターで上下にスワイプし、TalkBack ではアクションメニューを使います。`actions` はビューにアクションを並べます。

```asuka
component Thread(subject: string) {
  event reply
  event archive

  View {
    padding: [12, 20]
    actions: [
      A11yAction(name: "Reply", run: () => { emit reply }),
      A11yAction(name: "Archive", run: () => { emit archive }),
    ]
    Text(subject)
  }
}
```

アクションを持つビューは一つの要素になり、中のテキストが読み上げられます。アクションが現れる場所は次のとおりです。

- **iOS** と **macOS**：ビューのカスタムアクション（`UIAccessibilityCustomAction`、`NSAccessibilityCustomAction`）になります。iOS では VoiceOver のアクションのローターに、Mac では VoiceOver のアクションメニュー（VO-Command-Space）に表示されます。
- **Android**：TalkBack のアクションメニューに表示されます。
- **Windows**：ビューのコンテキストメニューになります。キーボード（Shift+F10、アプリケーションキー）でも、マウスの右クリックでも開けます。ビューはタブストップになるので、キーボードでたどり着けます。
- **Linux**：AT-SPI 経由のビューのアクションになり、Orca が一覧にして実行します。あわせてビューのコンテキストメニュー（副ボタンのクリック、アプリケーションキー、Shift+F10）にもなります。ビューはタブストップになります。
- **Web**：ARIA にはカスタムアクションがないため、提供されません。アプリをブラウザーで動かす場合は、同じコマンドを別の方法（ボタンや `Menu`）でも用意してください。

`List` の `swipeActions` は、自動でスクリーンリーダーに提供されます。UIKit のリストは自分でスワイプアクションを VoiceOver に提供し、Android では Asuka がそれを TalkBack のアクションとして行に追加します。`actions` で重ねて並べないでください。

```asuka
state items = ["Milk", "Bread", "Eggs"]

component Groceries {
  List {
    for (i, item) in items.enumerated() key item {
      Row {
        padding: [12, 20]
        swipeActions: [
          .button(title: "Delete", icon: "trash", role: .destructive, run: () => { items.removeAt(i) }),
        ]
        Text(item)
      }
    }
  }
}
```

`Menu` のアクションや、`Menu` を持つボタンはプラットフォームのメニューなので、そのままでアクセシブルです。

## 調整可能なビュー

`on adjust(by)` は、スライダーのようにビューを調整可能にします。VoiceOver の上下スワイプや TalkBack の同様の操作（または音量キー）で、値が 1 または -1 ずつ変わります。値を選ぶ独自のコントロールに使ってください。`Slider` や `Stepper` は最初から調整可能です。

Mac では、ビューはインクリメンター（`AXIncrementor`）になり、VoiceOver では VO-上矢印と VO-下矢印で値を変えます。Windows、Linux、Web ではタブストップになり、矢印キーで値を変えます。上と右で 1、下と左で -1 です（Windows と Linux では、右から左に書く言語のとき左右が入れ替わります）。Linux と Web では slider ロールを持ちます。

```asuka
component Rating {
  state stars = 3

  Row {
    gap: 4
    align: .center
    on adjust(by) { stars = min(max(stars + by, 0), 5) }
    for i in 0..<5 key i {
      Symbol("star.fill") { color: if i < stars { .orange } else { .tertiaryLabel } }
    }
    Text("Rating: {stars} of 5")
  }
}
```

このビューは中のテキストとして読み上げられます。テキストは現在の値を伝え、値を変えるたびに変わります。`label` は付けていません。付けるとテキストの代わりにラベルが読み上げられるからです。値の範囲は自分で制限してください。スクリーンリーダーは限界を超えても調整を求め続けます。

## 読み上げ

`accessibility.announce`（`platform/accessibility`）は、スクリーンリーダーがオンになっていれば、テキストを読み上げさせます。フォーカスのない場所で起きた変化を伝えるためのものです。たとえばトースト、検索結果の件数、完了した保存などです。

```asuka
import platform/accessibility

component SaveButton {
  state shown = false

  Column {
    Button("Save") {
      on press {
        animate(() => { shown = true })
        accessibility.announce("Saved") // スクリーンリーダーはトーストに気づかない
      }
    }
    if shown { Text("Saved") }
  }
}
```

iOS では `UIAccessibility` の読み上げ通知（announcement）、Android では `announceForAccessibility`、macOS では読み上げのリクエスト（announcement request）、Windows では UI Automation の通知、Linux では GTK の読み上げ（GTK 4.14 以降）、Web では polite の `aria-live` リージョンになります。スクリーンリーダーがオフなら何もしません。フォーカスのあるビューがすでに伝えている内容は読み上げさせないでください。

## フォーカスの移動

`accessibility.focus(ref)` は、スクリーンリーダーのフォーカスをビュー（それを指す `Ref`）に移します。ユーザーの目の前で画面が変わり、大事なものが別の場所にあるときに使います。送信に失敗したフォームの下に出るエラー、検索の最初の結果、スライドインしてきたパネルのタイトルなどです。

```asuka
import platform/accessibility

component SignIn {
  state message = ""

  let error = Ref()

  async fn submit() {
    do {
      try await signIn()
    } catch e {
      message = e.message
      accessibility.focus(error)
    }
  }

  Column {
    gap: 12
    Button("Sign In") { on press { await submit() } }
    Text(message) {
      ref: error
      color: .red
    }
  }
}

async fn signIn() throws { throw Error("The password is wrong") }
```

フォーカスの移動は、ビューが作られてフレームが決まったあと、次に画面が更新されるときに送られます。iOS ではビューを指定した `UIAccessibility` の画面変更通知（screen-changed notification）、Android では `ACTION_ACCESSIBILITY_FOCUS`、macOS ではビューを指定したレイアウト変更の通知、Windows ではフォーカスイベントになります。自分のビューを持たないコンテナーでは、その中の最初のビューにフォーカスが移ります。この四つのプラットフォームでは、スクリーンリーダーがオフなら何もせず、キーボードのフォーカスを動かすこともありません（それは `Ref` の `focus()` の役目です）。

Linux と Web では、キーボードのフォーカスをビューに移し、スクリーンリーダーはそれを追いかけます。GTK とページには、スクリーンリーダーにビューを指し示すほかの方法がないためです。

## スクリーンリーダーが有効かどうか

`environment().screenReader` は、スクリーンリーダーがオンのあいだ true になります。これを読んでいるところは、オンとオフの切り替えに追従します。

- iOS と macOS：VoiceOver。
- Android：TalkBack。
- Windows：システムのスクリーンリーダーフラグ。ナレーターやほかのスクリーンリーダーが設定します。
- Linux：GNOME のスクリーンリーダーの設定（Orca）。
- Web：常に false です。ブラウザーはスクリーンリーダーがオンかどうかを教えてくれません。

これは、耳で使うと勝手が違うものを調整するために使い、何かを隠すためには使わないでください。たとえば、トーストを 3 秒で消すのではなくユーザーが閉じるまで表示しておく、自動で進むカルーセルを操作を待つものに変える、といった使い方です。

## テキストサイズ

文字はコードを書かなくても拡大縮小されるので、残る課題は、大きなサイズでも崩れないレイアウトです。

- テキストを含むものの高さを固定しないでください。伸びるようにしておき、最小値が必要なら `minHeight` を使います。
- 重要なテキストに `lines: 1` を使うのは避けてください。大きなサイズでは途中で切れてしまいます。
- 長い行は `Scroll` や `List` に入れてください。

`environment().fontScale` はユーザーの文字サイズで、デフォルトが 1 です。Dynamic Type の「大」（Large）、Android のデフォルトのフォントサイズ、Windows の 100%（「設定」>「アクセシビリティ」>「テキストのサイズ」）、GNOME の拡大率 1、ブラウザーのデフォルトの文字サイズ（16 CSS ピクセル）がこれにあたります。Mac では常に 1 です。スタイルの中で読めば、大きなサイズのときにレイアウトを変えられます。iOS の純正アプリが、行のラベルと値を縦に積むのと同じです。

```asuka
component Adaptive {
  let large = environment().fontScale > 1.5

  View {
    direction: if large { .column } else { .row }
    gap: if large { 4 } else { 12 }
    align: if large { .stretch } else { .center }
    Text("Wi-Fi") { grow: 1 }
    Text("Home") { color: .secondaryLabel }
  }
}
```

## 視差効果を減らす

`environment().reduceMotion` は、iOS と macOS の「視差効果を減らす」、Android の「アニメーションを削除」、Windows の「アニメーション効果」のオフ、GNOME のアニメーションのオフ、ブラウザーの `prefers-reduced-motion` のいずれかを表します。iOS では、Asuka はトランジションを動きではなくディゾルブにします。それ以外のプラットフォームでは、Asuka のアニメーションはすぐに終わります。開発者に任されている部分については[モーションとジェスチャー](motion.md)を参照してください。

## キーボード

デスクトップ、Web、そしてハードウェアキーボードをつないだ iPad、iPhone、Android デバイスでは、Asuka のビューはプラットフォーム純正のコントロールと同じようにキーボードで操作できます。

- **Tab** で、コントロールと押せるビューのあいだをビューの順に移動します。Shift+Tab で戻ります。
- **Enter** と **Space** で、フォーカスのある押せるビューをボタンと同じように押せます。
- フォーカスのあるビューには、プラットフォームのフォーカスリングが表示されます。AppKit のもの、Windows では Fluent のもの、Linux ではアクセントカラーの輪郭線、Web ではブラウザーのもの、UIKit のフォーカスエフェクト、Android のフォーカスハイライトです。
- `actions` か `on adjust` を持つビューはタブストップになります。コンテキストメニューは Shift+F10 かアプリケーションキーで開き（Windows、Linux）、矢印キーで値を変えます。

デスクトップと Web では、ポインターで押すとフォーカスも移ります。タッチスクリーンでのタップでは移りません。

### フォーカスを受け取るビュー

`focusable: true` を付けると、どのコンテナーもタブストップになり、クリックでもフォーカスを受け取ります。フォーカスが来たときと離れたときは `on focusChange(focused)` で受け取れます。`pressKeepsFocus: true` を付けたビューは、ポインターで押してもフォーカスを元の場所に残します（ツールバーのボタンやメニューを開くボタン）。

`pressed` が押下に従うのと同じように、二つのインタラクション状態がフォーカスに従います。スタイルにも使え、値として読むこともできます。

- `focused`：フォーカスがそのビューの中にある（ビュー自身か、その中のビュー。CSS の `:focus-within` にあたる）。そのため、中のテキストフィールドにフォーカスがあるあいだ、フィールドを囲む枠のスタイルを変えられます。
- `focusVisible`：同じく、ただしフォーカスがキーボードから来たか、フィールドにあるときだけ。フォーカスリングを出すべき場面です（CSS の `:focus-visible` にあたる）。

```asuka
component Card(title: string) {
  state opened = false

  View as card {
    focusable: true
    padding: 12
    radius: 8
    focusVisible { border: 2; borderColor: .accent }
    on press { opened = true }
    Text(if card.focused { "{title}, focused" } else { title })
  }
}
```

`focusVisible` にスタイルを付けたビューは自分でリングを描き、プラットフォームのリングは消えます。`hideFocusRing: true` は、代わりのリングなしでリングを消します。

`Ref` の `focus()` と `blur()` は、キーボードのフォーカスをビュー（フィールド、コントロール、フォーカスを受け取るビューや押せるビュー）に移したり、外したりします。キー操作に続いて移ったフォーカスは表示されます。

### ウィンドウの一部にフォーカスを閉じ込める

- `trapFocus: true`：フォーカスがそのビューの中にあるあいだ、Tab と Shift+Tab はその中のビューだけを巡回します（ダイアログのように）。
- `autofocus: true`：ビューがレイアウトされたら、ビュー自身がフォーカスを受け取れるならそこへ、そうでなければその中で最初にフォーカスを受け取れるビューへフォーカスを移します。
- `restoreFocus: true`：ビューが消えるとき、そのビューが現れたときにフォーカスを持っていたビューへフォーカスを戻します。ただし、ユーザーがフォーカスを別の場所へ移していた場合は戻しません。

`std/base` のダイアログ、メニュー、ポップオーバーは、この三つをすべて行います。

### ビューへの入力

`on keyText(text)` は、ビューにフォーカスがあり、どの `keys` も受け取らなかったときに入力された文字を受け取ります。リストのタイプアヘッド（入力した文字で始まる項目へ移動する機能）に使います。

## スクリーンリーダーでのテスト

スクリーンリーダーをオンにして、できれば画面を見ずに、すべての画面を試してください。すべてにたどり着けるか、止まるたびにそれが何かわかるか、スワイプやドラッグのすべてにアクションがあるかを確かめます。デスクトップでは、キーボードだけでも試してください。

- **VoiceOver** は iPhone で動きます。「設定」>「アクセシビリティ」>「VoiceOver」で設定します。「ショートカット」（サイドボタンのトリプルクリック）を設定しておくと、オンとオフを切り替えられます。シミュレーターには VoiceOver がありません。シミュレーターでは、Xcode の Accessibility Inspector（Xcode > Open Developer Tool）で各要素のラベル、トレイト、アクションを確認できます。
- **TalkBack** は、Android のスマートフォンか Google Play 入りのエミュレーターで、Android ユーザー補助設定ツール（Android Accessibility Suite）から使います。「設定」>「ユーザー補助」>「TalkBack」で設定します。
- **Mac の VoiceOver**：Command-F5 を押すか、「システム設定」>「アクセシビリティ」>「VoiceOver」で設定します。Accessibility Inspector（Xcode > Open Developer Tool）で macOS アプリの要素を直接調べられます。
- **Windows のナレーター**：Ctrl+Windows+Enter で起動します。NVDA は無料でダウンロードできます。Accessibility Insights for Windows では、各要素の UI Automation の名前、コントロールタイプ、パターンを確認できます。
- **Linux の Orca**：GNOME では Super+Alt+S を押すか、「設定」>「アクセシビリティ」>「スクリーンリーダー」で設定します。Accerciser で AT-SPI のツリーを確認できます。
- **Web**：ブラウザーの開発者ツールでアクセシビリティツリーを確認できます（Chrome と Firefox の Accessibility パネル）。Safari では VoiceOver を、Chrome や Firefox では NVDA を試してください。
- **文字サイズ**：iOS では「設定」>「アクセシビリティ」>「画面表示とテキストサイズ」>「さらに大きな文字」、シミュレーターでは Xcode の Environment Overrides を使います。Android では「設定」>「ディスプレイ」>「フォントサイズ」、Windows では「設定」>「アクセシビリティ」>「テキストのサイズ」、GNOME では「設定」>「アクセシビリティ」>「大きな文字」（Large Text）、そしてブラウザーの文字サイズです。最大のサイズで試してください。
- **視差効果を減らす**：iOS では「設定」>「アクセシビリティ」>「動作」、Android では「設定」>「ユーザー補助」>「アニメーションを削除」、macOS では「システム設定」>「アクセシビリティ」>「ディスプレイ」>「視差効果を減らす」、Windows では「設定」>「アクセシビリティ」>「視覚効果」>「アニメーション効果」、GNOME では「設定」>「アクセシビリティ」>「アニメーションを減らす」（Reduce Animation）です。ブラウザーではシステムの設定に従います。

`asuka dev` は、これらの設定を変えても動き続けます。アプリは設定が変わるたびにそれに追従します。
