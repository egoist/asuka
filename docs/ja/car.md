# 車の画面

`platform/car` を使うと、アプリを車の画面に表示できます。iOS では CarPlay、Android では Android Auto です。車の画面にはアプリ自身のビューは表示されません。運転中の視線は道路に向けておくべきなので、どちらのプラットフォームも自前のテンプレート（リスト、グリッド、タブ、情報、メッセージ、地図上の場所、プレイヤー）だけを表示し、車に合わせて描きます。画面の大きさ、ノブかタッチスクリーンか、昼と夜の見た目などです。アプリはテンプレートを値として記述し、その行を選んだときに何をするかを指定するだけで、あとはプラットフォームが受け持ちます。

```asuka check
import platform/car

struct Station {
  name: string
  free: int
  latitude: float
  longitude: float
  meters: float
}

let stations = [
  Station(name: "Main St", free: 2, latitude: 35.681, longitude: 139.767, meters: 850),
  Station(name: "Harbor", free: 0, latitude: 35.662, longitude: 139.75, meters: 3200),
]

component Chargers {
  state reserved = ""

  fn detail(s: Station) -> car.Template = .info(
    title: s.name,
    fields: [car.Field(title: "Free", detail: "{s.free}"), car.Field(title: "Reserved", detail: if reserved == s.name { "Yes" } else { "No" })],
    actions: [.button(title: "Reserve", disabled: s.free == 0, run: () => { reserved = s.name })],
  )

  car.Screen {
    root: .places(title: "Chargers", places: stations.map(s => car.Place(
      title: s.name,
      detail: "{s.free} free",
      latitude: s.latitude,
      longitude: s.longitude,
      distance: s.meters,
      run: () => car.push(() => detail(s)),
    )))
  }

  Column {
    padding: 24
    gap: 8
    Text(if car.connected() { "On the car's screen" } else { "Not in a car" })
    Text(if reserved.isEmpty() { "Nothing reserved" } else { "Reserved: {reserved}" })
  }
}

app { Chargers() }
```

