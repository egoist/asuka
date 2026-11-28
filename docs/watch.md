# Apple Watch

An Asuka app runs on Apple Watch with the watch's own controls, as it
runs on a phone with the phone's: SwiftUI's buttons, toggles, sliders,
steppers, pickers and lists, navigation and alerts as watchOS draws
them, the Digital Crown scrolling what scrolls. Asuka lays the app out
as it does everywhere, so a screen written for a phone is the same
screen on the wrist, made to fit.

The same code runs on the watch. A watch app is built from the iOS
app's code, so it's `@platform == .iOS` there, as on an Apple TV, and
your iOS files and branches work as they are. `environment().watch`
tells you the app is on a watch, to show less, bigger.

## Running it

```sh
asuka dev --platform watchos .                # a watch simulator, reloaded on save
asuka build --platform watchos --simulator .  # a release .app for the simulator
asuka build --platform watchos .              # an .ipa of a watch-only app, in dist/watchos
asuka publish --platform watchos .            # built and uploaded to App Store Connect, for TestFlight
```

`asuka dev --platform watchos` runs the app on an Apple Watch
simulator: the one booted, else the newest Apple Watch Series, which it
makes if there's none and boots (`--device` names another). Saves
reload it with its state kept, as on a phone. Xcode installs the watch
simulator with its watchOS platform: Xcode › Settings › Components, or
`xcodebuild -downloadPlatform watchOS`. In the simulator, scroll with
the mouse's wheel or the trackpad to turn the Digital Crown.

`asuka dev` runs on the watch simulator, not on a watch: watchOS keeps
an app from opening the connection to your computer that reloading
needs. Try the app on a watch with a release build, through TestFlight.

## Building and shipping

