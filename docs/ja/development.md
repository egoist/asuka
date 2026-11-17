# 開発

`asuka dev` はアプリを実行し、ファイルを保存するたびにリロードします。実行先は、iOS シミュレーターか iPhone、Android のエミュレーターか実機、手元の Mac・PC・Linux マシン、ブラウザーのいずれかです。このページでは、`asuka dev` が動かす開発用アプリ、リロードで何が保たれるか、デバイス、アセット、ログ、エディター、よくある問題について説明します。デバイスやディスプレイなしでアプリの UI をテストするには、[テスト](testing.md)を参照してください。

## asuka dev

```sh
asuka dev [flags] [dir]                     # このコンピューター：macOS、Windows、Linux
asuka dev --platform ios [flags] [dir]       # iOS シミュレーター
asuka dev --platform ios --device iphone [dir]  # 接続している iPhone（または iPad）
asuka dev --platform android [flags] [dir]   # Android のエミュレーターまたは実機
asuka dev --platform web [flags] [dir]       # ブラウザー
```

`--platform` には `ios`、`android`、`mac`、`windows`、`linux`、`web` のいずれかを指定します。`asuka build` でも同じように使えます。省略すると、今使っているコンピューター向けにアプリを実行（またはビルド）します。一回の実行で扱えるプラットフォームは一つだけです。二つのプラットフォームで動かすなら、ターミナルを二つ開いてください。

`dir` はアプリのディレクトリで、デフォルトはカレントディレクトリです。ここにはアプリの `.asuka` ファイル（そのうち一つに `app` ブロックがあります）と `asuka.toml` を置きます。フラグは次のとおりです。

- `--platform`：上で説明したとおりです。
- `--lang`：この実行でだけ表示する言語（`de`、`zh-Hans`）か、疑似ロケールを指定します。疑似ロケールは `en-XA`（アクセント記号付きで長めのテキスト）と `ar-XB`（右から左）です。`--dir rtl` を付けると、どのアプリでも右から左にレイアウトします。[多言語対応](i18n.md)を参照してください。
- `--once`：アプリを一度だけ起動し、ファイルの監視はしません。
- `-v`：Asuka が実行するコマンドを表示します。

スマートフォンでは、次のフラグも使えます。

