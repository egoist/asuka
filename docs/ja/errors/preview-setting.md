# preview-setting

プレビューが持たない設定、またはそのまま書かれていない値。

コンパイラーが報告するエラーです。直すまでプログラムはビルドできません。

## 理由

プレビューの設定は `asuka dev` がそれをどう表示するかを決めます。`dark`（bool）、`fontScale`（文字サイズ。標準の倍率で 0.5 から 4）、`lang`（言語タグ）、`rtl`（bool）、そして `width` と `height`（枠の大きさ。単位はポイント）です。プレーヤーはプレビューのコードが動く前にこれらを適用するので、どの値もプログラムが計算するものではなく、そのまま書いた値にします。プレビューは名前で一覧に出るので、名前は補間を含まない文字列です。

## 直し方

プレビューが持つ設定を書き、値をリテラルで書きます：`(dark: true, fontScale: 1.5, lang: "ar")`。

## 誤った例

```asuka
component Card {
  Text("Hi")
}

let big = 1.5

preview "Large text" (fontScale: big, theme: .dark) {
  Card()
}
```

## 正しい例

```asuka
component Card {
  Text("Hi")
}

preview "Large text, dark" (fontScale: 1.5, dark: true) {
  Card()
}
```
