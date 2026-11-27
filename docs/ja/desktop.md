# デスクトップ

ウィンドウとメニューバー（[ナビゲーション](navigation.md#ウィンドウ)）のほかにも、デスクトップにしかない機能があります。トレイアイコンと Dock メニュー、フライアウト、バックグラウンドで動き続けるアプリ、グローバルショートカット、パネルとランチャー、インストール済みアプリの検索、クリックスルーウィンドウ、ファイルダイアログとファイルマネージャー、アプリで開くファイル、単一インスタンス、終了、最近使ったドキュメント、ドキュメントベースのアプリ、ログイン時の起動、アップデート、「再生中」とメディアキー、ディスプレイ、電源、アプリアイコン上の進捗表示、そしてシステムの外観です。ファイルやフォルダーの変更（アプリが表示しているフォルダーや、ほかのアプリが保存したドキュメントなど）を追いかけるには、[ファイルの監視](data.md#ファイルの監視stdwatch)を参照してください。こちらはスマートフォンと Web でも使えます。

どれもプラットフォーム SDK（または標準ライブラリ）のパッケージか、prelude の要素や関数で、Mac、Windows、Linux で使えます。その機能がない電話やブラウザーでは、ないことがわかるように答えるか（`unsupported` や `notAvailable` のエラー、`supported()` が false）、何も表示しません。そのため、アプリは対応しているところでだけその機能を提供できます。

## デスクトップアプリの構成

Mac では、アプリは iOS アプリと同じく `Name.app` の中の 1 つの実行ファイルで、ネイティブパッケージの Swift もそこに組み込まれます。対象は 1 つのアーキテクチャだけです。Apple シリコン向け、または `--arch amd64` を付けると Intel 向けになります（両方をまとめたユニバーサルバイナリにはしません。サイズが倍になるためです）。Windows と Linux では、アプリは 1 つの実行ファイルで、Windows ではアプリのアイコン、マニフェスト、バージョン情報も中に組み込みます。Windows と Linux 向けのビルドは（Zig を使えば）どのシステムからでもでき、GTK をインストールしておく必要もありません。どのデスクトップアプリにも、`asuka.toml` から作られたアプリの設定（`asuka-app.json`：名前、ID、バージョン、アクセントカラー、URL スキーム、ファイルの種類、アップデート）が付きます。

| | アプリ | 設定 |
|---|---|---|
| macOS（`Name.app`） | `Contents/MacOS/Name` | `Contents/Resources/asuka-app.json` |
| Windows（フォルダー） | `Name.exe` | `asuka-app.json`（その隣） |
| Linux（インストールプレフィックス） | `bin/<exe>` | `share/<exe>/asuka-app.json` |

`asuka build` は、それぞれをユーザーがインストールする形で配布用にまとめます。Mac ではアプリを `.dmg` と `.zip` に、Windows ではフォルダーを `.zip` に、Linux ではプレフィックスを `.deb`、`.rpm`、AppImage、`.tar.gz`、Flatpak マニフェストにします（[配布](shipping.md)を参照）。

`asuka dev` では、代わりに開発用アプリの中でアプリを動かし、保存のたびにその変更を読み込みます（[開発](development.md#デスクトップでの実行)を参照）。

## トレイアイコンと Dock メニュー

`platform/tray` の `Tray` は、表示されているあいだトレイにアイコンを置きます（置いた場所には何も表示しません）。Mac ではメニューバーの `NSStatusItem`、Windows では通知領域のアイコン、Linux では StatusNotifierItem になります（KDE、Xfce、Cinnamon、MATE、Budgie、そして AppIndicator 拡張機能を入れた GNOME で表示されます）。

```asuka
import platform/tray

component Focus {
  state minutes = 25
  tray.Tray("timer") {
    symbol: "moon.zzz"
    title: "{minutes}:00" // アイコンの横に表示
    tooltip: "Focus timer"
    menu: [
      .button(title: "Start", run: () => start()),
      .submenu(title: "Length", items: [
        .button(title: "25 minutes", checked: minutes == 25, run: () => { minutes = 25 }),
        .button(title: "50 minutes", checked: minutes == 50, run: () => { minutes = 50 }),
      ]),
      .section(title: "", items: [.button(title: "Quit", run: () => quit())]),
    ]
  }
  Timer()
}

fn start() {}

component Timer {
  Text("25:00")
}
```

id はアプリのアイコンの中でそのアイコンを識別します（Mac では、ユーザーが ⌘ を押しながらドラッグした位置に戻ります）。`image`（アセットまたはファイル。高さ 16 ポイントで描かれます）または `symbol`（Mac ではテンプレート画像になり、メニューバーの外観に合わせて色が変わります。`template: true` で画像もテンプレートにできます）、`title`（Mac と、ラベルを表示する Linux のパネルで表示されます）、`tooltip`、`menu` はバインディングで、変わるたびに送り直されます。表示しているコンポーネントがなくなるとアイコンも消えます。`on click` がなければ、クリックでメニューが開きます。ある場合は、クリックで `click` が発生し、右クリックでメニューが開きます。

`menu` は `Menu` と同じく `Action` の配列です。`.button`（`disabled`、`checked`、`.destructive` のロール）、`.submenu`、それに区切り線で分けられる `.section` を使えます。`tray.DockMenu { items: […] }` は、Mac では Dock アイコンのメニューに項目を追加します。Windows ではジャンプリストの「タスク」になり、選ぶと実行中のアプリに届きます。Linux ではランチャーのクイックリストになります。複数表示されているときは、最後のものの項目が使われます。

`examples/tray` は、メニューバーに常駐するタイマーです。

## フライアウト

フライアウトは、トレイアイコンからぶら下がるウィンドウです。Mac のメニューバーエクストラや Windows の通知領域が開くパネルのようなもので、ウィンドウ内の矩形からぶら下げることもできます。`flyout` を指定した `Window` として作ります。

```asuka
import platform/tray

component Power {
  state open = false
  tray.Tray("power") {
    symbol: "bolt.fill"
    menu: [.button(title: "Quit", run: () => quit())] // 右クリックで表示
    on click { open = !open }
  }
  Window {
    bind open: open
    flyout: Flyout(anchor: tray.anchor("power"), placement: .bottom)
    fitContent: true
    minWidth: 260
    maxHeight: 480
    Panel()
  }
}

component Panel {
  Text("Charging")
}
```

- 枠もタスクバーボタンもありません。Mac では `background` を指定しなければ、コンテンツの背後にメニューの素材が表示され、角が丸く影が付きます。そのため、そこではコンテンツに背景を付けないでください。Windows 11 では角が丸くなり、ウィンドウの色の上に影が付きます。Linux ではどちらも描きません。
- `placement` は、アンカーのどちら側に出すか、アンカーとどう揃えるかを決めます（`.bottom`：下、中央揃え）。ディスプレイのその側に収まらないときは、まず反対側に移り（Mac のメニューバーのアイコンなら下、画面下部のタスクバーのアイコンなら上）、次にディスプレイ内に収まるようずれ、それでも足りなければ縮みます。`gap` はアンカーとのあいだを空けます。
- `anchor` はバインディングです。`tray.anchor(id)` はその id で表示されているトレイアイコン、`Anchor(x:, y:, width:, height:)` はフライアウトを宣言したウィンドウ内の矩形で、コンテンツの左上からのポイントで表します（ビューのフレームなど）。この場合、フライアウトはそのウィンドウの子になり、一緒に動きます。アンカーが変わると配置し直されます。
- 表示されるとキーボードを受け取りますが、アプリを前面には出しません。ユーザーがほかの場所をクリックしたとき、ほかのアプリに移ったとき、コンテンツが処理しない Escape を押したときに、ユーザーが閉じたのと同じように閉じます（`open` が false になり、`close` が発生するか、先に `closeRequest` に確認します）。
- その上のダイアログ：コンテンツの中で宣言した `modal` の `Window` は、フライアウトの上に、その中央に表示され、閉じるまでフライアウトを開いたままにします。閉じるとキーボードはフライアウトに戻ります。上にアラートを出しているあいだも開いたままです。
- `fitContent` はサイズをコンテンツに合わせ（後述）、大きさが変わるたびにアンカーに合わせて配置し直します。

| | トレイのフライアウト | 配置の基準 |
|---|---|---|
| macOS | 枠のない、アプリをアクティブにしない `NSPanel`。メニューのレベルにあり、背後はメニューの素材 | ユーザーがクリックしたステータス項目のコピー（ディスプレイごとのメニューバーに一つずつある） |
| Windows | 枠のない最前面のツールウィンドウ。Windows 11 の丸い角 | アイコンの矩形（`Shell_NotifyIconGetRect`）。タスクバーがどの辺にあるかも考慮 |
| Linux | 装飾のない、タスクバーに出ないウィンドウ | X11 ではパネルが伝えるアイコンのクリック位置。Wayland ではアプリがウィンドウを配置できないため、デスクトップが決める位置 |
| Web | ページ内のパネル。トレイがアンカーならページの右上 | ページ内の、そのウィンドウの矩形 |

`examples/tray` のアイコンは、タイマーの入ったフライアウトと、その上のダイアログを開きます。

## コンテンツに合わせたウィンドウサイズ

`fitContent: true`（`Window` のもので、メインウィンドウにもあります）は、ウィンドウを表示内容に合わせたサイズにし、内容が変わればそれに追従させます。たとえば、ノードの一覧が増えていくフライアウトや、検索結果が出たり消えたりするランチャーに使います。幅はコンテンツのいちばん長い行の幅（max-content）で、`minWidth` から `maxWidth` の範囲に収まります。`maxWidth` に達すると、テキストはその幅で折り返されます。高さはその幅でのコンテンツの高さで、`minHeight` から `maxHeight` の範囲に収まります。`maxHeight` を超えた分は、中にある `Scroll` がスクロールさせます。Asuka は各フレームのレイアウトでコンテンツをそのサイズで配置し、ウィンドウも同じパスでそのサイズになります。そのため、ウィンドウは最初から正しいサイズで表示され、いったん別のサイズで表示されることはありません。フライアウトはアンカーに対して配置し直され、通常のウィンドウやパネルは上端を保ったまま下へ伸びます。ユーザーはサイズを変更できません。Web では、メインウィンドウはタブそのもので、ページ全体を占めます。

## バックグラウンドで動くアプリ

`asuka.toml` で `keepRunning = true` にすると、最後のウィンドウを閉じてもアプリは動き続けます。メインウィンドウは隠れるだけで（バインドした `open` は false になります）、もう一度表示すれば戻ります（Mac では Dock アイコンのクリック、Linux ではアプリの再起動でも戻ります）。`background = true` にすると、さらに起動時にウィンドウを出さず、Mac では Dock アイコンも出しません。メニューバーやトレイに住み、`Tray` を入口にするアプリ向けです。このアプリが開く `Window` は通常どおり表示されます。終了はメニューから行います。トレイアイコンのメニューの `quit()` や、Dock の「終了」です。

こうしたアプリにメインウィンドウは要りません。app ブロックがウィンドウの外に何も表示しない（トレイアイコン、メニューバー、メニューから開く `Window` だけの）アプリにはメインウィンドウがなく、アプリが開くウィンドウだけがそのウィンドウになります（[メインウィンドウのないアプリ](navigation.md#メインウィンドウのないアプリ)）。`examples/tray` がその例です。

## グローバルショートカット

`hotkey.Hotkey` は、ほかのアプリが前面にあるときでもアプリに届くキーを登録します。登録が有効なのは、それが表示されている間です（それ自体は何も表示しません）。

```asuka
import platform/hotkey

component Launcher {
  state problem = ""

  Column {
    hotkey.Hotkey(Shortcut(key: "space", alt: true)) {
      description: "Show the launcher"
      on press { window.main().focus() }
      on error(e) { problem = e.message }
    }
    if !problem.isEmpty() { Text(problem) }
  }
}
```

`on press` は押されるたびにメインスレッドで実行されます。ショートカットはバインディング（ユーザーが設定で選んだキーなど）に追従し、変わると登録し直されます。`on error(e)` は失敗した理由を伝えます。`.taken`（ほかのアプリがそのキーを使っている）、`.notAllowed`（ユーザーが拒否した、またはデスクトップがアプリを認識していない）、`.notAvailable`、`.noKey` のいずれかです。`window.main()` はメインウィンドウのハンドルで、どこから呼んでも同じです。`focus()` で前面に出し、`show()`、`hide()` などもこれに対して働きます。

- **macOS**：Carbon のホットキーを使います。アクセシビリティの許可は不要です。システム自体のショートカットは使用中として扱われます。キーを押してもアプリは前面に来ないので、必要ならウィンドウに `focus()` してください。
- **Windows**：`RegisterHotKey` を使います。キーが押されると、アプリはフォアグラウンドを取れます。
- **Linux**：X11 ではアプリ自身がキーをつかみます。Wayland ではデスクトップが GlobalShortcuts ポータル（GNOME 48、KDE Plasma 6）を通じてキーを割り当てます。ユーザーに確認を求めたり、別のキーを選ばせたりすることがあり、アプリがデスクトップエントリーとともにインストールされている必要があります。`on press` の中で表示したウィンドウは、そのキー操作のアクティベーショントークンでフォーカスを得ます。
- スマートフォンと Web にはありません（`.notAvailable`）。

## パネルとランチャー

パネルは、Spotlight や Raycast のような、ランチャー、コマンドパレット、クイック入力のためのウィンドウです。グローバルショートカットでどこからでも呼び出せ、すべてのアプリのウィンドウより手前に浮かびます。ユーザーがいる操作スペース（Space）や仮想デスクトップに表示され、フルスクリーンのアプリの上にも重なります。タスクバー、Dock のウィンドウ一覧、アプリスイッチャーには現れません。Mac では、アプリを前面に出さずにキーボード入力を受け取ります。そのため、ユーザーが使っていたアプリのメニューバーはそのまま残り、パネルが消えるとキーボード入力もそのアプリに戻ります。パネルは `panel: true` を指定したウィンドウです。次の例では、バックグラウンドで動くアプリ（`background = true`）のメインウィンドウとしてランチャーを作っています。

```asuka
import platform/hotkey

state shown = false

app {
  Window {
    bind open: shown // ショートカットで表示するまで隠しておく
    panel: true
    fitContent: true
    minWidth: 640
    maxWidth: 640
    maxHeight: 440
    on closeRequest { shown = false } // Escape やほかの場所のクリック：隠す
    Launcher()
  }
}

component Launcher {
  hotkey.Hotkey(Shortcut(key: "space", alt: true)) {
    description: "Show the launcher"
    on press { shown = !shown }
  }
  Search()
}

component Search {
  TextField { placeholder: "Search" }
}
```

- 枠はありません。Mac では、`background` を指定しないとコンテンツの背後にポップオーバーのマテリアルが表示され、角が丸くなって影が付きます。Windows 11 では角が丸くなります。Linux では、通常のウィンドウのコンテンツと同じように描かれます。
- `x` と `y`（または `stateKey`）を指定しないと、表示するたびに、ポインターのあるディスプレイの左右中央、上から 3 分の 1 の位置に表示されます。`w.setPosition` や `w.center()` を使うと、位置を固定できます。`fitContent` を指定すると、結果が増えるにつれて上端から下へ伸びます。
- コンテンツが処理しなかった Escape キー（テキストフィールドでの Escape も含む）を押したときや、キーボードフォーカスを失ったとき（ほかの場所のクリック、ほかのアプリへの切り替え）は、ユーザーが閉じたときと同じように閉じます。閉じる代わりに隠すようにすれば（上の例の `on closeRequest`）、次はすぐに元の状態のまま表示できます。なお、動き続けるアプリのメインウィンドウは、どのみち閉じると隠れるだけです。
- 表示すると（`open` を true にする、`w.show()`）、パネルはキーボード入力を受け取ります。`w.focus()` も同じ動作で、アプリを前面に出すことはありません。

| | パネル |
|---|---|
| macOS | nonactivating スタイルの `NSPanel`。すべての操作スペースとフルスクリーンのアプリの上に表示され（`canJoinAllSpaces`、`fullScreenAuxiliary`）、ウィンドウの切り替え対象から外れ、フローティングする。メインウィンドウにはならない |
| Windows | 非表示のウィンドウが所有する、枠なしで最前面のツールウィンドウ（タスクバーのボタンなし）。Windows 11 の角丸付き。フォアグラウンドになる（グローバルショートカットのキー入力がそれを許可する） |
| Linux | 装飾のないウィンドウ。X11 では、常に手前に保たれ、すべてのワークスペースに表示され、タスクバーとページャーに出ないユーティリティウィンドウ。Wayland では、アプリが gtk4-layer-shell（`LD_PRELOAD`）付きで、そのプロトコルを持つコンポジター（KDE Plasma、Sway、Hyprland、COSMIC）上で動いていれば、overlay レイヤーの layer-shell サーフェス。そうでなければほかと同じ普通のウィンドウ（GNOME が配置し、常に手前に保たれることもない） |
| Web | ページの上に重なるパネル。上から 3 分の 1 の位置 |

`examples/launcher` は自作の Spotlight です。⌥Space（Alt+Space）で検索フィールド付きのパネルが開き、入力に合わせてインストール済みのアプリがアイコン付きで表示されます。Enter を押すと先頭のアプリが開きます。

## アプリの検索

`platform/apps` は、ランチャーのようにインストール済みのアプリを一覧し、検索します。

```asuka
import platform/apps

component Apps {
  state installed: [apps.App] = []
  state query = ""
  state found: [apps.App] = []
  state icon = ""

  task { installed = (try? await apps.list()) ?? [] }
  task(installed, query) {
    found = (try? await apps.search(installed, query)) ?? []
    // Image に渡せる PNG。
    if let first = found.first() { icon = (try? await apps.icon(first, 32)) ?? "" }
  }

  Column {
    TextField { bind text: query }
    for a in found key a.id {
      Button(a.name) { on press { try? await apps.open(a) } }
    }
    if !icon.isEmpty() { Image(icon) }
  }
}
```

- `list()` は各アプリの `name`（システムが表示する名前、ユーザーの言語で）、`id`、`path` を名前順で返します。ディスクの読み込みはメインスレッドの外で行います。アプリは追加も削除もされうるので、パネルを表示するたびに一覧し直してください。
- `search` は、ユーザーが入力した文字列でアプリを順位付けします。大文字と小文字、アクセントの違いは無視し、名前そのもの、名前の先頭、単語の先頭または頭文字（"vsc" で Visual Studio Code）、名前のどこか、名前の中に順に現れる文字、の順に並べます。
- `icon` は、アプリのアイコンをポイント単位のサイズで PNG ファイルとして描き、アプリのキャッシュに保存して（次回にも使います）、そのパスを返します。
- `open` はアプリを起動するか、前面に出します。先にパネルを隠してください。そうすれば、開いたアプリがキーボードを受け取るまで、ユーザーが使っていたアプリがキーボードを持ち続けます。

| | 場所 | ID | アイコン | 起動 |
|---|---|---|---|---|
| macOS | `/Applications`、`/System/Applications`、`~/Applications` と、それぞれの一つ下のフォルダー、そして Finder | bundle identifier | ワークスペースが返すそのアプリのアイコン | `NSWorkspace` |
| Windows | スタートメニューのアプリ（シェルの Applications フォルダー）。パッケージアプリも含む | AppUserModelID | シェルが返すそのアプリの画像 | シェル |
| Linux | メニューに表示されるデスクトップエントリー（XDG のほか、Flatpak と Snap のものも含む）。`OnlyShowIn`、`NoDisplay`、`TryExec` で判定する | デスクトップファイル ID | テーマから取得したエントリーのアイコン | GIO（ディスプレイの起動コンテキスト付き） |

スマートフォンと Web では `apps.AppsError.unsupported` を投げます。

## クリックスルーウィンドウ

`clickThrough: true`（または `w.setClickThrough(true)`）を指定すると、マウス操作がウィンドウを素通りして、その背後にあるもの（ほかのアプリのウィンドウやデスクトップ）に届きます。`transparent` と `alwaysOnTop` を組み合わせれば、オーバーレイ、HUD、デスクトップマスコットなどを作れます。`w.clickThrough` でこの状態を読め、読んだバインディングは追従します。有効なあいだ、ウィンドウはクリックもホバーもホイールも受け取りません。ただし `takesClicks: true` を指定したビューの上は例外で、HUD のボタンやユーザーがドラッグするマスコットなどに使えます。ウィンドウ全体を操作させたいときは無効にしてください。

```asuka
state hud = true

app {
  Window {
    bind open: hud
    transparent: true
    alwaysOnTop: true
    clickThrough: true
    width: 240
    height: 80
    Meters()
  }
}

component Meters {
  Row {
    gap: 8; align: .center
    Text("CPU 12%")
    View {
      takesClicks: true
      Button("Hide") { on press { hud = false } }
    }
  }
}
```

実装は、Mac では `ignoresMouseEvents`、Windows では `WS_EX_TRANSPARENT` を付けたレイヤードウィンドウ、Linux ではクリックを受け取るビューだけからなる入力領域（`gdk_surface_set_input_region`）です。Mac と Windows では、マウスがそれらのビューの上に来たときにウィンドウがマウスを受け取ります。Web では、ページ内のウィンドウがクリックをページに通します。ブラウザーの外までクリックを通す方法はありません。

## ファイルダイアログ

`platform/dialog` はシステムのファイルダイアログを表示し、コピーではなくユーザーのファイルそのもののパスを返します。

```asuka
import platform/dialog
import std/fs
import platform/shell

async fn export(notes: string) throws {
  let md = [dialog.Filter(name: "Markdown", types: ["text/markdown"], extensions: ["md"])]
  if let path = try await dialog.save(name: "Notes.md", button: "Export", filters: md) {
    try fs.writeText(path, notes)
    try? await shell.reveal(path)
  }
}
```

`dialog.open`（`multiple` で複数のファイル、`filters` で `image/*` のような MIME タイプや拡張子による絞り込み）、`dialog.save`（候補の `name` を指定でき、既存のファイルを置き換える前にはダイアログが確認します）、`dialog.chooseFolder`、`dialog.chooseFolders` があります。`title`、`button`、`start`（フォルダー、またはその中のファイル）で設定します。キャンセルされると、パスは返らず（`none` または空のリスト）、エラーにもなりません。

Mac ではウィンドウに付くシートとして、Windows ではウィンドウに対してモーダルなコモンアイテムダイアログとして、Linux では GTK のファイルダイアログ（Flatpak の中ではデスクトップのポータル）として表示されます。ブラウザーでは、open と chooseFolder は選ばれたものをアプリのファイルにコピーし、save が返すファイルは、ユーザーが選んだファイルに書き込まれるか（Chrome、Edge）、ダウンロードされます。iOS と Android では、open はドキュメントピッカー（`device.pickFiles` と同じくコピー）で、save と chooseFolder は `unsupported` のエラーを投げます。

テストでは、ダイアログにテスト自身が答えます。`dialog.fake()` は `FakeDialogs` を入れます。その `opens`、`saveTo`、`folder` がユーザーの選ぶもの（none や `[]` はキャンセル）で、`shown` は表示を求められたダイアログの一覧です（`"save Untitled.txt"`）。

## ファイルマネージャー

`platform/shell` は、ファイルマネージャーがファイルに対して行う操作を提供します。`shell.reveal(path)` は、ファイルを含むフォルダーを開き、そのファイルを選択した状態で表示します（Finder、エクスプローラー、またはデスクトップのファイルマネージャー）。`shell.open(path)` は、ファイルをその種類に対応するユーザーのアプリで開き（該当するアプリがなければ `shell.ShellError.noApp` を投げます）、フォルダーならファイルマネージャーで開きます。`shell.trash(path)` は、ファイルをゴミ箱（Windows ではごみ箱）に移します。完全に削除することはなく、移せないときはエラーを投げます。`shell.beep()` は警告音を鳴らします。スマートフォンと Web にはファイルマネージャーがないため、reveal と trash は `unsupported` のエラーを投げます。Web の open は、新しいタブでファイルを表示します。ファイルの内容をアプリの中で表示するには、`platform/preview` を使ってください。

## ファイルを開く

アプリが開くファイルの種類を `asuka.toml` の `fileTypes` で宣言し（[配布](shipping.md)を参照）、`app` ブロックの `on files(paths)` で処理します（コンポーネントの本体で `onOpenFiles` を使えば、そのコンポーネントがある間だけ処理します）。

```asuka
state opened: [string] = []

app {
  on files(paths) {
    for p in paths { opened.push(p) }
  }
  Column {
    for p in opened key p { Text(p) }
  }
}
```

一緒に開かれたファイルは、絶対パスとして 1 回の呼び出しでまとめて届きます。Finder、エクスプローラー、「ファイル」でのダブルクリック、「このアプリケーションで開く」、Dock アイコンへのドロップ、最近使ったドキュメントの選択、Windows と Linux ではコマンドラインで渡されたファイルです。起動時に渡されたファイルは、最初のハンドラーが登録されるまで待ちます。iOS では、パスはドキュメントがある場所そのもので、アプリが終了するまで読めます。Android と Web では、アプリのキャッシュにあるアプリ自身のコピーです。

## 単一インスタンス

Windows と Linux では、`id` を持つアプリは一つしか起動しません。もう一度起動すると、実行中のアプリが前面に出ます。`app` ブロックの `on secondInstance(args, dir)` で、新たな起動の引数と作業ディレクトリを受け取れます。引数に含まれるファイルとリンクは、`on files(paths)` と `on openURL(url)` にも渡されます。

```asuka
state newWindow = false

app {
  on secondInstance(args, dir) {
    if args.contains("--new-window") { newWindow = true }
  }
  Window { Main() }
}
component Main {
  Text(if newWindow { "A new window" } else { "Main" })
}
```

コンポーネントの本体に書く `onSecondInstance(fn([string], string))` も同じ働きで、そのコンポーネントがあるあいだ有効です。毎回新しいインスタンスを起動したい場合は、`asuka.toml` で `singleInstance = false` と設定します。Mac では、アプリは常に一つしか起動せず、渡されるのは引数ではなくファイルなので、これは呼ばれません。

## 終了

`quit()` は、ユーザーが「終了」を選んだときと同じようにアプリを終了します。終了のきっかけ（⌘Q、Dock、最後のウィンドウを閉じる、`quit()`、Ctrl+C や SIGTERM、ログアウト）にかかわらず、まず `app` ブロックの `on quit(e)` が呼ばれます。`e.cancel()` を呼べばアプリは終了しないので、未保存の変更について確認できます。`e.canceled()` で、ほかのハンドラーが取り消したかどうかがわかります。`e.sessionEnding` が true のときは終了を取り消せないため、ダイアログを出さずにすぐ保存してください。

```asuka
state unsaved = false
state confirmQuit = false

app {
  on quit(e) {
    if unsaved && !e.sessionEnding {
      e.cancel()
      confirmQuit = true // 確認し、保存できたらもう一度 quit() を呼ぶ
    }
  }
  Window { Editor() }
}
component Editor {
  Text("Editor")
}
```

コンポーネントの本体に書く `onQuit(fn(QuitEvent))` も同じ働きで、そのコンポーネントがあるあいだ有効です。ウィンドウ自体を閉じるかどうかは、それより先に、そのウィンドウのガードで問い合わせられます（[ナビゲーション](navigation.md#ウィンドウを閉じる前の確認)）。

`relaunch()` は同じ手順で終了し（ハンドラーで取り消すこともできます）、終了したあとで同じ引数でアプリを再び起動します。アップデートのあとや、起動時に反映される設定を変えたときに使います。アプリが自分で終了しない電話と Web では、どちらも無視されます。

## アップデート

`platform/update` の `update.UpdateWindow()` をアプリのメインウィンドウに一度置くと、Sparkle 方式のアップデーターになります。起動直後と 1 日 1 回、新しいバージョンを確認し、リリースノートを「アップデートをインストール」「このバージョンをスキップ」「後で通知」のボタンとともに表示します。ダウンロードは進捗バー付きで行い（実行中のバージョンからの差分があれば、変更部分だけをダウンロードします）、アプリの鍵で署名されていることを確かめてからインストールし、「今すぐ再起動」を提示します。`MenuBar` に `update.AppMenu()` を置くと、Mac では「…について」の次に「アップデートを確認…」が入ります（ほかのプラットフォームでは「ヘルプ」メニューに入ります）。`update.CheckForUpdates()` はその項目だけです。独自の UI を作るには、`update.check()`、`Release.install(progress:)`、`update.relaunch()` と、アップデーターの状態（`update.updater`）を使います。

```asuka
import platform/update

app {
  Window {
    title: "Notes"
    update.UpdateWindow()
    MenuBar { update.AppMenu() }
    Text("Notes")
  }
}
```

鍵ペアは `asuka update keygen` で作ります。リリースの公開先（GitHub のリリース、S3 バケットや任意の HTTPS ディレクトリ、または自前のサーバー）は `asuka.toml` の `updates` で指定し、`asuka publish --platform mac`（`windows`、`linux`）でビルド、署名、アップロードを行います（[配布](shipping.md#自動アップデート)）。ダウンロードとアップデートの配置はアプリ自身が行います。Mac アプリの新しいバンドルは、アプリが終了するのを待ってから置き換わります。Windows と Linux では、実行中にアプリのファイルを置き換えます。システムのパッケージ（`.deb`、`.rpm`）、Flatpak、Snap、AppImage、アプリストアからインストールされたアプリは、それらによって更新されます。そのことは `await update.unsupported()` でわかり、スマートフォンでは `.notAvailable` が返ります。Web では、サイトより古いページがリロードを促します。

## 最近使ったドキュメント

ユーザーがドキュメントを開いたり保存したりするたびに、`recent.add(path)`（`platform/recent`）を呼びます。Mac では「ファイル」メニューの「最近使った項目を開く」と Dock メニューに、Windows ではジャンプリストに、Linux ではユーザーの最近使ったファイルに並びます。ユーザーがその一つを選ぶと、`on files(paths)` で開かれます。`await recent.list()` は、なくなったファイルを除いて新しい順に返し、`recent.clear()` はアプリ自身の項目を消します。

## ドキュメント

`platform/document` を使うと、テキストエディターのように、ウィンドウごとに一つのファイルを表示するアプリを作れます。ドキュメントのモデルと、その周りのウィンドウや画面を提供します。ドキュメントの種類とエディターを記述します。

```asuka
import platform/document

let types = [document.Type(name: "Plain Text", extensions: ["txt"])]

app {
  document.Documents(types) { editor: doc => Editor(doc) }
}

component Editor(doc: document.Document<string>) {
  if let text = doc.content {
    TextField {
      text: text
      multiline: true
      grow: 1
      on change(t) { doc.edit(t) }
    }
  }
}
```

システムがこれらのファイルを開くアプリとしてこのアプリを候補に出すよう、同じ種類を `asuka.toml` の `fileTypes` にも列挙してください。`examples/textedit` はこの種のエディターに、ステータス行と、ほかのアプリがファイルを変更したときに出るバーを加えたものです。

`Document` はストアです。`content`（ファイルを読むまでは none）と `state`（`.loading`、`.saved`、`.edited`、`.saving`、`.conflict`、`.failed`、理由は `error`）を持ちます。`doc.edit(c)` で内容を変えると、最後の変更から少しして保存されます。`save()` はすぐに保存し、`saveSoon()` は待てないコード（キーやメニュー項目）から保存します。読み込んだあとにほかのプログラムがファイルを変更していると、競合になります。`saveAnyway()` はアプリの内容を残し、`revert()` はファイルの内容を取ります。`file` はドキュメントのあるファイル（名称未設定なら ""）、`name` はその名前です。`saveAs(path)`、`rename(name)`、`moveTo(dir)` で場所を変え、`unsaved()` はファイルにまだない変更があるかどうかを返し、`discard()` はその変更を捨てます。`versions()` はファイルの以前の内容で、新しいものが先です。保存のたびに、ファイルを置き換える前の状態がバージョンとして残ります（ユーザーが求めた保存は毎回、自動保存は前のバージョンから数分たってから）。バージョンは Mac と同じく、古くなるにつれて間引かれます（直近 1 時間はすべて、そのあと 1 日は 1 時間に一つ、1 か月は 1 日に一つ、それ以降は 1 週間に一つ）。`restore(v)` はそのバージョンを編集として戻します。`lock()` はファイルをロックし（Mac ではロック、ほかでは読み出し専用）、`locked` でわかり、`unlock()` で解除します。ロックされたドキュメントは保存されず、編集は `lockedEdits` に数えられるので、エディタはユーザーに尋ねられます。`document.open(path, read:, write:)` は、独自の read と write 関数でバイト列と相互に変換する、どんな型の内容のドキュメントでも作れます。`Documents` が編集するのはテキストです。

ユーザーに見えるもの：

- **デスクトップでは、ドキュメントごとにウィンドウが一つ**。ファイル名がタイトルになり、Mac ではタイトルバーにファイルのアイコンが、未保存の変更があるあいだは閉じるボタンに点が表示されます。アプリのメインウィンドウ（`Documents` はその中身です）は、起動時に隠されます。Windows と Linux では、メモ帳と同じく最後のウィンドウを閉じるとアプリが終了します。Mac ではアプリは動き続け、メニューバーの新規と開く…が使えます。
- **「ファイル」メニュー**：Mac ではテキストエディットと同じく、新規（⌘N）は名称未設定のドキュメントを開き、開く…（⌘O）はシステムのダイアログ、最近使った項目を開くはシステムの最近使ったドキュメントです。続いて、閉じる（⌘W）、保存（⌘S）、複製（⇧⌘S）、別名で保存…（⌥⇧⌘S）、名称変更…、移動…、最後に保存した状態に戻す。Windows と Linux では、メモ帳や GNOME テキストエディターと同じく、新規、開く…、最近使った項目を開く（最近のドキュメントと「メニューを消去」）、保存、別名で保存…（Ctrl+Shift+S）、元に戻す、閉じる、終了です。どちらにも「バージョンをブラウズ…」と「ロック」（「ロック解除」）があります。どの項目も、使用中のウィンドウのドキュメントに対して働きます。メニューの言葉はアプリの言語に合わせます（英語、ドイツ語、スペイン語、フランス語、日本語、中国語。それ以外は英語）。
- **自動保存**：ユーザーが入力をやめて少しすると、ドキュメントは保存されます。何も尋ねません。その場で保存できないドキュメント（名称未設定のもの、ほかのアプリが変更したもの、ファイルに書き込めないもの）は、変更を `document.recoveryDir()` の復元用コピーに残し、アプリを終了してもそこに残ります。次に起動したとき、それらのドキュメントは編集済みの状態でまた開きます。
- **バージョンとロック**：「バージョンをブラウズ…」はドキュメントの以前のバージョンを一覧にし、選んだもののテキストを横に表示します。「復元」で選んだバージョンを戻します（戻す前のドキュメントもバージョンとして残ります）。ロックされたドキュメントを編集すると、Mac と同じく、ロック解除、複製（その編集を新しいドキュメントに）、キャンセル（ファイルはそのまま）を尋ねます。
- **変更が失われるときの確認**：変更のある名称未設定のドキュメントを閉じると、残すかどうかを尋ねます（保存…、削除、キャンセル）。保存に失敗したドキュメントや、ほかのアプリの変更とぶつかったドキュメントを閉じると、保存するかどうかを尋ねます（保存、保存しない、キャンセル）。ほかのアプリが変更したファイルに保存するときは「それでも保存」か「元に戻す」かを尋ね、最後に保存した状態に戻す前にも確認します。移動…は、同じ名前のファイルを置き換える前に尋ねます。
- **スマートフォンとブラウザーでは**、一度に一つのドキュメントです。アプリはドキュメントの一覧（新規付き）で始まり、ドキュメントを開くとその画面が一覧の上にプッシュされ、編集するそばから保存されます。一覧の行をスワイプすると、複製、名称変更（新しい名前を尋ねます）、削除（先に確認します）ができます。

## ログイン時の起動

`try await login.set(true)`（`platform/login`）を設定として用意し、`await login.enabled()` で状態を読み戻します。macOS 13 以降ではログイン項目になります（パッケージ化したアプリのみ。ユーザーがシステム設定で許可するまでは `LoginError.needsApproval` を投げます）。Windows では Run エントリー、Linux では自動起動エントリー（Flatpak では Background ポータル）です。`await login.openedAtLogin()` でログイン時の起動かどうかを見分けられるので、アプリは目立たずに起動できます。

## 「再生中」とメディアキー

`nowplaying.NowPlaying` は、表示されている間、アプリが再生しているものをシステムのメディアコントロールに表示します。そのボタンやキーボードのメディアキー（再生／一時停止、次へ、前へ）の操作は、アプリのハンドラーに届きます。Mac ではコントロールセンターとメニューバーの「再生中」、Windows では音量のフライアウトとクイック設定のメディアコントロール（アプリのウィンドウの SystemMediaTransportControls）、Linux では MPRIS プレイヤーになり、GNOME や KDE のコントロール、`playerctl` から操作できます。スマートフォンと同じパッケージで、スマートフォンではロック画面のコントロールになります（[デバイスとサービス](device.md#再生中の表示とメディアキーplatformnowplaying)）。

```asuka
import platform/audio
import platform/nowplaying

component Spring {
  event next

  let music = audio.Player("music/spring.mp3")

  nowplaying.NowPlaying {
    title: "Spring"
    artist: "Vivaldi"
    // 再生状態と再生位置はここから取り、再生・一時停止・シークもここに対して行う。
    player: music
    on next { emit next }
  }
}
```

## ディスプレイ、電源、アプリのアイコン

`screen.displays()` はディスプレイの状態に追従します（追跡される値で、バインディングで読むと、読まれているあいだディスプレイを監視します）。各ディスプレイの位置、作業領域、スケール、リフレッシュレート、どれがメインかがわかります。`screen.nearest` と `screen.matching` は、ある点やウィンドウの下にあるディスプレイを選びます。Wayland には作業領域がないため、ディスプレイ全体の範囲になります。

`power.status()` は、バッテリー、低電力モード、温度の状態に追従します。`power.suspends()`、`resumes()`、`locks()`、`unlocks()` は、コンピューターのスリープと復帰、画面のロックとその解除のストリームです。`power.idleTime()` は、ユーザーが操作していない時間を返します。スマートフォンにはロックとロック解除はありますが、スリープとアイドル時間はありません。Web ではバッテリーだけです。

```asuka
import platform/power
import platform/screen

component Status {
  let p = power.status()
  let displays = screen.displays()

  task {
    for await _ in power.suspends() { print("going to sleep") }
  }

  Column {
    Text(if p.battery { "Battery {(p.level * 100).fixed(0)}%" } else { "No battery" })
    Text(displays.map(d => "{d.name} {d.bounds.width}x{d.bounds.height}@{d.scale}x").joined(separator: "\n"))
  }
}
```

`dock.setProgress` は、タスクの進捗を Dock アイコン、タスクバーのボタン、または Unity ランチャー API を持つ Linux のドックに表示します（GNOME 標準の Dash には表示されません）。`dock.requestAttention` は、アプリがバックグラウンドにあるあいだ、アイコンを弾ませたり点滅させたりします。アイコン上の数字は `notify.setBadge` で設定します。

`appearance.current()` は、システムのアクセントカラー（`Color?`）と、「コントラストを上げる」「透明度を下げる」の設定を、追跡される値として返します。これはアプリが自分で描くもののためのもので、Asuka が表示するビューはすでにこれらに従います。
