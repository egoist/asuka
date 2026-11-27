# 车机屏幕

借助 `platform/car`，应用可以显示在汽车的屏幕上：iOS 上是 CarPlay，Android 上是 Android Auto。车机屏幕不会显示应用自己的视图。驾驶时视线应该留在路上，所以两个平台都只显示自己的模板（列表、网格、标签页、信息、消息、地图上的地点、播放器、导航应用的地图），并按照汽车的要求来绘制：屏幕尺寸、旋钮还是触摸屏、白天和夜间的外观。应用只需把模板描述成值，并说明选中其中的行时要做什么，其余的交给平台。

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

应用还要在 `asuka.toml` 里说明自己在车上属于哪一类，也就是它的类别（[类别](#类别asukatoml-的-car)）：

```toml
[car]
category = "charging"
```

## 在车机屏幕上显示

`car.Screen` 挂载期间，应用就会显示在车机上；它放在应用里的什么位置都可以，在手机上什么也不显示。它的 `root` 是车机屏幕一开始显示、按返回最终回到的模板。它像视图一样跟随自己读取的状态：上面的例子里 `reserved` 一变，车机上的信息模板就会原地更新。

- `car.push(() => template)` 把一个模板压到当前显示的内容之上，车上的返回键会再把它移走。它接受的是一个函数，这样被压入的模板同样会跟随它读取的状态。
- `car.pop()` 返回一层，`car.popToRoot()` 回到根模板，`car.depth()` 给出当前有几层模板。
- `car.connected()` 表示车机屏幕上此刻是否显示着应用。它是被追踪的：在视图里读取它，视图就会随着汽车的连接和断开而更新。

汽车可以在手机上没有任何界面的情况下启动应用：比如插上手机后在车机上选中这个应用。这时应用的代码照常运行，它的视图在没有屏幕的情况下创建，所以其中任何位置的 `car.Screen` 都会显示出来。汽车断开时，压入的模板随之清空，下一次连接从根模板开始。同时挂载了多个 `car.Screen` 时，显示最后挂载的那个。

## 模板

| 模板 | 显示什么 | CarPlay | Android Auto |
|---|---|---|---|
| `.list(title:, items:, sections:, actions:, empty:)` | 若干行：先是 `items`，再是带标题的 `sections`；没有行时显示 `empty` | `CPListTemplate` | `ListTemplate` |
| `.grid(title:, items:, actions:)` | 若干格子，每个是一张图片加标题 | `CPGridTemplate`（最多 8 个） | `GridTemplate` |
| `.tabs(tabs)` | 标签页（`car.Tab(title:, symbol:, content:)`），每页是一个列表、网格、信息或地点模板 | `CPTabBarTemplate` | `TabTemplate`（Car App API 等级 6；更低的等级只显示第一页） |
| `.info(title:, fields:, actions:)` | 若干条信息，每条一个标签加一个值，下方有按钮 | `CPInformationTemplate` | `PaneTemplate` |
| `.message(title:, text:, symbol:, actions:)` | 一条消息，下方有按钮 | `CPInformationTemplate` | `MessageTemplate` |
| `.places(title:, places:)` | 地图上的地点，旁边列出它们 | `CPPointOfInterestTemplate` | `PlaceListMapTemplate` |
| `.nowPlaying` | 正在播放的内容及其控制按钮 | `CPNowPlayingTemplate` | Android Auto 自带的播放器 |
| `.navigation(map:, preview:, trip:, actions:)` | 导航应用的地图，上面显示可选的路线或行程的导航提示（[导航应用](#导航应用)） | Apple 地图之上的 `CPMapTemplate` | 车机画面上绘制的地图之上的 `NavigationTemplate`、`RoutePreviewNavigationTemplate` |

和 `Action` 一样，省略的字段取零值：`.list(title: "Recent", items: recent)` 就没有分组，也没有按钮。汽车能显示多少就显示多少：CarPlay 大约十几行，Android Auto 则取决于汽车允许的数量（有些车在行驶中只显示六行），其余的会被省略。所以把最有用的放在前面。

### 行、格子和按钮

列表里的一行、网格里的一格都是一个 `car.Item`：

| 字段 | 含义 |
|---|---|
| `title` | 标题。 |
| `detail` | 标题下方的一行文字。 |
| `image` | 一张图片：资源文件（`"covers/spring.jpg"`）、文件路径，或 http(s) URL。 |
| `symbol` | 没有图片时使用的符号名，和 `Symbol` 的一样。 |
| `browsable` | 选中后会显示更多内容（会压入模板）：行尾显示一个箭头。 |
| `playing` | 它就是正在播放的内容：CarPlay 会标出来。 |
| `progress` | 已播放的比例，从 0 到 1：CarPlay 在行下方显示进度条。 |
| `enabled` | 设为 `false` 时仍会显示，但不能选中。 |
| `run` | 选中后要做的事。 |

模板的 `actions` 是它的按钮，写成 `Action`：`.button(title:, icon:, role:, disabled:, run:)`。它们显示在列表或网格的顶栏上，或者信息、消息模板的文字下方。`section` 里的按钮会被展开进来；子菜单则不会（车上没有子菜单）。

地点（`car.Place`）有 `title`、`detail`、`latitude` 和 `longitude`、以米为单位的 `distance`（按车上使用的单位显示），以及 `run`。Android Auto 列出一个选中后不压入任何内容的地点时，必须带上距离；没有距离的地点会显示成一行选中后压入内容的行。

## 音频应用和正在播放

音频应用用 `platform/nowplaying` 显示正在播放的内容，和锁屏上一样：车上的播放器显示的是同一份信息，它的按钮会调用 `NowPlaying` 的处理函数（或者它的 `player` 的播放、暂停和跳转）。像 CarPlay 上的音频应用那样，在某一行开始播放时压入 `.nowPlaying`：

```asuka
car.Item(title: track.title, image: track.cover, playing: track == current, run: () => {
  play(track)
  car.push(() => .nowPlaying)
})
```

在 Android Auto 上，音频应用不使用模板：Android Auto 用它自带的浏览界面和播放器来显示媒体应用。`platform/car` 根据模板生成这个浏览界面的树：根节点是根模板的各行（标签栏的各个标签页会成为 Android Auto 的标签页），一个 `browsable` 行的子节点，就是选中它之后压入的模板里的各行。不是 browsable 的行可以播放：选中它会调用它的 `run`，由它来播放。Android Auto 的播放器显示应用的 `NowPlaying`，并把命令发回来，和通知栏里的控制按钮一样。信息、消息和地点模板在媒体浏览界面里没有对应的位置。

`examples/car` 是一个同时支持 CarPlay 和 Android Auto 的音频应用。

## 导航应用

导航应用在车机屏幕上显示的是它的地图：地图铺满整个屏幕，平台的导航提示叠在上面。它的根模板是 `.navigation`，类别是 `navigation`。应用把地图要显示的内容写成值，由平台来绘制。路线由应用自己规划：应用告诉平台车在哪里、前方的路线以及路上的每个转弯，平台负责显示。Asuka 不负责计算路线。

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

### 地图

`car.Map` 描述地图上显示的内容：

| 字段 | 含义 |
|---|---|
| `position` | 车所在的位置（来自 `platform/location`），画成一个定位点。 |
| `heading` | 车的行驶方向，以正北为 0 的角度。 |
| `follow` | 地图是否让车保持在中央（默认 `true`），并旋转到行驶方向朝上。 |
| `center` | 不跟随车辆时地图看向的位置。 |
| `zoom` | 远近，和网页地图的缩放级别一样：10 是一座城市，15 是街道（默认），18 是建筑。 |
| `route` | 前方的路线，一条穿过这些点的线。 |
| `markers` | 图钉（`car.Marker(at:, title:, color:)`）。 |
| `night` | `true` 用夜间配色，`false` 用白天配色；省略时跟随汽车，汽车会随车灯切换。 |
| `tiles` | 地图瓦片，一个带 `{z}`、`{x}` 和 `{y}` 的 URL：Android Auto 上默认是 OpenStreetMap 的；CarPlay 上默认是 Apple 地图，给出 URL 时改用它的瓦片。 |

车始终位于导航提示没有遮住的区域的中央。平台会给地图加上自己的按钮：放大、缩小、平移（CarPlay 的平移箭头，Android Auto 的平移模式以及触摸屏上的拖动和双指缩放），以及回到当前位置。司机移动地图后，地图会停在司机移到的地方，直到按下回到当前位置，或者应用自己移动了地图（换了 `center`、`zoom` 或 `follow`）。`actions` 是应用放在地图旁的按钮（CarPlay 的顶栏两侧各两个，Android Auto 的操作栏最多四个）。

CarPlay 上的地图是 Apple 地图（MapKit），显示在 `CPMapTemplate` 下方的 CarPlay 窗口里。Android Auto 没有能用在车机画面上的地图视图，所以由 `platform/car` 自己在上面绘制地图：地图瓦片（OpenStreetMap 的，按照它的瓦片使用政策在设备上保存一周，并在地图上注明出处）、路线、图钉和车辆，夜间会调暗。用户较多的应用请使用自己的或服务商的瓦片服务器（`tiles:`）：OpenStreetMap 的服务器只适合少量使用。

### 选择路线

`preview` 在出发前把前往目的地的几条路线显示在地图上：一个 `car.Preview(destination:, at:, routes:, start:)`，每条路线是一个 `car.Route(title:, detail:, distance:, time:, path:, run:)`。汽车会列出这些路线，绘制司机当前选中的那条，司机出发时调用它的 `run`；接着由应用设置 `trip`。`start` 是出发按钮的标题（省略时 CarPlay 用自己的标题；Android Auto 上用路线的标题加一个箭头）。CarPlay 把它们显示成 `CPTrip` 的路线选项，Android Auto 显示成 `RoutePreviewNavigationTemplate`。

### 导航

设置了 `trip` 时，汽车会引导司机：一个 `car.Trip(destination:, maneuvers:, distance:, time:, rerouting:, stop:)`。`maneuvers` 是前方的转弯，最近的在前，每个是一个 `car.Maneuver(instruction:, turn:, road:, distance:, time:, exit:)`：要说的话（"Turn right onto Main St"）、箭头（`turn`：`.left`、`.slightRight`、`.uTurn`、带 `exit` 的 `.roundabout`、`.arrive` 等），以及离它还有多远、多久。`distance` 和 `time` 是到目的地还剩的距离和时间，汽车会据此显示预计到达时间。车辆行驶时随时更新它们：平台会原地更新显示的内容。

- `rerouting: true` 表示正在重新规划路线，这时不显示转弯。
- `stop` 在汽车自己结束行程时调用：另一个导航应用开始导航，或者司机在车上结束了导航。这时请停止引导（把 `trip` 设为 none）。

在 CarPlay 上，一次行程就是一个 `CPNavigationSession`（它的 `CPManeuver` 和 `CPTravelEstimates`，仪表盘上也会显示）；在 Android Auto 上，是 `NavigationTemplate` 的路线信息和行程预估，以及 `NavigationManager` 告诉汽车的行程。

### 提醒

`car.alert` 会立即在地图上显示一个提醒，就像导航应用提示前方情况那样：

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

它会在时间到了之后消失，或者在司机选择某个按钮时消失，并调用那个按钮的 `run`；没有选择按钮就消失时调用 `onDismiss`。最多两个按钮；没有按钮时显示一个"好"。新的提醒会替换正在显示的那个，`car.alert` 返回的函数可以把它移走。CarPlay 上显示 `CPNavigationAlert`；Android Auto 上显示 `Alert`（Car App API 等级 5；更低的等级只把标题显示成一条 toast）。

`examples/navigation-car` 模拟一辆在东京行驶的车，在 CarPlay 和 Android Auto 上逐向导航。

## 类别：asuka.toml 的 [car]

汽车只会显示其平台允许的类别的应用，每个类别有各自能用的模板，Apple 和 Google 也会审核每个应用的类别。`asuka.toml` 的 `[car]` 说明应用的类别：

| `category` | CarPlay | Android Auto |
|---|---|---|
| `audio` | 音频（`com.apple.developer.carplay-audio`） | 媒体：一个媒体浏览服务 |
| `navigation` | 导航（`com.apple.developer.carplay-maps`） | 导航 |
| `communication` | 通讯 | 消息 |
| `charging` | 电动汽车充电 | 地点（POI） |
| `parking` | 停车 | 地点（POI） |
| `fueling` | 加油 | 地点（POI） |
| `quickFood` | 快餐点单 | （无） |
| `drivingTask` | 驾驶任务 | （无） |
| `pointOfInterest` | （无） | 地点（POI） |
| `iot` | （无） | 物联网 |
| `weather` | （无） | 天气 |

`minCarApiLevel` 是应用的模板在 Android Auto 上需要的 Car App API 等级（默认 1；标签页需要 6，导航应用的提醒需要 5）。

每个平台需要的东西都由类别决定：

- **iOS**：该类别的 CarPlay entitlement，以及 `Info.plist` 里的 CarPlay 场景，由 `platform/car` 接管。Apple 会根据申请授予这项 entitlement（[CarPlay](https://developer.apple.com/carplay/)）：把它加到应用的 App ID 上，描述文件里就会带上它；签名构建缺少它时会提示你。模拟器构建则总是带有它。
- **Android**：Car App Library 的 `CarAppService` 及其类别、Android Auto 的 `automotive_app_desc.xml`，以及 Car App API 等级（音频应用则改为它的媒体浏览服务）。导航应用还会申请在车机画面上绘制和使用导航模板的权限（`androidx.car.app.ACCESS_SURFACE`、`androidx.car.app.NAVIGATION_TEMPLATES`）。调试构建可以显示在任何车机宿主上，包括 Desktop Head Unit；发布构建只显示在 Google 列出的车机宿主上。应用的车机类别在进入汽车之前要经过 Google Play 的审核。

如果应用的类别在某个平台上不存在，它就不会显示在那个平台的汽车上，构建时也会提示。内置 Android 的汽车（Android Automotive OS）需要单独的应用，`platform/car` 不会构建它。

## 试一试

- **CarPlay**：在 Xcode 26 及更早的版本里，iOS 模拟器的 I/O › External Displays › CarPlay 会在手机旁边打开一个 CarPlay 屏幕。Xcode 的 CarPlay Simulator（Xcode › Open Developer Tool）可以为连接到 Mac 的 iPhone 显示 CarPlay 屏幕。
- **Android Auto**：Desktop Head Unit（Android SDK 中的 `extras;google;auto`）会在电脑上为装有 Android Auto 应用的手机显示一块车机屏幕（模拟器的系统镜像里没有 Android Auto 应用，所以请用手机）：在 Android Auto 的设置里连续点击版本号十次打开开发者设置，打开 Unknown sources（允许不是来自 Google Play 的应用），并启动 head unit server，然后运行 `adb forward tcp:5277 tcp:5277` 和 `desktop-head-unit`。导航应用的地图也会显示在那里，可以在 head unit 的触摸屏上拖动和缩放。

## 测试

`car.fake()` 在测试里放一块车机屏幕：应用的 `car.Screen` 会显示在上面，测试可以像司机一样选择上面的内容。

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

- `c.connect()` 和 `c.disconnect()` 模拟插上汽车和拔掉汽车（`car.connected()` 会随之变化）。
- `c.titles()` 是当前显示的各个模板的标题，根模板在前；`c.title()` 是最上层那个的标题。
- `c.texts()` 是最上层模板显示的内容：各行、各格子和各地点的标题，写成 `"title: detail"` 的信息字段，导航地图上各路线的标题和行程中各转弯的提示，消息正文，以及按钮的标题。
- `c.select(title)` 在最上层模板上选中带有这个标题的内容（在标签栏上是某个标签页里的），找不到或者它被禁用时会抛出错误。
- `c.back()` 相当于车上的返回键。
- `c.map()` 是根模板显示的导航地图，也就是应用给出的值（根模板不是 `.navigation` 时为 none）。
- `c.alert()` 是正在显示的提醒的内容：标题、详情和各按钮的标题（没有提醒时为 `[]`）。`c.select(title)` 会先在提醒的按钮里查找，`c.dismissAlert()` 让提醒不经按钮直接消失，就像时间到了一样。
- `c.endTrip()` 相当于汽车自己结束行程：会调用行程的 `stop`。
