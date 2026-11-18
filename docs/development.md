# Development

`asuka dev` runs an app and reloads it as you save: on the iOS simulator
or an iPhone, an Android emulator or phone, your Mac, PC or Linux
machine, or your browser. This page covers the player it runs, what a
reload keeps, devices, assets and logs, editors, and the problems you
may meet. To test an app's interface with no device or display, see
[Testing](testing.md).

## asuka dev

```sh
asuka dev [flags] [dir]                     # this computer: macOS, Windows or Linux
asuka dev --platform ios [flags] [dir]       # the iOS simulator
asuka dev --platform ios --device iphone [dir]  # the iPhone (or iPad) plugged in
asuka dev --platform android [flags] [dir]   # an Android emulator or phone
asuka dev --platform web [flags] [dir]       # your browser
```

`--platform` is one of `ios`, `android`, `mac`, `windows`, `linux` and
`web`; `asuka build` takes it likewise. Without it, the app runs (or
builds) for the computer you're on. One platform per run: run two
terminals for two.

`dir` is the app's directory, the current one by default: its `.asuka`
files, one of which has the `app` block, and its `asuka.toml`. The
flags:

- `--platform`: as above.
- `--lang`: show a language for this run (`de`, `zh-Hans`), or a pseudo
  locale: `en-XA` (accented and longer) or `ar-XB` (right to left).
  `--dir rtl` lays any app out right to left. See
  [Languages](i18n.md).
- `--once`: launch the app once, without watching for changes.
- `-v`: print the commands Asuka runs.

On a phone:

- `--device`: on iOS, a simulator's name or UDID (by default the booted
  iPhone, else a recent iPhone Pro, which it boots), a connected iPhone's
  or iPad's name or UDID, or `iphone` for the one connected (see
  [Devices](#devices)); on Android, an adb serial or an AVD's name (by
  default the first device adb lists, else the first AVD, which it
  starts).
- `--team`: on an iPhone, the Apple Developer team that signs the
  player.

