# コンポーネント

コンポーネントは、ビューを表示する宣言です（[言語](language-tour.md) を参照）。このページでは、Asuka が提供する要素を用途ごとにまとめて紹介し、あわせて標準ライブラリの Markdown、SVG、Lottie、グラフ、シェーダーのパッケージも取り上げます。

プロパティはどれもバインディングです。値でも、状態を読む式でもよく、ビューはその変化に追従します。ユーザーが変更するコントロール（フィールドやトグル）は `bind` で、ユーザーの変更を書き込む状態を受け取ります（`bind text: query`）。[コンセプト](concepts.md) を参照してください。

## 各プラットフォームのウィジェット

| コンポーネント | iOS | Android | macOS | Windows | Linux | Web |
|---|---|---|---|---|---|---|
| `Text` | `UILabel` | `TextView` | `NSTextField`（ラベル） | Asuka が描画 | `GtkLabel` | ページのテキスト |
| `Button` | `UIButton` | `MaterialButton` | `NSButton` | Fluent の見た目で描画した `BUTTON` | `GtkButton` | `<button>` |
| `TextField` | `UITextField`、複数行は `UITextView` | `EditText` | `NSTextField`、`NSSecureTextField`、複数行は `NSTextView` | Fluent の見た目で描画した枠の中の `EDIT` | `GtkEntry`、`GtkPasswordEntry`、複数行は `GtkTextView` | `<input>`、複数行は `<textarea>` |
| `Toggle` | `UISwitch` | `MaterialSwitch` | `NSSwitch` | トグルスイッチとして描画した `BUTTON` のチェックボックス | `GtkSwitch` | `<input type="checkbox" switch>` |
| `Slider` | `UISlider` | Material の `Slider` | `NSSlider` | トラックバー | `GtkScale` | `<input type="range">` |
| `Segmented` | `UISegmentedControl` | Material のセグメントボタン | `NSSegmentedControl` | Windows Community Toolkit のものに倣って描画 | 連結した `GtkToggleButton` | ラジオグループのボタン |
| `Stepper` | `UIStepper` | Material のアイコンボタン 2 つ | `NSStepper` | ボタン 2 つ | 連結した − と + のボタン | ボタン 2 つ |
| `Wheel` | `UIPickerView` | `NumberPicker` | `NSPopUpButton` | ドロップダウンの `COMBOBOX` | `GtkDropDown` | `<select>` |
| `ColorPicker` | `UIColorWell` | 色見本と Material のダイアログ | `NSColorWell` とカラーパネル | 色見本と色の設定ダイアログ（`ChooseColor`） | `GtkColorDialogButton` | `<input type="color">` |
| `DatePicker` | `UIDatePicker` | Material のピッカーを開くボタン | `NSDatePicker` | 日付と時刻の選択コントロール（`SysDateTimePick32`） | `GtkCalendar` とスピンボタンを開くボタン | `<input type="date">`、`time`、`datetime-local` |
| `Progress` | `UIProgressView` | `LinearProgressIndicator` | `NSProgressIndicator` | Fluent の見た目で描画 | `GtkProgressBar` | `<progress>` |
| `ProgressRing` | SwiftUI の円形ゲージに倣って描画。値がなければ `UIActivityIndicatorView` | `CircularProgressIndicator` | 16 ポイントと 32 ポイントでは spinning スタイルの `NSProgressIndicator`。それ以外のサイズや `color` の指定時は、そのスタイルに倣って描画 | Fluent の `ProgressRing` に倣って描画 | GNOME のスタイルで描画。値がなければ `AdwSpinner` | SVG のリング |
| `Activity` | `UIActivityIndicatorView` | `CircularProgressIndicator` | spinning スタイルの `NSProgressIndicator` | Fluent のプログレスリングに倣って描画 | `AdwSpinner`（libadwaita 1.6 より前は `GtkSpinner`） | 描画したスピナー |
| `Image` | `UIImageView` | `ImageView` | `NSImageView` | GDI+ で描画 | GDK のテクスチャから描画 | `<img>` |
| `lottie.View` | lottie-ios | lottie-android | lottie-ios | Asuka 自身の `Canvas` 上のプレーヤー | Asuka 自身の `Canvas` 上のプレーヤー | lottie-web |
| `Symbol` | SF Symbols | Material Symbols | SF Symbols | Segoe Fluent Icons | アイコンテーマのもの。なければ Material Symbols | Material Symbols |
| `Video` | `AVPlayerViewController` | Media3 のプレイヤー | `AVPlayerView` | WebView2 内の `<video>` | `GtkVideo`（GStreamer） | `<video>` |
| `WebView` | `WKWebView` | `WebView` | `WKWebView` | WebView2 | `WebKitWebView`（WebKitGTK 6.0） | `<iframe>` |
| `Scroll` | `UIScrollView` | `NestedScrollView` | `NSScrollView` | Asuka 自身のビューと Fluent のスクロールインジケーター | `GtkScrolledWindow` | スクロールする要素 |
| `List` | `UICollectionView` | `RecyclerView` | `NSScrollView` の中で Asuka が再利用する行 | Asuka が再利用する行 | `GtkScrolledWindow` の中で Asuka が再利用する行 | Asuka が再利用する行 |

以降のセクションでは、プラットフォームによって動作が異なる点を説明します。

## テキスト

### Text

`Text` は、1 行または 1 段落のテキストを表示します。プラットフォームのラベルそのもので、デフォルトで折り返します。`lines` を指定すると行数を制限でき、収まらない部分は省略記号で切り詰められます。

```asuka
component Inbox {
  state count = 3

  Column {
    gap: 8
    Text("Inbox") { font: .title2.bold() }
    Text("{count} unread") { color: .secondaryLabel }
    Text("A long description that stops after two lines.") { lines: 2 }
  }
}
```

スタイルの `font`、`color`、`textAlign`、`lines` がテキストに適用されます。テキストスタイル（`.body`、`.headline`、`.footnote` など）は、ユーザーが選んだ文字サイズに追従します。

### RichText

`RichText` は、装飾付きのテキストを表示します。テキストは `Span`（スパン）のリストです。スパンの中の `"\n"` で新しい段落になります。スパンで既定値のままのフィールドには、`RichText` のスタイルが使われます。

```asuka
import platform/device

component About {
  RichText([
    Span(text: "asuka", font: .body.bold()),
    Span(text: " draws "),
    Span(text: "native", italic: true),
    Span(text: " views. Run "),
    Span(text: "asuka dev", mono: true),
    Span(text: ", or read "),
    Span(text: "the docs", link: "https://example.com/docs"),
    Span(text: ".\nA second paragraph, "),
    Span(text: "in red", color: .red),
    Span(text: "."),
  ]) {
    on link(url) { device.openURL(url) }
  }
}
```

- `Span` は、`text`、`font`、`italic`、`mono`、`underline`、`strike`、`color`、`background`（スパンがかかる各行に角丸で描かれます）、`link` を持ちます。
- リンクがタップされると `on link(url)` が実行されます。指定しなければ、リンクをタップしても何も起きません（ルーターの中では、アプリ自身の href へのリンクはそこへ移動します）。
- `role: .header` を指定すると、スクリーンリーダーに見出しとして伝わります。

iOS と Android では、各段落をプラットフォームの組版エンジン（CoreText、StaticLayout）が組み、内容と幅をキーにしてキャッシュします。そのため、ストリーミングで届く返信のように長いテキストに追記していく場合でも、組み直されるのは変わった段落だけです。macOS では読み取り専用の `NSTextView`（TextKit）、Windows では読み取り専用のリッチエディット（`RICHEDIT50W`）、Linux では Pango の属性で装飾した `GtkLabel`、ブラウザーでは段落の中のスパンになります。[`examples/richtext`](../../examples/richtext) は、テキストをストリーミング表示する例です。

プレーンテキストには `Text` を使ってください。プラットフォームのラベルそのものなので、ネイティブの動作がそのまま得られます。

**インラインビュー。** スパンの `inline` を使うと、テキストの中にコンポーネントを置けます。コンポーネントはベースラインに乗り、単語と一緒に折り返します。キーが同じなら、テキストが変わっても同じコンポーネントのまま、状態も保たれます。

```asuka
component Status {
  RichText([
    Span(text: "Status: "),
    Span(inline: Inline(key: "badge", build: () => Live())),
    Span(text: " since this morning."),
  ])
}

component Live {
  View {
    background: .green
    radius: 6
    padding: [2, 6]
    Text("live") {
      font: .caption
      color: #ffffff
    }
  }
}
```

### 選択できるテキスト

`Selectable` は、その中にある `RichText` ビューを選択できるようにします。複数のビューにまたがって、ひとつながりのテキストとして選択できます。長押しで単語を選択し、プラットフォームのハンドルで範囲を広げ、編集メニューからコピーします。マウスではドラッグで選択し、プラットフォームのコピーのショートカットでコピーします。Linux と Web でも複数のテキストをひとつながりで選択できますが、macOS と Windows ではテキストごとに個別に選択します。

```asuka
component Answer(question: [Span], answer: [Span]) {
  Selectable {
    gap: 12
    RichText(question) { role: .header }
    RichText(answer)
  }
}
```

`selectable: true` を指定した `Text` は単独で選択できます。複数のテキストにまたがって選択させたいときは、`Selectable` の中で `RichText` を使ってください。選択を介さずに何かをコピーするには、`device.copy(text)` でクリップボードに入れます。

### Markdown

`std/markdown` は Markdown（CommonMark に GitHub の表、タスクリスト、取り消し線、自動リンクを加えたもの）を、プラットフォーム純正のスタイル付きテキストとして表示します。見出し、強調、コード、引用、リスト、表、画像、リンクを扱い、文字サイズはユーザーの設定に従います。テキスト中の生の HTML は文字として表示されます。

```asuka
import std/markdown

component Reply(text: string, writing: bool) {
  markdown.View(text) { streaming: writing; padding: 16 }
}
```

`streaming` は、言語モデルの返答が流れてくるときのように、テキストがまだ書かれている途中であることを表します。このときテキストの末尾は、書きかけの記法ではなく、最終的になりそうな姿で表示されます（コードフェンスは閉じ、`**bold` は太字に、書きかけのリンクはリンクに）。`markdown.heal(text)` で同じ処理だけを行うこともできます。

見た目とリンクはアプリが決めます。

- `theme` は `markdown.Theme` です。本文のフォントと色、見出し、リンク、コード（とその背景）、引用、区切り線のスタイル、ブロックの間隔とインデントの幅を指定します。指定しなかった部分はプラットフォームのものになります。
- `render` はブロックをアプリのやり方で描きます。`markdown.Block`（`kind`、プレーンテキストの `text`、スタイル付きの `spans`、見出しの `level`、コードブロックの `language`、画像の `url`、表の `rows`）を受け取ってビューを返し、`none` を返せば Markdown 自身の描画になります。コードブロックのハイライト、コードから描く図、アプリ独自の画像などに使えます。
- `on link(url)` はタップされたリンクを、書かれたままの宛先（相対パスも）で受け取ります。何を開くかはアプリが決めます。これがなければ、Web、メール、電話のリンクはそれを扱うアプリで開き、それ以外のリンクは何もしません。

```asuka
import std/markdown

component Code(text: string, language: string) {
  Column {
    background: .secondaryBackground; radius: 8; padding: 12
    Text(language) { font: .caption; color: .secondaryLabel }
    Text(text) { font: Font.callout.family("Menlo") }
  }
}

component Answer(text: string) {
  state opened = ""

  markdown.View(text) {
    theme: markdown.Theme(font: .callout, linkColor: .purple, spacing: 8)
    render: b => if b.kind == .code { Code(b.text, language: b.language) } else { none }
    on link(url) { opened = url }
  }
}
```

