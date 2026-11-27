# AGENTS.md

Asuka is a UI framework that renders with each platform's own widgets,
driven by fine-grained signals. Apps are written in the **Asuka
language** (docs/language.md; its compiler is Go, in `lang/`, run by the
CLI) and run on a **core in C** (`core/`: the VM, signals, the UI
runtime, layout, ops). **Mobile first: iOS and Android.** Desktop and
the web run on the same core: the macOS host (`platform/macos`, Swift
on AppKit, the core linked in, as iOS's), the Windows host
(`platform/windows`, C on Win32) and the Linux host (`platform/linux`, C
on GTK 4 and libadwaita, opened with dlopen), those two each linked with
the core and the app into one executable and sharing C in
`platform/desktop`, and the web (`platform/web`, the DOM from
JavaScript, the core as WebAssembly). The packages the CLI carries are
the standard library (`std/<name>`, the language's own, the same
everywhere: in `std/`) and the platform SDK (`platform/<name>`, what
wraps a device, an OS service or a store: in `sdk/`); no other package
is named under either prefix. **Read [DESIGN.md](DESIGN.md),
[RFC 0007](rfcs/0007-c-core-and-asuka-language.md) and [RFC
0008](rfcs/0008-no-go-at-run-time-and-c-ffi.md) before changing code**;
DESIGN.md is the source of truth for the architecture, the RFCs for the
rewrite's milestones, lang/LOWERING.md for how the language compiles.
Work milestone by milestone, in order.

## Hard rules

- **No Go at run time** (RFC 0008). Nothing an app ships is Go: Go is
  the CLI's, the compiler's, the generators' and the tests' language.
  New run-time code is C (the core, the Windows and Linux hosts, a
  package's Windows and Linux code), Swift (Apple platforms), Kotlin
  (Android) or JavaScript (the web). Don't add to package `native`:
  it's being replaced.
- **No cgo, anywhere.** Go code (the CLI, the compiler, the generators,
  the tests) builds with `CGO_ENABLED=0`; it reaches C through purego and
  `syscall`, and builds C by running a compiler (internal/cbuild).
- **The core never knows a host.** Hosts talk to it through
  `core/include/asuka.h` only. Adding an element means: its kind and
  props in `ops/schema.go` (`go generate ./ops`) when it makes a new
  kind of native view, its keys and events in `lang/prelude/elements.go`
  (`go generate ./lang/prelude`), its behavior in `core/src/ui`, and its
  view in every host (the C fake host first, then iOS and Android, then
  macOS, Windows, Linux and the web).
- **C:** C17, `-Wall -Wextra -Werror`, internal symbols `ak_`, the ABI
  `asuka_`; tests under ASan and UBSan (`go test ./core`).
- **Nothing phone-only in the core's data structures**: no single global
  root, no assumption that the screen size never changes.
- **Main thread.** The core, every host call and every state write run
  on the platform's main thread. Other threads reach it with `ak_post`;
  `spawn` runs values-only work on workers. Never block the main thread.
- **One flush, two crossings.** Effects emit ops into a batch; a flush
  applies the props batch, lays out, applies the frames batch. Never
  cross into Swift or Kotlin per property.
- **Hot paths** (signal propagation, flush, layout, op encoding and
  decoding, the VM's dispatch): no per-op or per-write allocation in
  the steady state; tests count allocations (`ak_alloc_count`).
- **Owners dispose explicitly.** Every effect and task has an owner;
  disposing it stops the effect, cancels the task and runs cleanups.
  Test it whenever you add a construct that makes owners.
- **One source, generated outputs:** the wire (`ops/schema.go`), the
  bytecode (`lang/bytecode/opcodes.go`), the elements
  (`lang/prelude/elements.go`), the diagnostics' codes (`lang/diag`:
  docs/errors, and asuka guide's pages). Edit those, run go generate,
  commit the outputs.
- **Every diagnostic has a code** (rfcs/0015): a new error or warning
  takes one of `lang/diag`'s (one per rule, not per message), or a new
  one with its summary, why, fix, a wrong and a right program, and its
  zh and ja text.

## Commands

```sh
go test ./core                      # the C core's tests (ASan/UBSan) and the VM's; -run 'TestC/Layout': one C test file
go test ./lang/...                  # the compiler: parser, checker, codegen, and Asuka programs run on the VM
go run ./cmd/asuka test [dir]       # an Asuka package's test blocks, on the fake host
go run ./cmd/asuka fmt -w [paths]   # format .asuka files
go run ./cmd/asuka check [dir]      # a package checked for every platform's files (tray_linux.asuka: Linux's); --platform ios: one's
go run ./cmd/asuka doc std/time     # a package's documentation, as Markdown (lang/doc); --all -o dir: the website's reference pages, /docs/std and /docs/platform
go test ./lang/doc                  # every std and platform package's pub declarations have their /// comment
go run ./cmd/asuka test --bench bench/ui   # the UI runtime's benches on the fake host (js-framework-benchmark's operations, signals, layout): time, allocations, what reached the host
bench/ui/instr.sh ./asuka [core]    # the same benches as instructions retired per iteration (a loaded machine doesn't move them); bench/ui/run.sh: timed runs, tabulated
go test ./bench/tools -bench . -benchmem   # asuka check's Go side on the biggest packages; bench/tools/instr.sh N ./asuka-old ./asuka-new: whole commands, as instructions
ASUKA_CPUPROFILE=cpu.out go run ./cmd/asuka check std/base  # the CLI's CPU profile (ASUKA_MEMPROFILE: its allocations), for go tool pprof
go generate ./lang/bytecode ./lang/prelude  # the VM's opcodes header; the prelude and the UI's element constants
go test ./lang/diag                 # every diagnostic code's wrong and right program, for every platform (the run time's on the fake host)
go generate ./lang/diag             # the codes' pages: docs/errors/<code>.md, and docs/zh/errors, docs/ja/errors
go generate ./cmd/asuka             # asuka guide's pages (cmd/asuka/guide), from docs/language-tour.md, docs/language and lang/diag
go run ./cmd/asuka explain write-in-binding  # a code's page (--lang zh); asuka check --json, asuka test --json: diagnostics as JSON lines
go test ./internal/cbuild           # the core built for this computer, iOS, Android and the web (ASUKA_CC=zig for Zig)
go build ./... && go vet ./...      # CGO_ENABLED=0: everything
go test ./...                       # everything Go runs: the core's C tests, the compiler, ops, the CLI, the generators
go run ./cmd/asuka test examples/counter  # an example's tests (its test blocks); the examples are Asuka, what they still ask of the platforms in examples/STDLIB.md
go test ./core -run TestC/Layout -update-known  # after fixing layout: refresh core/test/testdata/css/known_failures.txt
go test ./ops -update               # after changing the wire format: refresh golden files
go test ./platform/macos/mactest -run Conformance  # what the macOS host draws for the style props, measured (package conformance; go generate ./conformance writes its app)
go test -tags e2e ./e2e -run Conformance # likewise on the simulator, the emulator and headless Chrome (ASUKA_CONFORMANCE_OUT=dir: the captures)
swift test --package-path platform/ios # the Swift decoder against the golden files (macOS)
node --test platform/web                # the JavaScript decoder, likewise
go test ./std                            # std packages' Swift run on this Mac (std/http's URLSession download)
(cd platform/android && ./gradlew :asuka:testDebugUnitTest)  # the Kotlin decoder, likewise (ANDROID_HOME set)
go generate ./ops                   # Go constants, Swift, Kotlin and JavaScript decoders
go run platform/web/gen_symbols.go  # the web host's symbols, from the Android host's
go generate ./i18n                  # CLDR's tables, from the pinned release
go generate ./internal/tzgen        # std/time's tz database (core/src/std/tzdata_gen.c) and its C test's cases, from the pinned tzdb release
go generate ./internal/ucdgen       # strings' Unicode tables (core/src/rt/unicode_gen.c: graphemes, widths, case) and their C test's data, from the pinned UCD release
go generate ./internal/mimallocgen  # the core's allocator, mimalloc (core/third_party/mimalloc), from the pinned release, and its names renamed (core/src/base/mimalloc_names.h)
go test ./i18n -run 'CTables|CompileCases' -update-c  # the C core's CLDR tables (core/src/ui/i18n_gen.c) and its catalog test data
go run platform/web/testing/asqlite.go  # std/sqlite's C (its [c], SQLite vendored in third_party) as the web's SQLite module, asqlite.wasm, into the build cache (Zig); node --test platform/web runs it
go run ./cmd/asuka i18n check examples/i18n  # an app's catalogs, checked (the compiler reads them: dev and build check them too)
go run ./cmd/asuka native generate examples/media/segmented  # a native package's glue: Go, Swift, Kotlin, JavaScript (dev and build do it too; `native check`: CI)
go test ./internal/nativegen -update     # after changing the generator: refresh its golden files (testdata/pantry)
go test ./cmd/asuka -run MacSwift        # a native package's Swift (the pantry), built into a Mac app's executable with the host
go test -tags e2e ./e2e -run NativePackages  # native packages on the simulator, the emulator and headless Chrome
go run ./cmd/asuka dev --platform ios --lang ar examples/i18n  # a language for one run; --lang ar-XB, en-XA: pseudo locales; --dir rtl: any app right to left
go run ./cmd/asuka dev --platform ios examples/counter       # iOS simulator (Xcode): the player (the core without an app) loads the bytecode asuka dev sends
go run ./cmd/asuka dev --platform android examples/counter   # Android emulator (SDK + NDK), likewise
go run ./cmd/asuka dev --platform ios --once examples/counter # build the player, launch it once with the app, without watching
go run ./cmd/asuka run file.asuka                # one file: its test blocks, then what its app shows (fake host)
go run ./cmd/asuka new --bin /tmp/greet          # a program (fn main): asuka.toml, main.asuka
go run ./cmd/asuka run examples/notes -- add Hi  # a program's main on the VM (here [bin.notes]), the arguments after --
go run ./cmd/asuka build --bin notes --target linux-arm64 examples/notes  # a program's executable (another desktop's with Zig)
go test ./lang/compile -run 'TestPrograms|TestRun/(os|flag|command)'      # programs on the VM and compiled to C; std/os, flag, command
go test -timeout 30m ./internal/appbuild ./cmd/asuka  # the builds for every target, the dev loop end to end (mac, Chrome, simulator, emulator)
go test -tags e2e ./e2e -run IOS                 # GUI tests: the examples on the simulator (Android: -run Android, an emulator running)
go run ./cmd/asuka build --platform ios --simulator examples/counter    # a release build (--platform ios alone: a signed .ipa)
go run ./cmd/asuka build --platform android examples/counter           # an .aab (ASUKA_KEYSTORE, ASUKA_KEY_ALIAS, ASUKA_KEYSTORE_PASSWORD)
go run ./cmd/asuka publish --platform ios --groups Friends examples/counter  # build and upload to TestFlight (ASUKA_ASC_KEY_ID, ASUKA_ASC_ISSUER_ID, ASUKA_ASC_KEY)
go run ./cmd/asuka publish --platform android --track internal examples/counter  # build and upload to Google Play (ASUKA_PLAY_SERVICE_ACCOUNT)
go run ./cmd/asuka dev examples/counter            # macOS (on a Mac, no --platform: mac): the player (the Swift host and the core) loads the bytecode asuka dev sends
go run ./cmd/asuka build examples/counter          # likewise: an .app, its .zip and a .dmg (--sign "Developer ID Application: ..."; default ad hoc)
go test ./platform/macos/mactest                 # the macOS host: Asuka apps on the Mac player, driven and read through its automation (ASUKA_COMMANDS: the view tree as JSON)
swift test --package-path platform/macos         # the macOS host's Swift unit tests and its decoder against the golden files
ASUKA_SNAPSHOT=/tmp/c.png ASUKA_DUMP=1 ASUKA_SCRIPT="tap:Add" "dist/darwin-arm64/Counter.app/Contents/MacOS/Counter"  # macOS: steps, then the window as a PNG (the window server's) and its view tree, then quit
go run ./cmd/asuka dev examples/counter            # Windows (on a PC, no --platform: windows): the app and its Win32 host in one process, rerun on save
go run ./cmd/asuka build --platform windows examples/counter  # a folder: the .exe (icon, manifest, version) and its assets; from any system
go test ./platform/windows                       # the Windows host (C), its apps built with Zig and driven in Windows (on Windows, or the swiftvm VM ASUKA_WINDOWS_VM names: "Windows Fresh")
go test ./platform/desktop                       # the C the Windows and Linux hosts share, on this computer (ASan, UBSan)
xvfb-run -a dbus-run-session go test ./platform/linux  # the Linux host: apps built and driven in real GTK windows (GTK 4.14, libadwaita 1.5)
go run ./cmd/asuka dev examples/counter            # Linux (on Linux, no --platform: linux): the app and its GTK host in one process, rerun on save
go run ./cmd/asuka build --platform linux examples/counter    # an install prefix, a .tar.gz, a .deb, an .rpm, an AppImage and a Flatpak manifest; from any system (--package deb,rpm: some)
go test ./internal/distpkg                       # the .deb, .rpm, SquashFS/AppImage and .DS_Store writers, read back
go run platform/linux/gen_symbols.go             # the Linux host's fallback symbols, from the web host's
go test -tags e2e ./e2e -run Linux               # GUI tests: the examples under Xvfb (Linux)
$env:ASUKA_SNAPSHOT='c.png'; $env:ASUKA_DUMP='1'; $env:ASUKA_SCRIPT='tap:Add'; .\examples\counter\dist\windows-amd64\Counter\Counter.exe  # Windows (PowerShell): steps, then the window as a PNG and its view tree
go run ./cmd/asuka dev --platform web examples/counter       # the web: served to the browser, reloaded on save (--lan: to a phone)
go run ./cmd/asuka build --platform web examples/counter     # a static site, in examples/counter/dist/web
go test -tags e2e ./e2e -run Web                  # GUI tests: the examples in headless Chrome
go test ./platform/macos -run StdSwift; go test ./cmd/asuka -run StdAndroidViews; go test -tags e2e ./e2e -run StdWebViews; go test ./platform/windows -run StdViews  # std and sdk packages' views driven on macOS and the iOS simulator, a device, Chrome, Windows (Linux: TestStdUnits)
go test ./cmd/asuka -run StdAndroidUnits    # std packages' Kotlin unit tests (android/src/test) on this computer's JVM, no device; internal/testserver serves std/http's and std/websocket's
go run ./cmd/asuka update keygen                  # the key pair that signs desktop apps' updates (asuka.toml's updates)
go run ./cmd/asuka publish --platform linux examples/counter  # a desktop build and its signed update, uploaded (GitHub, S3)
go test -tags e2e ./e2e -run Updater               # a desktop app updates itself and relaunches (macOS; Linux under xvfb-run)
go test ./platform/windows -run Updater           # likewise on Windows (a PC, or the VM; the feed served in Windows)
```

## Conventions

- Tests assert op streams from the fake host (`core/host/fake`) and count work (ops,
  measurements, allocations). They never pass or fail on wall-clock
  time: wait for the condition, count the work.
- Prefer the platform's behavior and defaults; an app that sets no style
  must look native (UIKit on iOS, Material 3 on Android). When unsure
  how a platform behaves, read its docs or an established framework's
  implementation (React Native, react-navigation, Solid, Taffy, the CSS specs) before
  inventing one.
- Platform widgets for UI. What isn't UI may use the platform's own
  implementation too, unless the platforms' versions give materially
  different results for the same input: then one shared implementation
  (C in the core or a std package's, the web through the core's wasm)
  runs everywhere. Markdown is the case: Apple's parser, Markwon and
  marked disagree on tables, autolinks, HTML, safe links and list
  spacing, so the core parses it (md4c) and each host only styles the
  runs with its own text widget. Decide per feature; don't share by
  default.
- Swift host: UIKit only, no SwiftUI. Kotlin host: Android Views and
  Material Components, no Compose. macOS host: Swift on AppKit only
  (`platform/macos`), no SwiftUI, the core linked in; its decoder is
  iOS's (Sources/AsukaOps, `go generate ./ops` writes both); its parts
  each in files of their own (Parts.swift lists them); native packages'
  Swift is built into the app's executable with it. Windows host: C17 on
  Win32, the common controls v6, GDI+'s flat API, DWM, Direct3D 11,
  WebView2 and WinRT through COM vtables in C (`platform/windows/src`),
  no WinUI; the controls drawn as Fluent's in custom draw, so an app
  that sets no style looks like Windows 11. Linux host:
  GTK 4 and libadwaita widgets from C (platform/linux/src), their
  functions declared once for the host and packages
  (`platform/desktop/include/asuka_linux.def`) and resolved with dlsym,
  so it builds from any system without GTK's headers; no GtkBuilder or
  Blueprint; an app that sets no style looks like a GNOME app. Web host:
  the DOM from plain JavaScript modules in `platform/web/src` (no build
  step, no framework), the browser's own elements for controls and
  dialogs, driven by the core as WebAssembly (`src/core.js`): batches, measures and
  events.
- Every prop that shows text is a binding, so a message said in it
  (`tr.title()`) follows the app's language; only what's shown once (an
  alert) takes a string. Frames are always physical: a host never mirrors
  what the core places.
- A docs example that's a whole file, not a fragment, is fenced
  ` ```asuka check `, in every language's page: `go test ./lang/compile
  -run CheckedDoc` compiles each.
- The docs have a Chinese translation in `docs/zh` and a Japanese one in
  `docs/ja`, the same pages with the same headings (each one's
  TRANSLATING.md: its style and terms). A change to a page of `docs`
  changes its translations too, written as natural Chinese and Japanese,
  not word for word.
- Keep generated files (`ops` decoders, the core's CLDR tables and catalog
  test data (`go test ./i18n -run 'CTables|CompileCases' -update-c`), native
  packages' `*.asuka.*`) committed and up to date.
- Platform code an app needs is a native package (docs/native.md): a
  `native interface` or `native component` in its Asuka, implemented in
  each platform's own project (Package.swift, an android/ Gradle module,
  web/, desktop/'s C for Windows and Linux). Changing `internal/nativegen`
  or core/src/native's wire means regenerating every native package in
  the repo (`asuka native generate` in each: std's and sdk's packages,
  the examples', the tests' testdata) and the platforms' runtimes
  (NativeModules.swift, kept identical in platform/ios and
  platform/macos; NativeModules.kt; native.js).
