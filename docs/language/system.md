# The system

| Function | What it does |
|---|---|
| `share(text)` | the share sheet, for a text or a link |
| `try await shareFiles(paths, title:, from: ref)` | the share sheet for files, from a view (where a popover points); returns once it's closed |
| `haptic(.success)` | the platform's haptic feedback (a `Haptic`) |
| `copy(text)` | puts text on the clipboard |
| `openURL(url)` | opens url in the app that takes it |
| `announce(text)` | has a screen reader say text |
| `a11yFocus(ref)` | moves a screen reader to the view |
| `try await webCookies(url)` | the web views' cookies for url |
| `useKeys([Shortcut(…)])` | the window's keys while the component lives |
| `onOpenURL(u => …)`, `onOpenFiles(paths => …)`, `onShare(items => …)` | what the app is opened with, or what other apps share with it, while the component lives |
| `quit()`, `relaunch()` | quits the app as the user's Quit does (and starts it again), asking it first; desktop only |
| `closeWindow()` | closes the window the code is in, without asking |
| `onCloseRequest(fn() -> bool)`, `onQuit(e => …)`, `onSecondInstance((args, dir) => …)` | asked before the window the component is in closes; hears the app about to quit (`e.cancel()`), or started again, while the component lives |
| `onBack { … }` | asked when the user goes back from the screen, sheet or popover the component is in: `true` takes it (the screen stays), `false` lets it go |
| `onLifecycle(l => …)` | hears the app go to the foreground and the background, and memory warnings (a `Lifecycle`), while the component lives |
| `alert(title, message:, actions:)` | shows an alert now; its `actions` are `.button`s (none: an OK). An `Alert` element shows one while its `open` is true |
| `snackbar(message, action:, duration:, onDismiss:)` | a brief message at the bottom of the window, with an action (Undo); returns a closure that dismisses it |
| `setColorScheme(.dark)` | the app's appearance: `.light`, `.dark`, or `.system` (the platform's) |
| `environment()`, `screenEnvironment()` | the size, safe area, text size, platform and the rest (tracked): the screen's or sheet's own, and the whole screen's |
| `mainWindow()` | the main window's `Ref`: `show()`, `hide()`, `focus()`... from anywhere |

What code the app has on each platform is `@platform`'s, a constant
known as the app compiles ([Code for one
platform](control-flow.md#code-for-one-platform)): `environment().platform`
is the one a view runs on, tracked like the rest of its environment.

`useKeys` and the `on…` registrations are items of a component's body
(or the app's block). The app's block also handles them as events: `on
openURL(url) { … }`, `on files(paths) { … }`, `on share(items) { … }`,
`on quit(e) { … }`, `on secondInstance(args, dir) { … }`, `on
foreground { … }`, `on background { … }`. Its first `Window` is the
main window ([Navigation](../navigation.md#windows)).

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