アプリが車の上で何であるか、つまりカテゴリも `asuka.toml` に書きます（[カテゴリ](#カテゴリasukatoml-の-car)）。

```toml
[car]
category = "charging"
```

## 車の画面への表示

`car.Screen` がマウントされているあいだ、アプリは車の画面に表示されます。アプリのどこに置いてもかまいません。スマートフォンの画面には何も表示しません。`root` は、車の画面が最初に表示し、戻るボタンで最後に戻ってくるテンプレートです。ビューと同じく、読み取った状態に追従します。上の例では `reserved` が変わると、車の画面の情報テンプレートがその場で更新されます。

- `car.push(() => template)` は、表示中のものの上にテンプレートを積みます。車の戻るボタンで取り除かれます。関数を受け取るので、積んだテンプレートも読み取った状態に追従します。
- `car.pop()` は一つ戻り、`car.popToRoot()` はルートまで戻ります。`car.depth()` はテンプレートがいくつ積まれているかを返します。
- `car.connected()` は、車の画面にいまアプリが表示されているかどうかを返します。追跡されるので、ビューの中で読むと、車の接続や切断に合わせてビューが更新されます。

車は、スマートフォンに画面がない状態でアプリを起動することがあります。スマートフォンをつないで、車の画面でアプリを選んだ場合などです。このときもアプリのコードはスマートフォンと同じように動き、ビューは画面なしで作られるので、その中のどこかにある `car.Screen` が表示されます。車が切断されると積んだテンプレートも消え、次に接続したときはルートから始まります。`car.Screen` が複数マウントされているときは、最後にマウントされたものが表示されます。

## テンプレート

| テンプレート | 表示するもの | CarPlay | Android Auto |
|---|---|---|---|
| `.list(title:, items:, sections:, actions:, empty:)` | 行の並び。まず `items`、続いて見出し付きの `sections`。行がないときは `empty` | `CPListTemplate` | `ListTemplate` |
| `.grid(title:, items:, actions:)` | セルの並び。それぞれ画像とタイトル | `CPGridTemplate`（最大 8 個） | `GridTemplate` |
| `.tabs(tabs)` | タブ（`car.Tab(title:, symbol:, content:)`）。それぞれリスト、グリッド、情報、場所のいずれか | `CPTabBarTemplate` | `TabTemplate`（Car App API レベル 6。それより前は最初のタブだけ） |
| `.info(title:, fields:, actions:)` | ラベルと値の組の情報と、その下のボタン | `CPInformationTemplate` | `PaneTemplate` |
| `.message(title:, text:, symbol:, actions:)` | メッセージと、その下のボタン | `CPInformationTemplate` | `MessageTemplate` |
| `.places(title:, places:)` | 地図上の場所と、その横の一覧 | `CPPointOfInterestTemplate` | `PlaceListMapTemplate` |
| `.nowPlaying` | 再生中のものとその操作ボタン | `CPNowPlayingTemplate` | Android Auto 自身のプレイヤー |

`Action` と同じく、省略したフィールドはゼロ値になります。`.list(title: "Recent", items: recent)` にはセクションもボタンもありません。車は表示できるだけ表示します。CarPlay ではおよそ十数行、Android Auto では車が許す数まで（走行中は 6 行の車もあります）で、残りは省かれます。役に立つものから先に並べてください。

### 行、セル、ボタン

リストの行やグリッドのセルは `car.Item` です。

| フィールド | 内容 |
|---|---|
| `title` | タイトル。 |
| `detail` | タイトルの下の一行。 |
| `image` | 画像。アセット（`"covers/spring.jpg"`）、ファイルのパス、http(s) の URL。 |
| `symbol` | 画像がないときに使うシンボルの名前（`Symbol` と同じ）。 |
| `browsable` | 選ぶとさらに表示する（テンプレートを積む）もの。末尾にシェブロンが付きます。 |
| `playing` | いま再生しているもの。CarPlay が印を付けます。 |
| `progress` | 再生した割合（0 から 1）。CarPlay が行の下にバーを表示します。 |
| `enabled` | `false` にすると表示はされますが、選べません。 |
| `run` | 選んだときにすること。 |

テンプレートの `actions` はそのボタンで、`Action` として書きます。`.button(title:, icon:, role:, disabled:, run:)` です。リストやグリッドではバーに、情報やメッセージのテンプレートでは本文の下に表示されます。`section` の中のボタンは展開して取り込まれますが、サブメニューは取り込まれません（車にはサブメニューがありません）。

場所（`car.Place`）には `title`、`detail`、`latitude` と `longitude`、メートル単位の `distance`（車で使われる単位で表示）、`run` があります。Android Auto は、選んでも何も積まない場所を距離付きでしか一覧に載せません。距離のない場所は、選ぶと何かを積む行として表示されます。

## オーディオアプリと再生中の表示

オーディオアプリは、ロック画面と同じく `platform/nowplaying` で再生中のものを表示します。車のプレイヤーに表示されるのも同じ情報で、そのボタンは `NowPlaying` のハンドラー（またはその `player` の再生、一時停止、シーク）を呼び出します。CarPlay のオーディオアプリにならって、行の再生を始めたときに `.nowPlaying` を積みます。

```asuka
car.Item(title: track.title, image: track.cover, playing: track == current, run: () => {
  play(track)
  car.push(() => .nowPlaying)
})
```

Android Auto では、オーディオアプリはテンプレートを使いません。Android Auto はメディアアプリを自前のブラウザーとプレイヤーで表示します。`platform/car` はこのブラウザーのツリーをテンプレートから作ります。ルートはルートテンプレートの行（タブバーのタブは Android Auto のタブになります）で、`browsable` な行の子は、その行を選んだときに積まれるテンプレートの行です。browsable でない行は再生できる行で、選ぶとその `run` が呼ばれ、それが再生を始めます。Android Auto のプレイヤーはアプリの `NowPlaying` を表示し、通知の操作ボタンと同じようにコマンドを送り返します。情報、メッセージ、場所のテンプレートは、メディアブラウザーには載りません。

`examples/car` は、CarPlay と Android Auto の両方で動くオーディオアプリです。

## カテゴリ：asuka.toml の [car]

車は、そのプラットフォームが認めるカテゴリのアプリだけを表示します。カテゴリごとに使えるテンプレートがあり、Apple と Google はアプリごとにカテゴリを審査します。アプリのカテゴリは `asuka.toml` の `[car]` に書きます。

| `category` | CarPlay | Android Auto |
|---|---|---|
| `audio` | オーディオ（`com.apple.developer.carplay-audio`） | メディア：メディアブラウザー |
| `communication` | コミュニケーション | メッセージ |
| `charging` | EV 充電 | スポット（POI） |
| `parking` | 駐車場 | スポット（POI） |
| `fueling` | 給油 | スポット（POI） |
| `quickFood` | クイックフードの注文 | （なし） |
| `drivingTask` | 運転タスク | （なし） |
| `pointOfInterest` | （なし） | スポット（POI） |
| `iot` | （なし） | IoT |
| `weather` | （なし） | 天気 |

`minCarApiLevel` は、アプリのテンプレートが Android Auto で必要とする Car App API のレベルです（デフォルトは 1。タブには 6 が必要）。ナビゲーションアプリは車の画面に自分で地図を描く必要がありますが、`platform/car` はそれを行いません。

各プラットフォームに必要なものはカテゴリから決まります。

- **iOS**：そのカテゴリの CarPlay の entitlement と、`Info.plist` の CarPlay シーン（`platform/car` が受け持ちます）。entitlement は申請すると Apple が付与します（[CarPlay](https://developer.apple.com/carplay/)）。アプリの App ID に追加すると、プロビジョニングプロファイルにも含まれます。これがないまま署名付きビルドを作ると、その旨が表示されます。シミュレーター用のビルドには常に含まれます。
- **Android**：Car App Library の `CarAppService` とそのカテゴリ、Android Auto の `automotive_app_desc.xml`、そして Car App API のレベル（オーディオアプリでは代わりにメディアブラウザー）。デバッグビルドは Desktop Head Unit を含むどの車載ホストにも表示されますが、リリースビルドは Google が挙げる車載ホストにだけ表示されます。アプリの車のカテゴリは、車に届く前に Google Play の審査を受けます。

カテゴリがそのプラットフォームにないアプリは、そのプラットフォームの車には表示されず、ビルド時にその旨が表示されます。Android を搭載した車（Android Automotive OS）には別のアプリが必要で、`platform/car` はそれをビルドしません。

## 動かしてみる

- **CarPlay**：Xcode 26 以前では、iOS シミュレーターの I/O › External Displays › CarPlay で、スマートフォンの横に CarPlay の画面が開きます。Xcode の CarPlay Simulator（Xcode › Open Developer Tool）は、Mac につないだ iPhone の CarPlay 画面を表示します。
- **Android Auto**：Desktop Head Unit（Android SDK の `extras;google;auto`）は、Android Auto アプリの入ったスマートフォンやエミュレーターに対して、車の画面をコンピューター上に表示します。Android Auto の設定でバージョンを 10 回タップして開発者向け設定を開き、「提供元不明のアプリ」（Unknown sources：Google Play 以外から入れたアプリ）をオンにしてヘッドユニットサーバーを起動したら、`adb forward tcp:5277 tcp:5277` を実行して `desktop-head-unit` を起動します。

## テスト

`car.fake()` はテストの中に車の画面を用意します。アプリの `car.Screen` はそこに表示され、テストはドライバーと同じようにその上のものを選べます。

```asuka
test "reserving a charger" {
  let c = car.fake()
  let app = mount(Chargers())
  c.connect()
  await app.idle()
  expect(c.titles() == ["Chargers"])
  try c.select("Main St")
  await app.idle()
  expect(c.texts() == ["Free: 2", "Reserved: No", "Reserve"])
  try c.select("Reserve")
  await app.idle()
  app.expect("Reserved: Main St")
  c.back()
}
```

- `c.connect()` と `c.disconnect()` は、車をつないだり外したりします（`car.connected()` もそれに従います）。
- `c.titles()` は表示中のテンプレートのタイトルで、ルートが先頭です。`c.title()` は一番上のもののタイトルです。
- `c.texts()` は一番上のテンプレートが表示しているものです。行、セル、場所のタイトル、`"title: detail"` の形の情報、メッセージの本文、ボタンのタイトルが並びます。
- `c.select(title)` は、一番上のテンプレートでそのタイトルのものを選びます（タブバーではいずれかのタブの中のもの）。見つからないとき、または無効なときはエラーを投げます。
- `c.back()` は車の戻るボタンです。
