# TVs

An Asuka app runs on Apple TV and on Android TV (Google TV too) with
the TV's own widgets, as it runs on a phone with the phone's. A TV has
no touch: a remote moves the focus from view to view, and its center
button presses what has it. The platform's focus engine does that,
over the app's views: UIKit's on tvOS, Android's on a TV.

The same code runs on the TV. Apple TV's app is built from the iOS
app's code, so it's `@platform == .iOS` there, as with iPad; Android
TV's is the Android app, one build for phones and TVs.
`environment().tv` tells you the app is on a TV, to make a screen
bigger or simpler for a room's distance.

## Apple TV

```sh
asuka dev --platform tvos .                # an Apple TV simulator, reloaded on save
asuka build --platform tvos --simulator .  # a release .app for the simulator
asuka build --platform tvos .              # an .ipa for Apple TVs, in dist/tvos
asuka publish --platform tvos .            # built and uploaded to TestFlight
```

`asuka dev --platform tvos` runs the app on an Apple TV simulator: the
one booted, else an Apple TV 4K's, which it boots (`--device` names
another). Xcode installs the tvOS simulator with its tvOS platform:
Xcode › Settings › Components, or `xcodebuild -downloadPlatform tvOS`.
In the simulator, the arrow keys are the remote's, Return its center
button and Escape its Menu (Back) button; Window › Show Apple TV Remote
shows the remote itself.

`asuka build --platform tvos` signs the app with a tvOS provisioning
profile for its ID (`--profile` picks one; App Store by default, or
`--distribution`), as an iOS build signs with an iOS one: make one at
developer.apple.com (Profiles › + › tvOS), since the iPhone app's
profile doesn't sign the Apple TV app. Apps run on tvOS 17 and later.

