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
goes on to the next. So does a line followed by one that starts with
`.name` or `?.name`, or with a binary operator that can't start an
expression (any but `-`, which can: `-x`), so a chain may take a line
a step:

```asuka
let names = users
  .filter(u => u.active)
  .map(u => u.name)
let label = user?.nickname
  ?? user?.name
  ?? "Guest"
```

A line that starts with `.name` is something of its own in two places.
In a `match`, it's the next arm (`.ready(v) => …`). And alone on the
last line of a block whose last expression is its value (an `if` or a
`match` used as a value, a closure's block), it's that value:

```asuka
let tint: Color = if urgent {
  log("urgent")
  .red  // the if's value
} else { .gray }
```

There, two `.name` lines or more are still a chain.

Comments are `// …` and `/* … */`. A `///` comment documents the
declaration under it, and editors show it.

A name starts with a letter or `_` and goes on with letters, digits and
`_`. A letter is any script's, with its combining marks: `total`,
`नमस्ते`, `ราคา`, `名前` (Unicode's identifiers, UAX #31's XID_Start and
XID_Continue, as in Swift, Rust and Python). Names that look the same
are the same name: they're compared in Unicode's NFC form, so an `é`
typed as one character and one typed as an `e` and an accent are one.

## Files for one platform

A file whose name ends in a platform's suffix is part of its package
only when the app is built for that platform: `_ios`, `_android`,
`_macos`, `_windows`, `_linux` and `_web`, and `_desktop` (macOS,
Windows and Linux) and `_mobile` (iOS and Android) for several. The
platforms' files may each declare the same names, which the package's
other files use as they would any:

```asuka
// feedback_mobile.asuka: iOS and Android
import platform/device

pub fn confirm() { device.haptic(.success) }

// feedback_desktop.asuka: macOS, Windows and Linux
import platform/accessibility

pub fn confirm() { accessibility.announce("Done") }

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

```asuka check
import std/os

fn main() {
  print("Hello, {os.args().first() ?? "world"}!")
}
```
