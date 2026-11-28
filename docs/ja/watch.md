# Apple Watch

Asuka のアプリは Apple Watch でも、ウォッチ純正のコントロールを使って動きます。スマートフォンでスマートフォンのコントロールを使うのと同じです。ボタン、トグル、スライダー、ステッパー、ピッカー、リストは SwiftUI のもので、ナビゲーションやアラートも watchOS 自身が描き、スクロールするものは Digital Crown でスクロールします。Asuka はほかのプラットフォームと同じようにアプリをレイアウトするので、スマートフォン向けに書いた画面も、手首の上では同じ画面のまま、サイズに合わせて表示されます。

ウォッチでも同じコードが動きます。ウォッチのアプリは iOS アプリのコードからビルドされるので、Apple TV と同じく `@platform == .iOS` です。iOS 向けのファイルや分岐はそのまま使えます。`environment().watch` を見れば、アプリがウォッチで動いていることがわかります。表示するものを少なく、大きくするのに使います。

## 実行する

```sh
asuka dev --platform watchos .                # ウォッチのシミュレーターで実行し、保存のたびに再読み込み
asuka build --platform watchos --simulator .  # シミュレーター用のリリース .app
asuka build --platform watchos .              # ウォッチ単体アプリの .ipa（dist/watchos に出力）
asuka publish --platform watchos .            # ビルドして App Store Connect にアップロード（TestFlight 用）
```

`asuka dev --platform watchos` は、Apple Watch のシミュレーターでアプリを動かします。起動中のものがあればそれを、なければ最新の Apple Watch Series のシミュレーターを使い、一台もなければ作って起動します（`--device` でほかのものを指定できます）。保存するとスマートフォンと同じように、状態を保ったまま再読み込みされます。ウォッチのシミュレーターは Xcode の watchOS プラットフォームと一緒にインストールされます。Xcode › Settings › Components から入れるか、`xcodebuild -downloadPlatform watchOS` を実行してください。シミュレーターでは、マウスのホイールやトラックパッドでのスクロールが Digital Crown を回す操作になります。

`asuka dev` が動くのはウォッチのシミュレーターで、実機のウォッチではありません。再読み込みに必要な、コンピューターへの接続を watchOS がアプリに許さないためです。実機のウォッチで試すには、リリースビルドを TestFlight で入れてください。

## ビルドと公開