`asuka publish --platform tvos` builds the `.ipa` and uploads it to App
Store Connect, as `--platform ios` does, with the same key and options
(`--groups`, `--review`, `--notes`, `--file`):
[Publishing](shipping.md#publishing). The Apple TV app is a platform of
the iPhone app's record, with its bundle ID and in-app purchases: add
tvOS to the record in App Store Connect first (the app's page, Add
Platform › tvOS).

The app's icon on Apple TV is a layered image, 5 by 3: Asuka makes it
from `asuka.toml`'s `icon`, the icon in the middle of its corner's
color, the two layers apart, so tvOS tilts them as the focus moves
over the icon. The top shelf's images (what the Home screen shows above
the app's icon when it's in the top row) are made the same way.
`[tv]`'s `banner` gives an image of your own for both, and `background`
the color behind the icon ([asuka.toml](asuka-toml.md#tv)).

What tvOS doesn't have, a tvOS build goes without: a share target,
widgets and Live Activities, App Shortcuts, push notifications'
rewriting; and web views (tvOS has no WebKit), notifications that show
(only the app's badge), the photo library and its picker, contacts,
calendars and the mail composer. A package that calls them throws
`unsupported` there, as on any platform that lacks them; check
`environment().tv` and do without. [Packages on Apple
TV](#packages-on-apple-tv) says which work.

## Android TV

Android TV runs the Android app when it says it's a TV app too:

```toml
[tv]
android = true
```

The build then has what Android TV and Google Play look for: the TV
launcher's intent (`LEANBACK_LAUNCHER`), touch and the Leanback library
not required (`uses-feature … required="false"`), so Google Play offers
the one app to phones and TVs, and the banner the TV launcher shows,
320 × 180, made from `icon` (or `tv.banner`).

To try it, make an Android TV emulator in Android Studio's Device
Manager (a TV device, an Android TV or Google TV system image) and run
`asuka dev --platform android --device <its name> .`. The emulator's
arrow keys are the D-pad, Return its center and Escape Back.

## The remote

On both TVs:

- **The focus** moves among the views that take it: the controls and
  fields, and the views that press (`on press`), adjust (`on adjust`)
  or are `focusable`. The platform picks the view the remote's
  direction points to, as in its own apps.
- **The focused view shows it**: tvOS's controls their own focus
  effect; the app's views grow a little and cast a shadow, as tvOS's
  cards do. On Android TV, a view's focused state (a ripple's, a
  button's) and the same lift. A view that styles `focusVisible` draws
  its own instead, and `hideFocusRing: true` takes it away
  ([Accessibility](accessibility.md#views-that-take-the-focus)).
- **Select** (the center button, or a click of the Siri Remote's
  clickpad) presses the focused view: `on press`, a button's action, a
  toggle's flip. A view's `pressed` state shows while it's down.
- **Back** (Android TV's Back, the Siri Remote's Back or Menu) goes
  back as a phone's back does: it closes the sheet, pops the stack,
  and an `onBack` can take it
  ([Navigation](navigation.md#taking-back-yourself)). At the app's
  first screen it leaves the app, to the TV's Home screen.
- **Keys**: a view's `keys` take the remote's directions (`up`, `down`,
  `left`, `right`) ahead of the focus moving, and `playpause`, its
  Play/Pause button: a player's play and pause, a game's moves
  ([Keys](motion.md#keys)).

```asuka check
component Player(title: string) {
  state playing = false

  View {
    padding: if environment().tv { 48 } else { 16 }
    gap: 16
    keys: [Shortcut(key: "playpause", run: () => { playing = !playing })]
    Text(title) { font: .title }
    Button(if playing { "Pause" } else { "Play" }) {
      variant: .filled
      on press { playing = !playing }
    }
  }
}
```

## Controls on a TV

Where the TV has the control, it's the TV's own. Where it doesn't:

| Component | Apple TV | Android TV |
|---|---|---|
| `Toggle` | a button with a check mark (a circle when off) that Select flips; tvOS has no switch | Material's switch |
| `Slider` | a bar that left and right step by a twentieth of its range while it's focused; tvOS has no slider | Material's slider, which left and right step |
| `Stepper` | a minus and a plus, stepped by left and right while focused | Material's buttons |
| `DatePicker` | the date (or time), stepped a day (a minute) at a time by left and right; tvOS has no date picker | Material's date and time pickers |
| `Picker`, `Wheel` | a button whose menu chooses | as on a phone |
| `ColorPicker` | the color, which only the app changes | as on a phone |
| `TextField` | a field typed on tvOS's full-screen keyboard; a multiline one takes a line | as on a phone, with the TV's keyboard |
| `WebView` | none: tvOS has no WebKit | Android's `WebView` |
| `Sheet` | the whole screen, without detents | as on a phone |
| `Popover` | a sheet | as on a phone |
| a screen's `toolbar` | its buttons lead the navigation bar | as on a phone |
| a screen's `search` | its field is the navigation bar's title | as on a phone |

Swipe actions, pull to refresh, drag and drop, selecting text, pinches,
rotations, tooltips and haptics need a touch, a pointer or a hand on
the device: on a TV they don't happen.

## Adapting to a TV

`environment().tv` is true on Apple TV and Android TV. A TV is watched
from across the room and has no touch, so a TV's screen usually shows
fewer, bigger things, in rows and grids the focus moves through, and
leaves out what asks for typing:

```asuka check
component Shelf(items: [string]) {
  let tv = environment().tv

  Scroll {
    horizontal: true
    Row {
      gap: if tv { 48 } else { 12 }
      padding: if tv { 64 } else { 16 }
      for item in items key item {
        View {
          padding: if tv { 32 } else { 12 }
          radius: 12
          background: .secondaryBackground
          on press { print(item) }
          Text(item) { font: if tv { .title2 } else { .body } }
        }
      }
    }
  }
}
```

The safe area keeps the app clear of a TV's edges, which some TVs
crop: tvOS reports it in `environment()`'s insets.

## Packages on Apple TV

The standard library's packages run on Apple TV as on an iPhone, those
with native code too (`std/http`, `std/websocket`, `std/sqlite`,
`std/store`, `std/svg`). Of the platform SDK's, those whose services
tvOS has run there, some with less than on a phone:

| Package | On Apple TV |
|---|---|
| `platform/analytics` | as on an iPhone (Firebase, with `ios.googleServices`) |
| `platform/appearance`, `platform/audio`, `platform/crash`, `platform/document`, `platform/icloud`, `platform/imageedit`, `platform/network`, `platform/periodic`, `platform/speech`, `platform/splash`, `platform/tracking`, `platform/vision` | as on an iPhone |
| `platform/appicon` | as on an iPhone: each of `alternateIcons` is a layered icon too, made as the app's is |
| `platform/appleauth` | as on an iPhone: Sign in with Apple's sheet and button |
| `platform/bluetooth` | a central only: scanning, connecting, reading and writing; `serve` and `advertise` throw `unsupported` |
| `platform/device` | `info`, `openURL`, `openSettings`, `keepAwake`; no browser, photo or document picker (`unsupported`), and no clipboard: nothing to read, and copying throws `unsupported` |
| `platform/googleauth` | `signInWithCode`: the TV shows an address and a code the user enters on their phone or computer, Google's sign-in for TVs; `signIn` throws |
| `platform/integrity` | App Attest, on an Apple TV (not the simulator) |
| `platform/js` | JavaScriptCore, without WebKit: the language and its built-ins, timers too, but no DOM or `fetch` |
| `platform/location` | when in use only, a position at a time: `positions()` asks again every five minutes; no background updates, and regions are computed while the app runs |
| `platform/maps` | Apple Maps, which doesn't rotate or tilt |
| `platform/notify` | the badge alone: `request` asks for it and `setBadge` sets it; `schedule` and `show` throw `unsupported` |
| `platform/nowplaying` | as on an iPhone: Control Center and the remote's play and pause |
| `platform/power` | no battery (`battery` false): the thermal state |
| `platform/purchase` | StoreKit, as on an iPhone; `manageSubscriptions`, `requestRefund` and `redeemCode` throw `unsupported` (tvOS has no such sheets: subscriptions are in Settings) |
| `platform/push` | the device's token; a push sets the badge or reaches the app as data (`received`), and none is opened |
| `platform/screen` | the TV's screen; no brightness to set |
| `platform/secure` | the keychain; a `protected` secret is `SecureError.notAvailable`: an Apple TV has no passcode, Face ID or Touch ID |

The others aren't built for tvOS, for what it lacks: `platform/ads`
(Google's ads SDK has no tvOS), `agerange`, `biometric` (`check` says
there's none), `calendar`, `camera` and `record` (an Apple TV has no
camera or microphone of its own), `compose`, `contacts`, `dialog`,
`health`, `liveactivity`, `nfc`, `pay`, `pdf`, `photos`, `preview`,
`print`, `review` (tvOS has no rating prompt), `scan`, `sensors`,
`sharesheet`, `shortcuts`, `transcribe`, `webauth`, `webview` and
`widget`; and the desktop's (`tray`, `dock`, `hotkey`, ...), as on an
iPhone. Their calls throw `unsupported`, or answer that there's none
(`biometric.check`).

## Native packages on tvOS


A native package's Swift is built for tvOS when its `Package.swift`
names tvOS among its platforms (or names none):

```swift
platforms: [.iOS("16.0"), .tvOS("17.0"), .macOS("11.0")],
```

One that doesn't is left out of the tvOS build, and its module throws
`unsupported` there, as where a package has no code at all
([Native code](native.md#ios-and-macos-swift)). Its Swift uses
`#if os(tvOS)` where tvOS's UIKit differs, and `tvOS.entitlements`
beside `Package.swift` holds what it needs of a TV app.
