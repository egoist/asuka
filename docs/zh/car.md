# 车机屏幕

借助 `platform/car`，应用可以显示在汽车的屏幕上：iOS 上是 CarPlay，Android 上是 Android Auto。车机屏幕不会显示应用自己的视图。驾驶时视线应该留在路上，所以两个平台都只显示自己的模板（列表、网格、标签页、信息、消息、地图上的地点、播放器），并按照汽车的要求来绘制：屏幕尺寸、旋钮还是触摸屏、白天和夜间的外观。应用只需把模板描述成值，并说明选中其中的行时要做什么，其余的交给平台。

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

## 类别：asuka.toml 的 [car]

汽车只会显示其平台允许的类别的应用，每个类别有各自能用的模板，Apple 和 Google 也会审核每个应用的类别。`asuka.toml` 的 `[car]` 说明应用的类别：

| `category` | CarPlay | Android Auto |
|---|---|---|
| `audio` | 音频（`com.apple.developer.carplay-audio`） | 媒体：一个媒体浏览服务 |
| `communication` | 通讯 | 消息 |
| `charging` | 电动汽车充电 | 地点（POI） |
| `parking` | 停车 | 地点（POI） |
| `fueling` | 加油 | 地点（POI） |
| `quickFood` | 快餐点单 | （无） |
| `drivingTask` | 驾驶任务 | （无） |
| `pointOfInterest` | （无） | 地点（POI） |
| `iot` | （无） | 物联网 |
| `weather` | （无） | 天气 |

`minCarApiLevel` 是应用的模板在 Android Auto 上需要的 Car App API 等级（默认 1；标签页需要 6）。导航应用需要在车机屏幕上自己绘制地图，`platform/car` 不支持这一点。

每个平台需要的东西都由类别决定：

- **iOS**：该类别的 CarPlay entitlement，以及 `Info.plist` 里的 CarPlay 场景，由 `platform/car` 接管。Apple 会根据申请授予这项 entitlement（[CarPlay](https://developer.apple.com/carplay/)）：把它加到应用的 App ID 上，描述文件里就会带上它；签名构建缺少它时会提示你。模拟器构建则总是带有它。
- **Android**：Car App Library 的 `CarAppService` 及其类别、Android Auto 的 `automotive_app_desc.xml`，以及 Car App API 等级（音频应用则改为它的媒体浏览服务）。调试构建可以显示在任何车机宿主上，包括 Desktop Head Unit；发布构建只显示在 Google 列出的车机宿主上。应用的车机类别在进入汽车之前要经过 Google Play 的审核。

如果应用的类别在某个平台上不存在，它就不会显示在那个平台的汽车上，构建时也会提示。内置 Android 的汽车（Android Automotive OS）需要单独的应用，`platform/car` 不会构建它。

## 试一试

- **CarPlay**：在 Xcode 26 及更早的版本里，iOS 模拟器的 I/O › External Displays › CarPlay 会在手机旁边打开一个 CarPlay 屏幕。Xcode 的 CarPlay Simulator（Xcode › Open Developer Tool）可以为连接到 Mac 的 iPhone 显示 CarPlay 屏幕。
- **Android Auto**：Desktop Head Unit（Android SDK 中的 `extras;google;auto`）会在电脑上为装有 Android Auto 应用的手机或 Android 模拟器显示一块车机屏幕：在 Android Auto 的设置里连续点击版本号十次打开开发者设置，打开 Unknown sources（允许不是来自 Google Play 的应用），并启动 head unit server，然后运行 `adb forward tcp:5277 tcp:5277` 和 `desktop-head-unit`。

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
- `c.texts()` 是最上层模板显示的内容：各行、各格子和各地点的标题，写成 `"title: detail"` 的信息字段，消息正文，以及按钮的标题。
- `c.select(title)` 在最上层模板上选中带有这个标题的内容（在标签栏上是某个标签页里的），找不到或者它被禁用时会抛出错误。
- `c.back()` 相当于车上的返回键。