- `--device`：iOS ではシミュレーターの名前か UDID（デフォルトは起動中の iPhone で、なければ最近の iPhone Pro を起動して使います）、接続している iPhone か iPad の名前か UDID、または接続している 1 台を表す `iphone` を指定します（[デバイス](#デバイス)を参照）。Android では adb のシリアル番号か AVD の名前を指定します（デフォルトは adb が最初に挙げるデバイスで、なければ最初の AVD を起動して使います）。
- `--team`：iPhone で実行するときに、開発用アプリに署名する Apple Developer のチームを指定します。

ブラウザーでは `--port`、`--lan`（スマートフォンのブラウザーから使えるよう、ネットワーク上に配信します）、`--no-open` が使えます。[Web](web.md#asuka-dev---platform-web) を参照してください。

## 開発用アプリ

開発中、アプリは**開発用アプリ**の中で動きます。これはアプリの外側（名前、ID、アイコン、`Info.plist` またはマニフェスト、ネイティブパッケージのコード）で、`asuka dev` がプラットフォームごとに一度だけビルドし、インストールして起動します。そこへ `asuka dev` があなたのアプリを送ります。保存するたびにコンパイルし直し（数ミリ秒です）、新しいバージョンを送ります。開発用アプリはそれをその場で読み込み、アプリの状態を保ったまま、結果を報告します。

```
asuka: compiled in 3.1 ms
asuka: reloaded in 2.2 ms, 4 states kept
```

保存した内容がコンパイルできない場合は、エラーが `file:line:col: message` の形で表示され、開発用アプリはそれまでのバージョンを実行し続けます。次にコンパイルできたときに、そこから続きます。アプリのそれ以外の部分（`asuka.toml`、アセット、ネイティブパッケージの Swift・Kotlin・JavaScript）が変わると、開発用アプリをビルドし直し、変わっていれば再インストールしてから起動します。

## デスクトップでの実行

macOS、Windows、Linux では、`asuka dev` は開発用アプリをウィンドウで実行します。初回の実行では、このコンピューター向けに開発用アプリをビルドするので少し時間がかかります。2 回目以降はすぐに起動します。ビルドには C コンパイラーが必要です。Mac では Xcode のコマンドラインツール、Windows と Linux では Zig、clang、gcc のいずれか（`ASUKA_CC` で選べます）です。

アプリは `assets/` をその場所から直接読むので、アセットを変更してもコピーは必要ありません。アプリのデータ（SQLite データベース、ファイル）は、インストールしたアプリのデータとは別に、Asuka のキャッシュ内にあるアプリ専用の `data` ディレクトリに保存されます（[最初からやり直す](#よくある問題)を参照）。そのため、`asuka dev` とリリース版がデータを共有することはありません。アプリの出力とエラーはターミナルに表示されます。

**macOS。** 開発用アプリは、リリース版と同じ形の `.app` として Asuka のキャッシュ内に置かれます。この `.app` には、`Info.plist`（`asuka.toml` の名前、bundle ID、URL スキーム、利用目的の説明文、言語）とアイコンが含まれ、アドホック署名されます。そのため、Finder から開いたときと同じく、システムからはそのアプリ自身として扱われます。Dock にはアプリの名前とアイコンが表示され、通知もアプリ自身のものになり、プライバシーに関する許可の確認（カメラ、位置情報、連絡先）もターミナルではなくアプリに対して、アプリの利用目的の説明文とともに表示されます。プロビジョニングプロファイルのエンタイトルメントが必要なパッケージ（プッシュ通知、Sign in with Apple、App Attest）は、そのプロファイルで署名したリリースビルドでしか動きません。[配布](shipping.md#macos)を参照してください。

**Windows。** `asuka dev` は Windows 上でしか動きません。一方、`asuka build --platform windows` はどのシステムからでも（Zig で）ビルドでき、アプリを含む 1 つの `.exe` ができます。開発用アプリはコンソールプログラムなので、出力はターミナルに表示されます。リリース版にはコンソールがありません。

**Linux。** Linux では、`asuka dev` は GTK 4 と libadwaita の上で開発用アプリを実行します。これらがインストールされている必要があります（[はじめに](getting-started.md#必要なもの)を参照）。アプリは、セッションのディスプレイ（Wayland または X11）で動きます。Mac や PC で `asuka dev --platform linux` を実行できるのは、`ASUKA_LINUX_RUN` に、ディスプレイのある Linux マシンでプログラムを実行するコマンドを指定した場合だけです。そのマシンからは、このコンピューターのファイルが同じパスで見える必要があります。たとえば VM で、`ASUKA_LINUX_RUN="orb -m ubuntu"`（OrbStack）や `ASUKA_LINUX_RUN="limactl shell default"`（Lima）のように指定します。Asuka はそのマシンのアーキテクチャー向けに開発用アプリをビルドし（Zig が必要です）、開発用アプリはネットワーク経由で `asuka dev` に接続します。接続先は、このコンピューターの最初のアドレスか、`ASUKA_DEV_HOST` に指定したアドレスです。

## リロードで保たれるもの

どのプラットフォームでも、リロードではアプリのすべての `state` が保たれます。そのためのコードを書く必要はありません。状態の各セルは、そのコンポーネントのビューツリー上の位置と、セルの名前で保存されます（`Inbox.query` なら、それを表示している画面と行の下）。新しいモジュールで同じ位置、同じ名前にあるセルは、その値から始まります。値はフィールド名とケース名をもとに、新しいモジュールの型に変換されます。追加したフィールドはデフォルト値から始まり、削除したフィールドは捨てられます。変換できない形で型が変わった状態（`int` が `string` になったなど）は、初期値からやり直しになります。ビューは新しいモジュールから作り直され、それまでの状態を表示します。画面は変更を反映しつつ、前と同じように見えます。

次のものは保たれません。

- 初期値を書き換えた状態は、新しい初期値から始まります（`state count = 0` を `state count = 10` にした場合など）。ほかの状態はそれぞれの値を保ちます。初期値の書き方（空白や改行）を変えただけなら、書き換えたことにはなりません。
- 名前を変えた状態や、別のコンポーネントに移した状態は、最初からやり直しになります。コンポーネント自体をビューツリーの別の位置に移した場合も同じです。
- `let` の値は保たれません。読んでいるものから計算し直されます。
- タスクとエフェクトは、新しいモジュールで始め直されます（保存した時点で実行中の `task` はキャンセルされ、新しいものが実行されます）。

アプリを最初からやり直すには、`asuka dev` を実行しているターミナルで **r** を押します。どの開発用アプリも状態を何も保たずにアプリを読み込み直し、すべての `state` が起動したときと同じく初期値から始まります。開発用アプリ自体は起動し直しません（`asuka: restarted in 2.0 ms`）。`asuka dev` を止めて実行し直しても、アプリは何もない状態から始まります。どちらの場合も、アプリが書き込んだファイル（SQLite データベース、ファイル）は残ります。これらはアプリのデータディレクトリ、つまりデバイス上か、デスクトップなら Asuka のキャッシュ内にあります。

## リロード

Asuka は、アプリのディレクトリとそのローカルの依存関係（`asuka.toml` の `file:`）のディレクトリを監視し、次の変更を検知します。

- `.asuka` ファイル：コンパイルして、その場でリロードします。
- `asuka.toml` と `asuka.lock`、`assets/` 以下のファイル、言語のカタログ、ネイティブパッケージの Swift・Kotlin・JavaScript、`Info.plist` とエンタイトルメント：開発用アプリをビルドし直して起動します（アプリは何もない状態から始まります）。

`.` で始まるディレクトリと、`testdata`、`build`、`dist`、`node_modules` は監視の対象外です。

## デバイス

**手元のコンピューター**：Mac、PC、Linux で、`asuka dev` を何も指定せずに実行します。

**iOS シミュレーター**は Mac とネットワークを共有するので、特に設定はいりません。

**Android のエミュレーターや実機**：`asuka dev` は、エミュレーターにも USB で接続した実機にも adb 経由で接続します。実機の場合は、開発者向けオプションで USB デバッグを有効にし、ケーブルで接続して確認ダイアログで許可したら、`adb devices` に表示されることを確かめます。`asuka dev --platform android` は、実機かエミュレーターかを問わず、adb が最初に挙げるデバイスを使います。複数ある場合は、`--device` にシリアル番号を指定して選んでください。

**iPhone**（または iPad）では Xcode 26 以降が必要です。iPhone をケーブルで接続してロックを解除し、「信頼」をタップします。次に「設定」›「プライバシーとセキュリティ」で「デベロッパモード」をオンにします（iPhone が一度 Xcode に接続されると表示されます。オンにすると iPhone が再起動します）。あとは `asuka dev --platform ios --device iphone` で実行できます。複数台つないでいる場合は、`--device` に名前か UDID を指定します（一覧は `xcrun devicectl list devices` で確認できます）。

- **署名**：開発用アプリは、あなたのチームの開発用証明書で署名されます。チームは `--team`、`ASUKA_TEAM_ID`、`asuka.toml` の `ios.team` の順に探し、どれもなければキーチェーンにある Apple Development 証明書のチームを使います。初回の実行では、必要な開発用プロビジョニングプロファイルを Xcode に作らせます。このとき iPhone がチームに登録され、アプリが使うケイパビリティ（プッシュ通知、App Groups など）が有効になります。使われるのは Xcode の「設定」›「アカウント」の Apple ID か、`ASUKA_ASC_KEY_ID`（または `ios.appStoreConnect`）で指定した [App Store Connect API キー](shipping.md)です。チームの既存のプロファイルが合うなら（ワイルドカードのものも含め）、そのまま使います。
- **Mac への接続**：ケーブルでつないでいれば、Xcode と同じように、Mac がケーブル経由で開発用アプリに接続します。iPhone はどのネットワークにもつながっている必要がありません。ケーブルがないときは、開発用アプリがネットワーク経由で `asuka dev` に接続します。Mac のアドレス（または `ASUKA_DEV_HOST` のアドレス）を順に試し、ポートは空いていれば 8791 です。iOS は一度だけ、このアプリがネットワーク上のデバイスを探してよいかを尋ねるので、許可してください（あとから「設定」›「アプリ」›「ローカルネットワーク」でもオンにできます）。どちらの場合も、`asuka dev` が起動し直せば開発用アプリは自動でつなぎ直し、ホーム画面から開き直しても `asuka dev` を見つけられます。開発用アプリが接続できない場合は、何を確かめればよいかを Asuka が表示します。

**スマートフォンのブラウザー**：`asuka dev --platform web --lan` でアプリをネットワーク上に配信すると、開くべきアドレスが表示されます。

## アセット

デスクトップでは、開発用アプリは `assets/` をその場で読みます。スマートフォンでは、アセットは変更があったときに開発用アプリへコピーされます。ブラウザーでは、開発サーバーがアセットを配信します。アセットを変更すると、開発用アプリがビルドし直されて起動します。

## ログ

開発用アプリは自身の出力とエラーを `asuka dev` に送り、`asuka dev` がそれをターミナルに表示します。どのプラットフォームでも、`print`、捕捉されなかったエラー、Asuka 自身の警告とエラーが表示されます。開発用アプリがモジュールを読み込めなかった場合は、その理由が表示されます（`asuka: the player: ...`）。ブラウザーでは、ページのエラーもターミナルに表示されます。

## エディター

`asuka lsp` は Asuka の言語サーバーです。`.asuka` ファイルに対して、入力中の診断、補完、`///` のドキュメントを表示するホバー、定義へのジャンプ、参照の検索、名前の変更、フォーマット（`asuka fmt` と同じもの）、シンボル、シグネチャヘルプ、セマンティックハイライトを提供します。保存していない変更も含めて、編集中のパッケージと、それをインポートするパッケージをチェックします。

VS Code 用の拡張機能は `editors/vscode` にあります。この言語のハイライトを加え、`asuka lsp` を実行します（`PATH` にある `asuka`、または `asuka.server.path` で指定したもの）。

```sh
cd editors/vscode
npm install
npx @vscode/vsce package
code --install-extension asuka-0.1.0.vsix
```

LSP クライアントを備えたエディターなら、どれでも使えます。`asuka lsp` を実行すると、標準入出力で LSP 3.17 を話し、`.asuka` ファイルを扱い、`asuka.toml` のあるディレクトリーをルートにします。Neovim と Helix の設定は `editors/vscode` の README にあります。

## スナップショットとスクリプト

デスクトップのアプリは、いくつかの環境変数を受け付けます。これらを使うと、アプリに特定の状態を表示させてキャプチャーできるので、変更を目で確かめたり、CI で確認したりするのに便利です。リリースビルドの実行ファイルか、`asuka dev`（自身の環境変数を開発用アプリに渡します）に付けて実行します。

```sh
ASUKA_SNAPSHOT=shot.png ./MyApp.app/Contents/MacOS/MyApp            # ウィンドウが表示されたら PNG に保存して終了
ASUKA_SNAPSHOT=shot.png ASUKA_DUMP=1 asuka dev                      # あわせてビューツリーを shot.png.txt に書き出す
ASUKA_SCRIPT="tap:Add;tap:Add" ASUKA_SNAPSHOT=shot.png asuka dev    # 先に操作を実行する
```

- `ASUKA_SNAPSHOT`：ウィンドウが表示されたら（スクリプトがあればその実行後に）、ウィンドウを PNG に書き出して終了します。`ASUKA_SNAPSHOT_STAY=1` を指定すると、終了せずにアプリを実行し続けます。macOS ではウィンドウサーバーが合成したウィンドウの画像（シートも含む）なので、マテリアルや Liquid Glass も画面どおりに写ります。ファイル名が `*.content.png` なら、タイトルバーを除いたコンテンツだけを書き出します。
- `ASUKA_DUMP`：その PNG の隣にビューツリーを書き出します。各ビューの種類、フレーム、表示内容が含まれます。
- `ASUKA_SCRIPT`：スナップショットの前に、`;` で区切った操作を順に実行します。使える操作は、`tap:<text>`（そのテキストを表示しているビュー。ボタンのタイトル、テキスト、タブ、サイドバーの行、セグメントなど）、`type:<text>`（フォーカスのある入力欄に入力）、`back`、`key:<shortcut>`（`ctrl+shift+k`）、`menu:<item>`（メニューバーの項目）、`escape`、`close`（使用中のウィンドウを閉じる）、`wait:<duration>`（`500ms`）、`waitfor:<file>`（そのファイルができるまで待つ。続けるタイミングをテストが知らせる）、`snap:<file>` です。

## ビルドキャッシュ

ビルドでコンパイルしたものやダウンロードしたものは Asuka のキャッシュに保存され、このコンピューター上のすべてのアプリで共有されます。2 つ目のアプリをビルドするときや、少し変更してからビルドし直すときは、新しい部分だけがコンパイルされます。あるプラットフォーム向けの最初のビルドでは、そのプラットフォーム向けにビルド済みの Asuka のライブラリを npm レジストリから一度だけダウンロードします。`.npmrc` のレジストリとトークン（または `npm_config_registry`）もそのまま使われるので、ミラーやプロキシーも利用できます。Swift のライブラリは、ビルドに使われた Xcode か、それより新しい Xcode でのみ使われます。Xcode がそれより古い場合は、代わりにお使いのコンピューターで一度だけコンパイルされます。

- `asuka cache stats` は、キャッシュの場所とサイズ、ビルドが必要なものをキャッシュから見つけられた回数を表示します。
- `asuka cache clean` はキャッシュを空にします。次のビルドでは、すべてがコンパイルし直されます。
- キャッシュは 10 GB 以内に保たれ、最も長く使われていないものから削除されます。`ASUKA_CACHE_MAX` で別の上限を（`ASUKA_CACHE_MAX=20G`、`500M`、上限なしなら `0`）、`ASUKA_CACHE` で別の場所を指定できます。

## よくある問題

- **`cbuild: no C compiler`**：システムの C コンパイラー（Xcode のコマンドラインツール、`build-essential`、LLVM）をインストールするか、Zig をインストールして `ASUKA_CC=zig` を指定します。`building for the web needs Zig` と `cross-compiling for ... needs Zig` の場合は、[Zig](https://ziglang.org/download) をインストールしてください。Zig は Web 向けや、ほかのシステム向けにビルドします。
- **`no iOS simulator SDK (install Xcode)`**：Xcode をインストールして一度起動し、Xcode の設定の「Components」で iOS シミュレーターのランタイムを追加します。`no iPhone simulator; add one in Xcode` や `no simulator "..."` は、その名前のシミュレーターがないという意味です。シミュレーターの一覧は `xcrun simctl list devices` で確認できます。
- **iPhone の準備ができていない**：`... isn't paired with this Mac`（ケーブルで接続し、ロックを解除して「信頼」をタップ）、`... isn't reachable`（ケーブルで接続するか、Mac と同じ Wi-Fi でスリープを解除）、`... has Developer Mode off`（「設定」›「プライバシーとセキュリティ」›「デベロッパモード」）、`... is locked`（ロックを解除）。接続しているのに `no iPhone or iPad is connected` と表示される場合は、`xcrun devicectl list devices` に表示されるか確かめてください。
- **`Xcode couldn't make the development profiles`**：チームの Apple ID で Xcode にサインインするか（Xcode ›「設定」›「アカウント」）、App Store Connect API キーを設定してください。ほかのチームが持っている bundle ID なら、`asuka.toml` で別の `id` にする必要があります。`the keychain has Apple Development certificates of N teams` の場合は、`--team` でチームを選んでください。
- **`the player hasn't connected yet`**：iPhone をケーブルでつないでロックを解除するか、Mac と同じネットワークにつなぎ、ローカルネットワークへのアクセスを許可し、Mac のファイアウォールで `asuka` が接続を受け付けられるようにしてください（「システム設定」›「ネットワーク」›「ファイアウォール」）。
- **`no Android SDK: install Android Studio or set ANDROID_HOME`**：Asuka は `ANDROID_HOME`、`ANDROID_SDK_ROOT` の順に探し、次に Mac なら `~/Library/Android/sdk`、Windows なら `%LOCALAPPDATA%\Android\Sdk`、Linux なら `~/Android/Sdk` を探します。
- **`no Android NDK`**：Android Studio の SDK Manager（SDK Tools、NDK）から NDK をインストールします。Asuka は、このコンピューター向けのビルドがあるもののうち最新のものを使います（Windows の PC では、Arm でも `windows-x86_64`）。
- **`no Android device or emulator`**：Android Studio の Device Manager で AVD を作るか、実機を接続します。Asuka が起動したエミュレーターについては、起動が完了するまで最大 4 分待ちます（`the emulator didn't boot`）。
- **Java**：Gradle にはバージョン 17〜24 の JDK が必要です。`JAVA_HOME` がその範囲ならそれが使われ、そうでなければ Android Studio に同梱の JDK が使われます。Android Studio はインストーラーか JetBrains Toolbox が置いた場所から探します（Windows では Program Files か `%LOCALAPPDATA%\Programs`）。それもなければ、インストール済みの JDK のうち範囲内で最も新しいものが使われます。どれもないときはビルドがそう伝えるので、`JAVA_HOME` をその範囲の JDK に設定してください。
- **Android の初回ビルドが遅い**：初回だけ、Gradle が自分自身と Android のライブラリをダウンロードするためです。
- **実機にリリースビルドが入っている**：`asuka build` でインストールしたアプリ（署名鍵が異なります）は、開発用アプリをインストールするために、データごとアンインストールされます。
- **`asuka dev --platform windows runs the app on this computer`**：`asuka dev` で Windows のアプリを実行できるのは、Windows 上だけです。ほかのシステムでは `asuka build --platform windows` でビルドし、できたフォルダーを PC にコピーしてください。
- **Linux での `... isn't installed`**：アプリには GTK 4.14 以降と libadwaita 1.5 以降が必要です。また、パッケージによっては独自のライブラリが必要です。`WebView` には WebKitGTK 6.0、動画とオーディオには GStreamer、セキュアストレージには libsecret が必要です。足りないライブラリはエラーメッセージに示されるので、ディストリビューションのパッケージマネージャーでインストールしてください。
- **Linux のアプリが開かない**：アプリにはディスプレイが必要です。SSH 経由や CI では Xvfb（`xvfb-run -a`）の下で実行し、D-Bus を使う機能のためにセッションバス（`dbus-run-session`）も用意してください。
- **最初からやり直す**：Asuka のビルド、キャッシュ、アプリの開発用データは、Asuka のキャッシュディレクトリにあります。場所は Mac では `~/Library/Caches/asuka`、Linux では `~/.cache/asuka`、Windows では `%LocalAppData%\asuka` です。`asuka cache clean` でこのディレクトリを空にすると、データのない状態から、すべてをビルドし直せます。

## asuka.toml

`asuka dev` と `asuka build` は、同じ `asuka.toml` を読み込みます。キーについては [asuka.toml のリファレンス](asuka-toml.md)を、リリースに向けてそれらがどう組み合わさるかについては[配布](shipping.md#asukatoml)を参照してください。
