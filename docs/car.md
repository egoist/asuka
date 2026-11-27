# Car screens

An app shows on a car's screen through `platform/car`: CarPlay on iOS,
Android Auto on Android. A car's screen doesn't show the app's views.
The driver's eyes belong on the road, so each platform shows only its
own templates (lists, grids, tabs, facts, a message, places on a map,
a player, a navigation app's map) and draws them as the car wants: its
size, its knob or touch screen, its day and night look. The app
describes the templates as values and says what choosing their rows
does; the platform does the rest.

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
| `.navigation(map:, preview:, trip:, actions:)` | A navigation app's map, with routes to choose or a trip's guidance ([Navigation apps](#navigation-apps)) | `CPMapTemplate` over Apple Maps | `NavigationTemplate`, `RoutePreviewNavigationTemplate`, over a map drawn on the car's screen |

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

## Navigation apps

A navigation app's car screen is its map, which fills the screen, with
the platform's guidance over it. Its root is `.navigation`, and its
category `navigation`. The app gives what the map shows as values, and
the platform draws it. The app routes: it says where the car is, the
route ahead and the turns on it, and the platform shows them. Asuka
doesn't find routes.

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

### The map

A `car.Map` is what the map shows:

| Field | What it is |
|---|---|
| `position` | Where the car is (from `platform/location`), drawn as a puck. |
| `heading` | Which way the car heads, in degrees from north. |
| `follow` | Whether the map keeps the car in the middle (default `true`), turned so that its heading is up. |
| `center` | Where the map looks when it doesn't follow the car. |
| `zoom` | How near, as web maps' zoom levels: 10 a city, 15 streets (default), 18 buildings. |
| `route` | The route ahead, a line through its points. |
| `markers` | Pins (`car.Marker(at:, title:, color:)`). |
| `night` | `true` night's colors, `false` day's; left out, the car's, which follow its headlights. |
| `tiles` | The map's tiles, a URL with `{z}`, `{x}` and `{y}`: on Android Auto, OpenStreetMap's by default; on CarPlay, Apple Maps by default, and a URL's tiles instead. |

The car sits in the middle of what the guidance leaves visible. The
platform gives the map its buttons: zoom in and out, pan (CarPlay's pan
arrows, Android Auto's pan mode and its touch screen's drag and pinch),
and recenter. Once the driver moves the map it stays where they took
it, and recenter brings it back, or the app's own move does (another
`center`, `zoom` or `follow`). `actions` are the app's buttons beside
the map (two on each side of CarPlay's bar; four in Android Auto's
action strip).

On CarPlay the map is Apple Maps (MapKit), in the CarPlay window under
the `CPMapTemplate`. Android Auto has no map view for the car's screen,
so `platform/car` draws one there: map tiles (OpenStreetMap's, kept on
the device for a week and attributed on the map, as its tile policy
asks), the route, the pins and the car, darkened at night. For an app
with many users, use a tile server of your own or a provider's
(`tiles:`): OpenStreetMap's servers are for light use.

### Routes to choose

`preview` shows routes to a destination over the map before a trip: a
`car.Preview(destination:, at:, routes:, start:)`, each route a
`car.Route(title:, detail:, distance:, time:, path:, run:)`. The car
lists them, draws the one the driver highlights, and runs its `run`
when the driver starts it; the app then sets `trip`. `start` is the
start button's title (left out: CarPlay's own; on Android Auto, the
route's title with an arrow). CarPlay shows them as `CPTrip`'s route
choices, Android Auto as a `RoutePreviewNavigationTemplate`.

### Guidance

While `trip` is set, the car guides the driver: a
`car.Trip(destination:, maneuvers:, distance:, time:, rerouting:,
stop:)`. Its `maneuvers` are the turns ahead, the next first, each a
`car.Maneuver(instruction:, turn:, road:, distance:, time:, exit:)`:
what's said ("Turn right onto Main St"), its arrow (`turn`: `.left`,
`.slightRight`, `.uTurn`, `.roundabout` with its `exit`, `.arrive` and
the rest), and how far and how long until it. `distance` and `time` are
what's left to the destination; the car says when the car arrives.
Update them as the car drives: the platform changes what shows in
place.

- `rerouting: true` says a new route is coming, instead of the turns.
- `stop` runs when the car ends the trip itself: another navigation app
  started, or the driver ended it from the car. Stop guiding then
  (`trip` none).

On CarPlay a trip is a `CPNavigationSession` (its `CPManeuver`s and
`CPTravelEstimates`, the instrument cluster's too); on Android Auto, a
`NavigationTemplate`'s routing info and travel estimate, and the trip
the `NavigationManager` tells the car.

### Alerts

`car.alert` shows an alert over the map now, as navigation apps say
what's ahead:

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

It goes when its time is up, or when the driver chooses a button, which
runs its `run`; going without one runs `onDismiss`. Two buttons at most;
none, an OK. A new alert replaces the one showing, and the function
`car.alert` returns takes it away. CarPlay shows a `CPNavigationAlert`;
Android Auto an `Alert` (Car App API level 5; before, a toast of the
title).

`examples/navigation-car` drives a simulated car through Tokyo with
guidance, on CarPlay and Android Auto.

## Categories: asuka.toml's [car]

A car shows only the apps of the categories its platform allows, each
with its own templates, and Apple and Google review each app's category.
`asuka.toml`'s `[car]` says the app's:

| `category` | CarPlay | Android Auto |
|---|---|---|
| `audio` | Audio (`com.apple.developer.carplay-audio`) | Media: a media browser |
| `navigation` | Navigation (`com.apple.developer.carplay-maps`) | Navigation |
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
Android Auto (default 1; tabs need 6, a navigation app's alerts 5).

What each platform needs follows from the category:

- **iOS**: the category's CarPlay entitlement and a CarPlay scene in
  `Info.plist`, which `platform/car` takes. Apple grants the entitlement
  on request ([CarPlay](https://developer.apple.com/carplay/)): add it
  to the app's App ID, and its provisioning profile then has it; a
  signed build without it says so. A simulator build has it anyway.
- **Android**: the Car App Library's `CarAppService` with the
  category, Android Auto's `automotive_app_desc.xml`, and the Car App
  API level (an audio app's: its media browser instead). A navigation
  app asks too for the permissions to draw on the car's screen and use
  the navigation templates (`androidx.car.app.ACCESS_SURFACE`,
  `androidx.car.app.NAVIGATION_TEMPLATES`). A debug build
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
  phone with the Android Auto app (the emulator's system images don't
  have it, so use a phone): in Android Auto's settings, tap its version
  ten times for its developer settings, turn on Unknown sources (an app
  not from Google Play) and start the head unit server, then
  `adb forward tcp:5277 tcp:5277` and run `desktop-head-unit`. A
  navigation app's map shows there too; the head unit's touch screen
  pans and pinches it.

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
  places' titles, its fields as `"title: detail"`, a navigation map's
  routes' titles and its trip's instructions, its message, its
  buttons'.
- `c.select(title)` chooses what has that title on the top template
  (on a tab bar, a tab's), and throws when nothing there does, or it's
  disabled.
- `c.back()` is the car's back button.
- `c.map()` is the navigation map the root shows, as the app gave it
  (none: the root isn't `.navigation`).
- `c.alert()` is what the alert showing says: its title, its detail,
  its buttons' titles (`[]`: none). `c.select(title)` chooses its
  buttons first, and `c.dismissAlert()` lets it go without one, as when
  its time is up.
- `c.endTrip()` is the car ending the trip itself: the trip's `stop`
  runs.
