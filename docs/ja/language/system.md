# システムの機能

アプリの一部として働くためにコンポーネントが使うものは、プレリュードにあり、インポートは不要です。コンポーネントがある間に登録するもの、環境、アラート、アプリのウィンドウです。

| 関数 | 動作 |
|---|---|
| `useKeys([Shortcut(…)])` | コンポーネントがある間の、ウィンドウのキー操作 |
| `onOpenURL(u => …)`、`onOpenFiles(paths => …)`、`onShare(items => …)` | コンポーネントがある間に、アプリが開かれたときに渡されたもの、またはほかのアプリから共有されたもの |
| `quit()`、`relaunch()` | ユーザーが「終了」を選んだときと同じようにアプリを終了し（再び起動し）ます。先にアプリに確認します。デスクトップのみ |
| `onCloseRequest(fn() -> bool)`、`onQuit(e => …)`、`onSecondInstance((args, dir) => …)` | コンポーネントがあるあいだ：そのウィンドウが閉じる前の確認、アプリの終了の直前（`e.cancel()`）、アプリの再起動を受け取ります |
| `onBack { … }` | コンポーネントがある画面、シート、ポップオーバーからユーザーが戻ろうとしたときに問い合わせられます。`true` なら引き受け（画面は残る）、`false` なら戻らせます |
| `onLifecycle(l => …)` | コンポーネントがある間、アプリがフォアグラウンドやバックグラウンドに移ったこと、メモリ警告（`Lifecycle`）を受け取ります |
| `alert(title, message:, actions:)` | その場でアラートを表示します。`actions` は `.button` の配列です（なければ OK だけ）。`Alert` 要素は、`open` が true の間アラートを表示します |
| `snackbar(message, action:, duration:, onDismiss:)` | ウィンドウの下部に短いメッセージを、アクション（元に戻す）付きで表示します。閉じるためのクロージャーを返します |
| `environment()`、`screenEnvironment()` | サイズ、セーフエリア、文字サイズ、プラットフォームなど（追跡されます）。画面やシート自身のものと、画面全体のもの |
| `window.main()`、`window.current()` | メインウィンドウの `Ref` と、コードがあるウィンドウの `Ref`。どこからでも `show()`、`hide()`、`focus()` などができます |
| `window.close()` | コードがあるウィンドウを確認せずに閉じます |
| `undo.register(name, undo:, redo:)`、`undo.group(name, body)` | アプリが行った変更を記録します。「編集」>「取り消す」、⌘Z や Ctrl+Z、スマートフォンのシェイクで取り消せます。`undo.undo()`、`undo.redo()`、`canUndo()`、`canRedo()`、`undoName()`、`redoName()`（追跡されます）、`clear()`（[取り消しとやり直し](../desktop.md#取り消しとやり直し)） |

システムのサービスに頼むことは[プラットフォーム SDK](../stdlib.md) のパッケージにあり、ほかのパッケージと同じようにインポートして使います。

| 関数 | 動作 |
|---|---|
| `device.share(text)` | テキストやリンクの共有シートを表示する |
| `try await device.shareFiles(paths, title:, from: ref)` | ファイルの共有シートを、あるビューから（ポップオーバーが指す位置に）表示する。シートが閉じると戻る |
| `device.haptic(.success)` | プラットフォームの触覚フィードバック（`device.Haptic`） |
| `device.copy(text)` | テキストをクリップボードに入れる |
| `device.openURL(url)` | url を扱えるアプリで開く |
| `accessibility.announce(text)` | スクリーンリーダーにテキストを読み上げさせる |
| `accessibility.focus(ref)` | スクリーンリーダーのフォーカスをそのビューに移す |
| `try await webview.cookies(url)` | Web ビューが url について保持している Cookie |
| `appearance.setColorScheme(.dark)` | アプリの外観：`.light`、`.dark`、`.system`（プラットフォームのもの） |

```asuka check
import platform/device

component ShareLink(link: string) {
  Button("Share") {
    on press {
      device.haptic(.light)
      device.share(link)
    }
  }
}
```

アプリが各プラットフォームでどのコードを持つかは、コンパイル時に決まる定数 `@platform` で決めます（[特定のプラットフォーム向けのコード](control-flow.md#特定のプラットフォーム向けのコード)）。`environment().platform` はビューが実際に動いているプラットフォームで、環境のほかの値と同じように追跡されます。

`useKeys` とこれらの `on…` による登録は、コンポーネントの本体（またはアプリのブロック）に書きます。アプリのブロックでは、これらをイベントとしても扱えます：`on openURL(url) { … }`、`on files(paths) { … }`、`on share(items) { … }`、`on quit(e) { … }`、`on secondInstance(args, dir) { … }`、`on foreground { … }`、`on background { … }`。ブロックの最初の `Window` がメインウィンドウです。`Window` の外に何も表示しない（トレイアイコン、メニューバーだけの）app ブロックには、メインウィンドウがありません（[ナビゲーション](../navigation.md#ウィンドウ)）。

`std/embed` は、アプリのコンパイル時にパッケージのファイルを読んでアプリに組み込みます。どのプラットフォームでも、実行時に読み込むことなくそのファイルを使えます。

```asuka
import std/embed

struct Config { api: string; retries: int }

let welcome = embed.text("welcome.md")         // string
let logo = embed.bytes("art/logo.png")         // bytes
let config: Config = embed.json("config.json") // コンパイル時に検査される
let posts = embed.texts("posts/*.md")          // [パス: テキスト]
let icons = embed.files("icons/**/*.svg")      // [パス: バイト列]
```

パスは文字列リテラルで、パッケージのディレクトリ（`asuka.toml` のある場所）からの相対パスです。その外は指せません。グロブには `*`、`?`、`[abc]`、`**`（任意の階層のディレクトリ）が使え、名前がドットで始まるファイルには一致しません。`embed.json` は `json.decode` と同じようにファイルを読みますが、それをコンパイル時に行います。型に合わないファイルは、行と列を示すコンパイルエラーになり、実行時のエラーにはなりません。ファイルがない場合や、グロブに一致するファイルがない場合もコンパイルエラーです。`asuka dev` では、組み込んだファイルを変更するとコードを変更したときと同じようにアプリがその場で再読み込みされます。実行時にプラットフォームが読むファイル（画像、フォント）は `assets/` に置きます（[std/embed](../std/embed.md)）。