Markdown はどのプラットフォームでも同じく Asuka が読み、各ブロックを言語自身のビューで描きます。段落はそれぞれ [`Selectable`](#選択できるテキスト) の中の [`RichText`](#richtext)、表はグリッド、画像は `Image` です。テキストは変わるたびに読み直されますが、ブロックは位置ごとに保たれるので、返答がストリーミングで届く間に描き直されるのは最後の 1、2 ブロックだけで、その前はレイアウトと選択範囲を保ちます。[`examples/markdown`](../../examples/markdown) を見てください。

## ボタン

`Button` はプラットフォームのボタンです。iOS では `UIButton` の configuration、Android では Material のボタン、macOS では `NSButton`、Windows では Fluent の見た目で描画した `BUTTON`、Linux では `GtkButton`、ブラウザーでは `<button>` になります。

```asuka
component Actions {
  state saving = false

  Row {
    gap: 12
    wrap: true
    Button("Save") {
      variant: .filled
      disabled: saving
      on press { saving = true }
    }
    Button("Compose") {
      icon: "square.and.pencil"
      variant: .tinted
    }
    Button("") {
      icon: "trash"
      label: "Delete"
      variant: .gray
    }
  }
}
```

- タイトルはボタンのテキストで、`icon` はその前に置くシンボルです（[シンボル](#シンボルとアイコン) を参照）。アイコンがあってタイトルが空のボタンは、アイコンだけになります。
- `label` は VoiceOver や TalkBack が読み上げる名前で、デフォルトではタイトルが使われます。アイコンだけのボタンには指定してください。
- `tooltip` は、ポインターを重ねているあいだ表示される短いテキストです。iPad のポインター（`UIToolTipInteraction`）、Android のマウス（`tooltipText`。Android では長押しでも表示されます）、macOS、Windows、Linux、Web（要素の `title`）で表示されます。アイコンだけのボタンには、その名前をツールチップとして指定してください。どのビューの `tooltip` は、任意のビューで同じ働きをします。iOS ではタッチスクリーンの操作だけでは決して表示されないので、ツールチップでしか伝えない情報を作らないでください。
- `disabled` は、プラットフォームの方法でボタンを無効にします。
- `font` と `color` で、テキストと色合い（tint）を変えられます。
- `href` は、押されたときにルートへ移動します（[ルート](navigation.md#ルート) を参照）。

`variant` はボタンの見た目です。

| Variant | iOS | Android | macOS | Windows | Linux | Web |
| --- | --- | --- | --- | --- | --- | --- |
| `.plain`（デフォルト） | plain | テキストボタン | 枠なし、アクセントカラー | subtle、アクセントカラー | フラット、アクセントカラー | プレーン |
| `.filled` | filled | Filled ボタン | アクセントカラーのプッシュボタン | アクセントボタン | suggested action | 塗りつぶし |
| `.tinted` | tinted | Tonal ボタン | タイトルがアクセントカラーのプッシュボタン | アクセントカラーの淡い塗り | アクセントカラーの淡い塗り | 淡い色付き |
| `.gray` | gray | Tonal ボタン | プッシュボタン | 標準のボタン | 標準のボタン | グレー |
| `.bordered` | bordered | Outlined ボタン | プッシュボタン | 標準のボタン | 標準のボタン | 枠線付き |
| `.glass` | Liquid Glass（`glass()`、iOS 26。それより前は gray） | Tonal ボタン | ガラスのベゼル。カプセル形、アイコンだけなら円（macOS 26。それより前はプッシュボタン） | `.gray` と同じ | 標準のボタン | `.gray` と同じ |
| `.glassProminent` | 色付きの Liquid Glass（`prominentGlass()`、iOS 26。それより前は filled） | Filled ボタン | アクセントカラーで色付けしたガラスのベゼル、主要なアクション（macOS 26。それより前は `.filled` と同じ） | `.filled` と同じ | suggested action | `.filled` と同じ |

ガラスのボタンはカプセル形で、アイコンだけのガラスのボタンは円形になります。iOS 26 純正のシートの閉じるボタンと同じ形です。

### メニュー

`menu` を指定したボタンは、タップされると `on press` を実行する代わりにアクションの一覧を表示します（iOS では `UIMenu`、Android では `PopupMenu`、macOS では `NSMenu`、Windows ではポップアップメニュー、Linux では `GtkPopoverMenu`、ブラウザーではポップオーバー）。それぞれは `Action` で、`.button(title:, icon:, role:, disabled:, checked:, run:)` のように書き、タイトル以外は省略できます。`role: .destructive` を指定すると赤で表示されます。`run` の中では await できます（`run: () => { await save() }`）。ハンドラーと同じく、アクションを作ったコードのタスクとして実行され、メニューはその終わりを待ちません。そのコードのスタック、シート、ルーターがそのまま使えるので、`push` や `navigate` も呼べます。`runAction(a)` は、自前のメニューのために、同じようにアクションを実行します。

```asuka
import platform/device

component More {
  Button("") {
    icon: "ellipsis.circle"
    label: "More"
    menu: [
      .button(title: "Copy", icon: "doc.on.doc", run: () => device.copy("https://example.com")),
      .button(title: "Share", icon: "square.and.arrow.up", run: () => device.share("https://example.com")),
      .button(title: "Delete", icon: "trash", role: .destructive, run: () => {}),
    ]
  }
}
```

アクションは `disabled`（淡色で表示され、選べなくなる）や `checked`（横にチェックマークが付く）にでき、どちらもメニューが開いている間、読んでいる値に追従します。`.submenu(title:, icon:, items:)` はサブメニューです。`.section(title:, items:)` は、代わりにメニューの中のセクションになり、タイトルの下で区切り線に挟まれます（SwiftUI の `Menu` の中の `Section`）。チェックされた項目を含むセクションは、選択肢になります。

```asuka
component Sorting {
  state sortBy = "Name"
  state locked = false

  fn sorted(by: string) -> Action = .button(title: by, checked: sortBy == by, run: () => { sortBy = by })

  Button("") {
    icon: "ellipsis.circle"
    label: "More"
    menu: [
      .section(title: "Sort by", items: [sorted("Name"), sorted("Date")]),
      .submenu(title: "Move to", icon: "folder", items: [
        .button(title: "Inbox", run: () => {}),
        .button(title: "Archive", run: () => {}),
      ]),
      .button(title: "Delete", role: .destructive, disabled: locked, run: () => {}),
    ]
  }
}
```

| | iOS | Android | macOS | Windows | Linux | Web |
|---|---|---|---|---|---|---|
| `disabled` | `.disabled` | 無効な項目 | 無効な項目 | 淡色表示の項目 | 無効なアクション | `disabled`、`aria-disabled` |
| `checked` | `state = .on` | チェックされた項目。セクションの選択肢はラジオボタン | `state = on` | `MF_CHECKED` | 状態を持つアクション（チェック） | `menuitemcheckbox` |
| `.submenu` | 入れ子の `UIMenu` | `SubMenu` | サブメニュー | サブメニュー | GMenu のサブメニュー | サブメニュー（そちら向きの矢印キーで開く） |
| `.section` | `displayInline` の `UIMenu` | 区切り線で仕切ったグループ。タイトルは無効な項目 | 区切り線とセクションヘッダー（macOS 14） | 区切り線とオーナードローのヘッダー | ラベル付きの GMenu セクション | 区切り線のあいだ、見出しの下に置いた `group` |

画面のバーボタン（その `toolbar` のボタン）も同じです。無効なボタンは淡色で表示され、`menu` を持つボタンはメニューを開きます（SwiftUI のツールバーの中の `Menu`）。アラートのボタンも無効にでき、表示中も `disabled` に追従します（名前が入力されるまで押せない OK など）。スワイプアクションは、どちらも表示しません。

`Menu` は、任意のコンテンツにメニューを付けます。`longPress: true` を指定すると、長押しで表示するコンテキストメニューになります。マウスでは（macOS、Windows、Linux、Web）、副ボタンのクリックで表示します。

```asuka
component Message {
  Menu {
    longPress: true
    actions: [
      .button(title: "Reply", icon: "message"),
      .button(title: "Delete", role: .destructive),
    ]
    View {
      padding: 16
      background: .secondaryBackground
      radius: 12
      Text("Long-press for a menu")
    }
  }
}
```

## テキストフィールド

`TextField` はテキストの入力欄です。`bind text` がそのテキストで、ユーザーが入力すると状態に書き込まれ、コードから状態に書き込むとテキストが置き換わります。

```asuka
component SignIn {
  state email = ""
  state password = ""

  let pw = Ref()

  Column {
    gap: 12
    TextField {
      bind text: email
      placeholder: "Email"
      keyboard: .email
      contentType: "email"
      autocapitalize: .off
      autocorrect: .no
      returnKey: .next
      on submit { pw.focus() }
    }
    TextField {
      ref: pw
      bind text: password
      placeholder: "Password"
      secure: true
      contentType: "password"
      returnKey: .go
      on submit { pw.blur() }
    }
  }
}
```

- `secure` は、パスワード用に入力内容を隠します。
- `keyboard`：`.default`、`.number`、`.decimal`、`.email`、`.url`、`.phone`。
- `returnKey` はリターンキーのラベルです：`.default`、`.done`、`.go`、`.next`、`.search`、`.send`。押すと `on submit` が実行されます。
- `contentType` は、システムの自動入力（AutoFill）のために、フィールドに入る内容の種類を示します：`"username"`、`"password"`、`"newPassword"`（プラットフォームが強力なパスワードを提案します）、`"oneTimeCode"`（届いたばかりのメッセージのコードがキーボードの上に表示されます。UIKit の `oneTimeCode`、Android の SMS コードのヒント）、`"email"`、`"phone"`、`"name"`、`"address"`、`"postalCode"`、`"cardNumber"` など。各プラットフォームが、それぞれの名前で設定します。
- `autocapitalize`：`.sentences`（デフォルト）、`.off`、`.words`、`.characters`。`autocorrect`：`.default`、`.yes`、`.no`。
- `on focus` と `on blur` は、キーボードのフォーカスを得たとき、失ったときに実行されます。`bind focused` はそれに追従し、フォーカスを動かすこともできます。`Ref` の `focus()` でフォーカスを求め、`blur()` で外します。

デスクトップには切り替えるオンスクリーンキーボードがないため、`keyboard` と `returnKey` はほとんど見た目に表れません。それでも Return キーを押せば `on submit` が実行されます。macOS では、`contentType` はフィールドの AutoFill のコンテンツタイプになります。Windows では、`.number` を指定すると数字しか入力できなくなります。Linux では、`keyboard`、`contentType`、`autocapitalize`、`autocorrect` が、フィールドに何を入力するかをインプットメソッドに伝えます（GTK の input purpose と input hints）。ブラウザーでは、これらは入力要素の `inputmode`、`enterkeyhint`、`autocomplete`、`autocapitalize`、`spellcheck` になり、スマートフォンのキーボードはそれに従います。

**複数行のテキスト。** `multiline: true` を指定すると、メッセージやコメントを入力するテキストエリアになります。リターンキーは `on submit` を実行する代わりに改行し、テキストに合わせて `lines` 行（0 なら無制限）まで高くなり、その先はスクロールします。

```asuka
component Composer {
  state message = ""

  Row {
    gap: 8
    align: .end
    TextField {
      bind text: message
      placeholder: "Message"
      multiline: true
      grow: 1
      lines: 5
    }
    Button("") {
      icon: "paperplane"
      label: "Send"
      variant: .filled
      disabled: message.isEmpty()
      on press { message = "" }
    }
  }
}
```

**テキストだけのフィールド。** `plain: true` を指定すると、フィールドの枠線、背景、内側の余白がなくなり、テキストとプレースホルダーだけが残ります。「メモ」や「リマインダー」のように、カードの中や独自のフォームに置くフィールドに使います。

```asuka
component NoteField {
  state note = ""

  TextField {
    bind text: note
    placeholder: "Note"
    multiline: true
    plain: true
  }
}
```

`SafeArea` にはキーボードの分の余白が加わるので、その下端に置いたフィールドはキーボードに隠れません。

## コントロール

各コントロールは、変更する状態を `bind` で受け取り、`disabled` も受け取ります。`Picker` と `Toggle` の `title` 以外はラベルを持たないので、横に `Text` を置いてください。

```asuka
import std/time

component Settings {
  state wifi = true
  state volume = 0.5
  state period = 0
  state cups = 2.0
  state size = 1
  state milk = -1
  state tint: Color = .blue
  state when = time.now()

  Column {
    gap: 16
    Row {
      align: .center
      Text("Wi-Fi") { grow: 1 }
      Toggle { bind on: wifi }
    }
    Slider { bind value: volume }
    Segmented(["Day", "Week", "Month"]) { bind selected: period }
    Row {
      gap: 12
      align: .center
      Text("{cups.fixed(0)} cups") { grow: 1 }
      Stepper {
        bind value: cups
        min: 1
        max: 8
      }
    }
    Wheel(["Small", "Medium", "Large"]) { bind selected: size }
    Picker(["Whole", "Oat", "None"]) {
      bind selected: milk
      prompt: "Milk"
    }
    Row {
      gap: 12
      align: .center
      Text("Tint") { grow: 1 }
      ColorPicker {
        bind value: tint
        alpha: true
        title: "Tint"
      }
    }
    Row {
      gap: 12
      align: .center
      Text("Starts") { grow: 1 }
      time.Picker {
        bind value: when
        mode: .dateTime
        min: time.now()
      }
    }
  }
}
```

- **Toggle** はスイッチで、`bind on` で値を受け取ります。`checkbox: true` を指定するとチェックボックスになります。Android では `MaterialCheckBox`、Mac と Windows ではそれぞれのチェックボックス、Linux では `GtkCheckButton`、ブラウザーではチェックボックスです。チェックボックスのない iOS では、「リマインダー」と同じように、チェックマークで塗りつぶされる円になります。
- **Slider** は、`min` から `max` までの `float` を選びます（デフォルトは 0 から 1）。
- **Segmented** は、横に並んだ少数の選択肢から一つを選ぶコントロールで、`selected` は選ばれた項目のインデックスです。UIKit のセグメンテッドコントロールや、Material のセグメントボタンを使います。Windows には該当するものがないため、Asuka が Windows Community Toolkit の `Segmented` に倣って描画します。
- **Stepper** は、マイナスとプラスで `float` を `step`（デフォルトは 1）ずつ `min` から `max`（デフォルトは 0 から 100）の範囲で増減させますです。iOS では `UIStepper`、macOS では `NSStepper` で、それ以外では 2 つのボタンになります（Android では Material のアイコンボタン、Linux では GNOME のスピンボタンと同じように連結したボタン）。
- **Wheel** は、回転するホイールで多数の項目から一つを選びます。`UIPickerView` や Android の `NumberPicker` です。デスクトップと Web にはホイールがないため、項目を並べたポップアップメニューになります（`NSPopUpButton`、WinUI の `ComboBox` に倣って描画したドロップダウンのコンボボックス、`GtkDropDown`、`<select>`）。
- **Picker** は、メニューから多数の項目の一つを選び、選んだ項目を表示します（メニュースタイルの SwiftUI の `Picker`、Material の exposed dropdown menu）。`selected` が範囲外（-1、デフォルト）なら何も選ばれておらず、ピッカーは `prompt` を表示します。これはスクリーンリーダーに伝える名前にもなります。Android では、`prompt` はフィールドのラベルとして、選ばれた項目の上に浮かびます。バインドしなければ、ピッカーはユーザーの選択を自分で保持します。

  | | Picker |
  |---|---|
  | iOS | ティントカラーのプレーンな `UIButton` に、ポップアップのインジケーターを付けたもの。メニューでは選んだ項目にチェックが付きます（`changesSelectionAsPrimaryAction`） |
  | Android | ユーザーが文字を入力できない `MaterialAutoCompleteTextView` を、exposed dropdown menu スタイルの outlined な `TextInputLayout` で包んだもの。TalkBack はドロップダウンリストとして読み上げます |
  | macOS | `NSPopUpButton`（ラベルは非表示の項目で、何も選ばれていないあいだだけ表示されます） |
  | Windows | WinUI 3 の `ComboBox` に倣って描画した、ドロップダウンリスト形式のコンボボックス（コントロールの塗りと線、末尾のシェブロン。リストはフライアウトの面にオーナードローで描画し、選んだ項目の先頭にアクセントカラーのピルを付けます。Windows 11 では DWM が角を丸めます）。何も選ばれていないあいだはラベルを二次テキストの色で描画し、UI Automation にはラベルを名前として伝えます |
  | Linux | `GtkStringList` を使う `GtkDropDown`。選んだ項目が入る位置に、ラベルを淡色のラベル（dim label）として表示します |
  | Web | `<select>`。最初の option は、ラベルを表示する非表示のプレースホルダーです |
- **ColorPicker** は、タップするとプラットフォームのピッカーを開く色見本で、選んだ色を `color` として書き込みます。`alpha` で不透明度も選べるようになり、`title` はピッカーの名前です。iOS では `UIColorWell` とシステムのピッカーです。Android にはないので、パレットを並べた Material のダイアログで、`alpha` には不透明度のスライダーが付きます。macOS ではカラーパネルを開く `NSColorWell`、Linux では `GtkColorDialogButton`、ブラウザーでは `<input type="color">` です。Windows のカラーダイアログには不透明度がないので、そこでは `alpha` は何もしません。
- **DatePicker** は日時を選び、デバイスのタイムゾーンで表示します。`std/time` の `time.Picker` は `time.Time` をバインドします（プレリュードの `DatePicker` がバインドするのは 1970 年からのミリ秒です）。`mode` は `.date`（デフォルト）、`.time`、`.dateTime` のいずれかで、`min` と `max` で範囲を制限します。

  | | コンパクト（デフォルト） | `.section` |
  |---|---|---|
  | iOS | UIKit のコンパクトな日付ピッカー | カレンダーと時刻をその場に表示（専用のシートに置く場合向け） |
  | Android | 日付と時刻を表示し、Material のピッカーを開くボタン | 同じボタン |
  | macOS | `NSDatePicker` のテキストフィールドとステッパー | カレンダーと時計 |
  | Windows | カレンダーがドロップダウンで開く、日付と時刻の選択コントロール | 月のカレンダー。`.dateTime` ではその下に時刻の選択コントロール（`.time` はコンパクトのまま） |
  | Linux | GTK のカレンダーと、時と分のスピンボタンをポップオーバーで開くボタン | カレンダーとスピンボタンをその場に表示 |
  | Web | `<input type="date">`、`"time"`、`"datetime-local"` | 同じ入力要素 |

## 進行状況とアクティビティ

`Progress` はプログレスバーです。`value` は 0 から 1 で、どこまで進んだかわからないときは `none` にします。スタイルの `color` が塗りの色です。`ProgressRing` は同じものを円で表したもので、リングは上から時計回りに埋まり、値の変化に合わせてアニメーションします。`none` ならプラットフォームの不確定の円形インジケーターを表示します。`Activity` はスピナーです。

```asuka
component Upload {
  state done = 0.4

  Column {
    gap: 12
    Progress { value: done }
    Progress()
    ProgressRing { value: done }
    ProgressRing {
      value: done
      width: 24
      color: .green
    }
    ProgressRing()
    Activity()
  }
}
```

リングの大きさはレイアウト時に測定され、プラットフォームごとに固有のサイズがあります（iOS は 58 ポイント、Android は 48dp、Mac と Windows は 32、Linux と Web は 32）。スタイルで一辺を指定するともう一辺も同じ値になり、リングは常に正方形を保ちます。親が引き伸ばすはずのところ（`Column` の既定の `align`）では、各プラットフォームのインジケーターと同じく、そのサイズのまま中央に置かれます。スクリーンリーダーは、パーセンテージ付きの進行状況インジケーターとして読み上げます。

| | リング | 値がない場合 |
|---|---|---|
| iOS | UIKit には確定的な（determinate）リングがなく、SwiftUI の円形の `ProgressView` も iOS では値を無視します。そのため、リングは SwiftUI の円形の容量 `Gauge` と同じように描画します。ティントカラーを 30% にしたトラックの上に、ティントカラーで端の丸い線を引きます。線の太さはリングのサイズの 10 分の 1 です | `UIActivityIndicatorView` |
| Android | Material の `CircularProgressIndicator`。フレームに合った Material 3 のサイズ（extra small、small、medium と、それぞれのトラック、インセット、ギャップ）を使います | 同じもの（不確定） |
| macOS | `color` を指定せず、AppKit 自身のサイズである 16 ポイントか 32 ポイントのときは、`NSProgressIndicator` の確定的な円です。AppKit はこの円をこの 2 つのサイズでしか、しかもアクセントカラーでしか描かないため、それ以外のサイズや `color` を指定したときは、AppKit が描くものをフレームいっぱいに描画します。16〜32 ポイントでは 5 ポイントの端の丸い線を引き（32 ポイントより大きければ 32 ポイントの円の比率、16 ポイントより小さければ 16 ポイントの円の比率）、AppKit のトラックに重ねます。色は `color`、または AppKit 自身のインジケーターから読み取った AppKit の描画色（アクセントカラー、非アクティブなウィンドウではグレー、ライトとダーク）です。値は AppKit のアニメーターと同じく 0.25 秒かけて動きます。ただし「視差効果を減らす」がオンのときは動きません。どちらの場合も、VoiceOver は AppKit のインジケーターとして読み上げます | 16 ポイントと 32 ポイントでは AppKit のスピナー。それ以外のサイズでは、スピナーの 8 本のスポークをそのサイズで描画し（AppKit のスピナーは拡大縮小するとぼやけ、色も指定できないため）、先頭のスポークを `color` で描きます |
| Windows | Fluent の `ProgressRing`（WinUI のもの）で、トラックのない円弧です。ナレーターはプログレスバーとして読み上げます | `Activity` と同じく、回転する円弧 |
| Linux | `AdwSpinner` が円を描くのと同じように描画します。テキストの色を 15% にしたトラックと、アクセントカラーの円弧です | `AdwSpinner`（libadwaita 1.6 より前は `GtkSpinner`） |
| Web | SVG のリング。`aria-valuenow` を持つ `role="progressbar"` です | 回転する円弧。`prefers-reduced-motion` では回転が遅くなります |

## 画像

`Image` は画像を表示します。読み込みとデコードはプラットフォームがメインスレッドの外で行い、メモリにキャッシュします。iOS と Android では、http(s) で取得した画像をディスクにも保存し、アプリを起動し直しても使えるようにします。デスクトップではメモリにだけ保持し、ブラウザーは自身のキャッシュに保持します。

```asuka
struct Photo {
  url: string
  blurHash: string
}

component Pictures(photo: Photo) {
  Column {
    gap: 12
    Image("logo.png") { width: 64 }
    Image("https://picsum.photos/id/1015/600/400") {
      placeholder: "placeholder.png"
      mode: .fill
      label: "A river in a valley"
      aspectRatio: 3.0 / 2
      radius: 12
      clip: true
    }
    Image(photo.url) {
      blurHash: photo.blurHash
      mode: .fill
      transition: 0.4
      aspectRatio: 1
      radius: 12
    }
  }
}
```

- 画像のソースは、http(s) の URL、アプリの `assets` ディレクトリ内のパス（`"logo.png"` は `assets/logo.png`）、または絶対パス（アプリが書き出したファイルや、ユーザーが選んだ写真）です。
- `mode` は、画像を枠にどう収めるかを決めます。`.fit`（デフォルト。全体を収め、余白ができる）、`.fill`（枠を覆い、はみ出た部分を切り取る）、`.stretch`（枠の縦横比に合わせて引き伸ばす）、`.center`（本来のサイズで中央に置く）があります。
- `placeholder` は、ソースが読み込まれるまでと、読み込みに失敗したときに表示されます。すぐに表示できるアセットを使ってください。プレースホルダーを表示する場所を確保するため、画像にはサイズを指定してください。
- `blurHash` と `thumbHash` は、アプリが画像と一緒に保存しておくハッシュから作るプレースホルダーです（[BlurHash](https://blurha.sh)、[ThumbHash](https://evanw.github.io/thumbhash/)。後者は base64）。expo-image と同じです。Asuka がハッシュを小さな画像に一度だけデコードし（最近の数百個は保持されます）、`placeholder` の代わりにすぐに表示します。BlurHash 自体は形を持たないので、スタイルの `aspectRatio` でデコードされ、指定がなければ正方形で枠に引き伸ばされます。ThumbHash は元の画像の縦横比と透明度を保ちます。リストの行が画像を差し替えるので、どちらもバインディングです。
- `placeholderMode` は、プレースホルダーを枠にどう収めるかです。`.fit`、`.fill`、`.stretch`、`.center` があり、デフォルトでは画像と同じです。
- `transition` は、画像が届いたときにフェードインする秒数です。プレースホルダーの上でクロスディゾルブするか、何もないところから現れます。デフォルトでは、プレースホルダーがあるときだけ、プラットフォームの短い時間（0.25 秒。Android では `config_shortAnimTime`）でフェードインします。`-1` ならすぐに表示します。
- `animated` が true のあいだはアニメーション画像を再生し、false にするとその場で止めます。指定しなければ、ファイルが指定する回数だけ再生します。ただし、ユーザーが動きを減らすよう設定しているとき（「視差効果を減らす」、Android のアニメーションのオフ、`prefers-reduced-motion`）は、最初のフレームを表示します。
- `label` は、スクリーンリーダーに伝える画像の説明です。指定しなければ、画像は装飾として扱われます。
- `on loaded(width, height)` は読み込みが終わると画像本来のサイズを受け取り、`on error(message)` は読み込めなかったときにメッセージを受け取ります。

サイズを指定していない画像は、読み込みが終わるまで空です。読み込み後は、使える幅を超えない範囲で、1 ピクセルを 1 ポイントとした本来のサイズになります。一辺だけを指定した場合（`width`、または行の中での `grow`）は縦横比を保ちます。`aspectRatio` を指定すると、読み込む前から縦横比を決めておけます。

デコードできる形式は、各プラットフォームのデコーダーが読めるものです。

| | デコード | 再生（GIF、APNG、アニメーション WebP） |
|---|---|---|
| iOS | ImageIO：PNG、JPEG、GIF、WebP、HEIC、AVIF | ImageIO の `CGAnimateImageDataWithBlock`。ファイルの指定どおりのタイミングとループで再生します（HEIF のシーケンスも） |
| Android | `BitmapFactory`、次に `ImageDecoder`：PNG、JPEG、GIF、WebP、HEIF、AVIF | `AnimatedImageDrawable`：GIF と WebP（システムが再生できれば AVIF のシーケンスも）。APNG は最初のフレームを表示します |
| macOS | `NSImage`：PNG、JPEG、GIF、WebP、HEIC、AVIF、SVG | iOS と同じ |
| Windows | WIC：PNG、JPEG、GIF、BMP、TIFF、ICO。コーデックが入っていれば WebP、HEIF、AVIF も（WebP のものは Windows 11 に付属。HEIF には HEIF 画像拡張機能と HEVC ビデオ拡張機能、AVIF には AV1 ビデオ拡張機能が必要） | Asuka がフレームを順に再生。アニメーション WebP の各フレームは WIC の WebP コーデックでデコード |
| Linux | GDK のローダー：PNG、JPEG、GIF。インストールされていれば WebP、AVIF、HEIF、SVG、JPEG XL も | Asuka がフレームを順に再生。アニメーション WebP の各フレームは GDK の WebP ローダーでデコード |
| Web | ブラウザー | WebCodecs の `ImageDecoder` でキャンバスに描画（AVIF のシーケンスも）。ない場合（Safari）は `<img>` そのもので、一時停止中は最初のフレームを表示します |

SVG を画像として表示できるのは、Mac、Linux、Web だけです。どのプラットフォームでも SVG のアイコンを表示するには、[`std/svg`](#svg-アイコン) でベクターとして描きます。

## シンボルとアイコン

`Symbol` はシンボルを表示します。iOS と macOS では SF Symbol、Android と Web では対応する Material Symbol、Windows では対応する Segoe Fluent Icons のグリフ（Windows 11 より前は Segoe MDL2 Assets）、Linux ではアイコンテーマ（Adwaita）の対応するシンボリックアイコンで、なければ Material Symbol になります。大きさはスタイルの `font` のテキストとして決まるので文字サイズに追従し、色は `color`（デフォルトはラベルの色）になります。

```asuka
component Symbols {
  Row {
    gap: 14
    align: .center
    Symbol("bell")
    Symbol("star.fill") {
      color: .yellow
      font: .title
    }
    Symbol("archivebox") { color: .secondaryLabel }
  }
}
```

シンボルの名前は SF Symbol の名前です。ボタン、メニュー項目、バーボタン、タブも受け取ります。`"house"`、`"magnifyingglass"`、`"gear"`、`"plus"`、`"xmark"`、`"trash"`、`"square.and.arrow.up"`、`"ellipsis.circle"` などで、名前の後ろに `".fill"` を付けると塗りつぶしの形になります（`"star.fill"`）。

iOS と macOS は、どの SF Symbol でも描画できます。それ以外のプラットフォームが描画できるのは、厳選したセットだけです。このセットにはよく使うものとそれ以外のシンボルが含まれ、Material Symbols に対応付けられています。Windows では Segoe Fluent Icons のグリフに対応付けられています。セットにない名前は、これらのプラットフォームでは何も表示されません（ログに警告が出ます）。そのため、`"archivebox"` のように文字列で書いた名前は、配布するすべてのプラットフォームで確認してください。Linux では、`-symbolic` で終わる名前はアイコンテーマの名前として、そのまま使われます。

### SVG アイコン

独自のアイコンには、`std/svg` がプラットフォームのベクター描画で SVG を描きます。ラスター化はしません。`svg.Icon` のマークアップは文字列リテラルで、コンパイラーが解析します。読めないマークアップはコンパイルエラーになり、デバイス上では何も解析しません。

```asuka
import std/svg

let download = svg.Icon(r"""<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><polyline points="7 10 12 15 17 10"/><line x1="12" x2="12" y1="15" y2="3"/></svg>""")

component DownloadRow {
  Row {
    gap: 8
    svg.View(download) { color: .accent; width: 24; height: 24 }
    Text("Download")
  }
}
```

`color` は、SVG が `currentColor` で塗る部分と、SVG が塗りを指定していない部分を塗ります。既定はテキストの色 `.label` です。スタイルに幅も高さもなければ、ビューは SVG 自身のサイズになります。両方あれば、縦横比を保ってアイコンがその中に収まります。アイコンセット（Lucide、Material、Heroicons、Feather）が使うもの、つまりパス、基本図形、グループ、`use`、塗り、線、変形を読みます。テキスト、グラデーション（アイコンの色で塗ります）、クリッピング、マスク、フィルター、`fill-rule="evenodd"` は読み飛ばします。

`svg.Shape` はアプリが作ったパスを描きます。それぞれの `svg.Layer` は一つの `svg.Path` で、塗り（`fill`。ボックスの下に向かって `fade` へフェード）と線（`stroke`、`width`、`cap`、`join`）を持ちます。`svg.Point` はボックスの左上からのポイント数にボックスのサイズに対する割合を足したものなので、ボックスの大きさが変わっても、アプリが計算し直さなくても形が追従します。

```asuka
import std/svg

let tick = svg.Path().move(svg.Point(x: 4, fy: 0.5)).line(svg.Point(fx: 0.4, fy: 1, y: -4)).line(svg.Point(fx: 1, x: -4, y: 4))

component Done {
  svg.Shape([svg.Layer(path: tick, stroke: .green, width: 3, cap: .round, join: .round)]) {
    width: 32
    height: 32
  }
}
```

iOS と macOS では Core Graphics、Android では `Canvas`、ブラウザーでは `<svg>`、Windows では GDI+、Linux では Cairo で描きます。

## 動画

`Video` は、プラットフォームのプレイヤーとそのコントロールで動画を再生します。iOS では `AVPlayerViewController`、Android では Media3 の ExoPlayer、macOS では `AVPlayerView`、Linux では `GtkVideo`（GStreamer）、ブラウザーでは `<video>` です。Win32 には動画のビューがないため、Windows では WebView2 の中の `<video>` になり、Edge のコントロールが表示されます。再生は `Ref` の `play()`、`pause()`、`seek(seconds)` で操作します。

```asuka
component Clip {
  let v = Ref()

  Column {
    gap: 12
    Video("https://example.com/clip.mp4") {
      ref: v
      autoPlay: true
      muted: true
      loop: true
      radius: 12
    }
    Button("Start over") {
      on press {
        v.seek(0)
        v.play()
      }
    }
    Button("Pause") { on press { v.pause() } }
  }
}
```

- 動画のソースは、http(s) の URL（ファイルや HLS ストリーム）か、アプリのアセット内のファイルです。
- `mode` は `.fit`（余白を付けて全体を収める。デフォルト）か `.fill` です。
- `hideControls: true` は、プラットフォームのコントロール（再生、タイムライン、フルスクリーン、iOS では AirPlay とピクチャ・イン・ピクチャ）を隠します。
- `on end` は最後まで再生するたびに実行され、`on error(message)` は再生できないときに実行されます。

サイズの決まり方は `Image` と同じです。スタイルでサイズを指定しない限り、動画のサイズがわかるまでは空で、その後は動画の縦横比を保ちます。`seek` には先頭からの秒数を渡します。

## Lottie アニメーション

`std/lottie` は Lottie アニメーション（After Effects から Lottie JSON `.json` または dotLottie `.lottie` として書き出したもの）を、各プラットフォーム自身のプレーヤーで再生します。iOS と Mac では lottie-ios、Android では lottie-android、Web では lottie-web です。Windows と Linux には独自のプレーヤーがないので、そこでは Asuka が `Canvas` 上で再生します。シェイプ、グラデーション、トリムパス、リピーター、スター、プリコンポジション、アルファマットを描き、マスク、ほかのマット、画像、テキストは描きません。

```asuka
import std/lottie

component Success {
  state playing = true
  state progress = 0.0
  state done = false

  Column {
    lottie.View("success.json") {
      bind playing: playing
      bind progress: progress
      overrides: [lottie.colors("Ring", light: #0a84ff, dark: #64d2ff)]
      width: 200
      on complete { done = true }
    }
    Slider { bind value: progress }
  }
}
```

- ソースはファイルで、http(s) の URL、アプリの assets 内のパス、絶対パスのどれかです。`data` でファイルの中身を直接渡すこともできます。`animation` は dotLottie の中のアニメーションを ID で選びます。
- `playing` が true の間は再生し、false の間はその位置で止まります。バインドすると、最後まで再生したところで false になります。`loop` は繰り返し再生し、そうでなければ最後で止まって `complete` を報告します。`speed` はフレームレートの倍率です（負の値で逆再生）。
- `progress` は 0 から 1 の再生位置です。バインドすると、再生中にビューが書き込み、アプリが書き込んだ値の位置へ移動します。スライダーで動かしたり、ジェスチャーで操作したりできます。
- `segment`（フレームの `from` から `to`）か `marker`（名前で選んだマーカー）が再生する部分です。`loaded` で `Info`（サイズ、フレーム、フレームレート、マーカー）が得られます。
- `fit` はボックスの埋め方で、`.contain`（既定）、`.cover`、`.stretch`、`.center` のどれかです。スタイルにサイズがなければ、アニメーション自身のサイズになります。
- `overrides` はキーパスで色や値を変えます。テーマやダークモードに使えます。`lottie.color(keypath, c)`（キーパスの下の塗りと線）、`lottie.colors(keypath, light:, dark:)`、`lottie.opacity(keypath, v)`、`lottie.strokeWidth(keypath, w)` です。キーパスはルートからのレイヤー、グループ、シェイプの名前をドットでつないだもので、`*` は任意の一つの名前、`**` は任意の数の名前を表します（`"Ring"`、`"Circle.Disc.Fill 1"`、`"**.Stroke 1"`）。

このパッケージを import したアプリは、これらのプレーヤーと一緒にビルドされます。Swift パッケージ（lottie-spm）、Gradle ライブラリ（`com.airbnb.android:lottie`）、そしてビューが最初に表示されたときに読み込まれる lottie-web のスクリプトです。import していないアプリには含まれません。[ネイティブパッケージ](native.md)なので、スマートフォンで `asuka dev` を使うと、このパッケージを使うアプリはホットリロードモードではなく、それ専用のビルドとして動きます。

## Web ページ

`WebView` は、プラットフォームの Web ビューで Web ページを表示します。スクロール、ズーム、テキストの選択もプラットフォームのものです。iOS と macOS では `WKWebView`、Android では `WebView`、Windows では WebView2（Edge のランタイム）、Linux では WebKitGTK 6.0 の `WebKitWebView` です。Linux で WebKitGTK がインストールされていない場合は何も表示せず、何をインストールすればよいかを伝えるメッセージで `on error` を一度だけ呼びます。ブラウザーでは `<iframe>` になり、以下で説明する機能をすべて使えるのはアプリ自身のサイトのページに限られます。詳しくは [Web](web.md#web-ビュー) を参照してください。高さも `grow` も指定しなければ、与えられた領域いっぱいに広がります。

```asuka
import platform/device

component Browser {
  state page = WebState()

  let web = Ref()

  Column {
    grow: 1
    Row {
      gap: 8
      padding: [8, 12]
      align: .center
      Button("") {
        icon: "chevron.backward"
        label: "Back"
        disabled: !page.canGoBack
        on press { web.goBack() }
      }
      Text(page.title) {
        grow: 1
        lines: 1
      }
      Button("") {
        icon: "arrow.clockwise"
        label: "Reload"
        on press { web.reload() }
      }
    }
    if page.loading { Progress { value: page.progress } }
    WebView("https://example.com") {
      ref: web
      block: ["doubleclick.net", "/ads/"]
      on state(s) { page = s }
      on message(m) { print("the page says {m}") }
      on newWindow(url) { device.openURL(url) }
    }
  }
}
```

表示するものを指定するプロパティは次のとおりです。

- その URL は、http(s) の URL か、アプリのアセット内のファイルです（ページ内の相対リンクもアセットを指します）。代わりに、表示するマークアップを `html` で渡すこともできます。
- `userAgent` は、ページの取得に使うユーザーエージェントで、`navigator.userAgent` の値にもなります。空ならプラットフォームのものを使います。
- `preload` は、各ページ自身のスクリプトより前に、すべてのフレームで実行するスクリプトです。`fetch` をフックしたり、グローバル変数を設定したりするのに使います。
- `block` は、広告ブロッカーのように特定のリクエストを読み込ませません。各項目はホスト名（`"doubleclick.net"`。サブドメインにも一致します）か、`/` を含む場合は URL の一部（`"/ads/"`）です。

`preload`、`userAgent`、`block` は、ビューを作るときに設定されます。

ページの様子を知らせるコールバックは次のとおりです。

- `on loaded(url)` は読み込みが完了した各ページの URL を受け取り、`on error(message)` は読み込めなかったときや、ページの Web プロセスが終了したとき（メモリ不足やクラッシュ。「the page's web process ended」）にメッセージを受け取ります。プロセスが終了したあとは、ビューがページを一度読み込み直します。
- `on state(s)` は、アドレスバーのようにページの状態を追います。`WebState` で、`url`、`title`、`loading`、`progress`（0 から 1）、`canGoBack`、`canGoForward` を持ちます。
- `on newWindow(url)` は、ページが新しいウィンドウで開こうとした URL（`window.open` や `target="_blank"` のリンク）を受け取ります。指定しなければ、そのページはこのビューの中で読み込まれます。
- `on download(d)` は、ページが表示できないレスポンス、つまり保存すべきファイル（`.zip` へのリンクや `Content-Disposition: attachment`）を、`url`、`mime`、`filename`、`size`（不明なら -1）を持つ `WebDownload` として受け取ります。ダウンロード自体は行いません。
- `on request(url)` は、ページが送る各リクエスト（スクリプト、画像、メディア、`fetch`）の URL を受け取ります。**iOS と macOS では使えません**。`WKWebView` がアプリにリクエストを見せないためです。

`Ref` の `goBack()`、`goForward()`、`reload()`、`stopLoading()` で、履歴を戻る・進む、ページを再読み込みする、読み込みを止める、ができます。`load(url)` は URL をすぐに読み込みます。ページが移動したあと（リンクをたどったとき）にビュー自身の `url` を読み込み直すのにも使えます。`url` を今と同じ値に設定しても何も変わらないので、表示するたびに Web ビューを最初のページに戻すタブは `load` を呼びます。

### ページとのやり取り

ページは `window.asuka.postMessage(text)` でアプリにメッセージを送り、それによって `on message` が実行されます。アプリからは、`Ref` の `run(script)` でページの中でスクリプトを実行できます。これは何も返しません。`try await eval(script)` は値を JSON で返します。最後の文の値か、返した Promise が解決した値（`undefined` なら `null`）で、スクリプトが投げたエラーはそのまま投げられます。

```asuka
component Page {
  let web = Ref()

  WebView {
    ref: web
    html: "<button onclick=\"asuka.postMessage('tapped')\">Tap</button>"
    on message(m) {
      let title = try? await web.eval("document.title")
      print("{m} {title ?? ""}")
    }
  }
}
```

`eval` は Promise を待つためにページの `eval` を使いますが、コンテンツセキュリティポリシー（CSP）で `'unsafe-eval'` を許可していないページでは `eval` がブロックされます。そのようなページでは、iOS、Android、macOS、Linux はスクリプトを `run` と同じ方法で実行します。この方法には CSP が適用されません。値はこれまでどおり最後の文の値で、エラーもスクリプトが投げたものですが、Promise は待ちません（Android では `{}` が返り、ほかのプラットフォームではエラーになります）。また、iOS と macOS ではオブジェクトのキーの順序が変わることがあります。Windows は `eval` を使わないので、これまでどおり Promise を待ちます。Web では、アプリがページ内で実行するスクリプトにもブラウザーがページのポリシーを適用するため、`eval` をブロックするページでは `run` も `eval` も拒否されます。

`try await webview.cookies(url)` は、Web ビューが持っているその URL の Cookie を、`Cookie` ヘッダーの形で取得します。ユーザーがページでサインインした状態を、アプリ自身のリクエストで使うためのものです。

[`examples/downloader`](../../examples/downloader) は、これらをすべて使った例です。アドレスバー、プリロードスクリプト、ブロック、ダウンロードを備えたブラウザーになっています。

## 図形

`Shape` は輪郭を描き、塗りつぶし、線、またはその両方で描画します。パスの関数は図形のサイズに合わせて輪郭を作り、サイズが変わったときや、読んでいる状態が変わったときに実行し直されます。図形のサイズはスタイルで決まり（幅と高さ、縦横比、`grow`）、何も指定しなければ空です。

```asuka
component Triangle {
  Shape((w, h) => Path().move(w / 2, 0).line(w, h).line(0, h).close()) {
    fill: .orange
    width: 48
    height: 48
  }
}
```

`Path` の座標は、図形の左上を原点とするポイント単位です。メソッドはパスに要素を追加してパス自身を返すので、つなげて書けます。`move`、`line`、`quad`、`cubic`、`arc`（角度はラジアンで、x 軸から時計回り）、`close`、そして図形をまるごと加える `rect`、`roundedRect`、`ellipse` があります。

`stroke` と `lineWidth`（デフォルトは 1）で輪郭に線を引きます。線端は `lineCap`（`.butt`、`.round`、`.square`）、角は `lineJoin`（`.miter`、`.round`、`.bevel`）で指定します。`trimStart` と `trimEnd` は 0 から 1 で輪郭の一部だけを描き、進捗リングや、自分を描いていく線になります。`animate` の中で変えるとアニメーションします（[モーション](motion.md) を参照）。`dash` は線を破線にします。描く長さと空ける長さを交互にポイントで並べ（`[6, 4]`。個数が奇数なら繰り返します）、SwiftUI の `StrokeStyle(dash:)` と同じです。`dashPhase` は、パターンのどこから始めるかです。[`examples/usage`](../../examples/usage) は、これで予測線を描いています。

```asuka
import std/math

fn ring(w: float, h: float) -> Path {
  let r = min(w, h) / 2 - 4
  return Path().arc(w / 2, h / 2, r, start: -math.pi / 2, end: 3 * math.pi / 2)
}

component ProgressCircle(done: float) {
  View {
    label: "Progress"
    width: 72
    height: 72
    Shape(ring) {
      stroke: .secondaryBackground
      lineWidth: 8
      absolute: true
      width: 100%
      height: 100%
    }
    Shape(ring) {
      stroke: .accent
      lineWidth: 8
      lineCap: .round
      trimEnd: done
      absolute: true
      width: 100%
      height: 100%
    }
  }
}
```

### Canvas

`Canvas` には、アプリが自分で描くものを描画します。グラフ、メーター、手書きのサイン、ゲームの盤面など、既存のビューにないものなら何でもです。関数は、左上を原点とするポイント単位で、`Graphics` を使って描きます。

```asuka
component Level(level: float) {
  Canvas(g => {
    let w = g.width
    let h = g.height
    g.fill(g.path().roundedRect(0, 0, w, h, 12), .secondaryBackground)
    g.fill(g.path().roundedRect(0, 0, w * level, h, 12), .accent)
    g.text("{(level * 100).fixed(0)}%", w / 2, h / 2 + 6, CanvasText(weight: .semibold, align: .center))
  }) {
    height: 36
  }
}
```

- `g.path()` は組み立て用のパス（Shape のパスと同じもの）で、描画のたびに再利用されます。
- `fill` と `stroke`（`StrokeStyle` で太さ、線端、角、破線を指定）は、色、`LinearGradient`、`RadialGradient`（中心、半径、そして始まりの焦点）でパスを塗ります。`text` は 1 行のテキストを描き、ベースラインを y に、左端、中央、右端のいずれか（`CanvasText` の `align`）を x に合わせます。
- `translate`、`scale`、`rotate`（度単位、時計回り）、`transform`（任意のアフィン行列。傾斜も）、`clip`、`opacity`、`evenOdd`（偶奇規則で塗りつぶしとクリップを行う。デフォルトは非ゼロ規則）は、`save` と `restore` で囲んで使います。
- 色にはセマンティックカラー（`.label`、`.separator`、`.accent`）を使えます。これらはダークモードに追従します。

キャンバスは、レイアウトが決まると描画され、サイズが変わったときや関数が読んだ状態が変わったときに、エフェクトと同じように再描画されます。描画のたびにディスプレイリストが記録され、前回と違うときだけ送られて、プラットフォームがそれを再生します。iOS と macOS では Core Graphics、Android では `Canvas`、Windows では GDI+、Linux では GTK のスナップショット（GSK）、ブラウザーでは 2D の `<canvas>` です。以前に同じ量を描いたことがあれば、記録の際にメモリを割り当てることはありません。キャンバス自体はタップを受け取らないので、`on press` や `on pan` を持つ `View` で包んでください。これらはキャンバスの座標系でポイントを報告します。スクリーンリーダーはキャンバスを読み上げないので、包んでいるビューに `label` を付けてください。

### グラフ

`std/chart` はグラフを描きます。値の軸の上に棒、線、面、点を描き、ラベルにはプラットフォームのテキストを使います（ユーザーの文字サイズに合わせて拡大縮小されます）。Asuka で書かれていて、目盛りと図形は一度だけ計算され、`svg.Shape` が描くので、どのプラットフォームでも同じになります。

```asuka
import std/chart

component Week(steps: [float], goal: [float]) {
  chart.View([
    chart.Series(name: "Steps", kind: .bar, values: steps),
    chart.Series(name: "Goal", kind: .line, values: goal, color: #ff9500),
  ]) {
    labels: ["Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"]
    label: "Steps this week"
    height: 180
  }
}
```

- 系列はラベルごとに一つの値を持ちます。棒の系列は横に並び、面、線、点はその上に重なります。`smooth` は値を飛び越えない単調な曲線を描き、`width` は線の太さか点の大きさです。
- 値の軸はデータに合わせ、目盛りの切りのよいところまで外側に広げます（棒と面は 0 まで）。`low` と `high` で範囲を固定できます。目盛りのラベルは目盛りを区別できる範囲で短くなります（`2.5k`）。`grid: false` で図形の後ろの線をなくせます。
- 色の既定は、アプリのアクセントカラー、次にシステムの色です（`chart.palette`）。
- スクリーンリーダーはグラフを一枚の画像として読みます。まず `label`、続いて各系列の値です。スタイルに高さがなければ高さは 200 です。

### シェーダー

シェーダーは、GPU のプログラムで枠の各ピクセルに色を付けます。流れるグラデーション、ノイズ、光、波紋などです。`shader Name(params) { … }` で Asuka 自身の構文のまま宣言し、ビューとして使います。引数がそのまま uniform になり、コンポーネントの引数と同じように設定でき、コンポーネントと同じようにスタイルも受け取ります。シェーダーはアプリのコンパイル時にすべてのプラットフォーム向けにコンパイルされます。間違いはその行を示すコンパイルエラーになり、デバイスではプラットフォーム自身のプログラムだけがコンパイルされます。

```asuka
/// 枠の中央で明滅する光。
shader Glow(tint: Color, level: float = 1) {
  let speed = 2

  fn fragment(pos: vec2) -> vec4 {
    let d = distance(pos / size, vec2(0.5))
    let g = smoothstep(0.5, 0, d) * (0.8 + 0.2 * sin(time * speed)) * level
    return vec4(tint.rgb * g, 1) * tint.a
  }
}

component Card(level: float) {
  Glow(tint: .accent, level: level) {
    animated: true
    height: 200
    radius: 16
    clip: true
    background: #0b1030
  }
}
```

- `fn fragment(pos: vec2) -> vec4` は、`pos`（枠の左上からのピクセルの中心、ポイント単位）から、そのピクセルの色を返します。色はプリマルチプライドで、赤、緑、青、アルファが 0 から 1 です。シェーダーの中では、`size` が枠のサイズ（ポイント）、`scale` が 1 ポイントあたりのピクセル数、`time` が表示されてからの秒数です。
- 引数は `float`、`int`、`bool`、`Color`（シェーダーの中では各チャンネルが 0 から 1 の `vec4`）のいずれかです。引数が変わると枠が描き直されます。`animated: true` で `time` が進み、ディスプレイが描くフレームごとに枠も描かれます。
- 値は `bool`、`int`、`float`、`vec2` から `vec4`、`ivec2` から `ivec4`、`mat2` から `mat4` で、スウィズル（`v.rgb`、`p.yx`）が使えます。色のリテラルは `vec4` です。GLSL の関数（`sin`、`mix`、`smoothstep`、`dot`、`normalize` など）と自分の関数（`fn wave(x: float) -> float = …`）を呼べ、定数（`let`）、`let` と `var`、代入、`if`、`while`、`for i in 0..<n`、`return`、`break`、`continue` があります。GPU で実行できないもの（クロージャ、文字列、オプショナル、値としての `if`）は、その旨を伝えるコンパイルエラーになります。
- コンパイラーは、Android で実行できない部分もすべてのプラットフォームで同じように報告します。AGSL は GLSL ES 1.00 の言語なので、符号なし整数、ビット演算、整数の剰余（浮動小数点数の `mod` は使えます）、微分、定数でない範囲のループは使えません。

iOS と macOS では Metal（`CAMetalLayer`）、Android 13 以降では AGSL（`RuntimeShader`。それより前は空の枠）、ブラウザーでは WebGL 2（GLSL ES 3.00）、Windows では Direct3D 11（HLSL。GPU がなければシステムのソフトウェアラスタライザー WARP）、Linux では OpenGL（`GtkGLArea`。GPU がなければ Mesa のソフトウェアラスタライザー）です。各プラットフォームのプログラムはアプリと一緒にコンパイルされ、デバイスでは自分のシェーディング言語のものだけがコンパイルされます。Windows ではフレームをメインスレッドの外で描画し、できたところで表示します。GL のない Linux（`GDK_DISABLE=gl`）では空の枠が表示されます。最初のフレームまでの代わりとして、枠に `background` を指定してください。

## コンテナー

`View` は、ほかのビューを入れるコンテナーです。`Column` と `Row` は、子を上から下へ、または先頭から末尾へ並べるビューです。`SafeArea` は、ノッチ、システムバー、キーボードの分の余白を持つ列です。中身の大きさと配置の決まり方は、[レイアウトとスタイル](layout.md)で説明しています。

見た目、イベント、アクセシビリティのプロパティを何も持たないビューは、自分のネイティブビューを作りません。その子は、ネイティブビューを持つ最も近い祖先に直接入ります。そのため、行や列をいくら入れ子にしてもコストはかかりません。

```asuka
import platform/device

component Person {
  state open = false

  Row {
    gap: 12
    padding: 16
    background: .secondaryBackground
    radius: 12
    label: "Ada, 2 unread"
    on press { open = !open }
    on longPress { device.haptic(.medium) }
    Symbol("person.fill")
    Column {
      grow: 1
      gap: 2
      Text("Ada") { font: .headline }
      Text("2 unread") { color: .secondaryLabel }
    }
  }
}
```

`View`、`Row`、`Column`、`SafeArea` は次のものを受け取ります。

- `on press` と `on longPress`。
- プラットフォームのジェスチャー認識を使うジェスチャー：`on pan(g)`、`on pinch(g)`、`on rotate(g)`（それぞれ `Gesture` を受け取ります）と `on swipe(direction)`。[モーション](motion.md) を参照してください。
- インタラクションの状態。ビュー自身のスタイルは、バリアント（`pressed { … }`、`hovered { … }`、`dragging { … }`、`dragOver { … }`）でこれに追従します。ほかのビューは、その `Ref`（`card.hovered`、`card.pressed`）で追従します。副作用は `on pressedChange` と `on hoverChange` に書きます。ドラッグ＆ドロップは `dragItems` と `dropTypes`、そして `on drop(d)` です。[モーション](motion.md#押下とホバーの状態) を参照してください。
- `on layout(frame)`。レイアウトが決まったときと、その後フレームが変わるたびに、ビューの `Frame` を受け取って実行されます。
- `href`。押されたときに移動するルートです（リンクなので、`role: .link` も付けてください）。[ルート](navigation.md#ルート) を参照してください。
- アクセシビリティ：`label`、`hint`、`role`、`a11yHidden`、`actions`、`on adjust(by)`。[アクセシビリティ](accessibility.md) を参照してください。

### Scroll

`Scroll` は、子を一方向にスクロールさせます。スクロールする範囲ができるよう、サイズか `grow` を指定してください。スタイルはスクロールビュー自体の枠です。子はその中で縦に並びます（中に `SafeArea` や `Column` を置くと、余白と間隔を付けられます）。

```asuka
component Page {
  state down = false

  let scroll = Ref()

  View {
    grow: 1
    Scroll {
      ref: scroll
      grow: 1
      on scroll(_, y) { down = y > 300 }
      SafeArea {
        padding: 20
        gap: 16
        for i in 0..<50 key i { Text("Line {i + 1}") }
      }
    }
    if down {
      View {
        absolute: true
        right: 20
        bottom: 20
        Button("") {
          icon: "arrow.up"
          label: "Back to top"
          variant: .filled
          on press { scroll.scrollTo(0, 0) }
        }
      }
    }
  }
}
```

- `on scroll(x, y)` は、スクロール中にコンテンツのオフセット（ポイント単位）を受け取って実行されます。
- `Ref` の `scrollTo(x, y, animated:)` は、指定したオフセットまでスクロールします（末尾を越えれば末尾まで）。`scrollToEnd(animated:)` は一番下まで、横方向なら末尾側の端までスクロールします。どちらも `List` でも使えます。
- `horizontal: true` を指定すると、横方向にスクロールします。

```asuka
component Tags(tags: [string]) {
  Scroll {
    horizontal: true
    Row {
      gap: 8
      padding: [0, 16]
      for t in tags key t { Text(t) }
    }
  }
}
```

**ページ。** `paging: true` を指定すると、ページの切れ目でだけ止まるようになります。カルーセル、オンボーディングの各ステップ、全画面の動画のフィードなどに使います。子の一つ一つがページで、スクロール方向にはスクロールビューと同じ大きさです。`bind page` は表示中のページで、スワイプが落ち着いたときに書き込まれ、書き込むとそのページまでスクロールします。`PageControl` はページごとの点で現在位置を示し、点をタップしてもページを移動できます。

```asuka
component Carousel {
  state page = 0

  Column {
    gap: 8
    Scroll {
      horizontal: true
      paging: true
      bind page: page
      height: 200
      for i in 0..<3 key i { Text("Card {i + 1}") }
    }
    PageControl(3) { bind page: page }
  }
}
```

| | iOS | Android | macOS | Windows | Linux | Web |
|---|---|---|---|---|---|---|
| ページング | `isPagingEnabled` | スワイプのたびに次のページ、または元のページにスナップ | スワイプが終わると最寄りのページで止まる | ホイールの 1 ノッチごとに 1 ページ（FlipView と同じ） | ホイールの 1 ノッチごとに 1 ページ。タッチパッドやタッチスクリーンのスワイプはページで止まり、矢印キー、Page Up、Page Down で 1 ページずつ移動 | CSS のスクロールスナップ |
| `PageControl` | `UIPageControl` | テーマのプライマリーカラーのドット（Material にはない） | アクセントカラーのドット | ピップ（PipsPager と同じ） | libadwaita に倣って描画したドット。現在のページのものは大きく濃い | ドット。現在のページのものはラベルの色 |

ページは、読む方向の始まりから数えます。右から左のレイアウトでは、最初のページは一番右です。画面の回転などでスクロールビューのサイズが変わっても、同じページにとどまります。スクリーンリーダーはドットを「3 ページ中 2 ページ目」のように読み上げ、ドットを調整してページを移動できます。

**バーの下へのスクロール。** スクロールビュー（`Scroll`、`List`）の上端が、内容が透けて見えるバー（iOS のナビゲーションバーや、デスクトップのウインドウの隠したタイトルバー）の下にあるときは、内容はバーの下から始まり、スクロールするとバーの下へ入っていきます。`edgeEffect` は、内容がバーの下に入るときの見せ方です：`.automatic`（プラットフォームの既定）、`.soft`（ぼかして消える）、`.hard`（はっきりした境界）。iOS 26 と macOS 26 のスクロールエッジ効果です（Mac では、そのビューがある分割ビューの列に適用されます）。ほかのプラットフォームでは何もしません。

**引っ張って更新。** `on refresh` を指定すると有効になります（`UIRefreshControl`、`SwipeRefreshLayout`）。更新している間は `refreshing` を true にしておき、false に戻るとスピナーが消えます。`refreshing` を指定しなければ、ハンドラーが戻るとすぐに消えます。macOS では、トラックパッドや Magic Mouse のスクロールで上端を越えて引っ張れます（iOS と同じです）。Windows と Linux はデスクトップの流儀で更新します。F5（Linux では Ctrl+R も）で、フォーカスのあるスクロールビュー、なければ画面上でもっとも内側のスクロールビューを更新し、その間は上端にスピナーを表示します。ブラウザーでは、タッチで下へ引っ張ります。

```asuka
component Feed {
  state refreshing = false

  Scroll {
    grow: 1
    refreshing: refreshing
    on refresh {
      refreshing = true
      await sleep(1s) // fetch
      refreshing = false
    }
    Text("Pull down")
  }
}
```

**キーボードの上のバー。** `TextField` の `accessory { … }` は、フィールドにフォーカスがあるあいだキーボードの上に表示するバーです。文字数と「完了」、書式のボタン、クイック返信などを置きます（SwiftUI のキーボードツールバー）。幅はウィンドウと同じで、高さは中身に合わせ、フィールドと一緒に作られて一緒に破棄されます。

```asuka
component Note {
  state note = ""
  state focused = false

  TextField {
    bind text: note
    bind focused: focused
    multiline: true
    accessory {
      Row {
        padding: [6, 16]
        align: .center
        material: .bar
        Text("{note.count()} characters") { grow: 1 }
        Button("Done") { on press { focused = false } }
      }
    }
  }
}
```

iOS では、フィールドの `inputAccessoryView` になります。キーボードと一緒に動き、ユーザーがスワイプでキーボードを閉じるときも追従します。Android にはこれに当たるものがないため、バーをフィールドのウィンドウ（シートのウィンドウも含む）の、キーボードの上端に置き、フレームごとにキーボードを追います。オンスクリーンキーボードのない macOS、Windows、Linux では表示されません。ブラウザーでは、タッチスクリーンでだけ表示されます。

**装飾付きテキスト。** `bind rich` を指定すると、フィールドは装飾付きテキストのテキストエリアになります。メールの本文や書式付きのメッセージ向けで、太字、斜体、下線、取り消し線、等幅のコード、リンクを扱えます（スパンが持つ色やテキストスタイルはそのまま保たれます）。`RichText` が表示するのと同じ `[Span]` です。ユーザーの入力や書式の操作で書き込まれ、書き込むとテキストが置き換わります。`Ref` の `toggleFormat(.bold)` は、選択範囲のすべてがその書式でなければ書式を付け、そうでなければ外します（`format(.bold, on:)` はどちらかに決めて設定します）。選択範囲がなければ、次に入力する文字に効きます。`setLink(url)` は選択範囲にリンクを付けます（`""` で外し、カーソル位置ではカーソルがあるリンクに効きます）。`on formats(f)` は、カーソル位置の書式、または選択範囲全体が持つ書式を報告するので、ツールバーの表示に使えます。

```asuka
component Composer {
  state body = [Span(text: "Hi,\n\nThe "), Span(text: "1.0", font: .body.bold()), Span(text: " build is ready.")]
  state formats = TextFormats()

  let field = Ref()

  Column {
    grow: 1
    Button("") {
      icon: "bold"
      label: "Bold"
      background: if formats.bold { .secondaryBackground } else { .clear }
      on press { field.toggleFormat(.bold) }
    }
    TextField {
      ref: field
      bind rich: body
      plain: true
      grow: 1
      on formats(f) { formats = f }
    }
  }
}
```

| | iOS | Android | macOS | Windows | Linux | Web |
|---|---|---|---|---|---|---|
| ビュー | `UITextView` の属性付きテキスト | `EditText` のスパン | リッチテキストの `NSTextView` | RTF を介したリッチエディット（`RICHEDIT50W`） | `GtkTextView` のバッファーのタグ | `contenteditable` な要素 |
| ユーザーによる書式設定 | 編集メニューの B/I/U | ハードウェアキーボードの Ctrl+B、Ctrl+I、Ctrl+U | 「フォーマット」メニュー、⌘B ⌘I ⌘U | Ctrl+B、Ctrl+I、Ctrl+U | Ctrl+B、Ctrl+I、Ctrl+U | ブラウザーの ⌘B ⌘I ⌘U（Mac 以外では Ctrl） |
| 書式設定の取り消し | 可能 | 可能。フィールド独自の履歴（Android 標準の履歴はテキストしか保持しない）、Ctrl+Z、Ctrl+Shift+Z、Ctrl+Y | 可能 | 可能 | GTK の履歴（Ctrl+Z、Ctrl+Shift+Z）。テキストしか保持しない | 可能。ページの編集コマンドによる |

[`examples/mail`](../../examples/mail) には、ツールバー付きのメール作成画面があります。

**パディング。** テキストやフィールドの `padding`（と `border`）は、CSS と同じく、フレームの内側でコンテンツの周りに取られます。背景はパディングの部分まで塗られ、テキストはその内側に描かれ、フィールドの編集もその内側で行われます。

## リスト

`List` は、プラットフォームの再利用型のリスト（`UICollectionView`、`RecyclerView`）で、項目ごとに 1 行を表示します。デスクトップと Web には任意のビューを行として持てるリストがないため、Asuka がプラットフォームのスクロールビューの中で、行を自前で再利用します。行は画面上のセルの分しか作られません。スクロールして画面外に出たセルは別の項目に再利用され、更新されるのは値が違うプロパティだけです。[`examples/list10k`](../../examples/list10k) には 1 万行があります。

```asuka
struct Message {
  id: int
  from: string
  subject: string
}

state messages = [Message(id: 1, from: "Ada", subject: "Lunch?"), Message(id: 2, from: "Grace", subject: "The report")]

component Inbox {
  List {
    for m in messages key m.id {
      Column {
        gap: 2
        padding: [12, 20]
        Text(m.from) { font: .headline }
        Text(m.subject) { color: .secondaryLabel }
      }
    }
  }
}
```

その `for` は、項目と各項目のキーを受け取り、項目（とそのインデックス。`for (i, m) in messages.enumerated()`）から行を作ります。キーは、リストが変わったときにどの項目がどれかを示します。変わった項目はその行を更新し、移動した項目は行を移動し、プラットフォームがその差分をアニメーションします。行のバインディングは項目を読むので、再利用された行は新しい項目に追従します。

リストは与えられた領域を埋めます。高さも `grow` も指定しなければ、広がって領域を埋めます。行のあいだには、プラットフォームの区切り線を描きます。

`List` は次のものを受け取ります。

- `swipeActions`：行のアクションで、行をスワイプすると現れます。行の一番外側のビューに書くか（`swipeActions: [...]`）、リストに行のインデックスの関数として書きます。iOS では行の末尾側のアクションになり、破壊的なアクションは赤で表示され、最後までスワイプすると最初のアクションが実行されます。Android では、Material のスワイプして閉じる操作と同じように、最後までスワイプすると、最初のアクションの色とアイコンを背景にそのアクションが実行されます。macOS、Windows、Linux、Web では行のコンテキストメニュー（副ボタンのクリック。Linux と Web では長押しも）になり、先頭側、末尾側の順に並びます。スクリーンリーダー向けに、同じものを行の
- `on endReached`：最後の数行が表示されそうになると、リストの長さごとに一度だけ実行されます。ここで続きを読み込みます。
- `on refresh` と `refreshing`：`Scroll` と同じ、引っ張って更新です（Windows と Linux では F5）。
- `on scroll(x, y)`：`Scroll` と同じです。
- `appearance`：リストの見た目です。`.plain` は、端から端までの行を区切り線で仕切ります。`.insetGrouped` は、プラットフォームのグループ化されたリストです（Linux では libadwaita の boxed list）。`.bare` は区切り線も行の背景もないので、リストの後ろにあるものが透けて見えます。自分でカードを描く行に使います。bare のリストでは、行のあいだの余白は行自身が取ります。スワイプも引き続き使えます。

```asuka
struct Message {
  id: int
  subject: string
}

fn fetch(from: int, n: int) -> [Message] = (from..<from + n).map(i => Message(id: i, subject: "Message {i + 1}"))

component Inbox {
  state messages = fetch(0, 30)

  List {
    on endReached {
      messages += fetch(messages.count(), 30)
    }
    for m in messages key m.id {
      Row {
        padding: [12, 20]
        swipeActions: [
          .button(title: "Delete", icon: "trash", role: .destructive, run: () => messages.removeWhere(x => x.id == m.id)),
          .button(title: "Archive", icon: "archivebox", run: () => messages.removeWhere(x => x.id == m.id)),
        ]
        Text(m.subject)
      }
    }
  }
}
```

`leadingSwipeActions` は、反対方向、つまり行の先頭側の端からスワイプすると現れるアクションです（「メール」の「未開封」や「ピン」など）。最初のアクションがその端に置かれ、最後までスワイプしたときに実行されます。iOS ではアクションに色が付き、Android では Material の Filled ボタンとして表示されます。

`Ref` の `scrollToItem(index, animated:)` は、項目が見えるところまでリストをスクロールします。`scrollTo` と `scrollToEnd` も使えます。

### チャット：下端に固定したリスト

`anchor: .bottom` は、チャット向けのリストです。最後の行を表示した状態で開き、行が領域を埋めないときは下端に寄せます。末尾にいるあいだは、末尾に行が追加されたときも、行の高さが変わったときも、リストが短くなったとき（キーボードが出たときなど）も、末尾にとどまります。ユーザーが上にスクロールすると末尾から離れ、末尾までスクロールし直すか `scrollToEnd()` を呼ぶと、再び末尾に固定されます。`on startReached` は、最初の数行が表示されそうになると、先頭の項目ごとに一度だけ呼ばれます。ここで古いメッセージを読み込みます。

アンカーがどちらであっても、画面に表示中の行より上で行が追加・削除されたとき、表示中の行は動きません（React Native の `maintainVisibleContentPosition`）。上端に古いメッセージを読み込んだり、上の項目を削除したりしても、ユーザーは読んでいたところをそのまま読み続けられます。新しい行を表示するのは、リストが固定されている側の端だけです。上から読むリストなら上端（新着メールなど）、チャットなら末尾です。

```asuka
struct Message {
  id: int
  text: string
}

component Chat {
  state msgs: [Message] = []
  state next = 0

  let list = Ref()

  fn send(text: string) {
    next += 1
    msgs.push(Message(id: next, text: text))
    list.scrollToEnd() // one's own message shows, wherever the list was
  }

  Column {
    grow: 1
    List {
      ref: list
      anchor: .bottom
      on startReached { print("load older") }
      for m in msgs key m.id { Text(m.text) }
    }
    Button("Send") { on press { send("Hello") } }
  }
}
```

[`examples/chat`](../../examples/chat) は、会話画面をひととおり実装した例です。

### セクション

`section` は、行をセクションに分けます。行のインデックスからセクションのキーへの関数です。同じキーを持つ隣り合った行が 1 つのセクションになり、`header` のビューが見出しになります（チャットの日付、連絡先の頭文字、メールボックスの「今日」「昨日」など）。`header` はキーを受け取り、それに追従します。見出しはそれ自体が 1 つの行で、画面に入るときに作られ、その後は別のセクションに再利用されます。

```asuka
struct Message {
  id: int
  day: string
  text: string
}

state msgs = [Message(id: 1, day: "Yesterday", text: "Hi"), Message(id: 2, day: "Today", text: "Hello")]

component Chat {
  List {
    section: i => msgs[i].day
    header: day => Text(day) {
      font: .footnote.semibold()
      padding: [6, 20]
    }
    for m in msgs key m.id { Text(m.text) }
  }
}
```

plain または bare のリストの見出しは、セクションの行がその下をスクロールしていくあいだ上端にとどまり、次の見出しが来ると押し上げられます（UITableView の plain スタイルと同じです。これに当たるものがない Android では、上端のセクションに結び付けたセルをリストの上に重ねます。デスクトップと Web では、見出しの行そのものがとどまります）。plain のリストの見出しはリストの背景の上に表示され、bare のリストの見出しは自分で背景を描きます（メッセージの上に重ねるチャットのピルなど）。inset grouped のリストでは、各セクションがそれぞれ独立したグループとして見出しの下に置かれ、見出しはセクションと一緒にスクロールします。

### 行の選択

`selecting` は、リストを選択モードにします（「メール」の「編集」）。各行にプラットフォームの選択コントロールが表示され、タップは行を押す代わりに選択・選択解除し、行はスワイプできなくなります。行の `selected` は選択されているかどうかで（追従されるので、状態に保持した選択は変わるたびに表示されます）、`on select(on)` はユーザーの選択を受け取ります。アプリが受け入れなかった選択は元に戻ります。行はコントロールの分だけ狭く配置されます。

```asuka
struct Message {
  id: int
  text: string
}

state msgs = [Message(id: 1, text: "Hi"), Message(id: 2, text: "Hello")]

component Mailbox {
  state selecting = false
  state chosen: [int] = []

  List {
    selecting: selecting
    for m in msgs key m.id {
      Row {
        padding: [12, 20]
        selected: chosen.contains(m.id)
        on select(on) {
          if on { chosen.push(m.id) } else { chosen.removeWhere(id => id == m.id) }
        }
        on selectBegin { selecting = true }
        Text(m.text)
      }
    }
  }
}
```

| | iOS | Android | macOS、Windows、Linux | Web |
|---|---|---|---|---|
| コントロール | UIKit の編集モードの円 | 行の末尾の Material のチェックボックス | 行のハイライト（アクセントカラーの選択色。Windows では WinUI のリストのピル） | 行の先頭にある、チェックマークで塗りつぶされる円。行には色が付く |
| `on selectBegin` | 行の上での 2 本指のパン。そのまま、なぞった行を次々に選択する | 長押し（Gmail と同じ） | Mac では ⌘ クリック、Windows と Linux では Ctrl クリック（Finder やエクスプローラーと同じ） | ⌘ クリック（Apple 以外のシステムでは Ctrl クリック）。タッチスクリーンでは長押し |
| スクリーンリーダー | 各行が選択可能 | 各行がチェックボックスで、行のテキストがラベル | — | 各行の選択状態（`aria-selected`） |

### グリッド

`columns` を指定すると、リストはグリッドになります（`LazyVGrid`、`GridLayoutManager`）。項目はその数ずつ行を埋め、すべて同じ幅になり、縦横に `gap` の間隔を空けて並びます。行の高さは、その行で最も高い項目に合わせます。グリッドには区切り線もスワイプアクションもありません。

```asuka
component Photos(ids: [int]) {
  List {
    columns: 3
    gap: 2
    for id in ids key id {
      Image("https://picsum.photos/id/{id}/300/300") {
        mode: .fill
        aspectRatio: 1
      }
    }
  }
}
```

### List か、Scroll の中の for か

`List` の外の `for` も、同じようにキーを付けて項目ごとにビューを作ります。ただし、すべての行を作り、兄弟のビューと並ぶ通常のビューとして置きます。ページの一部になる短いリスト、たとえばいくつかのタグ、フォームの行、見出しの下のセクションなどには、`Scroll` の中で `for` を使ってください。

```asuka
component Tags(tags: [string]) {
  Scroll {
    grow: 1
    Column {
      padding: 20
      gap: 8
      Text("Tags") { font: .headline }
      for t in tags key t { Text(t) }
    }
  }
}
```

項目が多い、または上限がない場合（フィード、受信トレイ、検索結果、写真など）や、スワイプアクション、区切り線、末尾での追加読み込み、グリッドが必要な場合は `List` を使ってください。`List` はそれ自体がスクロールするので、`Scroll` の中には入れないでください。

## 独自の見た目

ここまでの要素は、どれも各プラットフォーム純正のコントロールです。スタイルを付けなければ、アプリはどこでもネイティブな見た目になります。たとえば `Picker` は、iOS ではメニュー、Windows ではドロップダウンです。一方で、アプリに独自のデザインがある場合（ブランドの色、形、動きをすべてのコントロールに、スマートフォンでもブラウザーでも同じように使いたい）や、プラットフォームにないコントロール（コンボボックス、トグルグループ、範囲スライダー）が必要な場合は、`std/base` が見た目を持たず振る舞いだけを持つコンポーネントを提供します。セレクト、メニュー、ダイアログ、チェックボックス、タブ、スライダーなどです。それぞれが状態、キーボードとフォーカス、スクリーンリーダー、そしてウィンドウの上での位置を扱い、表示されるのはアプリがスタイルを付けたものだけです。

```asuka
import std/base

style field {
  padding: [8, 12]
  radius: 8
  border: 1
  borderColor: .separator
  focusVisible { borderColor: .accent; border: 2 }
}

style menu {
  padding: 4
  radius: 10
  background: .background
  shadow: Shadow(radius: 12, y: 4)
}

style option {
  padding: [6, 10]
  radius: 6
}

component FruitPicker {
  state fruit = ""

  base.Select {
    bind value: fruit
    base.SelectTrigger {
      style: field
      base.SelectValue { placeholder: "Pick a fruit" }
    }
    popup {
      base.SelectPopup {
        style: menu
        for f in ["Apple", "Banana", "Cherry"] key f {
          base.SelectItem(f) {
            style: option
            highlighted { background: .accent.opacity(0.15) }
            selected { font: .headline }
            Text(f)
          }
        }
      }
    }
  }
}
```

コンポーネントは部品の組み合わせです。状態を持つルート（`Select`）と、アプリがその中に並べてスタイルを付ける部品（`SelectTrigger`、`SelectValue`、`SelectPopup`、`SelectItem`）があります。どの部品も `style:` とスタイルのプロパティを受け取り、部品の状態は `pressed { … }` と同じく、アプリがスタイルを付けるバリアントになります。`checked { … }`、`open { … }`、`highlighted { … }`、`selected { … }`、`disabled { … }` です。ルートの値は、バインドされていれば（`bind value: fruit`）呼び出し側のもの、そうでなければルート自身のものです。ポップアップの部品はルートの `popup` スロットに入れ、トリガーの隣に表示されます。`backdrop` はダイアログの後ろのウィンドウを暗くします。

| 分類 | コンポーネント |
|---|---|
| アクション | `Button`、`Toggle`、`ToggleGroup`、`Toolbar` |
| 選択 | `Checkbox`（`CheckboxGroup`）、`Switch`、`RadioGroup` と `Radio`、`Select`、`Listbox`、`Combobox` |
| 値 | `Slider`、`NumberField`、`Progress`、`Meter` |
| フォーム | `Field`（ラベル、説明、エラー）、`Input`、`Fieldset`、`Form` |
| 開閉 | `Collapsible`、`Accordion`、`Tabs` |
| ポップアップ | `Dialog`、`AlertDialog`、`Popover`、`Tooltip`、`PreviewCard`、`Menu`、`ContextMenu`、`Menubar` |
| その他 | `Avatar`、`ScrollArea`、`Separator`、トースト（`Toasts`、`ToastViewport`） |

キー操作は、同じ種類のコントロールに対する各プラットフォームの慣習に従います。矢印キーでメニュー、セレクト、ラジオグループの項目を移動し、文字を入力するとその項目へ移ります。Escape でポップアップが閉じ、Android の「戻る」でも閉じます。ポップアップが開くとフォーカスはその中へ移り、閉じるとトリガーに戻ります。すべての部品とそのプロパティ、バリアントは `asuka doc std/base` で確認できます。

プラットフォームのコントロールがロール以上のことをしてくれる場合は、そちらを優先してください。iOS の日付ピッカーのホイール、フィールドの自動入力、共有シートなどです。両者は一つの画面の中で混在できます。

### Overlay

`std/base` のポップアップは `Overlay` でできています。`Overlay` は、`open` のあいだ子をウィンドウの上、アンカーの隣に表示します。

```asuka
component Hint {
  state open = false
  let anchor = Ref()

  Column {
    View {
      ref: anchor
      on press { open = !open }
      Text("What's this?")
    }
    Overlay {
      bind open: open
      anchor: anchor
      side: .bottom
      align: .start
      offset: 4
      on dismiss(reason) { print("closed by {reason}") }
      Text("Overlays are placed next to their anchor") {
        padding: 8
        background: .secondaryBackground
      }
    }
  }
}
```

| キー | 働き |
|---|---|
| `open` | true のあいだ子を表示する。開くときに組み立て、閉じるときに破棄する（`animate { }` の中ではトランジションが再生される） |
| `anchor` | 隣に表示する対象のビュー（`Ref`）。指定しなければウィンドウの中に表示する（ダイアログ、ドロワー、トースト） |
| `side`、`align` | アンカーのどの側に置くか（`.bottom`、`.top`、`.start`、`.end`、またはアンカーに重ねる `.center`）と、その辺に沿った位置（`.start`、`.center`、`.end`。`.stretch` はアンカーと同じ長さ） |
| `offset`、`alignOffset` | アンカーとの間隔と、辺に沿った移動量 |
| `flip` | 収まらないときに反対側へ移す（デフォルトは true） |
| `edgeGap` | ウィンドウの端とセーフエリアから空ける最小の距離（デフォルトは 8）。はみ出さないよう辺に沿って移動し、残りの領域より大きくはならない |
| `matchWidth` | 少なくともアンカーと同じ幅にする |
| `modal` | 押す操作はこのオーバーレイにだけ届き、スクリーンリーダーにもこのオーバーレイだけが見える |
| `dismissOn` | 何で閉じるか：`.outsidePress`（アンカーを押した場合は除く）、`.escape`、`.back`、`.focusOut`、`.anchorHidden`（スクロールで見えなくなった）。デフォルトはすべて |
| `backdrop { … }` | 開いているあいだ、後ろのウィンドウを覆うもの |
| `on place(placed)` | 最終的に置かれた側、その辺に沿ったアンカーの中心の位置（矢印を置く場所）、使える領域 |

オーバーレイはアンカーの移動（スクロール、リサイズ、キーボードの表示）に追従し、領域が足りなくなると反対側へ移ります。この動作はすべてのプラットフォームで同じです。オーバーレイはアンカーと同じウィンドウに置かれ、アンカーがシートの中にあればシートの上に表示されます。

## 独自のビュー

Asuka が対応していないウィジェットがプラットフォームにある場合は、自分で `native component` を書けます。iOS と macOS では Swift、Android では Kotlin、Windows と Linux では C、Web では JavaScript で書きます。[ネイティブコード](native.md) を参照してください。
