# SwiftUI・Expo との比較

各行は、アプリでよく必要になる機能です。SwiftUI と Expo がそれをどう実現しているか、Asuka には何があるかを並べています。行に特に記載がない限り、✓ は Asuka が動くすべてのプラットフォーム（iOS、Android、Mac、Windows、Linux、Web）で使えることを表します。あるプラットフォームに機能がない場合（Mac にはセンサーがなく、ブラウザーにはホーム画面ウィジェットがありません）、そのパッケージは「対応していない」と答えます（`unsupported` や `notAvailable` のエラー、または `available()` が false）。スマートフォンにできてブラウザーにはできないことについて、ブラウザーで各パッケージが代わりに何をするかは [Web](web.md#ネイティブモジュール) を参照してください。

Asuka の列には、その機能を担う要素、プロパティ、パッケージ（`std/...` か `platform/...`、[標準ライブラリとプラットフォーム SDK](stdlib.md)）と、それが各プラットフォームで何にあたるかを書いています。Asuka は各プラットフォーム純正のウィジェットを表示し、各プラットフォーム自身の API を呼び出します。iOS と Mac では Swift、Android では Kotlin、Web では JavaScript、Windows と Linux では C から呼び出します。

SwiftUI は Mac でも動きます。Expo は iOS、Android、Web で動きます。React Native は、Expo の外にある Microsoft の React Native for Windows and macOS によって Windows と Mac にも対応しています。デスクトップのフレームワーク（Electron、Tauri、Wails）と Asuka の比較は[デスクトップのフレームワーク](#デスクトップのフレームワーク)を、デスクトップアプリがウィンドウ以外に必要とするものについては[デスクトップアプリの機能](#デスクトップアプリの機能)を参照してください。UI を自前で描画する MyGo との比較は [Asuka と MyGo](mygo.md) を参照してください。

✓ 対応 · ◐ 一部対応（足りないものを記載） · ✗ 未対応

## レイアウト

| | SwiftUI | Expo (React Native) | Asuka |
|---|---|---|---|
| スタック、間隔、パディング、配置 | `HStack`、`VStack`、`Spacer` | flexbox | ✓ `Row`、`Column`、`View` と flexbox のプロパティ：`grow`、`shrink`、`gap`、`align`、`justify`、`wrap`、`padding`、`margin`（[レイアウト](layout.md)） |
| オーバーレイ、絶対配置 | `ZStack`、`.overlay` | `position: absolute` | ✓ `absolute` と `top`、`left`、`start`、`end`… |
| ビューのグリッド | `Grid`、`LazyVGrid` | コミュニティ製 | ✓ `grid`（CSS グリッド：`Grid(columns:)`、`.fr`、`.repeat`、`.autoFill`。`gridColumn`、`gridRow`） |
| スクロール | `ScrollView` | `ScrollView` | ✓ `Scroll`（縦、`horizontal` で横） |
| 遅延リスト、セルの再利用 | `List`、`LazyVStack` | `FlatList` | ✓ `List`（プラットフォームのもの：`UICollectionView`、`RecyclerView`。Mac、Windows、Linux、Web では、プラットフォームのスクロールビューの中で、表示中の行を Asuka が再利用） |
| 遅延グリッド | `LazyVGrid` | `FlatList numColumns` | ✓ `List` の `columns`、`gap` |
| チャットのリスト：末尾から始め、古いものを上に読み込む | `defaultScrollAnchor(.bottom)`、`scrollPosition` | `FlatList inverted`、`maintainVisibleContentPosition` | ✓ `List` の `anchor: .bottom`、`on startReached`。どのリストでも、表示中の部分より上に行を追加しても表示位置は動かない |
| セクション、固定ヘッダー | `List` の中の `Section` | `SectionList`、`stickySectionHeadersEnabled` | ✓ `List` の `section` と `header`（plain と bare のリストでは固定） |
| 行の選択 | `List(selection:)`、`EditButton` | コミュニティ製 | ✓ `List` の `selecting`、行の `selected`、`on select`、`on selectBegin`（iOS の編集モードと 2 本指のパン、Android のチェックボックスと長押し。デスクトップでは行のハイライト、ブラウザーではチェックマーク） |
| セーフエリア、キーボード | 自動 | `SafeAreaView`、`KeyboardAvoidingView` | ✓ `SafeArea`（キーボードの分もパディングする）、`environment()` の `safeTop`…と `keyboard` |
| ビューのサイズ、ウィンドウのサイズの取得 | `GeometryReader`、サイズクラス | `onLayout`、`useWindowDimensions` | ✓ `on layout(frame)`。`environment()` の `width`、`height`、`widthClass`、`heightClass` |
| アスペクト比、最小・最大サイズ | `.aspectRatio`、`.frame(min:max:)` | スタイル | ✓ `aspectRatio`、`minWidth`、`maxWidth`、`minHeight`、`maxHeight` |

## コントロール

| | SwiftUI | Expo | Asuka |
|---|---|---|---|
| テキスト、スタイル付きテキスト、リンク | `Text`、`AttributedString` | `Text` | ✓ `Text`、`RichText`（スパン、リンク）、`Selectable`。Markdown は `std/markdown`（Asuka が解析するのでどこでも同じ結果になり、プラットフォームのスタイル付きテキストとして描画） |
| ボタン（スタイル、アイコン、メニュー） | `Button`、`Menu` | `Pressable` | ✓ `Button`（`variant`：plain、filled、tinted、gray、bordered、glass、glass prominent。`icon`。`menu`）、`Menu`（タップまたは `longPress`：コンテキストメニュー）。項目は `disabled`、`checked` にでき、`.submenu` やタイトル付きの `.section` に入れられる。メニューを開くバーボタン |
| テキストフィールド | `TextField`、`SecureField`、`TextEditor` | `TextInput` | ✓ `TextField`（`secure`、`keyboard`、`returnKey`、自動入力のための `contentType`、`multiline`、`plain`） |
| カーソル、カーソル位置への挿入 | `TextSelection` | `selection`、`onSelectionChange` | ✓ `bind selection`。`Ref` の `insert(text)` |
| リッチテキストの編集（メールの本文など） | `AttributedString` を使った `TextEditor`（iOS 26） | コミュニティ製（10tap、pell） | ✓ `bind rich`、`Ref` の `toggleFormat`、`format`、`setLink`、`on formats`（`UITextView`、`EditText`、`NSTextView`、リッチエディットコントロール、`GtkTextView`、`contenteditable`） |
| キーボードの上のバー | `.toolbar { ToolbarItemGroup(placement: .keyboard) }` | `InputAccessoryView`（iOS） | ✓ `TextField` の `accessory { … }`（iOS の `inputAccessoryView`。Android ではキーボードに付いて動くバー。ブラウザーではタッチスクリーンのみ） |
| 画像の貼り付け、キーボードの GIF やステッカー | `onPasteCommand`、`PasteButton` | コミュニティ製（`onPaste`） | ✓ `pasteTypes`、`on paste`（貼り付けたもの、またはキーボードが入れたもの） |
| トグル、スライダー | `Toggle`、`Slider` | コミュニティ製 | ✓ `Toggle`、`Slider` |
| セグメンテッドコントロール | `Picker(.segmented)` | コミュニティ製 | ✓ `Segmented` |
| 日付と時刻 | `DatePicker` | コミュニティ製 | ✓ `DatePicker`、`time.Picker`（`mode`：日付、時刻、両方。`inline`） |
| プログレス、スピナー | `ProgressView` | `ActivityIndicator` | ✓ `Progress`、`ProgressRing`（値あり・なし）、`Activity` |
| ホイールとメニューのピッカー、ステッパー | `Picker`、`Stepper` | コミュニティ製 | ✓ `Wheel`（`UIPickerView`、`NumberPicker`。デスクトップと Web ではポップアップメニュー）、`Picker`（iOS と Mac ではポップアップボタン、Android では Material の exposed dropdown menu、Windows では WinUI 風に描画したコンボボックス、`GtkDropDown`、`<select>`）、`Stepper` |
| カラーピッカー | `ColorPicker` | コミュニティ製 | ✓ `ColorPicker`（`UIColorWell`。Android にはないので、不透明度も選べるパレットの Material ダイアログ。`NSColorWell`、Windows の色の設定ダイアログ、`GtkColorDialogButton`、`<input type="color">`） |
| アイコン | SF Symbols | `@expo/vector-icons` | ✓ `Symbol`（iOS と Mac では SF Symbols。同じ名前で、Android と Web では Material Symbols、Windows では Segoe Fluent Icons、Linux ではアイコンテーマのもの、なければ Material Symbols：厳選したセット）。SVG のアイコンは `std/svg` でベクターとして描画 |
| 画像（リモート、キャッシュ、プレースホルダー） | `AsyncImage` | `expo-image` | ✓ `Image`（`placeholder`、`blurHash` と `thumbHash` のプレースホルダー、フェードする `transition`。メモリにキャッシュし、iOS と Android ではディスクにも。各プラットフォームのデコーダー（ImageIO、`ImageDecoder`、`NSImage`、WIC、GDK のローダー、ブラウザー）で読むので、プラットフォームが読める WebP、HEIC、AVIF も表示できる。アニメーション GIF、APNG、WebP はすべてのプラットフォームで再生、`animated`。SVG は Mac、Linux、Web） |
| Lottie アニメーション | コミュニティ製（lottie-ios） | `lottie-react-native` | ✓ `std/lottie` の `lottie.View`（lottie-ios、lottie-android、lottie-web：JSON と dotLottie、`playing`、`loop`、`speed`、読み書きできる `progress`、`segment`、`marker`、キーパスによる `overrides`）。Windows と Linux では Asuka 自身の `Canvas` 上のプレーヤー：シェイプ、グラデーション、トリムパス、リピーター、スター、プリコンポジション、アルファマット。マスク、画像、テキストは非対応 |
| 動画 | `VideoPlayer` | `expo-video` | ✓ `Video`（iOS と Mac では AVKit、Android では Media3、Windows では WebView2 の中の `<video>`、Linux では `GtkVideo`（GStreamer）、Web では `<video>`） |
| Web のコンテンツ | `WKWebView` | `react-native-webview` | ✓ `WebView`（iOS と Mac では `WKWebView`、Android の `WebView`、Windows では WebView2、Linux では WebKitGTK：URL か HTML、双方向のメッセージ、ページより先に実行するスクリプト（`preload`）、リクエストのブロック、ページの状態と履歴、ダウンロード、新しいウィンドウ、個々のリクエスト、スクリプトの値、`webview.cookies(url)`。Web では `<iframe>` で、すべてが使えるのはアプリ自身のサイトのページ）。`platform/js` は画面外で JavaScript を実行する |
| 地図 | `Map` | `expo-maps` | ✓ `platform/maps` の `maps.View`（iOS と Mac では MapKit、Android では Google Maps、Windows、Linux（libshumate）、Web では OpenStreetMap のタイル：表示範囲、マーカー、ユーザーの現在地、タップ） |
| 独自の描画 | `Shape`、`Path`、`Canvas` | `react-native-svg`、Skia | ✓ `Shape` と `Path`（塗り、線、線端、角の形、破線、アニメーションできるトリム）。`Canvas`（パス、グラデーション、テキスト、変形、クリップ：Core Graphics、Android の `Canvas`、GDI+、GSK、2D の `<canvas>`）。`std/svg` |
| グラフ | Swift Charts | `victory-native`、コミュニティ製 | ✓ `std/chart`（棒、折れ線、面、点。`svg.Shape` で描くのでどこでも同じ） |
| シェーダー | `.colorEffect(ShaderLibrary...)` | Skia のランタイムエフェクト | ✓ `shader` 宣言（Asuka で書き、アプリと一緒にコンパイル：iOS と Mac では Metal、Android 13 以降では AGSL、Web では WebGL 2、Windows では Direct3D 11、Linux では OpenGL） |
| ネイティブへの抜け道 | `UIViewRepresentable` | ネイティブモジュール | ✓ ネイティブパッケージ：`native interface` や `native component` を Asuka で宣言し、iOS と Mac では Swift、Android では Kotlin、Web では JavaScript、Windows と Linux では C で実装（[ネイティブコード](native.md)）。C のライブラリは `extern` で直接呼び出せる（[Asuka から C を呼ぶ](c.md)） |
| ページング、ページのドット | `.scrollTargetBehavior(.paging)`、`TabView(.page)` | `react-native-pager-view` | ✓ `Scroll` の `paging`、`PageControl` |
| チェックボックス | `Toggle`（`.checkbox`、macOS） | コミュニティ製 | ✓ `Toggle { checkbox: true }` |

## ナビゲーションと表示

| | SwiftUI | Expo (expo-router) | Asuka |
|---|---|---|---|
| ネイティブのプッシュ、ポップ、戻るジェスチャーのあるスタック | `NavigationStack` | Stack | ✓ `Stack`、`push`、`pop` |
| タブ | `TabView` | Tabs | ✓ `Tabs`、`Tab` |
| サイドバー、ドロワー | `NavigationSplitView` | Drawer | ✓ `Split`、`Destination`、`Sidebar`（iPhone ではスタック、Android ではドロワー。`UISplitViewController`、`NSSplitViewController`、Windows では WinUI の `NavigationView` のペインとして描画、Linux では libadwaita のスプリットビュー） |
| タブとサイドバーの行のバッジ | `badge` | `tabBarBadge`、`drawerItem` の件数 | ✓ `Tab` と `Destination` の `badge`（`badgeValue`、`BadgeDrawable`、ドロワーのカウンター。Mac ではソースリストの件数。Fluent の `InfoBadge`。Linux と Web では件数） |
| バーボタン、タイトル、検索 | `.toolbar`、`.searchable` | ヘッダーのオプション | ✓ 画面の `title`、`largeTitle`、`toolbar { … }`、`bind search` |
| 下部のツールバー、主要なアクション（FAB） | `ToolbarItem(placement: .bottomBar)` | — | ✓ 画面の `bottomToolbar`、`primary`、`primaryExtended`：UIKit のツールバー、Material の `BottomAppBar` と（拡張）FAB、Mac ではウィンドウのツールバー、Windows ではコマンドバー、Linux では `GtkActionBar`、Web ではバーとフローティングボタン |
| 段階的な高さのシート | `.sheet`、`.presentationDetents` | モーダル | ✓ `Sheet`（`detents`、`grabber`） |
| フルスクリーンのモーダル | `.fullScreenCover` | `presentation: "fullScreenModal"` | ✓ `Sheet { fullScreen: true }`（iOS の `.fullScreen`。Material のフルスクリーンダイアログ。デスクトップではウィンドウのコンテンツ全体を覆う。Web ではページを覆う `<dialog>`） |
| 戻る操作の横取り、閉じるのを防ぐ | `.navigationBarBackButtonHidden`、`.interactiveDismissDisabled` | `BackHandler`、`usePreventRemove` | ✓ `onBack { … }`（画面、シート、アプリ全体）、`backGesture: false` |
| ポップオーバー | `.popover` | — | ✓ `Popover`（iPhone でもポップオーバー） |
| アラート、アクションシート | `.alert`、`.confirmationDialog` | `Alert` | ✓ `Alert`（`actionSheet`。中にテキストフィールドを置け、パスワードには `secure`、ボタンは `disabled` で入力に追従） |
| スナックバー、トースト（「元に戻す」付きの短いメッセージ） | — | コミュニティ製（react-native-paper の `Snackbar`） | ✓ `snackbar(message, …)`（Android では Material のもの、Linux では `AdwToast`。iOS、macOS、Windows、Web ではプラットフォーム風に描画） |
| ディープリンク、ユニバーサルリンク、App Links | `.onOpenURL` | Linking | ✓ `Router` はルートのリンク（スキームのリンク、ユニバーサルリンクと App Links、通知の URL）へ自分で移動する。それ以外は `on openURL(url)`。`asuka.toml` の `scheme` と `links` |
| URL のルート：パスのパターン、入れ子のナビゲーター、リンクの下の初期ルート、リダイレクト、ガード、見つからない場合 | `Codable` の値を持つ `NavigationPath` | expo-router（`[id]`、`[...rest]`、`unstable_settings`、`<Redirect>`、`Stack.Protected`、`+not-found`） | ✓ `Router` と `Route`（`:id`、`*rest`、`initial`、`redirect`、`guard`。`layout`：スタック、タブ、スプリットビュー、スロット。`sheet: true`）、`navigate`、`replace`、`router.back()`、`router.setQuery`、ボタンやビューの `href`。状態はアドレスから復元される（再読み込み、デスクトップのウィンドウの前回の状態、`asuka dev` のリロード）（[ルート](navigation.md#ルート)） |
| 型付きのリンク | `NavigationPath` の値 | 型付きルート（生成される `.d.ts`） | ✓ ルートを持つ画面の呼び出しがそのままアドレスになり、コンパイル時に検査される：`navigate(Thread(id: 7))`、`href: Thread(id: 7)` |
| 引っ張って更新、スワイプアクション | `.refreshable`、`.swipeActions` | `RefreshControl` | ✓ `on refresh`、`refreshing`。両端：`swipeActions`、`leadingSwipeActions` |

## モーション

| | SwiftUI | Expo (Reanimated) | Asuka |
|---|---|---|---|
| 変更のアニメーション | `withAnimation` | `withTiming`、`withSpring` | ✓ `animate { … }`、`animate(.spring) { … }`（`Animation`：長さ、イーズ、リニア、スプリング、遅延、繰り返し、往復）（[モーション](motion.md)） |
| 変形 | `.rotationEffect`、`.scaleEffect`、`.offset` | `transform` | ✓ `transform: Transform(…)` |
| 出現と退場のトランジション | `.transition` | entering/exiting | ✓ `transition`（`.opacity`、`.pop`、`.slide`、独自の `Transition(…)`） |
| フレームごとのアニメーション | `TimelineView(.animation)` | `useFrameCallback` | ✓ `frames()`、`onFrame` |
| 影、グラデーション、ぼかし | `.shadow`、`LinearGradient`、マテリアル | スタイル、expo-linear-gradient、expo-blur | ✓ `shadow`、`gradient`、`material` |
| ジェスチャー | タップ、長押し、ドラッグ、拡大縮小、回転 | Gesture Handler | ✓ `on press`、`on longPress`、`on pan`、`on pinch`、`on rotate`、`on swipe` |
| ハードウェアキーボードのキー | `.onKeyPress`、`.keyboardShortcut` | `onKeyPress`（`TextInput` のもの。React Native for Windows and macOS では `onKeyDown`） | ✓ `keys`（フォーカスのあるビューから外側へ、コントロールやメニューより先に）、`useKeys`、`WindowKeys`（ウィンドウのもの） |
| 押下とホバーの状態 | `ButtonStyle` の `isPressed`、`.onHover` | `Pressable` の `pressed`、`onHoverIn`/`onHoverOut` | ✓ `pressed { … }` と `hovered { … }` のバリアント。グループや隣のビューの状態は `Ref` の `pressed` と `hovered` で。`on pressedChange`、`on hoverChange` |
| ドラッグ＆ドロップ | `.draggable`、`.dropDestination` | （組み込みなし） | ✓ `dragItems`、`dropTypes`、`on drop`（アプリ内ではアプリ自身の値、ほかのアプリとはテキスト、リンク、ファイル。`dragging { … }` と `dragOver { … }` のバリアント） |
| 共有要素のトランジション | `matchedGeometryEffect`、`.navigationTransition(.zoom)` | shared transitions | ✓ `shared`（ほかのビューの位置とサイズからビューが広がる）、画面の `zoomFrom`（iOS 18 のズーム、Material のコンテナトランスフォーム） |
| 触覚フィードバック | `sensoryFeedback` | `expo-haptics` | ✓ `device.haptic(…)`（Mac では感圧タッチトラックパッド、ブラウザーでは `navigator.vibrate`） |
| 視差効果を減らす | `accessibilityReduceMotion` | `useReducedMotion` | ✓ `environment().reduceMotion`（iOS ではトランジションがディゾルブになり、デスクトップと Web では `animate` の変更がすぐに適用される） |

## データとサービス

| | SwiftUI / Apple | Expo | Asuka |
|---|---|---|---|
| 状態 | `@State`、`@Observable` | フック | ✓ `state`、ストア、派生した `let`（細粒度のシグナル：書き込むと、それを読んでいるところだけが更新される）（[コンセプト](concepts.md)） |
| 永続化する小さな値 | `@AppStorage` | AsyncStorage | ✓ `std/storage` の `storage.Value`、`std/store`（UserDefaults、SharedPreferences、Windows と Linux ではファイル、`localStorage`） |
| SQLite | SwiftData / GRDB | `expo-sqlite` | ✓ `std/sqlite`（`migrate`、行を `T` に直接デコードする `query<T>`、`exec`、トランザクション。メインスレッドの外で実行。`watch<T>`：読んでいるテーブルに書き込みがあったときや引数が変わったときに再実行されるライブクエリ。どのプラットフォームでも同じ SQLite で、ブラウザーではページのワーカーで動く）（[データ](data.md)） |
| セキュアストレージ | キーチェーン | `expo-secure-store` | ✓ `platform/secure`（キーチェーン、Android Keystore、資格情報マネージャー、libsecret。Web では、ブラウザーが外に出さない鍵で暗号化して IndexedDB に保存するもので、キーチェーンではない） |
| ファイル、ネットワーク | Foundation | `expo-file-system`、fetch | ✓ `std/fs`。`std/http`（プラットフォーム自身のクライアント：URLSession、`HttpURLConnection`、WinHTTP、libsoup、`fetch`）、`http.download`、`http.stream` とサーバー送信イベント（`http.events`）。`std/websocket` |
| 写真 | `PhotosPicker` | `expo-image-picker` | ✓ `device.pickPhotos(max:)` |
| 書類 | `UIDocumentPickerViewController` | `expo-document-picker` | ✓ `device.pickFiles(types:, multiple:)`。デスクトップでは `platform/dialog` |
| カメラ | AVFoundation | `expo-camera` | ✓ `platform/camera`：`camera.take()`（システムのカメラ。Mac、Windows、Linux ではライブプレビュー付きのダイアログ）、`camera.View`（前面または背面のライブプレビュー）、`capture()`、`record()`（AVFoundation、CameraX、Media Foundation、GStreamer、`getUserMedia`）。`platform/record` で音声付きの動画を録画 |
| バーコード、QR コード | AVFoundation、Vision | `expo-camera` のスキャン | ✓ `camera.View` でライブに（`scan`、`on code`）、画像ファイルからも（`scan.file`）：Vision、Android では ML Kit（インポートしたアプリにだけ含まれる）、ブラウザーでは `BarcodeDetector` があればそれ、Windows と Linux では C で書いた Asuka 独自のリーダー（QR、Data Matrix、EAN、UPC、Code 128、Code 39） |
| 位置情報 | Core Location | `expo-location` | ✓ `platform/location`（Core Location、Android の位置情報マネージャー、WinRT の `Geolocator`、GeoClue または位置情報ポータル、Geolocation API。使用中と常に、正確とおおよそ。地名。バックグラウンドでの更新と領域の監視で、iOS と Android ではそのイベントのためにアプリが起動され、処理中は保持される） |
| 翻訳、複数形、地域のフォーマット | String Catalogs、FormatStyle | `expo-localization`、i18next | ✓ カタログと `tr.key(…)`、`std/i18n`（[言語](i18n.md)） |
| 右から左のレイアウト | `layoutDirection` | `I18nManager` | ✓ 言語に従う（`start` と `end` は反転する）。どの言語でも同じ向きで読むものには `layoutDirection` |
| ローカル通知 | UserNotifications | `expo-notifications` | ✓ `platform/notify`（すぐに、または予約して表示、画像付き。iOS と macOS では UserNotifications、Android の通知、ボタンと返信付きの Windows のトースト、Linux のデスクトップ通知。Web ではアプリの Service Worker を通じて表示し、予約した通知はアプリのページが開いていればその時刻に、そうでなければ次にページを開いたときに表示する。ブラウザーには通知を予約する手段がない） |
| プッシュ通知 | APNs | `expo-notifications` | ✓ `platform/push`（iOS と macOS では APNs、Android では Firebase Cloud Messaging、Web では VAPID による Web Push：`register()` でトークン、`received()`、`opened()`） |
| 通知のアクション、インライン返信 | `UNNotificationCategory`、`UNTextInputNotificationAction` | `setNotificationCategoryAsync` | ✓ `notify.setCategories`、`notify.responses()`（アクションと返信。`r.defer()` で処理が終わるまでアプリを動かし続ける。Web では Service Worker の通知アクション、Chrome にあればインライン返信も） |
| スレッド、表示中の通知、バッジ | `threadIdentifier`、`getDeliveredNotifications`、`setBadgeCount` | `getPresentedNotificationsAsync`、`setBadgeCountAsync` | ✓ 通知の `thread`（Android では概要通知を保ったグループ）、`notify.showing()`、`remove`、`removeThread`、`setBadge` |
| フォアグラウンドでの通知 | `willPresent` | `setNotificationHandler` | ✓ `notify.present(decide)` |
| Android の通知チャンネル | — | `setNotificationChannelAsync` | ✓ `notify.setChannels`、`setChannelGroups`。プッシュのチャンネル（iOS にはない） |
| 会話のメッセージ | 通信通知（`INSendMessageIntent`） | コミュニティ製 | ✓ `Notification(…).from(person, conversation)`、`Message.from`：iOS の通信通知、Android の MessagingStyle と会話のショートカット。ほかでは通常の通知 |
| 表示前のプッシュの書き換え | Notification Service Extension | コミュニティ製（`expo-notifications` にはない） | ✓ `push.rewrite(f)`：iOS の Notification Service Extension の中で（`asuka build` がビルドして署名）、Android ではデータメッセージを受ける Firebase のサービスの中でアプリのコードが動く |
| クリップボード、共有シート、ブラウザー | `UIPasteboard`、`ShareLink`、`SFSafariViewController` | `expo-clipboard`、`expo-sharing`、`expo-web-browser` | ✓ `device.copy(text)`、`device.clipboardText()`、`clipboardHas`、`clipboardImage`、`copyImage`、`clipboardChanges()`。`device.share(text)`、`device.shareFiles(paths)`（Mac の共有メニュー、Windows の共有 UI、ブラウザーのもの。Linux にはないので、クリップボードにコピーしてトーストで知らせる）。`device.browse(url)`（`SFSafariViewController`、Custom Tab）、`device.openURL(url)` |
| ファイルのプレビュー | `QLPreviewController`、`.quickLookPreview` | `react-native-file-viewer` | ✓ `platform/preview`（iOS と Mac ではクイックルックで、複数のファイル、マークアップ。Android、Windows、Linux ではそのファイルのアプリ。Web では新しいタブ） |
| 印刷、HTML からの PDF | `UIPrintInteractionController`、`UIPrintPageRenderer` | `expo-print` | ✓ `platform/print`（ファイル、HTML、`WebView` を、システムのダイアログで印刷し、結果を返す。`print.pdf`：ページ分割、地域の用紙サイズ、余白） |
| メールとメッセージの作成画面 | `MFMailComposeViewController`、`MFMessageComposeViewController` | `expo-mail-composer`、`expo-sms` | ✓ `platform/compose`（`compose.mail`：宛先、件名、HTML の本文、添付ファイル。iOS ではユーザーが何をしたかがわかり、ほかではメールアプリ。`compose.message`：iOS のメッセージ、Android の SMS アプリ） |
| スリープの防止、デバイスとインストールの情報 | `isIdleTimerDisabled`、`UIDevice` | `expo-keep-awake`、`expo-device`、`expo-application` | ✓ `device.keepAwake(on)`、`device.info()`（アプリの入手元とインストール日時を含む） |
| ネットワークの状態 | `NWPathMonitor` | `expo-network` | ✓ `platform/network`（`network.status()`：オンラインかどうか、Wi-Fi かモバイル通信か、従量制かどうか、省データモード。NWPathMonitor、ConnectivityManager、Network List Manager、NetworkManager、Network Information API） |
| ギャラリーやダウンロードへの保存 | `PHPhotoLibrary` | `expo-media-library` | ✓ `platform/library`（`library.save`：写真、Android のギャラリーとダウンロード、ミュージック。デスクトップではダウンロード、ピクチャ、ムービー、ミュージックのフォルダー） |
| バックグラウンドの処理 | `beginBackgroundTask`、`BGTaskScheduler` | `expo-task-manager`、`expo-background-task` | ✓ `platform/background`：Android はアプリを動かし続け、進捗とボタンを通知に出す。iOS はおよそ 30 秒。デスクトップはアプリが休止されないようにする（App Nap、スリープ）。`platform/periodic`：システムがときどき実行する処理で、画面なしでアプリを起動する（BGTaskScheduler、WorkManager。デスクトップではアプリの実行中のタイマー） |
| ほかのアプリからの共有の受け取り | 共有拡張 | `expo-sharing`（`useIncomingShare`） | ✓ `on share(items)`：テキスト、リンク、画像、動画、音声、ファイルを一つでも複数でも、名前、MIME タイプ、サイズとともに（`asuka.toml` の `shareTarget` から、Android のインテントフィルターと、iOS の共有拡張とその起動条件を作る）。`platform/sharesheet` は共有元のアプリの上に出すアプリ独自のシートで（iOS の共有拡張、Android のアクティビティ）、データはアプリと共有する。iOS と Android |
| オーディオ | AVAudioPlayer、AVAudioRecorder | `expo-audio` | ✓ `platform/audio`（`audio.play`、`audio.Player`：再生、一時停止、シーク、音量、速度、ループ。AVPlayer、Media3 の ExoPlayer、MCI、GStreamer、`<audio>`）。`platform/record`（`record.audio`：音質、一時停止、レベル） |
| 再生中の情報、ロック画面のコントロール、メディアキー | `MPNowPlayingInfoCenter`、`MPRemoteCommandCenter` | `expo-audio` のロック画面のコントロール、コミュニティ製（`react-native-track-player`） | ✓ `platform/nowplaying` の `nowplaying.NowPlaying`（タイトル、アーティスト、アートワーク、長さ、システムが進める再生位置、再生、一時停止、次へ、前へ、シーク。`audio.Player` には自動で追従。Android では MediaSession、Windows では SystemMediaTransportControls、Linux では MPRIS、Web では Media Session API） |
| Apple でサインイン | `SignInWithAppleButton` | `expo-apple-authentication` | ✓ `platform/appleauth`（Apple の `SignInButton`、`signIn`、`credentialState`。iOS と macOS。ほかでは `platform/webauth` で） |
| Web のサインイン（OAuth） | `ASWebAuthenticationSession` | `expo-auth-session`、`expo-web-browser` | ✓ `platform/webauth`（`ASWebAuthenticationSession`、Custom Tab、WebView2 のウィンドウ、WebKitGTK のダイアログ、ポップアップ） |
| iCloud：キー値ストア、iCloud Drive | `NSUbiquitousKeyValueStore`、ユビキティコンテナ、`NSMetadataQuery` | コミュニティ製（`react-native-cloud-storage`、`expo-icloud-storage`） | ✓ `platform/icloud`（アカウント。iCloud のキー値ストアに保存する `icloud.Value`。iCloud Drive にあるアプリの書類を、変化に合わせてダウンロード・アップロードの状態とともに一覧し、協調して読み書きし、ダウンロード、ローカルコピーの削除、競合の解決。テストには `icloud.fake()`。iOS と macOS。CloudKit のデータベースはない） |
| Apple Pay、Google Pay | PassKit（`PayWithApplePayButton`） | `@stripe/stripe-react-native` の Platform Pay、`react-native-payments` | ✓ `platform/pay`（ウォレットのシートと `PayButton`。アプリの決済代行会社に渡すトークン。配送、クーポン、カードの変更に合わせた合計金額の更新。項目ごとのエラー。iOS、macOS、Safari では Apple Pay、Android とほかのブラウザーでは Google Pay） |
| Face ID、指紋 | LocalAuthentication | `expo-local-authentication` | ✓ `platform/biometric`（LocalAuthentication、BiometricPrompt、Windows Hello。Web では WebAuthn によるプラットフォームの認証器。Linux にはない） |
| アプリ内課金、サブスクリプション | StoreKit 2 | `expo-iap`、`react-native-iap`、RevenueCat | ✓ `platform/purchase`（ローカライズされた価格とオファー付きの商品、購入、保留中の支払い、アプリの起動時からのトランザクションのストリーム、完了処理、エンタイトルメント、復元、サブスクリプションの状態、管理・返金・オファーコードのシート。iOS と macOS では StoreKit 2、Play Billing、Microsoft Store、Trusted Web Activity の Digital Goods API。テストには `purchase.fake()`）（[アプリ内課金](purchases.md)） |
| ストアのレビュー | `requestReview` | `expo-store-review` | ✓ `platform/review`（App Store のプロンプト、Play のアプリ内レビュー） |
| トラッキングの許可 | App Tracking Transparency | `expo-tracking-transparency` | ✓ `platform/tracking` |
| アプリの完全性 | App Attest | `expo-app-integrity` | ✓ `platform/integrity`（App Attest、Play Integrity） |
| 年齢層 | Declared Age Range | `expo-age-range` | ✓ `platform/agerange`（Declared Age Range、Play Age Signals） |
| センサー | Core Motion | `expo-sensors` | ✓ `platform/sensors`（加速度計、ジャイロスコープ、磁力計、デバイスモーション、気圧計、照度、歩数計。どこでも同じ軸と単位。Core Motion、SensorManager、Linux では iio-sensor-proxy の照度と向き、Generic Sensor API。Mac と Windows にはない） |
| NFC タグ | Core NFC | `react-native-nfc-manager` | ✓ `platform/nfc`（NDEF の読み書き、読み取り専用のタグ、タグの ID、生のコマンド。iOS のシート、Android のリーダーモード、アプリを開くタグ。Android の Chrome の Web NFC。デスクトップにはない）（[NFC タグ](nfc.md)） |
| Bluetooth LE | Core Bluetooth | コミュニティ製（`react-native-ble-plx`） | ✓ `platform/bluetooth`（スキャン、接続、サービス、読み取り、書き込み、通知。アドバタイズと GATT サーバー。バックグラウンドのスキャンと状態の復元。Core Bluetooth、Android の BLE、WinRT のもの、BlueZ。Chromium の Web Bluetooth のデバイス選択画面で、ペリフェラルの役割はない）（[Bluetooth](bluetooth.md)） |
| 画像の編集 | Core Graphics、ImageIO | `expo-image-manipulator` | ✓ `platform/imageedit`（リサイズ、切り抜き、回転、反転、塗りつぶし、モザイク。HEIC を読み、JPEG か PNG で書き出し、向きを正す。Image I/O、Android の `Bitmap`、WIC、GdkPixbuf、canvas） |
| 動画のサムネイル | `AVAssetImageGenerator` | `expo-video-thumbnails` | ✓ `imageedit.videoFrame` |
| ビューを画像にする | `ImageRenderer`、`drawHierarchy` | `react-native-view-shot` | ✓ `imageedit.capture(view: ref)`：PNG か JPEG。プラットフォームが描く（レイヤー、Android の `View`、Windows と Linux ではウィンドウのピクセル、Web では要素の SVG のコピー） |
| 画像の中のテキスト、顔、コード | Vision | なし | ✓ `platform/vision`、それぞれの位置とともに（Vision。Android では ML Kit で、インポートしたアプリにだけ含まれる。Windows の OCR と顔検出。Linux では Tesseract のテキストのみで顔はない。ブラウザーでは Shape Detection API があればそれ） |
| 音声のテキスト化 | Speech（`SFSpeechRecognizer`） | コミュニティ製（`expo-speech-recognition`） | ◐ `platform/transcribe`（録音をデバイス上だけで：iOS と macOS では Speech、Android 13 のデバイス上の認識、ブラウザーのデバイス上の認識（Chrome 139 以降）。Windows のデスクトップ音声認識（SAPI）。`transcribe.listen`：マイクからの話すそばからの音声入力。Linux には認識エンジンがないため非対応） |
| 写真ライブラリ | PhotoKit | `expo-media-library` | ✓ `platform/photos`：許可、一覧、サムネイル、元のファイル（PhotoKit、MediaStore） |
| 読み上げ | `AVSpeechSynthesizer` | `expo-speech` | ✓ `platform/speech`（キュー、声、ピッチ、速度、一時停止。AVSpeechSynthesizer、TextToSpeech、SAPI、Speech Dispatcher、`speechSynthesis`） |
| 連絡先 | Contacts、ContactsUI | `expo-contacts` | ✓ `platform/contacts`（`pick()` と新規連絡先の画面は許可なしで、アドレス帳は許可を得て） |
| カレンダー | EventKit、EventKitUI | `expo-calendar` | ✓ `platform/calendar`（イベントの編集画面は許可なしで、カレンダーとイベントは許可を得て。EventKit、Calendar Provider） |
| ホーム画面ウィジェット | WidgetKit | `expo-widgets` | ✓ `platform/widget` と `asuka.toml` の `widgets`（すべてのサイズに一つのテンプレート、iOS ではロック画面のものも。またはアプリ独自のレイアウト：列、行、重ね、テキスト、シンボル、画像、プログレス、ゲージ、タイマー、リンク。タイムライン、カウントダウン、URL を開くタップ。iOS と Android） |
| ライブアクティビティ | ActivityKit | `expo-live-activity` | ✓ `platform/liveactivity`（iOS：ロック画面と Dynamic Island。Android 16 の Live Update。カウントダウン、進捗、アラート、サーバーからの更新のためのプッシュトークン） |
| App Intents、ショートカット | App Intents、App Shortcuts | — | ✓ `asuka.toml` の `shortcuts`（iOS：Siri のフレーズ付きの App Shortcuts で、Spotlight とショートカットに出る。パラメーターはテキスト、数値、スイッチ、選択肢、アプリ自身のデータ（`platform/shortcuts` のエンティティ）で、選択肢とエンティティはフレーズに入れられる。Android：ランチャーのショートカットで、データ一つにつき一つ） |
| カスタムフォント | `.font(.custom)` | `expo-font` | ✓ `assets/fonts`、`Font(family:)` |
| ライフサイクル | `scenePhase` | AppState | ✓ `onLifecycle` |
| 起動画面 | `UILaunchScreen` | `expo-splash-screen` | ✓ `asuka.toml` の `splash`。`platform/splash` で読み込み中は表示を保つ |
| アプリアイコンの切り替え | `setAlternateIconName` | コミュニティ（`expo-dynamic-app-icon`） | ✓ `asuka.toml` の `alternateIcons`、`platform/appicon` の `set`（iOS のホーム画面。Android のランチャーはアプリが画面から離れたときに切り替わる。Mac の Dock、Windows のウィンドウとタスクバー、Web ページのアイコン。Linux はランチャーが desktop ファイルのアイコンを表示するので不可） |
| ステータスバー、ナビゲーションバー | `statusBarHidden`、`preferredStatusBarStyle` | `expo-status-bar`、`expo-navigation-bar` | ✓ `screen.setBars`（`platform/screen`） |
| 画面の向きの固定 | `supportedInterfaceOrientations` | `expo-screen-orientation` | ✓ `screen.lock` |
| スクリーンショット | `isCaptured`、`userDidTakeScreenshotNotification` | `expo-screen-capture` | ✓ `screen.preventCapture`（Android：キャプチャーは黒くなる。iOS：録画中は覆われる。macOS と Windows：ウィンドウはキャプチャーから除かれる）、`screen.screenshots()` |
| 画面の明るさ | `UIScreen.brightness` | `expo-brightness` | ✓ `screen.setBrightness`、`screen.brightness()`（iOS と Android：アプリが前面にあるあいだはアプリの明るさ、離れるとユーザーの明るさに戻る。デスクトップと Web ではユーザーに任せる） |
| クラッシュレポート | MetricKit、Crashlytics、Sentry | Sentry、Crashlytics（`@react-native-firebase/crashlytics`） | ✓ `platform/crash`：Asuka のスタック付きの Asuka の致命的なエラー、捕捉されなかった例外、MetricKit のクラッシュとハング、Android のネイティブクラッシュと ANR、デスクトップのクラッシュを、パンくずと値とともに。送り先はアプリが決める（組み込みのサービスはない）。シンボルは `asuka build` が出力 |

## 外観とアクセシビリティ

| | SwiftUI | Expo | Asuka |
|---|---|---|---|
| ダークモード、セマンティックカラー、アクセントカラー | 自動 | `useColorScheme` | ✓ セマンティックカラー、`environment().dark`、`asuka.toml` の `accent`（Android ではこれをもとに Material 3 の配色を作り、`dynamicColor` なら壁紙から） |
| Dynamic Type、フォントサイズ | 自動 | `allowFontScaling` | ✓ テキストスタイルもカスタムフォントも拡大縮小される。`environment().fontScale` |
| スクリーンリーダーのラベル、ロール、非表示 | `.accessibilityLabel`... | `accessibilityLabel`... | ✓ `label`、`hint`、`role`、`a11yHidden`。押せるビューはボタンになる（[アクセシビリティ](accessibility.md)） |
| 読み上げ | `AccessibilityNotification.Announcement` | `announceForAccessibility` | ✓ `accessibility.announce(text)` |
| スクリーンリーダーのフォーカスの移動 | `@AccessibilityFocusState` | `setAccessibilityFocus` | ✓ `accessibility.focus(ref)` |
| スクリーンリーダーが有効かどうか | `voiceOverEnabled` | `isScreenReaderEnabled` | ✓ `environment().screenReader` |
| カスタムアクション、調整可能なビュー | `.accessibilityAction`、`.accessibilityAdjustableAction` | `accessibilityActions` | ✓ `actions`、`on adjust`（リストのスワイプアクションも TalkBack と VoiceOver のアクションになる） |

## ツール

| | Xcode / Expo | Asuka |
|---|---|---|
| ライブリロード、状態の保持 | プレビュー、Fast Refresh | ✓ `asuka dev`：開発用アプリが、保存するたびに数ミリ秒で新しいものに入れ替え、すべての `state` を保つ。スマートフォン、デスクトップ、ブラウザーのどれでも（[開発](development.md)） |
| ビューの調査 | Xcode のビューデバッガー、React Native DevTools、Flutter DevTools | ✓ `asuka dev` のインスペクター：実行中のアプリのビューをブラウザーにツリーで表示し（要素、テキスト、frame）、ポインターを合わせたビューをデバイス上でハイライトする（[開発](development.md#インスペクター)） |
| 開発環境の確認 | Expo の doctor、`flutter doctor` | ✓ `asuka doctor`：このコンピューターが各プラットフォーム向けに何をビルドできるか、残りに何をインストールすればよいか |
| スマートフォンでの実行 | Xcode の Run、Expo の開発ビルド | ✓ `asuka dev --platform ios` または `android`：シミュレーター、エミュレーター、Android のスマートフォン（`adb reverse`）、USB か Wi-Fi でつないだ iPhone（`--device iphone`、自分のチームで署名） |
| テスト | XCTest、Jest と React Native Testing Library | ✓ `asuka test`：`test` ブロック。アプリをデバイスなしでマウントし（`mount`、`app.tap`、`app.expect`）、デバイスのパッケージにはフェイクがある（[テスト](testing.md)） |
| リリースビルドと署名 | Xcode、EAS Build | ✓ `asuka build`：アプリを事前にコンパイルする。`.ipa`、`.aab`、署名と公証を済ませた `.app`（または Mac App Store 用の `.pkg`）、署名済みの `.exe` を含む Windows のフォルダー、`.tar.gz` と Flatpak のマニフェストを含む Linux のインストールプレフィックス、静的サイト（[配布](shipping.md)） |
| ストアへのアップロード | Xcode、EAS Submit | ✓ `asuka publish`：TestFlight、App Review、Google Play のトラック。Mac App Store と Mac の TestFlight。デスクトップのビルドと署名付きのアップデート |
| GUI のテスト | XCUITest、Detox、Maestro | ✓ `asuka test --gui`：アプリのフロー（`flows/*.flow`：`tap Add`、`type hello`、`see Count: 1`、`back`）を、シミュレーター、エミュレーター、スマートフォン、ヘッドレス Chrome、このコンピューターで実行する（[GUI テスト](testing.md#gui-テストフロー)）。デスクトップアプリの操作手順とスナップショットも（`ASUKA_SCRIPT`、`ASUKA_SNAPSHOT`） |
| ベンチマーク | Instruments | ✓ `bench/`（SwiftUI、Compose との比較） |
| 既存のアプリへの組み込み | ブラウンフィールド（React Native のもの） | ✓ `asuka build --platform ios --xcframework`、`asuka build --platform android --aar` |
| OTA アップデート | EAS Update | ✗（リリースビルドにはアプリがコンパイルされて組み込まれています） |

## デスクトップのフレームワーク

Mac、Windows、Linux では、HTML を Web ビューで包んだアプリもよく作られます。そうしたフレームワークと Asuka を比べると次のようになります。

| | Electron | Tauri | Wails | React Native for Windows and macOS | Asuka |
|---|---|---|---|---|---|
| プラットフォーム | Mac、Windows、Linux | Mac、Windows、Linux。iOS と Android（Tauri 2） | Mac、Windows、Linux | Windows、Mac（React Native の iOS、Android と合わせて） | Mac、Windows、Linux、iOS、Android、Web |
| UI を表示するもの | Chromium の中の HTML と CSS | システムの Web ビュー（WKWebView、WebView2、WebKitGTK）の中の HTML と CSS | システムの Web ビューの中の HTML と CSS | プラットフォームのネイティブのビュー | 各プラットフォーム純正のウィジェット：AppKit、Windows 11 風に描画した Win32 とコモンコントロール、GTK 4 と libadwaita |
| アプリの言語 | JavaScript（ページとは別に Node.js） | Rust と、ページ内の JavaScript | Go と、ページ内の JavaScript | JavaScript | Asuka（ネイティブパッケージでは Swift、C、JavaScript も） |
| アプリと一緒に配布するもの | Chromium と Node.js | アプリのバイナリ（Web ビューはシステムのもの） | アプリのバイナリ（同上） | JavaScript エンジンと React Native のランタイム | 実行ファイル一つ（事前にコンパイルしたアプリ）。Linux では GTK 4 と libadwaita はシステムのもの。アプリが `WebView` を表示しない限り Web ビューは含まない |

Web ビューのアプリは、HTML と CSS で書いたとおりに、どのシステムでも同じ見た目になり、Web のライブラリを使えます。Asuka のアプリは各プラットフォームのアプリと同じ見た目になり（スタイルを設定しないアプリは、Mac では Mac のアプリ、Linux では GNOME のアプリ、Windows では Windows 11 のアプリになります）、そのプラットフォームのアクセシビリティ、テキスト入力、メニューをそのまま使えます。しかも、レイアウトエンジンは一つだけで、すべてのプラットフォームに共通です。`asuka build` はどのシステムからでも（Zig を使い、GTK のインストールなしで）Windows と Linux のビルドを作れます。Mac のビルドには Xcode のある Mac が必要です。

## デスクトップアプリの機能

デスクトップアプリがウィンドウとメニューバー以外に必要とする機能を、Electron、MyGo と比べます（[デスクトップ](desktop.md)）。行に特に記載がない限り、✓ は Mac、Windows、Linux で使えることを表します。

| | Electron | MyGo | Asuka |
|---|---|---|---|
| ウィンドウの制御 | `BrowserWindow` | `Window` | ✓ `app` ブロックの `Window` とほかの `Window`（これらだけを持つアプリ、トレイのアプリにはメインウィンドウがない）、`window.main()`、`window.current()`、ウィンドウの `Ref`（`Window as w`）：表示、非表示、フォーカス、最小化、最大化、フルスクリーン、サイズ、位置、常に手前に表示、状態の追従。`on closeRequest`、`WindowCloseGuard`。`stateKey` による前回の位置の復元。`child` と `modal` のウィンドウ。`titleBar: .hidden`（Wayland では位置の指定、中央への配置、常に手前に表示はできず、コンポジターが決める） |
| トレイアイコン | `Tray` | トレイアイコン | ✓ `platform/tray` の `tray.Tray`（`NSStatusItem`、通知領域、Linux では StatusNotifierItem）：画像またはシンボル、タイトル、ツールチップ、メニュー、クリック |
| サブメニュー、終了の項目 | `MenuItem.submenu`、ロール | ロール、サブメニュー | ✓ `MenuBar`、`Menu`、`MenuItem`（項目の中の項目はサブメニュー。`role: .quit`、`.close`、`.openRecent`）。トレイと Dock のメニューでは `Action` の `.submenu` と `.section` |
| Dock のメニュー、ジャンプリストのタスク | `app.dock.setMenu`、`setUserTasks` | Dock のメニュー、ジャンプリスト | ✓ `tray.DockMenu`（Dock のメニュー、ジャンプリストのタスク、Linux のランチャーのクイックリスト） |
| バックグラウンドのアプリ | `app.dock.hide`、`window-all-closed` で終了しない | バックグラウンドのアプリ | ✓ `asuka.toml` の `keepRunning` と `background` |
| フライアウト：トレイアイコンから垂れ下がるウィンドウ | アプリが `tray.getBounds()` の位置に置くフレームなしの `BrowserWindow` | フライアウト | ✓ `Window` の `flyout`。`tray.anchor(id)` か、ウィンドウ内の矩形から垂れ下がる：ディスプレイに収まるように配置・反転し、ほかの場所のクリックか Escape で閉じ、上にモーダルダイアログも出せる（Linux では X11 の場合のみ配置） |
| ランチャー用のパネル | `type: 'panel'`（macOS） | パネル | ✓ `Window` の `panel`：すべてのアプリと操作スペースの上に浮かび、Mac ではアプリをアクティブにせずにキーボード入力を受け、Escape かほかの場所のクリックで閉じる（Linux の Wayland では、gtk4-layer-shell があれば layer-shell のサーフェス） |
| コンテンツに合わせたウィンドウのサイズ | アプリが `setContentSize` を呼ぶ | `FitContent` | ✓ `fitContent`：コンテンツが変わると、Asuka のレイアウトがウィンドウのサイズを決める |
| クリックを透過するウィンドウ | `setIgnoreMouseEvents` | `SetIgnoreMouseEvents`、`SetInputRegion` | ✓ `clickThrough`、`w.setClickThrough(on)`、そしてマウスを受け取り続けるビューの `takesClicks` |
| ランチャー向けのインストール済みアプリ | なし | `InstalledApps`、`FileIcon`、`LaunchApp` | ✓ `platform/apps` の `list`、`search`、`icon`、`open` |
| グローバルショートカット | `globalShortcut` | グローバルショートカット | ✓ `platform/hotkey` の `hotkey.Hotkey`（Carbon のホットキー、`RegisterHotKey`、X11 のグラブまたは Wayland の GlobalShortcuts ポータル） |
| 開く、保存、フォルダーのダイアログ | `dialog.showOpenDialog`、`showSaveDialog` | ダイアログ | ✓ `platform/dialog` の `open`、`save`、`chooseFolder`、`chooseFolders`（Mac ではシート、Windows のコモンアイテムダイアログ、GTK のファイルダイアログまたはポータル。Web では File System Access のピッカーかファイル入力。スマートフォンでは `open` のみ） |
| Finder などでの表示、既定のアプリで開く、ゴミ箱に移動 | `shell.showItemInFolder`、`openPath`、`trashItem` | シェル | ✓ `platform/shell` の `reveal`、`open`、`trash`、`beep` |
| ファイルの関連付け | `fileAssociations`（electron-builder）、`open-file` | ファイルの関連付け | ✓ `asuka.toml` の `fileTypes`、`on files(paths)`（iOS、Android、インストールした Web アプリでも） |
| 単一インスタンスと二回目の起動 | `requestSingleInstanceLock`、`second-instance` | 単一インスタンス | ✓ `on secondInstance(args, dir)`、`asuka.toml` の `singleInstance`（Mac では引数ではなくファイルを渡す） |
| 終了と、終了前の確認 | `app.quit`、`before-quit` | 終了 | ✓ `quit()`、`relaunch()`、`on quit(e)`（`e.cancel()`、`e.sessionEnding`） |
| アプリのフォルダー | `app.getPath` | ディレクトリ | ✓ `fs.appDir()`（アプリのファイル）、`configDir()`（設定：Windows では Roaming の AppData、Linux では `~/.config`）、`cacheDir()`（Library/Caches、Android のキャッシュ、`~/.cache`。Web では別に保存）、`logDir()`（Library/Logs、`~/.local/state`）。どれもすべてのプラットフォームで、それぞれの決まった場所にある |
| 最近使った書類 | `app.addRecentDocument` | 最近使った書類 | ✓ `platform/recent`（Mac の「最近使った項目を開く」、ジャンプリスト、Linux のユーザーの最近使ったファイル） |
| 書類ベースのアプリ | — | 書類 | ✓ `platform/document`：`Documents`、ファイルごとのウィンドウ、「ファイル」メニュー、自動保存、復元用のコピー、閉じるときと終了するときの保存の確認、ほかのアプリによるファイルの変更（スマートフォンと Web では一度に一つの書類で、その場で保存） |
| ログイン時の起動 | `app.setLoginItemSettings` | ログイン時に起動 | ✓ `platform/login`（`SMAppService`、macOS 13 以降。Run キー。XDG autostart または Background ポータル） |
| ファイルの監視 | Node の `fs.watch`（chokidar） | `WatchFiles`、watch プラグイン | ✓ `std/watch` の `watch.path`：名前の変更は対にし、アトミックな保存は書き込みとして扱い、連続した変更はまとめる。スマートフォンと Web では非対応 |
| ディスプレイ | `screen` | ディスプレイ | ✓ `screen.displays()`、`nearest`、`matching`（Wayland では作業領域なし。スマートフォンと Web ではそれぞれの画面） |
| スリープ、スリープ解除、ロック、アイドル時間、バッテリー | `powerMonitor` | 電源 | ✓ `platform/power`（スマートフォンではバッテリー、低電力モード、ロック。Web ではバッテリー） |
| アプリのアイコンでの進捗と注意喚起 | `setProgressBar`、`flashFrame`、`dock.bounce` | Dock とタスクバー | ✓ `dock.setProgress`、`dock.requestAttention`（Linux では Unity launcher API に対応したドック） |
| アクセントカラー、コントラストを上げる、透明度を下げる | `systemPreferences` | アクセントカラーとアクセシビリティ | ✓ `platform/appearance`（Linux では「透明度を下げる」はなし） |
| 再生中の情報とメディアキー | （なし：ページ内の `navigator.mediaSession`） | Now Playing | ✓ `platform/nowplaying`（`MPNowPlayingInfoCenter`、ウィンドウの SystemMediaTransportControls、MPRIS のプレイヤー） |
| 自動アップデート：署名付きのリリース、差分、Sparkle のウィンドウ | `autoUpdater`（Squirrel）、electron-updater | アップデーターとそのプラグイン | ✓ `platform/update`（`update.UpdateWindow`、`update.AppMenu()`、`checkForUpdates`、`check`、`install`、`relaunch`。Ed25519 の署名、差分アップデート、チャンネル。GitHub のリリース、S3 のバケット、任意の HTTPS のディレクトリ、独自のサーバー。`asuka publish --platform mac\|windows\|linux`）。システムのパッケージ、Flatpak、ストアでインストールしたアプリは、それらを通じてアップデートする。Web は再読み込み |

MyGo にあって Asuka にまだないものは、書類のバージョン（Mac の「すべてのバージョンをブラウズ」）とロックです。

## 未対応のもの

アプリでの需要が高いものとして、OTA アップデート（EAS Update のようなもの。リリースビルドにはアプリがコンパイルされて組み込まれています）があります。Linux では音声のテキスト化が未対応です。