In a browser: `--port`, `--lan` (serve to your network, for a phone's
browser) and `--no-open`; see [the
web](web.md#asuka-dev---platform-web).

## The player

In development, the app runs in a **player**: the app's shell (its
name, ID, icon, `Info.plist` or manifest, its native packages' code),
which `asuka dev` builds once for the platform, installs and launches.
It then sends the player your app; on each save it compiles again (a
few milliseconds) and sends the new version, which the player loads in
place, keeping the app's state, and reports how it went:

```
asuka: compiled in 3.1 ms
asuka: reloaded in 2.2 ms, 4 states kept
```

A save that doesn't compile prints its errors, as `file:line:col:
message`, and shows them over the app (see [Errors over the
app](#errors-over-the-app)); the player keeps running what it had, and
the next save that compiles goes on from there. A change to the rest of the app (its
`asuka.toml`, its assets, its native packages' Swift, Kotlin or
JavaScript) builds the player again, reinstalls it if it changed, and
launches it.

## On the desktop

On macOS, Windows and Linux, `asuka dev` runs the player in a window.
The first run builds the player for this computer, which takes a
moment; later runs start at once. It needs a C compiler: Xcode's command
line tools on a Mac; on Windows and Linux Zig, clang or gcc (`ASUKA_CC`
picks one).

The app reads its `assets/` where they are, so changing one needs no
copy. Its data (SQLite databases, files) is in a `data` directory of its
own in Asuka's cache (see [Starting over](#common-problems)), not the
installed app's, so `asuka dev` and a release don't share it. Its output
and its errors print in the terminal.

**macOS.** The player is an `.app` like a release's, in Asuka's cache:
its `Info.plist` (its name, bundle ID, URL scheme, purpose strings and
languages from `asuka.toml`) and icon, signed ad hoc. So the app is
itself to the system, as when it's opened from the Finder: the Dock
shows its name and icon, notifications are its own, and its privacy
prompts (the camera, location, contacts) ask for it, with its purpose
strings, not for the terminal. Packages that need a provisioning
profile's entitlements (push, Sign in with Apple, App Attest) work only
in a release build signed with one: see [Shipping](shipping.md#macos).

**Windows.** `asuka dev` runs only on Windows itself; `asuka build
--platform windows` builds from any system (with Zig), one `.exe` with
the app in it. The player is a console program, so its output prints in
the terminal; a release has no console.

**Linux.** `asuka dev` on Linux runs the player on GTK 4 and libadwaita,
which it needs installed (see [Getting
started](getting-started.md#what-you-need)), under your session's
display, Wayland or X11. On a Mac or a PC, `asuka dev --platform linux`
runs it only where `ASUKA_LINUX_RUN` names a command that runs a program
on a Linux machine with a display, which sees this computer's files at
the same paths: a VM, as `ASUKA_LINUX_RUN="orb -m ubuntu"` (OrbStack) or
`ASUKA_LINUX_RUN="limactl shell default"` (Lima). Asuka builds the
player for that machine's architecture, which needs Zig, and the player
reaches `asuka dev` over the network: at this computer's first address,
or `ASUKA_DEV_HOST`'s.

## What a reload keeps

A reload keeps every `state` of the app, with no code of yours, on every
platform: each state cell is kept under its component's place in the
view tree and its name (`Inbox.query`, under the screen and the row
that show it), and the new module's cell at the same place, with the
same name, starts from its value. Values are converted to the new
module's types by their fields' and cases' names: a field added starts
from its default, one removed is dropped. A state whose type changed in
a way that can't be converted (an `int` that's now a `string`) starts
again from its initial value. The views are made again from the new
module, showing the state they had: the screen looks as it did, with
the change.

What isn't kept:

- A state whose initial value you edited starts from the new one
  (`state count = 0` changed to `state count = 10`); the other states
  keep theirs. Laying the initial value out differently (spaces, line
  breaks) doesn't count as an edit.
- A state renamed, or moved to another component, starts over; so does
  one whose component moved to another place in the view tree.
- `let` values aren't kept: they're computed again from what they read.
- Tasks and effects start again in the new module (a `task` running
  when you save is cancelled, and the new one runs).

To start the app over, press **r** in the terminal `asuka dev` runs in:
every player loads the app again with none of its state, every `state`
from its initial value, as when it launches, without the player
launching again (`asuka: restarted in 2.0 ms`). Stopping `asuka dev`
and running it again starts the app from nothing too. Either way, files
the app writes (SQLite databases, files) are kept: they're in the app's
data directory, on the device or, on the desktop, in Asuka's cache.

## Errors over the app

What goes wrong while you work shows on the device or in the window,
over the app, not only in the terminal:

- **An error nothing caught**: one a handler, an effect or a task threw
  with no `Boundary` around it, or one the app's `init` threw. The
  overlay shows its message, the lines of your source where it was
  thrown, and the calls that led there, innermost first.
- **A save that doesn't compile**: its first error, the lines at it with
  a caret under the part that's wrong, the error's page, and the other
  errors. It goes by itself as soon as a save compiles, and shows before
  an error of the app's, which came from the code you're changing.

**Dismiss** (or Escape, or Android's back) goes back to the app as it
was: the handler or effect that threw stopped there, everything else
runs on. **Reload**, offered for an error of the app's, starts the app
over as **r** does, every `state` from its initial value. When more
errors come while one shows, the first stays, with how many came after
it: the others usually follow from it. A save that compiles clears the
app's errors with the old code.

The overlay is drawn in each platform's own views, over the app's
screens, sheets and alerts, so it shows even when the app's own views
are what broke. It's only in `asuka dev`: in a build, an error nothing
caught is logged, and a `Boundary` is how the app shows one to its
users.

## Reloading

Asuka watches the app's directory and its local dependencies'
(`file:` in `asuka.toml`):

- `.asuka` files: compiled and reloaded in place;
- `asuka.toml` and `asuka.lock`, files under `assets/`, the language
  catalogs, native packages' Swift, Kotlin and JavaScript, `Info.plist`
  and entitlements: the player is built and launched again (the app
  starts from nothing).

It skips directories starting with `.`, and `testdata`, `build`, `dist`
and `node_modules`.

## Devices

**Your computer**: `asuka dev` alone, on a Mac, a PC or Linux.

**The iOS simulator** needs nothing more: it shares the Mac's network.

**An Android emulator or phone**: `asuka dev` reaches an emulator, or a
phone over USB, through adb. For a phone, turn on USB debugging in its
developer options, plug it in and accept the prompt, and check that
`adb devices` lists it. `asuka dev --platform android` takes the first
device adb lists, phone or emulator; with several, name it with
`--device` and its serial.

**An iPhone** (or iPad), with Xcode 26 or later: plug it in, unlock it
and tap Trust; turn on Developer Mode in its Settings › Privacy &
Security (it shows once the phone has met Xcode; the phone restarts).
Then `asuka dev --platform ios --device iphone` runs on it, or
`--device` with its name or UDID when several are connected (`xcrun
devicectl list devices` lists them).

- **Signing**: the player is signed for development by your team:
  `--team`, else `ASUKA_TEAM_ID`, else `asuka.toml`'s `ios.team`, else
  the team of the Apple Development certificate in your keychain. The
  first run has Xcode make the development profiles it needs,
  registering the phone with
  the team and turning on the capabilities the app uses (push, App
  Groups...), with the Apple ID in Xcode › Settings › Accounts, or with
  an [App Store Connect API key](shipping.md) if `ASUKA_ASC_KEY_ID` (or
  `ios.appStoreConnect`) sets one. A team's profiles that already fit,
  wildcard ones too, are used as they are.
- **Reaching your Mac**: plugged in, the Mac connects to the player
  through the cable, as Xcode does, and the phone needn't be
  on any network. Unplugged, the player connects to `asuka dev` over the
  network, trying each of the Mac's addresses (or `ASUKA_DEV_HOST`), on
  port 8791 when it's free; iOS asks once whether the app may find
  devices on it (allow it; its Settings › Apps › Local Network turns it
  on later). Either way it reconnects by itself when `asuka dev` comes
  back, and a player opened again from the home screen finds it. If the
  player doesn't connect, Asuka says what to check.

**A phone's browser**: `asuka dev --platform web --lan` serves the app to
your network, and prints the addresses to open.

## Assets

On the desktop, the player reads `assets/` in place. On a phone,
they're copied into the player when they change. In the browser, the dev
server serves them. A change to an asset builds and launches the player
again.

## Logs

The player sends its output and its errors to `asuka dev`, which prints
them in the terminal: `print`, uncaught errors (with the calls that led
there, which the app also shows: [Errors over the
app](#errors-over-the-app)), and Asuka's own warnings and errors, on
every platform. A module the player couldn't load
says why (`asuka: the player: ...`). In the browser, the page's errors
print there too.

## Errors and their codes

Every error and warning has a code, the name of the rule it reports a
break of. The terminal says it with the help, when there is some, and
the code's page:

```
app.asuka:14:9: error[write-in-binding]: can't assign to count here
  = help: what the UI follows is computed, not changed; change state in a handler (on press { … }), a fn, a task or an effect
  = see https://asuka.egoist.dev/docs/errors/write-in-binding
```

`asuka explain write-in-binding` prints that page in the terminal, in
your language (`LANG`), for the version installed: why the rule is
there, how to fix what breaks it, a program that breaks it and one that
doesn't. [Error codes](errors/README.md) lists them all.

`asuka check --json` and `asuka test --json` write one JSON object per
line instead, for tools and coding agents: `code`, `severity`, `file`,
`range` (lines and columns from 1), `message`, `help` and `url`, and
`platforms` when it's some platforms' only. The editor shows the code
too, linked to its page.

As the app runs under `asuka dev`, `asuka run` or `asuka test`, the
core reports what it sees go wrong in its state, with codes too: effects
that keep triggering each other (`effect-loop`, with the effect's line
and the state it wrote last), a load that waited for another one it
doesn't use (`load-waterfall`), a load that has shown its last answer
for two seconds with nothing reading its `loading` (`long-hold`), a
write that ran more than 1,000 bindings, twice (`wide-fanout`), and an
effect's write that ran a binding again in the same update
(`effect-in-effect-pass`). Each is said once per place in the code, with
the player's logs. Release builds don't look for them; a crash report
says a panic's code.

## Editors

`asuka lsp` is Asuka's language server: diagnostics as you type,
completion, hover with `///` docs, go to definition, references,
rename, formatting (`asuka fmt`'s), symbols, signature help and
semantic highlighting for `.asuka` files. It checks the package being
edited with your unsaved changes, and the packages that import it.

For VS Code, the extension is in `editors/vscode`: it adds the
language's highlighting and runs `asuka lsp`, the `asuka` on your `PATH`
or the one `asuka.server.path` names.

```sh
cd editors/vscode
npm install
npx @vscode/vsce package
code --install-extension asuka-0.1.0.vsix
```

Any editor with an LSP client can use it too: run `asuka lsp`, which
speaks LSP 3.17 over standard input and output, for `.asuka` files,
with the directory of `asuka.toml` as the root. `editors/vscode`'s
README has Neovim's and Helix's settings.

## Snapshots and scripts

Desktop apps take a few environment variables that make the app
show a state and capture it, for checking a change by eye or in CI. Run
a release build's executable with them, or `asuka dev` (which passes its
environment to the player):

```sh
ASUKA_SNAPSHOT=shot.png ./MyApp.app/Contents/MacOS/MyApp            # the window as a PNG, once it has shown, then quit
ASUKA_SNAPSHOT=shot.png ASUKA_DUMP=1 asuka dev                      # and its view tree, in shot.png.txt
ASUKA_SCRIPT="tap:Add;tap:Add" ASUKA_SNAPSHOT=shot.png asuka dev    # steps first
```

- `ASUKA_SNAPSHOT` writes the window as a PNG once it has shown (and
  the script has run), and quits; `ASUKA_SNAPSHOT_STAY=1` keeps the app
  running. On macOS it's the window server's image of the window, with
  its sheets over it, so materials and Liquid Glass show as they do on
  screen; a file named `*.content.png` is the content alone, without the
  title bar.
- `ASUKA_DUMP` writes the view tree beside it: each view's kind, frame
  and what it shows.
- `ASUKA_SCRIPT` runs steps, separated by `;`, before the snapshot:
  `tap:<text>` (the view showing it: a button's title, a text, a tab, a
  sidebar's row, a segment), `type:<text>` (into the focused field),
  `back`, `key:<shortcut>` (`ctrl+shift+k`), `menu:<item>` (the menu
  bar's), `escape`, `close` (the window in use), `wait:<duration>`
  (`500ms`), `waitfor:<file>` (until the file exists: a test says when
  to go on) and `snap:<file>`.

## The build cache

What builds compile and download is kept in Asuka's cache and shared by
every app on your computer: a second app, or a build after a small
change, compiles only what's new. The first build for a platform
downloads Asuka's prebuilt libraries for it, once, from the CLI's
GitHub release, checked against the checksums the CLI carries (the
install script and `asuka upgrade` download your computer's own ahead
of it); your proxy (`HTTPS_PROXY`, `NO_PROXY`) applies. For a mirror, set
`ASUKA_PREBUILT_URL` to where the release's `asuka-prebuilt-*.tar.gz`
files are: a URL, in which `{version}` stands for the CLI's version
(`ASUKA_PREBUILT_TOKEN`, if set, is sent to it as a bearer token), or,
on a computer without the network, a directory you copied them into.
Its Swift
libraries are used with the Xcode they were built with or a newer one;
with an older Xcode, they're compiled once on your computer instead.

- `asuka cache stats` prints where the cache is, its size, and how often
  builds found what they needed in it.
- `asuka cache clean` empties it; the next builds compile everything
  again.
- The cache stays under 10 GB, removing what was used least lately.
  `ASUKA_CACHE_MAX` sets another limit (`ASUKA_CACHE_MAX=20G`, `500M`;
  `0` for none), and `ASUKA_CACHE` another place for it.

## Common problems

- **`cbuild: no C compiler`**: install your system's (Xcode's command
  line tools, `build-essential`, LLVM), or Zig with `ASUKA_CC=zig`.
  `building for the web needs Zig` and `cross-compiling for ... needs
  Zig`: install [Zig](https://ziglang.org/download), which builds for
  the web and for other systems.
- **`no iOS simulator SDK (install Xcode)`**: install Xcode, open it
  once, and add an iOS simulator runtime in its Components settings.
  `no iPhone simulator; add one in Xcode` and `no simulator "..."`
  mean there's no simulator of that name: `xcrun simctl list devices`
  lists them.
- **An iPhone that isn't ready**: `... isn't paired with this Mac`
  (plug it in, unlock it and tap Trust), `... isn't reachable` (plug it
  in, or wake it on the Mac's Wi-Fi), `... has Developer Mode off`
  (Settings › Privacy & Security › Developer Mode), `... is locked`
  (unlock it). `no iPhone or iPad is connected` with one plugged in:
  check that `xcrun devicectl list devices` lists it.
- **`Xcode couldn't make the development profiles`**: sign in to Xcode
  with the team's Apple ID (Xcode › Settings › Accounts), or set an App
  Store Connect API key; a bundle ID another team has needs another
  `id` in `asuka.toml`. `the keychain has Apple Development certificates
  of N teams`: pick one with `--team`.
- **`the player hasn't connected yet`**: plug the phone in and unlock
  it, or put it on the Mac's network, allow its Local Network access,
  and let `asuka` take connections in the Mac's firewall (System
  Settings › Network › Firewall).
- **`no Android SDK: install Android Studio or set ANDROID_HOME`**: Asuka
  looks in `ANDROID_HOME`, `ANDROID_SDK_ROOT`, then
  `~/Library/Android/sdk` on a Mac, `%LOCALAPPDATA%\Android\Sdk` on
  Windows and `~/Android/Sdk` on Linux.
- **`no Android NDK`**: install one from Android Studio's SDK Manager
  (SDK Tools, NDK). Asuka takes the newest that has a build for your
  computer (`windows-x86_64` on any Windows PC, Arm too).
- **`no Android device or emulator`**: make an AVD in Android Studio's
  Device Manager, or plug in a phone. Asuka waits up to four minutes for
  an emulator it starts to boot (`the emulator didn't boot`).
- **Java**: Gradle needs a JDK from 17 to 24: `JAVA_HOME` when it's one
  of those, else Android Studio's own, where its installer or JetBrains
  Toolbox put it (on Windows, in Program Files or
  `%LOCALAPPDATA%\Programs`), else the newest other JDK installed that
  is. With none, the build says so: set `JAVA_HOME` to one.
- **The first Android build is slow**: Gradle downloads itself and
  Android's libraries, once.
- **A release build on the phone**: an app installed by `asuka build`
  (another signing key) is uninstalled, with its data, to install the
  player.
- **`asuka dev --platform windows runs the app on this computer`**:
  `asuka dev` runs a Windows app on Windows only. Build it elsewhere
  with `asuka build --platform windows`, and copy the folder to a PC.
- **`... isn't installed` on Linux**: an app needs GTK 4.14 and
  libadwaita 1.5 or later, and some packages a library of their own:
  WebKitGTK 6.0 for `WebView`, GStreamer for video and audio, libsecret
  for secure storage. The error names it; install it with your
  distribution's package manager.
- **A Linux app that doesn't open**: it needs a display. Over SSH or in
  CI, run it under Xvfb (`xvfb-run -a`), with a session bus
  (`dbus-run-session`) for what goes through D-Bus.
- **Starting over**: Asuka's builds, caches and the apps' development
  data are in Asuka's cache directory: `~/Library/Caches/asuka` on a
  Mac, `~/.cache/asuka` on Linux, `%LocalAppData%\asuka` on Windows.
  `asuka cache clean` empties it, to build everything again, with no
  data.

## asuka.toml

`asuka dev` and `asuka build` read the same `asuka.toml`; see
[the asuka.toml reference](asuka-toml.md) for its keys, and
[Shipping](shipping.md#asukatoml) for how they come together for a
release.
