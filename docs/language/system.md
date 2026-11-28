# The system

What a component uses to be part of the app is the prelude's, with no
import: what it registers while it lives, its environment, alerts, and
the app's windows.

| Function | What it does |
|---|---|
| `useKeys([Shortcut(…)])` | the window's keys while the component lives |
| `onOpenURL(u => …)`, `onOpenFiles(paths => …)`, `onShare(items => …)` | what the app is opened with, or what other apps share with it, while the component lives |
| `quit()`, `relaunch()` | quits the app as the user's Quit does (and starts it again), asking it first; desktop only |
| `onCloseRequest(fn() -> bool)`, `onQuit(e => …)`, `onSecondInstance((args, dir) => …)` | asked before the window the component is in closes; hears the app about to quit (`e.cancel()`), or started again, while the component lives |
| `onBack { … }` | asked when the user goes back from the screen, sheet or popover the component is in: `true` takes it (the screen stays), `false` lets it go |
| `onLifecycle(l => …)` | hears the app go to the foreground and the background, and memory warnings (a `Lifecycle`), while the component lives |
| `alert(title, message:, actions:)` | shows an alert now; its `actions` are `.button`s (none: an OK). An `Alert` element shows one while its `open` is true |
| `snackbar(message, action:, duration:, onDismiss:)` | a brief message at the bottom of the window, with an action (Undo); returns a closure that dismisses it |
| `environment()`, `screenEnvironment()` | the size, safe area, text size, platform and the rest (tracked): the screen's or sheet's own, and the whole screen's |
| `window.main()`, `window.current()` | the main window's `Ref`, and the one of the window the code is in: `show()`, `hide()`, `focus()`... from anywhere |
| `window.close()` | closes the window the code is in, without asking |
| `undo.register(name, undo:, redo:)`, `undo.group(name, body)` | records a change the app made, which Edit > Undo, ⌘Z or Ctrl+Z and a phone's shake undo; `undo.undo()`, `undo.redo()`, `canUndo()`, `canRedo()`, `undoName()`, `redoName()` (tracked), `clear()` ([Undo and redo](../desktop.md#undo-and-redo)) |

What the app asks of the system's services is in the [platform
SDK](../stdlib.md)'s packages, imported as any other:

| Function | What it does |
|---|---|
| `device.share(text)` | the share sheet, for a text or a link |
| `try await device.shareFiles(paths, title:, from: ref)` | the share sheet for files, from a view (where a popover points); returns once it's closed |
| `device.haptic(.success)` | the platform's haptic feedback (a `device.Haptic`) |
| `device.copy(text)` | puts text on the clipboard |
| `device.openURL(url)` | opens url in the app that takes it |
| `accessibility.announce(text)` | has a screen reader say text |
| `accessibility.focus(ref)` | moves a screen reader to the view |
| `try await webview.cookies(url)` | the web views' cookies for url |
| `appearance.setColorScheme(.dark)` | the app's appearance: `.light`, `.dark`, or `.system` (the platform's) |

```asuka check
import platform/device

component ShareLink(link: string) {
  Button("Share") {
    on press {
      device.haptic(.light)
      device.share(link)
    }
  }
}
```

What code the app has on each platform is `@platform`'s, a constant
known as the app compiles ([Code for one
platform](control-flow.md#code-for-one-platform)): `environment().platform`
is the one a view runs on, tracked like the rest of its environment.

`useKeys` and the `on…` registrations are items of a component's body
(or the app's block). The app's block also handles them as events: `on
openURL(url) { … }`, `on files(paths) { … }`, `on share(items) { … }`,
`on quit(e) { … }`, `on secondInstance(args, dir) { … }`, `on
foreground { … }`, `on background { … }`. Its first `Window` is the
main window; an app block that shows nothing outside its `Window`s (a
tray icon, a menu bar) has none ([Navigation](../navigation.md#windows)).

`std/embed` compiles the package's files into the app, read as it
compiles, so every platform has them with nothing to load:

```asuka
import std/embed

struct Config { api: string; retries: int }

let welcome = embed.text("welcome.md")         // string
let logo = embed.bytes("art/logo.png")         // bytes
let config: Config = embed.json("config.json") // checked as the app compiles
let posts = embed.texts("posts/*.md")          // [path: text]
let icons = embed.files("icons/**/*.svg")      // [path: bytes]
```

A path is a string literal, relative to the package's directory (where
its `asuka.toml` is), inside it; a glob has `*`, `?`, `[abc]` and `**`
(any directories) and never names a file whose name starts with a dot.
`embed.json` reads the file as `json.decode` does, but as the app
compiles: a file that doesn't fit the type is a compile error, with its
line and column, not one at run time. A file that isn't there, or a
glob that names none, is a compile error too. With `asuka dev`,
changing an embedded file reloads the app in place, as changing its
code does. Files a platform reads as the app runs (images, fonts) go in
`assets/` instead ([std/embed](../std/embed.md)).
