<img src="artwork/icons/atfield.svg" width="96" alt="">

# Asuka

Apps in the Asuka language with each platform's own widgets, on iOS,
Android, macOS, Windows, Linux and the web.
Asuka renders with each platform's widgets (UIKit on iOS, Android
Views and Material 3 on Android, AppKit on the Mac, Win32 drawn as
Windows 11's, GTK 4 and libadwaita on Linux, the DOM in a browser) and
updates them with fine-grained signals: a change touches only the views
that read it.

> [!WARNING]
> **Alpha.** Asuka is pre-1.0 and changes daily. There is no API
> stability, no backwards compatibility, and no migration guide yet.

```asuka
component Counter {
  state count = 0

  SafeArea {
    align: .center
    justify: .center

    Text("Count: {count}")
    Button("Add") {
      variant: .filled
      on press { count += 1 }
    }
  }
}

app { Counter() }
```

- **A language for UI**: statically typed, declarative components whose
  state is bound where it's read, async tasks owned by the component
  that started them, and errors the compiler sees ([a tour of
  the language](docs/language-tour.md)).
- **A small core in C**: signals, layout (flexbox and grid), the view
  tree and its batches to the platform, and a VM, built for each
  platform with its own compiler; no garbage collector, no runtime of
  another language.
- **Native controls**: stacks and tabs with the platform's navigation,
  lists, text fields, pickers, sheets, alerts, menus, web views, video,
  and your own native views.
- **`asuka dev`**: each save compiles the app to bytecode and reloads it
  in the running app in milliseconds, its state kept: on the simulator,
  an emulator or a phone, in a browser, or as a window on your Mac, PC
  or Linux machine.
- **`asuka build`**: the app compiled to C and built with the core: a
  signed `.ipa` for the App Store, an `.aab` for Google Play, a Mac
  `.app` (Apple silicon, or Intel) with its `.dmg`, a Windows folder, a
  Linux prefix as a `.deb`, an `.rpm`, an AppImage, a `.tar.gz` and a
  Flatpak manifest, and a static site with the core as WebAssembly;
  **`asuka publish`** uploads them, numbered after the store's last
  build, to TestFlight (its groups, App Review) or a Play track.
- **`asuka test`**: `test` blocks run on a fake host that lays the app
  out as a phone would, driven by what the screen shows.

## Quick start

You need, for phones, Xcode (iOS) or Android Studio with an NDK
(Android); for the Mac, Xcode's command line tools; for the web,
[Zig](https://ziglang.org/download); Windows LLVM's clang, Linux a C
compiler and GTK 4.14 with libadwaita 1.5. See [Getting
started](docs/getting-started.md).

```sh
curl -fsSL https://asuka.egoist.dev/install.sh | sh
# Windows, in PowerShell: irm https://asuka.egoist.dev/install.ps1 | iex
asuka new hello
cd hello
asuka dev --platform ios     # or android, web; on a Mac, PC or Linux, asuka dev alone runs it there
asuka test                   # its test blocks
```

See [the documentation](docs/README.md) for the guides, and
[`examples`](examples) for apps. The documentation is also in
[Chinese](docs/zh/README.md) (中文文档) and
[Japanese](docs/ja/README.md) (日本語ドキュメント).

## License

MIT
