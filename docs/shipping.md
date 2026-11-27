# Shipping

`asuka build` makes an app's release build for one platform, from the
app's directory (or the one given), into its `dist`. It compiles the app
ahead of time and builds it, optimized, for the platform, with the
platform's own compiler: there's no interpreter in what you ship, and
[`@debug`](language/control-flow.md#code-for-development-builds) is
false, so what's for development only isn't in it. `asuka publish`
uploads the iOS and Android builds to their stores: to TestFlight and
App Review, or a Google Play track (see [Publishing](#publishing)); and
the desktop builds' signed updates, which installed apps update to, to
GitHub's releases or an S3 bucket (see [Updates](#updates)).

| `--platform` | What it makes, in `dist` | Builds on | Given to users |
|---|---|---|---|
| `ios` | `<Name>.ipa`, signed for the App Store | a Mac with Xcode | TestFlight, the App Store (`asuka publish`) |
| `android` | `<Name>.aab`, signed with your upload key | any system with a JDK, the Android SDK and the NDK | Google Play (`asuka publish`) |
| `mac` | `darwin-<arch>/`: `<Name>.app`, its `.zip` and a `.dmg` (Apple silicon; `--arch amd64`: Intel); or a `.pkg` (`--appstore`) | a Mac with Xcode's command line tools | the `.dmg` or the `.zip`, signed and notarized; the Mac App Store |
| `windows` | `windows-<arch>/`: `<Name>/`, the `.exe` and its assets, an installer (`-setup.exe`), its `.zip`, and an `.msix` (`--package msix`) | Windows with clang, or any system with Zig | the installer, the `.zip`; the MSIX in the Microsoft Store, or sideloaded |
| `linux` | `linux-<arch>/`: an install prefix, a `.tar.gz`, a `.deb`, an `.rpm` (signed with your OpenPGP key), an AppImage, a Flatpak manifest | Linux with a C compiler, or any system with Zig | the `.deb`, the `.rpm`, the AppImage, the `.tar.gz`, a Flatpak; an apt and a dnf repository (`asuka publish --repo`) |
| `web` | `web/`, a static site | any system with Zig | any web server or CDN |

Without `--platform`, on a Mac, a Windows PC or a Linux machine, it
builds for that computer. It builds with the platform's compiler
(Xcode's for Apple's platforms, the NDK's for Android, the system's on
Windows and Linux); [Zig](https://ziglang.org/download) builds for the
web, and for Windows and Linux from another system (`ASUKA_CC=zig` uses
it everywhere). See [the desktop](desktop.md#how-a-desktop-app-is-made)
for what a desktop build holds.

## asuka.toml

```toml
name = "Notes"
id = "com.example.notes"
version = "1.2"
build = 7
icon = "icon.png"

[ios]
team = "ABCDE12345"

[android]
keystore = "upload.jks"
keyAlias = "upload"
```

Every key is optional; [the asuka.toml reference](asuka-toml.md) lists
them all, with their types and defaults. These apply on every platform:

- `name` is the app's name: under the icon, in the Mac's menu bar, the
  window's title on Windows, the launcher's on Linux, the web page's
  title. It names the build's files too (`Notes.app`, `Notes.exe`).
  The default is the directory's name.
- `id` is the bundle ID on iOS and the Mac, the application ID on
  Android and Linux (its desktop file, icon and D-Bus name), the
  AppUserModelID on Windows (its toasts'). The default is
  `dev.asuka.<directory>`.
- `version` is what users see (default `1.0`); `build` counts the builds
  uploaded, and must grow with each (default 1). `asuka publish` numbers
  each build after the store's highest itself, so `build` is only where
  it starts. On Windows they're the `.exe`'s file version
  (`1.2.0.7` for version 1.2 and build 7), on Linux the metainfo's
  release.
- `icon` is a square PNG, 1024×1024, without transparency for iOS. Each
  build makes the sizes its platform wants: an asset catalog on iOS,
  the launcher's on Android, an `.icns` on the Mac, an `.ico` in the
  `.exe` on Windows, the hicolor theme's from 16 to 512 pixels on
  Linux, the web manifest's, the favicon and iOS's home screen icon on
  the web.
- `accent` is the app's accent color, `"#rrggbb"`: the tint on iOS, the
  seed of the Material 3 color scheme on Android, the color of `Accent`,
  filled buttons and links on the Mac (AppKit's own controls keep the
  accent the user picks), the controls' accent on Windows in place of
  the system's, libadwaita's accent on Linux, and on the web the page's
  accent and its manifest's `theme_color`. Without it, each platform's
  own.
- `scheme` opens the app for `myapp://` URLs: on iOS, Android, the Mac
  (`CFBundleURLTypes`), Linux (the desktop file's
  `x-scheme-handler`) and Windows (the app registers it for the user as
  it starts, in `HKEY_CURRENT_USER\Software\Classes`). `links`
  opens it for https links to your domains (universal links, App Links;
  iOS and Android). `onOpenURL` gets them.
- `fileTypes` are the kinds of files the app opens, which
  `onOpenFiles` gets:

  ```toml
  [[fileTypes]]
  name = "Markdown Document"
  extensions = ["md", "markdown"]
  role = "viewer"

  [[fileTypes]]
  name = "Notes Document"
  extensions = ["note"]
  icon = "note.png"
  ```

  `extensions` is required; `name` defaults to the first extension in
  capitals and "Document". `mimeTypes` and `uti` default to what the
  systems call an extension Asuka knows (md, txt, json, csv, pdf, png,
  jpg...); for another, the type is the app's own:
  `application/x-<app>-<ext>` and `<id>.<ext>`, exported (the Mac and
  iOS), defined for shared-mime-info (Linux), and the app is its files'
  default app. `conformsTo` is the UTIs an exported or imported type
  conforms to (default `public.data`, `public.content`); `role` is
  `editor` (default) or `viewer`; `icon`, a square PNG, is the files'
  icon on the Mac, Windows and Linux. The Mac and iOS declare them in
  Info.plist (`CFBundleDocumentTypes`, iOS opening documents in place),
  Android as intent filters, Windows registers them for the user as the
  app starts (a ProgID per type, the extensions' `OpenWithProgids`, the
  icons in the `.exe`), Linux in the desktop file's `MimeType` and
  `share/mime/packages/<id>.xml` (run `update-mime-database` and
  `update-desktop-database` after unpacking the `.tar.gz`; the `.deb`,
  the `.rpm` and Flatpak have them run), the web in the manifest's
  `file_handlers` (an installed app, in Chromium).
- `singleInstance`, `true` by default: on Windows and Linux an app with
  an `id` runs once, a second launch handing its arguments to the
  running one (`onSecondInstance`, its files to `onOpenFiles`)
  and quitting. `false` runs it as often as it's started. The Mac runs
  an app once either way.

Platform by platform:

- `ios.*` is the iOS build's ([iOS](#ios)); `ios.team` is the Mac
  build's team too, and `ios.usage`'s purpose strings go in the Mac's
  Info.plist as well.
- `android.*` is the Android build's ([Android](#android)).
- `mac.profile` is a Mac build's provisioning profile, and
  `mac.appStoreProfile` the Mac App Store's; `mac.packages`, what it's
  handed out in: see [macOS](#macos).
- `linux.*` is what a Linux build's desktop file, AppStream metainfo and
  packages say, and which packages it makes: see [Linux](#linux).
- `web.vapidPublicKey` is the app server's VAPID public key, for
  `platform/push` on the web (Web Push; see
  [the web](web.md#platformpush-web-push)).
- A Windows build needs nothing beyond the keys above; its signing
  certificate is given in the environment ([Windows](#windows)).

These are for the phones and tablets:

- `orientations`: `portrait`, `landscape` (both by default), on phones.
  On iPad the app takes every orientation, as Split View, Slide Over
  and Stage Manager need; Android 16 likewise ignores it on tablets and
  foldables.
- `ios.iphoneOnly` leaves iPad out. By default the app is an iPad app
  too, at whatever size the user gives its window; on Android it's
  resizable for split screen and desktop windows. Lay out from
  `environment()`'s `width` and `height`, the window's size, not
  the device's.
- `shareTarget` offers the app in other apps' share sheets: `true` for
  texts and links, or a table, `{types = ["text", "image/*"], max = 10}`,
  for files too (see [Device](device.md#shares-from-other-apps-onshare));
  `onShare` gets them. On iOS it's a share extension in the app, with
  a bundle ID of its own (the app's plus `.share`).
- `splash` is the launch screen: `{image = "logo.png", size = 96,
  background = "#ffffff", darkBackground = "#000000"}` (see
  [Device](device.md#the-launch-screen-platformsplash)).
- `widgets` are the app's home screen widgets and `shortcuts` its
  actions for Siri, Spotlight and the launcher (see
  [Device](device.md#home-screen-widgets-platformwidget)). The widget
  extension has a bundle ID of its own (the app's plus `.widgets`): a
  release needs a profile for it too, and `ios.widgetProfile` names one.
- `android.googleServices` is the Firebase project's
  `google-services.json`, for `platform/push`; `android.mapsKey`, a Google
  Maps Platform API key, for `platform/maps`.
- `ios.usage` gives the purpose strings of protected resources
  (`NSCameraUsageDescription`...; the Mac's too), and `ios.infoPlist` adds keys to
  Info.plist; `ios.backgroundModes` adds to `UIBackgroundModes` (`"audio"`,
  to record and play with the screen locked); `android.permissions` adds
  permissions.

The packages an app imports give it the permissions and purpose strings
they need: `platform/camera` the camera's, `platform/location` the location's.
Write your own in `ios.usage`: the default is generic. Some need a
capability on the App ID too (Push Notifications for `platform/push`, Sign
in with Apple for `platform/appleauth`, App Attest for `platform/integrity`,
Declared Age Range for `platform/agerange`, iCloud with its containers for
`platform/icloud`): turn it on at
developer.apple.com, then make the profile again. `asuka build --platform ios` (and
`--platform mac`) says which a profile lacks.

## iOS

```sh
asuka build --platform ios
```

makes a `.ipa` signed for the App Store: the app, linking only the
system frameworks it uses (WebKit, AVKit..., from the packages it
imports and the elements it shows), with the team's distribution
certificate (in your keychain, from Xcode's Accounts) and a provisioning
profile for the app's ID: `ios.profile`, or `--profile`, else the best
match installed. `--distribution ad-hoc` or `development` signs it for the
devices of its profile instead, and `--simulator` makes an unsigned `.app`
for the simulator.

An app that uses `std/sqlite` has SQLite compiled into it, as the rest
of the app is: there's no library of its own beside it to embed or
sign.

With `shareTarget`, the share extension is signed too, with a profile
for its own ID (`com.example.notes.share` for `com.example.notes`), from
the app's team: `ios.shareProfile`, else the best match installed. A
wildcard profile (`com.example.*`) serves both. Register the extension's
ID at developer.apple.com and make it a profile as you did the app's.

A `shareTarget` that takes files also needs the App Group
`group.<app ID>`, through which the extension hands the files to the
app. At developer.apple.com, register the group, give both IDs (the
app's and the extension's) the App Groups capability with it, then make
their profiles again. A wildcard ID can't have App Groups. The build
stops, saying so, if a profile lacks the group. The simulator needs
none of this. An app with its own share sheet (`platform/sharesheet`) needs
the group too, for its data, whatever it takes.

`asuka publish --platform ios` makes it and uploads it to App Store Connect (see
[Publishing](#publishing)); Apple's Transporter uploads one too.

**Privacy manifest.** The App Store requires one, giving a reason for
each API of Apple's list the app uses. Asuka writes it, from what the
binary uses: Asuka reads the clock and file times. It declares
no tracking, and the data the app collects as `ios.collected` lists it,
for what App Store Connect compares with the app's privacy labels:

```toml
[[ios.collected]]
type = "EmailAddress"
linked = true
purposes = ["AppFunctionality"]
```

`type` and `purposes` are Apple's names without their prefixes
(`NSPrivacyCollectedDataType`, `NSPrivacyCollectedDataTypePurpose`);
`linked` says the data is tied to the user's identity, `tracking` that
it's used to track them (see [the reference](asuka-toml.md#ios)). If
your app tracks, or its manifest needs more than this, write your own
as `ios/PrivacyInfo.xcprivacy`, with the reasons above in it too; Asuka
uses it instead, and `ios.collected` with it is ignored. SQLite is part
of the app's executable: the reasons Asuka writes cover what it uses
(file times, the disk's free space), and a manifest of your own needs
them too.

## Android

A release is signed with your upload key. Make one, once, and keep it
safe:

```sh
keytool -genkeypair -v -keystore upload.jks -alias upload -keyalg RSA -keysize 2048 -validity 10000
```

Name it in `asuka.toml` (`android.keystore`, `android.keyAlias`), and give
its password in the environment:

```sh
ASUKA_KEYSTORE_PASSWORD=... asuka build --platform android
```

(`ASUKA_KEY_PASSWORD` too if the key's differs; `ASUKA_KEYSTORE` and
`ASUKA_KEY_ALIAS` name them in CI.) It makes an `.aab` for Google Play, for
64-bit and 32-bit ARM phones and x86_64 (emulators, Chromebooks),
Android 7 and later: the app compiled for each architecture. Play makes
each phone's APK from it, with only that phone's architecture. An app
that uses `std/sqlite` has SQLite compiled into it for each
architecture. `asuka publish --platform android` makes it and uploads it to Google Play (see
[Publishing](#publishing)).

`--apk` makes a signed release `.apk` with every architecture instead,
to install directly (`adb install dist/<Name>.apk`): for testing a
release build, or for an app given out other than through a store.

**TVs**: with `asuka.toml`'s `[tv]` `android = true` the Android build is
an Android TV app too, the same `.aab` for phones and TVs. `asuka build
--platform tvos` makes the Apple TV app, an `.ipa` signed with a tvOS
profile, in `dist/tvos`, and `asuka publish --platform tvos` uploads it
(see [Publishing](#publishing)). See [TVs](tv.md).

## Publishing

`asuka publish` makes the release build, as `asuka build` does, and
uploads it through the store's API. It asks the store first what could
stop it (that the app is there and the key may upload to it, that the
version still takes builds, that the TestFlight groups exist), so
nothing is built for an upload that would fail. Each build is numbered
one above the store's highest (and at least `asuka.toml`'s `build`):
there's nothing to bump between releases. `--build 42` gives one.

**App Store Connect.** Make an API key in App Store Connect (Users and
Access › Integrations › App Store Connect API) with the App Manager
role, download its `.p8`, and name it in `asuka.toml`, or in the
environment (`ASUKA_ASC_KEY_ID`, `ASUKA_ASC_ISSUER_ID`, and
`ASUKA_ASC_KEY`, the `.p8`'s path or its text). Without a `key`, it's
looked for as `altool` does, `AuthKey_<id>.p8` in
`~/.appstoreconnect/private_keys`. Keep the `.p8` out of your
repository. Add the app in App Store Connect first (Apps › + › New
App), with its bundle ID.

```toml
[ios.appStoreConnect]
keyId = "ABC123DEF4"
issuerId = "57246542-96fe-1a63-e053-0824d011072a"
```

```sh
asuka publish --platform ios                                  # to TestFlight: the team's testers
asuka publish --platform ios --groups "Friends, QA" --notes "Try the new sync."
asuka publish --platform ios --review --notes @release-notes/ios --release manual
```

It uploads the `.ipa`, waits for Apple to process it (minutes, usually;
at times an hour or more), printing what Apple warns of or turns down,
then:

- `--notes` is TestFlight's What to Test and, with `--review`, the App
  Store's What's New: text, or `@file`, in the app's primary language;
  or `@dir`, a file per language, named as App Store Connect names it
  (`en-US.txt`, `ja.txt`). An update needs What's New in each of the
  version's languages.
- `--groups` gives the build to TestFlight groups, by name. With an
  external group (testers outside your team), the build goes to beta
  review; fill in the test information in TestFlight first.
- `--review` makes the App Store version (`asuka.toml`'s `version`) if
  there isn't one, gives it the build, and submits it for review. A
  first submission needs its listing, screenshots and privacy answers
  filled in App Store Connect first; what's missing is printed.
  `--release auto` releases it once approved, `--release manual` waits for
  you.
- `--no-wait` stops once Apple has the upload.

`asuka publish --platform mac --appstore` does the same for the Mac
App Store, with the same key: the `.pkg` (`--appstore` builds), the
app's macOS builds and version, TestFlight for Mac, and review.

`asuka publish --platform tvos` does it for the Apple TV app, with the
same key and options: the tvOS `.ipa`, signed with a tvOS App Store
profile for the app's ID (`--profile` picks one; the iPhone app's
profile doesn't sign it), the app's tvOS builds and version, TestFlight's
groups, and review. The iPhone app and the Apple TV app are one record
in App Store Connect, with one bundle ID and its in-app purchases: add
tvOS to the record first (the app's page, Add Platform › tvOS), which
`asuka publish` checks before it builds.

**Google Play.** Make a service account in Google Cloud, with a JSON key
(IAM › Service accounts › Keys), in a project with the Google Play
Android Developer API turned on. Invite its email in the Play Console
(Users and permissions), with the rights to release the app. Name its
key in `asuka.toml` (`serviceAccount = "play.json"` under `[android]`) or in
`ASUKA_PLAY_SERVICE_ACCOUNT` (its path or its JSON). Play takes an app's
first `.aab` only in the Play Console: create the app there and upload
its first build by hand; `asuka publish` uploads the ones after.

```sh
asuka publish --platform android                              # the internal test track
asuka publish --platform android --track beta --notes @release-notes/android
asuka publish --platform android --track production --rollout 0.1
```

- `--track` is `internal` (default), `alpha` (closed testing), `beta`
  (open testing), `production`, or a closed track's name.
- `--rollout 0.1` rolls the release out to a tenth of users; raise it in
  the Play Console. `--draft` makes a draft, to roll out there. Until the
  app is published, Play takes drafts only: the release becomes one.
- `--notes` are the release notes (at most 500 characters): text or
  `@file` in the listing's default language, or `@dir` with a file per
  language as Play names it (`en-US.txt`, `ja-JP.txt`).
- If the app has changes in review, it stops; `--cancel-review` cancels
  that review and sends them with this release. If Play holds the app's
  changes for you to send (after a rejection), they're saved, and you
  send them in the Play Console.

`--file dist/Notes.ipa` (or `.aab`) uploads a build `asuka build` made,
instead of making one.

## macOS

```sh
asuka build --platform mac
```

makes `dist/darwin-arm64/<Name>.app` on a Mac: an app for Macs with
Apple silicon, macOS 11 and later, and beside it
`<name>-<version>-darwin-arm64.zip`, the app to hand out (zipped as
Apple's tools do, its signature and notarization kept). `--arch amd64`
makes Intel Macs' in `dist/darwin-amd64` instead, a build of its own
(not one universal app of both, twice the size): each build's files
have names of their own, so they go in one release side by side. The
app has its executable, its settings (`Contents/Resources/asuka-app.json`), its
Info.plist, icon, languages and assets, signed ad hoc: it runs on your
Mac. The packages the app imports
give it their purpose strings, as on iOS, from `ios.usage` (location's
is `NSLocationUsageDescription` on the Mac, taken from
`NSLocationWhenInUseUsageDescription`), and the hardened runtime's
entitlements for them (`com.apple.security.device.camera`,
`...audio-input`, `...personal-information.location`...), without which
an app signed with a Developer ID is refused the camera, the microphone
or the user's data. An app's native packages' Swift is
compiled into its executable, for the build's architecture (see
[Native code](native.md)). An app that uses `std/sqlite` has SQLite
compiled into its executable too
([The library](data.md#the-library)).

Beside the `.zip` is `<name>-<version>-darwin-arm64.dmg`, a disk image
of the app, which is how a Mac app is usually downloaded: it opens to a
window with the app and the Applications folder side by side, to drag
one onto the other, and its icon is the app's. It's compressed (LZFSE),
signed as the app is, and with `--notarize` notarized and stapled too.
An app run from the disk image itself can't update itself
(`update.unsupported()` is `.translocated`): dragged to Applications,
it can. `--package dmg` makes the disk image alone, `--package zip` the
`.zip`; `asuka.toml`'s `mac.packages` says the same for every build:

```toml
[mac]
packages = ["dmg"]   # "zip" and "dmg" by default
```

To give it to others outside the Mac App Store, sign it with your
Developer ID and have Apple notarize it, so Gatekeeper opens it:

```sh
xcrun notarytool store-credentials asuka   # once: your Apple ID, team and an app-specific password
asuka build --platform mac --sign "Developer ID Application: Your Name (TEAMID)" --notarize asuka
```

`--notarize` sends the signed app to Apple's notary service, waits for
its verdict (a few minutes, usually), prints the service's log if it's
rejected, and staples the ticket to the app, so it opens offline too;
then the disk image, signed with the same identity, likewise.
In CI, `--notarize env` takes `ASUKA_APPLE_ID`, `ASUKA_APPLE_PASSWORD` (an
app-specific password) and `ASUKA_TEAM_ID` from the environment instead
of a keychain profile.

Push (`platform/push`), Sign in with Apple (`platform/appleauth`), App Attest
(`platform/integrity`) and iCloud (`platform/icloud`) need entitlements
only a provisioning profile grants.
Make a macOS Developer ID profile for the app's ID at developer.apple.com
(Profiles), with those capabilities on for the ID, and download it:

```sh
asuka build --platform mac --profile Notes.provisionprofile --notarize asuka
```

The profile goes in the app as `Contents/embedded.provisionprofile`, and
the app is signed with what its packages need of the profile's
entitlements (`com.apple.developer.aps-environment`,
`com.apple.developer.applesignin`,
`com.apple.developer.devicecheck.appattest-environment`), its app ID and
team, and the hardened runtime. The build says which a profile lacks.
Without `--sign`, the identity is the keychain's certificate the profile
allows (Developer ID Application); a `--sign` the profile doesn't list is
an error. `--profile` takes a path, or an installed profile's name or UUID;
`ASUKA_MAC_PROFILE` and asuka.toml's `mac.profile` say the same. With none,
an app that uses those packages is signed with the best match installed
for its ID (in Xcode's profile folders), else without one: the build
says what won't work on the Mac, and the rest of the app does.

`--appstore` makes `dist/darwin-arm64/<name>-<version>-darwin-arm64.pkg` for the Mac App Store instead: the app in the
App Sandbox (with the network and the files the user picks), signed
with a Mac App Store profile's certificate (Apple Distribution;
asuka.toml's `mac.appStoreProfile`, else `--profile`, else the best match),
and the package with the team's Mac Installer Distribution certificate.
`asuka publish --platform mac --appstore` builds it and uploads it to
App Store Connect as the iOS app goes: TestFlight's `--groups`,
`--review` for the Mac App Store, `--notes`, and `--file` for a `.pkg`
built before (see [Publishing](#publishing)). An app that needs a later
macOS for a package of its own (`platform/camera`'s macOS 13) says so in
its Info.plist. Notarizing is for apps outside the store: `--notarize`
with `--appstore` is an error.

## Windows

```sh
asuka build --platform windows               # x64, which Windows on Arm runs emulated
asuka build --platform windows --arch arm64  # native on Arm PCs
```

makes a folder, `dist/windows-amd64/<Name>` (`windows-arm64` for Arm):
`<Name>.exe`, the app (built for the
architecture by clang on Windows or by Zig on any system); beside it
`asuka-app.json`, its settings; and the app's assets. An app that uses
`std/sqlite` has SQLite compiled into the `.exe`. The `.exe` is a GUI
program (no console window) with its resources linked in: the icon at Windows's sizes, its version
information (`version` and `build`), and a manifest that asks for the
common controls 6, per-monitor DPI awareness and UTF-8, for Windows 10
and 11, and runs it as the user who opens it (no administrator's
rights, whatever its name). An app with a resource object of its own in its directory (a
COFF `.syso`, `rsrc_windows_amd64.syso`) has its own resources instead.

Beside the folder are the packages to hand out, as electron-builder
makes them, each built by `asuka build` from any system (no NSIS, WiX
or Windows SDK):

- `<name>-<version>-windows-amd64-setup.exe`, the installer: one click,
  as most Windows apps install now. It installs the app for the user
  who runs it, with no administrator's rights and no questions, into
  `%LOCALAPPDATA%\Programs\<Name>` (where the app can
  [update itself](#updates)), with a shortcut in the Start menu, and
  lists it in Settings' Installed apps with its version, publisher, size
  and an uninstaller; then it opens the app. Run again (a newer
  version's), it updates the app in place, closing it first if it's
  open (it asks), and removes what the version before had and this one
  hasn't. The uninstaller, `Uninstall <Name>.exe` in the app's folder,
  takes away the app, its shortcuts and what it registered (its URL
  scheme, its file types, its notifications'); what the app saved, its
  data, stays. For scripts and IT departments both take `/S`, silently
  (the installer doesn't open the app then), and the installer
  `/D=<folder>` for another folder, as NSIS's do. Both speak the user's
  Windows display language, any of those Asuka is translated to
  (English otherwise; right to left in Arabic and Hebrew), and call the
  app by its catalogs' `asuka.appName` in the language of the app's that
  the user reads best ([What the system shows](i18n.md#what-the-system-shows));
  `/LANG=de` picks another language. The Start menu and Installed apps
  show the app's `name`, as its own shortcut does.
- `<name>-<version>-windows-amd64.zip`, the folder zipped, to unzip
  anywhere: a portable copy, which updates itself too where the user
  can write.
- `<name>-<version>-windows-amd64.msix`, with `--package msix`: the app
  as a package, for the [Microsoft Store](https://partner.microsoft.com/dashboard)
  and for sideloading (`Add-AppxPackage`, App Installer, Intune). Its
  manifest declares the app as a full-trust desktop app, its tiles'
  images (from the `icon`), its URL scheme, its file types and its
  notifications' activator, which a packaged app can't register as it
  runs, and the catalogs' languages (the source language first; English
  without catalogs), which the Store lists the app in. Its name isn't
  translated there: that takes a `resources.pri`, which the build
  doesn't write, so Start and Installed apps show the app's `name`.
  The Store takes it unsigned and signs it: set `windows.identityName`
  and `windows.publisherId` to what Partner Center says (Product
  identity), and upload it. To sideload it, it's signed as the `.exe` is
  (below), by a certificate whose subject is `windows.publisherId`
  (`CN=` and `windows.publisher` by default) and which the PC trusts.
  Windows installs, updates and removes an MSIX; the app doesn't update
  itself (`update.unsupported()` is `.managed`).

`--package installer,msix` makes some of them, of `zip`, `installer`
and `msix`; `asuka.toml`'s `windows.packages` says the same for every
build. By default it's the installer and the `.zip`:

```toml
[windows]
packages = ["installer", "zip", "msix"]
publisher = "Example Inc."
homepage = "https://example.com"
```

Sign the app, so SmartScreen knows who it's from, with a code signing
certificate exported as a `.pfx`: the build signs the `.exe`, the
installer and its uninstaller, and the MSIX.

```sh
ASUKA_WINDOWS_CERT=cert.pfx ASUKA_WINDOWS_CERT_PASSWORD=... asuka build --platform windows
```

The build signs it with SHA-256 and has the signature timestamped, so
it stays valid once the certificate expires, by the first RFC 3161 server
that answers: `ASUKA_WINDOWS_TIMESTAMP`'s (a comma-separated list), else
DigiCert's, Sectigo's and GlobalSign's. When none answers the build
fails, saying so; `ASUKA_WINDOWS_TIMESTAMP=none` signs without a timestamp
(the signature then ends with the certificate). It signs with `signtool` on
Windows (the Windows SDK's; on the `PATH`, else the newest installed),
with `osslsigncode` elsewhere (`brew install osslsigncode`, `apt install
osslsigncode`). Without `ASUKA_WINDOWS_CERT` the `.exe` isn't signed. A
certificate on a hardware token or in a cloud service (Azure Trusted
Signing, a key vault) signs through its own tool: set
`ASUKA_WINDOWS_SIGN_COMMAND` to its command, `%1` where the file goes,
and the build runs it for each file to sign instead:

```sh
ASUKA_WINDOWS_SIGN_COMMAND='AzureSignTool sign -kvu https://vault.vault.azure.net -kvc cert -kvm -tr http://timestamp.digicert.com -td sha256 %1' \
  asuka build --platform windows
```

An app that shows notifications (`platform/notify`) is given a shortcut in the
user's Start menu the first time, which Windows needs for an app's
toasts: its AppUserModelID is asuka.toml's `id` (else one of its name).

## Linux

```sh
asuka build --platform linux                # this machine's architecture, else amd64
asuka build --platform linux --arch arm64   # amd64 or arm64
```

It builds from any system, with no GTK installed to build against
(the app opens GTK 4 and libadwaita as it starts): with this machine's
C compiler on Linux, with Zig elsewhere. In `dist/linux-amd64`
(`linux-arm64` for `--arch arm64`) it makes:

- `<name>/`, a folder laid out as an install prefix, named after the
  app in lower case (`My Notes`: `my-notes`):
  - `bin/my-notes`, the app, one executable;
  - `share/my-notes/assets` and `share/my-notes/asuka-app.json`, its
    assets and settings, which it finds from `bin/` too;
  - `share/applications/<id>.desktop`, what launchers show and run;
  - `share/icons/hicolor/<size>/apps/<id>.png`, the `icon` at each size
    from 16 to 512 pixels;
  - `share/metainfo/<id>.metainfo.xml`, its AppStream metainfo, what
    software centers show.

  Run `bin/my-notes` from it, or install it into a prefix, where
  launchers find it:

  ```sh
  tar -xzf my-notes-1.0-linux-amd64.tar.gz --strip-components=1 -C ~/.local
  ```

- `my-notes-<version>-linux-<arch>.tar.gz`, that folder, to hand out.
- `my-notes-<version>-linux-<arch>.deb`, for Debian and Ubuntu, and
  `my-notes-<version>-linux-<arch>.rpm`, for Fedora: the folder
  installed under `/usr`, as the system's own apps are, which the
  package manager installs with what the app needs from the
  distribution: GTK 4.14, libadwaita 1.5, libsoup and the glibc its
  executable was built for, and what the app's packages open (WebKitGTK
  for a `WebView`, Poppler for `platform/pdf`, GStreamer for audio and
  video...). The desktop's caches of its desktop file, icons and MIME
  types are updated by the system's own triggers. A prerelease's
  version comes before its release's (`1.3.0-beta.1` is `1.3.0~beta.1`).
  With your OpenPGP key ([below](#signed-packages-and-repositories)),
  the `.rpm` is signed, as `rpm --addsign` signs one:

  ```sh
  sudo apt install ./my-notes-1.0-linux-amd64.deb
  sudo dnf install ./my-notes-1.0-linux-amd64.rpm
  ```

- `my-notes-<version>-linux-<arch>.AppImage`, the app in one file, to
  download and run, on a distribution with GTK 4.14 and libadwaita 1.5
  (they aren't in it): its desktop file and icon are where AppImage's
  tools and launchers find them. Where FUSE isn't (a container), run it
  with `--appimage-extract-and-run`. With [updates](#updates) it
  updates itself as the `.tar.gz`'s prefix does, and carries the update
  information that AppImageUpdate, appimageupdatetool and
  AppImageLauncher read (`--appimage-updateinformation` prints it): the
  latest GitHub release's `.zsync`, or `url`'s, which `asuka build`
  writes beside the update. It's made with the [AppImage
  runtime](https://github.com/AppImage/type2-runtime), downloaded once
  into the build cache and checked; to build offline, set
  `ASUKA_APPIMAGE_RUNTIME` to a `runtime-x86_64` or `runtime-aarch64`
  of yours.
- `<id>.yml`, a [Flatpak](https://flatpak.org) manifest that packages
  that folder (its `bin` and `share`) on the GNOME runtime:

  ```sh
  flatpak install flathub org.gnome.Platform//50 org.gnome.Sdk//50   # once
  flatpak-builder --user --install --force-clean build dist/linux-amd64/com.example.notes.yml
  flatpak run com.example.notes
  ```

The app needs GTK 4.14 and libadwaita 1.5 or later where it runs (the
GNOME runtime 46 or later has them). Its ID is its application ID: it
names its desktop file, its icon and its D-Bus name (a second launch
shows the first's window), and its data directory
(`~/.local/share/<id>`); it must be letters, digits, `_` and `-`, in two
parts or more, none starting with a digit.

`--package deb,appimage` makes some of them alone, of `tar.gz`, `deb`,
`rpm`, `appimage` and `flatpak` (the manifest); `asuka.toml`'s
`linux.packages` says the same for every build. A build with
[updates](#updates) writes the update's archive, the `.tar.gz`, either
way. Each is written by `asuka build` itself, from any system: no
`dpkg`, `rpmbuild` or `mksquashfs` needed.

An app the `.deb`, the `.rpm` or Flatpak installed is updated by them,
not by itself (`update.unsupported()` is `.managed`): the `.tar.gz`
unpacked into a prefix of the user's and the AppImage, in a folder the
user can write, are the ones that update themselves.

What the desktop file, metainfo and packages say, in `asuka.toml`:

```toml
[linux]
summary = "Write things down"
description = """
Notes, kept on your computer.

A second paragraph."""
categories = ["Office"]
keywords = ["memo"]
license = "MIT"
developer = "Example Inc."
homepage = "https://example.com"
flatpakRuntime = "50"
packages = ["tar.gz", "deb", "rpm", "appimage", "flatpak"]   # all by default
maintainer = "Example Inc. <dev@example.com>"
debDepends = ["libfoo1 (>= 1.2)"]
rpmRequires = ["foo >= 1.2"]
```

`categories` are [freedesktop.org's](https://specifications.freedesktop.org/menu-spec/latest/category-registry.html)
(`Utility` by default; the `.deb`'s section follows them); `license` is
an SPDX expression; Flathub and software centers want the summary,
description, license and developer. `maintainer` is who the `.deb` and
`.rpm` say maintains them (`developer` by default); `debDepends` and
`rpmRequires` are more packages they depend on, in each one's syntax
(`a | b` is either, in a `.deb`; `(a or b)` in an `.rpm`).

### Signed packages and repositories

Users who add your apt or dnf repository once get each version with the
system's updates (`apt upgrade`, `dnf upgrade`, the software center),
checked against your key. Make the OpenPGP key (RSA 4096) that signs
them, once:

```sh
asuka linux keygen --uid "Example Inc. <dev@example.com>"
```

It writes `asuka-linux.key`, the secret key, and `asuka-linux.asc`, the
public key, to `asuka/linux-keys` in your configuration directory
(`--out` chooses another). Keep the secret out of the repository, as the
update key: in a password manager, and in CI's secrets. Users' systems
trust the key once: a repository is never signed with another.
`ASUKA_LINUX_GPG_KEY` is the key (or its file's path) for builds in CI,
`ASUKA_LINUX_GPG_PASSPHRASE` its passphrase when it has one (set as the
key is made, it encrypts it); `linux.signingKey` is its path on your
machine:

```toml
[linux]
signingKey = "~/.config/asuka/linux-keys/asuka-linux.key"

[linux.repository]
url = "https://packages.example.com/notes"   # where it's served
s3 = { bucket = "packages", prefix = "notes", endpoint = "https://<account>.r2.cloudflarestorage.com" }
```

With the key, `asuka build` signs the `.rpm`. `asuka publish --platform
linux` then writes the `.deb` and the `.rpm` into the repository that
`linux.repository` names (its `s3` bucket, or a directory, `dir`), or
`--repo` (a directory, or `s3://bucket/prefix`), signed, keeping the
versions it has; the update goes where `updates` says, as before, when
there are `updates`:

```sh
asuka publish --platform linux --repo ../packages
asuka publish --platform linux --arch arm64 --repo s3://packages/notes
```

In it, all written by `asuka publish` itself, in Go:

- `key.asc` and `key.gpg`, the public key, armored and binary;
- `deb/`, the apt repository: `dists/stable/InRelease` (and `Release`,
  `Release.gpg`), each architecture's `Packages`, the packages in
  `pool/`;
- `rpm/stable/`, the dnf repository: `repodata/repomd.xml`, signed
  (`repomd.xml.asc`), the files it names, the packages in `Packages/`;
  and `rpm/notes.repo`, dnf's configuration of it.

A prerelease goes to its channel's suite and repository (`beta`, and
`rpm/notes-beta.repo`), which the beta's users add beside `stable`.
Publish one architecture after the other: each adds to what the last
wrote. In an S3 bucket the indexes are served uncached; the credentials need
`s3:GetObject`, `s3:PutObject`, `s3:DeleteObject` and `s3:ListBucket`
(without which S3 answers 403 for a file that isn't there yet). Serve
the directory over HTTPS from a web server or CDN.

What users run, once (`asuka publish` prints it with your `url`). On
Debian and Ubuntu:

```sh
curl -fsSL https://packages.example.com/notes/key.asc | sudo gpg --dearmor -o /etc/apt/keyrings/notes.gpg
echo "deb [signed-by=/etc/apt/keyrings/notes.gpg] https://packages.example.com/notes/deb stable main" | sudo tee /etc/apt/sources.list.d/notes.list
sudo apt update && sudo apt install notes
```

On Fedora (`gpgcheck` and `repo_gpgcheck` on: dnf checks the packages
and the repository's metadata, and asks to trust the key once):

```sh
sudo dnf config-manager addrepo --from-repofile=https://packages.example.com/notes/rpm/notes.repo
sudo dnf install notes
```

(`sudo dnf config-manager --add-repo …` before Fedora 41.) `rpm -K`
checks a downloaded `.rpm` against the keys rpm trusts (`sudo rpm
--import https://packages.example.com/notes/key.asc`).

## The web

```sh
asuka build --platform web
```

makes a static site in `dist/web`: `index.html`, the app as
WebAssembly (built by Zig), Asuka's scripts, the app's service worker, its
assets, a web app manifest with its icons, gzipped copies, and a
`_headers` file for Cloudflare and Netlify. The files are named by their
digests, so all but `index.html` and the manifest can be cached for
good. Serve the folder from any web server or CDN, `.wasm` as
`application/wasm`, over HTTPS (service workers, Web Push and the secure
store need it). See [the web](web.md) for what's in it and what a
browser does differently.

It also writes `version.json`, the version built and its release notes
(`CHANGELOG.md`'s section), which `platform/update` reads in the page: a
page older than the site offers to reload ([Updates](#updates)).

## Updates

A desktop app updates itself as Mac apps do with Sparkle: it checks a
feed for a newer version, shows its release notes, downloads it (only
what changed, when there's a delta from the version it runs), checks
that it's signed with your key, installs it and relaunches. Phones
update from their stores, and between store releases their code over
the air ([below](#phones-updates-of-the-apps-code)); a web app's page
reloads for a newer site.

### Set up

Make the key pair that signs updates, once:

```sh
asuka update keygen
```

It writes `asuka-update.key`, the secret key, and `asuka-update.pub`, the
public key, to `asuka/update-keys` in your configuration directory
(`--out` chooses another), and prints what to add to `asuka.toml`. Keep
the secret key out of the repository: in a password manager, and in your
CI's secrets. Installed apps take only updates signed with it, so losing
it strands them, and whoever has it can ship code to your users.

```toml
[updates]
publicKey = "…the contents of asuka-update.pub…"
github = "you/notes"
```

| Key | What it is |
|---|---|
| `publicKey` | `asuka-update.pub`'s contents, built into the app |
| `github` | `owner/name`: a public repository whose releases, tagged `v1.2.0`, hold the updates; apps read the latest release's |
| `tagPrefix` | the releases' tags' prefix (default `v`); another, as `desktop-v`, lets the repository's latest release be another's (a CLI's): apps then find the newest release tagged with it through the GitHub API |
| `url` | instead of `github`: an HTTPS directory that serves the files (an S3 or R2 bucket, a CDN, your server); apps read `url/update-<target>.json` |
| `feed` | a server of yours that apps ask instead (with `url`, where the files are): see [your own server](#your-own-update-server) |
| `s3` | the bucket that serves `url`, which `asuka publish` uploads to: `bucket`, `prefix` (its directory), `region`, `endpoint` (R2's, MinIO's; default Amazon S3), `pathStyle` |
| `privateKey` | the secret key's path (`~/` the home directory), for builds on your machine |
| `changelog` | a Markdown file whose `## 1.2.0` (or Keep a Changelog's `## [1.2.0] - 2026-11-13`) section is the release notes; default `CHANGELOG.md`, when there is one, which must then have the version's section |
| `deltas` | how many versions before get a delta (default 3; 0: none) |

The environment says the same for CI and tests: `ASUKA_UPDATE_PRIVATE_KEY`
(the secret key itself, or its file's path), `ASUKA_UPDATE_PUBLIC_KEY` and
`ASUKA_UPDATE_URL` (over `publicKey` and `url`).

### In the app

`platform/update`'s `UpdateWindow` is the update window: show it once, in
the app's main window, where it takes no room. Its `AppMenu` puts "Check
for Updates…" in the app's menu after About on the Mac (a Help menu on
Windows and Linux); `CheckForUpdates` is that item alone, for a menu of
your own or a tray icon's:

```asuka
import platform/update

app {
  Window {
    title: "Notes"
    update.UpdateWindow { icon: "logo.png" }  // an image in assets/
    MenuBar { update.AppMenu() }
    Text("Notes")
  }
}
```

It checks 10 seconds after launch, then once a day (its `interval`), and
when a version is out it shows its release notes with **Install
Update**, **Remind Me Later** and **Skip This Version** (checks in the
background don't offer that version again, but offer the next), and
"Automatically download and install updates in the future": then checks
install what they find without asking, and it runs at the next launch.
Installing shows the download's progress, then **Relaunch Now**.
`update.checkForUpdates()` checks as the user asked (the menu item, a
button): the window also says when the app is up to date or can't
update itself. `automaticChecks()`, `setAutomaticChecks(on)`,
`automaticDownloads()`, `setAutomaticDownloads(on)`, `lastCheck()` and
`skippedVersion()` are the user's choices, kept on the device
(std/storage), for a settings page; `UpdateWindow { automaticChecks:
false }` doesn't check in the background until the user turns it on.
The window's words are Asuka's, in the app's language.

An interface of the app's own uses `check()`, `Release.install` and
`relaunch()`, or follows the updater's state, `update.updater`, a store
(its `phase`: checking, available, downloading with `downloaded` and
`total`, ready...):

```asuka
import platform/update

component UpdateBanner {
  state found: update.Release? = none
  state done = 0.0

  task {
    do {
      found = try await update.check()  // none: up to date
    } catch e {
      print(e.message)
    }
  }

  if let r = found {
    Row {
      gap: 8
      Text("Notes {r.version} is out")  // r.notes: Markdown; r.date, r.size
      Button("Install") {
        on press {
          do {
            try await r.install(progress: (got, total) => { done = float(got) / float(total) })
            update.relaunch()  // quits (on quit may cancel), then starts the new version
          } catch e {
            print(e.message)
          }
        }
      }
      if done > 0 { Progress { value: done } }
    }
  }
}
```

`await update.unsupported()` says why the app can't update itself, or
none when it can: a development build or one without `updates`
(`.notConfigured`), an app the system's packages (the `.deb`, the
`.rpm`), Flatpak, Snap, the Mac App Store or the Microsoft Store
installed (`.managed`: they update it), a Mac app run from its
disk image or from where it was downloaded, which macOS translocates
(`.translocated`: the user moves it to Applications), a folder the user
can't write (`.notWritable`), a phone (`.notAvailable`). The window
never checks in the background then, and says why when the user checks.

`update.setChannel("beta")` offers a channel's prereleases too, for the
users who chose your beta: a version's channel is its prerelease's
letters (`1.4.0-beta.2` is `beta`'s), `"*"` takes every prerelease.
Releases are offered on every channel. `update.setFeed(url)` reads
another feed than `asuka.toml`'s (one chosen by the user's account): a
manifest's URL, GitHub's releases, or a server's URL with `{target}`,
`{version}` and `{channel}`; the signatures are checked all the same.
`update.compareVersions(a, b)` orders versions as the updater does.

On the web there's nothing to install: `check()` reads the site's
`version.json` past the browser's caches, and a newer version's
`relaunch()` reloads the page.

In tests there's no update module, as on a phone, until
`update.fakeHost(update.FakeApp(…))` makes the test answer as an
installed app would and serve the feed's files (`host.serve(update.Served(url:,
file:))`); `host.log()` says what it downloaded and installed.

### Build and publish

With `updates` and the secret key, `asuka build --platform mac`,
`windows` or `linux` writes in `dist/updates`, beside the build:

- `notes-1.2.0-<target>.tar.gz`, the app as installed, signed (Ed25519
  over its SHA-256): the bundle; the Windows folder's entries; on Linux,
  the build's `.tar.gz` itself. The targets are `darwin-arm64`,
  `darwin-amd64`, `windows-amd64`, `windows-arm64`, `linux-amd64` and `linux-arm64`.
- delta updates from the versions published before, as
  `notes-1.1.0-to-1.2.0-<target>.delta`: it downloads them (checking
  their signatures) from where the published manifest says, and writes
  what changed: files moved or copied from the installed app, bsdiff
  patches of those that changed, the rest. Apps download only the delta
  of their version, and the whole archive when it fails (the app was
  changed since it was installed). A first version has none.
- on Linux, the build's AppImage too, signed, which an app run from an
  AppImage downloads whole in place of the archive (the manifest's
  `appimage`), and its `.zsync` for AppImageUpdate: named for the
  version on GitHub (`notes-1.2.0-linux-amd64.AppImage.zsync`, which
  the AppImage finds in the latest release), for none with `url`
  (`notes-linux-amd64.AppImage.zsync`, a channel's
  `notes-linux-amd64-beta.AppImage.zsync`), which goes up with the
  manifests. With a `tagPrefix` of your own the AppImage carries no
  update information: the repository's latest release is another's.
- `update-<target>.json`, the manifest: the version, its release notes,
  the date, the archive's and deltas' URLs, sizes and signatures, and
  the versions before (for the next deltas). A prerelease's is
  `update-<target>-beta.json`, which only apps on its channel read.

Without the key it builds the app and writes no update files, saying
so. `--version 1.2.0` builds another version than `asuka.toml`'s (CI takes
it from the tag). A Windows folder and a Linux prefix also get
`.asuka-files`, the list of what the version installs, so that its
update removes what the next one no longer has, and nothing of yours.

`asuka publish` builds and uploads them, with the build's packages to
hand out (the Mac's `.zip` and `.dmg`; Windows' installer, `.zip` and
`.msix`; Linux's `.deb`, `.rpm` and AppImage, its `.tar.gz` being the
update's archive), the manifests last,
so no app reads a manifest whose files aren't there yet:

```sh
asuka publish --platform mac --sign "Developer ID Application: …" --notarize asuka
asuka publish --platform mac --arch amd64 --sign "Developer ID Application: …" --notarize asuka
asuka publish --platform windows --arch arm64
asuka publish --platform linux --draft
```

- With `github`, to the release of the version's tag (`v1.2.0`), with the
  [GitHub CLI](https://cli.github.com) signed in (`gh auth login`, or
  `GH_TOKEN`): made a draft if there's none, then published, as a
  prerelease for a prerelease, and not as the latest with a `tagPrefix`
  of your own. `--draft` leaves it a draft, to publish once every
  platform is there (`gh release edit v1.2.0 --draft=false`): apps check
  the latest published release.
- With `s3`, to the bucket, the manifests with `Cache-Control: no-cache`
  (their names stay from version to version), with
  `AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY` (and `AWS_SESSION_TOKEN`),
  the region from `s3.region`, `AWS_REGION` or `us-east-1` (which R2
  takes). No other tool.
- With `url` alone, upload `dist/updates` there yourself, the manifests
  last, and keep the earlier versions' files.

### Your own update server

With `feed`, apps ask your server instead, its URL with `{target}`,
`{version}` (the version they run) and `{channel}` filled in:

```toml
[updates]
publicKey = "…"
url = "https://downloads.example.com/notes"
feed = "https://api.example.com/updates?target={target}&version={version}&channel={channel}"
```

It answers `204 No Content` (or 404) when there's nothing newer, else
with a manifest, the JSON `asuka build` writes:

```json
{
  "version": "1.2.0",
  "notes": "- Faster sync.\n- Fixed the sidebar.",
  "date": "2026-11-13T09:00:00Z",
  "url": "https://downloads.example.com/notes/notes-1.2.0-darwin-arm64.tar.gz",
  "size": 9123456,
  "signature": "…base64…",
  "deltas": [
    {"from": "1.1.0", "url": "https://…/notes-1.1.0-to-1.2.0-darwin-arm64.delta", "size": 21432, "signature": "…"}
  ]
}
```

`asuka build` writes the files for `url`; serve what it wrote, or pick
among manifests as you like (a staged rollout, a channel per account).
The signatures are the ones `asuka build` made: the server only chooses.

### Where apps update

- macOS: the new bundle, checked (its code signature valid, of the
  running app's team and identifier, its quarantine attribute taken off),
  waits beside the app until it quits, however it quits: a helper then
  puts it in the old one's place and, when the app asked to relaunch,
  opens it through Launch Services, so the Dock keeps one tile. The app's
  folder must be one the user can write: `/Applications` for an
  administrator, `~/Applications`.
- Windows: the folder's entries are replaced while the app runs, each
  renamed aside first and everything put back if one can't be; the
  running `.exe` is renamed (Windows lets it), and removed at the next
  launch. The folder must be the user's (not `Program Files`): the
  installer's (`%LOCALAPPDATA%\Programs\<Name>`), or the `.zip` unzipped
  in their profile; Installed apps then shows the version the app
  updated to. An MSIX updates through the Store or App Installer.
- Linux: the tarball's prefix, such as `~/.local`: the app's files alone
  (`bin/notes`, `share/notes/`, its desktop file, icons, metainfo and MIME
  types), each by a rename, then the desktop's caches
  (`update-desktop-database`, `update-mime-database`). An AppImage: the
  new one, downloaded beside it and checked, takes its file's place by a
  rename (whatever the user named it, in a folder they can write, as
  `~/Applications`), and relaunching starts it, extracted again when the
  running one was (`--appimage-extract-and-run`). A Flatpak, a Snap or a
  distribution's package updates through its own channel: there the app
  doesn't update itself.

### Phones: updates of the app's code

Between store releases a phone app (and an Apple TV app) takes updates
of its code over the air, as Expo's updates do: its Asuka code, which the core's VM runs in
place of the code compiled into the build, signed with the same key as
desktop updates. A build made with asuka.toml's `[updates]` takes them:

```sh
asuka build --platform ios --update      # in dist/updates: the update, signed, and its manifest
asuka publish --platform ios --update    # built, and uploaded where [updates] says
```

`--platform android` makes Android's, which Android TV takes too (it's
the same build), and `--platform tvos` Apple TV's, in `dist/tvos/updates`.
Each platform's updates are its own: an update is made for one
platform's builds, its manifest is named for it
(`update-tvos-<fingerprint>.json`), and an iPhone never takes an Apple
TV's update, nor an Apple TV an iPhone's. Publish one for each platform
the change is for.

An update fits only the builds whose native side it was made against:
their *fingerprint*, of the CLI's version, the platform, the parts of
the host the build has (a `Video`'s player, a `WebView`'s WebKit: those
the app shows), the native code of the packages the app imports and its
own, the builtins the build links, its assets and asuka.toml (but for
its version and build). An Apple TV build's differs from the iPhone
build's: its host has fewer parts, and only the packages that list tvOS
are built into it. An update that needs more (a new native package or
asset, a permission, an element the build doesn't show) is for the next
store build: its manifest names a fingerprint no installed build has,
and it reaches none.

In the app, `update.UpdateWindow()` checks soon after launch and once a
day, downloads what it finds, and the next launch runs it; there's no
window on a phone. `update.check()`, `Release.install()` and `relaunch()`
(which runs it at once) do it by hand, and `update.version()` says what
runs (`1.4.0+1764150000`: the update's number after the `+`). A launch of
an update that ends in its first seconds twice in a row, as a crash at
startup does, goes back to the build's code, and that update isn't taken
again. On Apple TV, the update is kept in the app's caches, which tvOS
may clear while the app isn't running: the app then runs the build's
code until it downloads the update again.

An update's code runs some times slower than compiled code (its own
logic: layout, signals and the views stay native) until the next store
build compiles it. Keep to what the stores allow: Apple lets an app
download interpreted code that doesn't change its purpose, and App
Review rules out features review didn't see (2.5.2, 2.5.19); Google
Play allows code that runs in an interpreter. Updates are for fixes and
small changes.

## In an existing app

An Asuka app can live inside an app made in Xcode or Android Studio: a
feature written once in Asuka, shown from a native app. (On the web, the
build's app runs in an element of a page of yours: see
[the web](web.md).)

**iOS.** `asuka build --platform ios --xcframework` makes `dist/AsukaApp.xcframework`:
the app, for devices and the simulator, with
a Swift module interface and the app's assets. Add it to your target
(Embed & Sign), then show the Asuka app as any view controller:

```swift
import AsukaApp

present(AsukaViewController(), animated: true)
// or push it, or make it a tab, or a window's root
```

An app that uses `std/sqlite` has SQLite in the framework, its names
its own: a SQLite of your app's doesn't mind it.

**Android.** `asuka build --platform android --aar` makes a Maven repository in
`dist/maven`: the app's library for each ABI, its
assets, and `AsukaActivity` declared, with its dependencies (Material,
AndroidX) in its POM, and the optional modules the app uses (`platform/push`,
`platform/maps`) beside it; SQLite, when the app uses `std/sqlite`, is in
the app's library. Add the repository and the dependency the command
prints, then start the activity:

```kotlin
// settings.gradle.kts
dependencyResolutionManagement { repositories { maven { url = uri("path/to/dist/maven") } } }
// app/build.gradle.kts
dependencies { implementation("dev.asuka.counter:counter:1.0.0") }
// anywhere
startActivity(Intent(this, dev.asuka.AsukaActivity::class.java))
```

The embedded app is one Asuka app, mounted once, as in a standalone
build. What `asuka.toml` sets for a whole app (its icon, URL scheme,
share target, widgets, permissions' texts) is the embedding app's to
set in its own project.

## Symbols for crash reports

A native crash's frames are addresses in the app's binaries; a crash
service (or App Store Connect, or Google Play) turns them into
functions and lines with the build's symbols. An error the app
doesn't catch is reported by Asuka with its message, whatever the
symbols.

**iOS.** `asuka build --platform ios` builds the app with debug information
and makes a dSYM of each binary (the app, its
framework when it has a share sheet, its extensions), as Xcode's archive
does, then strips the binaries. They're in `dist/<Name>.dSYMs`, beside
the `.ipa` (or the simulator's `.app`), and their symbols in the `.ipa`
(`Symbols/`), so App Store Connect and Xcode's Organizer symbolicate
the crashes they show. Upload the dSYMs to your crash service:

```sh
sentry-cli debug-files upload --org ORG --project PROJECT dist/Notes.dSYMs
# Crashlytics' tool, from its SDK:
upload-symbols -gsp GoogleService-Info.plist -p ios dist/Notes.dSYMs
```

**Android.** `asuka build --platform android` keeps the app's symbol tables
for Gradle, which strips the libraries it packages and puts their
symbols in the `.aab`, with R8's map of the Kotlin it renamed: Google
Play symbolicates native crashes and ANRs, and deobfuscates stack
traces, with them. For a crash service they're beside the `.aab` too:
`dist/<Name>-native-debug-symbols.zip` and `dist/<Name>-mapping.txt`.

```sh
sentry-cli debug-files upload --org ORG --project PROJECT dist/Notes-native-debug-symbols.zip
sentry-cli upload-proguard --org ORG --project PROJECT dist/Notes-mapping.txt
```

**The Mac, Windows, Linux and the web.** Apps are built
without debug information, and on Windows and Linux without their
symbol tables (a Mac's binary keeps its global symbols): a crash
report's frames are each a binary and an offset, named where the binary
has the name. There are no symbols to upload.

Keep each release's symbols: a crash is symbolicated with those of the
build it happened in. See [Device](device.md#crash-reports-platformcrash)
for the reports themselves.

## Before you release

Run the release build where your users will, before you release it:

- On a phone: an iOS build with `--distribution development`, and the
  Android build from Play's internal test track (`asuka publish
  --platform android --track internal`), which installs the APK Play makes
  from the `.aab`. TestFlight and a Play internal test also check what a
  simulator can't.
- On the Mac: the signed, notarized `.dmg`, downloaded on another Mac
  (a download is what Gatekeeper checks), the app dragged to
  Applications; `spctl --assess --type execute -vv Notes.app` says what
  Gatekeeper will make of it.
- On Windows and Linux: the folder, or on Linux the `.deb`, `.rpm`,
  AppImage, `.tar.gz` or Flatpak, on a machine that has never had the app
  (Linux: with GTK 4.14 and libadwaita 1.5 or later).
- On the web: the built site served over HTTPS, in each browser you
  support.

## In CI

This GitHub Actions workflow builds both apps when you push a tag like
`v1.2`, uploads the iOS build to TestFlight and the Android build to
Play's internal test track with `asuka publish`, and attaches the `.ipa`
and the `.aab` to a draft GitHub release for the tag, each as soon as
it's uploaded; publish
the draft when you're ready. (Run by hand, it keeps them as the run's
artifacts only.) It expects the app at the root of its repository. Do
your first uploads by hand: Play won't take one through its API until
the app has had one.

```yaml
# .github/workflows/release.yml
name: release
on:
  push:
    tags: ["v*"]
  workflow_dispatch:
permissions:
  contents: write # to attach the builds to the tag's release
env:
  ASUKA_VERSION: 0.1.0 # the Asuka the app is made with

jobs:
  release:
    if: startsWith(github.ref, 'refs/tags/')
    runs-on: ubuntu-latest
    steps:
      - run: gh release create "$GITHUB_REF_NAME" --draft --generate-notes --verify-tag
        env:
          GH_TOKEN: ${{ github.token }}
          GH_REPO: ${{ github.repository }}

  ios:
    needs: release
    if: ${{ !failure() && !cancelled() }} # and when run by hand, which skips release
    runs-on: macos-latest
    steps:
      - uses: actions/checkout@v4
      - name: the asuka CLI, at the app's version of Asuka
        run: |
          curl -fsSL https://asuka.egoist.dev/install.sh | sh
          echo "$HOME/.asuka/bin" >> "$GITHUB_PATH"
      - name: the distribution certificate and the profile
        env:
          CERTIFICATE: ${{ secrets.IOS_CERTIFICATE }}
          CERTIFICATE_PASSWORD: ${{ secrets.IOS_CERTIFICATE_PASSWORD }}
          PROFILE: ${{ secrets.IOS_PROFILE }}
        run: |
          KC=$RUNNER_TEMP/signing.keychain-db
          PW=$(openssl rand -hex 16)
          security create-keychain -p "$PW" "$KC"
          security set-keychain-settings -lut 3600 "$KC"
          security unlock-keychain -p "$PW" "$KC"
          echo "$CERTIFICATE" | base64 --decode > $RUNNER_TEMP/certificate.p12
          security import $RUNNER_TEMP/certificate.p12 -k "$KC" -P "$CERTIFICATE_PASSWORD" -T /usr/bin/codesign
          security set-key-partition-list -S apple-tool:,apple: -k "$PW" "$KC"
          security list-keychains -d user -s "$KC" $(security list-keychains -d user | tr -d '"')
          echo "$PROFILE" | base64 --decode > $RUNNER_TEMP/app.mobileprovision
          # With shareTarget, the extension's profile, installed where asuka looks:
          # mkdir -p ~/Library/MobileDevice/Provisioning\ Profiles
          # echo "$SHARE_PROFILE" | base64 --decode > ~/Library/MobileDevice/Provisioning\ Profiles/share.mobileprovision
      - name: build, and upload to TestFlight
        env:
          ASUKA_ASC_KEY_ID: ${{ secrets.ASC_KEY_ID }}
          ASUKA_ASC_ISSUER_ID: ${{ secrets.ASC_ISSUER_ID }}
          ASUKA_ASC_KEY: ${{ secrets.ASC_KEY }}
        run: asuka publish --platform ios --profile $RUNNER_TEMP/app.mobileprovision --no-wait
      - uses: actions/upload-artifact@v4
        with: { name: ios, path: dist/*.ipa }
      - name: attach to the tag's release
        if: startsWith(github.ref, 'refs/tags/')
        env:
          GH_TOKEN: ${{ github.token }}
          GH_REPO: ${{ github.repository }}
        run: gh release upload "$GITHUB_REF_NAME" dist/*.ipa --clobber

  android:
    needs: release
    if: ${{ !failure() && !cancelled() }}
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-java@v4
        with: { distribution: temurin, java-version: 17 }
      - uses: android-actions/setup-android@v3
        with: { packages: platform-tools }
      - name: an NDK
        run: sdkmanager "ndk;27.1.12297006"
      - name: the asuka CLI, at the app's version of Asuka
        run: |
          curl -fsSL https://asuka.egoist.dev/install.sh | sh
          echo "$HOME/.asuka/bin" >> "$GITHUB_PATH"
      - name: the upload key
        env:
          KEYSTORE: ${{ secrets.ANDROID_KEYSTORE }}
        run: echo "$KEYSTORE" | base64 --decode > $RUNNER_TEMP/upload.jks
      - name: build, and upload to Play's internal test track
        run: asuka publish --platform android --track internal
        env:
          ASUKA_KEYSTORE: ${{ runner.temp }}/upload.jks
          ASUKA_KEYSTORE_PASSWORD: ${{ secrets.ANDROID_KEYSTORE_PASSWORD }}
          ASUKA_PLAY_SERVICE_ACCOUNT: ${{ secrets.PLAY_SERVICE_ACCOUNT }}
      - uses: actions/upload-artifact@v4
        with:
          name: android
          path: dist/*.aab
      - name: attach to the tag's release
        if: startsWith(github.ref, 'refs/tags/')
        env:
          GH_TOKEN: ${{ github.token }}
          GH_REPO: ${{ github.repository }}
        run: gh release upload "$GITHUB_REF_NAME" dist/*.aab --clobber
```

Its secrets (Settings › Secrets and variables › Actions), the files in
base64 (`base64 -i file | pbcopy`):

- `IOS_CERTIFICATE`: your Apple Distribution certificate with its
  private key, a `.p12` exported from Keychain Access;
  `IOS_CERTIFICATE_PASSWORD` the password you exported it with.
- `IOS_PROFILE`: the app's App Store provisioning profile, from
  developer.apple.com (with `shareTarget`, `SHARE_PROFILE` the share
  extension's).
- `ASC_KEY_ID`, `ASC_ISSUER_ID` and `ASC_KEY` (the `.p8`'s text): an App
  Store Connect API key with the App Manager role, from Users and Access
  › Integrations.
- `ANDROID_KEYSTORE`: the upload key's `.jks`; `ANDROID_KEYSTORE_PASSWORD`
  its password (add `ASUKA_KEY_PASSWORD` if the key's differs).
- `PLAY_SERVICE_ACCOUNT`: a Google Cloud service account's JSON key,
  invited in the Play Console's Users and permissions with the right to
  release to testing tracks.

The CLI builds the app with its own version of Asuka, so it's installed
at the version the app is made with: `ASUKA_VERSION`, which the install
script reads and you raise as you move the app to a newer Asuka. Each build is numbered after the
store's highest, so nothing is bumped before you tag; `--no-wait` ends
the iOS job once Apple has the upload, not once it's processed. The App Store takes builds
made with a recent Xcode only; if the runner's default is older, pick
one with `sudo xcode-select -s /Applications/Xcode_<version>.app`.

### The desktop

These jobs, added to the same workflow, build the desktop apps for the
tag and attach them to its draft release: the Mac's for Apple silicon
and Intel, signed with your Developer ID and notarized, Windows' for x64
and Arm, signed, and
Linux's for amd64 and arm64. Windows' and Linux's build on Linux, so
one Ubuntu runner makes both. (The app is named Notes here: its files
are named after `asuka.toml`'s `name`.)

```yaml
  mac:
    needs: release
    runs-on: macos-latest
    steps:
      - uses: actions/checkout@v4
      - name: the asuka CLI, at the app's version of Asuka
        run: |
          curl -fsSL https://asuka.egoist.dev/install.sh | sh
          echo "$HOME/.asuka/bin" >> "$GITHUB_PATH"
      - name: the Developer ID certificate
        env:
          CERTIFICATE: ${{ secrets.MAC_CERTIFICATE }}
          CERTIFICATE_PASSWORD: ${{ secrets.MAC_CERTIFICATE_PASSWORD }}
        run: |
          KC=$RUNNER_TEMP/signing.keychain-db
          PW=$(openssl rand -hex 16)
          security create-keychain -p "$PW" "$KC"
          security set-keychain-settings -lut 3600 "$KC"
          security unlock-keychain -p "$PW" "$KC"
          echo "$CERTIFICATE" | base64 --decode > $RUNNER_TEMP/certificate.p12
          security import $RUNNER_TEMP/certificate.p12 -k "$KC" -P "$CERTIFICATE_PASSWORD" -T /usr/bin/codesign
          security set-key-partition-list -S apple-tool:,apple: -k "$PW" "$KC"
          security list-keychains -d user -s "$KC" $(security list-keychains -d user | tr -d '"')
      - name: build, sign and notarize
        env:
          ASUKA_APPLE_ID: ${{ secrets.APPLE_ID }}
          ASUKA_APPLE_PASSWORD: ${{ secrets.APPLE_APP_PASSWORD }}
          ASUKA_TEAM_ID: ${{ secrets.APPLE_TEAM_ID }}
        run: |
          asuka build --platform mac --sign "Developer ID Application: Your Name (TEAMID)" --notarize env
          asuka build --platform mac --arch amd64 --sign "Developer ID Application: Your Name (TEAMID)" --notarize env
      - name: attach to the tag's release
        env:
          GH_TOKEN: ${{ github.token }}
          GH_REPO: ${{ github.repository }}
        run: gh release upload "$GITHUB_REF_NAME" dist/*/*.zip dist/*/*.dmg --clobber

  windows-linux:
    needs: release
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: mlugg/setup-zig@v2 # cross-compiles the app
      - name: the asuka CLI, at the app's version of Asuka
        run: |
          curl -fsSL https://asuka.egoist.dev/install.sh | sh
          echo "$HOME/.asuka/bin" >> "$GITHUB_PATH"
      - name: osslsigncode, and the code signing certificate
        env:
          CERTIFICATE: ${{ secrets.WINDOWS_CERTIFICATE }}
        run: |
          sudo apt-get install -y osslsigncode
          echo "$CERTIFICATE" | base64 --decode > $RUNNER_TEMP/cert.pfx
      - name: Windows, x64 and Arm
        env:
          ASUKA_WINDOWS_CERT: ${{ runner.temp }}/cert.pfx
          ASUKA_WINDOWS_CERT_PASSWORD: ${{ secrets.WINDOWS_CERTIFICATE_PASSWORD }}
        run: |
          asuka build --platform windows
          asuka build --platform windows --arch arm64
      - name: Linux, amd64 and arm64
        run: |
          asuka build --platform linux
          asuka build --platform linux --arch arm64
      - name: attach to the tag's release
        env:
          GH_TOKEN: ${{ github.token }}
          GH_REPO: ${{ github.repository }}
        run: gh release upload "$GITHUB_REF_NAME" dist/*/*.zip dist/*/*.tar.gz dist/*/*.deb dist/*/*.rpm dist/*/*.AppImage --clobber
```

Their secrets: `MAC_CERTIFICATE`, your Developer ID Application
certificate with its private key (a `.p12`), and
`MAC_CERTIFICATE_PASSWORD`; `APPLE_ID`, `APPLE_APP_PASSWORD` (an
app-specific password, from account.apple.com) and `APPLE_TEAM_ID`, for
the notary service; `WINDOWS_CERTIFICATE`, a code signing certificate
(a `.pfx`), and `WINDOWS_CERTIFICATE_PASSWORD`. Each build makes its
files in a directory of its own (`dist/darwin-arm64`,
`dist/windows-amd64`, `dist/linux-arm64`), named for the app, its
version, platform and architecture, so the release takes them all, side
by side; each Linux build's Flatpak manifest packages its own.

With [updates](#updates) on GitHub, `asuka publish` in place of `asuka
build` also uploads each platform's update to the tag's draft release
(the `release` job made it), to publish once every platform is there.
Add the secret key as a secret (`gh secret set ASUKA_UPDATE_PRIVATE_KEY <
asuka-update.key`), give the jobs `ASUKA_UPDATE_PRIVATE_KEY` and
`GH_TOKEN`, and build the tag's version:

```yaml
      - name: build, sign, notarize, and upload with its update
        env:
          ASUKA_UPDATE_PRIVATE_KEY: ${{ secrets.ASUKA_UPDATE_PRIVATE_KEY }}
          GH_TOKEN: ${{ github.token }}
          ASUKA_APPLE_ID: ${{ secrets.APPLE_ID }}
          ASUKA_APPLE_PASSWORD: ${{ secrets.APPLE_APP_PASSWORD }}
          ASUKA_TEAM_ID: ${{ secrets.APPLE_TEAM_ID }}
        run: asuka publish --platform mac --draft --version "${GITHUB_REF_NAME#v}" --sign "Developer ID Application: Your Name (TEAMID)" --notarize env
```

and in the Windows and Linux job:

```yaml
        run: |
          for p in "windows" "windows --arch arm64" "linux" "linux --arch arm64"; do
            asuka publish --platform $p --draft --version "${GITHUB_REF_NAME#v}"
          done
```

Then publish the draft (`gh release edit v1.2.0 --draft=false`, or in
the release's page): installed apps update to it. The release job's tag
is `tagPrefix` and the version (`v1.2.0`); `--version` makes the build's
the tag's, so `asuka.toml`'s needn't be bumped. Deltas start from the
published release, which the runners download (`GH_TOKEN` lifts the
GitHub API's limit on requests, which a `tagPrefix` uses).

A Linux job that signs the `.rpm` and writes the [apt and dnf
repository](#signed-packages-and-repositories) takes the OpenPGP key
too (`gh secret set ASUKA_LINUX_GPG_KEY < asuka-linux.key`), and the
bucket's credentials:

```yaml
        env:
          ASUKA_LINUX_GPG_KEY: ${{ secrets.ASUKA_LINUX_GPG_KEY }}
          AWS_ACCESS_KEY_ID: ${{ secrets.R2_ACCESS_KEY_ID }}
          AWS_SECRET_ACCESS_KEY: ${{ secrets.R2_SECRET_ACCESS_KEY }}
        run: |
          for a in amd64 arm64; do
            asuka publish --platform linux --arch $a --draft --version "${GITHUB_REF_NAME#v}"
          done
```

## Languages

An app with [catalogs](i18n.md) needs nothing more: `asuka build` declares its
languages to each store and system (`CFBundleLocalizations` and `.lproj`s on
iOS and macOS, a `locales_config` on Android), which makes Settings offer the
app's language. Windows, Linux and web builds declare none: the app
shows the language the user's settings pick from its catalogs. The app's name and its purpose strings can be translated too,
with keys `asuka.appName` and `asuka.NSCameraUsageDescription` in the catalogs
([Packaging](i18n.md#what-the-system-shows)).
