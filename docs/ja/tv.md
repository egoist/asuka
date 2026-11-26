# テレビ

Asuka のアプリは Apple TV と Android TV（Google TV も含む）で、テレビ純正のウィジェットを使って動きます。スマートフォンでスマートフォンのウィジェットを使うのと同じです。テレビにはタッチがありません。リモコンでフォーカスをビューからビューへ動かし、中央のボタンでフォーカスのあるビューを押します。これを行うのはプラットフォーム自身のフォーカスエンジンで、アプリのビューの上で動きます。tvOS では UIKit の、Android のテレビでは Android のフォーカスエンジンです。

テレビでも同じコードが動きます。Apple TV のアプリは iOS アプリのコードからビルドされるので、iPad と同じく `@platform == .iOS` です。Android TV で動くのは Android アプリそのもので、スマートフォンとテレビで一つのビルドを共有します。`environment().tv` を見れば、アプリがテレビで動いていることがわかります。部屋の向こうから見る距離に合わせて、画面を大きく、シンプルにするのに使います。

## Apple TV

```sh
asuka dev --platform tvos .                # Apple TV シミュレーターで実行し、保存のたびに再読み込み
asuka build --platform tvos --simulator .  # シミュレーター用のリリース .app
asuka build --platform tvos .              # Apple TV 実機用の .ipa（dist/tvos に出力）
```

`asuka dev --platform tvos` は、Apple TV シミュレーターでアプリを動かします。起動中のものがあればそれを、なければ Apple TV 4K のシミュレーターを起動して使います（`--device` でほかのものを指定できます）。tvOS シミュレーターは Xcode の tvOS プラットフォームと一緒にインストールされます。Xcode › Settings › Components から入れるか、`xcodebuild -downloadPlatform tvOS` を実行してください。シミュレーターでは、矢印キーがリモコンの方向ボタン、Return が中央のボタン、Escape が Menu（戻る）ボタンになります。Window › Show Apple TV Remote でリモコンそのものも表示できます。

`asuka build --platform tvos` は、iOS のビルドが iOS のプロビジョニングプロファイルで署名するのと同じように、アプリの ID に対応する tvOS のプロビジョニングプロファイルで署名します（`--profile` で選べます。デフォルトは App Store 用で、`--distribution` でも選べます）。`.ipa` は Transporter で App Store Connect にアップロードし、同じアプリレコード（または別のレコード）の Apple TV アプリにします。tvOS 17 以降で動きます。

