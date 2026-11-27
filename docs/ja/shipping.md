# 配布

`asuka build` は、アプリのディレクトリ（または指定したディレクトリ）から、一つのプラットフォーム向けのリリースビルドを作り、そのディレクトリの `dist` に出力します。アプリは事前にコンパイルされ、プラットフォーム自身のコンパイラーで、そのプラットフォーム向けに最適化してビルドされます。配布するものにインタープリターは含まれません。また [`@debug`](language/control-flow.md#開発ビルド向けのコード) は false なので、開発専用のコードも含まれません。`asuka publish` は、iOS と Android のビルドをそれぞれのストアにアップロードします。行き先は TestFlight と App 審査、または Google Play のトラックです（[ストアへの公開](#ストアへの公開)を参照）。デスクトップのビルドについては、インストール済みのアプリが更新に使う署名済みのアップデートを、GitHub のリリースか S3 バケットにアップロードします（[自動アップデート](#自動アップデート)を参照）。

| `--platform` | `dist` に作られるもの | ビルドできる環境 | ユーザーへの届け方 |
|---|---|---|---|
| `ios` | `<Name>.ipa`（App Store 向けに署名済み） | Xcode のある Mac | TestFlight、App Store（`asuka publish`） |
| `android` | `<Name>.aab`（アップロード鍵で署名済み） | JDK、Android SDK、NDK があればどのシステムでも | Google Play（`asuka publish`） |
| `mac` | `darwin-<arch>/`：`<Name>.app` とその `.zip`、`.dmg`（Apple シリコン向け。`--arch amd64` で Intel 向け）、または `.pkg`（`--appstore`） | Xcode のコマンドラインツールのある Mac | 署名・公証済みの `.dmg` か `.zip`、Mac App Store |
| `windows` | `windows-<arch>/`：`<Name>/`（`.exe` とそのアセット）、インストーラー（`-setup.exe`）、フォルダーの `.zip`、`.msix`（`--package msix`） | clang のある Windows、または Zig のあるどのシステムでも | インストーラー、`.zip`。MSIX は Microsoft Store での配布、またはサイドロード |
| `linux` | `linux-<arch>/`：インストールプレフィックス、`.tar.gz`、`.deb`、`.rpm`（自分の OpenPGP 鍵で署名）、AppImage、Flatpak マニフェスト | C コンパイラーのある Linux、または Zig のあるどのシステムでも | `.deb`、`.rpm`、AppImage、`.tar.gz`、Flatpak。apt と dnf のリポジトリ（`asuka publish --repo`） |
| `web` | `web/`（静的サイト） | Zig のあるどのシステムでも | 任意の Web サーバーや CDN |

`--platform` を省略すると、Mac、Windows PC、Linux マシンでは、そのコンピューター向けにビルドします。ビルドにはプラットフォームのコンパイラーを使います（Apple のプラットフォームでは Xcode のもの、Android では NDK のもの、Windows と Linux ではシステムのもの）。[Zig](https://ziglang.org/download) は、Web 向けにビルドするときと、ほかのシステムから Windows と Linux 向けにビルドするときに使います（`ASUKA_CC=zig` を指定すると、どこでも Zig を使います）。デスクトップのビルドの中身は[デスクトップ](desktop.md#デスクトップアプリの構成)を参照してください。

## asuka.toml

```toml
name = "Notes"
id = "com.example.notes"
version = "1.2"
build = 7
icon = "icon.png"

[ios]
team = "ABCDE12345"

[android]
keystore = "upload.jks"
keyAlias = "upload"
```

キーはどれも省略できます。すべてのキーとその型、デフォルト値は [asuka.toml リファレンス](asuka-toml.md)にまとめてあります。次のキーはすべてのプラットフォームに適用されます。

- `name` はアプリの名前です。アイコンの下、Mac のメニューバー、Windows のウィンドウのタイトル、Linux のランチャー、Web ページのタイトルに表示されます。ビルドされるファイルの名前にもなります（`Notes.app`、`Notes.exe`）。デフォルトはディレクトリの名前です。
- `id` は、iOS と Mac では bundle ID、Android と Linux では application ID（Linux ではデスクトップファイル、アイコン、D-Bus 名に使われます）、Windows では AppUserModelID（トースト通知に使われます）になります。デフォルトは `dev.asuka.<directory>` です。
- `version` はユーザーに見えるバージョンです（デフォルトは `1.0`）。`build` はアップロードしたビルドの通し番号で、ビルドのたびに増やす必要があります（デフォルトは 1）。ただし `asuka publish` は、各ビルドにストア上の最大の番号に続く番号を自動で付けるので、`build` は番号の始まりを決めるだけです。Windows では二つを合わせて `.exe` のファイルバージョンになり（バージョン 1.2、ビルド 7 なら `1.2.0.7`）、Linux では metainfo の release になります。
- `icon` は 1024×1024 の正方形の PNG で、iOS 向けには透過のないものにします。各ビルドは、プラットフォームが必要とするサイズを作ります。iOS ではアセットカタログ、Android ではランチャーアイコン、Mac では `.icns`、Windows では `.exe` に埋め込む `.ico`、Linux では hicolor テーマの 16〜512 ピクセルのアイコン、Web では Web マニフェストのアイコン、favicon、iOS のホーム画面アイコンです。
- `accent` はアプリのアクセントカラーで、`"#rrggbb"` の形式で指定します。iOS では tint、Android では Material 3 のカラースキームのシード、Mac では `Accent`、塗りつぶしボタン、リンクの色（AppKit 自身のコントロールはユーザーが選んだアクセントカラーのままです）、Windows ではシステムのアクセントカラーに代わるコントロールのアクセント、Linux では libadwaita のアクセント、Web ではページのアクセントとマニフェストの `theme_color` になります。指定しなければ、各プラットフォーム自身のアクセントカラーになります。
- `scheme` を指定すると、`myapp://` の URL でアプリが開くようになります。対象は iOS、Android、Mac（`CFBundleURLTypes`）、Linux（デスクトップファイルの `x-scheme-handler`）、Windows（アプリが起動時に `HKEY_CURRENT_USER\Software\Classes` にユーザー単位で登録します）です。`links` を指定すると、自分のドメインへの https リンクでアプリが開きます（ユニバーサルリンク、App Links。iOS と Android）。どちらも `onOpenURL` で受け取ります。
- `fileTypes` はアプリが開くファイルの種類で、そのファイルは `onOpenFiles` で受け取ります。

  ```toml
  [[fileTypes]]
  name = "Markdown Document"
  extensions = ["md", "markdown"]
  role = "viewer"

  [[fileTypes]]
  name = "Notes Document"
  extensions = ["note"]
  icon = "note.png"
  ```

  `extensions` は必須です。`name` のデフォルトは、最初の拡張子を大文字にして「Document」を付けたものです。`mimeTypes` と `uti` のデフォルトは、Asuka が知っている拡張子（md、txt、json、csv、pdf、png、jpg など）であれば、各システムでの呼び名になります。それ以外の拡張子では、アプリ独自の型 `application/x-<app>-<ext>` と `<id>.<ext>` になります。この型は Mac と iOS ではエクスポートされ、Linux では shared-mime-info に定義され、アプリがそのファイルのデフォルトアプリになります。`conformsTo` は、エクスポートまたはインポートする型が準拠する UTI です（デフォルトは `public.data`、`public.content`）。`role` は `editor`（デフォルト）または `viewer` です。`icon` は正方形の PNG で、Mac、Windows、Linux でそのファイルのアイコンになります。宣言のしかたはプラットフォームごとに異なります。Mac と iOS では Info.plist（`CFBundleDocumentTypes`。iOS ではドキュメントをその場で開きます）に、Android ではインテントフィルターとして宣言します。Windows ではアプリが起動時にユーザー単位で登録します（型ごとの ProgID、拡張子の `OpenWithProgids`、`.exe` に埋め込んだアイコン）。Linux ではデスクトップファイルの `MimeType` と `share/mime/packages/<id>.xml` に書かれます（`.tar.gz` を展開したあとは `update-mime-database` と `update-desktop-database` を実行してください。`.deb`、`.rpm`、Flatpak なら自動で実行されます）。Web ではマニフェストの `file_handlers` に書かれます（Chromium でインストールしたアプリが対象です）。
- `singleInstance` はデフォルトで `true` です。Windows と Linux では、`id` のあるアプリは一つしか起動しません。二回目に起動したアプリは、引数を実行中のアプリに渡して（`onSecondInstance`、ファイルは `onOpenFiles` に届きます）終了します。`false` にすると、起動した回数だけアプリが立ち上がります。Mac では、どちらの場合もアプリは一つしか起動しません。

プラットフォームごとのキーは次のとおりです。

- `ios.*` は iOS ビルドの設定です（[iOS](#ios)）。`ios.team` は Mac ビルドのチームにもなり、`ios.usage` の利用目的の説明文は Mac の Info.plist にも入ります。
- `android.*` は Android ビルドの設定です（[Android](#android)）。
- `mac.profile` は Mac ビルドのプロビジョニングプロファイル、`mac.appStoreProfile` は Mac App Store 向けのプロビジョニングプロファイルです。`mac.packages` は配布に使う形式です。[macOS](#macos) を参照してください。
- `linux.*` は、Linux ビルドのデスクトップファイル、AppStream metainfo、パッケージに書かれる内容と、作るパッケージの種類です。[Linux](#linux) を参照してください。
- `web.vapidPublicKey` はアプリサーバーの VAPID 公開鍵で、Web で `platform/push` を使うとき（Web Push）に必要です（[Web](web.md#platformpushweb-push) を参照）。
- Windows ビルドには、上のキー以外に必要なものはありません。署名用の証明書は環境変数で渡します（[Windows](#windows)）。

次のキーはスマートフォンとタブレット向けです。

- `orientations`：スマートフォンでの画面の向きで、`portrait`、`landscape` を指定します（デフォルトは両方）。iPad では、Split View、Slide Over、ステージマネージャに必要なため、アプリはすべての向きに対応します。Android 16 も同様に、タブレットと折りたたみ端末ではこの設定を無視します。
- `ios.iphoneOnly` を指定すると、iPad を対象から外します。デフォルトではアプリは iPad アプリでもあり、ユーザーがウィンドウに与えたサイズで動きます。Android では、分割画面やデスクトップのウィンドウに合わせてサイズを変えられます。レイアウトは、デバイスのサイズではなく、ウィンドウのサイズである `environment()` の `width` と `height` をもとに組み立ててください。
- `shareTarget` を指定すると、ほかのアプリの共有シートにアプリが表示されます。`shareTarget = true` ならテキストとリンクを、`[shareTarget]` のテーブルに `types = ["text", "image/*"]`、`max = 10` と書けばファイルも受け取ります（[デバイスとサービス](device.md#ほかのアプリからの共有onshare)を参照）。受け取ったものは `onShare` に届きます。iOS ではアプリに含まれる共有拡張機能（Share Extension）になり、独自の bundle ID（アプリの ID に `.share` を付けたもの）を持ちます。
- `splash` は起動画面です。`[splash]` のテーブルに `image = "logo.png"`、`size = 96`、`background = "#ffffff"`、`darkBackground = "#000000"` のように指定します（[デバイスとサービス](device.md#起動画面platformsplash)を参照）。
- `widgets` はアプリのホーム画面ウィジェット、`shortcuts` は Siri、Spotlight、ランチャー向けのアクションです（[デバイスとサービス](device.md#ホーム画面ウィジェットplatformwidget)を参照）。ウィジェット拡張機能は独自の bundle ID（アプリの ID に `.widgets` を付けたもの）を持ちます。そのためリリースにはこの拡張機能用のプロファイルも必要で、`ios.widgetProfile` で指定できます。
- `android.googleServices` は、`platform/push` に使う Firebase プロジェクトの `google-services.json` です。`android.mapsKey` は、`platform/maps` に使う Google Maps Platform の API キーです。
- `ios.usage` は保護されたリソースの利用目的の説明文（`NSCameraUsageDescription` など。Mac のものも含みます）を指定し、`ios.infoPlist` は Info.plist にキーを追加します。`ios.backgroundModes` は `UIBackgroundModes` に値を追加します（`"audio"` なら、画面がロックされていても録音・再生できます）。`android.permissions` はパーミッションを追加します。

アプリがインポートしたパッケージは、自分に必要なパーミッションと利用目的の説明文をアプリに追加します。`platform/camera` ならカメラ、`platform/location` なら位置情報のものです。ただしデフォルトの説明文は汎用的なので、`ios.usage` に自分の言葉で書いてください。パッケージによっては、App ID のケイパビリティも必要です（`platform/push` には Push Notifications、`platform/appleauth` には Sign in with Apple、`platform/integrity` には App Attest、`platform/agerange` には Declared Age Range、`platform/icloud` には iCloud とそのコンテナー）。developer.apple.com でケイパビリティを有効にしてから、プロファイルを作り直してください。プロファイルに足りないケイパビリティは、`asuka build --platform ios`（と `--platform mac`）が教えてくれます。

## iOS

```sh
asuka build --platform ios
```

このコマンドは、App Store 向けに署名した `.ipa` を作ります。中身はアプリ本体で、アプリが使うシステムのフレームワーク（インポートしたパッケージと表示する要素に応じて、WebKit や AVKit など）だけをリンクします。署名には、チームの配布用証明書（Xcode の「Accounts」設定から取得し、キーチェーンに入っているもの）と、アプリの ID 用のプロビジョニングプロファイルを使います。プロファイルは `ios.profile` か `--profile` で指定したもの、指定がなければインストール済みのもののうち最も合うものです。`--distribution ad-hoc` または `development` を指定すると、代わりにプロファイルに登録されたデバイス向けに署名します。`--simulator` を指定すると、シミュレーター用の署名なしの `.app` を作ります。

`std/sqlite` を使うアプリでは、SQLite がアプリのほかの部分と同じようにアプリに組み込まれます。アプリの隣に、別に埋め込んだり署名したりするライブラリはありません。

`shareTarget` を指定すると、共有拡張機能も署名されます。使うのは、アプリと同じチームの、拡張機能自身の ID（`com.example.notes` なら `com.example.notes.share`）用のプロファイルです。`ios.shareProfile` で指定したもの、指定がなければインストール済みのもののうち最も合うものが使われます。ワイルドカードのプロファイル（`com.example.*`）なら、アプリと拡張機能の両方に使えます。developer.apple.com で拡張機能の ID を登録し、アプリのときと同じようにプロファイルを作ってください。

ファイルを受け取る `shareTarget` には、App Group `group.<app ID>` も必要です。拡張機能は、このグループを通してファイルをアプリに渡します。developer.apple.com でグループを登録し、両方の ID（アプリと拡張機能）にそのグループを指定した App Groups ケイパビリティを付けてから、それぞれのプロファイルを作り直してください。ワイルドカードの ID には App Groups を付けられません。プロファイルにグループが含まれていなければ、ビルドはその旨を表示して止まります。シミュレーターでは、こうした準備はどれも必要ありません。独自の共有シート（`platform/sharesheet`）を持つアプリも、受け取るものが何であれ、データを渡すためにこのグループが必要です。

`asuka publish --platform ios` は、ビルドしてから App Store Connect にアップロードします（[ストアへの公開](#ストアへの公開)を参照）。Apple の Transporter でもアップロードできます。

**プライバシーマニフェスト。** App Store ではプライバシーマニフェストが必須で、Apple のリストにある API のうちアプリが使うものについて、それぞれ理由を記載する必要があります。Asuka は、バイナリが使うものをもとにこのマニフェストを書き出します。Asuka は時刻とファイルの日時を読み取ります。マニフェストでは、トラッキングをしないことを宣言し、アプリが収集するデータを `ios.collected` に書いたとおりに記載します。App Store Connect はこれを、アプリのプライバシーラベルと照合します。

```toml
[[ios.collected]]
type = "EmailAddress"
linked = true
purposes = ["AppFunctionality"]
```

`type` と `purposes` は、Apple の名前から接頭辞（`NSPrivacyCollectedDataType`、`NSPrivacyCollectedDataTypePurpose`）を除いたものです。`linked` はデータがユーザーの身元に紐付くことを、`tracking` はデータがユーザーのトラッキングに使われることを示します（[リファレンス](asuka-toml.md#ios)を参照）。アプリがトラッキングをする場合や、マニフェストにこれ以上の内容が必要な場合は、独自のマニフェストを `ios/PrivacyInfo.xcprivacy` として書き、上で述べた理由もそこに含めてください。Asuka は代わりにそのファイルを使い、その場合 `ios.collected` は無視されます。SQLite はアプリの実行ファイルの一部です。Asuka が書く理由は SQLite が使うもの（ファイルの日時、ディスクの空き容量）もカバーしており、独自のマニフェストにもそれらが必要です。

## Android

リリースはアップロード鍵で署名します。一度だけ作成し、安全に保管してください。

```sh
keytool -genkeypair -v -keystore upload.jks -alias upload -keyalg RSA -keysize 2048 -validity 10000
```

鍵を `asuka.toml` で指定し（`android.keystore`、`android.keyAlias`）、パスワードを環境変数で渡します。

```sh
ASUKA_KEYSTORE_PASSWORD=... asuka build --platform android
```

（鍵のパスワードがキーストアと異なる場合は `ASUKA_KEY_PASSWORD` も渡します。CI では `ASUKA_KEYSTORE` と `ASUKA_KEY_ALIAS` でキーストアとエイリアスを指定します。）これで Google Play 向けの `.aab` ができます。対象は、64 ビットと 32 ビットの ARM のスマートフォンと x86_64（エミュレーター、Chromebook）で、Android 7 以降です。中身は、アーキテクチャごとにコンパイルしたアプリです。Play はこの `.aab` から、各スマートフォンのアーキテクチャだけを含む APK を作ります。`std/sqlite` を使うアプリでは、各アーキテクチャのアプリに SQLite が組み込まれます。`asuka publish --platform android` は、ビルドしてから Google Play にアップロードします（[ストアへの公開](#ストアへの公開)を参照）。

`--apk` を付けると、代わりにすべてのアーキテクチャを含む署名済みのリリース用 `.apk` を作ります。そのままインストールできます（`adb install dist/<Name>.apk`）。リリースビルドのテストや、ストアを通さずにアプリを配るときに使います。

**テレビ**：`asuka.toml` の `[tv]` で `android = true` にすると、Android のビルドは Android TV のアプリにもなり、スマートフォンとテレビで同じ `.aab` を使います。`asuka build --platform tvos` は Apple TV のアプリを作ります。tvOS のプロファイルで署名した `.ipa` が `dist/tvos` にでき、`asuka publish --platform tvos` でアップロードできます（[ストアへの公開](#ストアへの公開)を参照）。[テレビ](tv.md)を参照してください。

## ストアへの公開

`asuka publish` は、`asuka build` と同じようにリリースビルドを作り、ストアの API でアップロードします。最初に、アップロードを妨げそうなことをストアに確認します（アプリが登録されていて鍵にアップロードの権限があるか、そのバージョンがまだビルドを受け付けるか、TestFlight のグループが存在するか）。そのため、失敗するアップロードのためにわざわざビルドすることはありません。各ビルドの番号はストア上の最大の番号に 1 を足したもの（かつ `asuka.toml` の `build` 以上）になるので、リリースのたびに番号を上げる必要はありません。`--build 42` で番号を指定することもできます。

**App Store Connect。** App Store Connect の「ユーザとアクセス」›「統合」›「App Store Connect API」で、「App マネージャ」の役割を持つ API キーを作成し、`.p8` をダウンロードします。そのキーを `asuka.toml` か環境変数（`ASUKA_ASC_KEY_ID`、`ASUKA_ASC_ISSUER_ID`、そして `.p8` のパスか中身を入れた `ASUKA_ASC_KEY`）で指定します。`key` を指定しない場合は、`altool` と同じように `~/.appstoreconnect/private_keys` にある `AuthKey_<id>.p8` を探します。`.p8` はリポジトリに入れないでください。また、事前に App Store Connect でアプリを bundle ID とともに追加しておきます（「アプリ」›「+」›「新規 App」）。

```toml
[ios.appStoreConnect]
keyId = "ABC123DEF4"
issuerId = "57246542-96fe-1a63-e053-0824d011072a"
```

```sh
asuka publish --platform ios                                  # TestFlight へ：チームのテスターに配布
asuka publish --platform ios --groups "Friends, QA" --notes "Try the new sync."
asuka publish --platform ios --review --notes @release-notes/ios --release manual
```

このコマンドは `.ipa` をアップロードし、Apple の処理が終わるのを待ちます（たいていは数分ですが、1 時間以上かかることもあります）。その間、Apple が警告した内容や却下した内容を表示します。そのあと、次のように動きます。

- `--notes` は TestFlight の「テスト内容」になり、`--review` を付けた場合は App Store の「このバージョンの新機能」にもなります。テキストか、アプリのプライマリ言語で書いた `@file` を指定します。あるいは `@dir` を指定し、言語ごとに一つずつファイルを置きます。ファイル名は App Store Connect での言語の名前に合わせます（`en-US.txt`、`ja.txt`）。アップデートでは、そのバージョンのすべての言語に「このバージョンの新機能」が必要です。
- `--groups` は、名前で指定した TestFlight のグループにビルドを配布します。外部グループ（チーム外のテスター）を指定すると、ビルドはベータ版 App 審査に回ります。先に TestFlight でテスト情報を入力しておいてください。
- `--review` は、App Store のバージョン（`asuka.toml` の `version`）がなければ作成し、そこにビルドを割り当てて審査に提出します。初回の提出では、先に App Store Connect でストアの掲載情報、スクリーンショット、プライバシーに関する回答を入力しておく必要があります。足りないものは表示されます。`--release auto` は承認されるとすぐにリリースし、`--release manual` はリリースを手動で行うまで待ちます。
- `--no-wait` は、Apple がアップロードを受け取った時点で終了します。

`asuka publish --platform mac --appstore` は、同じキーで Mac App Store に対して同じことを行います。`.pkg`（`--appstore` でビルドしたもの）、アプリの macOS 向けのビルドとバージョン、Mac 版の TestFlight、審査が対象です。

`asuka publish --platform tvos` は、同じキーとオプションで Apple TV アプリに対して同じことを行います。tvOS の `.ipa`（アプリの ID に対応する tvOS の App Store 用プロファイルで署名したもの。`--profile` で選べます。iPhone アプリのプロファイルでは署名できません）、アプリの tvOS 向けのビルドとバージョン、TestFlight のグループ、審査が対象です。App Store Connect では iPhone アプリと Apple TV アプリは一つのレコードで、バンドル ID と App 内課金を共有します。先にそのレコードに tvOS を追加してください（アプリのページで Add Platform › tvOS）。`asuka publish` はビルドの前にこれを確認します。

**Google Play。** Google Play Android Developer API を有効にしたプロジェクトで、Google Cloud のサービス アカウントを作成し、JSON キーを発行します（「IAM」›「サービス アカウント」›「キー」）。Play Console の「ユーザーと権限」でそのアカウントのメールアドレスを招待し、アプリをリリースする権限を付与します。キーは `asuka.toml`（`[android]` の `serviceAccount = "play.json"`）か `ASUKA_PLAY_SERVICE_ACCOUNT`（パスか JSON そのもの）で指定します。Play は、アプリの最初の `.aab` を Play Console からしか受け付けません。Play Console でアプリを作成し、最初のビルドは手動でアップロードしてください。2 つ目以降のビルドは `asuka publish` でアップロードできます。

```sh
asuka publish --platform android                              # 内部テストトラック
asuka publish --platform android --track beta --notes @release-notes/android
asuka publish --platform android --track production --rollout 0.1
```

- `--track` は `internal`（デフォルト、内部テスト）、`alpha`（クローズドテスト）、`beta`（オープンテスト）、`production`（製品版）、またはクローズドテストのトラックの名前です。
- `--rollout 0.1` は、リリースをユーザーの 10 分の 1 に段階的に公開します。割合は Play Console で引き上げます。`--draft` は下書きを作り、公開は Play Console で行います。アプリが公開されるまで Play は下書きしか受け付けないため、そのあいだリリースは下書きになります。
- `--notes` はリリースノートです（500 文字以内）。テキストか、ストアの掲載情報のデフォルト言語で書いた `@file` を指定します。あるいは `@dir` を指定し、Play での言語の名前を付けたファイルを言語ごとに置きます（`en-US.txt`、`ja-JP.txt`）。
- アプリに審査中の変更があると、処理は止まります。`--cancel-review` を付けると、その審査を取り消し、変更をこのリリースと一緒に送信します。Play がアプリの変更を送信待ちとして保留している場合（却下されたあとなど）は、変更は保存され、Play Console から送信することになります。

`--file dist/Notes.ipa`（または `.aab`）を指定すると、新たにビルドせず、`asuka build` で作ったビルドをアップロードします。

## macOS

```sh
asuka build --platform mac
```

このコマンドは、Mac 上で `dist/darwin-arm64/<Name>.app` を作ります。Apple シリコン搭載の Mac 向けで、macOS 11 以降に対応しています。隣には配布用の `<name>-<version>-darwin-arm64.zip` も作られます（Apple のツールと同じ方法で圧縮され、署名と公証が保たれます）。`--arch amd64` を付けると、代わりに Intel Mac 向けを別のビルドとして `dist/darwin-amd64` に作ります（両方に対応してサイズが倍になるユニバーサルアプリにはしません）。ビルドごとにファイル名が異なるので、1 つのリリースに並べて置けます。アプリには実行ファイルと、アプリの設定（`Contents/Resources/asuka-app.json`）、Info.plist、アイコン、言語、アセットを含み、アドホック署名されています。つまり、自分の Mac で動かせる状態です。アプリがインポートしたパッケージは、iOS と同じく `ios.usage` から利用目的の説明文を追加します（位置情報の説明文は、Mac では `NSLocationWhenInUseUsageDescription` から取った `NSLocationUsageDescription` になります）。あわせて、それに必要な hardened runtime のエンタイトルメント（`com.apple.security.device.camera`、`...audio-input`、`...personal-information.location` など）も追加します。これがないと、Developer ID で署名したアプリは、カメラ、マイク、ユーザーのデータへのアクセスを拒否されます。アプリのネイティブパッケージの Swift は、ビルドするアーキテクチャ向けに実行ファイルへコンパイルされます（[ネイティブコード](native.md)を参照）。`std/sqlite` を使うアプリでは、SQLite も実行ファイルに組み込まれます（[ライブラリ](data.md#ライブラリ)）。

`.zip` の隣には、アプリのディスクイメージ `<name>-<version>-darwin-arm64.dmg` も作られます。Mac アプリのダウンロードとしてはこれが一般的な形です。開くとアプリと「アプリケーション」フォルダーが並んだウィンドウが表示され、アプリをフォルダーにドラッグすればインストールできます。ディスクイメージのアイコンはアプリのアイコンです。LZFSE で圧縮され、アプリと同じ ID で署名されます。`--notarize` を付ければ、こちらも公証とステープルが行われます。ディスクイメージから直接起動したアプリは自分自身を更新できません（`update.unsupported()` は `.translocated`）。「アプリケーション」フォルダーにドラッグしたあとなら更新できます。`--package dmg` でディスクイメージだけを、`--package zip` で `.zip` だけを作ります。`asuka.toml` の `mac.packages` を書けば、すべてのビルドで同じ指定になります。

```toml
[mac]
packages = ["dmg"]   # デフォルトは "zip" と "dmg"
```

Mac App Store 以外でほかの人に配布するには、Developer ID で署名し、Apple に公証してもらいます。これで Gatekeeper がアプリを開けるようになります。

```sh
xcrun notarytool store-credentials asuka   # 初回のみ：Apple ID、チーム、App 用パスワード
asuka build --platform mac --sign "Developer ID Application: Your Name (TEAMID)" --notarize asuka
```

`--notarize` は、署名したアプリを Apple の公証サービスに送り、結果を待ちます（たいていは数分です）。却下された場合はサービスのログを表示します。最後にチケットをアプリにステープルするので、オフラインでも開けます。続いて、同じ ID で署名したディスクイメージにも同じことを行います。CI では `--notarize env` を指定すると、キーチェーンのプロファイルの代わりに、環境変数の `ASUKA_APPLE_ID`、`ASUKA_APPLE_PASSWORD`（App 用パスワード）、`ASUKA_TEAM_ID` を使います。

プッシュ通知（`platform/push`）、Sign in with Apple（`platform/appleauth`）、App Attest（`platform/integrity`）、iCloud（`platform/icloud`）には、プロビジョニングプロファイルでしか付与されないエンタイトルメントが必要です。developer.apple.com（Profiles）で、それらのケイパビリティを有効にしたアプリの ID に対して macOS の Developer ID プロファイルを作成し、ダウンロードしてください。

```sh
asuka build --platform mac --profile Notes.provisionprofile --notarize asuka
```

プロファイルは `Contents/embedded.provisionprofile` としてアプリに入ります。アプリは、プロファイルのエンタイトルメントのうちパッケージが必要とするもの（`com.apple.developer.aps-environment`、`com.apple.developer.applesignin`、`com.apple.developer.devicecheck.appattest-environment`）、アプリの ID とチーム、そして hardened runtime を付けて署名されます。プロファイルに足りないものはビルドが教えてくれます。`--sign` を省略すると、プロファイルが許可する、キーチェーン内の証明書（Developer ID Application）で署名します。プロファイルに含まれない証明書を `--sign` に指定するとエラーになります。`--profile` には、パス、またはインストール済みのプロファイルの名前か UUID を指定します。`ASUKA_MAC_PROFILE` や asuka.toml の `mac.profile` でも同じ指定ができます。どれも指定がなければ、これらのパッケージを使うアプリは、アプリの ID に最も合うインストール済みのプロファイル（Xcode のプロファイル用フォルダーにあるもの）で署名されます。それもなければプロファイルなしで署名されます。その場合、Mac で何が動かないかをビルドが表示し、アプリのそれ以外の部分は問題なく動きます。

`--appstore` を指定すると、代わりに Mac App Store 向けの `dist/darwin-arm64/<name>-<version>-darwin-arm64.pkg` を作ります。アプリは App Sandbox に入り（ネットワークと、ユーザーが選んだファイルにアクセスできます）、Mac App Store のプロファイルの証明書（Apple Distribution）で署名されます。プロファイルは asuka.toml の `mac.appStoreProfile`、なければ `--profile`、それもなければ最も合うものです。パッケージは、チームの Mac Installer Distribution 証明書で署名されます。`asuka publish --platform mac --appstore` は、これをビルドして、iOS アプリと同じように App Store Connect にアップロードします。TestFlight の `--groups`、Mac App Store の審査への提出の `--review`、`--notes`、以前にビルドした `.pkg` を渡す `--file` が使えます（[ストアへの公開](#ストアへの公開)を参照）。アプリのパッケージがより新しい macOS を必要とする場合（`platform/camera` は macOS 13）、アプリはそのことを Info.plist に書きます。公証はストア外で配布するアプリのためのものなので、`--appstore` と `--notarize` を一緒に指定するとエラーになります。

## Windows

```sh
asuka build --platform windows               # x64。Arm 版 Windows ではエミュレーションで動きます
asuka build --platform windows --arch arm64  # Arm の PC でネイティブに動きます
```

このコマンドは、`dist/windows-amd64/<Name>`（Arm 向けは `windows-arm64`）というフォルダーを作ります。`<Name>.exe` がアプリそのものです（Windows では clang、どのシステムでも Zig で、アーキテクチャに合わせてビルドされます）。その隣には、アプリの設定 `asuka-app.json` とアプリのアセットが置かれます。`std/sqlite` を使うアプリでは、SQLite が `.exe` に組み込まれます。`.exe` は GUI プログラム（コンソールウィンドウを開きません）で、リソースがリンクされています。Windows が使う各サイズのアイコン、バージョン情報（`version` と `build`）、そしてコモンコントロール 6、モニターごとの DPI 対応、UTF-8 を要求する、Windows 10 と 11 向けのマニフェストです。このマニフェストにより、アプリは開いたユーザーの権限で動きます（名前がどうであれ、管理者権限を求めることはありません）。ディレクトリに独自のリソースのオブジェクトファイル（COFF の `.syso`、`rsrc_windows_amd64.syso`）を置いたアプリでは、代わりにそのリソースが使われます。

フォルダーの隣には、electron-builder が作るのと同じ種類の配布用パッケージが置かれます。どれも `asuka build` がどのシステムからでも作れます（NSIS、WiX、Windows SDK は不要です）。

- `<name>-<version>-windows-amd64-setup.exe`：インストーラーです。いまの Windows アプリの多くと同じく、ワンクリックでインストールできます。実行したユーザーだけのために、管理者権限なしで、何も尋ねずに `%LOCALAPPDATA%\Programs\<Name>`（アプリが[自分自身を更新](#自動アップデート)できる場所）にインストールし、「スタート」メニューにショートカットを置きます。また、「設定」の「インストールされているアプリ」に、バージョン、発行元、サイズ、アンインストーラーとともに登録します。そのあとアプリを開きます。もう一度（新しいバージョンのものを）実行すると、アプリをその場で更新します。アプリが開いていれば、確認したうえで先に閉じます。前のバージョンにあって今回のバージョンにないものは削除します。アンインストーラーはアプリのフォルダーにある `Uninstall <Name>.exe` で、アプリ、ショートカット、アプリが登録したもの（URL スキーム、ファイルの種類、通知の登録）を取り除きます。アプリが保存したデータは残ります。スクリプトや IT 部門向けに、NSIS のインストーラーと同じく、どちらも `/S` を付けるとサイレントに実行でき（このときインストーラーはアプリを開きません）、インストーラーは `/D=<folder>` で別のフォルダーにインストールできます。どちらもユーザーの Windows の表示言語で表示されます。Asuka が翻訳されている言語ならどれでもよく、それ以外は英語になり、アラビア語とヘブライ語では右から左に表示されます。アプリの名前は、アプリの言語のうちユーザーにいちばん合うもので、カタログの `asuka.appName` を使います（[システムが表示するもの](i18n.md#システムが表示するもの)）。`/LANG=de` で別の言語を指定できます。「スタート」メニューと「インストールされているアプリ」に表示されるのはアプリの `name` で、アプリ自身が作るショートカットと同じです。
- `<name>-<version>-windows-amd64.zip`：フォルダーを zip にしたもので、どこにでも展開できます。ポータブル版で、ユーザーが書き込める場所にあれば、これも自分自身を更新します。
- `<name>-<version>-windows-amd64.msix`：`--package msix` を付けたときに作られます。アプリをパッケージにしたもので、[Microsoft Store](https://partner.microsoft.com/dashboard) での配布とサイドロード（`Add-AppxPackage`、アプリ インストーラー、Intune）に使います。マニフェストでは、アプリを完全信頼（full trust）のデスクトップアプリとして宣言し、タイルの画像（`icon` から作ったもの）、URL スキーム、ファイルの種類、通知のアクティベーターも宣言します。パッケージ化されたアプリは、これらを実行時に登録できないためです。カタログの言語も宣言し（ソース言語が先頭、カタログがなければ英語）、Microsoft Store はそれをアプリの対応言語として表示します。ただし、マニフェストのアプリ名は翻訳されません。翻訳には `resources.pri` が必要ですが、ビルドはこれを作らないため、「スタート」メニューと「インストールされているアプリ」にはアプリの `name` が表示されます。Microsoft Store は署名されていないパッケージを受け付け、Store 側で署名します。`windows.identityName` と `windows.publisherId` をパートナー センターの「製品 ID」に表示される値にして、アップロードしてください。サイドロードする場合は、`.exe` と同じように署名します（後述）。証明書は、サブジェクトが `windows.publisherId`（既定では `CN=` と `windows.publisher`）で、その PC が信頼しているものである必要があります。MSIX のインストール、更新、削除は Windows が行い、アプリ自身は更新しません（`update.unsupported()` は `.managed`）。

`--package installer,msix` のように指定すると、`zip`、`installer`、`msix` のうち指定したものだけを作ります。`asuka.toml` の `windows.packages` に書くと、同じ指定がすべてのビルドに適用されます。既定ではインストーラーと `.zip` です。

```toml
[windows]
packages = ["installer", "zip", "msix"]
publisher = "Example Inc."
homepage = "https://example.com"
```

SmartScreen が発行元を認識できるように、`.pfx` 形式でエクスポートしたコード署名証明書でアプリに署名します。ビルドは `.exe`、インストーラーとそのアンインストーラー、MSIX に署名します。

```sh
ASUKA_WINDOWS_CERT=cert.pfx ASUKA_WINDOWS_CERT_PASSWORD=... asuka build --platform windows
```

ビルドは SHA-256 で署名し、証明書の期限が切れたあとも署名が有効であり続けるよう、署名にタイムスタンプを付けます。タイムスタンプには、最初に応答した RFC 3161 サーバーを使います。候補は `ASUKA_WINDOWS_TIMESTAMP` に指定したサーバー（カンマ区切りのリスト）で、指定がなければ DigiCert、Sectigo、GlobalSign のサーバーです。どれも応答しなければ、ビルドはその旨を表示して失敗します。`ASUKA_WINDOWS_TIMESTAMP=none` にすると、タイムスタンプなしで署名します（その場合、署名は証明書の期限とともに無効になります）。署名には、Windows では `signtool`（Windows SDK のもの。`PATH` にあればそれを、なければインストール済みの最新のものを使います）、それ以外のシステムでは `osslsigncode`（`brew install osslsigncode`、`apt install osslsigncode`）を使います。`ASUKA_WINDOWS_CERT` がなければ、`.exe` は署名されません。ハードウェアトークンやクラウドサービス（Azure Trusted Signing、キー コンテナー）にある証明書では、それぞれのツールで署名します。`ASUKA_WINDOWS_SIGN_COMMAND` にそのツールのコマンドを指定し、ファイルのパスが入る位置に `%1` と書くと、ビルドは自分で署名する代わりに、署名するファイルごとにそのコマンドを実行します。

```sh
ASUKA_WINDOWS_SIGN_COMMAND='AzureSignTool sign -kvu https://vault.vault.azure.net -kvc cert -kvm -tr http://timestamp.digicert.com -td sha256 %1' \
  asuka build --platform windows
```

通知を表示するアプリ（`platform/notify`）では、初回にユーザーの「スタート」メニューにショートカットが作られます。Windows でアプリがトースト通知を出すには、これが必要です。ショートカットの AppUserModelID は asuka.toml の `id`（なければアプリの名前から作ったもの）です。

## Linux

```sh
asuka build --platform linux                # このマシンのアーキテクチャ（それ以外の環境では amd64）
asuka build --platform linux --arch arm64   # amd64 または arm64
```

どのシステムからでもビルドでき、ビルドに GTK のインストールは要りません（アプリは起動時に GTK 4 と libadwaita を開きます）。Linux ではそのマシンの C コンパイラーで、それ以外では Zig でビルドされます。`dist/linux-amd64`（`--arch arm64` では `linux-arm64`）には次のものが作られます。

- `<name>/`：インストールプレフィックスの構成になっているフォルダーで、アプリの名前を小文字にした名前が付きます（`My Notes` なら `my-notes`）。
  - `bin/my-notes`：アプリ本体（1 つの実行ファイル）
  - `share/my-notes/assets` と `share/my-notes/asuka-app.json`：アセットと設定。これも `bin/` から見つけます。
  - `share/applications/<id>.desktop`：ランチャーが表示し、起動に使うもの
  - `share/icons/hicolor/<size>/apps/<id>.png`：16〜512 ピクセルの各サイズの `icon`
  - `share/metainfo/<id>.metainfo.xml`：AppStream metainfo。ソフトウェアセンターに表示される内容です。

  このフォルダーから `bin/my-notes` を実行するか、ランチャーが見つけられるプレフィックスにインストールします。

  ```sh
  tar -xzf my-notes-1.0-linux-amd64.tar.gz --strip-components=1 -C ~/.local
  ```

- `my-notes-<version>-linux-<arch>.tar.gz`：配布用に、そのフォルダーをまとめたもの
- `my-notes-<version>-linux-<arch>.deb`（Debian と Ubuntu 向け）と `my-notes-<version>-linux-<arch>.rpm`（Fedora 向け）：そのフォルダーを、システム標準のアプリと同じように `/usr` の下にインストールするパッケージ。アプリがディストリビューションに求めるもの、つまり GTK 4.14、libadwaita 1.5、libsoup、実行ファイルのビルド対象の glibc、そしてアプリのパッケージが開くライブラリ（`WebView` なら WebKitGTK、`platform/pdf` なら Poppler、音声と動画なら GStreamer など）も、パッケージマネージャーがあわせてインストールします。デスクトップファイル、アイコン、MIME タイプのキャッシュは、システム側のトリガーが更新します。プレリリースのバージョンは正式リリースより前に並びます（`1.3.0-beta.1` は `1.3.0~beta.1` になります）。OpenPGP 鍵があれば（[後述](#署名付きパッケージとリポジトリ)）、`.rpm` には `rpm --addsign` と同じ形で署名が入ります。

  ```sh
  sudo apt install ./my-notes-1.0-linux-amd64.deb
  sudo dnf install ./my-notes-1.0-linux-amd64.rpm
  ```

- `my-notes-<version>-linux-<arch>.AppImage`：アプリを 1 つのファイルにまとめたもの。ダウンロードしてそのまま実行できます。GTK 4.14 と libadwaita 1.5 は含まれないので、それらのあるディストリビューションが必要です。デスクトップファイルとアイコンは、AppImage のツールやランチャーが見つけられる場所に置かれます。FUSE のない環境（コンテナーなど）では `--appimage-extract-and-run` を付けて実行します。[アップデート](#自動アップデート)を設定していれば、`.tar.gz` のプレフィックスと同じように自分自身を更新します。また、AppImageUpdate、appimageupdatetool、AppImageLauncher が読むアップデート情報も持っています（`--appimage-updateinformation` で表示できます）。指す先は GitHub の最新リリースの `.zsync`、または `url` の下のもので、`asuka build` がアップデートの隣に書き出します。OpenPGP 鍵があれば、`appimagetool --sign` と同じ方法で署名され、署名と公開鍵がランタイムのセクションに入ります。AppImageUpdate と appimageupdatetool はこれを検証します。一度署名したら、別の鍵で署名されたアップデートや署名のないアップデートは拒否されます。[AppImage ランタイム](https://github.com/AppImage/type2-runtime)を使って作られ、ランタイムは初回に一度だけダウンロードされてビルドキャッシュに入り、チェックサムが検証されます。オフラインでビルドするには、手元の `runtime-x86_64` または `runtime-aarch64` を `ASUKA_APPIMAGE_RUNTIME` に指定します。
- `<id>.yml`：そのフォルダー（`bin`、`share`）を GNOME ランタイム上でパッケージにする [Flatpak](https://flatpak.org) のマニフェスト

  ```sh
  flatpak install flathub org.gnome.Platform//50 org.gnome.Sdk//50   # 初回のみ
  flatpak-builder --user --install --force-clean build dist/linux-amd64/com.example.notes.yml
  flatpak run com.example.notes
  ```

アプリを動かす環境には、GTK 4.14 と libadwaita 1.5 以降が必要です（GNOME ランタイム 46 以降に含まれています）。アプリの ID は application ID でもあり、デスクトップファイル、アイコン、D-Bus 名（二回目に起動すると、最初に起動したアプリのウィンドウが表示されます）、データディレクトリ（`~/.local/share/<id>`）の名前になります。ID は英字、数字、`_`、`-` だけからなり、二つ以上の部分に分かれていて、どの部分も数字で始まってはいけません。

`--package deb,appimage` のように指定すると、`tar.gz`、`deb`、`rpm`、`appimage`、`flatpak`（マニフェスト）のうち一部だけを作ります。`asuka.toml` の `linux.packages` を書けば、すべてのビルドで同じ指定になります。[自動アップデート](#自動アップデート)を設定したビルドは、どちらにしてもアップデートのアーカイブである `.tar.gz` を書き出します。どの形式も `asuka build` 自身がどのシステムからでも書き出すので、`dpkg`、`rpmbuild`、`mksquashfs` は要りません。

`.deb`、`.rpm`、Flatpak でインストールされたアプリは、それらの仕組みで更新され、アプリ自身は更新しません（`update.unsupported()` は `.managed`）。自分自身を更新するのは、ユーザーのプレフィックスに展開した `.tar.gz` と、ユーザーが書き込めるフォルダーに置いた AppImage です。

デスクトップファイル、metainfo、パッケージに書く内容は、`asuka.toml` で指定します。

```toml
[linux]
summary = "Write things down"
description = """
Notes, kept on your computer.

A second paragraph."""
categories = ["Office"]
keywords = ["memo"]
license = "MIT"
developer = "Example Inc."
homepage = "https://example.com"
flatpakRuntime = "50"
packages = ["tar.gz", "deb", "rpm", "appimage", "flatpak"]   # デフォルトはすべて
maintainer = "Example Inc. <dev@example.com>"
debDepends = ["libfoo1 (>= 1.2)"]
rpmRequires = ["foo >= 1.2"]
```

`categories` は [freedesktop.org のカテゴリ](https://specifications.freedesktop.org/menu-spec/latest/category-registry.html)から選びます（デフォルトは `Utility`。`.deb` のセクションもこれに従います）。`license` は SPDX 式です。Flathub とソフトウェアセンターは、summary、description、license、developer を求めます。`maintainer` は `.deb` と `.rpm` に書かれる保守担当者です（デフォルトは `developer`）。`debDepends` と `rpmRequires` には、それらが追加で依存するパッケージを、それぞれの書式で書きます（どちらか一方でよいものは、`.deb` では `a | b`、`.rpm` では `(a or b)`）。

### 署名付きパッケージとリポジトリ

ユーザーが apt や dnf のリポジトリを一度追加すれば、以降のバージョンはシステムのアップデート（`apt upgrade`、`dnf upgrade`、ソフトウェアセンター）と一緒に届き、あなたの鍵で検証されます。まず署名に使う OpenPGP 鍵（RSA 4096）を一度だけ作ります。

```sh
asuka linux keygen --uid "Example Inc. <dev@example.com>"
```

秘密鍵 `asuka-linux.key` と公開鍵 `asuka-linux.asc` が、設定ディレクトリの `asuka/linux-keys` に書き出されます（`--out` で別の場所にできます）。アップデートの鍵と同じく、秘密鍵はリポジトリに入れず、パスワードマネージャーと CI のシークレットに保管してください。ユーザーのシステムはこの鍵を一度信頼するだけなので、リポジトリを別の鍵で署名することはありません。CI でのビルドでは `ASUKA_LINUX_GPG_KEY` に鍵（またはそのファイルのパス）を、パスフレーズがあれば `ASUKA_LINUX_GPG_PASSPHRASE` に渡します（鍵を作るときにこれを設定しておくと、鍵が暗号化されます）。自分のマシンでは `linux.signingKey` に鍵のパスを書きます。

```toml
[linux]
signingKey = "~/.config/asuka/linux-keys/asuka-linux.key"

[linux.repository]
url = "https://packages.example.com/notes"   # 配信する場所
s3 = { bucket = "packages", prefix = "notes", endpoint = "https://<account>.r2.cloudflarestorage.com" }
```

鍵があれば、`asuka build` が `.rpm` と AppImage に署名します。そのうえで `asuka publish --platform linux` を実行すると、`.deb` と `.rpm` が `linux.repository` のリポジトリ（その `s3` バケット、または `dir` で指定したディレクトリ）か、`--repo` で指定した先（ディレクトリ、または `s3://bucket/prefix`）に署名付きで書き込まれます。リポジトリにあった既存のバージョンは残ります。`updates` があれば、アップデートはこれまでどおり `updates` の指定先に公開されます。

```sh
asuka publish --platform linux --repo ../packages
asuka publish --platform linux --arch arm64 --repo s3://packages/notes
```

リポジトリの中身は、すべて `asuka publish` 自身が Go で書き出します。

- `key.asc` と `key.gpg`：公開鍵。ASCII 形式とバイナリ形式です。
- `deb/`：apt のリポジトリ。`dists/stable/InRelease`（と `Release`、`Release.gpg`）、アーキテクチャーごとの `Packages`（ダイジェストごとにも保存され（`by-hash`）、apt はそこから取得するので、読んだ `InRelease` と違うバージョンのインデックスを読むことはありません）、そして `pool/` の下のパッケージです。
- `rpm/stable/`：dnf のリポジトリ。署名付きの `repodata/repomd.xml`（署名は `repomd.xml.asc`）とそこから参照されるファイル、`Packages/` の下のパッケージです。`rpm/notes.repo` は、このリポジトリの dnf の設定ファイルです。

プレリリースは、そのチャンネルのスイートとリポジトリ（`beta` と `rpm/notes-beta.repo`）に入ります。ベータ版のユーザーは、それを `stable` と並べて追加します。公開はそれぞれ、ほかの公開が書き込んだ内容に追加していくので、アーキテクチャーごとに同時に公開してもかまいません（CI のジョブを分けるなど）。`InRelease` と `repomd.xml` は、その公開が読んだときのままである場合にだけ書き込まれ（S3、R2、MinIO の条件付き書き込み。ディレクトリではロックファイル）、そうでなければ読み直してから追加します。S3 バケットでは、インデックスはキャッシュされない設定で配信されます（R2、MinIO などの互換サービスは `s3.endpoint` か `AWS_ENDPOINT_URL` でエンドポイントを指定します）。認証情報には `s3:GetObject`、`s3:PutObject`、`s3:DeleteObject`、`s3:ListBucket` の権限が必要です（`s3:ListBucket` がないと、まだ存在しないファイルに対して S3 が 403 を返し、公開はそこで止まります）。ディレクトリは Web サーバーや CDN から HTTPS で配信してください。

ユーザーが最初に一度だけ実行するコマンドです（`asuka publish` があなたの `url` を入れて表示します）。Debian と Ubuntu では次のとおりです。

```sh
curl -fsSL https://packages.example.com/notes/key.asc | sudo gpg --dearmor -o /etc/apt/keyrings/notes.gpg
echo "deb [signed-by=/etc/apt/keyrings/notes.gpg] https://packages.example.com/notes/deb stable main" | sudo tee /etc/apt/sources.list.d/notes.list
sudo apt update && sudo apt install notes
```

Fedora では次のとおりです（`gpgcheck` と `repo_gpgcheck` は有効です。dnf はパッケージとリポジトリのメタデータを検証し、最初に一度だけ鍵を信頼するかどうかを尋ねます）。

```sh
sudo dnf config-manager addrepo --from-repofile=https://packages.example.com/notes/rpm/notes.repo
sudo dnf install notes
```

（Fedora 41 より前は `sudo dnf config-manager --add-repo …` です。）ダウンロードした `.rpm` は、`rpm -K` で rpm が信頼する鍵と照合できます（先に `sudo rpm --import https://packages.example.com/notes/key.asc`）。

## Web

```sh
asuka build --platform web
```

このコマンドは `dist/web` に静的サイトを作ります。中身は、`index.html`、WebAssembly にしたアプリ（Zig でビルドされます）、Asuka のスクリプト、アプリの Service Worker、アセット、アイコン付きの Web アプリマニフェスト、gzip で圧縮したコピー、そして Cloudflare と Netlify 向けの `_headers` ファイルです。ファイル名にはダイジェストが付くので、`index.html` とマニフェスト以外は無期限にキャッシュできます。このフォルダーを任意の Web サーバーや CDN から、`.wasm` を `application/wasm` として、HTTPS で配信してください（Service Worker、Web Push、セキュアストアには HTTPS が必要です）。中身の詳細と、ブラウザーでの動作の違いについては [Web](web.md) を参照してください。

あわせて `version.json` も書き出します。ビルドしたバージョンとそのリリースノート（`CHANGELOG.md` の該当セクション）が入っていて、ページ内の `platform/update` がこれを読みます。サイトより古いページは、再読み込みを提案します（[自動アップデート](#自動アップデート)）。

## 自動アップデート

デスクトップアプリは、Sparkle を使う Mac アプリと同じように自分自身を更新します。フィードで新しいバージョンを確認し、リリースノートを表示し、ダウンロードします（実行中のバージョンからの差分があれば、変わった部分だけです）。それから自分の鍵で署名されていることを確かめ、インストールして再起動します。スマートフォンのアプリはストアから更新され、ストアのリリースのあいだはコードを無線で更新できます（[後述](#スマートフォンアプリのコードのアップデート)）。Web アプリのページは新しいサイトがあれば再読み込みされます。

### 準備

アップデートに署名するための鍵ペアを、一度だけ作ります。

```sh
asuka update keygen
```

このコマンドは、秘密鍵 `asuka-update.key` と公開鍵 `asuka-update.pub` を設定ディレクトリの `asuka/update-keys` に書き出し（`--out` で別の場所を指定できます）、`asuka.toml` に追加する `[updates]` テーブル（`publicKey` と `github`）を表示します。秘密鍵はリポジトリに入れず、パスワードマネージャーと CI のシークレットに保管してください。インストール済みのアプリはこの鍵で署名されたアップデートしか受け付けないため、鍵をなくすとアプリを更新できなくなります。また、鍵を手に入れた人は誰でも、ユーザーにコードを配信できてしまいます。

```toml
[updates]
publicKey = "…the contents of asuka-update.pub…"
github = "you/notes"
```

| キー | 内容 |
|---|---|
| `publicKey` | アプリに組み込まれる、`asuka-update.pub` の中身 |
| `github` | `owner/name`：`v1.2.0` のようなタグの付いたリリースにアップデートを置く、公開リポジトリ。アプリは最新リリースのものを読みます |
| `tagPrefix` | リリースのタグの接頭辞（デフォルトは `v`）。`desktop-v` のように別の接頭辞にすると、リポジトリの最新リリースを別のもの（CLI など）のリリースにできます。その場合アプリは、GitHub API でこの接頭辞の付いた最新のリリースを探します |
| `url` | `github` の代わりに指定する、ファイルを配信する HTTPS のディレクトリ（S3 や R2 のバケット、CDN、自分のサーバー）。アプリは `url/update-<target>.json` を読みます |
| `feed` | アプリが代わりに問い合わせる自分のサーバー（ファイルの置き場所は `url` で指定）。[独自のアップデートサーバー](#独自のアップデートサーバー)を参照 |
| `s3` | `url` を配信するバケットで、`asuka publish` のアップロード先：`bucket`、`prefix`（バケット内のディレクトリ）、`region`、`endpoint`（R2 や MinIO のもの。デフォルトは Amazon S3）、`pathStyle` |
| `privateKey` | 秘密鍵のパス（`~/` はホームディレクトリ）。自分のマシンでビルドするときに使います |
| `changelog` | Markdown ファイル。その `## 1.2.0`（または Keep a Changelog 形式の `## [1.2.0] - 2026-11-13`）セクションがリリースノートになります。デフォルトは `CHANGELOG.md` で、このファイルがある場合は、そのバージョンのセクションが必要です |
| `deltas` | 何バージョン前まで差分を作るか（デフォルトは 3、0 なら作りません） |

CI やテストでは、環境変数でも同じ指定ができます。`ASUKA_UPDATE_PRIVATE_KEY`（秘密鍵そのもの、またはそのファイルのパス）と、`ASUKA_UPDATE_PUBLIC_KEY`、`ASUKA_UPDATE_URL`（それぞれ `publicKey` と `url` より優先されます）です。

### アプリ側の実装

`platform/update` の `UpdateWindow` がアップデート用のウィンドウです。アプリのメインウィンドウに一度だけ置けばよく、場所は取りません。`AppMenu` は、アプリのメニューに「アップデートを確認…」を追加します。Mac では「（アプリ名）について」の次に入り、Windows と Linux では「ヘルプ」メニューに入ります。`CheckForUpdates` はその項目だけなので、独自のメニューやトレイアイコンのメニューに置けます。

```asuka
import platform/update

app {
  Window {
    title: "Notes"
    update.UpdateWindow { icon: "logo.png" }  // assets/ にある画像
    MenuBar { update.AppMenu() }
    Text("Notes")
  }
}
```

アプリは起動の 10 秒後に確認し、その後は 1 日に 1 回確認します（`interval` で変えられます）。新しいバージョンが出ていれば、リリースノートとともに **アップデートをインストール**、**後で通知**、**このバージョンをスキップ** のボタンを表示します（スキップしたバージョンはバックグラウンドの確認では二度と提示されませんが、その次のバージョンは提示されます）。「今後はアップデートを自動的にダウンロードしてインストール」というチェックボックスもあり、これをオンにすると、確認で見つかったアップデートは尋ねずにインストールされ、次回の起動から新しいバージョンが動きます。インストール中はダウンロードの進捗を表示し、終わると **今すぐ再起動** を表示します。`update.checkForUpdates()` は、ユーザーが求めたときの確認（メニュー項目やボタンから）を行います。このときウィンドウは、アプリが最新であることや、アプリが自分で更新できないことも伝えます。`automaticChecks()`、`setAutomaticChecks(on)`、`automaticDownloads()`、`setAutomaticDownloads(on)`、`lastCheck()`、`skippedVersion()` はユーザーの選択（std/storage でデバイスに保存されます）で、設定画面を作るのに使えます。`UpdateWindow { automaticChecks: false }` にすると、ユーザーがオンにするまでバックグラウンドでは確認しません。ウィンドウの文言は Asuka が用意したもので、アプリの言語で表示されます。

アプリ独自の UI を作る場合は、`check()`、`Release.install`、`relaunch()` を使うか、アップデーターの状態 `update.updater` を読みます。これは store で、`phase` が確認中、利用可能、ダウンロード中（`downloaded` と `total` 付き）、準備完了などを表します。

```asuka
import platform/update

component UpdateBanner {
  state found: update.Release? = none
  state done = 0.0

  task {
    do {
      found = try await update.check()  // none なら最新の状態
    } catch e {
      print(e.message)
    }
  }

  if let r = found {
    Row {
      gap: 8
      Text("Notes {r.version} is out")  // r.notes は Markdown。r.date、r.size もあります
      Button("Install") {
        on press {
          do {
            try await r.install(progress: (got, total) => { done = float(got) / float(total) })
            update.relaunch()  // アプリを終了し（on quit で取り消せます）、新しいバージョンを起動します
          } catch e {
            print(e.message)
          }
        }
      }
      if done > 0 { Progress { value: done } }
    }
  }
}
```

`await update.unsupported()` は、アプリが自分自身を更新できない理由を返し、更新できるときは none を返します。更新できないのは、開発ビルドや `updates` のないビルド（`.notConfigured`）、システムのパッケージ（`.deb`、`.rpm`）・Flatpak・Snap・Mac App Store・Microsoft Store でインストールされたアプリ（`.managed`。更新はそれらが行います）、ディスクイメージから直接、またはダウンロードした場所から実行されていて macOS に App Translocation されている Mac アプリ（`.translocated`。ユーザーが「アプリケーション」フォルダーに移動する必要があります）、ユーザーが書き込めないフォルダーにあるアプリ（`.notWritable`）、そしてスマートフォン（`.notAvailable`）です。こうした場合、ウィンドウはバックグラウンドでは確認せず、ユーザーが確認したときにその理由を伝えます。

`update.setChannel("beta")` を呼ぶと、ベータ版を選んだユーザーに向けて、そのチャンネルのプレリリースも提示します。バージョンのチャンネルは、プレリリース部分の英字です（`1.4.0-beta.2` なら `beta`）。`"*"` を指定すると、すべてのプレリリースを受け取ります。正式リリースはどのチャンネルでも提示されます。`update.setFeed(url)` を使うと、`asuka.toml` とは別のフィード（ユーザーのアカウントごとに選んだものなど）を読みます。マニフェストの URL、GitHub のリリース、または `{target}`、`{version}`、`{channel}` を含むサーバーの URL を指定できます。その場合も、署名はいつもどおり検証されます。`update.compareVersions(a, b)` は、アップデーターと同じ規則でバージョンを比較します。

Web ではインストールするものはありません。`check()` はブラウザーのキャッシュを通さずにサイトの `version.json` を読み、新しいバージョンがあれば `relaunch()` がページを再読み込みします。

テストでは、スマートフォンと同じく update モジュールはありません。`update.fakeHost(update.FakeApp(…))` を呼ぶと、インストール済みのアプリとして応答し、フィードのファイルを返すようになります（`host.serve(update.Served(url:, file:))`）。`host.log()` で、何をダウンロードしてインストールしたかがわかります。

### ビルドと公開

`updates` と秘密鍵があれば、`asuka build --platform mac`、`windows`、`linux` は、ビルドの隣の `dist/updates` に次のものを書き出します。

- `notes-1.2.0-<target>.tar.gz`：インストールされた状態のアプリをまとめて署名したもの（SHA-256 に対する Ed25519 署名）。中身は、Mac ではバンドル、Windows ではフォルダーの中身で、Linux ではビルドの `.tar.gz` そのものです。ターゲットは `darwin-arm64`、`darwin-amd64`、`windows-amd64`、`windows-arm64`、`linux-amd64`、`linux-arm64` です。
- 以前に公開したバージョンからの差分アップデート（`notes-1.1.0-to-1.2.0-<target>.delta`）。ビルドは、公開済みのマニフェストが示す場所から以前のバージョンを（署名を確かめながら）ダウンロードし、変わった部分を書き出します。内容は、インストール済みのアプリから移動またはコピーするファイル、変更されたファイルの bsdiff パッチ、そして残りのファイルです。アプリは自分のバージョンからの差分だけをダウンロードし、それが失敗したとき（インストール後にアプリが変更されていた場合）はアーカイブ全体をダウンロードします。最初のバージョンには差分はありません。
- Linux では、ビルドした AppImage も署名付きで入ります。AppImage から実行しているアプリは、アーカイブの代わりにこれを使います（マニフェストの `appimage`）。以前に公開したバージョンの AppImage からの差分（`notes-1.1.0-to-1.2.0-linux-amd64.AppImage.delta`）も付き、アプリは自分のバージョンの差分を使って自分のファイルから新しい AppImage を作り、AppImage の署名で検証します。うまくいかなければ AppImage をまるごとダウンロードします。AppImageUpdate 用の `.zsync` も入ります。GitHub ではバージョン付きの名前（`notes-1.2.0-linux-amd64.AppImage.zsync`。AppImage は最新リリースの中からこれを探します）、`url` ではバージョンなしの名前（`notes-linux-amd64.AppImage.zsync`、チャンネルのものは `notes-linux-amd64-beta.AppImage.zsync`）で、マニフェストと一緒に最後にアップロードされます。独自の `tagPrefix` を使う場合、リポジトリの最新リリースは別のもの（CLI など）のことがあるので、AppImage にアップデート情報は入りません。
- `update-<target>.json`：マニフェスト。バージョン、リリースノート、日付、アーカイブと差分の URL・サイズ・署名、そして以前のバージョン（次の差分を作るため）を含みます。プレリリースのマニフェストは `update-<target>-beta.json` で、そのチャンネルのアプリだけが読みます。

秘密鍵がなければ、アプリをビルドするだけでアップデート用のファイルは書き出さず、その旨を表示します。`--version 1.2.0` を指定すると、`asuka.toml` とは別のバージョンとしてビルドします（CI ではタグからバージョンを取ります）。Windows のフォルダーと Linux のプレフィックスには `.asuka-files` も入ります。これはそのバージョンがインストールするファイルの一覧です。アップデートの際に、次のバージョンにはもうないファイルを削除し、それ以外のファイルには一切手を付けないために使います。

`asuka publish` はビルドしてから、これらを配布用のパッケージ（Mac の `.zip` と `.dmg`、Windows のインストーラー、`.zip`、`.msix`、Linux の `.deb`、`.rpm`、AppImage。Linux の `.tar.gz` はアップデートのアーカイブそのものです）と一緒にアップロードします。マニフェストは最後にアップロードするので、ファイルがまだ揃っていないマニフェストをアプリが読むことはありません。

```sh
asuka publish --platform mac --sign "Developer ID Application: …" --notarize asuka
asuka publish --platform mac --arch amd64 --sign "Developer ID Application: …" --notarize asuka
asuka publish --platform windows --arch arm64
asuka publish --platform linux --draft
```

- `github` の場合は、そのバージョンのタグ（`v1.2.0`）のリリースにアップロードします。[GitHub CLI](https://cli.github.com) にサインインしている必要があります（`gh auth login` または `GH_TOKEN`）。リリースがなければ下書きとして作り、そのあと公開します。プレリリースのバージョンならプレリリースとして公開し、独自の `tagPrefix` を使っている場合は最新リリース（latest）にはしません。`--draft` を指定すると下書きのまま残るので、すべてのプラットフォームが揃ってから公開できます（`gh release edit v1.2.0 --draft=false`）。アプリが確認するのは、公開済みの最新リリースです。
- `s3` の場合は、バケットにアップロードします。マニフェストは名前がバージョン間で変わらないので、`Cache-Control: no-cache` を付けます。認証には `AWS_ACCESS_KEY_ID`、`AWS_SECRET_ACCESS_KEY`（と `AWS_SESSION_TOKEN`）を使い、リージョンは `s3.region`、`AWS_REGION`、どちらもなければ `us-east-1`（R2 はこれを受け付けます）です。R2、MinIO などの互換サービスでは、その `s3.endpoint`（または `AWS_ENDPOINT_URL`）を指定します。ほかのツールは必要ありません。
- `url` だけの場合は、`dist/updates` を自分でそこにアップロードしてください。マニフェストは最後にアップロードし、以前のバージョンのファイルも残しておきます。

### 独自のアップデートサーバー

`feed` を指定すると、アプリは代わりに自分のサーバーに問い合わせます。その URL の `{target}`、`{version}`（アプリが実行中のバージョン）、`{channel}` には値が埋め込まれます。

```toml
[updates]
publicKey = "…"
url = "https://downloads.example.com/notes"
feed = "https://api.example.com/updates?target={target}&version={version}&channel={channel}"
```

サーバーは、新しいバージョンがなければ `204 No Content`（または 404）を返し、あればマニフェスト、つまり `asuka build` が書き出すのと同じ JSON を返します。

```json
{
  "version": "1.2.0",
  "notes": "- Faster sync.\n- Fixed the sidebar.",
  "date": "2026-11-13T09:00:00Z",
  "url": "https://downloads.example.com/notes/notes-1.2.0-darwin-arm64.tar.gz",
  "size": 9123456,
  "signature": "…base64…",
  "deltas": [
    {"from": "1.1.0", "url": "https://…/notes-1.1.0-to-1.2.0-darwin-arm64.delta", "size": 21432, "signature": "…"}
  ]
}
```

`asuka build` は `url` 用のファイルを書き出します。書き出されたものをそのまま配信しても、マニフェストを好きなように選んで返してもかまいません（段階的なロールアウト、アカウントごとのチャンネルなど）。署名は `asuka build` が作ったものなので、サーバーはどれを返すかを選ぶだけです。

### 各プラットフォームでの更新の仕組み

- macOS：新しいバンドルは検証されたうえで（コード署名が有効で、実行中のアプリと同じチームと識別子であることを確かめ、検疫属性を取り除きます）、アプリが終了するまでその隣で待機します。アプリがどのように終了しても、ヘルパーが古いバンドルを新しいバンドルに置き換えます。アプリが再起動を求めていた場合は Launch Services でアプリを開くので、Dock のアイコンは一つのままです。アプリのフォルダーは、ユーザーが書き込めるものである必要があります。管理者なら `/Applications`、または `~/Applications` です。
- Windows：アプリの実行中にフォルダーの中身が置き換えられます。各ファイルはまず名前を変えて脇に退避し、一つでも置き換えられなければすべて元に戻します。実行中の `.exe` は名前を変え（Windows はこれを許可しています）、次回の起動時に削除します。フォルダーはユーザーのもの（`Program Files` ではない場所）である必要があります。インストーラーがインストールしたもの（`%LOCALAPPDATA%\Programs\<Name>`）か、ユーザーのプロファイル内に展開した `.zip` です。更新後は、「インストールされているアプリ」に更新先のバージョンが表示されます。MSIX は、Microsoft Store かアプリ インストーラーを通じて更新されます。
- Linux：tarball のプレフィックス（`~/.local` など）にある、そのアプリのファイルだけ（`bin/notes`、`share/notes/`、デスクトップファイル、アイコン、metainfo、MIME タイプ）を一つずつリネームで置き換え、そのあとデスクトップのキャッシュを更新します（`update-desktop-database`、`update-mime-database`）。AppImage では、新しい AppImage をその隣に差分から作って（またはダウンロードして）検証し、リネームで元のファイルと置き換えます（ユーザーが付けた名前のまま。`~/Applications` のように、ユーザーが書き込めるフォルダーにあることが条件です）。再起動すると新しい AppImage が起動し、元の AppImage が展開して実行されていた（`--appimage-extract-and-run`）なら、新しいものも展開して実行されます。Flatpak、Snap、ディストリビューションのパッケージはそれぞれの仕組みで更新されるため、その場合アプリは自分自身を更新しません。

### スマートフォン：アプリのコードのアップデート

ストアのリリースのあいだ、スマートフォンのアプリ（と Apple TV のアプリ）は Expo のアップデートと同じように、コードを無線で更新できます。更新されるのはアプリの Asuka のコードで、ビルドにコンパイルされたコードの代わりにコアの VM が実行し、デスクトップのアップデートと同じ鍵で署名されます。asuka.toml に `[updates]` を書いて作ったビルドがアップデートを受け取ります。

```sh
asuka build --platform ios --update      # dist/updates に、署名したアップデートとそのマニフェストを書き出す
asuka publish --platform ios --update    # 作って、[updates] の指定先にアップロードする
```

`--platform android` は Android のアップデートを作り、Android TV もそれを受け取ります（同じビルドだからです）。`--platform tvos` は Apple TV のアップデートを `dist/tvos/updates` に作ります。アップデートはプラットフォームごとに別々です。アップデートは一つのプラットフォームのビルドのために作られ、マニフェストもそのプラットフォームの名前になる（`update-tvos-<フィンガープリント>.json`）ので、iPhone が Apple TV のアップデートを受け取ることも、その逆もありません。変更を届けたいプラットフォームごとに公開してください。

アップデートは、それを作るときに基にしたネイティブ部分と同じビルドにだけ合います。つまりビルドの*フィンガープリント*で、CLI のバージョン、プラットフォーム、ビルドが持つホストの部品（`Video` のプレーヤー、`WebView` の WebKit など、アプリが表示するもの）、アプリが読み込むパッケージとアプリ自身のネイティブコード、ビルドがリンクする組み込み関数、アプリのアセット、asuka.toml（バージョンとビルド番号を除く）から計算されます。Apple TV のビルドのフィンガープリントは iPhone のものと異なります。ホストの部品が少なく、tvOS を対象に挙げたパッケージだけが組み込まれるからです。それ以上を必要とするアップデート（新しいネイティブパッケージやアセット、権限、ビルドが表示していなかった要素）は次のストアビルドのためのものです。そのマニフェストにはインストール済みのどのビルドも持たないフィンガープリントが書かれるので、どの端末にも届きません。

アプリでは、`update.UpdateWindow()` が起動して少ししてから、その後は 1 日に一度確認し、見つけたアップデートをダウンロードして、次の起動で実行します。スマートフォンではウィンドウは表示しません。`update.check()`、`Release.install()`、`relaunch()`（すぐに実行します）で手動でも行え、`update.version()` は実行中のバージョンを返します（`1.4.0+1764150000`。`+` の後ろがアップデートの番号です）。あるアップデートの起動が 2 回続けて最初の数秒で終わると（起動時のクラッシュはそうなります）、ビルド自身のコードに戻り、そのアップデートは二度と受け取りません。Apple TV では、アップデートはアプリのキャッシュに保存され、アプリが動いていないあいだに tvOS が消すことがあります。そのときはアップデートをもう一度ダウンロードするまで、ビルド自身のコードで動きます。

次のストアビルドがコンパイルするまで、アップデートのコードはコンパイルされたコードより少し遅く動きます（遅いのはアプリ自身の処理だけで、レイアウト、シグナル、ビューはネイティブのままです）。ストアの規則を守ってください。Apple は、アプリの主な目的を変えない解釈実行されるコードのダウンロードを認めていますが、App Review は審査で見ていない機能を禁じています（2.5.2、2.5.19）。Google Play はインタープリターで動くコードを認めています。アップデートは修正や小さな変更のためのものです。

## 既存アプリへの組み込み

Asuka のアプリは、Xcode や Android Studio で作ったアプリの中に組み込めます。Asuka で一度だけ書いた機能を、ネイティブアプリから表示できるということです。（Web では、ビルドしたアプリを自分のページの要素の中で動かせます。[Web](web.md) を参照してください。）

**iOS。** `asuka build --platform ios --xcframework` は `dist/AsukaApp.xcframework` を作ります。中身は、アプリ本体（実機とシミュレーターの両方向け）、Swift のモジュールインターフェース、アプリのアセットです。これをターゲットに追加し（Embed & Sign）、ほかのビューコントローラーと同じように Asuka アプリを表示します。

```swift
import AsukaApp

present(AsukaViewController(), animated: true)
// push したり、タブにしたり、ウィンドウのルートにしたりもできます
```

`std/sqlite` を使うアプリでは、SQLite はこのフレームワークの中にあり、そのシンボルはフレームワークの中に閉じています。あなたのアプリが SQLite を持っていても干渉しません。

**Android。** `asuka build --platform android --aar` は `dist/maven` に Maven リポジトリを作ります。中身は、ABI ごとのアプリのライブラリ、アセット、宣言済みの `AsukaActivity` で、依存関係（Material、AndroidX）は POM に記載されています。その隣には、アプリが使うオプションのモジュール（`platform/push`、`platform/maps`）も置かれます。`std/sqlite` を使うアプリでは、SQLite はアプリのライブラリに入っています。リポジトリと、コマンドが表示する依存関係をプロジェクトに追加し、Activity を起動します。

```kotlin
// settings.gradle.kts
dependencyResolutionManagement { repositories { maven { url = uri("path/to/dist/maven") } } }
// app/build.gradle.kts
dependencies { implementation("dev.asuka.counter:counter:1.0.0") }
// 任意の場所で
startActivity(Intent(this, dev.asuka.AsukaActivity::class.java))
```

組み込まれたアプリは一つの Asuka アプリで、単体のビルドと同じく一度だけマウントされます。`asuka.toml` がアプリ全体に対して設定するもの（アイコン、URL スキーム、共有ターゲット、ウィジェット、パーミッションの説明文）は、組み込む側のアプリが自分のプロジェクトで設定します。

## クラッシュレポート用のシンボル

ネイティブのクラッシュのフレームは、アプリのバイナリ内のアドレスです。クラッシュレポートのサービス（あるいは App Store Connect や Google Play）は、ビルドのシンボルを使ってこれを関数名と行番号に変換します。アプリが捕捉しなかったエラーは、シンボルの有無にかかわらず、Asuka がそのメッセージとともに報告します。

**iOS。** `asuka build --platform ios` は、アプリをデバッグ情報付きでビルドし、Xcode のアーカイブと同じように、各バイナリ（アプリ本体、共有シートがある場合はそのフレームワーク、拡張機能）の dSYM を作り、そのあとバイナリからシンボルを取り除きます。dSYM は `.ipa`（またはシミュレーター用の `.app`）の隣の `dist/<Name>.dSYMs` に置かれ、シンボルは `.ipa` の中（`Symbols/`）にも入ります。そのため、App Store Connect と Xcode の Organizer は、表示するクラッシュをシンボル化できます。dSYM は、利用しているクラッシュレポートのサービスにアップロードしてください。

```sh
sentry-cli debug-files upload --org ORG --project PROJECT dist/Notes.dSYMs
# Crashlytics のツール（SDK に含まれています）：
upload-symbols -gsp GoogleService-Info.plist -p ios dist/Notes.dSYMs
```

**Android。** `asuka build --platform android` は、アプリのシンボルテーブルを Gradle のために残します。Gradle はパッケージするライブラリからシンボルを取り除き、そのシンボルを、R8 が名前を変えた Kotlin のマッピングとともに `.aab` に入れます。Google Play はこれらを使って、ネイティブのクラッシュと ANR をシンボル化し、スタックトレースの難読化を解除します。クラッシュレポートのサービス向けには、`.aab` の隣にも `dist/<Name>-native-debug-symbols.zip` と `dist/<Name>-mapping.txt` が置かれます。

```sh
sentry-cli debug-files upload --org ORG --project PROJECT dist/Notes-native-debug-symbols.zip
sentry-cli upload-proguard --org ORG --project PROJECT dist/Notes-mapping.txt
```

**Mac、Windows、Linux、Web。** アプリはデバッグ情報なしでビルドされ、Windows と Linux ではシンボルテーブルも除かれます（Mac のバイナリはグローバルなシンボルを残します）。クラッシュレポートの各フレームはバイナリとオフセットで、バイナリに名前があれば名前も付きます。アップロードするシンボルはありません。

リリースごとのシンボルは保管しておいてください。クラッシュは、それが起きたビルドのシンボルでシンボル化されます。レポートそのものについては[デバイスとサービス](device.md#クラッシュレポートplatformcrash)を参照してください。

## 再現可能なビルド

リリースビルドは再現可能です。同じソースを、同じバージョンの Asuka と同じコンパイラー（Xcode、Android NDK、Zig、システムの C コンパイラー）でビルドすれば、どのプラットフォームでも、どのコンピューターでビルドしても、プロジェクトやチェックアウトがどこにあっても、いつビルドしても、まったく同じバイト列になります。そのため、アップデートの差分には実際に変わった部分だけが入り（AppImage の差分、デスクトップアプリのアップデート、スマートフォンでのアプリのコードのアップデート）、リリースが本当にそのソースから作られたものかどうかは、誰でももう一度ビルドして比べれば確かめられます。

リリースビルドに記録されるファイル名は、ビルドしたコンピューター上のパスではなく、プロジェクトでの名前（`app.asuka`、`std/http/http.asuka`）です。エラーのトレースにもこの名前が表示されます。また、ビルドが日時を書き込むもの（`.tar.gz`、`.deb`、`.rpm`、AppImage、`.zip` の中のファイル、Linux アプリの metainfo の release、Web の `version.json`、スマートフォンのビルドがコードのアップデートの新旧を判断する基準の時刻）には、[reproducible-builds.org](https://reproducible-builds.org/docs/source-date-epoch/) の取り決めどおり `SOURCE_DATE_EPOCH`（1970 年からの秒数）が使われます。設定されていなければ、アプリのある git リポジトリの最後のコミットの時刻、リポジトリの外なら、アプリのファイルのうち最も新しい更新時刻です。ビルドを実行した時刻が使われることはありません。リポジトリのないソース（タグのアーカイブなど）からビルドするときは設定してください。

```sh
SOURCE_DATE_EPOCH=1767225600 asuka build --platform linux
sha256sum dist/linux-amd64/*.deb    # このバージョンをどこでビルドしても同じ
```

ビルドのたびに変わるのは、作られた時刻を記録するもののうち、ビルドの側では決められないものです。

- **Apple の署名。** 証明書による署名（`.ipa`、Apple Development 証明書で署名したシミュレーター用ビルド、Developer ID で署名した Mac アプリ）には署名した時刻が記録され、公証では Apple のチケットも加わります。アドホック署名（`--sign` なしの Mac ビルド）には時刻がありません。署名済みのバイナリーを比べるときは、コピーから `codesign --remove-signature` で署名を取り除いて比べてください。
- **Mac の `.dmg`。** `hdiutil` はディスクイメージのたびに新しい識別子と独自の日時を付けます。隣にある `.zip` は再現可能です。
- **Windows の署名**のうち、タイムスタンプサーバーが時刻を付けたもの（`ASUKA_WINDOWS_TIMESTAMP`）。
- **Linux の OpenPGP 署名**。署名した `.rpm` と AppImage、apt と dnf のリポジトリのメタデータの署名には、署名した時刻が記録されます。

## リリース前の確認

リリースする前に、ユーザーが使うのと同じ環境でリリースビルドを動かしてみてください。

- スマートフォン：iOS は `--distribution development` でビルドしたもの、Android は Play の内部テストトラックから入れたもの（`asuka publish --platform android --track internal`）です。後者では、Play が `.aab` から作る APK がインストールされます。TestFlight と Play の内部テストでは、シミュレーターでは確かめられないことも確認できます。
- Mac：署名・公証済みの `.dmg` を別の Mac でダウンロードし、アプリを「アプリケーション」フォルダーにドラッグして試します（Gatekeeper が確認するのはダウンロードしたものです）。`spctl --assess --type execute -vv Notes.app` で、Gatekeeper がどう判断するかがわかります。
- Windows と Linux：フォルダー、Linux では `.deb`、`.rpm`、AppImage、`.tar.gz`、Flatpak を、そのアプリを一度も入れたことのないマシンで試します（Linux では GTK 4.14 と libadwaita 1.5 以降のあるマシン）。
- Web：ビルドしたサイトを HTTPS で配信し、サポートする各ブラウザーで試します。

## CI での配布

次の GitHub Actions のワークフローは、`v1.2` のようなタグを push すると両方のアプリをビルドし、`asuka publish` で iOS のビルドを TestFlight に、Android のビルドを Play の内部テストトラックにアップロードします。さらに、そのタグの GitHub リリースの下書きに、`.ipa` と `.aab` をそれぞれアップロードが済みしだい添付します。準備ができたら下書きを公開してください。（手動で実行した場合、ビルドはその実行のアーティファクトとして残るだけです。）アプリがリポジトリのルートにあることを前提としています。最初のアップロードは手動で行ってください。Play は、アプリに一度もアップロードがないうちは API からのアップロードを受け付けません。

```yaml
# .github/workflows/release.yml
name: release
on:
  push:
    tags: ["v*"]
  workflow_dispatch:
permissions:
  contents: write # タグのリリースにビルドを添付するため
env:
  ASUKA_VERSION: 0.1.0 # アプリが使う Asuka のバージョン

jobs:
  release:
    if: startsWith(github.ref, 'refs/tags/')
    runs-on: ubuntu-latest
    steps:
      - run: gh release create "$GITHUB_REF_NAME" --draft --generate-notes --verify-tag
        env:
          GH_TOKEN: ${{ github.token }}
          GH_REPO: ${{ github.repository }}

  ios:
    needs: release
    if: ${{ !failure() && !cancelled() }} # 手動で実行して release がスキップされたときも動かす
    runs-on: macos-latest
    steps:
      - uses: actions/checkout@v4
      - name: the asuka CLI, at the app's version of Asuka
        run: |
          curl -fsSL https://asuka.egoist.dev/install.sh | sh
          echo "$HOME/.asuka/bin" >> "$GITHUB_PATH"
      - name: the distribution certificate and the profile
        env:
          CERTIFICATE: ${{ secrets.IOS_CERTIFICATE }}
          CERTIFICATE_PASSWORD: ${{ secrets.IOS_CERTIFICATE_PASSWORD }}
          PROFILE: ${{ secrets.IOS_PROFILE }}
        run: |
          KC=$RUNNER_TEMP/signing.keychain-db
          PW=$(openssl rand -hex 16)
          security create-keychain -p "$PW" "$KC"
          security set-keychain-settings -lut 3600 "$KC"
          security unlock-keychain -p "$PW" "$KC"
          echo "$CERTIFICATE" | base64 --decode > $RUNNER_TEMP/certificate.p12
          security import $RUNNER_TEMP/certificate.p12 -k "$KC" -P "$CERTIFICATE_PASSWORD" -T /usr/bin/codesign
          security set-key-partition-list -S apple-tool:,apple: -k "$PW" "$KC"
          security list-keychains -d user -s "$KC" $(security list-keychains -d user | tr -d '"')
          echo "$PROFILE" | base64 --decode > $RUNNER_TEMP/app.mobileprovision
          # shareTarget を使う場合は、拡張機能のプロファイルを asuka が探す場所にインストールする：
          # mkdir -p ~/Library/MobileDevice/Provisioning\ Profiles
          # echo "$SHARE_PROFILE" | base64 --decode > ~/Library/MobileDevice/Provisioning\ Profiles/share.mobileprovision
      - name: build, and upload to TestFlight
        env:
          ASUKA_ASC_KEY_ID: ${{ secrets.ASC_KEY_ID }}
          ASUKA_ASC_ISSUER_ID: ${{ secrets.ASC_ISSUER_ID }}
          ASUKA_ASC_KEY: ${{ secrets.ASC_KEY }}
        run: asuka publish --platform ios --profile $RUNNER_TEMP/app.mobileprovision --no-wait
      - uses: actions/upload-artifact@v4
        with: { name: ios, path: dist/*.ipa }
      - name: attach to the tag's release
        if: startsWith(github.ref, 'refs/tags/')
        env:
          GH_TOKEN: ${{ github.token }}
          GH_REPO: ${{ github.repository }}
        run: gh release upload "$GITHUB_REF_NAME" dist/*.ipa --clobber

  android:
    needs: release
    if: ${{ !failure() && !cancelled() }}
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-java@v4
        with: { distribution: temurin, java-version: 17 }
      - uses: android-actions/setup-android@v3
        with: { packages: platform-tools }
      - name: an NDK
        run: sdkmanager "ndk;27.1.12297006"
      - name: the asuka CLI, at the app's version of Asuka
        run: |
          curl -fsSL https://asuka.egoist.dev/install.sh | sh
          echo "$HOME/.asuka/bin" >> "$GITHUB_PATH"
      - name: the upload key
        env:
          KEYSTORE: ${{ secrets.ANDROID_KEYSTORE }}
        run: echo "$KEYSTORE" | base64 --decode > $RUNNER_TEMP/upload.jks
      - name: build, and upload to Play's internal test track
        run: asuka publish --platform android --track internal
        env:
          ASUKA_KEYSTORE: ${{ runner.temp }}/upload.jks
          ASUKA_KEYSTORE_PASSWORD: ${{ secrets.ANDROID_KEYSTORE_PASSWORD }}
          ASUKA_PLAY_SERVICE_ACCOUNT: ${{ secrets.PLAY_SERVICE_ACCOUNT }}
      - uses: actions/upload-artifact@v4
        with:
          name: android
          path: dist/*.aab
      - name: attach to the tag's release
        if: startsWith(github.ref, 'refs/tags/')
        env:
          GH_TOKEN: ${{ github.token }}
          GH_REPO: ${{ github.repository }}
        run: gh release upload "$GITHUB_REF_NAME" dist/*.aab --clobber
```

ワークフローが使うシークレット（Settings › Secrets and variables › Actions）は次のとおりです。ファイルは base64 にして登録します（`base64 -i file | pbcopy`）。

- `IOS_CERTIFICATE`：Apple Distribution 証明書とその秘密鍵を、「キーチェーンアクセス」から `.p12` として書き出したもの。`IOS_CERTIFICATE_PASSWORD` は、書き出すときに付けたパスワードです。
- `IOS_PROFILE`：developer.apple.com で取得した、アプリの App Store 用プロビジョニングプロファイル（`shareTarget` を使う場合は、`SHARE_PROFILE` に共有拡張機能のプロファイルを入れます）。
- `ASC_KEY_ID`、`ASC_ISSUER_ID`、`ASC_KEY`（`.p8` の中身）：「ユーザとアクセス」›「統合」で作成した、「App マネージャ」の役割を持つ App Store Connect API キー。
- `ANDROID_KEYSTORE`：アップロード鍵の `.jks`。`ANDROID_KEYSTORE_PASSWORD` はそのパスワードです（鍵のパスワードが異なる場合は `ASUKA_KEY_PASSWORD` も追加します）。
- `PLAY_SERVICE_ACCOUNT`：Google Cloud のサービス アカウントの JSON キー。このアカウントは、Play Console の「ユーザーと権限」で、テストトラックにリリースする権限を付けて招待しておきます。

CLI は自分のバージョンの Asuka でアプリをビルドするため、アプリが使う Asuka のバージョン（インストールスクリプトが読む `ASUKA_VERSION`）に合わせてインストールします。アプリを新しい Asuka に移すときは、この値も上げてください。各ビルドの番号はストア上の最大の番号の次になるので、タグを付ける前に番号を上げる必要はありません。`--no-wait` を付けているので、iOS のジョブは Apple の処理が終わるのを待たず、Apple がアップロードを受け取った時点で終わります。App Store は最近の Xcode で作ったビルドしか受け付けません。ランナーのデフォルトの Xcode が古い場合は、`sudo xcode-select -s /Applications/Xcode_<version>.app` で選んでください。

### デスクトップ

次のジョブを同じワークフローに加えると、タグに対してデスクトップアプリをビルドし、そのリリースの下書きに添付します。Mac 版は Apple シリコン向けと Intel 向けをそれぞれビルドし、Developer ID で署名して公証し、Windows 版は x64 と Arm 向けにビルドして署名し、Linux 版は amd64 と arm64 向けにビルドします。Windows 版と Linux 版はどちらも Linux 上でビルドできるので、一つの Ubuntu ランナーで両方を作れます。（ここではアプリの名前を Notes としています。ファイル名は `asuka.toml` の `name` をもとに付けられます。）

```yaml
  mac:
    needs: release
    runs-on: macos-latest
    steps:
      - uses: actions/checkout@v4
      - name: the asuka CLI, at the app's version of Asuka
        run: |
          curl -fsSL https://asuka.egoist.dev/install.sh | sh
          echo "$HOME/.asuka/bin" >> "$GITHUB_PATH"
      - name: the Developer ID certificate
        env:
          CERTIFICATE: ${{ secrets.MAC_CERTIFICATE }}
          CERTIFICATE_PASSWORD: ${{ secrets.MAC_CERTIFICATE_PASSWORD }}
        run: |
          KC=$RUNNER_TEMP/signing.keychain-db
          PW=$(openssl rand -hex 16)
          security create-keychain -p "$PW" "$KC"
          security set-keychain-settings -lut 3600 "$KC"
          security unlock-keychain -p "$PW" "$KC"
          echo "$CERTIFICATE" | base64 --decode > $RUNNER_TEMP/certificate.p12
          security import $RUNNER_TEMP/certificate.p12 -k "$KC" -P "$CERTIFICATE_PASSWORD" -T /usr/bin/codesign
          security set-key-partition-list -S apple-tool:,apple: -k "$PW" "$KC"
          security list-keychains -d user -s "$KC" $(security list-keychains -d user | tr -d '"')
      - name: build, sign and notarize
        env:
          ASUKA_APPLE_ID: ${{ secrets.APPLE_ID }}
          ASUKA_APPLE_PASSWORD: ${{ secrets.APPLE_APP_PASSWORD }}
          ASUKA_TEAM_ID: ${{ secrets.APPLE_TEAM_ID }}
        run: |
          asuka build --platform mac --sign "Developer ID Application: Your Name (TEAMID)" --notarize env
          asuka build --platform mac --arch amd64 --sign "Developer ID Application: Your Name (TEAMID)" --notarize env
      - name: attach to the tag's release
        env:
          GH_TOKEN: ${{ github.token }}
          GH_REPO: ${{ github.repository }}
        run: gh release upload "$GITHUB_REF_NAME" dist/*/*.zip dist/*/*.dmg --clobber

  windows-linux:
    needs: release
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: mlugg/setup-zig@v2 # アプリをクロスコンパイル
      - name: the asuka CLI, at the app's version of Asuka
        run: |
          curl -fsSL https://asuka.egoist.dev/install.sh | sh
          echo "$HOME/.asuka/bin" >> "$GITHUB_PATH"
      - name: osslsigncode, and the code signing certificate
        env:
          CERTIFICATE: ${{ secrets.WINDOWS_CERTIFICATE }}
        run: |
          sudo apt-get install -y osslsigncode
          echo "$CERTIFICATE" | base64 --decode > $RUNNER_TEMP/cert.pfx
      - name: Windows, x64 and Arm
        env:
          ASUKA_WINDOWS_CERT: ${{ runner.temp }}/cert.pfx
          ASUKA_WINDOWS_CERT_PASSWORD: ${{ secrets.WINDOWS_CERTIFICATE_PASSWORD }}
        run: |
          asuka build --platform windows
          asuka build --platform windows --arch arm64
      - name: Linux, amd64 and arm64
        run: |
          asuka build --platform linux
          asuka build --platform linux --arch arm64
      - name: attach to the tag's release
        env:
          GH_TOKEN: ${{ github.token }}
          GH_REPO: ${{ github.repository }}
        run: gh release upload "$GITHUB_REF_NAME" dist/*/*.zip dist/*/*.tar.gz dist/*/*.deb dist/*/*.rpm dist/*/*.AppImage --clobber
```

これらのジョブが使うシークレットは次のとおりです。`MAC_CERTIFICATE` は Developer ID Application 証明書とその秘密鍵（`.p12`）、`MAC_CERTIFICATE_PASSWORD` はそのパスワードです。`APPLE_ID`、`APPLE_APP_PASSWORD`（account.apple.com で発行する App 用パスワード）、`APPLE_TEAM_ID` は公証サービスに使います。`WINDOWS_CERTIFICATE` はコード署名証明書（`.pfx`）、`WINDOWS_CERTIFICATE_PASSWORD` はそのパスワードです。どのビルドも、自分専用のディレクトリ（`dist/darwin-arm64`、`dist/windows-amd64`、`dist/linux-arm64`）にファイルを作ります。ファイル名にはアプリ名、バージョン、プラットフォーム、アーキテクチャが入るので、リリースにすべて並べて置けます。Linux の各ビルドの Flatpak マニフェストは、それぞれ自分のビルドをパッケージします。

[自動アップデート](#自動アップデート)を GitHub で配信する場合は、`asuka build` の代わりに `asuka publish` を使うと、各プラットフォームのアップデートもタグのリリースの下書き（`release` ジョブが作ったもの）にアップロードされ、すべてのプラットフォームが揃ってから公開できます。秘密鍵をシークレットとして追加し（`gh secret set ASUKA_UPDATE_PRIVATE_KEY < asuka-update.key`）、ジョブに `ASUKA_UPDATE_PRIVATE_KEY` と `GH_TOKEN` を渡して、タグのバージョンでビルドします。

```yaml
      - name: build, sign, notarize, and upload with its update
        env:
          ASUKA_UPDATE_PRIVATE_KEY: ${{ secrets.ASUKA_UPDATE_PRIVATE_KEY }}
          GH_TOKEN: ${{ github.token }}
          ASUKA_APPLE_ID: ${{ secrets.APPLE_ID }}
          ASUKA_APPLE_PASSWORD: ${{ secrets.APPLE_APP_PASSWORD }}
          ASUKA_TEAM_ID: ${{ secrets.APPLE_TEAM_ID }}
        run: asuka publish --platform mac --draft --version "${GITHUB_REF_NAME#v}" --sign "Developer ID Application: Your Name (TEAMID)" --notarize env
```

Windows と Linux のジョブでは次のようにします。

```yaml
        run: |
          for p in "windows" "windows --arch arm64" "linux" "linux --arch arm64"; do
            asuka publish --platform $p --draft --version "${GITHUB_REF_NAME#v}"
          done
```

最後に下書きを公開すると（`gh release edit v1.2.0 --draft=false`、またはリリースのページから）、インストール済みのアプリがそのバージョンに更新されます。release ジョブのタグは、`tagPrefix` にバージョンを続けたもの（`v1.2.0`）です。`--version` でビルドのバージョンをタグのバージョンに合わせるので、`asuka.toml` のバージョンを上げる必要はありません。差分は公開済みのリリースをもとに作られ、ランナーがそのリリースをダウンロードします（`GH_TOKEN` があると、GitHub API のリクエスト数の制限が緩和されます。`tagPrefix` を使う場合は GitHub API を使います）。

`.rpm` と AppImage に署名して [apt と dnf のリポジトリ](#署名付きパッケージとリポジトリ)に書き込む Linux のジョブには、OpenPGP 鍵（`gh secret set ASUKA_LINUX_GPG_KEY < asuka-linux.key`）とバケットの認証情報も渡します。

```yaml
        env:
          ASUKA_LINUX_GPG_KEY: ${{ secrets.ASUKA_LINUX_GPG_KEY }}
          AWS_ACCESS_KEY_ID: ${{ secrets.R2_ACCESS_KEY_ID }}
          AWS_SECRET_ACCESS_KEY: ${{ secrets.R2_SECRET_ACCESS_KEY }}
        run: |
          for a in amd64 arm64; do
            asuka publish --platform linux --arch $a --draft --version "${GITHUB_REF_NAME#v}"
          done
```

## 言語

[カタログ](i18n.md)を持つアプリでは、ほかに何もする必要はありません。`asuka build` がアプリの言語を各ストアとシステムに宣言するので（iOS と macOS では `CFBundleLocalizations` と `.lproj`、Android では `locales_config`）、「設定」でアプリの言語を選べるようになります。Windows、Linux、Web のビルドは何も宣言しません。アプリは、ユーザーの設定に従ってカタログから選ばれた言語で表示されます。アプリの名前と利用目的の説明文も、カタログで `asuka.appName` や `asuka.NSCameraUsageDescription` といったキーを使って翻訳できます（[システムが表示するもの](i18n.md#システムが表示するもの)）。
