# はじめに

アプリは Asuka 言語（[Asuka 言語](language-tour.md)）で書き、各プラットフォーム純正のウィジェットで表示されます。`asuka` CLI は、プラットフォームごとに、そのプラットフォーム自身のツールでアプリをビルドします。コマンド一つでアプリを作成し、もう一つのコマンドで実行できます。実行中は、ファイルを保存するたびにアプリがリロードされます。

## 必要なもの

CLI と、実行したいプラットフォームそれぞれのビルドツールが必要です。

- **iOS**：Apple シリコンを搭載し、Xcode 26 以降をインストールした Mac と、その iOS シミュレーターか iPhone。（Intel Mac での開発には対応していません。）
- **Android**：Mac、Windows 10 または 11 の PC（x64 または Arm）、Linux のいずれかに Android Studio をインストールし、その SDK Manager から NDK（SDK Tools → NDK）を入れます。さらに、Device Manager で作成したエミュレーターか、USB デバッグを有効にした実機を用意します。Asuka は SDK を標準のインストール先か `ANDROID_HOME` から見つけ、Java は Android Studio に同梱のものを使います。ビルドには NDK と Gradle を使います。Arm の PC では、NDK とエミュレーターはエミュレーションで動きます（どちらも x64 向けのため）。エミュレーターには x86_64 のシステムイメージが必要です。
- **macOS**：Apple シリコンを搭載し、Xcode 26 以降か、そのコマンドラインツール（`xcode-select --install`）をインストールした Mac。作ったアプリは macOS 11 以降の Apple シリコン搭載 Mac で動きます。`--arch amd64` でビルドすれば Intel Mac で動きます。
- **Windows**：Windows 10 または 11 の PC（x64 または Arm）と、LLVM の clang（`winget install LLVM.LLVM`）。または [Zig](https://ziglang.org/download) を入れて `ASUKA_CC=zig` を指定します。`WebView` と `Video` は Microsoft Edge WebView2 Runtime を使います。これは Windows 11 には入っており、Windows 10 の PC にもたいてい入っています。
- **Linux**：C コンパイラー（`build-essential`、`gcc`、`clang`）と、GTK 4.14 以降と libadwaita 1.5 以降（Ubuntu 24.04、Fedora 40、Debian 13 以降には含まれています）。Wayland と X11 のどちらでも動きます。

  ```sh
  sudo apt install build-essential libgtk-4-1 libadwaita-1-0   # Debian、Ubuntu
  sudo dnf install gcc gtk4 libadwaita                         # Fedora
  sudo pacman -S base-devel gtk4 libadwaita                    # Arch
  ```

  `WebView` には WebKitGTK 6.0（`libwebkitgtk-6.0-4`）が、`Video` とオーディオには GStreamer が必要です。必要なライブラリが見つからない場合、パッケージはどのライブラリが足りないかをエラーで知らせます。
- **Web**：ブラウザーと、WebAssembly 向けにビルドする [Zig](https://ziglang.org/download)。

Zig をインストールしておけば、`asuka build` で Windows や Linux のアプリをどのシステムからでも作れます。Mac のアプリは Mac からビルドできます。

## CLI のインストール

macOS と Linux では：

```sh
curl -fsSL https://asuka.egoist.dev/install.sh | sh
```

Windows では、PowerShell で：

```powershell
irm https://asuka.egoist.dev/install.ps1 | iex
```

このスクリプトは CLI を GitHub のリリースからダウンロードし、リリースのチェックサムと照合したうえで `~/.asuka/bin` にインストールします（Windows では `%LOCALAPPDATA%\Programs\asuka\bin` にインストールし、PATH に追加します）。続けて、このコンピューター向けのアプリ用にビルド済みの Asuka のライブラリもダウンロードするので、最初のビルドでそれを待つことはありません。`ASUKA_VERSION=0.3.0` を指定すると最新版ではなくそのバージョンを、`ASUKA_INSTALL` を指定すると別のディレクトリにインストールします。

`asuka upgrade` は CLI を最新のリリースに更新します（`asuka upgrade 0.3.0` ならそのバージョンに）。ビルド済みのライブラリも合わせて更新されます。また asuka は 1 日に 1 回、新しいリリースが出ていないかをバックグラウンドで確認し、出ていればコマンドの終わりにそれを知らせます。この確認のためにコマンドが待たされることはありません。`ASUKA_NO_UPDATE_CHECK=1` で無効にでき、CI では動きません。

`asuka doctor` は、このコンピューターで各プラットフォーム向けに何をビルド・実行できるか、残りのプラットフォームに何をインストールすればよいかを表示します。

```
$ asuka doctor
✓ iOS
  ✓ Xcode: Xcode 26.1
  ✓ a simulator: 4 iPhone simulators
✗ Android
  ✗ the Android SDK and NDK: no Android SDK: install Android Studio or set ANDROID_HOME (Android Studio › SDK Manager: SDK Tools › NDK)
...
```

`!` の付いた行は任意の項目です。エミュレーターがなくても Android 向けのビルドはできます。`asuka doctor -p android` は一つのプラットフォームだけを調べ、準備ができていなければ失敗します（CI ジョブの最初のステップに使えます）。`--json` を付けると JSON で出力します。

## アプリの作成

```sh
asuka new hello
cd hello
```

`hello` には次のファイルができます。

- `app.asuka`：アプリ本体です。コンポーネント、それを表示する `app` ブロック、テストが一つ入っています。
- `asuka.toml`：アプリの名前、ID（bundle ID、application ID、デスクトップアプリの ID を兼ねます）、バージョンと、[ストアと各プラットフォーム](shipping.md)が求める情報です。
- `AGENTS.md`：コーディングエージェント向けで、検査、テスト、エラーコードの調べ方が書いてあり、まず `asuka guide` を読むよう求めます。`asuka guide` は、インストールしたバージョンの言語を 1 ページにまとめて表示します。
- `.gitignore`。

`asuka new` が作るプロジェクトは 3 種類です。アプリ（今回のもの）、`--lib` を付けるとほかのアプリがインポートするライブラリ、`--bin` を付けるとウィンドウを持たないコマンドラインツールである[プログラム](programs.md)になります。

## 実行

```sh
asuka dev                      # このコンピューター：Mac、PC、Linux
asuka dev --platform ios        # iOS シミュレーター
asuka dev --platform ios --device iphone  # 接続している iPhone
asuka dev --platform android    # Android のエミュレーターまたは実機
asuka dev --platform web        # ブラウザー
```

`.asuka` ファイルを保存すると、アプリは数ミリ秒でリロードされます。状態はそのまま残ります。アプリの出力とエラーは、リロードごとの報告（`asuka: reloaded in 2.1 ms, 3 states kept`）とともにターミナルに表示されます。

`asuka dev` は、プラットフォームの**開発用アプリ**を一度だけビルドします。これはアプリの外側（アイコン、名前、権限）で、アプリのコードは入っていません。Asuka はこれをインストールして起動し、そこへアプリを送ります。保存するたびにコンパイルし直し、開発用アプリは新しいバージョンをその場で読み込みます。保存した内容がコンパイルできなければ、エラーがターミナルとアプリの上に表示され、アプリはそれまでのものを実行し続けます。アプリが捕捉しなかったエラーも同じようにアプリの上に表示されます（[アプリに重なるエラー表示](development.md#アプリに重なるエラー表示)）。`asuka.toml`、アセット、ネイティブコードを変更すると、開発用アプリがビルドし直されて起動します。

- **コンピューターで実行する場合**（`asuka dev`、または `--platform mac`、`windows`、`linux`）、開発用アプリはウィンドウで動きます。
- **スマートフォンで実行する場合**、`--device` でシミュレーターか iPhone（名前か UDID、接続している 1 台なら `iphone`）、または Android デバイス（adb のシリアル番号か AVD の名前）を選べます。`--once` を付けると、監視せずにアプリを起動します。
- **ブラウザーで実行する場合**、Asuka はアプリを `http://localhost:8790/` で配信し、ブラウザーで開きます。保存するたびに、その内容がページにその場で読み込まれます。`--lan` を付けるとネットワーク上に配信するので、スマートフォンのブラウザーで試せます。詳しくは [Web](web.md) を参照してください。

iPhone の実機で動かすには、iPhone を接続してロックを解除し、「信頼」をタップして、デベロッパモード（「設定」›「プライバシーとセキュリティ」）をオンにします。さらに Xcode に Apple ID を追加します（「設定」›「アカウント」）。そのうえで `asuka dev --platform ios --device iphone` を実行すると、初回は開発用アプリにあなたのチームで署名してインストールします。iPhone はネットワーク経由で `asuka dev` に接続します。[デバイス](development.md#デバイス)を参照してください。

リロードで何が保たれるか、デバイス、ログ、よくある問題など、その先のことは[開発](development.md)で説明しています。

## アプリの変更

`asuka new` が作る `app.asuka` は、カウンターのアプリです。

```asuka
component Counter {
  state count = 0

  SafeArea {
    justify: .center
    align: .center
    gap: 16

    Text("hello: {count}") { font: .title }
    Button("Add") {
      variant: .filled
      on press { count += 1 }
    }
  }
}

app { Counter() }

test "Add counts" {
  let app = mount(Counter())
  app.tap("Add")
  app.expect("hello: 1")
}
```

- **状態**は `state count = 0` です。ビューの中で読むと（`"{count}"`）、そのビューが状態にバインドされます。書き込むと（`count += 1`）、それを読んでいる部分が更新されます。更新されるのはその部分だけです。
- **コンポーネント**は、自身の状態、プロパティ、ビューを宣言します。何かが変わっても本体が再実行されることはなく、変わったものを読んでいるバインディングだけが再実行されます。
- **スタイル**は flexbox で、単位はポイントです。`grow`、`gap`、`padding`、`align`、`justify` と、見た目（`background`、`radius`、`shadow` など）を指定します。スタイルを何も指定しないアプリは、そのプラットフォームのほかのアプリと同じ見た目になります。独自のデザインを持つアプリは、`std/base` を使えばどこでも同じ見た目にできます（[独自の見た目](custom-ui.md)）。
- **テスト**は `test` ブロックです。`asuka test` は、デバイスなしで、スマートフォンと同じようにビューをレイアウトしてテストを実行します（[テスト](testing.md)を参照）。

同じコードが、そのままどのプラットフォームのアプリにもなります。残りは [Asuka 言語](language-tour.md)で説明しています。

## アプリのファイル構成

- `assets/`：画像などのファイル（`Image("photo.jpg")` のように使います）と、`assets/fonts` に置くフォント。
- `locales/`：各言語でのアプリのメッセージ（[多言語対応](i18n.md)を参照）。
- アプリ独自のパッケージ：アプリの `.asuka` ファイルの隣に置いた、`.asuka` ファイルのディレクトリです。`import ./components` のようにインポートします。依存関係は `asuka.toml` の `[dependencies]` に書きます（`asuka add @acme/charts`。[パッケージ](packages.md)を参照）。
- ネイティブパッケージ：`native interface` や `native component` を宣言する Asuka のパッケージで、各プラットフォームがそれぞれのプロジェクト（Swift パッケージ、Gradle モジュール、JavaScript、Windows と Linux では C）で実装します。[ネイティブコード](native.md)を参照してください。

## 次のステップ

- [独自の見た目](custom-ui.md)：`std/base` のコンポーネントで、どのプラットフォームでも同じデザインにする。
- [開発](development.md)：開発ループ、デバイス、ログ。
- [テスト](testing.md)：`test` ブロックと `asuka test`。
- [配布](shipping.md)：App Store、Google Play、Mac、Windows、Linux、Web 向けのビルド。
