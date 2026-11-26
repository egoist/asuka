# Asuka documentation

<!-- repository-only:start -->
[简体中文](zh/README.md) · [日本語](ja/README.md)
<!-- repository-only:end -->

Asuka makes apps with each platform's own widgets, on iOS, Android,
macOS, Windows, Linux and the web. Apps are written in **Asuka**, a
language made for interfaces: a component declares what it shows, and
state says what changes. When state changes, only the props that read
it are sent to the platform's views, without diffing a tree.

```asuka
component Counter {
  state count = 0

  SafeArea {
    align: .center; justify: .center; gap: 16

    Text("Count: {count}") { font: .largeTitle }
    Button("Add") {
      variant: .filled
      on press { count += 1 }
    }
  }
}

app { Counter() }

test "counts taps" {
  let app = mount(Counter())
  app.tap("Add")
  app.expect("Count: 1")
}
```

## How it works

Asuka runs the app's components and state, its layout (flexbox and
grid, as the CSS specs say), and the work that waits (async functions,
tasks, timers), and it makes and updates each platform's own views,
what changed once a frame:

| Platform | Its views |
| --- | --- |
| iOS | UIKit |
| Android | Android Views and Material 3 |
| macOS | AppKit |
| Windows | Win32 and the common controls, drawn as Windows 11's |
| Linux | GTK 4 and libadwaita |
| The web | The page's DOM and the browser's own controls |

A text field is a `UITextField`, an `EditText`, an `NSTextField`, an
`EDIT` control, a `GtkEntry` or an `<input>`; a stack of screens is a
`UINavigationController`, Material's app bar with its back gesture, the
Mac window's toolbar with its back button, or libadwaita's navigation
view. So an app that sets no style looks as the platform's apps do, and
gets their accessibility, text input and scrolling as they have them.

While you work, `asuka dev` runs the app on the simulator, an emulator
or a phone, on your computer, or in your browser. A save reaches the
app in milliseconds, and the app keeps its state and its screens. A
release build compiles the app ahead of time with the platform's own
compiler: no interpreter, no garbage collector, no pauses.

The same app runs everywhere. On a phone it fills the screen; on a
desktop it's a window the user resizes, and it can open more windows
and have a menu bar with keyboard shortcuts (ignored on a phone). A
package for something a platform doesn't have (sensors or home screen
widgets on a Mac) answers there that it isn't available, so the app can
check and do without.

Apps run on iOS 16, Android 7 (API 24), macOS 11, Windows 10 and Linux
with GTK 4.14 and libadwaita 1.5, and later; and in current browsers.
The `asuka` CLI is one program, installed with one command ([Getting
started](getting-started.md#install-the-cli)); building for a platform needs that platform's tools (Xcode,
the Android SDK and NDK) and a C compiler (the system's, or Zig).

## Guides

- [Getting started](getting-started.md): install the CLI, make an app
  and run it on a phone, the simulator, your computer or a browser.
- [A tour of Asuka](language-tour.md): the language apps are written in,
  on one page: values, functions, structs and enums, components and
  state, async, errors and tests.
- [Concepts](concepts.md): components and state, what updates, effects
  and tasks, owners, the main thread, and the environment.
- [Layout and style](layout.md): flexbox, sizes, the safe area, colors,
  text, shadows and gradients.
- [Components](components.md): text, buttons, inputs, images, lists,
  video, web views and drawing.
- [Navigation](navigation.md): stacks, tabs, sheets, alerts and menus,
  windows and the menu bar, links into the app, and shares from other
  apps.
- [Motion and gestures](motion.md): animations, transitions, shared
  elements, gestures, keys and haptics.
- [Accessibility](accessibility.md): labels, roles, custom actions,
  announcements, and text that scales.
- [Languages](i18n.md): translated messages with plurals, numbers and
  dates in the user's way, right-to-left layouts, and trying a language.
- [Data](data.md): values kept across launches, SQLite with live
  queries, secure storage, and watching files.
- [Device and services](device.md): the camera, code scanning, photos
  and files, previewing, printing and PDFs, the mail and message
  composers, location, notifications and push, audio, Now Playing and recording,
  network, the media library, work in the background, sign-in and
  biometrics, the screen and the launch screen, maps, the stores'
  services, sensors, image editing, speech, contacts and calendars, home
  screen widgets and shortcuts, and crash reports, on each platform.
- [In-app purchases](purchases.md): consumables, non-consumables and
  subscriptions through the App Store, Google Play and the Microsoft
  Store, transactions, restoring, verifying on a server, and testing.
- [NFC tags](nfc.md): reading and writing tags' NDEF messages, their
  IDs and raw commands, iOS's sheet and Android's reader mode, Web NFC,
  and the tags that open the app.
- [Bluetooth](bluetooth.md): Bluetooth LE devices, scanned for or
  chosen, connected to, read, written and followed; the app as a
  peripheral, advertising and serving; on each platform.
- [iCloud](icloud.md): the key-value store that follows the user to each
  of their devices, and the app's documents in iCloud Drive: listed,
  read and written, downloaded and evicted, their conflicts resolved, on
  iOS and macOS.
- [TVs](tv.md): Apple TV and Android TV: the remote's focus, Back
  and Play/Pause, the controls a TV has, and building for each.
- [The desktop](desktop.md): tray icons and the Dock menu, apps in the
  background, global shortcuts, file dialogs and the file manager,
  opening files, one instance, quitting, recent documents, starting at
  login, updates, the displays, power and progress on the app's icon.
- [Programs](programs.md): command-line tools in Asuka (`fn main`):
  their arguments, flags and stdin, other programs run, an executable
  for each desktop, and the commands an app ships.
- [Packages](packages.md): sharing code between apps: `asuka add`, the
  npm registry, `asuka.lock`, publishing
- [The standard library and the platform SDK](stdlib.md): the `std/...`
  and `platform/...` packages, and what each one does on each platform.
- [C from Asuka](c.md): calling C (the app's own, a library's, the
  system's) with `extern` declarations, `asuka.toml`'s `[c]`, a vendor's
  prebuilt libraries, and callbacks.
- [Native code](native.md): native packages, typed modules and views
  declared in Asuka and implemented in each platform's own project:
  Swift on iOS and macOS, Kotlin on Android, JavaScript on the web, C
  on Windows and Linux.
- [Development](development.md): `asuka dev` in depth on each platform,
  devices, editors and common problems.
- [Testing](testing.md): your app's interface in `test` blocks, with no
  device or display: tap, type and check what the screen shows, and
  time that moves only when the app waits.
- [Shipping](shipping.md): builds for the App Store and Google Play, a
  signed and notarized Mac app, a Windows folder, a Linux archive and
  Flatpak manifest, desktop apps' signed updates, and what
  `asuka.toml` says of the app.
- [The web](web.md): the same app in a browser, as WebAssembly and the
  page's DOM; `asuka dev --platform web`, a static site to deploy,
  embedding, and what it doesn't do yet.

## Reference

- [asuka.toml](asuka-toml.md): every key of an app's `asuka.toml`, its
  type, its default and what it does on each platform.
- [Compared to SwiftUI and Expo](parity.md): what Asuka has, and what it
  doesn't yet.
- [Asuka and MyGo](mygo.md): the platform's widgets or UI drawn by the
  framework, where each runs (MyGo's phones: coming soon), and when to
  use each.
- [Language reference](language/README.md): the whole language, a topic
  a page: types and their built-in methods, declarations, components,
  stores, async, errors, styles, navigation, native code and tests.
