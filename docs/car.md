# Car screens

An app shows on a car's screen through `platform/car`: CarPlay on iOS,
Android Auto on Android. A car's screen doesn't show the app's views.
The driver's eyes belong on the road, so each platform shows only its
own templates (lists, grids, tabs, facts, a message, places on a map,
a player) and draws them as the car wants: its size, its knob or touch
screen, its day and night look. The app describes the templates as
values and says what choosing their rows does; the platform does the
rest.

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

The app also says what it is on the car, its category, in `asuka.toml`
([Categories](#categories-asukatomls-car)):

```toml
[car]
category = "charging"
```

## The car's screen

`car.Screen` shows the app on the car while it's mounted, wherever it
is in the app: it shows nothing on the phone. Its `root` is the template
the car's screen starts at and goes back to. It follows what it reads,
as a view does: when `reserved` changes above, the info template on the
car changes in place.

- `car.push(() => template)` puts a template over what shows; the car's
  back button takes it away again. It takes a function, so that the
  template pushed follows what it reads too.
- `car.pop()` goes back one, `car.popToRoot()` to the root, and
  `car.depth()` says how many templates there are.
- `car.connected()` says whether a car's screen shows the app. It's
  tracked: read in a view, the view follows the car as it comes and
  goes.

A car can start the app with no screen on the phone: plugged in, with
the app chosen on the car. The app's code runs then as it does on the
phone, its views made without a screen, so a `car.Screen` anywhere in
them shows. When the car goes, what was pushed goes with it, and the
next car starts at the root. While several `car.Screen`s are mounted,
the last one mounted shows.

## Templates

| Template | What it shows | CarPlay | Android Auto |
|---|---|---|---|
| `.list(title:, items:, sections:, actions:, empty:)` | Rows: `items`, then `sections` with their headers; `empty` with none | `CPListTemplate` | `ListTemplate` |
| `.grid(title:, items:, actions:)` | Cells, an image and a title each | `CPGridTemplate` (8 at most) | `GridTemplate` |
| `.tabs(tabs)` | Tabs (`car.Tab(title:, symbol:, content:)`), each a list, a grid, an info or places | `CPTabBarTemplate` | `TabTemplate` (Car App API level 6; before, the first tab) |
| `.info(title:, fields:, actions:)` | Facts, a label and its value each, with buttons | `CPInformationTemplate` | `PaneTemplate` |
| `.message(title:, text:, symbol:, actions:)` | A message, with buttons | `CPInformationTemplate` | `MessageTemplate` |
| `.places(title:, places:)` | Places on a map, listed beside it | `CPPointOfInterestTemplate` | `PlaceListMapTemplate` |
| `.nowPlaying` | What plays, with its controls | `CPNowPlayingTemplate` | Android Auto's own player |

A field left out is its zero, as an `Action`'s are: `.list(title:
"Recent", items: recent)` has no sections and no buttons. A car shows
as much as it can: CarPlay a dozen rows or so, Android Auto as many as
the car allows (six while driving on some), and the rest is left out.
Show the most useful first.

### Rows, cells and buttons

A row of a list or a cell of a grid is a `car.Item`:

| Field | What it is |
|---|---|
| `title` | Its title. |
| `detail` | A line under it. |
| `image` | An image: an asset (`"covers/spring.jpg"`), a file's path, an http(s) URL. |
| `symbol` | A symbol's name, as `Symbol`'s, when there's no image. |
| `browsable` | Choosing it shows more (it pushes): a chevron at its end. |
| `playing` | It's what plays now: CarPlay marks it. |
| `progress` | How much of it was played, from 0 to 1: CarPlay's bar under it. |
| `enabled` | `false`: it shows, and can't be chosen. |
| `run` | What choosing it does. |

A template's `actions` are its buttons, as `Action`s:
`.button(title:, icon:, role:, disabled:, run:)`, in a list's or a
grid's bar, under an info's or a message's text. A `section`'s buttons
are taken in; a submenu isn't (a car has none).

A place (`car.Place`) has its `title`, a `detail`, its `latitude` and
`longitude`, its `distance` in meters (shown in the car's units) and
`run`. Android Auto lists a place that pushes nothing only with its
distance; one without shows as a row that pushes.

## Audio apps and Now Playing

An audio app shows what plays with `platform/nowplaying`, as on the
lock screen: the car's player is the same, and its buttons run the
`NowPlaying`'s handlers (or its `player`'s play, pause and seek). Push
`.nowPlaying` when a row starts playing, as CarPlay's audio apps do:

```asuka
car.Item(title: track.title, image: track.cover, playing: track == current, run: () => {
  play(track)
  car.push(() => .nowPlaying)
})
```

On Android Auto an audio app isn't templated: Android Auto shows media
apps with its own browser and player. `platform/car` makes the
browser's tree from the templates: the root's rows (a tab bar's tabs, as
Android Auto's tabs), and a `browsable` row's rows are those of what
choosing it pushes. A row that isn't browsable is playable: choosing it
runs its `run`, which plays. Android Auto's player shows the app's
`NowPlaying` and sends its commands, as the notification's controls do.
Info, message and places templates have no place in a media browser.

`examples/car` is an audio app on CarPlay and Android Auto.

## Categories: asuka.toml's [car]

A car shows only the apps of the categories its platform allows, each
with its own templates, and Apple and Google review each app's category.
`asuka.toml`'s `[car]` says the app's:

| `category` | CarPlay | Android Auto |
|---|---|---|
| `audio` | Audio (`com.apple.developer.carplay-audio`) | Media: a media browser |
| `communication` | Communication | Messaging |
| `charging` | EV charging | Points of interest |
| `parking` | Parking | Points of interest |
| `fueling` | Fueling | Points of interest |
| `quickFood` | Quick food ordering | (none) |
| `drivingTask` | Driving task | (none) |
| `pointOfInterest` | (none) | Points of interest |
| `iot` | (none) | The Internet of things |
| `weather` | (none) | Weather |

`minCarApiLevel` is the Car App API level the app's templates need on
Android Auto (default 1; tabs need 6). Navigation apps draw their own
map on the car's screen, which `platform/car` doesn't do.

What each platform needs follows from the category:

- **iOS**: the category's CarPlay entitlement and a CarPlay scene in
  `Info.plist`, which `platform/car` takes. Apple grants the entitlement
  on request ([CarPlay](https://developer.apple.com/carplay/)): add it
  to the app's App ID, and its provisioning profile then has it; a
  signed build without it says so. A simulator build has it anyway.
- **Android**: the Car App Library's `CarAppService` with the
  category, Android Auto's `automotive_app_desc.xml`, and the Car App
  API level (an audio app's: its media browser instead). A debug build
  shows on any car host, the Desktop Head Unit's too; a release, on the
  car hosts Google lists. Google Play reviews an app's car category
  before it reaches cars.

An app with a category its platform doesn't have doesn't show on that
platform's cars, and the build says so. Cars with Android built in
(Android Automotive OS) take an app of their own, which `platform/car`
doesn't build.

## Trying it

- **CarPlay**: in Xcode 26 and before, the iOS Simulator's
  I/O › External Displays › CarPlay opens a CarPlay screen beside the
  phone. Xcode's CarPlay Simulator (Xcode › Open Developer Tool) shows
  one for an iPhone connected to the Mac.
- **Android Auto**: the Desktop Head Unit (the Android SDK's
  `extras;google;auto`) shows a car's screen on the computer for a
  phone or emulator with the Android Auto app: in Android Auto's
  settings, tap its version ten times for its developer settings, turn
  on Unknown sources (an app not from Google Play) and start the head
  unit server, then `adb forward tcp:5277 tcp:5277` and run
  `desktop-head-unit`.

## Testing

`car.fake()` puts a car's screen in a test: the app's `car.Screen`
shows on it, and the test chooses what's on it as a driver would.

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

- `c.connect()` and `c.disconnect()` plug a car in and take it away
  (`car.connected()` follows).
- `c.titles()` are the titles of the templates shown, the root first;
  `c.title()` the top one's.
- `c.texts()` is what the top template shows: its rows', cells' and
  places' titles, its fields as `"title: detail"`, its message, its
  buttons'.
- `c.select(title)` chooses what has that title on the top template
  (on a tab bar, a tab's), and throws when nothing there does, or it's
  disabled.
- `c.back()` is the car's back button.