`asuka build --platform watchos` makes a watch-only app: it installs
from the watch's App Store and runs without an iPhone app (watchOS 10
and later). Its executable runs on every Apple Watch that has watchOS
10, from Series 4 on. The `.ipa` in `dist/watchos` is what App Store
Connect takes for a watch-only app: the watch app, in the small iPhone
app that only carries it, which never shows on the phone. (An app whose
iPhone app carries its watch app is built with the iPhone app's:
[With an iPhone app](#with-an-iphone-app).)

It's signed with two provisioning profiles of your team: a watchOS one
for the watch app, whose ID is the app's with `.watchkitapp`
(`dev.asuka.counter.watchkitapp`), and an iOS one for the app's own ID,
the record's in App Store Connect. Make both at developer.apple.com
(Profiles › +), with App Store distribution unless you pass
`--distribution`. `--profile`, or `asuka.toml`'s `[watch]` `profile`,
picks the watch app's ([asuka.toml](asuka-toml.md#watch)); `ios.profile`
the iPhone app's.

`asuka publish --platform watchos` makes that build and uploads it to
App Store Connect, as `--platform ios` does an iPhone app's
([Publishing](shipping.md#publishing)), with the same flags: `--groups`
gives it to TestFlight's groups, `--notes` says what's new, `--review`
submits it for App Review, `--no-wait` stops once it's uploaded, and
`--file` uploads an `.ipa` built before. A watch-only app is an iOS
app's record on App Store Connect (make one as for an iPhone app, with
the app's ID): its builds, versions and review are the record's iOS
platform's, and the App Store offers it on the watch alone.

The app's icon is `asuka.toml`'s `icon`, which the watch shows in a
circle: keep what matters in its middle.

## Updates of the app's code

A watch app takes updates of its code over the air between App Store
releases, as a phone's does ([Phones: updates of the app's
code](shipping.md#phones-updates-of-the-apps-code)): build it with
`asuka.toml`'s `[updates]`, and publish an update for the watch's
builds.

```sh
asuka build --platform watchos --update      # in dist/watchos/updates: the update, signed, and its manifest
asuka publish --platform watchos --update    # built, and uploaded where [updates] says
```

The watch's updates are its own: its builds' fingerprint is of the
watch's native side (its host, the packages built for watchOS), and
`platform/update` on the watch reads the feed's
`update-watchos-<fingerprint>.json`, so an iPhone and a watch never
take each other's update. Publish one for each platform the change is
for. The update is kept in the app's directory (Application Support),
and a launch of it that ends in its first seconds twice in a row goes
back to the build's code, as on a phone.

## With an iPhone app

An app that has both, an iPhone app and its watch app, says so in
`asuka.toml`:

```toml
[watch]
companion = true
```

`asuka build --platform ios` then builds the watch app too, from the
same code, and puts it in the iPhone app's `Watch` folder: one `.ipa`,
one record in App Store Connect, and `asuka publish --platform ios`
uploads both. Installing the iPhone app offers the watch app on the
paired watch, and the watch app installs from the watch's App Store on
its own too, and runs without the iPhone near. A device build signs it
with a watchOS profile for `<id>.watchkitapp` (`[watch]` `profile`, or
the best match) beside the iPhone app's; `asuka build --platform
watchos --simulator` still builds the watch app alone, for its
simulator.

The two apps talk with `platform/companion` (Apple's WatchConnectivity),
the same calls on each side: on the iPhone the companion is the watch
app, on the watch the iPhone app.

```asuka check
import platform/companion

component Score {
  state score = 0
  state said = ""

  // What the other app sends; a message that waits gets an answer.
  task {
    for await m in companion.messages() {
      said = m.text
      if m.wantsReply { companion.reply(m, text: "{score}") }
    }
  }

  Column {
    Text("Theirs: {companion.context()}")
    Text(said)
    Button("Point") {
      on press {
        score += 1
        // The latest state, for the other app whenever it next runs.
        try? companion.setContext("{score}")
        // A message now, if the other app can answer.
        if companion.reachable() {
          said = (try? await companion.send("point")) ?? ""
        }
      }
    }
  }
}
```

- `companion.status()` (tracked) says whether the other app is there:
  `paired`, `installed`, and `reachable`, whether it can answer a
  message now. `available()` and `reachable()` are its short forms.
- `send(text)` sends a message to the other app, which must be
  reachable, and returns its `reply`. `queue(text)` keeps a message for
  when it next runs, delivered in order even after this app quits.
  `messages()` reads both, those that came before it was read first.
- `setContext(text)` keeps this side's latest state for the other app,
  which `context()` (tracked) has there: only the newest arrives.
- `sendFile(path, metadata:)` sends a file in the background;
  `files()` gives those that come, moved into the app's directory.

Messages are text: encode what they carry with `std/json`. On a device
without a companion (an iPad, Android, the desktops) `available()` is
false and the calls throw `CompanionError.unavailable`; tests install a
fake other app with `companion.fake()`. To try the two together on the
simulators, pair a watch simulator with an iPhone simulator (`xcrun
simctl pair <watch> <iPhone>`), boot both, and install each app's
simulator build: the iPhone app on the iPhone, and its watch app (in
the iPhone app's `Watch` folder) on the watch. The simulators carry
messages and the context, but not `queue`'s messages or `sendFile`'s
files: try those on a watch and an iPhone.

## Complications

A watch's widgets are its complications: on watch faces, and in the
Smart Stack. They're `asuka.toml`'s `widgets` in the sizes a watch has,
`circular`, `rectangular`, `inline` (the lock screen's sizes on an
iPhone) and the watch's `corner`:

```toml
[[widgets]]
kind = "steps"
name = "Steps"
description = "Today's steps"
sizes = ["small", "circular", "rectangular", "corner"]
```

The watch app fills them with `platform/widget`, as the iPhone app does
its widgets: `widget.set("steps", widget.Entry(...))` from the watch
app's code shows on the watch's faces. A `corner` shows the `symbol`
with the `value`, or a gauge of the `progress`, curved along the edge;
an app's own layout (`layout`, `sizes[.corner]`) draws in any size.
A kind with no watch size is the iPhone's alone, and `corner` the
watch's alone.

The CLI builds the watch's widget extension (WidgetKit's, `<watch app's
ID>.widgets`) into the watch app, which shares an App Group with it. A
device build signs it with a watchOS profile for
`<id>.watchkitapp.widgets` (`[watch]` `widgetProfile`, or the best
match), and both profiles need the App Groups capability.

## What the watch shows

Each element is the watch's own control where watchOS has one:

| Component | On Apple Watch |
|---|---|
| `Text`, `RichText` | SwiftUI's text, in the watch's system font at the user's text size; a rich text's styles and links' color, without its inline components |
| `Button` | the watch's button: a rounded capsule, or filled with the accent for `variant: .filled`; a `.plain` button with a `background` of its own is its label alone. No watch button is wider than the screen less its margins: one stretched across a wider column is centered in it |
| `Toggle` | the watch's switch |
| `Slider` | the watch's slider, a minus and a plus; the Digital Crown moves it while it's focused |
| `Stepper` | the watch's stepper, its value between a minus and a plus |
| `TextField` | a field that opens the watch's text input (dictation, scribble, the keyboard) |
| `Picker`, `Wheel`, `Segmented` | a wheel the Digital Crown turns |
| `DatePicker` | the watch's date and time pickers |
| `Image`, `Symbol` | the image, the SF Symbol |
| `Progress`, `ProgressRing`, `Activity` | a bar, a ring, a spinner |
| `Scroll`, `List` | scroll views the Digital Crown scrolls; a `List`'s rows on the watch's platters (`appearance: .bare`: as the app draws them) |
| `Stack` | the watch's navigation: a screen's title at the top, its back button, a swipe from the edge back |
| `Tabs` | pages the Digital Crown turns, top to bottom |
| `Sheet`, `Popover` | a sheet over the whole screen, which its close button or a swipe dismisses |
| alerts, action sheets | the watch's alert and confirmation dialog |
| a screen's bar buttons | its first one, at the top's trailing corner |

What needs a bigger screen, a pointer or a hand on the device doesn't
show: `WebView`, `Video`, `Canvas`, `Shader` and `ColorPicker` take
their space and draw nothing; menus, swipe actions, pull to refresh,
drag and drop, selecting text, pinches, rotations, tooltips, keys and
the keyboard's focus don't happen.

## Adapting to the wrist

`environment().watch` is true on Apple Watch. A watch is glanced at:
show the one thing that matters, big, and leave the rest to the phone.
The safe area keeps the app clear of the watch's rounded corners and
the time, as on a phone it keeps clear of the notch.

```asuka check
component Today(steps: int, goal: int) {
  let watch = environment().watch

  SafeArea {
    gap: if watch { 4 } else { 12 }
    padding: if watch { 4 } else { 16 }
    Text("{steps}") { font: if watch { .title.bold() } else { .largeTitle.bold() } }
    Text("of {goal} steps") { color: .secondaryLabel }
    if !watch {
      Text("Walk a little more each day: the goal follows you.") { font: .footnote }
    }
  }
}
```

## Packages on Apple Watch

The standard library's packages run on the watch, those with native
code too where watchOS has what they call: `std/http` (URLSession),
`std/sqlite` and `std/store`. `std/websocket` doesn't: watchOS keeps
apps from WebSockets and other direct connections outside of audio
streaming, so it throws `unsupported` there.

Of the platform SDK's, those whose services the watch has are built for
it: `platform/device` (its info, the wrist's haptics, `openURL`),
`secure`, `biometric`, `health`, `location`, `audio`, `speech`,
`nowplaying`, `network`, `purchase`, `appleauth`, `notify`, `push`,
`crash`, `widget` (complications) and `companion`. [On Apple
Watch](device.md#on-apple-watch) says where each differs from an
iPhone's. The rest throw `unsupported` there, as on any platform that
lacks what they wrap: check `environment().watch` and do without.

## Native packages on watchOS

A native package's Swift is built for watchOS when its `Package.swift`
names watchOS among its platforms (or names none):

```swift
platforms: [.iOS("16.0"), .watchOS("10.0"), .macOS("11.0")],
```

One that doesn't is left out of the watch's build, and its module
throws `unsupported` there ([Native code](native.md#ios-and-macos-swift)).
Its Swift uses `#if os(watchOS)` where the watch differs: the watch has
no UIKit views, so a package's view (`//asuka:view`) gives a SwiftUI
view (`AnyView`) there, and colors are SwiftUI's. `watchOS.entitlements`
beside `Package.swift` holds what it needs of a watch app.
