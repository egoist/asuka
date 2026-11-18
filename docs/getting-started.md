# Getting started

Apps are written in the Asuka language ([the language](language-tour.md))
and show each platform's own widgets. The `asuka` CLI builds them for
each platform with that platform's own tools; one command makes an app,
and another runs it, reloading as you save.

## What you need

The CLI, and for each platform you run on, the tools that build for it:

- **iOS**: a Mac with Apple silicon and Xcode 26 or later, and its iOS
  simulator or an iPhone. (An Intel Mac isn't one Asuka develops on.)
- **Android**: a Mac, a Windows 10 or 11 PC (x64 or Arm) or Linux, with
  Android Studio and, from its SDK Manager, an NDK (SDK Tools → NDK),
  and an emulator from its Device Manager, or a phone with USB debugging
  on. Asuka finds the SDK in its usual place, or at `ANDROID_HOME`, and
  Android Studio's Java, and builds with the NDK and Gradle. On an Arm PC, the NDK and the emulator run emulated
  (they're x64), and the emulator needs an x86_64 system image.
- **macOS**: a Mac with Apple silicon and Xcode 26 or later, or its
  command line tools (`xcode-select --install`). The apps you build run
  on macOS 11 or later, on Apple silicon, or on Intel Macs built with
  `--arch amd64`.
- **Windows**: a PC with Windows 10 or 11, x64 or Arm, with LLVM's clang
  (`winget install LLVM.LLVM`), or [Zig](https://ziglang.org/download)
  with `ASUKA_CC=zig`. `WebView` and `Video` use the Microsoft Edge
  WebView2 Runtime, which Windows 11 has, as most Windows 10 PCs do.
- **Linux**: a C compiler (`build-essential`, `gcc`, `clang`), and GTK
  4.14 and libadwaita 1.5 or later (Ubuntu 24.04, Fedora 40, Debian 13
  and later have them), on Wayland or X11:

  ```sh
  sudo apt install build-essential libgtk-4-1 libadwaita-1-0   # Debian, Ubuntu
  sudo dnf install gcc gtk4 libadwaita                         # Fedora
  sudo pacman -S base-devel gtk4 libadwaita                    # Arch
  ```

  `WebView` needs WebKitGTK 6.0 (`libwebkitgtk-6.0-4`), and `Video` and
  audio GStreamer; a package that needs a library that isn't there says
  which, as an error.
- **The web**: a browser, and [Zig](https://ziglang.org/download), which
  builds for WebAssembly.

With Zig installed, `asuka build` also makes a Windows or Linux app from
any system, and a Mac app from a Mac.

## Install the CLI

On macOS and Linux:

```sh
curl -fsSL https://asuka.egoist.dev/install.sh | sh
```

On Windows, in PowerShell:

```powershell
irm https://asuka.egoist.dev/install.ps1 | iex
```

The script downloads the CLI from its GitHub release, checks it against
the release's checksums, and installs it in `~/.asuka/bin` (on Windows,
`%LOCALAPPDATA%\Programs\asuka\bin`, which it adds to your PATH),
then downloads Asuka's prebuilt libraries for your computer's own apps,
so your first build doesn't wait for them.
`ASUKA_VERSION=0.3.0` installs that version instead of the latest, and
`ASUKA_INSTALL` another directory.

`asuka upgrade` updates the CLI to the latest release (`asuka upgrade
0.3.0`: to that one), and its prebuilt libraries with it. Once a day, asuka looks in the background for a
newer release and, if there is one, says so when a command ends; it
never makes a command wait. `ASUKA_NO_UPDATE_CHECK=1` turns that off,
and it's off in CI.

## Make an app

```sh
asuka new hello
cd hello
```

`hello` has:

- `app.asuka`, the app: a component, the `app` block that shows it, and a
  test.
- `asuka.toml`, its name, its ID (the bundle ID, application ID and
  desktop app ID) and version, and what [the stores and each
  platform](shipping.md) need.
- `AGENTS.md`, for coding agents: how to check, test and look up an
  error's code, and to read `asuka guide` first, which prints the
  language on one page for the version installed.
- `.gitignore`.

`asuka new` makes one of three kinds of project: an app (this one),
with `--lib` a library other apps import, and with `--bin` a program,
a command-line tool with no window ([Programs](programs.md)).

## Run it

```sh
asuka dev                      # this computer: on a Mac, a PC or Linux
asuka dev --platform ios        # the iOS simulator
asuka dev --platform ios --device iphone  # the iPhone plugged in
asuka dev --platform android    # an Android emulator or phone
asuka dev --platform web        # your browser
```

Save an `.asuka` file and the app reloads in a few milliseconds, its
state kept. Its output and its errors are in the terminal,
with each reload's report (`asuka: reloaded in 2.1 ms, 3 states kept`).

`asuka dev` builds the platform's **player** once: the app's shell (its
icon, its name, its permissions), but none of the app's code. It
installs and launches it, then sends it your app; on each save it
compiles again, and the player loads the new version in place. A save that doesn't compile prints its errors and shows them over the
app, which keeps running what it had; so does an error the app didn't
catch ([Errors over the app](development.md#errors-over-the-app)). Changing `asuka.toml`, the assets or native
code builds and launches the player again.

- **On your computer** (`asuka dev`, or `--platform mac`, `windows`,
  `linux`), the player runs in a window.
- **On a phone**, `--device` picks the simulator or the iPhone (its name
  or UDID, or `iphone` for the one plugged in) or the Android device (an
  adb serial or an AVD's name). `--once` launches the app without
  watching.
- **In a browser**, Asuka serves the app at `http://localhost:8790/` and
  opens it; each save loads into the page in place. `--lan`
  serves it to your network, to try it in a phone's browser. See [the
  web](web.md).

To run on an iPhone, plug it in, unlock it, tap Trust and turn on
Developer Mode (Settings › Privacy & Security), and add your Apple ID to
Xcode (Settings › Accounts): `asuka dev --platform ios --device iphone`
signs the player with your team and installs it, the first time; the
iPhone reaches `asuka dev` over your network. See
[Devices](development.md#devices).

[Development](development.md) has the rest: what a reload keeps,
devices, logs, and the problems you may meet.

## Change it

`app.asuka` from `asuka new` is a counter:

```asuka
component Counter {
  state count = 0

  SafeArea {
    justify: .center
    align: .center
    gap: 16

    Text("hello: {count}") { font: .title }
    Button("Add") {
      variant: .filled
      on press { count += 1 }
    }
  }
}

app { Counter() }

test "Add counts" {
  let app = mount(Counter())
  app.tap("Add")
  app.expect("hello: 1")
}
```

- **State** is `state count = 0`: reading it in a view (`"{count}"`)
  binds the view to it, and writing it (`count += 1`) updates what read
  it, and only that.
- **Components** declare their state, props and views; their body isn't
  run again when something changes, only the bindings that read it.
- **Style** is flexbox, in points: `grow`, `gap`, `padding`, `align`,
  `justify`, and the look (`background`, `radius`, `shadow`...). An app
  that sets none looks as the platform's apps do.
- **Tests** are `test` blocks: `asuka test` runs them with no device,
  laying the views out as a phone would (see [Testing](testing.md)).

The same code is the app on every platform. [The language](language-tour.md)
has the rest of it.

## An app's files

- `assets/`: images and other files, `Image("photo.jpg")`, and fonts in
  `assets/fonts`.
- `locales/`: the app's messages in each language (see
  [Languages](i18n.md)).
- Packages of its own: a directory of `.asuka` files beside the app's,
  `import ./components`. Dependencies are in `asuka.toml`'s
  `[dependencies]` (`asuka add @acme/charts`; see
  [Packages](packages.md)).
- Native packages: Asuka packages that declare a `native interface` or a
  `native component`, which each platform implements in its own project
  (a Swift package, a Gradle module, JavaScript, C on Windows and Linux). See
  [Native code](native.md).

## Next

- [Development](development.md): the dev loop, devices, logs.
- [Testing](testing.md): `test` blocks and `asuka test`.
- [Shipping](shipping.md): builds for the App Store, Google Play, the
  Mac, Windows, Linux and the web.