`asuka build --platform watchos` は、ウォッチ単体で動くアプリを作ります。ウォッチの App Store からインストールでき、iPhone のアプリがなくても動きます（watchOS 10 以降）。実行ファイルは watchOS 10 が動くすべての Apple Watch、つまり Series 4 以降で動きます。`dist/watchos` の `.ipa` は、App Store Connect がウォッチ単体アプリとして受け付ける形です。ウォッチのアプリを、それを運ぶためだけの小さな iPhone アプリに入れたもので、この iPhone アプリがスマートフォンに表示されることはありません。（iPhone アプリがウォッチのアプリを運ぶ場合は、iPhone アプリと一緒にビルドされます。[iPhone アプリと一緒に](#iphone-アプリと一緒に)を参照してください。）

署名には、チームのプロビジョニングプロファイルが二つ必要です。一つはウォッチのアプリ用の watchOS のプロファイルで、その ID はアプリの ID に `.watchkitapp` を付けたものです（`dev.asuka.counter.watchkitapp` など）。もう一つはアプリの ID そのもの、つまり App Store Connect のレコードの ID 用の iOS のプロファイルです。どちらも developer.apple.com で作ってください（Profiles › +）。デフォルトは App Store 配布用で、`--distribution` でも選べます。ウォッチのアプリのプロファイルは `--profile` か `asuka.toml` の `[watch]` の `profile` で（[asuka.toml](asuka-toml.md#watch)）、iPhone アプリのものは `ios.profile` で選べます。

`asuka publish --platform watchos` は、このビルドを作って App Store Connect にアップロードします。`--platform ios` が iPhone アプリをアップロードするのと同じで（[ストアへの公開](shipping.md#ストアへの公開)）、オプションも同じです。`--groups` で TestFlight のグループに配り、`--notes` で新しい内容を書き、`--review` で App 審査に提出し、`--no-wait` でアップロードが済んだところで終わり、`--file` で前にビルドした `.ipa` をアップロードします。ウォッチ単体アプリは App Store Connect では iOS アプリのレコードです（iPhone アプリと同じように、アプリの ID で作ってください）。ビルド、バージョン、審査はそのレコードの iOS プラットフォームのもので、App Store はウォッチにだけそれを提供します。

アプリのアイコンは `asuka.toml` の `icon` で、ウォッチはこれを円形に表示します。大事なものはアイコンの中央に置いてください。

## アプリのコードのアップデート

App Store のリリースのあいだ、ウォッチのアプリもスマートフォンのアプリと同じように、コードを無線で更新できます（[アプリのコードのアップデート](shipping.md#スマートフォンアプリのコードのアップデート)）。`asuka.toml` の `[updates]` を付けてビルドし、ウォッチのビルド向けにアップデートを公開します。

```sh
asuka build --platform watchos --update      # dist/watchos/updates に、署名したアップデートとそのマニフェストを書き出す
asuka publish --platform watchos --update    # 作って、[updates] の指定先にアップロードする
```

ウォッチのアップデートは独立しています。ウォッチのビルドのフィンガープリントはウォッチ自身のネイティブ部分（そのホストと、watchOS 向けにビルドされたパッケージ）から計算され、ウォッチの `platform/update` はフィードの `update-watchos-<フィンガープリント>.json` を読むので、iPhone とウォッチが互いのアップデートを受け取ることはありません。変更を届けたいプラットフォームごとに公開してください。アップデートはアプリのディレクトリ（Application Support）に保存され、あるアップデートの起動が 2 回続けて最初の数秒で終わると、スマートフォンと同じくビルド自身のコードに戻ります。

## iPhone アプリと一緒に

iPhone アプリとそのウォッチのアプリの両方を持つアプリは、`asuka.toml` にそう書きます。

```toml
[watch]
companion = true
```

すると `asuka build --platform ios` は同じコードからウォッチのアプリもビルドし、iPhone アプリの `Watch` フォルダに入れます。`.ipa` も App Store Connect のレコードも一つで、`asuka publish --platform ios` が両方をアップロードします。iPhone アプリをインストールすると、ペアリングしたウォッチにウォッチのアプリが提案されます。ウォッチのアプリはウォッチの App Store から単独でもインストールでき、iPhone が近くになくても動きます。実機向けのビルドは、iPhone アプリのプロファイルとは別に、`<id>.watchkitapp` の watchOS のプロファイル（`[watch]` の `profile`、または最も合うもの）で署名します。`asuka build --platform watchos --simulator` は、これまでどおりシミュレーター用にウォッチのアプリだけをビルドします。

二つのアプリは `platform/companion`（Apple の WatchConnectivity）でやりとりします。どちら側でも呼び出しは同じで、iPhone では相手がウォッチのアプリ、ウォッチでは相手が iPhone アプリです。

```asuka check
import platform/companion

component Score {
  state score = 0
  state said = ""

  // What the other app sends; a message that waits gets an answer.
  task {
    for await m in companion.messages() {
      said = m.text
      if m.wantsReply { companion.reply(m, text: "{score}") }
    }
  }

  Column {
    Text("Theirs: {companion.context()}")
    Text(said)
    Button("Point") {
      on press {
        score += 1
        // The latest state, for the other app whenever it next runs.
        try? companion.setContext("{score}")
        // A message now, if the other app can answer.
        if companion.reachable() {
          said = (try? await companion.send("point")) ?? ""
        }
      }
    }
  }
}
```

- `companion.status()`（追跡されます）は、相手がいるかどうかを表します。`paired`、`installed`、そして今メッセージに答えられるかどうかの `reachable` です。`available()` と `reachable()` はその短縮形です。
- `send(text)` は相手にメッセージを送り（相手に届く状態である必要があります）、相手の `reply` を返します。`queue(text)` は相手が次に動いたときのためにメッセージを残し、このアプリが終了したあとでも順番どおりに届けます。`messages()` はその両方を読み、最初に読む前に届いていたものも含みます。
- `setContext(text)` は、こちら側の最新の状態を相手のために保存し、相手は `context()`（追跡されます）でそれを読みます。届くのは最新のものだけです。
- `sendFile(path, metadata:)` はファイルをバックグラウンドで送ります。`files()` は届いたファイルを返し、それらはアプリのディレクトリに移されています。

メッセージはテキストです。運ぶ内容は `std/json` でエンコードしてください。相手のいない端末（iPad、Android、デスクトップ）では `available()` は false で、呼び出しは `CompanionError.unavailable` を投げます。テストでは `companion.fake()` で偽の相手を入れられます。シミュレーターで二つを一緒に試すには、ウォッチのシミュレーターと iPhone のシミュレーターをペアリングし（`xcrun simctl pair <ウォッチ> <iPhone>`）、両方を起動して、それぞれのアプリのシミュレーター用ビルドをインストールします。iPhone アプリは iPhone に、そのウォッチアプリ（iPhone アプリの `Watch` フォルダーの中）はウォッチに入れます。シミュレーター同士ではメッセージとコンテキストは届きますが、`queue` のメッセージと `sendFile` のファイルは届きません。この二つは実機のウォッチと iPhone で試してください。

## コンプリケーション

ウォッチのウィジェットはコンプリケーションです。文字盤にも、スマートスタックにも表示されます。`asuka.toml` の `widgets` のうち、ウォッチにあるサイズのもので、`circular`、`rectangular`、`inline`（iPhone ではロック画面のサイズ）と、ウォッチ独自の `corner` です。

```toml
[[widgets]]
kind = "steps"
name = "Steps"
description = "Today's steps"
sizes = ["small", "circular", "rectangular", "corner"]
```

ウォッチのアプリは、iPhone アプリがウィジェットを埋めるのと同じように `platform/widget` で中身を埋めます。ウォッチのアプリのコードで `widget.set("steps", widget.Entry(...))` を呼ぶと、ウォッチの文字盤に表示されます。`corner` は `symbol` と `value`、または `progress` のゲージを、画面の縁に沿って曲げて表示します。アプリ独自のレイアウト（`layout`、`sizes[.corner]`）はどのサイズでも描けます。ウォッチのサイズを持たない種類は iPhone だけのもので、`corner` はウォッチだけのものです。

CLI はウォッチのウィジェット拡張機能（WidgetKit のもので、ID は `<ウォッチのアプリの ID>.widgets`）をウォッチのアプリに組み込み、両者で App Group を共有します。実機向けのビルドはそれを `<id>.watchkitapp.widgets` の watchOS のプロファイル（`[watch]` の `widgetProfile`、または最も合うもの）で署名します。どちらのプロファイルにも App Groups ケイパビリティが必要です。

## ウォッチに表示されるもの

watchOS に対応するコントロールがあれば、それぞれの要素はウォッチ純正のコントロールになります。

| コンポーネント | Apple Watch では |
|---|---|
| `Text`、`RichText` | SwiftUI のテキスト。ウォッチのシステムフォントで、ユーザーの文字サイズに従います。リッチテキストはスタイルとリンクの色を保ちますが、インラインのコンポーネントは表示しません |
| `Button` | ウォッチのボタン。角の丸いカプセル形で、`variant: .filled` ではアクセントカラーで塗られます。独自の `background` を持つ `.plain` のボタンはラベルだけになります。ウォッチのボタンは画面幅から左右の余白を引いた幅より広くならないため、それより広い列に引き伸ばされたボタンは中央に置かれます |
| `Toggle` | ウォッチのスイッチ |
| `Slider` | ウォッチのスライダー。両端にマイナスとプラスがあり、フォーカスがあるときは Digital Crown で動かせます |
| `Stepper` | ウォッチのステッパー。値がマイナスとプラスのあいだに表示されます |
| `TextField` | タップするとウォッチの文字入力（音声入力、スクリブル、キーボード）が開きます |
| `Picker`、`Wheel`、`Segmented` | Digital Crown で回すホイール |
| `DatePicker` | ウォッチの日付と時刻のピッカー |
| `Image`、`Symbol` | 画像、SF Symbol |
| `Progress`、`ProgressRing`、`Activity` | バー、リング、インジケーター |
| `Scroll`、`List` | Digital Crown でスクロールするスクロールビュー。`List` の行はウォッチのプラッターに載ります（`appearance: .bare` ではアプリが描くとおり） |
| `Stack` | ウォッチのナビゲーション。画面のタイトルが上にあり、戻るボタンと端からのスワイプで戻れます |
| `Tabs` | Digital Crown で上から下へめくるページ |
| `Sheet`、`Popover` | 画面全体を覆うシートで、閉じるボタンかスワイプで閉じます |
| アラート、アクションシート | ウォッチのアラートと確認ダイアログ |
| 画面のバーボタン | 最初の一つが、上部の末尾側の角に表示されます |

大きな画面やポインター、もう一方の手を必要とするものは表示されません。`WebView`、`Video`、`Canvas`、`Shader`、`ColorPicker` は場所だけ取って何も描かず、メニュー、スワイプアクション、引っ張って更新、ドラッグ＆ドロップ、テキストの選択、ピンチ、回転、ツールチップ、キー、キーボードのフォーカスは起こりません。

## 手首に合わせる

Apple Watch では `environment().watch` が true になります。ウォッチはちらっと見るものです。大事な一つのことを大きく見せ、残りはスマートフォンに任せましょう。セーフエリアは、スマートフォンでノッチを避けるのと同じように、ウォッチの丸い角と時刻の表示を避けてくれます。

```asuka check
component Today(steps: int, goal: int) {
  let watch = environment().watch

  SafeArea {
    gap: if watch { 4 } else { 12 }
    padding: if watch { 4 } else { 16 }
    Text("{steps}") { font: if watch { .title.bold() } else { .largeTitle.bold() } }
    Text("of {goal} steps") { color: .secondaryLabel }
    if !watch {
      Text("Walk a little more each day: the goal follows you.") { font: .footnote }
    }
  }
}
```

## Apple Watch のパッケージ

標準ライブラリのパッケージはウォッチでも動きます。ネイティブコードを持つものも、呼び出す機能が watchOS にあれば動きます。`std/http`（URLSession）、`std/sqlite`、`std/store` です。`std/websocket` は動きません。watchOS は音声のストリーミング以外で、アプリが WebSocket などの直接の接続を使うことを許さないため、ウォッチでは `unsupported` を投げます。

プラットフォーム SDK のうち、ウォッチにそのサービスがあるパッケージはウォッチ向けにビルドされます。`platform/device`（端末の情報、手首への触覚、`openURL`）、`secure`、`biometric`、`health`、`location`、`audio`、`speech`、`nowplaying`、`network`、`purchase`、`appleauth`、`notify`、`push`、`crash`、`widget`（コンプリケーション）、`companion` です。それぞれが iPhone とどう違うかは [Apple Watch の場合](device.md#apple-watch-の場合)にあります。ほかのパッケージはウォッチでは、その機能を持たないほかのプラットフォームと同じく `unsupported` を投げます。`environment().watch` を確認して、ウォッチでは使わないようにしてください。

## watchOS のネイティブパッケージ

ネイティブパッケージの Swift は、`Package.swift` のプラットフォームに watchOS が挙がっているとき（または一つも挙がっていないとき）に watchOS 向けにビルドされます。

```swift
platforms: [.iOS("16.0"), .watchOS("10.0"), .macOS("11.0")],
```

watchOS を挙げていないパッケージはウォッチのビルドから外れ、そのモジュールはウォッチでは `unsupported` を投げます（[ネイティブコード](native.md#ios-と-macosswift)）。ウォッチが iOS と違うところでは、パッケージの Swift で `#if os(watchOS)` を使って分けます。ウォッチには UIKit のビューがないので、パッケージのビュー（`//asuka:view`）はそこでは SwiftUI のビュー（`AnyView`）を返し、色も SwiftUI のものになります。ウォッチのアプリに必要なエンタイトルメントは、`Package.swift` の隣の `watchOS.entitlements` に書きます。
