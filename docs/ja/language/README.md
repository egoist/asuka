# 言語リファレンス

Asuka のアプリは、Asuka という言語で書きます。各プラットフォーム純正のウィジェットを使う UI のために作られた、小さな言語です。静的型付けで、リリースビルドでは事前にコンパイルされます。開発中はその場でリロードされるので、保存した変更は数ミリ秒で画面に反映され、値もネイティブのビューもすべてそのまま残ります。

```asuka check
component Counter {
  state count = 0

  SafeArea {
    justify: .center; align: .center; gap: 16; padding: 24

    Text("Count: {count}") { font: .largeTitle.bold() }
    Row {
      gap: 12
      Button("Reset") { variant: .gray; on press { count = 0 } }
      Button("Add") { variant: .filled; on press { count += 1 } }
    }
    if count >= 10 {
      Text("That's a lot") { color: .secondaryLabel }
    }
  }
}

app { Counter() }
```

これらのページでは、言語のすべてをトピックごとに説明します。まず全体を見るなら[Asuka のツアー](../language-tour.md)を読んでください。要素そのもの（`Text`、`Button`、`List`、`Stack` など）については、専用のページ[コンポーネント](../components.md)を参照してください。

## ページ一覧

- [ファイル、パッケージ、インポート](packages.md)：ファイル、パッケージとインポート。特定のプラットフォーム向けのファイル。アプリ、プログラム、ライブラリ
- [値と型](types.md)：値と型、演算子、組み込みメソッド、値のセマンティクス
- [宣言](declarations.md)：let、var、state などの宣言。構造体と列挙型。非公開の宣言
- [関数とクロージャ](functions.md)：関数とクロージャ。インターフェースとジェネリクス
- [制御フロー](control-flow.md)：if、match、ループとパターン。特定のプラットフォーム向けのコード（@platform）と開発ビルド向けのコード（@debug）。ビルドが与える値（@define）
- [コンポーネント](components.md)：コンポーネント：本体、ハンドラー、双方向バインディング、ビューのリスト、effect と task
- [ストア](stores.md)：使うコンポーネントの間で共有する状態
- [非同期処理](async.md)：非同期関数、task、ストリーム、spawn。読み込むデータ
- [エラー](errors.md)：投げる、捕まえる、値としてのエラー
- [スタイル](styles.md)：スタイルと、ビューに名前を付けて参照すること
- [アニメーション](animation.md)：animate { }、アニメーション中の値、フレームごとの処理（onFrame）
- [描画](drawing.md)：Canvas、Shape、パス、シェーダー
- [システムの機能](system.md)：アプリがシステムに頼むこと（アラート、ウィンドウ。共有シートと触覚フィードバックはプラットフォーム SDK にあります）と、受け取るイベント（URL、ファイル、終了）
- [ナビゲーション](navigation.md)：画面、タブ、スタック。push、present、pop、dismiss
- [ほかの言語のテキスト](languages.md)：ほかの言語のテキスト
- [ネイティブコード](native.md)：ネイティブインターフェースとネイティブコンポーネント
- [テスト](tests.md)：test ブロック、mount、アプリの操作と確認

## メモリとパフォーマンス

ガベージコレクターがないので、停止もありません。値は参照カウントで管理され、書き込むときにコピーされます。状態、ストア、エフェクト、タスクは、それを作ったコンポーネントに属し、コンポーネントと一緒になくなります。

- 変更があっても、実行されるのは変わったものを読んでいるバインディングだけで、プラットフォームに送られるのも変わったプロパティだけで、送るのはフレームごとに一回です。
- リリースビルドは事前にコンパイルされるので、配布するアプリにインタープリターは含まれません。
- リストは画面に見えている分だけを組み立て、セルを再利用します。

## キーワード

`app as async await bind break catch component continue defer do effect
else emit enum event extend extern false fn for if import in interface
key let load match mut native none on prop provide pub reload return
screen self spawn state store struct style task test throw throws true
try type use var while worker`

`mut`、`extend`、`worker` は 0.2.7 から、`reload` は 0.2.9 からキーワードです。これらを名前に使っていた箇所はエラーになり、名前を変えるよう示されます。

`shader`（[シェーダー](../components.md#シェーダー)の宣言）、`at`（ルート付きの画面のパターン）、`static`（型の関数：`static fn`）、`variant`（コンポーネントのバリアント）、`preview`（[プレビュー](components.md#プレビュー)。トップレベルで、名前の文字列の前）は、その位置でだけキーワードで、ほかの場所では普通の名前です。

`load` のあとに呼び出しが続くときは [load](async.md#読み込むデータ) キーワードです（`load api.user(id)`、または `load (api.user(id))`）。括弧の中に要素が複数あり、少なくとも 1 つが呼び出しのときも同じです（`load (api.user(id), api.posts(id))`、[まとめた load](async.md#いっしょに届く-load)）。括弧の中がそれ以外のとき（`load()`、`load(id)`）は、`load` という名前の関数の呼び出しです。`reload` のあとに名前が続くときは [reload](async.md#変更が触れたものを読み込み直す) キーワードです（`reload api.notes(id)`、`reload api.notes`）。`(` が続くときは、`reload` という名前の関数の呼び出しです。
