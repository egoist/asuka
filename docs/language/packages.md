# Files, packages and imports

A source file ends in `.asuka`. A directory of them is a **package**:
its files see each other's declarations without importing anything.
`pub` makes a declaration visible to other packages.

```asuka
import std/http                 // the standard library
import platform/camera          // the platform SDK
import @acme/charts               // a dependency (asuka add @acme/charts)
import ./components               // a package beside this one
import ./model.{Message, Folder}  // names used without the package's name
import ./theme as t               // a shorter name
import ../model                   // a package a directory up
```

A package's names are used through it: `http.get(url)`,
`components.Card("Hi")`. A package is named by its path's last segment;
one whose name isn't a name (`@acme/line-charts`) is imported with
`as`. The prelude (the built-in types, elements and functions) needs no
import. [The standard library and the platform SDK](../stdlib.md) lists
the `std/...` and `platform/...` packages, which come with the CLI; no
other package can be named under `std/` or `platform/`.

Statements and declarations end at the end of a line; `;` puts several
on one. A line that ends with an operator, a comma or an opening bracket
goes on to the next.

Comments are `// …` and `/* … */`. A `///` comment documents the
declaration under it, and editors show it.

## Files for one platform

A file whose name ends in a platform's suffix is part of its package
only when the app is built for that platform: `_ios`, `_android`,
`_macos`, `_windows`, `_linux` and `_web`, and `_desktop` (macOS,
Windows and Linux) and `_mobile` (iOS and Android) for several. The
platforms' files may each declare the same names, which the package's
other files use as they would any:

```asuka
// feedback_mobile.asuka: iOS and Android
pub fn confirm() { haptic(.success) }

// feedback_desktop.asuka: macOS, Windows and Linux
pub fn confirm() { announce("Done") }

// feedback_web.asuka
pub fn confirm() {}
```

A name is declared once on each platform: in a file every platform has,
or in a file for each platform that has it. One declared in a file
every platform has and in a platform's, or in two files a platform has
both of (`feedback_desktop.asuka` and `feedback_linux.asuka`), is an error,
whichever platform the app is built for. A file whose name is only the
suffix (`linux.asuka`) is a file like any other.

Every package goes by its files' names alike: the app's, its
dependencies', the standard library's. What's built is a platform's:
`asuka dev` and `asuka build` build for the platform they run the app
on, `asuka test` for iOS unless `--platform` says another, and a program
for the computer it runs on (or `--target`'s). `asuka check` checks
each platform's files, and says which platforms an error is on when
it's not every one's: a name only some platforms' files declare is an
error where the others use it. An editor reads a platform's file with
that platform's files, and the others with iOS's, and shows every
platform's errors. For code that differs by platform within a file,
there's [`@platform`](control-flow.md#code-for-one-platform).

## Apps, programs and libraries

A package is an entry, or a library. One with an `app { }` block is an
app: the block is what it shows (see [Navigation](navigation.md#navigation)). One
whose files declare `fn main()` is a **program**, a command-line tool:
`main` takes no parameters (`os.args()` are its arguments), returns
nothing (`os.exit(code)` ends it with a code), and may be `async` and
throw; it ends as `main` does. A package with neither is a library,
which others import. A package can't be an app and a program both, and
a program declares no components: it shows no views. Only the package
an app or a program is made of has an entry; a `main` in a package it
imports is a function like any other. See [Programs](../programs.md).

```asuka
import std/os

fn main() {
  print("Hello, {os.args().first() ?? "world"}!")
}
```
