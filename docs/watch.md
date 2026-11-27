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
app that only carries it, which never shows on the phone.

It's signed with two provisioning profiles of your team: a watchOS one
for the watch app, whose ID is the app's with `.watchkitapp`
(`dev.asuka.counter.watchkitapp`), and an iOS one for the app's own ID,
the record's in App Store Connect. Make both at developer.apple.com
(Profiles › +), with App Store distribution unless you pass
`--distribution`. `--profile`, or `asuka.toml`'s `[watch]` `profile`,
picks the watch app's ([asuka.toml](asuka-toml.md#watch)); `ios.profile`
the iPhone app's. Upload the `.ipa` with Transporter.

The app's icon is `asuka.toml`'s `icon`, which the watch shows in a
circle: keep what matters in its middle.

## What the watch shows

Each element is the watch's own control where watchOS has one:

| Component | On Apple Watch |
|---|---|
| `Text`, `RichText` | SwiftUI's text, in the watch's system font at the user's text size; a rich text's styles and links' color, without its inline components |
| `Button` | the watch's button: a rounded capsule, or filled with the accent for `variant: .filled`; a `.plain` button with a `background` of its own is its label alone |
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

Of the platform SDK's, `platform/device`'s `haptic` taps the wrist (a
click for selection, the watch's success, retry and failure taps for
the rest) and its `openURL` opens what the watch can (a phone number, a
map); the rest of the platform SDK isn't built for the watch yet: its
calls throw `unsupported`, as on any platform that lacks what they
wrap. Check `environment().watch` and do without.

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
