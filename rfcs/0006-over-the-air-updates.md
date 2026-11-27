# 0006: Over-the-air updates on phones

- **Status**: Built (2026-11-26; rejected on 2026-11-15 for the Go
  runtime it was written for, reopened for the C core and its VM)
- **Created**: 2026-11-15
- **Related**: 0003 (in-process code patching), 0007 (the C core and the
  Asuka language), 0008 (no Go at run time), `platform/update` (desktop
  and web updates), bench/results/vm.md (the VM against compiled C)

## Summary

An update of an app's Asuka code shipped to the iPhones and Android
phones that have it, without a store release, as Expo's updates do for
React Native: the app's bytecode, signed with the key desktop updates
use, published to the same feeds (GitHub's releases, an S3 bucket, any
HTTPS directory, a server of the app's own), and run by the core's VM
in place of the code compiled into the build. An update fits only the
builds whose native side it was made against (their fingerprint); a
launch that crashes before the app has run a while goes back to the
build's own code.

## Why it's possible now

The first version of this RFC rejected code push for Go: Go on iOS
needed an interpreter that didn't exist, or Go compiled to WebAssembly
under one, measured at 50 to 1,400 times slower than native. Neither
holds for Asuka today (RFC 0007, 0008):

- **The interpreter is Asuka's own.** The core's VM runs every app in
  development (the player on iOS and Android, `asuka dev`), on the same
  core and hosts a release runs on, calling them directly. A release
  already links it: `asuka_load` is the ABI's.
- **It's fast enough.** On pure computation the VM costs 1.5 to 2.6
  times compiled C in cycles (bench/results/vm.md: fib 0.47 against
  0.18, nbody 1.28 against 0.92, trees 3.75 against 1.56), and about the
  same where the work is the libraries' (JSON, strings, maps, sorting:
  C either way); it's faster than CPython and close to LuaJIT's
  interpreter. In an app, layout, signals, the views and drawing are the
  core's and the host's, compiled, whatever runs the app's code: an
  updated app runs its own code somewhat slower until the next store
  release compiles it.
- **One semantics.** The VM is what developers run all day; the compiled
  C is checked against it (lang/compile's programs run both ways). An
  update isn't a path tests never take.

## What the stores allow

Apple's Developer Program License Agreement 3.3.1(B) lets an app
download interpreted code that doesn't change its primary purpose,
bypass the OS's security, or make a storefront; Guidelines 2.5.2 and
2.5.19 rule out downloaded code that adds features review didn't see.
Google Play allows code that runs in an interpreter. So an update is
for fixes and small changes, as Expo's are: what's allowed is the app
developer's to keep to, and the docs say so.

## Design

### An update

What `asuka publish --update` (a phone's platform) makes:

- **The module**: the app's bytecode, as `asuka dev` sends it, with the
  std packages it imports compiled in.
- **Its fingerprint**: a SHA-256 over what the build's native side
  gives the code: the core's and the host's version (the CLI's), the
  platform, the builtin tables linked (the module's builtins' names),
  the app's own C functions (its `extern`s), the native packages and
  their glue (their interfaces), and the app's assets. A build carries
  its own (compiled into it, as `asuka_runtime_fingerprint`); an update
  carries the one it was made against. They must be equal: an update
  that needs a new native package, a builtin the build didn't link, or
  a new asset is for the next store build, and `asuka publish` says so.
  (Since 2026-11-27: the host's parts the build has, its flags, are in
  it too, and Apple TV, a build of the iOS app, is a platform of its
  own, `tvos`: its host has fewer parts and only the packages that list
  tvOS, so an iPhone and an Apple TV never take each other's updates.)
- **Its manifest**, beside it in the feed:
  `update-<platform>-<fingerprint>.json` (with `-<channel>` for a
  prerelease channel), the desktop manifest's fields: the update's
  version (the app's version, then `+` and a number that grows:
  `1.4.0+3`), notes, date, the module's URL, size and the Ed25519
  signature of its SHA-256.

### On the phone

- **At launch** (the core, `asuka_run`, every host alike): if the app's
  directory has an update in place (`Updates/current.json`: its
  version, fingerprint and module) whose fingerprint is the build's,
  the core loads it (`asuka_load`'s path) instead of the build's code;
  otherwise it runs the build's.
- **Rollback**: before loading an update the core marks it as launching
  (`Updates/launching`, with how many launches before ended early); once
  the app has mounted and run for 10 seconds, the mark goes. A launch
  that finds the mark left behind follows one that ended before that (a
  crash, a hang the system killed, or the user closing the app at once):
  after two in a row, as a crash at launch repeats and a user's closing
  doesn't, the core sets the update aside (its files renamed `.bad`,
  never taken again) and runs the build's code. An update whose module doesn't load
  (corrupt, a VM error at init) is moved aside at once.
- **Checking and downloading** are `platform/update`'s, in Asuka, as on
  the desktops: `update.check()` reads the platform's manifest for the
  build's fingerprint and channel; a version newer than what runs is a
  `Release` whose `install()` downloads the module, checks its signature
  with the build's public key (the core's `update.verifyFile`), and puts
  it in place for the next launch. `relaunch()` runs it now: the core
  unmounts the app and loads the update, as `asuka dev` reloads one.
- **The ready-made way**: `update.UpdateWindow()` on a phone checks in
  the background soon after launch and then once a day, downloads what
  it finds, and the next launch runs it (Expo's default), showing
  nothing; `update.updater` says where it is.

### What doesn't change

Desktops keep updating the whole app, and the web reloads. A build
without asuka.toml's `[updates]` has no public key and takes no update.
`asuka dev`'s player is unaffected: it loads what the CLI sends.

## Open

- **Assets in updates.** v1 refuses an update whose assets differ from
  the build's (they're in its fingerprint). Shipping them means the
  hosts' asset lookups reading an update's directory first.
- **Partial updates.** A module is a few hundred KB to a few MB;
  bsdiff deltas, as desktop updates have, would make them smaller.
- **Rollouts** (a percentage of devices) and **rollbacks from the
  server** (publishing an earlier version as the newest) are the
  feed's: the second works today, the first doesn't.

## References

- [App Review Guidelines](https://developer.apple.com/app-store/review/guidelines/)
  (2.5.2, 2.5.19, 4.7), and [the 2.5.19 announcement](https://developer.apple.com/news/?id=vvzx6cul).
- [Apple Developer Program License Agreement](https://developer.apple.com/support/terms/apple-developer-program-license-agreement/), 3.3.1(B).
- [Google Play: Device and Network Abuse](https://support.google.com/googleplay/android-developer/answer/9888379).
- [Expo updates: how it works](https://docs.expo.dev/eas-update/how-it-works/),
  [error recovery](https://docs.expo.dev/eas-update/error-recovery/),
  [runtime versions and fingerprints](https://docs.expo.dev/eas-update/runtime-versions/),
  [the protocol](https://docs.expo.dev/technical-specs/expo-updates-1/).
- [Shorebird: how we built code push](https://shorebird.dev/blog/how-we-built-code-push/).
- The first version of this RFC (git history): the Go runtime's options,
  WebAssembly under wazero, WAMR and wasm3, and Shorebird's way, measured
  and rejected.
