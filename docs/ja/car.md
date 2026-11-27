# 車の画面

`platform/car` を使うと、アプリを車の画面に表示できます。iOS では CarPlay、Android では Android Auto です。車の画面にはアプリ自身のビューは表示されません。運転中の視線は道路に向けておくべきなので、どちらのプラットフォームも自前のテンプレート（リスト、グリッド、タブ、情報、メッセージ、地図上の場所、プレイヤー、ナビゲーションアプリの地図）だけを表示し、車に合わせて描きます。画面の大きさ、ノブかタッチスクリーンか、昼と夜の見た目などです。アプリはテンプレートを値として記述し、その行を選んだときに何をするかを指定するだけで、あとはプラットフォームが受け持ちます。

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
| `.navigation(map:, preview:, trip:, actions:)` | ナビゲーションアプリの地図と、その上の選べるルートや案内（[ナビゲーションアプリ](#ナビゲーションアプリ)） | Apple マップの上の `CPMapTemplate` | 車の画面に描いた地図の上の `NavigationTemplate`、`RoutePreviewNavigationTemplate` |

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

## ナビゲーションアプリ

ナビゲーションアプリの車の画面は、そのアプリの地図です。地図が画面いっぱいに広がり、その上にプラットフォームの案内が重なります。ルートは `.navigation`、カテゴリは `navigation` です。地図に何を表示するかをアプリが値として渡し、描くのはプラットフォームです。ルートはアプリが決めます。車がどこにいるか、この先のルートとその途中の曲がり角をアプリが伝え、プラットフォームがそれを表示します。Asuka はルートを探しません。

```asuka check
import platform/car

let home = car.Coordinate(lat: 35.6812, lng: 139.7671)
let office = car.Coordinate(lat: 35.6858, lng: 139.7745)
let path = [home, car.Coordinate(lat: 35.6830, lng: 139.7700), office]

component Drive {
  state guiding = false
  state position = home

  fn preview() -> car.Preview? {
    if guiding {
      return none
    }
    return car.Preview(destination: "Office", at: office, routes: [
      car.Route(title: "Fastest", distance: 1200, time: 5min, path: path, run: () => { guiding = true }),
    ])
  }

  fn trip() -> car.Trip? {
    if !guiding {
      return none
    }
    return car.Trip(
      destination: "Office",
      maneuvers: [car.Maneuver(instruction: "Turn right onto Main St", turn: .right, distance: 120)],
      distance: 1200,
      time: 5min,
      stop: () => { guiding = false },
    )
  }

  car.Screen {
    root: .navigation(
      map: car.Map(position: position, heading: 45, route: if guiding { path } else { [] }, markers: [car.Marker(at: office, title: "Office")]),
      preview: preview(),
      trip: trip(),
      actions: if guiding { [.button(title: "End", run: () => { guiding = false })] } else { [] },
    )
  }

  Text(if guiding { "Guiding" } else { "Choosing a route" })
}

app { Drive() }
```

```toml
[car]
category = "navigation"
```

### 地図

`car.Map` は地図に表示するものです：

| フィールド | 内容 |
|---|---|
| `position` | 車のいる場所（`platform/location` から）。現在地のマークとして描かれます。 |
| `heading` | 車の進む方向。北からの角度です。 |
| `follow` | 地図が車を中央に保つかどうか（デフォルトは `true`）。進行方向が上になるように回ります。 |
| `center` | 車を追わないときに地図が見る場所。 |
| `zoom` | 近さ。Web 地図のズームレベルと同じで、10 は都市、15 は通り（デフォルト）、18 は建物です。 |
| `route` | この先のルート。これらの点を通る線です。 |
| `markers` | ピン（`car.Marker(at:, title:, color:)`）。 |
| `night` | `true` は夜の配色、`false` は昼の配色。省略すると車に従い、車はヘッドライトに合わせて切り替えます。 |
| `tiles` | 地図のタイル。`{z}`、`{x}`、`{y}` を含む URL です。Android Auto ではデフォルトで OpenStreetMap のタイル、CarPlay ではデフォルトで Apple マップで、URL を渡すとそのタイルに替わります。 |

車は、案内に隠れていない部分の中央に置かれます。地図のボタンはプラットフォームが用意します。拡大、縮小、移動（CarPlay の移動用の矢印、Android Auto のパンモードとタッチスクリーンでのドラッグやピンチ）、そして現在地に戻るボタンです。ドライバーが地図を動かすと、現在地に戻るボタンを押すか、アプリ自身が地図を動かす（`center`、`zoom`、`follow` を変える）まで、その位置にとどまります。`actions` は地図の横に並ぶアプリのボタンです（CarPlay ではバーの左右に 2 つずつ、Android Auto ではアクションストリップに 4 つまで）。

CarPlay の地図は Apple マップ（MapKit）で、`CPMapTemplate` の下の CarPlay ウインドウに表示されます。Android Auto には車の画面に使える地図ビューがないので、`platform/car` がそこに地図を描きます。マップタイル（OpenStreetMap のもの。タイルの利用ポリシーに従って端末に 1 週間保存し、地図上に出典を表示します）、ルート、ピン、車を描き、夜は暗くします。利用者の多いアプリでは、自前のタイルサーバーかサービス事業者のもの（`tiles:`）を使ってください。OpenStreetMap のサーバーは軽い利用のためのものです。

### ルートを選ぶ

`preview` は、出発前に目的地までのルートを地図の上に表示します。`car.Preview(destination:, at:, routes:, start:)` で、各ルートは `car.Route(title:, detail:, distance:, time:, path:, run:)` です。車はルートを一覧にし、ドライバーが選んでいるルートを描き、ドライバーが出発するとその `run` を呼びます。そのあとアプリが `trip` を設定します。`start` は出発ボタンのタイトルです（省略すると CarPlay では CarPlay 自身のもの、Android Auto ではルートのタイトルに矢印を添えたもの）。CarPlay では `CPTrip` のルートの選択肢として、Android Auto では `RoutePreviewNavigationTemplate` として表示されます。

### 案内

`trip` が設定されている間、車はドライバーを案内します。`car.Trip(destination:, maneuvers:, distance:, time:, rerouting:, stop:)` です。`maneuvers` はこの先の曲がり角で、近いものから順に並び、それぞれが `car.Maneuver(instruction:, turn:, road:, distance:, time:, exit:)` です。読み上げる内容（"Turn right onto Main St"）、矢印（`turn`：`.left`、`.slightRight`、`.uTurn`、`exit` 付きの `.roundabout`、`.arrive` など）、そこまでの距離と時間を持ちます。`distance` と `time` は目的地までの残りで、車はそこから到着時刻を表示します。車が進むたびに更新してください。プラットフォームは表示をその場で更新します。

- `rerouting: true` は新しいルートを探している最中であることを示し、曲がり角の代わりにそう表示されます。
- `stop` は、車が自分で案内を終えたときに呼ばれます。別のナビゲーションアプリが案内を始めたときや、ドライバーが車の側で案内を終えたときです。そのときは案内をやめてください（`trip` を none にします）。

CarPlay では、案内は `CPNavigationSession`（その `CPManeuver` と `CPTravelEstimates`。メーターパネルにも表示されます）です。Android Auto では `NavigationTemplate` のルート情報と所要時間の見積もり、そして `NavigationManager` が車に伝える案内です。

### アラート

`car.alert` は、ナビゲーションアプリがこの先の状況を知らせるように、地図の上にアラートをすぐに表示します：

```asuka
let close = car.alert(
  title: "Traffic ahead",
  detail: "10 min slower",
  symbol: "exclamationmark.triangle",
  actions: [.button(title: "Reroute", run: () => reroute())],
  duration: 8s,
  onDismiss: () => {},
)
```

アラートは時間が来ると消えます。ドライバーがボタンを選んだときにも消え、そのボタンの `run` が呼ばれます。ボタンを選ばずに消えたときは `onDismiss` が呼ばれます。ボタンは 2 つまでで、ないときは「OK」が 1 つ表示されます。新しいアラートは表示中のものと入れ替わり、`car.alert` が返す関数でアラートを消せます。CarPlay では `CPNavigationAlert`、Android Auto では `Alert`（Car App API レベル 5。それより前はタイトルのトースト）が表示されます。

`examples/navigation-car` は、東京を走るシミュレーションの車を、CarPlay と Android Auto でターンバイターン案内します。

## カテゴリ：asuka.toml の [car]

車は、そのプラットフォームが認めるカテゴリのアプリだけを表示します。カテゴリごとに使えるテンプレートがあり、Apple と Google はアプリごとにカテゴリを審査します。アプリのカテゴリは `asuka.toml` の `[car]` に書きます。

| `category` | CarPlay | Android Auto |
|---|---|---|
| `audio` | オーディオ（`com.apple.developer.carplay-audio`） | メディア：メディアブラウザー |
| `navigation` | ナビゲーション（`com.apple.developer.carplay-maps`） | ナビゲーション |
| `communication` | コミュニケーション | メッセージ |
| `charging` | EV 充電 | スポット（POI） |
| `parking` | 駐車場 | スポット（POI） |
| `fueling` | 給油 | スポット（POI） |
| `quickFood` | クイックフードの注文 | （なし） |
| `drivingTask` | 運転タスク | （なし） |
| `pointOfInterest` | （なし） | スポット（POI） |
| `iot` | （なし） | IoT |
| `weather` | （なし） | 天気 |

`minCarApiLevel` は、アプリのテンプレートが Android Auto で必要とする Car App API のレベルです（デフォルトは 1。タブには 6、ナビゲーションアプリのアラートには 5 が必要）。

各プラットフォームに必要なものはカテゴリから決まります。

- **iOS**：そのカテゴリの CarPlay の entitlement と、`Info.plist` の CarPlay シーン（`platform/car` が受け持ちます）。entitlement は申請すると Apple が付与します（[CarPlay](https://developer.apple.com/carplay/)）。アプリの App ID に追加すると、プロビジョニングプロファイルにも含まれます。これがないまま署名付きビルドを作ると、その旨が表示されます。シミュレーター用のビルドには常に含まれます。
- **Android**：Car App Library の `CarAppService` とそのカテゴリ、Android Auto の `automotive_app_desc.xml`、そして Car App API のレベル（オーディオアプリでは代わりにメディアブラウザー）。ナビゲーションアプリは、車の画面に描く権限とナビゲーション用テンプレートの権限（`androidx.car.app.ACCESS_SURFACE`、`androidx.car.app.NAVIGATION_TEMPLATES`）も求めます。デバッグビルドは Desktop Head Unit を含むどの車載ホストにも表示されますが、リリースビルドは Google が挙げる車載ホストにだけ表示されます。アプリの車のカテゴリは、車に届く前に Google Play の審査を受けます。

カテゴリがそのプラットフォームにないアプリは、そのプラットフォームの車には表示されず、ビルド時にその旨が表示されます。Android を搭載した車（Android Automotive OS）には別のアプリが必要で、`platform/car` はそれをビルドしません。

## 動かしてみる

- **CarPlay**：Xcode 26 以前では、iOS シミュレーターの I/O › External Displays › CarPlay で、スマートフォンの横に CarPlay の画面が開きます。Xcode の CarPlay Simulator（Xcode › Open Developer Tool）は、Mac につないだ iPhone の CarPlay 画面を表示します。
- **Android Auto**：Desktop Head Unit（Android SDK の `extras;google;auto`）は、Android Auto アプリの入ったスマートフォンに対して（エミュレーターのシステムイメージには Android Auto アプリがないので、スマートフォンを使います）、車の画面をコンピューター上に表示します。Android Auto の設定でバージョンを 10 回タップして開発者向け設定を開き、「提供元不明のアプリ」（Unknown sources：Google Play 以外から入れたアプリ）をオンにしてヘッドユニットサーバーを起動したら、`adb forward tcp:5277 tcp:5277` を実行して `desktop-head-unit` を起動します。ナビゲーションアプリの地図もそこに表示され、ヘッドユニットのタッチスクリーンでドラッグやピンチができます。

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
- `c.texts()` は一番上のテンプレートが表示しているものです。行、セル、場所のタイトル、`"title: detail"` の形の情報、ナビゲーションの地図のルートのタイトルと案内中の曲がり角の指示、メッセージの本文、ボタンのタイトルが並びます。
- `c.select(title)` は、一番上のテンプレートでそのタイトルのものを選びます（タブバーではいずれかのタブの中のもの）。見つからないとき、または無効なときはエラーを投げます。
- `c.back()` は車の戻るボタンです。
- `c.map()` はルートが表示しているナビゲーションの地図で、アプリが渡した値そのものです（ルートが `.navigation` でなければ none）。
- `c.alert()` は表示中のアラートの内容です。タイトル、詳細、ボタンのタイトルが並びます（なければ `[]`）。`c.select(title)` はまずアラートのボタンから探し、`c.dismissAlert()` は時間切れのようにボタンを選ばずにアラートを消します。
- `c.endTrip()` は車が自分で案内を終えることにあたり、案内の `stop` が呼ばれます。
