# デバイスとサービス

Asuka のパッケージを使うと、ビューの外にあるデバイスの機能も扱えます。カメラ、バーコードや QR コード、画像に写っているものの認識、写真とファイル、プレビュー、印刷と PDF、メールとメッセージの作成画面、位置情報、通知とプッシュ通知、サウンド、再生中の情報（Now Playing）と録音、ネットワークの状態、写真やファイルのライブラリへの保存、バックグラウンドでの処理、ほかのアプリからの共有、JavaScript、サインインと生体認証、画面、地図、ストアのサービス、広告、クラッシュレポート、アナリティクスです。このページでは、パッケージごとに何ができるか、その API、必要になる権限、プラットフォームごとの動作を説明します。

どのパッケージも、すべてのプラットフォームで同じ API を持ちます。iOS と Android ではほぼすべての機能が使えます。macOS、Windows、Linux では、Asuka が各パッケージをデスクトップ自身のサービスで実装し、ブラウザーでは Web の API で実装します。プラットフォームに対応する機能がない場合、パッケージはその機能を持たないスマートフォンと同じように振る舞います。つまり、パッケージの `notAvailable` エラーになるか、足りないものを示すエラーになるか、何もしない呼び出しになります。各パッケージの節では、何ができるか、各プラットフォームが何を使ってそれを実現しているかを説明します。さらに、各デスクトップとブラウザーにはそれぞれ専用の節があり（[Mac の場合](#mac-の場合)、[Windows の場合](#windows-の場合)、[Linux の場合](#linux-の場合)、[ブラウザーの場合](#ブラウザーの場合)）、そこで必要な設定と、iOS や Android との違いを表にまとめています。

## 権限と利用目的の説明文

アプリがパッケージをインポートすると、そのパッケージに必要なものが自動で加わります。Android ではマニフェストの権限、iOS では Info.plist の利用目的の説明文（purpose string）、そしてプラットフォーム側のコードそのものです。インポートしていないパッケージのものは加わらないので、写真アプリがマイクの許可を求めることはありません。Mac アプリにも同じ説明文と、それに必要なエンタイトルメントが加わります。Windows、Linux、Web では何も宣言する必要はありません（下のそれぞれの節を参照してください）。

| パッケージ | Android | iOS |
| --- | --- | --- |
| `platform/camera` | `CAMERA`、`RECORD_AUDIO` | `NSCameraUsageDescription`、`NSMicrophoneUsageDescription` |
| `platform/location` | `ACCESS_COARSE_LOCATION`、`ACCESS_FINE_LOCATION`、`ACCESS_BACKGROUND_LOCATION`、`FOREGROUND_SERVICE_LOCATION`、`RECEIVE_BOOT_COMPLETED`。パッケージのフォアグラウンドサービスとそのレシーバー（バックグラウンドでの更新、リージョン） | `NSLocationWhenInUseUsageDescription`、`NSLocationAlwaysAndWhenInUseUsageDescription`、`location` バックグラウンドモード。`requestPrecise` を呼ぶアプリでは `NSLocationTemporaryUsageDescriptionDictionary` |
| `platform/notify` | `POST_NOTIFICATIONS` | |
| `platform/network` | `ACCESS_NETWORK_STATE` | |
| `platform/library` | `WRITE_EXTERNAL_STORAGE`（Android 9 以前） | `NSPhotoLibraryAddUsageDescription`。アプリの Documents を「ファイル」アプリに表示 |
| `platform/background` | `FOREGROUND_SERVICE`、`FOREGROUND_SERVICE_DATA_SYNC`、`POST_NOTIFICATIONS`、`WAKE_LOCK`。フォアグラウンドサービス | |
| `platform/scan` | ML Kit のバーコードモデル（約 2 MB） | |
| `platform/vision` | ML Kit のテキスト、顔、バーコードのモデル（約 30 MB） | |
| `platform/photos` | `READ_MEDIA_IMAGES`、`READ_MEDIA_VISUAL_USER_SELECTED`。`READ_EXTERNAL_STORAGE`（Android 12 以前） | `NSPhotoLibraryUsageDescription` |
| `platform/push` | Firebase Cloud Messaging、`POST_NOTIFICATIONS` | `aps-environment` エンタイトルメント。`remote-notification` バックグラウンドモード |
| `platform/analytics` | Firebase Analytics（`INTERNET`、`ACCESS_NETWORK_STATE`、`WAKE_LOCK`、`AD_ID`）。`android.googleServices` の設定 | Firebase Analytics。`ios.googleServices` が指すファイル。アプリの起動時から |
| `platform/appleauth` | | Sign in with Apple のエンタイトルメント |
| `platform/pay` | Google Pay のライブラリ、その API の有効化、コールバック用のサービス | Apple Pay のエンタイトルメント（`asuka.toml` の `applePay.merchantIds` とともに） |
| `platform/biometric` | `USE_BIOMETRIC` | `NSFaceIDUsageDescription` |
| `platform/record` | `RECORD_AUDIO`、`FOREGROUND_SERVICE_MICROPHONE`。フォアグラウンドサービス | `NSMicrophoneUsageDescription` |
| `platform/nowplaying` | `FOREGROUND_SERVICE`、`FOREGROUND_SERVICE_MEDIA_PLAYBACK`。フォアグラウンドサービス | `audio` バックグラウンドモード |
| `platform/liveactivity` | `POST_NOTIFICATIONS`、`POST_PROMOTED_NOTIFICATIONS` | `NSSupportsLiveActivities` とウィジェット拡張機能（呼び出すアプリの場合） |
| `platform/transcribe` | `RECORD_AUDIO` | `NSSpeechRecognitionUsageDescription` |
| `platform/screen` | `DETECT_SCREEN_CAPTURE` | |
| `platform/tracking` | | `NSUserTrackingUsageDescription` |
| `platform/review` | Play のアプリ内レビューライブラリ | |
| `platform/integrity` | Play Integrity | App Attest のエンタイトルメント |
| `platform/agerange` | Play Age Signals | Declared Age Range のエンタイトルメント |
| `platform/purchase` | Play Billing、`com.android.vending.BILLING` | StoreKit 2（呼び出すアプリの場合）。エンタイトルメントは不要 |
| `platform/icloud` | （なし：iCloud はありません） | キーバリューストアのエンタイトルメント、iCloud Documents とアプリのコンテナー、`NSUbiquitousContainers`。アプリが呼ぶものに応じて加わります（[iCloud](icloud.md)） |
| `platform/maps` | Google の Maps SDK | |
| `platform/periodic` | WorkManager | `fetch` と `processing` バックグラウンドモード。タスクの識別子 |
| `platform/sensors` | `ACTIVITY_RECOGNITION`（歩数計） | `NSMotionUsageDescription`（歩数計） |
| `platform/speech` | 音声合成エンジンの可視性（`<queries>`） | |
| `platform/contacts` | `READ_CONTACTS`（`list` 用） | `NSContactsUsageDescription` |
| `platform/calendar` | `READ_CALENDAR`、`WRITE_CALENDAR`（`calendars`、`events` 用） | `NSCalendarsFullAccessUsageDescription`、`NSCalendarsWriteOnlyAccessUsageDescription`、`NSCalendarsUsageDescription` |
| `platform/compose` | `mailto:` と `smsto:` を扱うアプリの可視性（`<queries>`） | `LSApplicationQueriesSchemes` に `mailto` |
| `platform/nfc` | `NFC`。`uses-feature android.hardware.nfc`（必須ではない）。asuka.toml の `nfc` のフィルター（[NFC タグ](nfc.md)） | `NFCReaderUsageDescription`。Core NFC のエンタイトルメント。asuka.toml の `nfc` の AID と FeliCa のシステムコード |
| `platform/bluetooth` | `BLUETOOTH_SCAN`（`neverForLocation`）、`BLUETOOTH_CONNECT`、`BLUETOOTH_ADVERTISE`。`backgroundScan` のためのバックグラウンドスキャンのレシーバー（[Bluetooth](bluetooth.md#ビルドが加えるもの) を参照） | `NSBluetoothAlwaysUsageDescription`。`bluetooth.background` のための `bluetooth-central`、`bluetooth-peripheral`（`backgroundScan` には `bluetooth-central`）と、iOS が復元するマネージャーを作る起動クラス |

`platform/device`、`platform/audio`、`platform/splash`、`platform/imageedit`、`platform/widget`、`platform/js`、`platform/secure`、`platform/preview`、`platform/print` は何も必要としません。

リリースビルドでエンタイトルメントを使うには、アプリの App ID（developer.apple.com の Identifiers）でそのケイパビリティを有効にしておく必要があります。そうすれば、プロビジョニングプロファイルにもそのケイパビリティが含まれます。プロファイルに足りないケイパビリティがあると、`asuka build --platform ios` は何が足りないかを示して止まります。シミュレーター向けのビルドにはすべてが含まれます。

Asuka が書き込む利用目的の説明文は汎用的なもの（"To take a photo."）です。iOS と macOS はユーザーに許可を求めるときにこの文を表示するので、`asuka.toml` の `ios.usage` に独自の文を書いておきましょう。

```toml
[ios.usage]
NSCameraUsageDescription = "To photograph your receipts."
NSLocationWhenInUseUsageDescription = "To find stores near you."
```

各パッケージがユーザーに許可を求めるのは、起動時ではなく、初めてその機能が必要になったときです。何かを待つ呼び出し（ユーザーの操作、位置情報の測位、プラットフォームの処理）は `async` です。ハンドラー、タスク、またはほかの async 関数の中で `await` してください。待っているあいだもアプリは動き続けます（[非同期処理](language/async.md#非同期処理)）。プラットフォーム側で時間とともに変わるもの（ネットワーク、権限、アカウント）は追跡されます。バインディングが読む関数か、タスクが読む `Stream` として受け取れます。

## Mac の場合

Mac アプリ（`asuka dev` と `asuka build`）の Info.plist にも同じ利用目的の説明文が入ります（位置情報の説明文は `NSLocationUsageDescription` としても入ります）。アプリの署名には、許可を求める機能（カメラ、マイク、位置情報、連絡先、カレンダー、写真）に対応する Hardened Runtime のエンタイトルメントが含まれます。これがないと、Developer ID で署名したビルドはその機能へのアクセスを拒否されます。プッシュ通知の `aps-environment`、Sign in with Apple、App Attest、Apple Pay、iCloud は、プロビジョニングプロファイルでしか付与されないエンタイトルメントです。これらを使うアプリには、`asuka build` がプロファイルを埋め込みます（`asuka.toml` の `mac.profile`、なければ `ASUKA_MAC_PROFILE`、それもなければインストール済みのプロファイルからもっとも合うもの）。プロファイルがなくてもアプリはビルドでき、CLI が何が動かないかを知らせます。その場合 Mac では、プッシュ通知の登録と Sign in with Apple が失敗し、App Attest は非対応と答えます。`asuka dev` はプロファイルなしでアドホック署名するので、同じことが当てはまります。Mac App Store 向けのビルド（`--appstore`）はサンドボックス化され、ネットワークとユーザーが選んだファイルにアクセスできます。[配布](shipping.md)を参照してください。

macOS でも各パッケージは iOS と同じフレームワーク（AVFoundation、PhotoKit、Core Location、MapKit、Vision、Speech、UserNotifications、AuthenticationServices）を通じて、iOS と同じように動きます。Mac で異なる点は次のとおりです。

| パッケージ | Mac の場合 |
| --- | --- |
| `platform/device` | `pickPhotos` と `pickFiles`：ウィンドウ上のシートとして開くオープンパネル（権限は不要）で、アプリのキャッシュにコピーを作ります。`browse` はユーザーのブラウザーでページを開きます。`info`：`model` は `"Mac"`、`system` は `"macOS"` |
| `platform/camera` | `take()`：ウィンドウの上にシートを出し、ライブプレビューと「写真を撮る」「キャンセル」を表示します（AppKit にはカメラ画面がないため）。`camera.View` の「背面」は内蔵カメラ（Mac には背面カメラがないため）か、接続されたカメラです。そこでコードを読み取るには macOS 13 が必要です |
| `platform/record` | オーディオセッションはありません。ほかのアプリの音は鳴り続けます。`background` は何も変えません。Mac アプリはどのみち見えていなくても録音できます |
| `platform/nowplaying` | iOS と同じく `MPNowPlayingInfoCenter` と `MPRemoteCommandCenter` を使います。コントロールセンターとメニューバーの「再生中」、キーボードのメディアキー、AirPods で操作できます。Mac ではオーディオセッションもバックグラウンドモードも不要です |
| `platform/contacts`、`platform/calendar` | `add` はウィンドウ上のアラートで確認してから保存します（macOS には新規連絡先や新規イベントの画面がないため）。保存の前にアクセスの許可を求めます |
| `platform/photos` | iOS と同じく PhotoKit です。Mac には限定アクセスのライブラリがないので、`manageLimited` は何もしません |
| `platform/library` | 写真とビデオは「写真」アプリに保存されます。`.downloads` と `.music` は、ユーザーの `~/Downloads` と `~/Music` の中の指定したフォルダーに保存されます |
| `platform/biometric` | Touch ID のプロンプト、またはユーザーのパスワード（`noPasscode` のときを除く） |
| `platform/push` | iOS と同じく APNs です。プロビジョニングプロファイルが必要です（上記） |
| `platform/appleauth` | iOS と同じく Apple のボタンとシートです。プロビジョニングプロファイルが必要です（上記） |
| `platform/pay` | iOS と同じく Apple Pay のシートとボタンです。Touch ID を備えた Mac か、ユーザーの iPhone や Apple Watch が近くにある Mac で支払えます。プロビジョニングプロファイルが必要です（上記） |
| `platform/integrity` | iOS と同じく App Attest です。プロビジョニングプロファイルが必要です（上記）。ない場合 `supported()` は false です |
| `platform/background` | タスクの実行中は App Nap によってアプリが遅くなるのを防ぎます。通知は出ないので、`update` は何も変えません |
| `platform/periodic` | `NSBackgroundActivityScheduler` を使います。タスクはアプリの実行中に動きます（タスクのためにアプリが起動されることはありません）。電源を必要とするタスクは、Mac がバッテリーで動いているあいだ待ちます |
| `platform/screen` | `lock`、`setBars`、`setBrightness` は何もしません。`preventCapture` はアプリのウィンドウをスクリーンショットと画面収録から除外します。`screenshots()` は何も知らせません |
| `platform/appicon` | `set` はアプリの実行中に Dock のアイコンを変え、次の起動時にも変えます。Finder ではアプリ自身のアイコンのままです |
| `platform/transcribe` | デバイス上での認識のみです。アプリのバンドルに利用目的の説明文が必要で、`asuka build` がそれを書き込みます |
| `platform/sensors` | Mac にはモーションセンサーがありません。`available` は false で、ストリームと `steps` は `SensorError.notAvailable` を投げます |
| `device.haptic` | トラックパッドのフィードバック（感圧タッチトラックパッド）です。`.selection` では整列（alignment）、それ以外ではレベル変更（level change）のフィードバックになります |
| `platform/widget`、`platform/liveactivity`、`platform/agerange` | Mac にはありません。`widget.set` は何もせず、`liveactivity.start` は「liveactivity: not available」を、`agerange.request` は `AgeRangeError.notAvailable` を投げます |
| `platform/purchase` | StoreKit 2 を、アプリの Swift ライブラリの中で使います。このライブラリは、パッケージをインポートしたアプリのためにビルドが作ります（[アプリ内課金](purchases.md)） |
| `platform/icloud` | iOS と同じで、アプリの Swift ライブラリの中で動きます。iCloud を含むプロビジョニングプロファイルが必要です（上記）。ない場合は「icloud: not available」になります（[iCloud](icloud.md)） |
| `platform/splash` | Mac アプリには起動画面がないので、`hold` と `hide` は何もしません |
| `platform/analytics` | iOS と同じく Firebase を、同じ `GoogleService-Info.plist`（アプリの ID と同じ Apple アプリのもの）で使います |
| `onShare`、ショートカット | iOS と Android だけです。Mac アプリはほかのアプリの共有メニューに表示されず、`asuka.toml` の `shortcuts` も追加されません |

## Windows の場合

Windows アプリは何も宣言しません。`asuka build --platform windows` はケイパビリティを書き込まず、Windows はデスクトップアプリに権限を求めません。ただし「設定」の「プライバシーとセキュリティ」にあるスイッチは、カメラ、マイク、位置情報に対して有効です。オフになっていると、カメラと録音はそれぞれの `notAllowed` エラーを、位置情報は `LocationError.denied` を投げます。

Windows では、各パッケージは Windows 自身のサービス（Media Foundation、WIC、WinRT、Windows Hello、SAPI、資格情報マネージャー）を通じて動きます。Windows で異なる点は次のとおりです。Windows に対応する機能がない場合、パッケージはその機能を持たないデバイスと同じように振る舞います。

| パッケージ | Windows の場合 |
| --- | --- |
| `platform/device` | `pickPhotos` と `pickFiles`：ウィンドウの上に開くファイルを開くダイアログで、アプリのキャッシュにコピーを作ります。`browse` はユーザーのブラウザーでページを開きます。クリップボードの画像は PNG です。`keepAwake` はディスプレイをオンのままにします。`info`：`model` は `"PC"`、`system` は `"Windows"`、`systemVersion` は `"10.0.<build>"` |
| `platform/secure` | 資格情報マネージャーにある、ユーザーの汎用資格情報です。名前は `<app>/<key>` です（値は 2560 バイトまで） |
| `platform/camera` | Media Foundation のキャプチャエンジンを使います。`take()`：ウィンドウの上にダイアログを出し、ライブプレビューと「写真を撮る」「キャンセル」を表示します（Enter と Escape でも操作できます）。`camera.View` は Windows が前面または背面と呼ぶカメラを選び、なければ最初のカメラを使います。`scan` を指定すると、Asuka 独自のリーダー（`scan.file` と同じもの）で毎秒 4 フレームからコードを読み取ります。「設定」でカメラのスイッチがオフのときは `CameraError.notAllowed` です |
| `platform/record`（ビデオ） | MPEG-4（`.mp4`）の H.264 と AAC です。ミュートされていなければ既定のマイクを使います |
| `platform/audio` | MCI（DirectShow の再生）を使います。URL は先にアプリのキャッシュにダウンロードされます |
| `platform/record`（音声） | 既定のマイクを使い、AAC 44.1 kHz で録音します（音声はモノラル、音楽はステレオ）。`background` は何も変えません |
| `platform/nowplaying` | アプリのウィンドウの SystemMediaTransportControls を使います（ウィンドウのないアプリには専用の隠しウィンドウを用意します）。Windows 10 では音量のフライアウト、Windows 11 ではクイック設定のメディアコントロール、ロック画面、キーボードのメディアキーに表示されます。スキップは早送りと巻き戻しのボタンになります。トグルはありません（再生/一時停止キーは再生か一時停止のどちらかを送ります） |
| `platform/speech` | SAPI の音声のうち、インストールされているもの（「設定」の「時刻と言語」>「音声認識」） |
| `platform/network` | Network List Manager を使います。Wi-Fi、モバイルデータ通信、有線のいずれかです。コストのかかる接続は `metered`、データ上限に近いか超えている接続は `constrained` です |
| `platform/location` | WinRT の Geolocator を使います。`placeName` は OpenStreetMap の Nominatim に問い合わせます（Windows 自身のジオコーダーには Bing Maps のキーが必要なため）。リクエストは最大で毎秒 1 回で、結果はアプリのキャッシュに保存されます。リージョンはアプリの実行中に `GeofenceMonitor` で監視し、バックグラウンドでの更新は位置から計算します |
| `platform/biometric` | Windows Hello のプロンプトです。`check()` が返す種類はセンサーのものです。Hello は常に PIN の入力も選択肢に出し、キャンセルボタンの名前も Hello 自身が決めます |
| `platform/webauth` | アプリのウィンドウの上に、WebView2 を入れたウィンドウを開きます（アプリの Web プロファイルを使います。`ephemeral` では使い捨てのプロファイル） |
| `platform/js` | 一度も表示されない WebView2 |
| `platform/contacts`、`platform/calendar` | Windows はパッケージ化されたアプリにしか連絡先とカレンダーを渡しません。`request` は `.denied` と答えます。`add` は vCard かイベントのファイルを、ユーザーの連絡先アプリやカレンダーアプリで開き、false を返します（保存されたかどうかを Windows は知らせないため） |
| `platform/photos` | ピクチャフォルダーです（求める権限はありません）。スクリーンショットはその中の Screenshots フォルダーのものです |
| `platform/library` | ユーザーのダウンロード、ビデオ、ミュージック、ピクチャフォルダーの、指定したフォルダーに保存されます |
| `platform/vision` | Windows の OCR（OCR がインストールされている言語のみ。「設定」の「時刻と言語」）と顔検出を使います。コードは `platform/scan` と同じ方法で読み取ります |
| `platform/scan` | Windows にはアプリ向けのコードリーダーがありません。そのため Asuka 独自のリーダーが、QR コード、Data Matrix、EAN-13 と EAN-8、UPC-A と UPC-E、Code 128、Code 39 を読み取ります（Aztec、PDF417、Code 93、Codabar、ITF は読めません） |
| `platform/imageedit` | Windows のコーデックで読み込みます（HEIC と WebP は、それぞれの拡張機能がインストールされている場合）。ビデオのフレームは Media Foundation で取り出します |
| `platform/maps` | Windows にはデスクトップアプリ向けの地図コントロールがありません（Windows 自身のものには Bing Maps のキーが必要です）。そのため Asuka が OpenStreetMap のタイルを描きます。タイルは 2 枚ずつダウンロードされ、アプリのキャッシュに 1 週間保存されます。地図には「© OpenStreetMap contributors」が表示されます。ドラッグ、ホイール、ピンチ、ダブルクリック、+ と - で操作できます。`.satellite` と `.hybrid` は道路地図を表示し（無料の航空写真がないため）、`showsUser` は Linux と同じく何も表示しません |
| `platform/background` | タスクの実行中は PC がスリープしないようにします。通知は出ないので、`update` は何も変えません |
| `platform/periodic` | アプリの実行中にタイマーで動きます（タスクのためにアプリが起動されることはありません）。電源を必要とするタスクは、PC がバッテリーで動いているあいだ待ちます |
| `platform/screen` | `lock`、`setBars`、`setBrightness` は何もしません。`preventCapture` はアプリのウィンドウをスクリーンショットと画面録画から除外します。`screenshots()` は何も知らせません |
| `platform/appicon` | `set` はアプリのウィンドウとタスクバーボタンのアイコンを変え、次の起動時にも変えます。「スタート」メニューと実行ファイルはアプリ自身のアイコンのままです |
| `platform/tracking` | プロンプトは出ません。「設定」で広告 ID がオンなら許可、オフなら拒否です |
| `platform/push`、`platform/widget`、`platform/review`、`platform/integrity` | これらはパッケージ化された（ストアの）アプリか、Apple や Google のものです。`push.register` はエラーを投げ、`widget.set` は何もせず、`review.request` は何も表示せず、`integrity.supported()` は false です |
| `platform/liveactivity` | スマートフォンの機能です。`enabled` は false で、`start` は「liveactivity: not available」を投げます |
| `platform/purchase` | Microsoft Store からインストールしたアプリでは Microsoft Store を使います。それ以外では `available()` は false で、ほかの呼び出しは `PurchaseError.notAvailable` を投げます（[アプリ内課金](purchases.md)） |
| `platform/pay` | Windows アプリの支払いに使えるウォレットはありません。`available` は `.none` を返し、`pay` は `PayError.notAvailable` を投げ、`PayButton` は何も表示しません |
| `platform/appleauth`、`platform/agerange`、`platform/icloud`、`platform/sensors` | Windows にはありません。その機能を持たないデバイスと同じく、利用できません |
| `platform/transcribe` | Windows のデスクトップ音声認識で、PC 上で処理します。英語の認識は Windows に付属し、ほかの言語はその言語パックの音声認識と一緒に入ります。どれが使えるかは `check` でわかります。`listen` は既定のマイクを使います |
| `device.haptic` | PC には触覚フィードバックがないので、何もしません |
| `platform/splash` | Windows アプリには起動画面がないので、`hold` と `hide` は何もしません |
| `platform/analytics` | GA4 の Measurement Protocol で、`asuka.toml` の Web ストリームに API シークレットを使って送ります。メインスレッドの外でまとめて送り、クライアント ID と未送信のイベントはアプリのデータディレクトリに保存します（[アナリティクス](#アナリティクスplatformanalytics)） |
| `onShare`、ショートカット | iOS と Android だけです |

## Linux の場合

Linux アプリも何も宣言しません。Asuka が XDG デスクトップポータルを通す機能（ファイル、カメラ、位置情報、通知、メール、印刷、ファイルやリンクを開く、システムをスリープさせない）では、ポータルがいつもどおりユーザーに確認します。これは Flatpak のサンドボックスの中でも外でも同じように動きます。`asuka build --platform linux` が書き出す Flatpak マニフェストで、アプリがアクセスできるのはディスプレイ、GPU、ネットワーク、ポータルだけです。音を再生または録音するアプリ（`Video`、`platform/audio`、`platform/record`、`platform/transcribe` を使うもの）は、サウンドサーバーにもアクセスできます。パッケージが開くライブラリのうち GNOME ランタイムにないもの（`platform/pdf` の Poppler、`platform/transcribe` の PocketSphinx）は、マニフェストが一緒にビルドします。Asuka がポータルを通さずに使うサービス（Speech Dispatcher、連絡先とカレンダーの Evolution Data Server、接続の種類を調べる NetworkManager、iio-sensor-proxy、Video4Linux のカメラ）を使うには、それらへのアクセスを開く `finish-args` をマニフェストに追加する必要があります。追加しない場合、パッケージはそのサービスがないシステムと同じように振る舞います。後で届く通知にはサンドボックスの外の systemd が必要です。Flatpak の中では、アプリの実行中にしか表示されません（[プラットフォームごとの動作](#プラットフォームごとの動作)を参照）。

Linux では、Asuka はポータルがあればポータルを通じて、ほかに GLib と GTK、そしてデスクトップにあるライブラリを通じて、デスクトップのサービスを使います。各ライブラリは初めて使うときに読み込まれます。ライブラリがないシステムでは、足りないものを示すエラーになります。

| パッケージ | Linux の場合 |
| --- | --- |
| `platform/device` | `pickPhotos` と `pickFiles`：GTK のファイルダイアログ（サンドボックス内ではポータルのもの）で、アプリのキャッシュにコピーを作ります。クリップボードの画像と変更は GDK のクリップボードを通じて扱います。`browse` はユーザーのブラウザーでページを開きます。`keepAwake` はセッションがアイドル状態になるのを防ぎます。`info`：`model` は `"PC"`、`system` は `"Linux"`、`systemVersion` はディストリビューションのバージョン（`/etc/os-release`） |
| `platform/secure` | キーリング（libsecret：GNOME Keyring、KWallet、KeePassXC）に、キーごとに 1 つ、アプリ名のラベルを付けた項目を作ります。libsecret やキーリングがなければ、呼び出しはその旨を示して失敗します |
| `platform/network` | GNetworkMonitor（サンドボックス内ではポータルのもの）を使います。Wi-Fi、Ethernet、モバイルデータ通信の区別は NetworkManager から取得します。constrained になることはありません |
| `platform/location` | 位置情報ポータル（GeoClue のもので、ユーザーに確認します）、なければ GeoClue 自体を使います。`placeName` は geocode-glib がインストールされていれば使えます。GeoClue にはジオフェンスがないので、リージョンとバックグラウンドでの更新は、アプリの実行中に位置から計算します |
| `platform/speech` | Speech Dispatcher（動いていなければ起動します）の音声、速さ、高さ、音量を使います |
| `platform/audio`、`platform/record` | GStreamer を使います。録音には AAC エンコーダー（FDK、libav など）を使い、マイクは既定のソースです |
| `platform/nowplaying` | 何かを表示しているあいだ、セッションバスに `org.mpris.MediaPlayer2.<app>.instance<pid>` という名前の MPRIS プレイヤーを置きます。GNOME や KDE のメディアコントロールとロック画面、メディアキー、`playerctl` から操作できます。MPRIS の `Seek` は、アプリがスキップを扱う場合はスキップ、そうでなければシークになります。アートワークは `file://` か http(s) の URL です |
| `platform/library` | ユーザーのフォルダー（XDG のピクチャ、ビデオ、ミュージック、ダウンロード）の、指定したフォルダーに保存されます |
| `device.shareFiles` | 共有シートはありません。ファイルはクリップボードにコピーされ、トーストが表示されます |
| `platform/camera`、`platform/record` | カメラポータル（ユーザーへの確認は一度だけ。PipeWire のカメラで、`gstreamer1.0-pipewire` が必要）、なければ最初の Video4Linux カメラを、GStreamer を通じて使います。`take()`：ウィンドウの上にダイアログを出し、ライブプレビューと「写真を撮る」「キャンセル」を表示します。`camera.View`：左右反転したプレビューで、枠いっぱいに表示します。前面と背面は同じカメラです（コンピューターのカメラはユーザーのほうを向いているため）。ビデオは MPEG-4（x264 か OpenH264 の H.264 と AAC）、それができなければ WebM です。コードの読み取りには GStreamer の `zbar` エレメントが必要です。カメラがなければ `CameraError.noCamera`、拒否されたら `.notAllowed` です |
| `platform/scan`、`platform/vision` | Windows と同じく、コードは Asuka 自身のリーダーが読み取ります。QR、Data Matrix、EAN、UPC、Code 128、Code 39 です（Aztec、PDF417、Code 93、Codabar、ITF は読めません）。テキストは Tesseract（`libtesseract5` と各言語のデータ）が読み取ります。顔を検出するものはないので、`faces` は空です。Tesseract がなければ、`vision.analyze` の結果にテキストは含まれません |
| `platform/imageedit` | GdkPixbuf のローダーでデコードし（HEIF と WebP は、それぞれのローダーがインストールされている場合）、向きを正しく直します。各操作は Asuka が行います。ビデオのフレームは GStreamer で取り出します |
| `platform/webauth` | ウィンドウの上に WebKitGTK の Web ビューを入れたダイアログを開きます（`ephemeral` では専用のセッション）。閉じるとキャンセルになります |
| `platform/js` | 一度も表示されない WebKitGTK の Web ビュー |
| `platform/maps` | libshumate（`libshumate-1.0-1`）で OpenStreetMap のタイルの地図を表示します。`.satellite` と `.hybrid` は道路地図を表示します。ユーザーの位置を示すレイヤーはありません。libshumate がなければ、ビューにその旨が表示され、`error` が報告されます |
| `platform/background` | タスクの実行中はシステムがサスペンドしないようにします。通知は出ず、`update` は何も変えません |
| `platform/periodic` | タスクはアプリの実行中にタイマーで動きます（タスクのためにアプリが起動されることはありません）。電源を必要とするタスクはバッテリーで動いているあいだ、ネットワークを必要とするタスクはオフラインのあいだ待ちます |
| `platform/screen` | `lock`、`setBars`、`preventCapture`、`setBrightness` は何もしません。`screenshots()` は何も知らせません |
| `platform/appicon` | `set` は `notAvailable` を投げます。ランチャーは desktop ファイルのアイコンを表示します |
| `platform/sensors` | iio-sensor-proxy（2-in-1 PC、タブレット）を使います。照度センサーをルクス単位で、加速度センサーの向きを、上を向いている軸に沿った 1 g として、向きが変わるたびに伝えます。ほかのセンサーはないので、`available` は false で、ストリームと `steps` は `SensorError.notAvailable` を投げます |
| `platform/contacts`、`platform/calendar` | Evolution Data Server のアドレス帳とカレンダー（GNOME Contacts、GNOME Calendar、オンラインアカウントのもの）を使います。アドレス帳やカレンダーがあれば、アクセスは `.allowed`（カレンダーは `.full`）です。`pick` は検索欄付きの連絡先のダイアログです。`add` は確認してから、既定のアドレス帳またはイベントのカレンダーに保存します。Evolution Data Server がない場合、連絡先のアクセスは `.denied`、カレンダーのアクセスは `.writeOnly` です。`add` は vCard か iCalendar のファイルをユーザーの連絡先アプリやカレンダーアプリで開いて false を返し、`contacts.pick` はエラーを投げます |
| `platform/photos` のライブラリ、`platform/appleauth`、`platform/widget`、`platform/liveactivity` | Linux にはありません。ライブラリのアクセスは `.denied` です。`appleauth.available()` は false で、そのボタンは何も表示しません。`liveactivity.start` は「liveactivity: not available」を投げます。`widget.set` は何もしません。`platform/splash` には保持しておく起動画面がありません |
| `platform/purchase` | ストアはありません。`available()` は false で、ほかの呼び出しは `PurchaseError.notAvailable` を投げます |
| `platform/icloud` | iCloud はありません。「icloud: not available」になります |
| `platform/biometric`、`platform/integrity`、`platform/agerange`、`platform/push` | Linux にはありません。利用不可、非対応と答えます。`platform/review` は何もせず、`platform/tracking` は `.allowed` と答えます |
| `platform/pay` | Linux アプリの支払いに使えるウォレットはありません。`available` は `.none` を返し、`pay` は `PayError.notAvailable` を投げ、`PayButton` は何も表示しません |
| `platform/crash` | Asuka のクラッシュ出力を使います。ネイティブコードでのクラッシュ（GTK のコードでのシグナル、GLib の abort）もここに含まれ、`"native"` として報告されます。システム自身の記録（systemd-coredump、apport）は読みません |
| `platform/analytics` | Windows と同じく GA4 の Measurement Protocol を使います |
| `device.haptic` | デスクトップには触覚フィードバックがないので、何もしません |
| `onShare`、ショートカット | iOS と Android だけです |

## ブラウザーの場合

ページは何も宣言しません。パッケージが初めてカメラ、マイク、位置情報、通知を必要としたときに、ブラウザーがユーザーに確認します。これらの API の多くは安全なページ、つまり `https` か `localhost` から配信されるページでしか使えません（`asuka dev --platform web` のアドレスは該当しますが、`--lan` のアドレスは該当しません）。各パッケージは Web の API を呼び出します。詳しくは [Web](web.md) を参照してください。通知と Web Push のための Service Worker についてもそこで説明しています。

| パッケージ | ブラウザーの場合 |
| --- | --- |
| `platform/device` | `pickPhotos` と `pickFiles`：ブラウザーのファイル選択で、キャッシュにコピーを作ります。`browse`：新しいタブで開きます。クリップボード：アプリ自身がコピーしたものと、ユーザーがページに読み取りを許可した場合はほかのアプリのものです。`keepAwake`：Screen Wake Lock です。`info` はユーザーエージェントから取得します（Chromium のクライアントヒントがあれば、システムのバージョンと Android スマートフォンの機種も分かります） |
| `platform/secure` | 値を AES-GCM で暗号化して IndexedDB に保存します。鍵はブラウザーが外に出さないものです。キーチェーンではありません（[Web](web.md) を参照） |
| `platform/camera`、`platform/record` | `take()` はスマートフォンのカメラを開くか、ライブのカメラ映像とシャッターのあるダイアログを開きます。`camera.View` は `<video>` に映したカメラで、`capture()` と録画が使えます。ブラウザーに `BarcodeDetector` があればコードも読み取れます |
| `platform/scan`、`platform/vision` | `BarcodeDetector` がコードを読み取ります。顔とテキストは、ブラウザーが対応している場合だけです |
| `platform/imageedit` | canvas を使い、JPEG か PNG を出力します。リモートの画像には CORS が必要です |
| `platform/photos` | ページはライブラリを読めないので、アクセスは `.denied` です（写真の選択には `device.pickPhotos` を使います） |
| `platform/library` | `save` はファイルをダウンロードします |
| `platform/location` | Geolocation API を使います。Web には `placeName` がありません（ブラウザーにはジオコーダーがないため）。リージョンとバックグラウンドでの更新は位置から計算し、ページが開いているあいだだけ動きます |
| `platform/network` | オンラインかどうかは `navigator.onLine` から取得します。Wi-Fi、モバイルデータ通信、Ethernet の区別はブラウザーが教える場合（Android の Chrome）だけです。モバイルデータ通信は metered、データセーバーは constrained になります |
| `platform/notify`、`platform/push` | Notification API と Web Push を、アプリの Service Worker を通じて使います（`push.register` の `service` は `"webpush"`）。後で届く通知は、アプリのページが一つも開いていないあいだは表示できません |
| `platform/audio`、`platform/record` | プレイヤーごとに audio 要素を使います（ブラウザーの自動再生のルールが適用されるので、タップから再生してください）。録音は `MediaRecorder` で、ブラウザーが対応していれば MPEG-4 の AAC、そうでなければ WebM の Opus です |
| `platform/nowplaying` | Media Session API（`navigator.mediaSession`）を使います。ページが音を鳴らしているあいだ、ブラウザーのメディアコントロール、メディアキー、スマートフォンのロック画面に表示されます。トグルはありません（キーは再生か一時停止のどちらかを送ります）。ブラウザーが対応していない操作（一部のブラウザーでは停止）は省かれます |
| `platform/speech` | `speechSynthesis` と、ブラウザーの音声を使います |
| `platform/sensors` | Generic Sensor API（Chrome）か、デバイスのモーションと向きのイベント（Safari ではタップ時に許可を求めます）を、Android の軸と単位で伝えます。気圧計と歩数計はありません |
| `platform/biometric` | WebAuthn を通じてプラットフォームの認証器（Touch ID、Face ID、Windows Hello、Android の画面ロック）を使います。最初の `authenticate` でそのサイトのパスキーを作ります。プロンプトはブラウザーのものなので、`reason` と `cancel` は表示されません |
| `platform/webauth` | ポップアップを開き、アプリと同じオリジンのページに戻ってきたら完了です（カスタムスキームではページを開けないため） |
| `platform/js` | 非表示の、サンドボックス化された frame |
| `platform/contacts`、`platform/calendar` | `contacts.pick` はブラウザーに Contact Picker がある場合（Android の Chrome）に使えます。`add` は vCard か `.ics` ファイルをダウンロードします。Web では連絡先やカレンダーを読み取れません |
| `platform/screen` | `lock` はフルスクリーンかインストールしたアプリでのみ使えます。バーを隠すとフルスクリーンになります。`preventCapture`、`screenshots()`、`setBrightness` はありません |
| `platform/appicon` | `set` はページ（タブ）のアイコンを変え、次に開いたときにも変えます。インストールした Web アプリはマニフェストのアイコンのままです |
| `platform/background` | ページは開いているあいだ動き続けます。タスクを作っても何も変わらず、通知も出ません |
| `platform/tracking` | プロンプトは出ません。ブラウザーが Global Privacy Control か Do Not Track を送っていれば `.denied`、そうでなければ `.allowed` です |
| `device.haptic` | ブラウザーが対応していれば（Android の Chrome）`navigator.vibrate` を使います |
| `platform/review`、`platform/integrity`、`platform/agerange`、`platform/appleauth`、`platform/icloud`、`platform/periodic`、`platform/widget`、`platform/liveactivity` | Web にはありません。その旨を示すエラーか、パッケージの `notAvailable` になります |
| `platform/purchase` | Trusted Web Activity の中では、Digital Goods API を通じて Google Play を使います。それ以外ではストアはありません（`PurchaseError.notAvailable`） |
| `platform/maps` | Windows と同じく、Asuka が OpenStreetMap のタイルを描きます。`showsUser` は、ページに位置情報の許可があれば表示されます |
| `platform/pay` | ブラウザーが対応していれば（Safari）、Payment Request API を通じて Apple Pay を使います。マーチャントの検証はアプリのサーバーが行います（`onValidateMerchant`）。それ以外では、Google の Web API（pay.js）を通じて Google Pay を使います。ボタンはそれぞれ専用のものです。シートはタップからしか表示できません |
| `platform/crash` | Asuka の致命的なエラーと、捕捉されなかった JavaScript のエラーを扱います。後者は致命的でない `"exception"` のレポートになります |
| `platform/analytics` | Google から読み込む gtag.js で、`asuka.toml` の Web ストリームに送ります。収集のオフは gtag 自身のスイッチで、`localStorage` に保存されます |
| `platform/splash` | `hold` と `hide` は何もしません |
| `onShare`、ショートカット | iOS と Android だけです |

## カメラ：platform/camera

`camera.View` はカメラに映っているものをライブで表示します。iOS と Mac では AVFoundation のプレビュー、Android では CameraX、ブラウザーでは `getUserMedia`、Windows では Media Foundation、Linux では GStreamer です。`camera.capture()` は画面上のこのビューから写真を撮ります。`camera.take()` は代わりにシステムのカメラ画面を開き（iOS のカメラ画面、Android のカメラアプリ。デスクトップではプレビュー付きのシート、ブラウザーではファイル選択のカメラ）、写真のファイルを返します。ユーザーがキャンセルしたときは `none` です。写真はアプリのキャッシュにある JPEG なので、残したいときは移動してください。

```asuka
import platform/camera

component Shutter {
  state facing: camera.Facing = .back
  state last = ""
  state status = ""

  Column {
    camera.View {
      facing: facing
      grow: 1
      on error(m) { status = m }
    }
    Row {
      gap: 12
      Button("Flip") { on press { facing = if facing == .back { .front } else { .back } } }
      Button("Take") { on press { last = try await camera.capture() } }
    }
    Text(status)
  }
}
```

ビューは最初に表示されたときにカメラの許可を求めます。拒否されたときや、カメラのないデバイス（iOS シミュレーター）では `error` を報告し、`capture` は `CameraError.notAllowed` か `.noCamera` を投げます（画面に `camera.View` がないときは `.noView`）。`platform/camera` をインポートしたアプリには、iOS ではカメラとマイクの利用目的の説明文が（asuka.toml の `[ios.usage]` で独自の文にできます）、Android では `CAMERA` と `RECORD_AUDIO` の権限が加わります。

`camera.access()` は、アプリがカメラを使えるかどうかを、何も尋ねずに返します（`.notAsked`、`.denied`、`.allowed`）。`camera.request()` は、まだ尋ねていなければユーザーに尋ね、その答えを返します。ユーザーがカメラの要る操作をしたときに、理由を伝えてから尋ねてください。一度断られるとシステムはもう尋ねないので、`device.openSettings()` でオンにできる場所へ案内します。

```asuka check
import platform/camera
import platform/device

component ScanButton {
  state why = ""

  Column {
    gap: 8
    Button("Scan a receipt") {
      on press {
        if camera.access() == .denied {
          why = "The camera is off for this app"
          return
        }
        if (try? await camera.request()) == .allowed {
          why = ""
        }
      }
    }
    if !why.isEmpty() {
      Text(why)
      Button("Open Settings") { on press { let _ = device.openSettings() } }
    }
  }
}
```

iOS と Mac はユーザーの選択を返します（ポリシーで禁止されている場合は `.denied`）。Android は許可されているかどうかと、アプリが尋ねたかどうかを返します。Windows はデスクトップアプリに何も尋ねないので、設定でデスクトップアプリのカメラをオフにしていなければ `.allowed` です。Linux はカメラポータルのこのアプリへの答え（ポータルがなければ `.allowed`）、ブラウザーはそのサイトへの許可です。`record.access()` と `record.request()` はマイクについて同じことをします。

## ビデオの録画：platform/record

ビデオモードの `camera.View` は録画できます。`record.start()` はビューに映っているものを、`mute` を指定しない限りマイクの音と一緒に、`record.stop()` を呼ぶまで（または `maxDuration` まで）録画し、ファイル（アプリのキャッシュにある MPEG-4 の動画）を返します。`camera.record` と `camera.stopRecording` も同じものです。

```asuka
import platform/camera
import platform/record

component Clip {
  state recording = false
  state file = ""

  Column {
    camera.View { mode: .video; grow: 1 }
    Button(if recording { "Stop" } else { "Record" }) {
      on press {
        if recording {
          record.stop()
        } else {
          recording = true
          defer { recording = false }
          file = try await record.start(maxDuration: 30s)
        }
      }
    }
  }
}
```

iOS と Mac では AVCaptureMovieFileOutput、Android では CameraX の `VideoCapture`、ブラウザーでは `MediaRecorder`（MPEG-4 を書けないときは WebM）、Windows では Media Foundation の sink writer、Linux では GStreamer を使います。

## バーコードと QR コード：platform/scan

`scan` をオンにした `camera.View` は、映ったコードをライブで読み取り、一つごとに `code` で報告します（映り続けているあいだは一度だけ）。`codes` で探す種類を絞れます。指定しなければすべての種類です。種類の名前は `"qr"`、`"aztec"`、`"datamatrix"`、`"pdf417"`、`"ean13"`、`"ean8"`、`"upca"`、`"upce"`、`"code128"`、`"code39"`、`"code93"`、`"codabar"`、`"itf"` です。

`scan.file(path)` は代わりに画像ファイル（撮った写真や選んだ写真）の中のコードを読み取ります。iOS と Mac では Vision、Android では ML Kit（モデルをアプリに含めるので約 2 MB 増えますが、Google Play 開発者サービスがなくても動きます）、ブラウザーでは `BarcodeDetector`（あれば）、Windows と Linux では Asuka 自身のリーダー（アプリが頼れるリーダーがないため。QR、Data Matrix、EAN、UPC、Code 128、Code 39）を使います。

```asuka
import platform/camera
import platform/scan

component Scanner {
  state found = ""

  Column {
    camera.View {
      scan: true
      codes: ["qr", "ean13"]
      grow: 1
      on code(c) { found = "{c.kind}: {c.value}" }
    }
    Text(found)
  }
}

async fn read(photo: string) throws -> [string] {
  var out: [string] = []
  for c in try await scan.file(photo, kinds: ["qr"]) { out.push(c.value) }
  return out
}
```

## 画像の内容の認識：platform/vision

`vision.analyze(path)` は画像に何が写っているかをデバイス上で見つけます。行ごと・単語ごとのテキスト、顔、バーコードが、それぞれの `Box`（画像の左上を原点に、画像の幅と高さに対する割合で表した位置。画像は正しい向きに直してあります。ビューのサイズを掛ければポイントになります）付きで返ります。必要なものだけを頼み（`text:`、`faces:`、`codes:`）、テキストの言語がわかっているときは指定してください（`languages: ["ja"]`）。データはデバイスの外に出ません。

```asuka
import platform/vision

async fn words(photo: string) throws -> [string] {
  let r = try await vision.analyze(photo, faces: false, codes: false)
  return r.lines.map(l => l.text)
}
```

iOS と Mac では Vision フレームワーク、Android では ML Kit と同梱のモデル（約 30 MB）、ブラウザーでは Shape Detection API（あれば）、Windows では Windows の OCR と顔検出、Linux では Tesseract（インストールされていれば）を使い、Windows と Linux のコードは Asuka 自身のリーダーが読み取ります。テキストと顔は各プラットフォーム自身の認識によるもので（すべてのプラットフォームで共有できる認識器はありません）、見つかるものはプラットフォームによって異なることがあります。コードの値を読むのは `platform/scan`、コードがどこにあるかを知るのはこのパッケージです。

## 写真ライブラリ：platform/photos

`platform/photos` は、ユーザーの許可を得てユーザーのライブラリを読み取ります。iOS と Mac では PhotoKit、Android では MediaStore、Windows ではピクチャフォルダーを使います。Linux と Web にはアプリが読めるライブラリがないので、アクセスは `.denied` です。このパッケージは、ライブラリそのものを画面にするアプリのためのものです。ユーザーに数枚の写真を選んでもらうだけなら、権限を求めない `device.pickPhotos` を使えます。それで足りる場合はそちらを使ってください。Google Play は、ライブラリを読むアプリにその理由を Play Console で尋ねます。呼び出しはすべて async です。

```asuka
import platform/photos

component Screenshots {
  state thumbs: [string] = []

  task {
    if (try? await photos.request()) == .denied { return }
    let assets = (try? await photos.list(screenshots: true, limit: 60)) ?? []
    for a in assets {
      thumbs.push((try? await photos.thumbnail(a.id, size: 300)) ?? "")   // Image 用のファイル
    }
  }

  Scroll {
    for (i, t) in thumbs.enumerated() key i { Image(t) { width: 100; height: 100 } }
  }
}

async fn original(a: photos.Asset) throws -> string {
  let f = try await photos.export(a.id)   // オリジナルをファイルとして
  return f.path
}
```

- `photos.current()` は、許可を求めずに、何が許可されているかを返します。`.notAsked`、`.denied`、`.limited`（ユーザーが選んだ写真だけ。iOS 14、Android 14）、`.allowed` のいずれかです。`photos.request()` は初回に許可を求めます。`photos.manageLimited()` を使うと、`.limited` のユーザーが対象の写真を変更できます。ユーザーが終えると戻るので、そこでもう一度一覧を取得してください。
- `photos.list(screenshots:, offset:, limit:)` は新しい順に、`offset` から最大 `limit` 件（既定は 100）を返します。`Asset` は `id`、`width`、`height`、`created`（`time.Time`）、`screenshot` を持ちます。`screenshots: true` にするとスクリーンショットだけに絞り込みます。iOS はスクリーンショットかどうかを知っていますが、Android では Screenshots フォルダーにあるものがスクリーンショットとして扱われます。
- `photos.thumbnail(id, size:)` は、短辺が `size` ピクセル以上の JPEG ファイルです。次回のためにアプリのキャッシュに保存されます。
- `photos.export(id)` は、ライブラリにあるままのフルサイズの画像（HEIC、JPEG、PNG）をファイルにコピーし、その `path` と、ライブラリでの名前 `name` を返します。iCloud の写真は先にダウンロードされます。

## 写真、ブラウザー、クリップボード：platform/device

`platform/device` は、ビューの外でプラットフォームが提供する機能をまとめたものです。

```asuka
import platform/device

component Gallery {
  state photos: [string] = []

  Column {
    Button("Pick photos") {
      on press { photos = (try? await device.pickPhotos(max: 3)) ?? [] }
    }
    Button("Help") {
      on press { try? await device.browse("https://example.com/help") }
    }
    for p in photos key p { Image(p) }
  }
}
```

- `device.pickPhotos(max:)` は、システムの写真ピッカー（iOS では PHPicker、Android では写真選択ツール、デスクトップではファイルを開くダイアログ、ブラウザーではファイル選択）を表示し、最大 `max` 枚（1〜255）の画像を選ばせます。権限は求めません。選ぶのはユーザーで、アプリに見えるのは選ばれたものだけです。戻り値は画像のファイルで、`Image` で表示できます。ユーザーがキャンセルした場合は一つも返りません。ファイルはアプリの一時ファイルかキャッシュに置かれたコピーなので、残しておきたいものは移動してください。
- `device.browse(url)` は、アプリの画面の上に Web ページを表示します（SFSafariViewController、Custom Tab）。URL は http か https でなければなりません。これに対して `device.openURL` は、アプリを離れてブラウザーで開きます。デスクトップではユーザーのブラウザーで、ブラウザーでは新しいタブでページを開きます。
- `device.clipboardText()` はクリップボードのテキストを読み取ります。テキストがなければ `none` です。iOS では、ほかのアプリが置いたものをアプリが初めて読むときにユーザーに確認します。書き込むには `device.copy(text)` を使います。
- `device.keepAwake(on)` は、`on` が true のあいだ画面をオンのままにします。ビデオ、レシピ、地図などに使います。それを必要とする画面が閉じたらオフにしてください。
- `device.pickFiles(types:, multiple:)` は、システムのドキュメントピッカー（iOS の「ファイル」ブラウザー、Android のストレージピッカー、デスクトップではファイルを開くダイアログ、ブラウザーではファイル選択。権限は不要）を表示します。`types` は MIME タイプ（`"application/pdf"`、`"image/*"`。指定しなければどのファイルでも）で、`multiple` を指定すると複数選べます。選ばれたファイルごとに `device.File` を返します。その `path` はアプリのキャッシュにあるコピーで、`name`、`type`、`size` も持ちます。ユーザーがキャンセルした場合は一つも返りません。
- `device.clipboardHas()` は、クリップボードの中身を読まずに、`text`、`image`、`url`、リッチテキスト（`html`）のどれがあるかを返します（iOS はこのときユーザーに確認しません）。貼り付けるものがあるときだけ「ペースト」ボタンを表示する、といった使い方ができます。`device.clipboardImage()` はクリップボードの画像を、コピーされたときの形式のままファイルに読み出します。`device.copyImage(path)` は画像ファイルをクリップボードに置きます。`device.clipboardHTML()` は、リッチテキスト（ブラウザー、メール、文書からコピーしたもの。太字、リンク、リストを含みます）を HTML として読み取ります。`device.copyHTML(html, text:)` はそれをクリップボードに置き、プレーンテキストしか貼り付けられないアプリのために `text`（既定では HTML 自身のテキスト）も添えます。`device.clipboardChanges()` は、クリップボードが変わるたびにその中身を伝えるストリームです（iOS と Android は変化を知らせます。Mac と Windows には毎秒 2 回問い合わせます。ブラウザーはページの中でのコピーと、ユーザーがクリップボードの読み取りを許可した場合はほかのアプリのコピーを検知します）。
- `device.info()` はデバイスとアプリの情報です。`model`（"iPhone"、"Pixel 9"、"Mac"、"PC"）、`system`（"iOS"、"Android"、"macOS"、"Windows"、"Linux"）、`systemVersion`、そして `asuka.toml` の `version` と `build` である `appVersion` と `build` を持ちます。expo-application と同じく、`source` はアプリの入手元（`.appStore`、`.testFlight`、`.playStore`、Microsoft Store や Flathub などの `.otherStore`、ダウンロードやサイドロードした APK の `.direct`、開発者のビルドの `.development`）で、`installedAt` と `updatedAt` はアプリがインストールされた日時と、このバージョンがインストールされた日時です（Web ではなく、`source` は `.unknown`）。ベータ版のバナーを出したり、ダウンロードしたコピーにストアを案内したりするのに使えます。

```asuka
import platform/device

component Player {
  let info = load device.info()

  task {
    device.keepAwake(true)
    defer { device.keepAwake(false) }
    await sleep(1h)
  }

  Text(if let i = info.value { "{i.model}, {i.system} {i.systemVersion}" } else { "" })
}
```

## Web でのサインイン：platform/webauth

`webauth.open(url)` は、システム自身のサインインシートにサインインページを表示します。iOS では `ASWebAuthenticationSession`、Android では Custom Tab です。中身はブラウザーなので、ユーザーのセッション、パスキー、パスワードの自動入力がそのまま使えます。また、アプリ独自の `WebView` を拒否するプロバイダー（Google のサインイン）にも拒否されません。OAuth には `WebView` ではなくこちらを使ってください。

Mac にも `ASWebAuthenticationSession` があります。Windows と Linux にはシステムのシートがないので、アプリのウィンドウの上に、アプリ自身の Web ビュー（WebView2、WebKitGTK）を開いてページを表示します。この Web ビューはアプリの Web プロファイルを使い、ユーザーのブラウザーのセッションは持ちません。また、埋め込みの Web ビューを拒否するプロバイダーには、ここでも拒否されます。ブラウザーではページをポップアップで開き、アプリと同じオリジンのページに戻ってきたら完了です（カスタムスキームではページを開けないため）。

```asuka
import platform/webauth

async fn signIn(authorizeURL: string) throws -> string? {
  do {
    // myapp://…?code=…：コードを交換する。
    return try await webauth.open(authorizeURL, callbackScheme: "myapp")
  } catch webauth.WebAuthError.canceled {
    return none // ユーザーが閉じた
  }
}
```

終わり方は 2 通りあります。

- **コールバックスキーム。** ページが `callbackScheme`（アプリ自身のスキーム）の URL に移動すると、シートが閉じて `open` がその URL を返します。Android では、スキームはアプリのもの、つまり asuka.toml の `scheme` でなければなりません。その URL はアプリの `openURL` イベントではなく `open` に届きます。
- **アプリが終わらせる。** スキームを使わない場合は、アプリが受け取れる場所にページをリダイレクトさせます。ループバックインターフェースへのリダイレクト（`http://localhost:port/callback`、RFC 8252）を許す OAuth サーバーなら、そのポートでアプリが動かしているサーバーにコードを送ってきます。この 2 つを競わせると（`race(server.next(), webauth.open(url))`）、`open` がキャンセルされてシートが閉じます。Android では、キャンセルするとタブの上にアプリが戻ってきます。

ユーザーがシートを閉じると、`open` は `webauth.WebAuthError.canceled` を投げます。`ephemeral: true` を指定すると、ブラウザーの Cookie を使わないプライベートセッションを求めます（iOS では `prefersEphemeralWebBrowserSession`、Android ではブラウザーが対応していれば ephemeral な Custom Tab、Windows と Linux では使い捨てのプロファイル）。

## コピー、共有、URL を開く：device.copy、device.share、device.openURL

`platform/device` には、テキストをシステムに渡す関数が 3 つあります。

```asuka
import platform/device

component Links {
  Column {
    Button("Copy link") { on press { device.copy("https://example.com/s/42") } }
    Button("Share") { on press { device.share("https://example.com/s/42") } }
    Button("Open in browser") { on press { device.openURL("https://example.com") } }
    Button("Call") { on press { device.openURL("tel:+15555550100") } }
  }
}
```

- `device.copy(text)` はテキストをクリップボードに置きます。
- `device.share(text)` はシステムの共有シートを表示し、アプリ、連絡先、AirDrop、Nearby Share にテキストを送れるようにします。URL はリンクとして共有されます。Mac では共有サービスのピッカー、Windows では共有 UI、ブラウザーでは Web Share のシート（ある場合）が表示されます。Linux には共有シートがないので、テキストはクリップボードにコピーされ、トーストが表示されます（Web Share のないブラウザーでも同じくコピーされます）。
- `device.openURL(url)` は、URL を開くようシステムに頼みます。Web のアドレスはブラウザーで、それ以外のスキーム（`tel:`、`mailto:`、ほかのアプリのスキーム）はそれを扱うアプリで開きます。
- `device.openSettings()` は、システムの設定のうちこのアプリのページを開きます。ユーザーは、一度断った権限（カメラ、通知、位置情報）をそこでオンにできます。iOS ではアプリの設定ページ、Android では「アプリ情報」です。アプリごとのページがない Mac と Windows では「プライバシーとセキュリティ」を開きます。開くものがない場合（Linux、Web）は false を返します。

`device.shareFiles(paths, mime:, uti:, title:, from:)` は、ファイルの共有シートを表示します。送りたいビデオ、「ファイル」に保存したい PDF、AirDrop で送りたい写真などに使います。async 関数で、ユーザーが共有したかどうかにかかわらず、シートが閉じたあとに戻ります。シートを表示できなかった（ファイルが存在しない）場合はエラーを投げます。

```asuka
import platform/device

component ShareReport(report: string) {
  state status = ""
  let button = Ref()

  Column {
    Button("Share") {
      ref: button
      on press {
        do {
          try await device.shareFiles([report], mime: "application/pdf", title: "Send the report", from: button)
        } catch e {
          status = e.message
        }
      }
    }
    Text(status)
  }
}
```

オプションは expo-sharing と同じものです。

| オプション | |
| --- | --- |
| `mime` | ファイルのタイプです。シートに並ぶアプリはこれを見て判断します。Android ではインテントのタイプになります。iOS では `uti` が空のときにタイプとして使われます。既定：最初のファイルの拡張子から決めます。 |
| `uti` | iOS：ファイルの Uniform Type Identifier（`com.adobe.pdf`）です。`mime` より優先されます。 |
| `title` | シートのタイトルです。Android ではチューザーのタイトル、iOS ではメールの件名になります。 |
| `from` | シートがポップオーバーになる場合（iPad、Mac）に、シートが指し示すビューの `Ref` です。普通は共有ボタンです。既定：画面の中央。 |

iOS では、拡張子がタイプと合わないファイルは、expo-sharing と同じく、タイプに合う名前で共有されます。たとえば JPEG として共有した `image.pdf` は `image.pdf.jpeg` として送られます。これはコピーではなく、ファイルへのリンクです。Android では、ファイルはアプリの FileProvider を通じて渡されます。アプリ自身のフォルダーの外にあるファイルは、先にキャッシュにコピーされます。Mac では共有サービスのピッカーです。Windows では共有 UI です（Windows はいつ閉じたかを知らせないので、`device.shareFiles` は表示された時点で戻ります）。Linux には共有シートがないので、テキストの共有と同じく、ファイルはクリップボードに置かれます。ブラウザーでは、可能なら Web Share のシートでファイルを共有し、できなければファイルを一つずつダウンロードします。

アプリ自身を URL で開けるようにするには、[配布](shipping.md#asukatoml)の `asuka.toml` の `scheme` と `links`、そしてプレリュードの `onOpenURL`（または app ブロックの `on openURL(url)`）を参照してください。

## ファイルのプレビュー：platform/preview

`preview.show(sources)` は、メールアプリが添付ファイルを表示するように、システムのビューアーでファイルを表示します。PDF、画像、ビデオ、オーディオ、Office と iWork の書類、テキストに対応します。ソースには、ファイルのパス、アセットのパス（`Image` が受け取る形式）、http(s) の URL を指定できます。URL の場合は、先にアプリのキャッシュへ、元の名前で（またはサーバーが返すタイプの拡張子を付けて）ダウンロードされます。

```asuka
import platform/preview

component Invoice {
  prop path: string
  state status = ""

  Column {
    Button("Open the invoice") {
      on press {
        do {
          try await preview.show([path, "docs/terms.pdf"])
        } catch preview.PreviewError.noViewer {
          status = "Nothing on this device opens it"
        } catch e {
          status = e.message
        }
      }
    }
    Text(status)
  }
}
```

`start:` は最初に表示するファイルのインデックスです。`edit: true` を指定すると、ユーザーがマークアップで PDF や画像に書き込めるようになり、書き込みはファイルに保存されます（iOS）。ビューアーが閉じると戻ります。ファイルを表示できるものがない場合は `preview.PreviewError.noViewer` を、それ以外の問題（ファイルが存在しない、ダウンロードに失敗した）の場合はほかのエラーを投げます。

| | ビューアー | 戻るとき |
| --- | --- | --- |
| iOS | 画面の上に Quick Look（`QLPreviewController`）。スワイプでファイルを切り替え、共有、マークアップ、検索ができます | 閉じられたとき |
| Android | `start` のファイルを `ACTION_VIEW` で、そのタイプ用にユーザーが選んだアプリ（Drive の PDF ビューアー、フォト）で開きます。アプリの FileProvider を通じ、そのファイルだけの読み取り権限を付けて渡します | ユーザーが戻ってきたとき |
| macOS | Quick Look のパネル（`QLPreviewPanel`）。矢印でファイルを切り替えます | パネルが閉じたとき |
| Windows | ファイルの既定のアプリ（`ShellExecuteEx`）か、「このファイルをどのように開きますか？」ダイアログ | 渡し終えたとき |
| Linux | ファイルの既定のアプリ（`GtkFileLauncher`、サンドボックス内ではポータルのもの） | 渡し終えたとき |
| Web | ブラウザーが表示できるもの（画像、ビデオ、オーディオ、テキスト、PDF、HTML）は新しいタブで開きます。それ以外のタイプは `noViewer` です | 開いたとき |

Android にはシステムのビューアーも、複数のファイルをめくれるビューアーもありません。そのため、Android のメールアプリが添付ファイルを一つずつ開くのと同じく、一つのファイルだけを開きます。iOS でビューアーが広がり出てくる元のビュー（`device.shareFiles` の `from:` のような `Ref`）を、`preview.show` はまだ受け取れません。

## PDF の表示：platform/pdf

`pdf.View` は、PDF をアプリの画面の中に、ほかのビューと並べて表示します。ページは上から下に並び、ビューの幅に合わせて表示され、スクロールしたりピンチで拡大したりできます。`source` には、ファイルのパス、アセットのパス（`Image` と同じ書き方）、http(s) の URL（先にダウンロードされます）を指定します。

```asuka check
import platform/pdf

component Manual {
  state page = 0
  state pages = 0

  Column {
    grow: 1
    pdf.View {
      source: "docs/Manual.pdf"
      bind page: page
      grow: 1
      on loaded(n) { pages = n }
    }
    Row {
      justify: .spaceBetween
      padding: 12
      Button("Previous") { on press { page = max(0, page - 1) } }
      Text("{page + 1} of {pages}")
      Button("Next") { on press { page = min(pages - 1, page + 1) } }
    }
  }
}
```

`page` は表示中のページで、0 から数えます。設定するとそのページまでスクロールし、バインドするとユーザーのスクロールに追従します（`pageChange` でも通知されます）。表示されると `loaded` がページ数を伝え、表示できないときは `error` が理由（ファイルがない、PDF ではない、暗号化されている、ダウンロードに失敗した）を伝えます。

| | 表示に使うもの |
| --- | --- |
| iOS、macOS | PDFKit の `PDFView` |
| Android | `PdfRenderer` が描いたページを、スクロールと拡大（ダブルタップ、ピンチ）ができるリストに並べたもの |
| Windows | `Windows.Data.Pdf` が描いたページ |
| Linux | Poppler（`libpoppler-glib8`）が描いたページ（ディストリビューションに Poppler がある場合） |
| Web | ブラウザー自身のビューアー。ビューアーのないブラウザー（ほとんどのスマートフォンのもの）では、PDF を開くリンクが表示され、`loaded` のページ数は 0 です |

ファイルをシステムのビューアーに渡したい場合は `platform/preview` を、PDF を作る場合は `print.pdf` を使います。

## 印刷と PDF：platform/print

`platform/print` は、ファイルや HTML マークアップに対して、プリンターとオプション（部数、ページ、用紙、向き。Android と Mac では「PDF として保存」も）を備えたシステムの印刷ダイアログを表示します。また、HTML をページに割り付けて PDF にすることもできます。印刷の呼び出しはどれも、ダイアログが閉じると `print.Outcome` を返します。このパッケージの名前はプレリュードの `print` と同じなので、テキストの出力にも `print` を使いたいときは、別の名前でインポートしてください。

```asuka
import platform/print as printing

async fn printReceipt(receipt: string) throws -> string {
  let r = try await printing.html(receipt, job: "Receipt")
  return if r == .printed { "Sent to the printer" } else { "" }
}
```

- `print.file(source, job:, orientation:)` は PDF か画像を印刷します（ソースは `preview.show` と同じく、パス、アセット、http(s) の URL）。それ以外のタイプは `print.PrintError.unsupported` を投げます。ただし Windows と Linux は例外です。Windows ではファイルに対応するアプリが印刷し、Linux ではプリンターが CUPS で印刷できるものを受け付けます。
- `print.html(markup, job:, orientation:)` はマークアップを、ブラウザーがページを印刷するのと同じようにページ分けして印刷します。CSS の `@media print` と `break-before` が効きます。画像とスタイルは絶対 URL（`https:`、`data:`）から読み込みます。
- `print.text(s, job:, mono:, orientation:)` はプレーンテキストを、アプリが表示するレポートやメモのように印刷します。行と空行はそのまま、長い行は折り返し、`job` を見出しにします。`mono: true` にすると、列にそろえたテキスト向けにプラットフォームの等幅フォントを使います。
- `print.webView(view, job:, orientation:)` は、`WebView` が今表示しているページ（ユーザーやページのスクリプトが加えた変更も含めて）を、そのブラウザーがページを印刷するようにページ分けして印刷します。`view` は WebView を入れた `Ref`（`ref:`）です。アプリが表示している WebView を Ref が持っていなければ、`print.PrintError.unsupported` を投げます。

```asuka
import platform/print as printing

let page = Ref()

component Article(url: string) {
  Screen {
    title: "Article"
    toolbar {
      Button("Print") {
        icon: "printer"
        on press { try? await printing.webView(page, job: "Article") }
      }
    }
    WebView(url: url) {
      ref: page
      grow: 1
    }
  }
}
```

`job` はキューに表示されるジョブ名です（既定はファイル名、ページのタイトル、どちらもなければアプリ名）。`orientation` はダイアログを開いたときの向きです（`.portrait`、`.landscape`）。

`Outcome` は、`.printed`（ジョブがプリンターのキューに入った、またはダイアログから PDF として保存された）、`.cancelled`、プラットフォームが結果を知らせない場合の `.unknown` のいずれかです。失敗したジョブはエラーになります。`print.PrintError.unavailable` は、デバイスが印刷できないことを示します。

| | ダイアログ | ファイル | HTML | 結果 |
| --- | --- | --- | --- | --- |
| iOS | `UIPrintInteractionController`。iPad ではポップオーバー | PDF と画像（`printingItem`） | WebKit の print formatter | 印刷またはキャンセル |
| Android | `PrintManager` の印刷画面 | PDF はそのまま書き出し、画像は向きを直してページに合わせます（`PrintHelper` と同じ） | WebView の print adapter | 印刷、キャンセル、失敗（ジョブの状態） |
| macOS | `NSPrintOperation` のパネル。ウィンドウ上のシートで、プレビュー付き | PDFKit（PDF、または 1 ページに置いた画像） | WebKit の print operation | 印刷またはキャンセル |
| Windows | WebView2 のもの。システムの印刷ダイアログ | タイプにかかわらず、そのファイルのアプリが印刷します（シェルの `print` verb） | WebView2 | 不明 |
| Linux | GTK の印刷ダイアログ（ファイルの場合は印刷ポータルのもの） | プリンターが受け付けます（CUPS：PDF、画像） | WebKitGTK の print operation | 印刷またはキャンセル |
| Web | ブラウザーの印刷ダイアログ | ブラウザーのビューアーで表示した PDF、画像 | 非表示の frame | 不明 |

Android と iOS では、写真の印刷と同じく、横長の画像は横向きで始まります。WebView のページは WebView 自身の印刷機能で印刷します。iOS ではその print formatter、Android ではその print adapter、Mac と Linux では WebKit の print operation、Windows では WebView2 の印刷ダイアログです。ブラウザーでは、アプリと同じオリジンのページ（`html:` の文字列から作ったページなど）は、その frame からページ全体を印刷します。ほかのサイトのページにはアプリのページから手が届かないので、その WebView だけを残したアプリのページを、表示されているとおりに印刷します。`platform/print` はまだ、iPad のポップオーバーが指し示すビューを受け取れません。

### HTML から PDF を作る

`print.pdf(markup, page:, landscape:, margins:)` は、`print.html` と同じようにマークアップをページに割り付けますが、ダイアログは出さず、アプリのキャッシュに PDF を書き出します。ファイル名はページの `<title>` から付けられます（"Receipt.pdf"。`<title>` がなければ "Document.pdf"）。戻り値はそのパスで、共有したり、メールで送ったり、移動したりできます。テキストはテキストのまま残るので、検索も選択もできます。

```asuka
import platform/print as printing
import platform/preview

async fn receiptPDF(receipt: string) throws {
  let path = try await printing.pdf(receipt, page: printing.Page.a4, margins: printing.Margins(top: 48, right: 48, bottom: 48, left: 48))
  try await preview.show([path])
}
```

| パラメーター | |
| --- | --- |
| `page` | 用紙の大きさ（ポイント単位）：`Page.a3`、`a4`、`a5`、`letter`、`legal`、または独自のサイズ。既定：ユーザーの地域の用紙です（`print.regionPage(environment().locale)`）。Letter を使う南北アメリカの国では Letter（CLDR の用紙サイズ）、それ以外では A4 です。 |
| `landscape` | ページを横向きにし、幅を長い辺にします。 |
| `margins` | 各ページの内容の周りの余白（ポイント単位）。既定：各辺 0.5 インチ（36 ポイント）。 |

レイアウトは、iOS では WebKit（`UIPrintPageRenderer` と Web ビューの print formatter）、Android では WebView の print adapter、Mac では WebKit の print operation、Windows では WebView2（Chromium の `Page.printToPDF`）、Linux では WebKitGTK（ファイルへの印刷）で行います。ブラウザーは印刷ダイアログを通してしか PDF を作れないので、`pdf` は `unsupported` エラーを投げます。代わりに `print.html` を使えば、ユーザーが「PDF として保存」を選べます。

## メールとメッセージ：platform/compose

`platform/compose` は、システムのメールとメッセージの作成画面を、内容を入れた状態で開きます。送信するのはユーザーです。ユーザーが操作を終えると `compose.Result` を返します。`.sent`、`.saved`（下書き）、`.cancelled`、またはメールをほかのアプリが扱っていて結果を知らせない場合の `.unknown` のいずれかです。

```asuka
import platform/compose

async fn mailReceipt(receiptPDF: string) -> string {
  do {
    let r = try await compose.mail(
      to: ["support@example.com"],
      subject: "Order 1042",
      body: "<p>Hello,</p><p>The receipt is attached.</p>",
      html: true,
      attachments: [compose.Attachment(source: receiptPDF, name: "Receipt 1042.pdf")],
    )
    return if r == .sent { "Sent" } else { "" }
  } catch e {
    return if e.message == "compose: unavailable" { "Set up a mail account to send it" } else { e.message }
  }
}

async fn onMyWay() throws {
  let _ = try await compose.message(to: ["+15555550100"], body: "On my way")
}
```

`compose.mail` は、宛先の `to`、`cc`、`bcc`、件名の `subject`、本文の `body`（`html` なら HTML マークアップ）、`attachments` を受け取ります。`compose.message` は、`to`（電話番号か iMessage のアドレス）、`subject`（「メッセージ」で件名がオンになっている場合。そうでなければ本文の前に入ります）、`body`、`attachments` を受け取ります。`Attachment` は、`source`（パス、アセット、http(s) の URL）、`type`（既定は拡張子から決まるタイプ）、受信者に表示される `name`（既定はファイル名）からなります。メールがマークアップを運べない場合（`mailto:` リンク、Simple MAPI）、本文は `compose.plainText(html)` で作ったものと同じテキストとして送られます。

`compose.check()` はデバイスで何ができるかを返します。`mail` はメールのアカウントかアプリがあること、`mailComposer` は iOS 自身の作成画面があること（ユーザーの操作の結果が分かり、添付ファイルも扱えます）、`text` はテキストメッセージ、`textAttachments` は添付ファイル付きのメッセージを送れることを表します。デバイスでできない呼び出しは「compose: unavailable」を投げます。

| | メール | メッセージ | 結果 |
| --- | --- | --- | --- |
| iOS | アプリの上に「メール」の作成画面（`MFMailComposeViewController`）。「メール」のアカウントがなければ、ユーザーが設定したメールアプリ向けの `mailto:` リンク（添付ファイルなし） | 「メッセージ」の作成画面（`MFMessageComposeViewController`） | 送信、保存、キャンセル。リンクの場合は不明 |
| Android | メールアプリだけを対象に、`ACTION_SENDTO` の `mailto:`、添付ファイルがあれば `mailto:` をセレクターにした `ACTION_SEND`。ファイルはアプリの FileProvider を通じて渡します。マークアップは span（`EXTRA_TEXT`）と `EXTRA_HTML_TEXT` の両方で渡します | 既定の SMS アプリ：`sms_body` を付けた `smsto:`、添付ファイルがあれば `ACTION_SEND` | ユーザーが戻ってきた時点で不明 |
| macOS | ユーザーのメールアプリ向けの `mailto:` リンク。添付ファイルがあればメールの共有サービス（`NSSharingService`）。これは CC と BCC を受け付けません（指定すると「compose: unavailable」を投げます） | 「メッセージ」（`NSSharingService`） | 不明、またはキャンセル |
| Windows | `mailto:` リンク。添付ファイルがあれば Simple MAPI（`MAPISendMailW`：Outlook、Thunderbird。新しい Outlook と Windows の「メール」アプリは対応していないので利用不可） | 利用不可 | 不明、またはキャンセル（MAPI） |
| Linux | メールポータル（メールアプリの作成画面に添付ファイル付きで）、なければ `xdg-email`、それもなければ `mailto:` リンク | 利用不可 | 不明、またはキャンセル（ポータル） |
| Web | `mailto:` リンク。添付ファイルがあれば Web Share のシート | `sms:` リンク。添付ファイルがあれば Web Share のシート | 不明 |

iOS シミュレーターには「メール」のアカウントも「メッセージ」もないので、作成画面は実機で試してください。

## 位置情報：platform/location

`platform/location` は、ユーザーの許可を得て、デバイスの現在地と移動中の位置を取得します。許可は初回に求めます。iOS と Mac では Core Location、Android では Android の位置情報マネージャー（Google Play 開発者サービスは不要）、Windows では WinRT の Geolocator、Linux では位置情報ポータル（または GeoClue）、ブラウザーでは Geolocation API を使います。アプリが画面に出ていないあいだも、バックグラウンドでデバイスの位置を追い、アプリの代わりに領域を監視できます（後述）。

`location.current()` は、まだ許可を求めていなければ求めてから、デバイスの現在地を返します。ユーザーが許可していなければ `LocationError.denied` を投げます。

```asuka
import platform/location

component Where {
  state where = ""

  Column {
    Button("Where am I?") {
      on press {
        do {
          let p = try await location.current()
          where = "{p.latitude.fixed(4)}, {p.longitude.fixed(4)} (within {p.accuracy.fixed(0)} m)"
        } catch location.LocationError.denied {
          where = "Allow location in Settings"
        } catch e {
          where = e.message
        }
      }
    }
    Text(where)
  }
}
```

`location.positions()` はストリームで、まず現在地を、その後はデバイスが動くたびにその位置を流します。タスクの中で読めば、コンポーネントとともに止まります。

```asuka
import platform/location

component Track {
  state here = location.Position()

  task {
    do {
      for await p in location.positions() { here = p }
    } catch e {
      print(e.message)
    }
  }

  Text("{here.latitude.fixed(5)}, {here.longitude.fixed(5)}")
}
```

`location.placeName(latitude, longitude)` は、その場所の名前（"Mission District, San Francisco"）を返します。プラットフォームのジオコーダーがサーバーに問い合わせるので、ユーザーが名前を求めたときだけ呼んでください。Linux では、geocode-glib がインストールされていればそれに問い合わせます。Windows とブラウザーにはジオコーダーがないので、エラーを投げます。

`location.Position` は、`latitude`、`longitude`、`accuracy`（メートル単位。デバイスがその半径内にいることを表します）、`altitude`（海抜のメートル数。分からなければ 0）、`speed` と `course`（分からなければ負の値）、`time`（`time.Time`）を持ちます。リージョンの出入りは、`region`、`kind`、`at`（システムが知らせたときの位置）、`time` を持ちます。

### 許可のレベル

ユーザーは位置情報の利用を、アプリの使用中だけ（`.whenInUse`）か、常に（`.always`）許可します。領域の監視と、アプリが動いていないときのイベントには `.always` が必要です。ユーザーはさらに、正確な位置か、おおよその位置（数 km 以内。iOS の「正確な位置情報」、Android の「おおよそ」）かも選びます。`location.authorization()` はユーザーが許可した内容を、追跡される値として返します。バインディングの中で読めば、ユーザーが「設定」で加えた変更に追従します。

```asuka
import platform/location

component Allowed {
  let auth = location.authorization()

  Column {
    Text("Allowed: {auth.level}; precise: {auth.precise}{if auth.off { "; location services are off" } else { "" }}")
    Button("Allow always") { on press { let _ = try? await location.requestAlways() } }
    Button("Location settings") { on press { try? await location.openSettings() } }
  }
}
```

- `location.request()` は、初回に `.whenInUse` を求めます。`.denied` になったあとで求め直しても何も表示されません。変えられるのは「設定」だけです（`location.openSettings()`）。
- `location.requestAlways()` は、（`.whenInUse` のあとで）`.always` を求めます。iOS が尋ねるのは一度だけです。Android 11 以降では、ユーザーはアプリの位置情報の設定画面に移動し、そこで「常に許可」を選びます。
- `location.requestPrecise(purpose)` は、おおよその位置だけを許可したユーザーに、正確な位置を求めます。iOS では今回だけ（`purpose` は asuka.toml の `ios.locationPurposes` のキーです）、Android では以後ずっと有効な許可を求めます。

デスクトップのアプリは、許可されると `.always` になります。もともと画面に出ていなくても動き続けるからです。Linux とブラウザーは、アプリが位置を求めるまで何も伝えないので、位置が届くまでは `.notAsked` です。

### バックグラウンドでの更新

`location.startUpdates(…)` は、アプリが画面に出ていないあいだも、起動をまたいでも続く更新を開始します（iOS と Android では、更新のためにシステムがアプリを起動します）。更新は `location.stopUpdates()` を呼ぶまで続きます。`location.updates()` はその更新のストリームで、`Batch`（`positions` を古い順に持ちます）を流します。アプリの起動時に読んでください。そうすれば、システムがアプリを起動するきっかけになったバッチにも、受け取り手が見つかります。

```asuka
import platform/location

async fn track() throws {
  try await location.startUpdates(
    accuracy: .high,
    distance: 20,
    activity: .fitness,
    delay: 30s,
    indicator: true,
    notification: location.Notification(title: "Tracking your route", text: "Your run is being recorded"),
  )
}

async fn followUpdates() throws {
  for await b in location.updates() {
    let done = b.hold()   // 送信が終わるまでアプリを動かし続ける
    defer { done() }
    print("{b.positions.count()} positions")
  }
}
```

`accuracy` は正確さと消費電力のどちらを優先するかを決めます（`.high`、`.navigation`、`.balanced`、`.low`、`.passive`）。`distance` は二つの更新のあいだにデバイスが動く距離、`interval` は更新が届く間隔です（Android のもの）。`activity` は iOS の更新（と、`autoPause` を指定したときの一時停止）を調整します。`delay` を指定すると更新がまとめて届くので、アプリが起こされる回数が減ります。`significant` を指定すると、大きな変化（約 500 m）だけを、ほとんど電力を使わずに受け取ります。iOS では、ユーザーがアプリを終了したあとでも、この更新がアプリを起動します。`indicator` を指定すると、アプリが画面に出ていないあいだに更新を受け取るとき、iOS の青いピルが表示されます。Android では、`notification` を指定すると位置情報タイプのフォアグラウンドサービスが動き、これがそのサービスの通知になります。これがあれば、アプリが画面に出ていなくても、更新は指定した頻度で届きます。`location.updating()` は、更新が動いているかどうかを返します（以前の起動で開始されている場合もあります）。

デスクトップとブラウザーでは、アプリの実行中に監視した位置が更新として届きます。

### 領域

`location.monitor(region)` は、`location.stopMonitoring(id)` を呼ぶまで、アプリの代わりに円形の領域を監視します。領域への出入りは `location.crossings()` から届きます。アプリがバックグラウンドにあっても、動いていなくても届きます（iOS と Android がアプリを起動します）。監視を始めたときにデバイスが領域の中にいれば、`.enter` が届きます。iOS と Android では `.always` が必要で（なければ `LocationError.needsAlways`）、システムの上限を超えると `tooManyRegions` を投げます。

```asuka
import platform/location

async fn watchHere() throws {
  let p = try await location.current()
  try await location.monitor(location.Region(id: "here", latitude: p.latitude, longitude: p.longitude, radius: 150, events: [.enter, .exit]))
}

async fn followCrossings() throws {
  for await c in location.crossings() {
    print("{c.kind} {c.region}")
  }
}
```

`location.supported()` は、システム自身が何をするかを返します。`regions`（iOS、Android、macOS、Windows はシステムが領域を監視します。Linux とブラウザーでは、アプリの実行中にこのパッケージが位置から計算します）、`maxRegions`（iOS と macOS では 20）、`dwell`（`.dwell` が届くかどうか）、`relaunch`（イベントのためにシステムがアプリを起動するかどうか）です。

### アプリが動いていないときのイベント

システムは、アプリが読むまでバッチや出入りのイベントを保持します。アプリの終了と同時に届いたものは、次の起動時にもう一度渡されます。イベントが届くと、アプリは数秒間動き続けます。`b.hold()` と `c.hold()` を使うと、それが返す関数を呼ぶまで（またはシステムの制限時間まで。iOS では約 30 秒、Android では約 10 秒）アプリを動かし続けられます。イベントを読んだあとも続く処理（サーバーへの送信など）に使います。

### テスト

`location.fake()` を呼ぶと、パッケージはプラットフォームの代わりに、返される `FakeLocation` を使います。`answer` は許可を求められたときのユーザーの答えです。`move(latitude, longitude)` はデバイスを別の場所に移します（位置を読んでいるコードはその位置を受け取り、更新が動いていればバッチが届き、入った領域や出た領域では出入りのイベントが起きます）。`allow(level)` はユーザーが「設定」で許可を変えることを表し、`pending` は `hold()` が終わっていないイベントの数です。

```asuka
import platform/location

test "a region, crossed" {
  let loc = location.fake()
  loc.answer = .always
  loc.move(35.6812, 139.7671)
  let _ = try await location.requestAlways()
  try await location.monitor(location.Region(id: "here", latitude: 35.6812, longitude: 139.7671, radius: 150))
  loc.move(35.70, 139.7671) // 北へ 2 km：領域から出る
}
```

## 通知：platform/notify

`platform/notify` はローカル通知を、すぐに、またはアプリが決めた時刻に表示します。その時刻にアプリが動いているかどうかは問いません。通知をタップすると、その URL でアプリが開きます。メッセージングアプリに必要な機能もそろっていて、`platform/push` の通知にも使えます。通知内で入力するアクションと返信、スレッド、Android のチャンネル、会話のメッセージ、バッジ、表示中の通知、アプリを開いているあいだにどの通知を表示するか、です。

```asuka
import platform/notify

async fn remindToWater() throws {
  if !(try await notify.request()) {
    return
  }
  try await notify.schedule(notify.Notification(
    id: "water",
    title: "Water the plants",
    body: "It's been a day.",
    url: "myapp://plants",
    after: 24h,
  ))
}
```

- `notify.request()` は、初回にユーザーへ通知の許可を求め、許可されているかどうかを返します。そのときのユーザーの答え、またはその後ユーザーが「設定」で変えた状態です。
- `notify.schedule(n)` は、今から `n.after` 後（1 秒以上）に `n` を表示するよう予約します。`notify.show(n)` はすぐに表示します。待機中または表示中の通知と同じ `id` を持つ通知は、それを置き換えます。どちらも非同期です。
- `notify.cancel(id)` は、待機中か表示済みかにかかわらず通知を取り消します。

`Notification` の `url` には、アプリのスキーム（`asuka.toml` の `scheme`）の URL を指定してください。ユーザーが通知をタップすると、アプリの `openURL` がそれを受け取ります。ほかのフィールドは次のとおりです。

| フィールド | 役割 |
|---|---|
| `subtitle` | タイトルの下の 1 行（iOS）。Android ではアプリ名の横のサブテキスト |
| `data` | アプリ独自のキー。`showing`、`responses`、`present` でそのまま返されます |
| `category` | カテゴリーの ID。そのカテゴリーのアクションが表示されます（下記） |
| `thread` | 同じスレッドのほかの通知とまとめます。iOS ではスレッド、Android では Asuka が管理するサマリー通知付きのグループです |
| `summary` | Android のグループのサマリーの下に出る行（"3 new messages"）。iOS は独自の文を書きます |
| `channel` | 通知が属する Android のチャンネル（下記）。iOS にはありません |
| `badge` | 通知が表示されたときのアプリアイコンの数字（iOS、macOS）。Android では通知の数 |
| `silent` | 音を鳴らしません |
| `image` | 画像。ソースは `Image` と同じ（アセット、ファイル、http(s) の URL）です。iOS では添付ファイル、Android では BigPicture になります |

**iOS** は、`present` で別の指定をしない限り、アプリを開いているあいだも通知をバナーで表示します。**Android** は予約した通知を不正確なアラーム（exact alarm の権限は不要）で投稿するので、予定の時刻より少し遅れることがあります。

### アクションと返信

カテゴリーは、通知の種類とそのボタンを表します。アプリのカテゴリーは起動時に、カテゴリーを指定した通知（プッシュ通知も含みます。iOS では `aps.category`、Android ではデータの `"category"`）が届く前に設定してください。設定は起動をまたいで保持されます。

```asuka
import platform/notify

component Chats {
  task {
    notify.setCategories([notify.Category(id: "message", actions: [
      notify.Action(id: "reply", title: "Reply", icon: "arrowshape.turn.up.left",
        reply: notify.Reply(button: "Send", placeholder: "Message")),
      notify.Action(id: "read", title: "Mark as Read"),
      notify.Action(id: "delete", title: "Delete", destructive: true, unlock: true),
      notify.Action(id: "open", title: "Open", open: true),
    ])])

    for await r in notify.responses() {
      match r.action {
        "" => openChat(r.notification.thread)   // 通知そのもののタップ
        "reply" => {
          let done = r.defer()                   // 送信には少し時間がかかる
          defer { done() }
          try? await send(r.notification.thread, r.reply)
        }
        "read" => notify.removeThread(r.notification.thread)
        _ => {}
      }
    }
  }

  Text("Chats")
}

// アプリ独自のもの。
fn openChat(thread: string) {}
async fn send(thread: string, text: string) throws {}

app { Chats() }
```

- `Action` は、`id` と `title`、`icon`（SF Symbol。iOS 15 と macOS 12 で表示されます。Android のアクションにはアイコンが表示されません）、`destructive`（iOS では赤で表示されます）、`unlock`（先にデバイスのロックを解除させます。iOS、Android 12 以降）、`open`（アプリを開きます）、`reply`（テキスト欄。送信ボタンのタイトルとプレースホルダーを指定します）を持ちます。
- `notify.responses()` は、ユーザーの操作のストリームです。操作は、タップ（`action` が `""`）かアクションです。`reply` は入力されたテキスト、`notification` は応答の対象になった通知（`platform/push` の通知も含みます）です。だれもストリームを読まないうちに来た応答（アプリを起動した応答や、アプリを起こした応答）は最初の読み手を待つので、アプリの起動時に、ルートのタスクの中で読んでください。
- `open` のないアクションはバックグラウンドで実行されます。アプリは動きますが（動いていなければ画面なしで起動され、ルートとそのタスクが実行されます）、前面には出てきません。応答を読んだあとも続く処理がある場合は、`r.defer()` を呼び、処理が終わったらそれが返す関数を呼びます。システムはそれまで、または制限時間まで（iOS では約 30 秒、Android では約 10 秒）アプリを動かし続けます。

**Android** は、iOS と同じく、バックグラウンドのアクションが終わるまでにアプリがその通知を（同じ `id` で `show` して）表示し直さなければ、通知を消します。返信のあと、Android は通知がもう一度投稿されるのを待ちます（それまでスピナーが回り続けます）。そのため Asuka は、会話の通知にはユーザーの名前で返信を加え、それ以外の通知は消します。

### スレッドと表示中の通知

`thread` はチャットの通知をまとめます。iOS では通知を重ねて表示し、Android ではサマリー通知の下のグループにまとめます。このサマリー通知は、通知の出入りに合わせて Asuka が管理します。

- `notify.showing()` は、表示中（通知センター、通知シェード）のアプリの通知を、プッシュ通知も含めて新しい順に返します。各 `Delivered` は、`id`、`title`、`subtitle`、`body`、`url`、`data`、`category`、`thread`、`time`（`time.Time`）、そしてプッシュ通知かどうかを示す `remote` を持ちます。
- `notify.remove(ids)`、`notify.removeThread(thread)`（読み終えたチャットに使います）、`notify.removeAll()` は通知を消します。

### アプリを開いているあいだに表示する通知

```asuka
import platform/notify

component Chat(id: string) {
  effect(id) {
    notify.present(n => if n.thread == id { .hide } else { .banner })   // そのチャットが画面に出ている
    cleanup { notify.stopPresenting() }
  }

  Text("Chat")
}
```

`notify.present(decide)` を使うと、アプリがフォアグラウンドにあるあいだに届いた各通知をどう表示するかを `decide` で決められます。`.banner`（アプリを閉じているときと同じ）、`.quiet`（バナーも音もなしで、通知センターや通知シェードに入れる）、`.hide` のいずれかです。`decide` は通知が届いたときに呼ばれ、すぐに答えを返す必要があります。Asuka は最大 1 秒待ち、それを過ぎると通知を表示します。`notify.stopPresenting()` を呼ぶと、ふたたびすべての通知を表示します。

### 会話のメッセージ

メッセージは、プラットフォームがメッセージを表示するのと同じ形で、送信者の写真付きで表示されます。iOS では通信通知（communication notification）、Android では会話（通知シェードの「会話」セクションに、会話のショートカット付きで表示）です。

```asuka
import platform/notify

async fn whosIn() throws {
  let ana = notify.Person(id: "u7", name: "Ana", image: "https://example.com/ana.jpg")
  let chat = notify.Conversation(id: "climbing", title: "Climbing", group: true)
  try await notify.show(notify.Notification(body: "Who's in for Saturday?", category: "message").from(ana, chat))
}
```

- `n.from(sender, c)` は `n` をメッセージにします。`sender` が `c` で `n.body` を発言した、という意味になります。`id` と `thread` の既定値は会話の `id`（1 対 1 の会話では送信者の `id`）、`title` の既定値は送信者の名前（グループではグループのタイトル）です。
- `Person` は、`id`（毎回同じもの）、`name`、`image`（ソースは `Image` と同じ）、`bot` を持ちます。`Conversation` は、`id`、グループの `title` と `image`、`group`、`me`（会話の中でのユーザーの表示。Android はユーザーの返信にこの名前を付けます。既定は "You"）、それ以前のメッセージ `earlier`（それぞれ `Message` で、`from`、`text`、`time` を持ちます。`time` は `time.Time?` で、分からなければ none です）を持ちます。
- **Android** は一つの会話を一つの通知として表示し、メッセージをその中に並べます。新しいメッセージは表示中の会話の通知に加わります（または `earlier` でそれより前のメッセージを設定します）。**iOS** はメッセージを一つずつ別の通知として、会話のスレッドの中に表示します。
- **iOS** では、App ID で Communication Notifications ケイパビリティを有効にする必要があります。`from` を呼ぶアプリにはそのエンタイトルメントと、Info.plist の `NSUserActivityTypes`（`INSendMessageIntent`）が加わります。リリースビルドのプロファイルにもこのケイパビリティが含まれていなければなりません。含まれていない場合、メッセージは普通の通知として表示されます。**macOS** はメッセージを普通の通知として表示します。

### Android の通知チャンネル

チャンネルは通知の種類の一つで、ユーザーはアプリの設定でチャンネルごとにオフにしたり、音を消したりできます。Android は通知を、そのチャンネルの音、バイブレーション、重要度で表示します。iOS にはチャンネルがないので、無視されます。

```asuka
import platform/notify

fn makeChannels() {
  notify.setChannelGroups([notify.ChannelGroup(id: "chats", name: "Chats")])
  notify.setChannels([
    notify.Channel(id: "messages", name: "Messages", importance: .high, group: "chats"),
    notify.Channel(id: "digest", name: "Daily digest", importance: .low, noBadge: true),
  ])
}
```

- `notify.setChannels(cs)` はチャンネルを作り、以前に作ったチャンネルの名前を更新し、以前に作ったもののうち `cs` にないチャンネルを削除します。チャンネルの `importance`（`.default`、`.high`：ヘッドアップ通知、`.low`：音なし、`.min`）、`silent`、`noVibration`、`noBadge` は作成時に決まり、その後はユーザーが変えるものになります。`notify.setChannelGroups(gs)` はグループに対して同じことをします（グループを削除すると、そのチャンネルも削除されます）。
- `channel` を指定しない通知、または存在しないチャンネルを指定した通知は、既定のチャンネル（"Notifications"）に入ります。既定のチャンネルは削除されません。プッシュ通知では、FCM の `android.notification.channel_id`、またはデータメッセージの `"channel"` でチャンネルを指定します。

### バッジ

`notify.setBadge(n)` はアプリのアイコンの数字を設定します。`0` を渡すと消えます。**iOS** と **macOS**（Dock）は設定したとおりに表示します。**Android** には独自のバッジがありません。バッジを表示するランチャーはアプリの通知を数える（またはドットを表示する）ので、通知が読まれる（`remove`、`removeThread`）とバッジは消え、`setBadge` は何もしません。通知の `badge` は、ランチャーが表示するかもしれない数字です。`noBadge` を指定したチャンネルの通知は数えられません。

### プラットフォームごとの動作

| | iOS | Android | macOS | Windows | Linux | Web |
|---|---|---|---|---|---|---|
| 表示、予約、取り消し | ✓ | ✓ | ✓ | ✓（トースト） | ✓（予約：systemd のユーザータイマー。アプリが動いているかどうかは問わない） | ✓（予約：アプリのページが開いているあいだ） |
| アクション、返信 | ✓ | ✓ | ✓ | ✓（ボタン 5 つ） | アクション（返信するとアプリが開く） | ブラウザーが表示する場合はボタン（Chrome、Edge：2 つ）。インライン返信がある場合は返信 |
| バックグラウンドのアクション | ✓（約 25 秒） | ✓（約 9 秒） | ✓ | ✓（約 25 秒。そのためにアプリが起動される） | アプリの実行中 | アプリのページの中（Service Worker がページを開く） |
| スレッド | ✓ | ✓（グループとサマリー） | ✓ | まとめない。`removeThread` は使える | ✓ | まとめない。`removeThread` は使える |
| チャンネル | 無視 | ✓ | 無視 | 無視 | 無視 | 無視 |
| 会話 | ✓（通信通知） | ✓（MessagingStyle、ショートカット） | 普通の通知 | 普通の通知 | 普通の通知 | 普通の通知。送信者の写真がアイコンになる |
| バッジ | ✓ | ランチャーのもの | ✓（Dock） | タスクバーボタンのオーバーレイ | ランチャー API のもの（Dash to Dock、Plasma） | インストールしたアプリのもの（`navigator.setAppBadge`） |
| 表示中の通知の取得、削除 | ✓ | ✓ | ✓ | ✓（通知センターのもの） | 今回の実行でアプリが表示したもの | ✓（Service Worker のもの） |
| `present` | ✓ | ✓ | ✓ | ✓（予約した通知を除く） | ✓ | ✓ |

**macOS** ではアプリのバンドルが必要です。`asuka dev` と `asuka build` はバンドルからアプリを実行します。単体で実行した実行ファイルには bundle ID がないので、呼び出しはその旨を示すエラーを投げます。**Windows** はトーストを、すぐに、または予約して表示します。画像も表示でき、カテゴリーのアクションはボタン、返信はテキストボックスになります。アプリは初回に自分自身を登録します（AppUserModelID、つまり asuka.toml の `id` を持つ「スタート」メニューのショートカットと、COM のアクティベーター。どちらもユーザーのレジストリに、アプリの実行ファイルを指して登録します）。これにより、タップやボタンの操作がアプリに届き、アプリが動いていなければ Windows がアプリを起動します。バックグラウンドのアクションでは、アプリは見えないまま動き、応答を処理し終えると終了します。バッジは、アプリのタスクバーボタンに描かれる数字です（パッケージ化されたアプリでは、システムのバッジ）。Windows は、予約したトーストを `present` の答えにかかわらず表示します。**Linux** は、アプリにデスクトップファイルがある（またはサンドボックスで動いている）場合は GNotification として通知を送り、そうでなければデスクトップの通知サーバーに直接送ります（GNOME Shell は、デスクトップファイルのないアプリの通知を受け付けません）。タップやボタンの操作は、アプリの実行中にアプリに届きます。後で表示する通知は systemd のユーザータイマー（`systemd-run --user`）になります。タイマーはアプリの実行ファイルをウィンドウなしで起動して通知を表示し、アプリが動いていればアプリに処理を渡し、タップされればアプリを起動します。systemd がない場合（Flatpak、`asuka dev`）は、アプリの実行中にしか表示されません。**ブラウザー**は、アプリの Service Worker を通じて通知を表示し、`platform/push` には Web Push を使います。詳しくは [Web](web.md) を参照してください。

## サウンド：platform/audio

`audio.play(source)` はサウンドを一度再生します。ソースはアプリの assets にあるファイル、ファイルのパス、http(s) の URL のどれかです。`audio.Player(source)` は一つのサウンドを持つストアで、作った時点で再生できます。`play`、`pause`、`seek(seconds:)`、`setVolume`、`setRate`（音程を保つので音声向き）、`setLoop`、そして再生の状態を表す `status()`（`position`、`duration`、`playing`、`ended`）があります。作ったコンポーネントがなくなるとき、または `release()` を呼んだときに、停止してサウンドを解放します。

```asuka
import platform/audio

component Episode(url: string) {
  state player = audio.Player(url)
  state status = audio.Status()

  task {
    while true {
      status = await player.status()
      await sleep(250ms)
    }
  }

  Column {
    Text("{status.position.fixed(0)} / {status.duration.fixed(0)} s")
    Button(if status.playing { "Pause" } else { "Play" }) {
      on press { if status.playing { player.pause() } else { player.play() } }
    }
    Button("Ding") { on press { audio.play("ding.wav") } }
  }
}
```

iOS と Mac では AVPlayer、Android では Media3 の ExoPlayer、ブラウザーでは `<audio>`、Windows では MCI、Linux では GStreamer の `playbin` を使います。画面がロックされても再生を続けるのは、アプリがメディアを再生中だとシステムに伝えたときだけです。下の「再生中の表示」を参照してください。

### 再生中の表示とメディアキー：platform/nowplaying

`nowplaying.NowPlaying` は、ビューの中にあるあいだ、アプリが何を再生しているかをシステムに伝えます。伝わる先は、iOS のロック画面とコントロールセンターのプレーヤー、Android のメディア通知（フォアグラウンドサービスなので再生が続きます）、Mac の「再生中」ウィジェット、Windows のメディアフライアウト（SMTC）、Linux の MPRIS（GNOME のメディアコントロール）、ブラウザーの Media Session API です。イベントはシステムからのコマンドで、メディアキー、ヘッドホンのボタン、ロック画面のボタンから届きます。`player` を渡すと、その再生位置に追従し、再生と一時停止も自分で行います。

```asuka
import platform/audio
import platform/nowplaying

component Playing(url: string) {
  state player = audio.Player(url)

  Column {
    nowplaying.NowPlaying {
      title: "Episode 12"
      artist: "The Show"
      artwork: "cover.jpg"
      player: player
      skipInterval: 15
      on next { player.seek(0) }
    }
    Button("Play") { on press { player.play() } }
  }
}
```

`player` がないときは、`state`（`.playing`、`.paused`）、`duration`、`position` を渡し、`play`、`pause`、`seek(position)`、`skipForward(seconds)`、`skipBackward(seconds)` を処理します。ビューの中にいくつもあるときは、最後に表示されたものがシステムに表示されます。このパッケージを import したアプリには、iOS の `audio` バックグラウンドモードと Android のフォアグラウンドサービスの権限が付きます。

## ネットワークの状態：platform/network

`platform/network` は、デバイスのネットワークの状態を伝えます。オンラインかどうか、Wi-Fi とモバイル通信のどちらでつながっているか、通信に料金がかかるかどうかです。大きなファイルは Wi-Fi のときだけダウンロードする、オフラインであることを表示する、といった用途に使います。実装は、iOS と Mac では NWPathMonitor、Android では ConnectivityManager、Windows では Network List Manager、Linux では GNetworkMonitor と NetworkManager、ブラウザーでは `navigator.onLine` です。ブラウザーで通信の種類がわかるのは、それを取得できる環境（Android の Chrome）だけです。

`network.status()` は、ネットワークの状態を追跡される値として返します。バインディングの中で読めば、変化に追従します。追従するのは何かが読んでいるあいだだけで、ネットワークを監視するのもそのあいだだけです。

```asuka
import platform/network

component NetworkBadge {
  let net = network.status()

  Text(if !net.connected { "Offline" } else if net.metered || net.constrained { "{net.type}: large downloads wait for Wi-Fi" } else { "{net.type}" })
}
```

`network.Status` は次のフィールドを持ちます。

- `connected`：接続しているかどうか。
- `type`：`.none`、`.wifi`、`.cellular`、`.ethernet`、`.other`（VPN だけの接続、Bluetooth など）のいずれか。テキストにすると、その名前（`"wifi"`）になります。
- `metered`：モバイルデータ通信やスマートフォンのインターネット共有のように、通信に料金がかかるか、量に制限があること。
- `constrained`：ユーザーがアプリにデータ使用量を減らすよう求めていること（iOS の「省データモード」、Android の「データセーバー」、ブラウザーのデータセーバー）。Windows では、通信量がデータ制限に近づいているか、超えていることも表します。Linux では常に false です。

プラットフォームから最初の応答があるまで、またプラットフォームがネットワークについて何も伝えない環境では、値は `connected` で、`type` は `.other` です。

`network.current()` は現在のネットワークの状態を、追跡されない値として返します。`network.changes()` はそのストリームで、まず現在の状態を、その後は変わるたびに新しい状態を流します。

## ライブラリへの保存：platform/library

`platform/library` は、ユーザーやほかのアプリから見える場所にファイルを保存します。動画はギャラリーに、曲は音楽ライブラリに、どんなファイルでも「ダウンロード」に保存できます。

```asuka
import platform/library

async fn keep(file: string) -> string {
  do {
    let _ = try await library.save(file, collection: .movies, folder: "My App", move: true)
    return "Saved"
  } catch e {
    return "Couldn't save: {e.message}"
  }
}
```

`library.save(path, collection:, name:, folder:, mime:, move:)` は、ファイルをコレクションにメインスレッドの外でコピーし（大きな動画だと時間がかかるためです）、保存先を返します。必要な権限は初回に求めます。

- `collection`：`.downloads`（既定）、`.movies`、`.music`、`.pictures` のいずれか。
- `name`：保存先でのファイル名（既定は元のファイル名）。同じ名前がすでにあれば番号が付きます。
- `folder`：コレクションの中に作る、アプリのファイル用のフォルダー。空ならコレクションの直下に保存します。
- `mime`：ファイルの種類（既定は拡張子から判断します。`library.mimeOf(name)`）。
- `move`：保存できたら元のファイルを削除します。アプリのキャッシュにダウンロードしたファイルを、コピーを二重に残さずに保存するときに使います。

各コレクションの保存先は次のとおりです。

| コレクション | Android | iOS | macOS | Windows、Linux |
| --- | --- | --- | --- | --- |
| `.downloads` | ダウンロード | アプリの Documents（「ファイル」App に表示） | `~/Downloads` | ユーザーのダウンロード |
| `.movies` | ギャラリーの動画 | 写真 | 写真 | ユーザーのビデオ |
| `.music` | ミュージック | アプリの Documents（「ファイル」App に表示） | `~/Music` | ユーザーのミュージック |
| `.pictures` | ギャラリーの画像 | 写真 | 写真 | ユーザーのピクチャ |

Windows と Linux、そして Mac の `~/Downloads` と `~/Music` では、ファイルはユーザーのフォルダーの中の `folder` に保存され、そのパスが返されます。Linux のフォルダーは XDG の規定に従います。ブラウザーでは、コレクションにかかわらずファイルをダウンロードします。保存先は、ブラウザーがダウンロードを置く場所です。

**Android** では、Android 10 以降は権限なしで MediaStore のコレクションを使い、`content://` URI を返します。Android 10 より前は、ストレージの権限を求めてから共有フォルダーに書き込みます。

**iOS** では、「写真」への追加の許可を求め（追加のみで、アプリはライブラリを読めません）、アセットの識別子を返します。「写真」では `folder` は使われません。それ以外のコレクションはアプリの Documents の中の `folder` に保存され、ファイルのパスが返されます。Asuka は、Documents が「ファイル」App に表示されるように設定します。

## バックグラウンド処理：platform/background

`platform/background` を使うと、アプリが画面に出ていないあいだも処理を続けられます。ユーザーが始めた、数分かかるダウンロード、アップロード、書き出しなどに使います。

`background.start(title:, text:, progress:, actions:, onAction:)` はタスクを開始します。タスクの `end()` を呼ぶまでアプリは動き続け、そのあいだタイトル、テキスト、進み具合、ボタンを通知に表示します。ユーザーが通知のボタンをタップすると、`onAction` がそのボタンの ID を受け取ります。`update(…)` は新しい内容を表示するもので、処理が進むにつれて進み具合を更新するのに使います。タスクは、ユーザーが処理を求めたときに、アプリが画面に出ているうちに開始してください。

```asuka
import platform/background

component Export {
  state status = ""
  state cancelled = false

  Column {
    Button("Export") {
      on press {
        cancelled = false
        let buttons = [background.Action(id: "cancel", title: "Cancel")]
        let job = background.start(title: "Exporting", progress: -1, actions: buttons, onAction: a => {
          if a == "cancel" { cancelled = true }
        })
        defer { job.end() }
        var i = 1
        while i <= 10 && !cancelled {
          await sleep(1s)   // 実際の処理
          let p = float(i) / 10
          job.update(title: "Exporting", text: "{(p * 100).fixed(0)}%", progress: p, actions: buttons)
          i += 1
        }
        status = if cancelled { "Cancelled" } else { "Exported" }
      }
    }
    Text(status)
  }
}
```

- `title` と `text`。
- `progress`：0 から 1。負の値は、処理中だが進み具合はわからないことを表します。
- `actions`：通知のボタン。それぞれ `background.Action`（`id` と `title`）です。

`update` のたびにこれらはすべて置き換わるので、`actions` も毎回指定してください。処理そのものはアプリの非同期コードで、タスクが生きているあいだ続きます。[`examples/downloader`](../../examples/downloader) は、タスクの中でダウンロードし、一時停止、再開、キャンセルができる例です。

**Android** は、バックグラウンドに回ったアプリを数分以内に停止します。タスクがあるあいだは、（データ同期タイプの）フォアグラウンドサービスでアプリを動かし続け、CPU もスリープさせません。タスクごとに常駐する通知が表示され、進み具合と最大三つのボタンが並びます。Android 13 以降では、ユーザーがまだ通知を許可していなければ、タスクの開始時に許可を求めます。許可してもしなくても処理は続きます。バックグラウンドにいるアプリはタスクを開始できません。Android 15 では、データ同期サービスは 1 日 6 時間までに制限されます。Google Play は、アプリのデータ同期フォアグラウンドサービスを何に使うのかを Play Console で尋ねるので、アップロードの前に申告してください。

**iOS** では、タスクがあるあいだ、バックグラウンドに回ったアプリに 30 秒ほどの猶予が与えられます。通知は表示されないので、`update` は何もせず、`onAction` も呼ばれません。時間切れになると iOS がタスクを終わらせ、処理が止まることがあります。

**デスクトップ**のアプリは、画面に出ていなくても動き続けます。タスクがあるあいだは、システムがアプリを遅くしたり停止したりしないようにします（Mac の App Nap、Windows と Linux のスリープ）。通知は表示されないので、`update` は何もせず、`onAction` も呼ばれません。**ブラウザー**は、タスクの有無にかかわらず、開いているあいだはページを動かします。

## ほかのアプリからの共有：onShare

コンポーネントの本体に書いた `onShare(items => …)` は、コンポーネントが生きているあいだ、ほかのアプリが共有シートからこのアプリに共有したものを受け取ります。YouTube やブラウザーからのリンク、メモからのテキスト、ギャラリーからの写真、「ファイル」からの PDF などです。app ブロックでも、イベント `on share(items) { … }` として受け取れます。これは iOS と Android の機能です。デスクトップとブラウザーでは、アプリがほかのアプリの共有メニューに表示されず、何も届きません。アプリが何の共有先として表示されるかは、`asuka.toml` の `shareTarget` で指定します。

```toml
shareTarget = true
```

こう書くと、テキストとリンクを受け取ります。テーブルで指定すると、MIME タイプごとにファイルも受け取れます。一度に受け取れるのは `max` 個までです（既定は 1）。

```toml
[shareTarget]
types = ["text", "image/*", "video/*"]
max = 10
```

タイプには `"text"`（テキストと Web のリンク）か、MIME タイプを指定します。MIME タイプは `"image/*"`、`"video/*"`、`"audio/*"`、`"application/pdf"` などで、`"*/*"` はあらゆるファイルを表します。

```asuka
component Shared {
  state link = ""
  state photos: [string] = []

  onShare(items => {
    for it in items {
      match it.kind {
        .link => { link = it.text }
        .image => { photos.push(it.path) }
        _ => {}
      }
    }
  })

  Column {
    Text(link)
    for p in photos key p { Image(p) }
  }
}
```

各 `SharedItem` には `kind` があり、`.text`、`.link`、`.image`、`.video`、`.audio`、`.file` のいずれかです。テキストやリンクは `text` に入ります。ファイルは `path` にあり、これはアプリのキャッシュにある、アプリ専用のコピーです。アプリで保管する場所に移すか、使い終わったら削除してください。ファイルには、共有元のアプリでの名前 `name`（`IMG_0042.HEIC` など）、`mime` タイプ、`size`（不明なら -1）も付いてきます。項目は、共有元のアプリが渡した順に届きます。ブラウザーから共有されたページは、タイトル（テキスト）とリンクとして届きます。

だれも受け取らないうちに届いたもの、つまりアプリを起動した共有は、最初の `onShare`（またはアプリの `on share`）に渡されます。コンポーネントの `onShare` は、コンポーネントがなくなると止まります。共有されたものに合わせてアプリの画面を開くには、[ナビゲーション](navigation.md)を参照してください。

**Android** では、タイプごとのインテントフィルターによって、共有されたものとともにアプリ自体が開きます（`ACTION_SEND`。`max` が 1 より大きければ `ACTION_SEND_MULTIPLE` も）。アプリは、ファイルへのアクセス権が有効なうちに、`onShare` に渡す前にファイルをキャッシュにコピーします。

**iOS** では、アプリに共有拡張機能（Share Extension）が入り、Android と同じように、共有されたものとともにアプリを開きます。拡張機能は独自の画面を表示しません。拡張機能のアクティベーションルールはタイプから作られます。テキスト、リンク、画像、動画、任意のファイルには Apple が用意したキーを使い、これらでは `max` が効きます。それ以外（オーディオや PDF）には添付ファイルのタイプに対する述語（predicate）を使い、こちらでは `max` は効きません。拡張機能は渡されたものを何でも受け取るので、`kind` と `mime` を確認してください。ファイルは App Group `group.<app ID>` を通じて拡張機能からアプリに渡ります。実機向けビルドのプロビジョニングプロファイルには、この App Group が含まれている必要があります（[配布](shipping.md#ios)を参照）。

`asuka dev` では、アプリの `shareTarget` にかかわらず、開発用アプリがあらゆる共有を受け取ります。

リンクの先が何か（ページ、画像、動画）を調べるのはアプリの役目で、`std/http` を使います。expo-sharing の「resolved」ペイロードに当たる処理を、Asuka で書くことになります。

### アプリ独自の共有シート：platform/sharesheet

通常、共有するとアプリが開きます。代わりに、アプリ独自の共有シートを用意することもできます。「Raindrop に保存」や「メモに追加」のように、共有元のアプリの上に表示される画面で、ユーザーは保存したらすぐ元の場所に戻れます。`platform/sharesheet` を使うと、アプリが共有シートとして動いているときに、app ブロックでそのシートを表示できます。

```asuka
import platform/sharesheet

component SaveLink {
  state link = ""
  state note = ""

  onShare(items => {
    for it in items {
      if it.kind == .link {
        link = it.text
      }
    }
  })

  Column {
    padding: 16; gap: 12; grow: 1
    if link.isEmpty() {
      ProgressRing()
    } else {
      Text(link)
    }
    TextField { bind text: note; placeholder: "Note" }
    Row {
      gap: 12
      Button("Cancel") {
        on press { sharesheet.cancel() }
      }
      Button("Save") {
        variant: .filled
        on press {
          save(link, note)
          sharesheet.done()
        }
      }
    }
  }
}

component Root {
  Text("Links")
}

// アプリ独自のもの。
fn save(link: string, note: string) {}

app {
  if sharesheet.active() {
    SaveLink()
  } else {
    Root()
  }
}
```

アプリが共有シートとして動いているとき、`sharesheet.active()` は true になります。このときルートはシートで、アプリのほかの部分は作られません。シートは、アプリと同じように `onShare` で共有されたものを受け取ります。項目がまだ届いていない間（動画をコピーしている途中など）は、スピナーを表示してください。シートは `sharesheet.done()`、`sharesheet.cancel()`、`sharesheet.openApp()` で閉じます。`openApp()` は、シートがないときと同じように、共有された項目を持ってアプリを開き、アプリの `onShare` に渡します。シートがどんな共有で候補に出るかは、上で説明した `asuka.toml` の `shareTarget` で決まります。`active()` を使っていても `shareTarget` がなければ、どの共有メニューにも出てきません（CLI が知らせます）。

シートは独立したルートで、iOS が共有拡張機能を動かすのと同じく、独立したプロセスで動きます。**iOS** ではアプリの共有拡張機能で、iOS がシートとして表示し、アプリと共有するフレームワークにあるアプリのコードを動かします。**Android** では `:share` プロセスの Activity で、共有元のアプリの上に Material のボトムシートとして表示されます（下にドラッグする、外側をタップする、戻るのいずれでもキャンセルになります）。シートが閉じるとプロセスも終わります。ファイルの `path` はシートが閉じるまでしか使えないので、アプリで残すものはコピーしてください。

アプリが保存しているものは、シートからも同じように使えます。アプリのファイル（`std/fs` のアプリ用ディレクトリと、そこにある SQLite のデータベース）、`std/storage` の値、`platform/secure` です。Android では二つのプロセスがアプリのファイルを共有します。iOS では、アプリのファイルと値は、シート付きで初めて起動したときに一度だけ App Group（`group.<app ID>`）に移され、キーチェーンのアクセスグループも拡張機能と共有されます。実機向けビルドのプロビジョニングプロファイルには App Groups の機能が必要です（[配布](shipping.md#ios)を参照）。アプリの実行中にシートが書き込んだ内容は、アプリには通知されません。アプリが前面に戻ったときに読み直してください。

アプリが `sharesheet.active` を使っていると、Asuka はアプリを開くだけの拡張機能の代わりにシートをビルドします。このとき iOS では、アプリのコードがフレームワークになり、Xcode が拡張機能とコードを共有するのと同じように、アプリと拡張機能の両方がそれを読み込みます。シートがあってもアプリのサイズは増えません。デスクトップとブラウザーでは、アプリはほかのアプリの共有メニューに出てこないので、`active()` は false です。

テストでは、`sharesheet.fake()` でアプリを共有シートとして動かせます（`mountApp()` がシートを表示します）。シートがどう閉じたかは `closed` に記録されます。マウントしたアプリの `device.share(items)` は、ほかのアプリがそれらを共有したことを表します。

```asuka
import platform/sharesheet

component SaveLink {
  state link = ""

  onShare(items => {
    link = items[0].text
  })

  Column {
    Text(link)
    Button("Save") {
      on press { sharesheet.done() }
    }
  }
}

app {
  if sharesheet.active() {
    SaveLink()
  } else {
    Text("Links")
  }
}

test "the sheet saves" {
  let sheet = sharesheet.fake()
  let app = mountApp()
  app.share([SharedItem(kind: .link, text: "https://asuka.dev")])
  app.expect("https://asuka.dev")
  app.tap("Save")
  expect(sheet.closed == .done)
}
```

## JavaScript：platform/js

`platform/js` は、プラットフォーム自身のエンジンを使い、画面に出さずに JavaScript を実行します。使うのは一度も表示されない Web ビュー（iOS と Mac では WKWebView、Android では WebView、Windows では WebView2、Linux では WebKitGTK）で、ブラウザーでは非表示でサンドボックス化されたフレームです。JavaScript でしか書かれていないコード、たとえば動画の URL に署名する関数を持つ、サイトのプレイヤーのようなコードを動かすためのものです。

`js.eval(script)` は、スクリプトを専用の新しいページで、`<script>` の中身と同じく文の並びとして実行し、その値を JSON で返します。値は最後の文の値か、スクリプトが返す Promise の解決値です（`undefined` は `null` になります）。例外が投げられたり Promise が reject されたりすると、スクリプトのメッセージを持つエラーになります。ページは実行後に破棄されるので、あるスクリプトが設定したものは次のスクリプトからは見えません。結果を待つあいだも、メインスレッドは止まりません。

```asuka
import platform/js
import std/json

async fn doubled() throws -> [int] {
  let v = try await js.eval("const xs = [1, 2, 3].map(x => x * 2); xs")   // "[2,4,6]"
  return try json.decode<[int]>(v)
}

async fn later() throws -> string {
  let v = try await js.eval("new Promise(done => setTimeout(() => done('later'), 100))", timeout: 10s)
  return try json.decode<string>(v)
}
```

スクリプトは `timeout`（既定は 1 分）のあいだ実行できます。時間切れになると `js.JsError.timeout` を投げます。タスクがキャンセルされると（タスクを開始した画面がなくなるなど）、そのページは破棄されます。

ユーザーに見えるページの中でスクリプトを実行するには、`WebView` を使います（[Web ページ](components.md#web-ページ)）。

## プッシュ通知：platform/push

`push.register()` は、（初回は）ユーザーに通知の許可を求め、デバイスを Apple（APNs）か Google（Firebase Cloud Messaging）のサービスに登録して、`Registration` を返します。`Registration` は、`token`、`service`（`"apns"`、`"fcm"`）、通知が許可されているかどうか（`allowed`）を持ちます。トークンを自分のサーバーに送り、サーバーからそのトークン宛てに通知を送ります。トークンは変わることがあるので、起動のたびに登録してください。Mac も iOS と同じように APNs に登録します（プロビジョニングプロファイルが必要です。[Mac の場合](#mac-の場合)を参照）。Web では Web Push を使います（`"webpush"` で、トークンはブラウザーのサブスクリプションです。[Web](web.md) を参照）。Windows と Linux にはアプリ向けのプッシュサービスがないため、`register` はエラーを投げます。

```asuka
import platform/push

component Root {
  task {
    if let r = try? await push.register() {
      try? await saveDeviceToken(r.service, r.token)
    }
  }
  task {
    for await m in push.opened() { openThread(m.data["thread"] ?? "") }
  }

  Text("Inbox")
}

// アプリ独自のもの。
async fn saveDeviceToken(service: string, token: string) throws {}
fn openThread(id: string) {}

app { Root() }
```

- `push.received()` は、アプリの実行中に届いたメッセージのストリームです。タイトルか本文があるメッセージは、フォアグラウンドでも通知として表示されます。データメッセージ（サイレントプッシュ）はここにだけ届きます。
- `push.opened()` は、ユーザーがタップした通知のストリームです。アプリを起動した通知も含まれます。その通知は最初の読み手を待つので、アプリの起動時に読んでください。メッセージのデータの `"url"` は、`platform/notify` の通知と同じように、アプリの `openURL` にも渡されます。
- `push.Message` には `id`、`title`、`body`、`data` があります（`data` は、iOS ではペイロードの `aps` 以外のキー、Android ではメッセージのデータです）。

iOS のリリースビルドでは、App ID に Push Notifications ケイパビリティが必要です。シミュレーターでもトークンを取得できます。Android では Firebase プロジェクトが必要です。プロジェクトに Android アプリを追加し、その `google-services.json` をダウンロードして、`asuka.toml` で指定します。

```toml
[android]
googleServices = "google-services.json"
```

Asuka は、Google の Gradle プラグインと同じようにこのファイルの設定をアプリに書き込みます。このファイルがないと、`register` は対処方法を示すエラーを投げます。

### プッシュ通知のカテゴリー、スレッド、チャンネル

プッシュ通知でも、`platform/notify` のカテゴリー（そのアクションと `notify.responses()`）、スレッド、チャンネル、バッジ、`present` を使えます。ペイロードで次のように指定します。

- **iOS**：APNs の仕様どおり、`aps` の `category`、`thread-id`、`badge`。
- **Android**：通知メッセージでは `android.notification.channel_id` と `image`、データメッセージでは `"title"`、`"body"`、`"subtitle"`、`"category"`、`"thread"`、`"summary"`、`"channel"`、`"image"`、`"badge"`、`"url"`。`"title"` か `"body"` を持つデータメッセージは通知として表示されます（Asuka がアクション付きで通知を組み立てます）。どちらも持たないデータメッセージはサイレントです（`received()` にだけ届きます）。アプリがバックグラウンドにあるときに届いた通知メッセージは Firebase が表示するので、アクションもスレッドも付きません。それらが必要なら、データメッセージを送ってください。

`push.Message` には、これらに対応する `subtitle`、`category`、`thread`、`channel`、`image`、`badge`、`summary` もあります。

### 表示前のプッシュ通知の書き換え

`push.rewrite(f)` を使うと、各プッシュ通知を表示前にアプリで書き換えられます。エンドツーエンドで暗号化されたペイロードを復号する、タイトルと本文を設定する、画像を添付する、送信者の写真付きの会話のメッセージにする、スレッドとカテゴリーを設定する、といったことができます。アプリの起動時に呼び出してください。

```asuka
import platform/push
import platform/notify

async fn reveal(m: push.Message) throws -> push.Message? {
  let box = m.data["box"] ?? ""
  if box.isEmpty() {
    return none   // アプリのものではない：届いたまま表示
  }
  let msg = try await decrypt(box)   // 鍵は platform/secure に保存
  var out = m
  out.body = msg.text
  out.category = "message"
  return out.from(notify.Person(id: msg.fromID, name: msg.fromName, image: msg.avatar),
    notify.Conversation(id: m.data["chat"] ?? ""))
}

component Root {
  task {
    push.rewrite(reveal)
  }

  Text("Chats")
}

// アプリ独自のもの。
struct Plain {
  text: string
  fromID: string
  fromName: string
  avatar: string
}

async fn decrypt(box: string) throws -> Plain {
  return Plain(text: box, fromID: "ana", fromName: "Ana", avatar: "")
}

app { Root() }
```

`f` は表示するプッシュ通知を返します。`none` を返したり、エラーを投げたりすると、プッシュ通知は届いたまま表示されます。書き換えは、届いた順に一つずつ行われます。`image` はダウンロードして添付されます。`from` は、`notify.Notification` の `from` と同じように、通知を会話のメッセージにします。`push.stopRewriting()` を呼ぶと、また届いたまま表示されるようになります。

**iOS** では、Notification Service Extension で書き換えます。`asuka build` は、`push.rewrite` を使うアプリにこの拡張機能を作ります。iOS は、`aps` に `"mutable-content": 1`（と、書き換えに失敗したときに表示される `alert`）を持つプッシュ通知ごとに拡張機能を起動します。拡張機能は、バックグラウンドでの起動と同じように画面なしでアプリを動かし、アプリが `push.rewrite` を呼んだ時点で書き換えが走ります。そのため、ルートが起動時に行う処理は軽くしておくか、拡張機能でのルートを `rewrite` を呼ぶだけのものにしてください（拡張機能の中では `push.inExtension()` が true です：`if push.inExtension() { Rewrites() } else { Root() }`）。iOS が与える時間はおよそ 30 秒、メモリは 24 MB で、時間切れになるとプッシュ通知は届いたまま表示されます。アプリが保存しているものは拡張機能でも同じです。ファイルと `std/storage` の値は両者が共有する App Group に、キーチェーンの項目（`platform/secure`）はアプリのアクセスグループにあります。リリースビルドには、拡張機能の Bundle ID（アプリの ID に `.notification` を付けたもの）用のプロビジョニングプロファイルが必要で、App Groups の機能とアプリの App Group を含んでいなければなりません（`asuka.toml` の `ios.notificationProfile` で指定できます。省略時はインストール済みのものから最も合うものを選びます）。iOS では、書き換えでプッシュ通知を非表示にすることはできません。シミュレーターがこの拡張機能を動かすのは APNs から届いたプッシュ通知だけで、`xcrun simctl push` で送ったものには動きません。

**Android** では、FCM のデータメッセージをアプリのプロセスで書き換えます。アプリが動いていなければ画面なしで起動し、10 秒ほどのうちに書き換えてから通知を表示します。書き換えたあとにタイトルも本文もないデータメッセージは表示されません。アプリがバックグラウンドにあるときに届いた通知メッセージは、書き換えずに Firebase が表示します。書き換えたい場合はデータメッセージを送ってください。

**Web** では、前面にあるページが、Service Worker が表示する前にプッシュ通知を書き換えます。ページが開いていなければ、届いたまま表示されます。

テストでは、`push.fakeRewrite(m)` が、届いたプッシュ通知が最終的にどう表示されるか（書き換えたプッシュ通知、または `m` そのもの）を返します。

## Apple でサインイン：platform/appleauth

`appleauth.SignInButton` は Apple 純正のボタンで、タイトル、ロゴ、色がユーザーの言語に合わせて表示されます。`appleauth.signIn` はシステムのシートを表示します。App Store は、アカウントのサインインにほかの会社のサインインを提供するアプリに、Apple でサインインも提供するよう求めています。

```asuka
import platform/appleauth

component SignIn {
  state status = ""

  Column {
    appleauth.SignInButton {
      type: .continueWithApple
      variant: .automatic   // ライトモードでは黒、ダークモードでは白
      height: 44
      on press {
        do {
          let c = try await appleauth.signIn(scopes: [.fullName, .email], nonce: "n-0S6_WzA2Mj")
          try await signInWithApple(c.identityToken, c.authorizationCode)
        } catch appleauth.AppleAuthError.canceled {
        } catch e {
          status = e.message
        }
      }
    }
    Text(status)
  }
}

// アプリ独自の処理：サーバーがトークンを Apple に照会して検証します。
async fn signInWithApple(token: string, code: string) throws {}
```

`Credential` の `user` は、アカウントの変わらない ID です。`identityToken`（Apple が署名した JWT）と `authorizationCode` を自分のサーバーに送り、サーバーで Apple に照会して検証します。`email`、`givenName`、`familyName` が届くのは、ユーザーが初めてそのアプリにサインインしたときだけなので、そのときに保存してください。`AppleAuthError.canceled` は、ユーザーがシートを閉じたことを表します。`appleauth.credentialState(user)` は、ユーザーがそのアプリでまだ Apple でサインインを使っているかどうかを返します（`.revoked` ならサインアウトさせてください）。Android にはこの機能がありません。`SignInButton` は何も表示せず、`available()` は false で、`signIn` は `AppleAuthError.notAvailable` を投げます。Windows、Linux、Web でも同じです。これらのプラットフォームでは、Web 経由で Apple でサインインを使ってください（`platform/webauth`）。Mac では、プロビジョニングプロファイルがあれば iOS と同じように使えます（[Mac の場合](#mac-の場合)を参照）。テストでは、`appleauth.fake()` がユーザーの代わりに応答します。応答の内容は、その `credential`、`cancel`、`states` で決めます。

## Google でサインイン：platform/googleauth

`googleauth.signIn(clients, scopes:, nonce:)` は、Google 自身の SDK と同じように、ユーザーの Google アカウントでサインインし、`GoogleUser` を返します。`id`、`email`、`name`、`givenName`、`familyName`、`picture`、アプリのサーバーに渡す `idToken`、そしてプロフィール以外に求めた `scopes` のための `accessToken` を持ちます。`none` はユーザーがキャンセルしたことを表します。

```asuka check
import platform/googleauth

let clients = googleauth.Clients(
  ios: "1234-ios.apps.googleusercontent.com",
  web: "1234-web.apps.googleusercontent.com",
  webRedirect: "https://example.com/",
  desktop: "1234-desktop.apps.googleusercontent.com",
  desktopSecret: "GOCSPX-example"
)

component SignIn {
  state who = ""

  Column {
    gap: 12
    Button("Sign in with Google") {
      on press {
        do {
          if let user = try await googleauth.signIn(clients) {
            who = user.email
            // Send user.idToken to the app's server, which verifies it.
          }
        } catch e {
          who = e.message
        }
      }
    }
    Text(who)
  }
}
```

プラットフォームごとにサインインの方法が異なり、それぞれ Google Cloud コンソールで作った種類の OAuth クライアントを使います（`Clients` で指定します）。

| | 方法 | クライアント |
| --- | --- | --- |
| Android | Credential Manager のアカウント選択画面（システムのもの）。scopes は Google の Authorization API で求めます | ID トークンには Web クライアントの ID（`web`）。アプリのパッケージ名と署名証明書を登録した Android クライアントも必要です |
| iOS、macOS | システムのサインインシート（`platform/webauth`）で Google のサインインページを開き、PKCE を使います | iOS クライアント（`ios`）。その ID を逆にしたものがコールバックのスキームです |
| Windows、Linux | `platform/webauth` のウィンドウでサインインページを開き、127.0.0.1 上のアプリ自身のサーバーに戻ります（RFC 8252） | デスクトップクライアントとそのシークレット（`desktop`、`desktopSecret`。インストールされるアプリのもので、秘密ではありません） |
| Web | ポップアップで、アプリのページに戻ります | Web クライアント（`web`）。`webRedirect` はその承認済みリダイレクト URI の一つです |
| テレビ（Apple TV、Android TV） | `signInWithCode`：アプリがアドレスとコードを表示し、ユーザーがスマートフォンやコンピュータで入力します（Google のテレビや入力が限られたデバイス向けのサインイン） | 「テレビと入力が限られたデバイス」タイプのクライアントとそのシークレット（`tv`、`tvSecret`） |

ID トークンは Google が署名した JWT です。アプリのサーバーは、示された人物を信用する前にそれを検証します（Google の鍵による署名、`aud` がアプリのクライアントであること、`iss`、`exp`、そしてサーバーがアプリに渡した `nonce`）。`Clients` にそのプラットフォームのクライアントがなければ `GoogleAuthError.notConfigured` を投げます。`googleauth.signOut()` は、Android の選択画面が次回もう一度尋ねるようにします。ほかのプラットフォームでは、セッションはブラウザーが保持します。

テレビには表示できるサインインのシートがないので、そこでは `signIn` はエラーを投げます。代わりに `googleauth.signInWithCode(clients, show:, scopes:)` でサインインします。`show` には、アプリが表示する `DeviceCode`（`url`、`code`、コードの有効期間 `expires`）が渡され、ユーザーが別のデバイスでコードを入力するまで待ちます。ユーザーがそこで許可するとアカウントを返し、拒否すると `none` を返します。入力の前にコードの期限が切れたら `GoogleAuthError.expired` を投げるので、もう一度呼んで新しいコードをもらってください。`tv` のクライアントがあればどのプラットフォームでも使えます。Google がこのサインインに許すスコープは、プロフィールのものとごく一部のもの（アプリが作った Drive のファイル、YouTube）です。

```asuka check
import platform/googleauth

let clients = googleauth.Clients(tv: "1234-tv.apps.googleusercontent.com", tvSecret: "GOCSPX-example")

component TVSignIn {
  state prompt = ""

  Column {
    gap: 24
    Button("Google でサインイン") {
      on press {
        do {
          let user = try await googleauth.signInWithCode(clients, show: c => {
            prompt = "スマートフォンで {c.url} を開き、{c.code} を入力してください"
          })
          if let u = user {
            prompt = u.email
            // u.idToken をアプリのサーバーに送り、サーバーで検証します。
          }
        } catch e {
          prompt = e.message
        }
      }
    }
    Text(prompt) { font: .title2 }
  }
}
```

## ウォレット決済：platform/pay

`platform/pay` は、プラットフォームのウォレットで支払いを受け付けます。iOS、macOS、Safari では Apple Pay、Android とそのほかのブラウザーでは Google Pay です。対象は物理的な商品やサービス（注文、配車、チケット、寄付など）で、アプリ内で販売するデジタルコンテンツには各ストアのアプリ内課金を使います。ユーザーはウォレットのシートで支払い、アプリは決済代行業者（Stripe、Adyen、Braintree など）に渡すウォレットの支払いトークンを受け取ります。請求するのはアプリのサーバーです。Asuka 自身がカードに請求することはありません。

```asuka
import platform/pay

fn request() -> pay.Request = pay.Request(
  country: "US",
  currency: "USD",
  items: [pay.Item(label: "Tea set", price: .parse("40.00")!), pay.Item(label: "Tax", price: .parse("3.20")!, kind: .tax)],
  total: pay.Item(label: "Asuka Tea", price: .parse("43.20")!),
  shipping: [.name, .address, .email],
  apple: pay.Apple(merchantId: "merchant.com.example.tea"),
  google: pay.Google(gateway: "stripe", gatewayParams: ["stripe:version": "2018-10-31", "stripe:publishableKey": "pk_test_123"]),
  onShippingContact: c => {
    // その住所での品目と合計
    if c.address.countryCode != "US" {
      throw pay.Invalid(field: .unserviceable, message: "We ship in the United States only.")
    }
    return pay.Update(total: pay.Item(price: .parse(if c.address.state == "CA" { "43.20" } else { "40.00" })!))
  },
  onAuthorize: p => {
    try await charge(p.token)   // 終われば支払い済み。投げたエラーはシートに表示されます
  },
)

component Checkout {
  state placed = ""
  let wallet = load pay.available(request())

  Column {
    if wallet.value?.canPay ?? false {
      pay.PayButton {
        type: .buy
        variant: .automatic   // ライトモードでは黒、ダークモードでは白
        height: 48
        on press {
          do {
            let p = try await pay.pay(request())
            placed = "Thanks, {p.shipping.name}!"
          } catch pay.PayError.canceled {
          } catch e {
            placed = e.message
          }
        }
      }
    }
    Text(placed)
  }
}

// アプリ独自の処理：サーバーがトークンで請求します。
async fn charge(token: string) throws {}
```

`pay.available(request)` は、リクエストで受け付けるカードについて、ウォレットに何ができるかを返します。`wallet`、`canPay`（ボタンを表示してよいか）、`hasCard`（ウォレットが受け付けるカードをユーザーが持っているか）です。`PayButton` は `canPay` が真のときだけ表示してください。これはウォレット純正のボタン（`PKPaymentButton`、Google の `PayButton`、Web では Safari と Google のボタン）で、各ブランドのガイドラインは、アプリが独自のボタンを描かないよう求めています。文言（`type`：`.buy`、`.checkout`、`.book`、`.donate`、`.order`、`.subscribe`、`.plain`、そのほかの Apple の種類）とロゴはユーザーの言語で表示されます。`variant` は、明るい背景では `.black`、暗い背景では `.white`、白い背景では `.whiteOutline`、または `.automatic` です。高さはウォレットが決めます（iOS では 44 ポイント、Mac では 30、Google は 48）。幅はアプリが指定してください。Apple Pay にまだカードがない場合、Apple のガイドラインでは `.setUp` ボタンを表示します。このボタンは `pay.setUp()` を呼んでカードを追加します（購入ボタンでもかまいません。そのシートでもカードを追加できます）。Google Pay では、シートそのものがカードを追加します。

`pay.pay(request)` はシートを表示し、シートが閉じると `Payment` を返します。ユーザーがキャンセルしたときは `PayError.canceled` を投げます（ほかのシートが表示されているあいだは `.busy`）。金額は [`std/decimal`](stdlib.md) の `Decimal` で、通貨の主単位で正確に表します（`price: .parse("12.75")!`。Google Pay では小数点以下 2 桁まで。割引は負の値にします）。どのプラットフォームにも同じ数が渡ります（Apple の `NSDecimalNumber`、Google のテキスト）。このパッケージの最初のリリースでは金額を文字列（`amount: "12.75"`）で受け取っていました。次のリリースまでは、`price` がゼロのときにその文字列も読み取ります。`total` のラベルは、カードの利用明細に表示されるとおりの事業者名です（Apple Pay のシートでは「Pay Asuka Tea」）。`items` はその上に並ぶ明細行で、その `kind` は Google Pay のシートで使われます。リクエストではほかに、受け付けるカード（`networks`、`cards`、`cardCountries`）、配送先と請求先の連絡先として求める項目（`.name`、`.email`、`.phone`、`.address`）、`shippingType` と `shippingMethods`（最初のものが選ばれた状態になります）、Google Pay の配送先の国（`shippingCountries`）、Apple Pay のクーポンコード（`coupons`）を指定します。`recurring` を指定すると、定期的な請求と無料トライアルを持つサブスクリプションの支払いになります。`deferred` はあとで請求する支払い（ホテルなど）、`automaticReload` は残高のチャージです。ウォレットはこれから発生する請求を表示し、そのトークンはそれらの請求のためのマーチャントトークンになります。

ハンドラーは非同期で、シートが待っているあいだ（最大で約 30 秒）に実行されるので、アプリのサーバーを呼び出してもかまいません。ハンドラーには、ユーザーが住所を選んだときの `onShippingContact`（支払いの前にウォレットが渡すのは住所のうち市区町村、州、郵便番号、国だけですが、税と配送方法を決めるには十分です）、`onShippingMethod`、`onPaymentMethod`（カードの手数料）、`onCoupon` があります。それぞれ `Update`（新しい品目、合計、配送方法。指定しなかったものはそのまま）を返すか、`pay.Invalid(field:, message:)` を投げます。投げたエラーは、シートがそのフィールドに合わせて表示します。フィールドは `.shippingPostalCode`、アプリが配送しない住所を表す `.unserviceable`、`.couponInvalid` などで、あわせて、そのあいだ表示する更新も渡します。複数のエラーは `Update` の `errors` で渡します。メッセージはユーザー向けに書いてください。`onAuthorize` は、ユーザーが（Face ID、Touch ID、PIN で）支払ったときに `Payment` を受け取ります。その `token` をサーバーに送り、サーバーで請求してください。ハンドラーが投げたエラーはシートに表示され、ユーザーはもう一度試せます。正常に終われば、シートはチェックマークを表示して閉じ、`pay` はカード、連絡先、配送方法を含む支払いを返します。

トークンは、決済代行業者の SDK が送るものです。Apple Pay のトークンは `PKPaymentToken` の `paymentData` で、JSON オブジェクトです（Stripe は `pk_token` として、Braintree と Adyen は Apple Pay のトークンとして受け取ります）。これはマーチャント ID の支払い処理証明書に対して署名されていて、その証明書は決済代行業者が持っています。Google Pay のトークンは、リクエストの `google.gateway` に指定したゲートウェイが、そのパラメーター（Stripe の公開可能キー、Adyen や Braintree のマーチャントアカウント）を使ってトークン化します。または、`directKey` でアプリのサーバー向けに暗号化されます。

Apple Pay を使うには、決済代行業者の支払い処理証明書を登録したマーチャント ID（developer.apple.com の Identifiers、Merchant IDs）が必要です。これを `asuka.toml` に書きます。

```toml
[applePay]
merchantIds = ["merchant.com.example.tea"]
```

このパッケージを import したアプリには、これらの ID を含む in-app-payments エンタイトルメントが付きます。リリースビルドのプロファイルのため、App ID にも同じ ID で Apple Pay ケイパビリティが必要です（Mac アプリにも必要です。[Mac の場合](#mac-の場合)を参照）。シミュレーターのシートはテスト用のカードで支払い、どの決済代行業者も請求できないトークンを返します。Web で Apple Pay を使うには、サイトのドメインをマーチャント ID に登録し、`onValidateMerchant` を用意する必要があります。`onValidateMerchant` では、アプリのサーバーがマーチャント ID 証明書（merchant identity certificate）を使って Apple にマーチャントセッションを要求します。Google Pay の `TEST` 環境はアカウントなしで使え、テスト用のトークンを返します。`pay.Google(production: true)` には、Google Pay & Wallet Console のマーチャント ID（`merchantId`）と、そこでのアプリの承認が必要です。このパッケージを import した Android アプリには、Google Pay のライブラリ（`play-services-wallet`）、マニフェストでの API の有効化、シートがコールバックするサービスが追加されます。import しないアプリには何も追加されません。

iOS と Mac では PassKit の `PKPaymentAuthorizationController` と `PKPaymentButton` を使います。Android では Google Pay API を使います。`isReadyToPay`、Activity Result API 経由の `loadPaymentData`、そしてコールバックサービスを通じた動的な価格の更新と承認です。ブラウザーでは、Apple Pay があるブラウザー（Safari と、iPhone や iPad のブラウザー）なら Payment Request API の Apple Pay メソッドで Apple Pay を使います。それ以外のブラウザーでは Google 自身の Web API（pay.google.com から読み込む `pay.js`）で Google Pay を使い、どのブラウザーでも Android と同じリクエストとコールバックが使えます。`pay` はボタンの press から呼んでください。ブラウザーは、ユーザーの操作に応じたときしかシートを表示しないためです。Windows と Linux にはアプリ向けのウォレットがありません。`available` は `.none` を返し、`pay` は `PayError.notAvailable` を投げ、`PayButton` は何も表示しません。

テストでは `pay.fake()`（`FakeWallet`）がユーザーの役を務めます。`pay` がシートを表示しているあいだ（`sheet` が none でないあいだ）、住所、支払い方法、クーポンを変えたり、支払ったり（`authorize`）、シートを閉じたり（`cancel`）でき、アプリのハンドラーがそれに応答します。

```asuka
import platform/pay

test "the tax, then paid" {
  let w = pay.fake()
  let app = mount(Checkout())
  await app.idle()
  app.tap("PayButton")
  await app.idle()
  w.changeShippingContact(pay.Contact(address: pay.PostalAddress(state: "CA", countryCode: "US")))
  await app.idle()
  expect(w.sheet!.total.price.string() == "43.2")
  w.authorize(pay.Payment(token: "tok_visa", shipping: pay.Contact(name: "Ana")))
  await app.idle()
  app.expect("Thanks, Ana!")
}

component Checkout {
  state placed = ""

  pay.PayButton {
    on press {
      let p = try await pay.pay(pay.Request(
        total: pay.Item(label: "Asuka Tea", price: .parse("40.00")!),
        shipping: [.address],
        onShippingContact: c => pay.Update(total: pay.Item(price: .parse(if c.address.state == "CA" { "43.20" } else { "40.00" })!)),
      ))
      placed = "Thanks, {p.shipping.name}!"
    }
  }
  Text(placed)
}
```

テストでは、ネイティブコンポーネントは名前で表示され、それをタップすると `press` になります。[examples/checkout](../../examples/checkout) は、この方法でティーセットを販売します。

## Face ID と指紋認証：platform/biometric

`biometric.authenticate(reason:)` はシステムのプロンプト（Face ID、Touch ID、Android の BiometricPrompt、Mac の Touch ID、Windows の Windows Hello、ブラウザーではパスキーの WebAuthn プロンプト）を表示し、デバイスの持ち主であれば戻ります。そうでなければ `BiometricError.canceled`、`.notAvailable`（生体認証もパスコードもないか、ロックアウトされている）、`.failed` のいずれかを投げます。待っているあいだもメインスレッドを止めません。

```asuka
import platform/biometric

component Lock {
  state unlocked = false

  Button(if unlocked { "Unlocked" } else { "Unlock" }) {
    on press {
      do {
        try await biometric.authenticate(reason: "Unlock your notes")
        unlocked = true
      } catch biometric.BiometricError.canceled {
      } catch e {
        unlocked = false
      }
    }
  }
}
```

- `reason` は認証する理由です（iOS ではサブタイトル、Android ではタイトルになります）。`cancel:` はキャンセルボタンのタイトルです。
- `noPasscode: true` を指定しない限り、ユーザーは代わりにデバイスのパスコード、PIN、パターンを使えます。指定すると生体認証だけになります（Android では、強力な生体認証である Class 3 に限ります）。
- `biometric.check()` は、デバイスが備えているものを返します。`kind`（`.face`、`.fingerprint`、`.iris`、`.optic`）、登録済みかどうか（`enrolled`）、`passcode` があるかどうかです。

Linux には持ち主を確認するシステムのプロンプトがありません。`check` は何もないと返し、`authenticate` は `.notAvailable` を投げます。

## 録音：platform/record

`record.audio()` は `record.stopAudio()` を呼ぶまで（または `maxDuration` まで）マイクの音を録音し、ファイルを返します。アプリのキャッシュ（または `path` の場所）にある AAC の MPEG-4 ファイル（.m4a）で、`platform/audio` で再生できます。最初の録音でマイクの許可を求めます。`pauseAudio` と `resumeAudio` で一時停止と再開ができ、`audioStatus()` で今の状態（メーター用の 0 から 1 の `level`、`elapsed`）がわかります。`quality: .music` は声に必要な以上の音質で残します。

```asuka
import platform/record

component Memo {
  state recording = false
  state file = ""

  Button(if recording { "Stop" } else { "Record" }) {
    on press {
      if recording {
        record.stopAudio()
      } else {
        recording = true
        defer { recording = false }
        file = try await record.audio(maxDuration: 60s)
      }
    }
  }
}
```

`background: true` にすると、アプリが見えていないときも録音を続けます。iOS ではアプリの asuka.toml の `ios.backgroundModes` に `"audio"` が必要です。Android ではフォアグラウンドサービスになり、その通知に `title` と停止ボタンが表示されます。iOS と Mac では AVAudioRecorder、Android とブラウザーでは `MediaRecorder`、Windows では Media Foundation、Linux では GStreamer を使います。

## 音声の文字起こし：platform/transcribe

`transcribe.file(path)` は録音をデバイス上でテキストにします。音声はデバイスの外に出ません。`Result` には `text`、`segments`（単語やフレーズごとの開始時刻と長さ、秒。プラットフォームが返すときだけ）、聞き取った `language` が入ります。`transcribe.check(language)` で、録音する前にデバイスが文字起こしできるかを確かめられます。

```asuka
import platform/transcribe

async fn caption(recording: string) throws -> string {
  let can = try await transcribe.check("en-US")
  if !can.onDevice { return "" }
  let r = try await transcribe.file(recording, language: "en-US")
  return r.text
}
```

iOS と Mac では Speech フレームワークにデバイス上で処理するよう求め、Android ではデバイス上の認識エンジン（録音は Android 13 以降、`listen` は Android 12 以降）を使います。ブラウザーでは、デバイス上で動くブラウザー自身の認識エンジン（Chrome 139 以降）に録音を再生して聞かせるので、ファイル一つにほぼその長さの時間がかかり、`segments` はありません。Windows ではデスクトップ音声認識（SAPI の音声入力。英語は Windows に付属し、ほかの言語はその言語パックの音声認識と一緒に入ります）を使います。Linux ではディストリビューションがパッケージにしている認識エンジン PocketSphinx（Debian と Ubuntu では `pocketsphinx-en-us`）を英語で使います。デバイスでできないとき（古い Android、その認識エンジンのないブラウザー、認識エンジンのない Windows の言語、PocketSphinx がないかほかの言語の Linux）は、音声をサーバーに送る代わりに `TranscribeError.notAvailable` を投げます。言語のモデルをダウンロード中のときは `.downloading` です（ブラウザーはそのときインストールを始めます）。

`transcribe.listen(language)` は音声入力です。マイクを聞き、話すそばから結果を返します。毎回の `Heard` の `text` には、それまでのテキストがすべて入っています（最後のいくつかの単語は、まだ変わることがあります）。最後の一つは `final` です。`transcribe.finish()` を呼んだとき、または認識エンジンが話し声を聞かなくなったときに来ます。読んでいるタスクを止めると、すぐに聞くのをやめ、それ以降は何も残しません。最初に使うときにマイクと音声認識の許可を求めます。`file` と同じく、同じプラットフォームでデバイス上で動きます。

```asuka check
import platform/transcribe

component Dictation {
  state on = false
  state text = ""

  task(on) {
    if !on { return }
    for await h in transcribe.listen() {
      text = h.text
    }
    on = false
  }

  Column {
    Text(text)
    Button(if on { "Done" } else { "Dictate" }) {
      on press {
        if on { transcribe.finish() } else { on = true }
      }
    }
  }
}
```

## 画面：platform/screen

```asuka
import platform/screen

component Player {
  state note = ""

  task {
    screen.lock(.landscape)               // 動画プレイヤー
    screen.setBars(style: .light, hideStatus: true)
    screen.preventCapture(true)           // チケットのコード
    defer {
      screen.lock(.any)
      screen.setBars()
      screen.preventCapture(false)
    }
    for await _ in screen.screenshots() { note = "That screenshot shows your balance" }
  }

  Text(note)
}
```

- `screen.lock(o)` は、アプリを `o` の向きに回転させ、その向きに固定します。指定できるのは `.portrait`、`.portraitUp`、`.landscape`、`.landscapeLeft`、`.landscapeRight` で、`.any` で固定を解除します。使えるのは `asuka.toml` で許可した向きだけです。Split View やステージマネージャを使っている iPad と、Android 16 の大画面デバイスは、この指定を無視します。
- `screen.setBars(style:, hideStatus:, hideNavigation:)` はステータスバーのスタイルを設定します（`.auto` はダークモードに従い、`.light` は暗い画像の上に置く明るいアイコン、`.dark` は暗いアイコンです）。また、ステータスバーや Android のナビゲーションバーを隠せます。隠したバーは、画面の端からスワイプすると一時的に表示されます。セーフエリアもそれに合わせて変わります。
- `screen.preventCapture(on)` は、アプリがスクリーンショットや画面収録に写らないようにします。Android ではウィンドウをセキュアにするので、キャプチャは黒くなり、最近使ったアプリの一覧でも空白で表示されます。iOS ではスクリーンショットを防げませんが、画面収録、ミラーリング、AirPlay のあいだはアプリを覆い隠します。
- `screen.screenshots()` は、ユーザーが撮ったスクリーンショットのストリームです（iOS、Android 14 以降）。
- `screen.setBrightness(level)` は、アプリが前面にあるあいだ画面を `level`（0〜1）の明るさで表示します（搭乗券のコードを 1 にして、スキャナーで読み取りやすくするなど）。`none` を渡すとシステムの明るさに戻ります。アプリが離れるとユーザーの明るさに戻り、戻ってくるとアプリの明るさになります。Android ではウィンドウ自身の明るさを設定し、iOS では画面の明るさを設定して、離れるときにユーザーの明るさを元に戻します。`screen.brightness()` は現在の明るさです。

デスクトップのウィンドウには向きもシステムバーもないので、`lock` と `setBars` は何もしません。ディスプレイの明るさはユーザーが決めるものなので、`setBrightness` も何もせず、`brightness()` は `none` です。`preventCapture` は、Mac と Windows では（Linux では効きません）アプリのウィンドウがスクリーンショットや画面収録に写らないようにします。デスクトップは、スクリーンショットが撮られたことをアプリに知らせません。ブラウザーで向きを固定できるのは、全画面表示のときかインストールされたアプリの中だけです。バーは全画面表示にすることで隠し、キャプチャの防止もスクリーンショットの通知も、明るさの設定もありません。

これらの設定は画面ごとではなく、アプリ全体のものです。ある画面で設定したら、その画面が閉じるときに元に戻してください（タスクの中の `defer`）。

### 画面のキャプチャ

`screen.capture(display)` は、ディスプレイに映っているものを PNG ファイルとしてアプリのキャッシュに保存します（`Image` で表示でき、共有や保存もできます）。引数がなければメインのディスプレイを、`screen.displays()` の一つを渡せばそのディスプレイをキャプチャします。

```asuka check
import platform/screen

component Grab {
  state shot = ""
  state note = ""

  Column {
    gap: 12
    Button("Capture the screen") {
      on press {
        do {
          shot = try await screen.capture()
        } catch screen.CaptureError.notAllowed {
          note = "Allow screen recording in System Settings"
        } catch e {
          note = e.message
        }
      }
    }
    if !shot.isEmpty() {
      Image(shot) { height: 200 }
    }
    Text(note)
  }
}
```

| | キャプチャするもの |
| --- | --- |
| macOS | ScreenCaptureKit によるディスプレイの内容。初回は「画面収録」の許可を求め（システム設定 › プライバシーとセキュリティ）、断られると `notAllowed` |
| Windows | ディスプレイのピクセル。何も確認しません |
| Linux | スクリーンショットポータルが撮ったもの。初回はユーザーに確認します |
| Web | ユーザーがブラウザーの選択画面で選んだ画面かウィンドウ（ボタンを押したときに呼んでください） |
| iOS、Android | アプリ自身のウィンドウ。アプリが前面にあるあいだ画面に映っているのはそれです |

ユーザーが断ると `CaptureError.notAllowed` を、プラットフォームにできないとき（そのディスプレイがない、画面のキャプチャに対応していないブラウザー、ポータルのない Linux）は `.notAvailable` を投げます。

## アプリのアイコン：platform/appicon

`asuka.toml` には、アプリ自身のアイコンのほかに表示できるアイコンを並べます。各ビルドは、アプリのアイコンと同じようにそれらを作ります。

```toml
icon = "icon.png"

[alternateIcons]
dark = "icons/dark.png"
classic = "icons/classic.png"
```

```asuka check
import platform/appicon

component IconPicker {
  state shown = appicon.current() ?? "default"

  Column {
    gap: 8
    for name in ["default"] + appicon.names() key name {
      Button(name) {
        on press {
          try? await appicon.set(if name == "default" { none } else { name })
          shown = appicon.current() ?? "default"
        }
      }
    }
    Text("Showing {shown}")
  }
}
```

- `appicon.names()` は `alternateIcons` の名前で、`appicon.current()` はアプリが今表示しているものです（`none` はアプリ自身のアイコン）。
- `appicon.set(name)` でそのアイコンに切り替え、`set(none)` でアプリ自身のアイコンに戻します。選んだアイコンは、再起動やアップデートのあとも変わりません。ビルドにない名前は `AppIconError.unknown` を投げます。
- iOS はホーム画面のアイコンを変え、そのことをシステムのアラートでユーザーに伝えます（アプリはこのアラートを省けません）。アプリがバックグラウンドにあるあいだに呼んだ場合は、前面に戻ったときに変わります。Android はアプリが画面から離れたときにランチャーのアイコンを変えます（表示中に変えるとアプリが閉じることがあるため）。アプリドロワーには新しいアイコンが表示され、ホーム画面にあった古いアイコンへのショートカットは消えます。

## 起動画面：platform/splash

`asuka.toml` の `splash` は起動画面です。ライトモードとダークモードそれぞれの背景色の中央に、画像（正方形の PNG、ロゴ）を `size` ポイントの幅（既定は 120、最大 160）で表示します。

```toml
[splash]
image = "logo.png"
size = 96
background = "#4A63B5"
darkBackground = "#1B2440"
```

起動画面は、アイコンをタップしてからアプリの最初のフレームが出るまで表示されます。iOS では起動画面（Launch Screen）を表示し、続いて最初のフレームの準備ができるまで、その複製をアプリの上に重ねます。Android 12 以降ではスプラッシュ画面（アイコンの円の中央に画像を表示）を、それより前のバージョンではウィンドウの背景を使います。デスクトップには起動画面がありません。Web では、asuka.toml で `background` を指定すると、その色（画像は除く）が読み込み中のページの色になり、`hold` の有無にかかわらずアプリの最初のフレームまで続きます。デスクトップと Web では `hold` と `hide` は何もしません。アプリの読み込み中も起動画面を表示し続けるには、起動時に（ルートコンポーネントの中で）`splash.hold()` を呼び、準備ができたら `splash.hide()` を呼びます。

```asuka
import platform/splash

component Root {
  state ready = false

  task {
    splash.hold()
    await loadSession()
    ready = true
    splash.hide()
  }

  if ready { Home() }
}

async fn loadSession() { await sleep(300ms) } // アプリ独自の処理：最初の画面に必要なもの
component Home { Text("Signed in") }
```

## 地図：platform/maps

`maps.View` は地図です。iOS と Mac では Apple マップ、Android では Google マップ、Windows とブラウザーでは OpenStreetMap のタイル、Linux では libshumate のタイルです。`region`（中心と、何度分の範囲を表示するか）をバインドすると、地図を動かせるうえ、ユーザーの操作にも追従します。`markers` は地図上のピンで、`markerPress`、`press`、`regionChange` がユーザーの操作です。

```asuka
import platform/maps

component Places {
  state region = maps.Region(center: maps.Coordinate(lat: 37.77, lng: -122.42), latDelta: 0.08, lngDelta: 0.08)
  state picked = ""

  Column {
    maps.View {
      bind region: region
      markers: [maps.Marker(id: "hq", at: maps.Coordinate(lat: 37.79, lng: -122.39), title: "HQ", color: .blue)]
      mapType: .muted
      grow: 1
      on markerPress(id) { picked = id }
    }
    Text(picked)
  }
}
```

`mapType` は `.standard`、`.satellite`、`.hybrid`、`.muted` のどれかで、`showsUser` はユーザーの現在地を表示します（先に `platform/location` で許可を求めてください）。OpenStreetMap の地図は道路を表示し（ブラウザーでは `.muted` で落ち着いた色になります）、現在地はブラウザーでだけ表示します。Android では、このパッケージを import したアプリは Google の Maps SDK と一緒にビルドされ、asuka.toml の `android.mapsKey` に Maps SDK for Android のキーが必要です。

## バックグラウンド更新：platform/periodic

`platform/periodic` は、アプリが画面に出ていないあいだに、ときどきアプリのコードを実行します。同期や新着の取得などに使います。`periodic.schedule` はシステムにタスクの実行を依頼し、`periodic.runs(name)` はその実行のストリームです。このストリームはアプリのルートのタスクで読みます。

```asuka
import platform/periodic

component Root {
  task {
    periodic.schedule("sync", every: 1h, network: true)
    for await run in periodic.runs("sync") {
      do {
        try await syncInbox()
        run.finish(true)
      } catch {
        run.finish(false)   // Android は再試行します
      }
    }
  }

  Text("Inbox")
}

// アプリ独自の処理
async fn syncInbox() throws {}

app { Root() }
```

いつ実行するかは、`every`（最短 15 分）、バッテリー、ネットワーク、ユーザーのアプリの使い方をもとにシステムが決めます。指定よりずっと長く待つこともあり、ユーザーがオフにすることもできます（iOS の「Appのバックグラウンド更新」）。`network` を指定するとオンラインのときだけ、`charging` を指定すると充電中だけ実行します。同じタスクをもう一度スケジュールすると、設定が変わります。`Run` には、実行にかけてよい時間である `budget` があります。これはシステムの期限より少し短く、iOS では約 25 秒（`charging` を指定したときに使う processing タスクでは数分）、Android（WorkManager）では 9 分です。`run.finish(ok)` で完了を伝えます。`false` を渡すと、Android に再試行を依頼します。実行を読むストリームは一つだけです。そのストリームに別のタスクの実行が届くと、すぐに完了扱いになります。そのため、アプリはタスクを一つにとどめ、その実行を一か所で読むようにします。

アプリが閉じているときにシステムがタスクを実行すると、アプリは画面なしで起動します。ルートが実行され、`runs` を読むタスクが、起動のきっかけになった実行を受け取ります（その実行は、読み手が現れるのを待ちます）。そのときユーザーがアプリを開くと、アプリの画面が表示されます。`periodic.cancel(name)` はタスクを止めます。

デスクトップは、タスクのためにアプリを起動しません。macOS（`NSBackgroundActivityScheduler`）、Windows、Linux では、アプリの実行中にタイマーでタスクを実行し、電源を必要とするタスクはバッテリー駆動中は待機します。ブラウザーは閉じているページのタスクを実行しないので、`schedule` は何もしません。

## ストア：platform/review, platform/tracking, platform/integrity, platform/agerange

`review.request()` は、ストアに評価のプロンプト（App Store の評価プロンプト、Google Play のアプリ内レビュー）を表示するよう依頼します。表示するかどうかはストアが独自の回数制限に基づいて決め、表示したかどうかは知らされません。何かがうまくいったあとに依頼し、ボタンからは呼ばないでください。「評価する」ボタンには `device.openURL(review.appStoreURL(id))` か `review.playStoreURL(id)` を使います。

`tracking.request()` は iOS の App Tracking Transparency のプロンプトを表示し、`.allowed`、`.denied`、`.restricted`、`.notAsked` のいずれかを返します。アプリは、広告識別子を読んだり広告ネットワークとデータを共有したりする前に、このプロンプトを表示しなければなりません。`tracking.current()` は、尋ねずに現在の状態を読みます。Android にはこのようなプロンプトがないので、`.allowed` です。

`platform/integrity` は、リクエストが実機上の本物のアプリから来たことを、自分のサーバーに証明します。iOS では App Attest を使います。`integrity.generateKey()` で鍵を作り（ID を保存しておきます）、`integrity.attestKey(id, challenge)` でサーバーのチャレンジに対して一度だけ Apple に鍵を証明してもらい、以後は `integrity.assertion(id, data)` で各リクエストに署名します。Android では Play Integrity を使います。起動時に `integrity.prepare(cloudProjectNumber:)` を呼び、リクエストごとに `integrity.token(requestHash:)` を呼びます。返された値は、サーバーが Apple や Google に照会して検証します。iOS のシミュレーターでは、`integrity.supported()` は false です。

`agerange.request(13, 18)` は、生年月日を聞かずにユーザーの年齢層を尋ねます。子どもの利用を把握するようアプリに求める法律への対応に使うもので、Apple の Declared Age Range（iOS 26）と Play の Age Signals を使います。プラットフォームが、ユーザーやその保護者に共有するかどうかを尋ねることがあります。`Range` の `status` は、`lower` と `upper` の年齢（0 は制限なし）を持つ `.shared`、`.declined`、`.verificationRequired`（Play）のいずれかで、`source` はその年齢をどうやって知ったのかを示します。プラットフォームが答えられない場合は `AgeRangeError.notAvailable` になります。

```asuka
import platform/agerange
import platform/review
import platform/tracking

async fn afterFirstOrder() throws {
  try? await review.request()
  if try await tracking.request() == .allowed { print("personalized ads") }
  let r = try await agerange.request(13, 18)
  if r.status == .shared && r.lower >= 18 { print("an adult") }
}
```

スマートフォン以外では次のとおりです。Mac には App Store の評価プロンプト、トラッキングのプロンプト、App Attest があります（プロビジョニングプロファイルが必要です。[Mac の場合](#mac-の場合)を参照）が、Declared Age Range はありません。Windows、Linux、Web には、ストアのプロンプト（`review.request` は何もしません）、アテステーション（`integrity.supported` は false）、年齢層（`.notAvailable`）のいずれもありません。これらのプラットフォームではトラッキングにプロンプトは不要です。Windows では「設定」で広告 ID がオンになっていれば `.allowed`、Linux では `.allowed`、ブラウザーでは Global Privacy Control か Do Not Track を送っていれば `.denied` を返します。

アプリ内での販売（`platform/purchase`）については、専用のページで説明しています。[アプリ内課金](purchases.md)を参照してください。iCloud のキーバリューストアと書類（`platform/icloud`）も同様に、[iCloud](icloud.md) のページで説明しています。

## モーションとセンサー：platform/sensors

`platform/sensors` は、デバイスのモーションセンサーと環境センサーを読みます。どのセンサーもストリームで、読んでいるあいだだけ動きます。

```asuka
import platform/sensors

component Level {
  state roll = 0.0

  task {
    for await m in sensors.motion(every: 50ms) { roll = m.attitude.roll }
  }

  Text("{(roll * 180 / 3.14159).fixed(0)}°")
}
```

- `sensors.accelerometer(every:)`、`gyroscope(every:)`、`magnetometer(every:)` は、ほぼ指定した間隔ごとに `Vector`（`x`、`y`、`z`、`time`）を返します。単位は、加速度が g、回転が毎秒のラジアン、磁場がマイクロテスラ（キャリブレーション済み）です。
- `sensors.motion(every:)` は、複数のセンサーを統合したデバイスの動きです。`gravity` とユーザーによる `acceleration`（g 単位）、`rotation`、そして `attitude` を返します。`attitude` は、デバイスの軸を東・北・上（East-North-Up）の座標に変換する `matrix`、`heading`（磁北からの角度。磁気センサーがなければ -1）、`pitch` と `roll` を持ちます。
- `sensors.pressure(every:)` は気圧計（ヘクトパスカルと、高度の変化）、`sensors.light(every:)` は周囲の明るさ（ルクス。Android、Linux）、`sensors.stepUpdates()` は監視を始めてからの歩数です。`sensors.steps(from:, to:)`（時刻は `time.now()` が返す形式）は iOS の歩数の履歴（1 週間分）を読みます。ほかのプラットフォームでは `SensorError.notAvailable` を投げます。
- `sensors.available(sensor)` は、デバイスにそのセンサーがあるかどうかを返します（`Sensor.all` ですべてのセンサーを列挙できます）。シミュレーターやエミュレーターには、センサーがほとんどありません。Mac と PC にはセンサーがありません。Linux では、2-in-1 やタブレットで光センサーと、加速度センサーによる向きが使えます（iio-sensor-proxy）。ブラウザーでは Generic Sensor API かデバイスモーションイベントが使えますが、気圧計と歩数計はありません。デバイスにないセンサーのストリームは `notAvailable` を投げます。
- `sensors.simulate(json)` は、モーションセンサーのない iOS シミュレーターに、Core Motion の形式で読み取り値を送ります（`kind` は 0 が加速度センサー、3 がモーション。ほかに `acceleration`、`gravity`、`user`、`rotation`、`field`）。

値はどのプラットフォームでも同じです。デバイスの軸は Android と W3C の定義どおり（x は画面の右方向、y は画面の上方向、z は画面から手前の方向）で、加速度は加速度センサーが測るとおりの値です（画面を上にして置いたデバイスでは z = +1 g になります。iOS の値はこれに合うよう向きを変えています）。姿勢もすべてのプラットフォームで同じ方法で計算します。読み取りは 1 秒に最大 200 回で、メインスレッドに届きます。

テストでは、`sensors.fake()` がパッケージに `FakeSensors` を渡します。これを使って、存在するセンサー（`present`）を決め、読み取り値を送れます（`accelerate(x, y, z)`、`rotate`、`move(gravity, user:, rotation:, field:)`、`press(hPa)`、`shine(lux)`、`walk(steps)`）。値は、そのセンサーを読んでいる各ストリームに、それぞれが求めた頻度を超えない範囲で届きます。

```asuka
import platform/sensors

test "lying face up" {
  let s = sensors.fake()
  s.accelerate(0, 0, 1)
}
```

## ヘルスケアデータ：platform/health

`platform/health` は、ユーザーの許可を得て、ヘルスケアデータを読み書きします。iOS では HealthKit、Android では Health Connect を使います。`health.request(read:, write:)` は種類（`.steps`、`.distance`、`.activeEnergy`、`.floors`、`.heartRate`、`.restingHeartRate`、`.oxygenSaturation`、`.respiratoryRate`、`.weight`、`.height`、`.bodyFat`、`.water`、`.sleep`、`.mindfulness`）の読み書きを求め、`health.samples(kind, from:, to:, limit:)` はある種類のサンプルを新しい順に読み、`health.total` は合計できる種類を合計し（スマートフォンとウォッチの両方が記録したものを二重に数えません）、`health.write` は一つ保存します。

```asuka check
import platform/health
import std/time

component Steps {
  state steps = 0.0
  state note = ""

  Column {
    gap: 8
    Text("{steps.fixed(0)} steps today")
    Text(note)
  }

  task {
    if !health.available() {
      note = "No health data on this device"
      return
    }
    do {
      if try await health.request(read: [.steps]) {
        steps = try await health.total(.steps, from: time.now().startOfDay(), to: time.now())
      }
    } catch e {
      note = e.message
    }
  }
}
```

- 種類ごとに単位があります。メートル、キロカロリー、1 分あたりの心拍数、キログラム、リットル、そして 0 から 1 の割合（血中酸素ウェルネス、体脂肪率）です。睡眠やマインドフルネスのセッションは値が 1 のサンプルで、開始と終了で長さを表します。
- iOS は読み取りを許可されたかどうかをアプリに伝えません。許可されていない種類は、サンプルがないものとして読めます。パッケージをインポートすると、アプリに HealthKit の機能（リリースのプロファイルに必要です）と利用目的の説明文が加わります（`[ios.usage]` で独自の文にできます）。
- Android には Health Connect が必要です。Android 14 からは組み込みで、Android 9 から 13 ではユーザーがインストールするアプリです（それより前にはありません）。Google Play は健康に関する権限一つひとつに理由を求めるので、アプリは読み書きする種類とプライバシーポリシーを [asuka.toml の `[health]`](asuka-toml.md) に書きます。書いていない種類を求めると `HealthError.notAllowed` を投げます。
- Mac、Windows、Linux、Web にはヘルスケアデータがなく、`available()` は false で、呼び出しは `HealthError.notAvailable` を投げます。

## 広告：platform/ads

`platform/ads` は、iOS と Android で Google の広告（AdMob）を表示します。アプリのビューの間に置くバナー広告と、あらかじめ読み込んでおき区切りのよいところで表示する全画面広告があります。アプリの AdMob アプリ ID は [asuka.toml の `[ads]`](asuka-toml.md#ads) に書きます。iOS や Android 向けのビルドでそのプラットフォームの ID がないと、理由を示してビルドが失敗します。

```asuka check
import platform/ads

let rewardUnit = ads.testUnit(.rewarded)

component Game {
  state coins = 0
  state ready = false
  state note = ""

  task {
    do {
      let c = try await ads.consent()
      if c.canRequestAds {
        try await ads.start()
        ready = true
      }
    } catch e {
      note = e.message
      ready = ads.currentConsent().canRequestAds
    }
  }

  Column {
    gap: 12
    grow: 1
    Text("{coins} coins")
    Text(note)
    Button("Watch an ad for 10 coins") {
      disabled: !ready
      on press {
        do {
          let ad = try await ads.rewarded(rewardUnit)
          if let r = try await ad.show() {
            coins += r.amount
          }
        } catch e {
          note = e.message
        }
      }
    }
    View { grow: 1 }
    if ready {
      ads.Banner(ads.testUnit(.banner)) {
        on error(message) { note = message }
      }
    }
  }
}
```

- **まず同意を得る。** 法律で求められる地域（欧州経済領域、英国、スイス、米国の一部の州）では、Google は広告の前にユーザーの同意を得ることを求めています。`ads.consent()` は Google の User Messaging Platform で同意を集めます。同意が必要でまだ得られていなければ、AdMob の「プライバシーとメッセージ」で用意したフォームを表示し、返ってくる `Consent` で広告をリクエストしてよいか（`canRequestAds`）がわかります。アプリを起動するたび、広告を読み込む前に呼んでください。`privacyOptionsRequired` が true のときは、アプリの設定画面からフォームをもう一度開けるようにします（`ads.privacyOptions()`）。シミュレーターやエミュレーターで試すときは、`ConsentOptions(debugGeography: .eea)` でどこでもフォームを表示できます。
- **トラッキングの許可。** iOS の App Tracking Transparency のプロンプトは `platform/tracking` の役目です。同意を得たあと、最初の広告の前に表示してください。ユーザーが許可しなくても広告は表示されますが、パーソナライズされなくなります。
- **バナー。** `ads.Banner(unit)` は Google のアンカー アダプティブ バナーです。幅はレイアウトで決まった幅になり、高さはその幅に合わせて Google が決めます。`size:` で固定サイズも選べます（`.banner` は 320×50、`.mediumRectangle` は 300×250、ほかに `.largeBanner`、`.fullBanner`、`.leaderboard`）。イベントで、広告が表示されたこと（`loaded`）、表示されない理由（`error`）、ユーザーがタップしたこと（`click`、`open`、`close`）がわかります。画面の上端か下端に置き、ユーザーがよくタップする場所から離してください。
- **全画面広告。** `ads.interstitial(unit)`、`ads.rewarded(unit)`、`ads.rewardedInterstitial(unit)`、`ads.appOpen(unit)` で広告を一つ読み込みます。`show()` はそれをアプリの上に表示し、ユーザーが閉じると戻ります。最後まで見たリワード広告なら `Reward`（AdMob の広告ユニットで設定した種類と量）を、それ以外は `none` を返します。広告は一度しか表示できず、1 時間で期限が切れます（アプリ起動広告は 4 時間）。必要になる少し前に読み込んでください。報酬をサーバー側で検証するなら、`Request` の `userId` と `customData` が Google のコールバックでサーバーに届きます。
- **設定。** `ads.configure(Settings(...))` で、以降のすべてのリクエストに付く内容を決めます。`testDevices`、広告の対象年齢 `age`（`.child`、`.teen`）、`maxRating`、`personalized: false`、動画広告の `muted` と `volume` です。
- **テスト広告。** `ads.testUnit(format)` は、実行中のプラットフォームでその形式に使える Google のテスト用広告ユニットです。常に広告があり、テスト広告であることが表示されます。開発中はこれと、`[ads]` に書いた Google のサンプルアプリ ID を使ってください。自分の本物の広告をタップすると、AdMob アカウントが停止されることがあります。シミュレーターとエミュレーターには常にテスト広告が届きます。実機では、その ID（広告の読み込み時に SDK がログに出します）を `testDevices` に加えてください。
- **エラー。** 広告がないと読み込みは `AdsError.noFill` を投げます。ネットワークの問題は `.network`、広告ユニットやアプリ ID の誤りは `.invalidRequest`、同じ広告を 2 回表示すると `.alreadyShown` です。
- **ほかのプラットフォーム。** Mac、Windows、Linux、Web には AdMob がありません。`ads.available()` は false で、Banner は何も表示せず、`consent()` は広告をリクエストできないと答え、広告の読み込みは `AdsError.notAvailable` を投げます。

テストでは、`ads.fake()` がパッケージに `FakeAds` を渡します。広告はすぐに読み込まれ、ユーザーがリワード広告を最後まで見ると（`watches`）その `reward` がもらえ、テストは読み込まれた広告（`loaded`）と表示された広告（`shown`）を確認できます。`noFill`、`unavailable`、`consent` を変えると結果が変わります。

```asuka
import platform/ads

test "a reward" {
  let f = ads.fake()
  let ad = try await ads.rewarded("unit")
  expect(try await ad.show() == f.reward)
}
```

## 画像と動画フレームの編集：platform/imageedit

`imageedit.edit(source, actions)` はプラットフォーム自身のコーデックで画像を編集し、JPEG か PNG で書き出します。カメラや写真ピッカーが返すもの（HEIC、JPEG、PNG、WebP など）を読み、向きの情報どおりに正しい向きに直すので、アップロードやキャッシュ、`Image` に向いています。アクションは順に実行されます。`.resize(width, height)`（片方を 0 にすると縦横比を保ちます）、`.crop(x, y, width, height)`、`.rotate(degrees)`、`.flipHorizontal`、`.flipVertical`、`.fill(...)`（単色で塗りつぶし、元の内容は消えます）、`.pixelate(...)` です。`imageedit.size(source)` は画像のサイズを読み、`imageedit.videoFrame(source, at:)` は動画のフレームを画像として取り出します。

```asuka
import platform/imageedit

async fn thumbnail(photo: string) throws -> string {
  let r = try await imageedit.edit(photo, [.resize(1080, 0), .crop(0, 0, 1080, 1080)], quality: 0.8)
  return r.path
}

async fn poster(video: string) throws -> string {
  return (try await imageedit.videoFrame(video, at: 1s, actions: [.resize(640, 0)])).path
}
```

iOS と Mac では Core Graphics と Image I/O、Android では `Bitmap` と `MediaMetadataRetriever`、ブラウザーでは canvas、Windows では WIC と Media Foundation、Linux では GdkPixbuf と GStreamer を使います。画像でないソースには `EditError.noImage` を投げます。

### ビューを画像にする

`imageedit.capture(view: ref)` は、`Ref` が持つビューを、中にあるものも含めていま表示されているとおりに、アプリのキャッシュに PNG（`format: .jpeg` なら JPEG）として描きます。共有するカードや保存するレシートに使います。描くのはプラットフォームなので、ウィジェットは画面で見えるとおりになります。`scale` は 1 ポイントあたりのピクセル数です（デフォルトは画面のもの）。

```asuka check
import platform/device
import platform/imageedit

component Receipt(total: string) {
  let card = Ref()

  Column {
    View {
      ref: card
      padding: 16
      radius: 12
      background: #f2f2f7
      Text("Paid {total}")
    }
    Button("Share") {
      on press {
        do {
          let image = try await imageedit.capture(view: card)
          try await device.shareFiles(paths: [image.path], from: card)
        } catch e {
          print(e.message)
        }
      }
    }
  }
}
```

iOS と Mac ではビューのレイヤーを、Android ではその `View` を描きます（中に動画や地図があれば、その位置のウィンドウのピクセルを使います）。Windows と Linux では、ビューの位置にあるウィンドウのピクセルを取ります（Windows は `scale` にかかわらず画面のスケールです）。ブラウザーでは要素を SVG 画像にコピーするので、ほかのサイトの画像で、それを許可していない（CORS）ものはそこで空白になります。

## 音声読み上げ：platform/speech

`speech.speak(text)` はテキストの言語に合ったデバイスの声で（または `Options(language:, voice:, rate:, pitch:, volume:)` の指定で）読み上げ、読み終わってから戻ります。読み上げ中に頼まれたものは順番を待ちます。`stop()` で止め（読み上げ中の呼び出しは `SpeechError.stopped` を投げます）、`pause()` と `resume()` で単語の区切りで一時停止して再開し、`speaking()` で読み上げ中かどうかがわかり、`voices()` で声の一覧とその `quality` が得られます。

```asuka
import platform/speech

component Reader(text: string) {
  Row {
    gap: 12
    Button("Read") { on press { try await speech.speak(text, options: speech.Options(rate: 1.1)) } }
    Button("Stop") { on press { speech.stop() } }
  }
}
```

iOS と Mac では AVSpeechSynthesizer、Android では OS の `TextToSpeech`、ブラウザーでは `speechSynthesis`、Windows では SAPI、Linux では Speech Dispatcher を使います。読み上げている間は、ほかのアプリの音が小さくなります。

## 連絡先：platform/contacts

```asuka
import platform/contacts

component Invite {
  state who = ""

  Column {
    Button("Choose a friend") {
      on press {
        if let c = try? await contacts.pick() {
          who = "{c.name}, {c.phones.count()} phones"
        }
      }
    }
    Text(who)
  }
}
```

- `contacts.pick()` は、システムの連絡先ピッカーを表示します。ユーザーが一件を選び、アプリにはその連絡先だけが見えます。権限は求めません（Android では、権限がなければ名前だけを取得できます）。ユーザーがキャンセルした場合は none を返します。
- `contacts.add(contact)` は、内容を入力済みにしたシステムの新規連絡先画面を表示し、ユーザーがそこで保存します。保存したかどうかを返します。権限は求めません。
- `contacts.request()` はアクセスの許可を求め（`.notAsked`、`.denied`、`.limited`、`.allowed`）、`contacts.list(name:, limit:)` はアドレス帳を読みます（名前で検索し、上限の件数まで）。各 `Contact` の名前、`organization` と `jobTitle`、ラベル付きの `phones` と `emails`、`birthday`、`note` を読めます。iOS 18 の限定アクセスでは、ユーザーが選んだ連絡先だけが返ります。アクセスが許可されていなければ、`list` は「contacts: not allowed」を投げます。

Mac では Contacts フレームワークを使います。`pick` はシステムのピッカーで、ウィンドウの上端からポップオーバーで表示されます。`add` はウィンドウの上のアラートで確認してから保存します（macOS には新規連絡先画面がないためです）。Linux では、[Linux の場合](#linux-の場合)で説明しているとおり、Evolution Data Server のアドレス帳を読み書きします。Windows はパッケージ化されたアプリにしか連絡先を渡さないので、`request` は `.denied` を返し、`pick` はエラーを投げ、`add` は vCard をユーザーの連絡先アプリで開きます。ブラウザーでは、Contact Picker がある環境（Android の Chrome）なら一件を選べ、`add` は vCard をダウンロードします。

## カレンダー：platform/calendar

```asuka
import platform/calendar
import std/time

async fn addDemo() throws -> bool {
  let start = time.date(2026, 12, 1, hour: 15)
  return try await calendar.add(calendar.Event(title: "Demo", location: "Room 2", start: start, end: start + 30min))
}
```

- `calendar.add(event)` は、内容を入力済みにしたシステムのイベントエディターを表示し、ユーザーがそこでイベントを保存します。保存したかどうかを返します。権限は求めません（iOS 17 以降）。iOS は保存されたかどうかを伝えますが、Android のカレンダーアプリは伝えません。
- `calendar.request()` はアクセスの許可を求めます（`.notAsked`、`.denied`、`.writeOnly`、`.full`）。`.full` なら、`calendar.calendars()` でカレンダーの一覧を、`calendar.events(from:, to:, calendars:)` で二つの時刻のあいだのイベントを取得できます。繰り返しイベントは、発生ごとに一件ずつ返ります。`.full` でなければ、これらは「calendar: not allowed」を投げます。
- イベントの `start` と `end` は `time.Time` です。終日イベントでは日付を表し、それぞれ初日の UTC の午前 0 時と、最終日の翌日の UTC の午前 0 時です。

Mac では EventKit を使います。`add` はウィンドウの上のアラートで確認してから保存します（macOS にはアプリ用のイベントエディターがないためです）。Linux では Evolution Data Server のカレンダーを読み書きします。Windows はパッケージ化されたアプリにしかカレンダーを渡さないので、`request` は `.denied` を返し、`add` は iCalendar ファイルをユーザーのカレンダーアプリで開きます（Windows は保存されたかどうかを伝えないので、false を返します）。ブラウザーはカレンダーを読めず、`add` は `.ics` ファイルをダウンロードします。

## ホーム画面ウィジェット：platform/widget

アプリのウィジェットは `asuka.toml` の `widgets` で宣言し、アプリのコードから `widget.set` で内容を設定します。

```toml
[[widgets]]
kind = "today"
name = "Today"
description = "What's next"
sizes = ["small", "medium", "rectangular"]
```

```asuka
import std/time
import platform/widget

fn showToday(meeting: time.Time) {
  widget.set("today", widget.Entry(
    title: "Next", symbol: "calendar", value: "3 tasks", body: "Call Ana at 4",
    progress: 0.4, showProgress: true, until: meeting, url: "myapp://today",
  ))
}
```

ウィジェットは、プラットフォームのテンプレート（または後述のアプリ独自のレイアウト）を使い、サイズに合わせたレイアウトで `Entry` を表示します。表示されるのは、`title`（`symbol` 付きで、`tint` の色）、大きな `value`、`body`、`progress` バー、`until`（`time.Time`）までリアルタイムで進むカウントダウン、`caption` です。アプリが別のエントリーを設定するまで、アプリが動いているかどうかにかかわらず表示され続けます。タップすると、その `url` でアプリが開きます（アプリの `openURL` で受け取ります）。`widget.setAll(kind, entries)` に、それぞれ `at` を持つエントリーを渡すとタイムラインになり、各エントリーはその時刻から表示されます。`widget.clear(kind)` でウィジェットを空にします。

サイズは `small`、`medium`、`large`、そして iOS の `extraLarge`（iPad）と、ロック画面の `circular`、`rectangular`、`inline` です。iOS では、CLI がアプリに組み込む WidgetKit 拡張機能になり、アプリと App Group を共有します。リリースビルドには App Groups ケイパビリティと、拡張機能の ID（アプリの ID に `.widgets` を付けたもの）のプロビジョニングプロファイルが必要です。Android では、種類ごとに一つの App Widget になり、Material You の色を使い、ランチャーのセルに合わせたサイズになります。デスクトップと Web にはウィジェットがないので、`widget.set` は何もしません。

### アプリ独自のレイアウト

`Entry` の `layout` を使うと、テンプレートの代わりにアプリが決めたレイアウトでウィジェットを描きます。ウィジェットに表示できる部品のツリーで、プラットフォームがレイアウトして描画します（iOS の拡張では SwiftUI のビュー、Android では App Widget のビュー）。ウィジェットは動いているアプリが描くのではなく、アプリが最後に設定した内容をホーム画面が描くからです。`sizes` で、サイズごとに専用のレイアウトを指定できます。

```asuka check
import std/fs
import std/time
import platform/widget

fn energy(now: time.Time) -> widget.Layout = widget.row([
  widget.image("cover").frame(width: 56, height: 56).background(.clear, radius: 12),
  widget.column([
    widget.row([widget.symbol("bolt.fill", size: 13, color: .orange), widget.text("Energy", font: .caption.bold())]),
    widget.text("72%", font: Font(size: 30, weight: .bold), lines: 1),
    widget.progress(0.72, color: .orange),
    widget.text("Updated {now.format("HH:mm")}", font: .caption2, color: .secondaryLabel),
  ], spacing: 4, align: .stretch).fill(),
], spacing: 12)

async fn showEnergy() throws {
  widget.setImage("cover", try await fs.readAsset("cover.png"))
  let now = time.now()
  widget.set("energy", widget.Entry(url: "myapp://energy", layout: energy(now), sizes: [
    .small: widget.column([
      widget.text("Energy", font: .caption.bold()),
      widget.spacer(),
      widget.gauge(0.72, label: "72", color: .orange),
      widget.timer(until: now + 25min, font: .footnote),
    ]),
  ]))
}
```

部品は次のとおりです。`column` と `row`（それぞれ `spacing` と `align` を持ちます）、`layers`（重ねて表示し、`x` と `y` で配置します）。`text`（`Font` と `Color`、最大 `lines` 行）。`symbol`（SF Symbol。Android では対応する Material のシンボル）。`image`（`widget.setImage` が保存した画像。ウィジェットが読める場所に置かれます）。`progress`（バー）と `gauge`（ラベル付きのリング）。`timer`（ある時刻までのカウントダウンで、過ぎるとそこからカウントアップします。ウィジェットがスリープしていても進みます）。`spacer` と `divider`。どの部品にも `padding`、`background`（色。角を丸められます）、`frame`（ポイント単位のサイズ）、`fill`（列や行の残りのスペースを占めます）、`link`（ウィジェットのその部分をタップしたときにアプリが開く URL。iOS の小さいウィジェットはエントリーの `url` だけを開きます）、`opacity` を付けられます。セマンティックカラー（`.label`、`.secondaryLabel`、`.accent` など）はホーム画面のライトモードとダークモードに従い、テキストスタイルはユーザーの文字サイズに従います。

## ライブアクティビティ：platform/liveactivity

ライブアクティビティは、いま起きていることを、変化に合わせてアプリの外に表示します。配達中の荷物、配車、試合のスコア、タイマーなどです。iOS ではライブアクティビティ（iOS 16.2）として、ロック画面と Dynamic Island に表示され、Dynamic Island のない iPhone ではバナーで表示されます。Android では常駐する通知になり、Android 16 ではこれが Live Update に昇格して、通知シェードとロック画面の最上部に表示され、ステータスバーにチップが出ます。

```asuka
import platform/liveactivity
import std/time

async fn followOrder(eta: time.Time) throws {
  let a = try await liveactivity.start(liveactivity.Content(
    title: "Order #1042", subtitle: "Ana is picking it up", value: "12 min",
    symbol: "bag.fill", until: eta, url: "myapp://orders/1042",
  ))

  // あとで：
  a.update(liveactivity.Content(title: "Order #1042", subtitle: "2 stops away", value: "4 min", until: eta),
    alert: liveactivity.Alert(title: "Almost there", body: "Ana is 2 stops away"))
  a.end(liveactivity.Content(title: "Order #1042", subtitle: "Delivered"))
}
```

アクティビティは、プラットフォームのテンプレートで `Content` を表示します。空の項目は表示されません。表示されるのは、`title` と `subtitle`、`value`（横に大きく表示され、Dynamic Island のコンパクト表示や Android 16 のステータスバーのチップにも使われます）、`tint` の色の `symbol`、`progress` バー、そしてアプリが動いていなくても進む `until`（`time.Time`）までのカウントダウンです。`since`、`until`、`showProgress` を指定すると、バーが自動で伸びていきます。タップすると、その `url` でアプリが開きます（アプリの `openURL` で受け取ります）。

`liveactivity.start(c, push:, relevance:)` は、アプリが画面に出ているあいだだけ動作します（どちらのプラットフォームでも、バックグラウンドのアプリはアクティビティを開始できません）。`relevance` は、アプリのアクティビティの並び順を決めます。`Alert` を付けると、更新とともに画面が点灯します。iOS では Dynamic Island が展開され、Apple Watch にも表示されます。Android では通知をもう一度鳴らします。`end(c, dismiss:)` は、最後の内容を、システムが取り除くまで（既定の `.later`。最大 4 時間）か、指定した時刻まで（`.at(t)`）表示したままにします。`.now` を指定すると、すぐに取り除きます。`liveactivity.enabled()` は、ユーザーがアクティビティを許可しているかどうかを返します（Android では最初の `start` で通知の許可を求めます）。このパッケージは、まだ実行中のアクティビティを列挙しません。

`liveactivity.events()` は、アクティビティに起きたことを伝えます。（ユーザー、システム、サーバーによって）`.ended` になったもの、サーバーによって `.started` になったもの、そしてプッシュトークン（`.pushToken`、`.pushToStartToken`）で、それぞれが `activity` と `token` を持つ `Event` です。`push: true` を指定すると、アプリが動いていないあいだも、サーバーからアクティビティを更新・終了できます。そのプッシュトークンと `id` をサーバーに送ってください。iOS では、サーバーは `liveactivity` タイプの APNs プッシュを送り、その `content-state` は内容を JSON にしたものです（`Content` のフィールドで、`since` と `until` は `sinceMs` と `untilMs` になります）。Android では、`asuka.liveActivity`（`update`、`end`）、`asuka.id`、`asuka.content` を持つ Firebase のデータメッセージを送ります。サーバーからアクティビティを開始するには、iOS では iOS 17.2 の push-to-start トークンを使い（attributes の型は `AsukaActivityAttributes` で、新しいアクティビティの `id` を含めます）、Android では `asuka.liveActivity` を `start` にします。プッシュを使うには、アプリに `platform/push` が必要です。iOS では Push Notifications ケイパビリティが、Android では Firebase が要ります。

iOS のビルドは、このパッケージを呼ぶアプリにウィジェット拡張機能を追加し、アクティビティのテンプレート（ウィジェットと同じく SwiftUI 製）と `NSSupportsLiveActivities` を加えます。リリースには、ウィジェットと同じく、拡張機能の ID のプロビジョニングプロファイルが必要です。ユーザーは「設定」でアプリのライブアクティビティをオフにでき、Android 16 では通知の設定で Live Update への昇格をオフにできます。デスクトップと Web にはこの機能がないので、`enabled()` は false で、`start` は「liveactivity: not available」を投げます。

## ショートカット：Siri、Spotlight、ランチャー

`asuka.toml` の `shortcuts` は、システムがアプリのために表示するアクションです。それぞれ、その URL でアプリを開き、URL はアプリの `openURL` に渡ります。

```toml
[[shortcuts]]
id = "newNote"
title = "New Note"
url = "notes://new"
symbol = "square.and.pencil"
phrases = ["New note in ${app}"]
```

iOS では App Shortcuts になります。Siri がそのフレーズ（それぞれアプリ名を表す `${app}` を含みます）を聞き取り、ユーザーが何も設定しなくても、Spotlight、「ショートカット」App、アクションボタンで使えます（最大 10 個）。CLI はこれらを App Intents としてコンパイルし、Xcode と同じようにメタデータを抽出します。Android ではランチャーのショートカットになり、アプリのアイコンを長押しすると、アクセントカラーのシンボル付きで表示されます。デスクトップと Web には追加されません。

シミュレーター向けのビルドは、Xcode と同じように、キーチェーンにある（`asuka.toml` で指定したチームの）Apple Development の証明書で署名されます。App Intents は、チームのあるアプリでしか動かないためです。証明書がなければビルドはアドホック署名になり、CLI はショートカットが動かないことを知らせます。

### パラメーター

ショートカットの `params` は、Siri と「ショートカット」App がユーザーに尋ねる値です。それぞれの値はエスケープされて、URL の `{name}` に入ります。

```toml
[[shortcuts]]
id = "addTask"
title = "Add Task"
url = "todo://add?title={title}&list={list}"
phrases = ["Add a task in ${app}", "Add a ${list} task in ${app}"]

[[shortcuts.params]]
name = "title"
title = "Title"
prompt = "What's the task?"

[[shortcuts.params]]
name = "list"
title = "List"
type = "options"
options = [{ value = "work", title = "Work" }, { value = "home", title = "Home" }]
default = "home"
```

「Add a task in Todo」と言うと Siri が *What's the task?* と尋ね、`todo://add?title=Buy%20milk&list=home` を開きます。「Add a work task in Todo」と言えば、タイトルだけを尋ねます。パラメーターの `type` は `string`（デフォルト）、`int`、`float`、`bool`、`options` のいずれかです。`default` のないパラメーターは Siri が尋ねます。ただし `optional` なら尋ねず、URL のその部分は空になります。フレーズには `options` のパラメーター（`${list}`）を入れられ、Siri は各選択肢のタイトルを聞き取ります。それ以外の型はフレーズに入れられません。Siri は任意のテキストを聞き取れないためです。「ショートカット」App では、どのパラメーターもアクションの項目になります。Android では、ランチャーのショートカットは各パラメーターのデフォルト値（なければ空）を入れた URL を開きます。

### アプリ自身のデータ

`entity` 型のパラメーターは、アプリ自身のデータを一つ受け取ります。メモ、プレイリスト、連絡先などです。アプリが `platform/shortcuts` でそれが何かを伝えると、アプリが動いていないときでも、Siri と「ショートカット」App がタイトルでそれを提示します。

```toml
[[shortcuts]]
id = "markDone"
title = "Mark Done"
url = "notes://done/{note}"
phrases = ["Mark ${note} done in ${app}"]

[[shortcuts.params]]
name = "note"
title = "Note"
type = "entity"
entity = "note"
```

```asuka check
import platform/shortcuts

struct Note {
  id: int
  text: string
}

// メモが変わるたびに。
fn offer(notes: [Note]) {
  shortcuts.setEntities("note", notes.map(n => shortcuts.Entity(id: "{n.id}", title: n.text)))
}
```

「Mark Buy milk done in Notes」と言うと `notes://done/12` が開きます。選ばれたデータの `id` が URL のパラメーターの位置に入ります。フレーズには、`options` のパラメーターと同じように entity のパラメーターを入れられます。Android では、それぞれのデータが一つずつランチャーのショートカットになり（ランチャーが表示できる数まで）、同じ URL を開きます。[`examples/notes`](../../examples/notes) がこの機能を使っています。

## 車の画面：platform/car

`platform/car` はアプリを車の画面、つまり CarPlay と Android Auto に表示します。表示されるのはアプリのビューではなくプラットフォームのテンプレート（リスト、グリッド、タブ、情報、地図上の場所、再生中の表示、ナビゲーションアプリの地図とその案内）で、値として記述し、アプリの状態に追従させ、行を選んだときに何をするかを指定します。`asuka.toml` の `[car]` にアプリのカテゴリを書くと、CLI がそこから CarPlay の entitlement とシーン、Android Auto のサービスを追加します。詳しくは[車の画面](car.md)を参照してください。

```asuka check
import platform/car

struct Album {
  title: string
  cover: string
  tracks: [string]
}

let albums = [Album(title: "Morning", cover: "covers/morning.jpg", tracks: ["Sunrise", "Coffee"])]

fn album(a: Album) -> car.Template = .list(title: a.title, items: a.tracks.map(t => car.Item(title: t)))

component Library {
  car.Screen {
    root: .list(title: "Library", items: albums.map(a => car.Item(
      title: a.title,
      image: a.cover,
      browsable: true,
      run: () => car.push(() => album(a)),
    )))
  }

  Text(if car.connected() { "On the car's screen" } else { "Not in a car" })
}

app { Library() }
```

## 触覚フィードバック：device.haptic

`device.haptic(kind)` は触覚フィードバックを再生します（iOS では UIFeedbackGenerator、Android では `performHapticFeedback`）。種類は `device.Haptic` の `.selection`、`.light`、`.medium`、`.heavy`、`.success`、`.warning`、`.error` のいずれかです。Mac ではトラックパッドのフィードバックを再生します（感圧タッチトラックパッドで、`.selection` では整列、それ以外ではレベル変更のフィードバック）。ブラウザーでは、可能な環境（Android の Chrome）で振動します。Windows と Linux には触覚フィードバックがないので、何もしません。

```asuka
import platform/device

component Save {
  Button("Save") { on press { device.haptic(.success) } }
}
```

アニメーションに合わせたフィードバックについては、[モーションとジェスチャー](motion.md)を参照してください。

## クラッシュレポート：platform/crash

`platform/crash` をインポートしたアプリは、クラッシュのたびにレポートを保存し、次の起動時にそれを読んで、自分のサーバー、Sentry、Crashlytics など好きな送り先に送れます。Asuka 自身は何もアップロードしません。

```asuka
import platform/crash

component Root {
  task {
    crash.set("user", "u42")
    for await r in crash.reports() {
      if (try? await send(r)) ?? false {   // 自前のアップロード処理（Report は JSON）
        crash.delete(r.id)
      }
    }
  }

  Button("Check out") {
    on press {
      crash.breadcrumb("opened the cart")
      do {
        try await checkOut()
      } catch e {
        crash.record(e)   // 処理済みのエラーもレポートする
      }
    }
  }
}

// アプリ独自の処理
async fn send(r: crash.Report) throws -> bool = true
async fn checkOut() throws {}

app { Root() }
```

`crash.reports()` はストリームです。保存されているレポートを古い順に渡し、その後も新しいレポート（起動の少しあとに iOS が届ける診断情報や、記録されたエラー）が届くたびに渡します。どのレポートも一度だけです。レポートは `crash.delete(id)` を呼ぶまで残り、50 件を超えると古いものから消えます。`id` は何度読んでも同じなので、サーバーは二重に送られたレポートを見分けられます。`crash.record(e)` はアプリが処理したエラーのレポートを作ります。`crash.breadcrumb(message)` は、アプリが直前に何をしていたかとしてレポートに載る記録に追加します。`crash.set(key, value)` はレポートに含まれる値を設定します（`""` で削除します）。

`Report` は、`id`、`kind`、致命的だったかどうか（`fatal`）、`time`（`time.Time`）、`message`（何がなぜクラッシュしたかを表す一行）、`text`（プラットフォームが書いたとおりのレポート全体）、`threads`、`context` を持ちます。プラットフォームごとに報告される内容（`kind`）は次のとおりです。

| | iOS | Android | macOS | Windows | Linux | Web |
|---|---|---|---|---|---|---|
| `"panic"`：Asuka 自身の致命的なエラー（Asuka のコードのスタック付き） | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| `"exception"`：捕捉されなかった例外 | NSException（Objective-C、Swift の `NSException`） | JVM の `Throwable` | — | — | — | 捕捉されなかった JavaScript のエラー（致命的ではない） |
| `"native"`：シグナル、Mach や Windows の例外 | MetricKit | システムの tombstone | MetricKit、致命的なシグナル（Asuka のレポート） | Asuka のレポート | Asuka のレポート（ネイティブコードでのシグナル） | — |
| `"hang"` | MetricKit のハング（致命的ではない）、ウォッチドッグ | ANR とそのトレース | MetricKit | — | — | — |
| `"oom"` | メモリ上限（MetricKit が報告した場合） | メモリ不足による強制終了 | — | — | — | — |
| `"resource"` | CPU とディスク書き込みの例外（致命的ではない） | リソースの過剰使用 | MetricKit | — | — | — |
| `"nonfatal"`：`crash.record(e)` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |

どのプラットフォームでも、Asuka が記録を残します。Asuka 自身の致命的なエラーは、実行が終わるときに Asuka のスタックとともに書き込まれます。捕捉されなかった例外は、アプリが終了するときに書き込まれます（それより前に設定されたハンドラー、たとえばクラッシュ SDK のハンドラーも、そのあとで実行されます）。それ以外は、次回以降の起動時にシステムが渡してくれます。iOS と macOS では MetricKit の診断情報、Android ではアプリのプロセスがどのように終了したかの記録です（`getHistoricalProcessExitReasons`、Android 11 以降。ネイティブクラッシュの tombstone は Android 12 から）。Asuka が報告したクラッシュをシステムが改めて報告したもの（捕捉されなかった例外による iOS の SIGABRT、Android の `REASON_CRASH`）は除外されます。macOS、Windows、Linux では、致命的なシグナルや処理されなかった例外は、次回の起動時にレポートになります。

各レポートには `context` があります。アプリのバージョンとビルド番号、デバイスの機種と OS、プロセスとその実行時間、フォアグラウンドにいたかどうか、最近のブレッドクラム（最大 100 件）、設定した値です。これらは変わるたびにアプリのデータディレクトリに書き込まれ、そのコストは書き込み一回分ほどです。そのため、アプリが何によって終了しても、クラッシュの前にはディスクに書かれています。ライフサイクルの変化もブレッドクラムになります（「app: background」）。

レポートの `text` は、プラットフォームが渡す内容そのもの（Asuka のクラッシュ出力、スタックトレース、tombstone、ANR のトレース）です。`threads` は、それらのスタックをフレームに分解したもので、クラッシュしたスレッドが先頭に来ます。Asuka のフレームは `function`、`file`、`line` を持ちます。ネイティブのフレームは、`binary`、バイナリの UUID（Mach-O）またはビルド ID（ELF）である `binaryId`、`address`、バイナリ内の `offset` を持ちます。サービスがビルドのシンボルを使ってシンボル化するには、これらの情報が必要です（[配布](shipping.md#クラッシュレポート用のシンボル)を参照）。

**Sentry や Crashlytics への送信。** Sentry には、各レポートからイベントを作って送ります（送り先はエンベロープのエンドポイント `https://<host>/api/<project>/envelope/` で、DSN のキーを付けます）。`exception.values[0]` の `type` に `kind`、`value` に `message`、`stacktrace.frames` にクラッシュしたスレッドのフレームを入れます。フレームは外側から順に並べ（Sentry の順序で、`Thread` とは逆です）、それぞれ `function`、`filename`、`lineno` を、ネイティブのフレームなら `instruction_addr` と `package` を持たせます。また `debug_meta.images` には、フレームに出てくる各バイナリを `{"type": "macho", "debug_id": binaryId, "image_addr": address - offset}`（Android では `"elf"`）として入れます。こうすると、`sentry-cli` でアップロードした dSYM やシンボルが適用されます。`context` は `contexts`、`tags`、`breadcrumbs` に入れます。Crashlytics には、送信先となる公開のエンドポイントがありません。レポートを自分のサーバーに送るか、独自のネイティブパッケージ（[ネイティブコード](native.md)）から Crashlytics の SDK に致命的でないエラーとして記録してください。どちらの場合も、App Store の審査ではクラッシュデータについて尋ねられます。送信するなら、そのことを申告してください（`asuka.toml` の `ios.collected` に `{type = "CrashData", purposes = ["AppFunctionality"]}`）。

**プラットフォームごとの違い。** iOS では、レポートを読むアプリにだけ MetricKit をリンクし、MetricKit は診断情報を共有拡張機能ではなくアプリに届けます。MetricKit は iOS 17 以降ではクラッシュのプロセス ID を持っているので、その実行のコンテキストと結び付けられます。それより前のバージョンでは、クラッシュの直前の実行と結び付けます。シミュレーターでは MetricKit は診断情報を届けません。Android では追加の設定は要りませんが、Android 11 より前にはネイティブクラッシュと ANR の記録がありません。macOS では、MetricKit は macOS 12 から使えます。アプリを終了させた、捕捉されない Objective-C の例外は、MetricKit のクラッシュとして報告されます（メインスレッドの例外は AppKit が捕捉してログに出力します）。Windows エラー報告は、これまでどおり記録を残します。ブラウザーでは、記録はアプリのファイル（IndexedDB）に置かれ、次の起動時に読まれます。捕捉されなかった JavaScript のエラー（Asuka のものでも、アプリの `web/*.js` のものでも）は、その場で `"exception"` レポートになります。致命的なエラーとしては扱われず（ページは動き続けます）、その実行のコンテキストが付きます。1 回の実行につき最大 20 件で、同じエラーは一度だけです。ページはオリジンの `localStorage` に自分の実行が生きていることを記録するので、同じアプリのほかの開いているページがその記録に手を出すことはありません。`asuka test` では記録は残らず、`reports()` は何も渡さずに終わり、ほかの関数は何もしません。

**`asuka dev` では**、開発用アプリもビルドと同じように記録を残します。開発用アプリ自身のネイティブクラッシュはアプリのものではないので、アプリには報告されません。アプリのビルド（`asuka dev --once`、`asuka build`）では、すべてが報告されます。

## アナリティクス：platform/analytics

`platform/analytics` は、ユーザーがアプリで何をしたかを Google Analytics に伝えます。アプリが記録するイベントとそのパラメータ、ユーザーが見た画面、ユーザーが誰か（ユーザー ID とユーザープロパティ）、ユーザーが何に同意したか（同意モード、Consent Mode）です。iOS・macOS・Android では Google Analytics for Firebase、Web では gtag.js、Windows と Linux では GA4 の Measurement Protocol を使います。React Native や Flutter がスマートフォンで使う Firebase Analytics と同じ役割です。

```asuka check
import platform/analytics

component Shop {
  state note = ""

  task {
    try? analytics.screen("Shop", class: "ShopScreen")
  }

  Column {
    gap: 8
    Button("Buy two pairs") {
      on press {
        do {
          try analytics.log("purchase", ["currency": "USD", "value": 20.02, "transaction_id": "T_1001"],
            items: [analytics.Item(id: "SKU_123", name: "Socks", price: 10.01, quantity: 2)])
        } catch e {
          note = e.message
        }
      }
    }
    Text(note)
  }
}

component ConsentBanner {
  Row {
    gap: 8
    Button("Accept") {
      on press {
        analytics.setConsent(analytics.Consent(analyticsStorage: true, adStorage: true, adUserData: true, adPersonalization: true))
      }
    }
    Button("Decline") {
      on press {
        analytics.setConsent(analytics.Consent(analyticsStorage: false, adStorage: false, adUserData: false, adPersonalization: false))
      }
    }
  }
}

test "buying logs a purchase" {
  let a = analytics.fake()
  let app = mount(Shop())
  app.tap("Buy two pairs")
  let p = a.logged("purchase")
  expect(p.count() == 1 && p[0].items[0]["item_id"] == .string("SKU_123"))
}
```

- `analytics.log(name, params, items:)` はイベントを記録します。パラメータは文字列・数値・真偽値です（真偽値は 1 か 0 として送られ、`none` のパラメータは省かれます）。EC イベントの商品は `items` に入れ、1 つずつ `analytics.Item`（`id`、`name`、`price`、`quantity`、それ以外は `params`）で表します。Firebase と同じく、商品を持てるのは EC イベント（`view_item`、`add_to_cart`、`purchase` など。`analytics.ecommerceEvents`）だけです。Google の推奨イベント（`login`、`sign_up`、`purchase`、`select_content` など）を使うと、Analytics の標準レポートにそのまま反映されます。
- `analytics.screen(name, class:)` は `screen_view` を記録します。アプリの画面は自動では報告されません（iOS と Android では Firebase から見えるのは Asuka の 1 つのビューコントローラやアクティビティだけなので、自動報告はオフにしてあります）。画面が表示されたときに呼び出してください。
- `setUserId(id)`（サインアウトしたら `none`）と `setUserProperty(name, value)` は、それ以降のイベントのユーザーを表します。`setDefaults(params)` は、それ以降のすべてのイベントにそのパラメータを付けます。
- `setEnabled(false)` は収集をオフにし、次回以降の起動にも引き継がれます。`setConsent(Consent(...))` は同意モードの設定です（`true` で許可、`false` で拒否、`none` はそのまま）。アプリが設定するまでは `asuka.toml` の `[analytics]` に従います（後述）。先に同意を求めるアプリは、収集をオフにした状態や、同意を拒否した状態から始められます。
- `await analytics.appInstanceId()` は、Analytics がこのインストールを識別する ID（BigQuery の `user_pseudo_id`）です。収集がオフのときや、アナリティクス ストレージが拒否されているときは `none` です。`reset()` はこの ID とユーザーの ID・プロパティを消し、インストールを新しいものとして扱わせます。

どのプラットフォームでも受け付けるイベントは同じです。パッケージが、プラットフォームに渡す前に自分でチェックするからです。イベント名とパラメータ名は、英字・数字・アンダースコアの 1〜40 文字で、英字で始まり、`firebase_`・`google_`・`ga_` で始まらず、Analytics が自分で記録するイベント（`first_open`、`session_start`、`app_update` など）でもないものです。1 つのイベントのパラメータは最大 25 個（商品は 1 つと数えます）、商品は最大 200 個で、商品 1 つにつきパラメータは最大 27 個です。ユーザープロパティ名は同じ文字の 1〜24 文字で、Analytics が自分で設定するもの（`user_id`、`first_open_time` など）は使えません。規則に反すると `AnalyticsError`（`invalidName`、`reservedName`、`tooManyParameters`、`invalidValue`）が投げられます。256 文字を超えるユーザー ID も同じです。一方、Analytics が保存できる長さを超えるテキストの値は切り詰められます。パラメータの値は 100 文字、ユーザープロパティの値は 36 文字までです。

プラットフォームごとの設定は `asuka.toml` に書きます（[analytics](asuka-toml.md#analytics)）。

```toml
[ios]
googleServices = "GoogleService-Info.plist"   # iOS と Mac
[android]
googleServices = "google-services.json"
[analytics]
measurementId = "G-XXXXXXXXXX"   # Web、Windows、Linux：Web ストリームのもの
apiSecret = "…"                   # Windows と Linux
enabled = false                   # アプリが setEnabled(true) を呼ぶまで
consent = { adStorage = "denied", adUserData = "denied", adPersonalization = "denied" }
```

- **iOS と macOS**：Firebase プロジェクトの、アプリの ID と同じ Apple アプリの `GoogleService-Info.plist` です。ビルドがアプリに含めます。Mac も同じファイルを使います。Firebase はアプリの起動時に始動します。
- **Android**：アプリの ID と同じ Android アプリを含む `google-services.json` です。Firebase Analytics は広告 ID の権限（`AD_ID`）を宣言するので、Play Console の広告 ID の申告でそのように答えてください。
- **Web**：Google から読み込む gtag.js で、`measurementId` の Web ストリームに送ります（Firebase の Web アプリの `measurementId` がそのストリームのものです）。ページには API シークレットを置きません。`reset()` はブラウザのクライアント ID（Analytics の Cookie）を消し、次の訪問は新しいインスタンスになります。
- **Windows と Linux**：Measurement Protocol で同じ Web ストリームに送り、その API シークレットを 1 つ使います（Analytics の「管理 > データストリーム > Measurement Protocol API シークレット」で作成します。`apiSecret` の代わりに `ASUKA_ANALYTICS_API_SECRET` を使えば、リポジトリに入れずに済みます）。イベントは専用のスレッドから、最初のイベントの数秒後に最大 25 個ずつまとめて送られるので、アプリがネットワークを待つことはありません。インストールのクライアント ID は初回起動時に作られ、ユーザーの ID とプロパティ、まだ送っていないイベントとともにアプリのデータディレクトリに保存されます（未送信のイベントは次の起動時に送られ、最大 72 時間保持されます。オフラインのときは後で再試行します）。イベントのない状態が 30 分続くとセッションが終わります。画面はページビューとして、画面名をタイトルに、`app://<アプリの ID>/<画面名>` を URL にして送られます。アナリティクス ストレージが拒否されている間は何も送りません。`asuka.toml` の `analytics.endpoint` を指定すると、GA ではなく独自のコレクター（サーバーサイド タグ設定のサーバーなど）に送ります。

設定のないプラットフォームでは何も送られません。そこでは `available()` が false になり、呼び出しは何もせず、`asuka dev` と `asuka build` が足りないものを知らせます。`asuka dev` では、Web とデスクトップは Analytics の DebugView に送ります。スマートフォンでは Firebase のデバッグモードを有効にします（Android では `adb shell setprop debug.firebase.analytics.app <アプリの ID>`）。テストでは、`analytics.fake()` が送る代わりにアプリの記録した内容を保持します（`events`、`logged(name)`、`userId`、`userProperties`、`consent`）。アプリが何を送るかは、各ストアのプライバシー情報（App Store Connect の「App のプライバシー」、Google Play の「データ セーフティ」）であなたが申告してください。