Apple TV でのアプリのアイコンは 5:3 のレイヤー画像です。Asuka は `asuka.toml` の `icon` からこれを作ります。アイコンを、その隅の色で塗った背景の中央に置き、二つのレイヤーに分けるので、フォーカスがアイコンに来ると tvOS がそれらを傾けて視差効果を出します。トップシェルフの画像（アプリのアイコンがホーム画面の最上段にあるとき、その上に表示される画像）も同じように作ります。`[tv]` の `banner` で両方に自分の画像を使え、`background` でアイコンの後ろの色を指定できます（[asuka.toml](asuka-toml.md#tv)）。

tvOS にないものは、tvOS のビルドにも含まれません。共有先（share target）、ウィジェットとライブアクティビティ、App Shortcuts、プッシュ通知の書き換え、それに Web ビュー（tvOS には WebKit がありません）、通知、写真ライブラリ、連絡先、カレンダー、メール作成画面です。これらを呼ぶパッケージは、それを持たないほかのプラットフォームと同じく、tvOS では `unsupported` を投げます。`environment().tv` を確認して、テレビでは使わないようにしてください。

ネイティブコードを持つ標準ライブラリのパッケージ（`std/http`、`std/websocket`、`std/sqlite`、`std/store`、`std/svg`）は、Apple TV でも iPhone と同じように動きます。プラットフォーム SDK のパッケージ（`platform/...`）は今のところ iOS と Mac 向けにだけビルドされるので、Apple TV ではどれも `unsupported` を投げます。

## Android TV

Android アプリがテレビ用でもあると宣言すると、Android TV で動きます。

```toml
[tv]
android = true
```

するとビルドに、Android TV と Google Play が求めるものが入ります。テレビのランチャーの intent（`LEANBACK_LAUNCHER`）、タッチスクリーンと Leanback ライブラリが必須ではないという宣言（`uses-feature … required="false"`。これで Google Play が一つのアプリをスマートフォンにもテレビにも配信します）、そしてテレビのランチャーに表示されるバナー（320 × 180、`icon` または `tv.banner` から作成）です。

試すには、Android Studio の Device Manager で Android TV のエミュレーター（テレビのデバイスと、Android TV か Google TV のシステムイメージ）を作り、`asuka dev --platform android --device <エミュレーターの名前> .` を実行します。エミュレーターでは、矢印キーが D-pad、Return が中央のボタン、Escape が戻るボタンです。

## リモコン

どちらのテレビでも共通です。

- **フォーカス**は、フォーカスを受け取るビューのあいだを移動します。コントロールとテキストフィールド、そして押せる（`on press`）ビュー、調整できる（`on adjust`）ビュー、`focusable` なビューです。プラットフォームが、自分のアプリと同じように、リモコンで指した方向にあるビューを選びます。
- **フォーカスのあるビューは、それがわかるように表示されます**。tvOS のコントロールは自前のフォーカス効果で、アプリのビューは tvOS のカードと同じように少し大きくなり影を落とします。Android TV では、ビュー自身のフォーカス状態（リップルやボタンのもの）に加えて、同じ浮き上がりの効果が付きます。`focusVisible` にスタイルを付けたビューは代わりに自分のスタイルで表示し、`hideFocusRing: true` で効果をなくせます（[アクセシビリティ](accessibility.md#フォーカスを受け取るビュー)）。
- **選択ボタン**（中央のボタン、または Siri Remote のクリックパッドのクリック）は、フォーカスのあるビューを押します。`on press`、ボタンのアクション、トグルの切り替えが起こります。押しているあいだ、ビューは `pressed` 状態になります。
- **戻るボタン**（Android TV の戻るボタン、Siri Remote の戻るボタンまたは Menu ボタン）は、スマートフォンの「戻る」と同じように動きます。シートを閉じ、スタックから画面を戻し、`onBack` で横取りできます（[ナビゲーション](navigation.md#戻るを自分で処理する)）。アプリの最初の画面では、アプリを離れてテレビのホーム画面に戻ります。
- **キー**：ビューの `keys` は、フォーカスが動くより先にリモコンの方向（`up`、`down`、`left`、`right`）を受け取ります。また、リモコンの再生／一時停止ボタンである `playpause` も受け取れます。プレーヤーの再生と一時停止や、ゲームの操作に使います（[キー](motion.md#キー)）。

```asuka check
component Player(title: string) {
  state playing = false

  View {
    padding: if environment().tv { 48 } else { 16 }
    gap: 16
    keys: [Shortcut(key: "playpause", run: () => { playing = !playing })]
    Text(title) { font: .title }
    Button(if playing { "Pause" } else { "Play" }) {
      variant: .filled
      on press { playing = !playing }
    }
  }
}
```

## テレビのコントロール

テレビにあるコントロールは、テレビ純正のものです。テレビにないものは次のとおりです。

| コンポーネント | Apple TV | Android TV |
|---|---|---|
| `Toggle` | チェックマークの付いたボタン（オフのときは丸）で、選択ボタンで切り替わります。tvOS にはスイッチがありません | Material のスイッチ |
| `Slider` | バーで、フォーカスがあるあいだ左右のボタンで範囲の 20 分の 1 ずつ動きます。tvOS にはスライダーがありません | Material のスライダー。左右のボタンで動きます |
| `Stepper` | マイナスとプラスで、フォーカスがあるあいだ左右のボタンで値が変わります | Material のボタン |
| `DatePicker` | 日付（または時刻）を表示し、左右のボタンで 1 日（1 分）ずつ動きます。tvOS には日付ピッカーがありません | Material の日付と時刻のピッカー |
| `Picker`、`Wheel` | メニューで選ぶボタン | スマートフォンと同じ |
| `ColorPicker` | 色を表示し、変えられるのはアプリだけです | スマートフォンと同じ |
| `TextField` | tvOS の全画面キーボードで入力するフィールド。複数行のものも 1 行になります | スマートフォンと同じで、テレビのキーボードを使います |
| `WebView` | なし（tvOS には WebKit がありません） | Android の `WebView` |
| `Sheet` | 画面全体に表示され、ディテントはありません | スマートフォンと同じ |
| `Popover` | シートとして表示 | スマートフォンと同じ |
| 画面の `toolbar` | ボタンがナビゲーションバーの先頭に並びます | スマートフォンと同じ |
| 画面の `search` | 検索フィールドがナビゲーションバーのタイトルになります | スマートフォンと同じ |

スワイプアクション、引っ張って更新、ドラッグ＆ドロップ、テキストの選択、ピンチ、回転、ツールチップ、触覚フィードバックには、タッチやポインター、あるいはデバイスを手に持つことが必要です。テレビでは起こりません。

## テレビに合わせる

Apple TV と Android TV では、`environment().tv` が true です。テレビは部屋の向こうから見るもので、タッチもありません。そのためテレビの画面では、少なく大きな要素を、フォーカスが移動していく行やグリッドに並べ、文字入力を求める部分は省くのが一般的です。

```asuka check
component Shelf(items: [string]) {
  let tv = environment().tv

  Scroll {
    horizontal: true
    Row {
      gap: if tv { 48 } else { 12 }
      padding: if tv { 64 } else { 16 }
      for item in items key item {
        View {
          padding: if tv { 32 } else { 12 }
          radius: 12
          background: .secondaryBackground
          on press { print(item) }
          Text(item) { font: if tv { .title2 } else { .body } }
        }
      }
    }
  }
}
```

セーフエリアは、テレビの端（端を切り落とすテレビもあります）からアプリを離します。tvOS は `environment()` のインセットでそれを伝えます。

## tvOS のネイティブパッケージ

ネイティブパッケージの Swift は、`Package.swift` のプラットフォームに tvOS が含まれていれば（あるいはプラットフォームを一つも挙げていなければ）、tvOS 向けにビルドされます。

```swift
platforms: [.iOS("16.0"), .tvOS("17.0"), .macOS("11.0")],
```

tvOS を挙げていないパッケージは tvOS のビルドから外れ、そのモジュールは tvOS では `unsupported` を投げます。パッケージにコードがまったくないプラットフォームと同じです（[ネイティブコード](native.md#ios-と-macosswift)）。tvOS の UIKit が iOS と違うところでは、パッケージの Swift で `#if os(tvOS)` を使って分けます。テレビのアプリに必要なエンタイトルメントは、`Package.swift` の隣の `tvOS.entitlements` に書きます。
