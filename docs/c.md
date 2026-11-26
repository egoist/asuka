# C from Asuka

An app calls C directly: its own C, a library's, or the system's.
`extern` declarations say, in Asuka, what a C function takes and gives
back, and the compiler generates the glue that converts the values and
calls it. There's no libffi and no serialization: a call converts
only what its types need, so an `Int32` costs nothing and a string is
passed as the pointer Asuka already holds.

```asuka
extern fn zlibVersion() -> CString
extern fn crc32_of(data: bytes) -> UInt32

fn checksum(text: string) -> u32 = crc32_of(text.bytes())
```

The C is compiled into the app on every platform (on the web, into its
WebAssembly module), and into `asuka test`'s runner. What it
compiles and links is `asuka.toml`'s `[c]`.

Use C for a library that has a C API (SQLite, a codec, a vendor SDK),
for code that's faster in C, or for code shared with other programs.
For what only a platform's own language reaches (UIKit, Android's
APIs, the DOM), write a [native package](native.md) instead.

## A first call

An app's C sits in its directory, under `c/` by convention, and
`asuka.toml` names it:

```toml
# asuka.toml
[c]
sources = ["c/*.c"]
include = ["c/include"]

[c.ios]
libraries = ["z"]
[c.android]
libraries = ["z"]
```

```c
// c/stats.c
#include <stddef.h>
#include <stdint.h>

int64_t stats_sum(const uint8_t *data, size_t len) {
    int64_t s = 0;
    for (size_t i = 0; i < len; i++) s += data[i];
    return s;
}

int32_t stats_clamp(int32_t x, int32_t lo, int32_t hi) {
    return x < lo ? lo : x > hi ? hi : x;
}
```

```asuka
extern fn stats_sum(data: bytes) -> Int64
extern fn stats_clamp(x: Int32, lo: Int32, hi: Int32) -> Int32

component Total {
  state text = "hello"

  Column {
    TextField { bind text: text }
    Text("Sum: {stats_sum(text.bytes())}")
    Text("Clamped: {stats_clamp(i32(clamping: text.count()), 0, 3)}")
  }
}

app { Total() }

test "C adds the bytes" {
  expect(stats_sum("ab".bytes()) == 195)
  expect(stats_clamp(10, 0, 3) == 3)
}
```

`asuka dev` builds the player with the C in it, and `asuka test` runs
the test with it. An extern fn is called like any function: Asuka sees
`stats_sum` as `fn(bytes) -> int`.

## Declaring C

The declarations go at the top of a file, beside the rest of the
package, and may be `pub`. Their names are C's.

### Functions

`extern fn name(params) -> Result` declares the C function `name`. It
has no body, isn't generic, isn't `async` and doesn't `throws`: C has no
errors to raise, so a function that fails returns a status the Asuka
side reads (and throws as an error of its own, if it likes). A
parameter may have a default, filled in where it's called.

```asuka
extern fn compress_level() -> Int32
extern fn deflate_into(data: bytes, level: Int32 = 6) -> Int64

enum CodecError: Error { failed(int) }

fn deflate(data: bytes) throws -> int {
  let n = deflate_into(data)
  if n < 0 {
    throw CodecError.failed(n)
  }
  return n
}
```

### Types C owns

`extern type Name with release fn` is a pointer C gives and Asuka holds
(`sqlite3 *`, a handle). Asuka never looks inside it. When the last
reference to it goes, Asuka calls `fn`, an extern fn of the same package
that takes one `Name`, to free it. Calling that function yourself frees
it at once, and only once: a call that uses it afterwards fails with
`the Db was released`. Without `with release`, Asuka frees nothing.

```asuka
extern type Db with release db_close

extern fn db_open(path: CString) -> Db?
extern fn db_close(db: Db)
extern fn db_changes(db: Db) -> Int32

fn changes(path: string) -> int {
  if let db = db_open(path) {
    return int(db_changes(db))
  } // db_close(db), as its last reference goes
  return -1
}
```

A function that returns `Db?` gives `none` for C's `NULL`. One that
returns `Db` fails if C returns `NULL` (`db_open returned NULL`), so
declare `?` where C may. Two values of an extern type are equal when
they hold the same pointer.

### Structs

`extern struct Name { fields }` is a C struct, passed and returned by
value. Its fields are C's value types: numbers, `bool`, `Pointer` and
other extern structs. Asuka sees it as a struct of their Asuka types,
made and read like any other, with defaults and methods if you like.
Its fields are in C's order, so they must be declared in the order the
C struct has them.

```asuka
extern struct Point {
  x: Float32
  y: Float32
}

extern struct Rect {
  origin: Point
  size: Point = Point(x: 0, y: 0)
}

extern fn rect_union(a: Rect, b: Rect) -> Rect

fn area(r: Rect) -> float = r.size.x * r.size.y
```

### Callbacks

A parameter of a function type is a callback: C gets a function pointer
and its context, two parameters, and calls the function with the
context first.

```asuka
extern fn each_row(sql: CString, row: fn(CString?, Int64) -> bool) -> Int32

fn names(sql: string) -> [string] {
  var out: [string] = []
  each_row(sql, (name, id) => {
    out.push(name ?? "#{id}")
    return out.count() < 100 // false stops it
  })
  return out
}
```

```c
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

int32_t each_row(const char *sql, bool (*row)(void *ctx, const char *name, int64_t id), void *ctx) {
    int32_t n = 0;
    for (int64_t id = 1; id <= 3; id++, n++) {
        if (!row(ctx, id == 2 ? NULL : "row", id)) break;
    }
    return n;
}
```

A callback takes numbers, `bool`, `CString`, `CString?`, `Pointer` and
`Pointer?`, and returns one of those numbers, a `bool`, a `Pointer` or
nothing. It isn't `async` and doesn't throw. A `(fn(…))?` parameter
passes `NULL` for none.

When C calls it during the extern call, on the thread that made the
call, it runs at once and C gets its result. If it fails (an index out
of range), C's further calls of it do nothing and return zero, and the
extern call throws the error once C returns. The callback's function
lives for that call. For C to keep it, see [Threads](#threads).

## Types

| In the declaration | C | Asuka sees |
|---|---|---|
| `Int8`, `Int16`, `Int32`, `Int64` | `int8_t` … `int64_t` | `i8`, `i16`, `i32`, `int` |
| `UInt8`, `UInt16`, `UInt32`, `UInt64` | `uint8_t` … `uint64_t` | `u8`, `u16`, `u32`, `u64` |
| `i8` … `u64`, `int` | the same as their C names' | themselves |
| `Float32` | `float` | `float` |
| `float` | `double` | `float` |
| `bool` | `bool` | `bool` |
| `CString`, `CString?` | `const char *` | `string`, `string?` |
| `bytes`, `bytes?` (parameters only) | `const uint8_t *, size_t`: two parameters | `bytes` |
| `Pointer`, `Pointer?` | `void *` | `Pointer`: opaque, compared with `==` |
| `extern type T`, `T?` | a pointer | `T` |
| `extern struct S` | the struct, by value | `S`, a struct |
| `fn(…) -> R`, `(fn(…) -> R)?` (parameters only) | `R (*)(void *ctx, …), void *ctx` | a function |

C's integers are Asuka's of the same width and sign, so a value
crosses whole both ways: a `UInt64` comes back as a `u64`, all of it,
and an `Int32` takes an `i32`. An `int` goes to an `Int32` parameter
converted where it's passed, `i32(n)` (an error when it doesn't fit),
`i32(truncating: n)` or `i32(clamping: n)`, so a value that doesn't fit
is never cut short unseen. `Int32` and its kind are written only in
extern declarations; elsewhere they're `i32` and the others (`let n:
Int32` is an error that says so).
`Pointer` is an Asuka type of its own: a value C gave, which Asuka
holds, compares and passes back, and never reads.

## Strings, bytes and memory

- **A `CString` passed to C** is the string's own bytes, which end with
  a NUL: nothing is copied. It's valid during the call only; C that
  keeps it copies it. A string with a NUL inside it reads, in C, up to
  that NUL.
- **A `CString` returned** is copied into an Asuka string. Asuka doesn't
  free C's: the string a function returns is C's to free, as most C APIs
  return strings they own (`sqlite3_errmsg`, `zlibVersion`). For one
  that returns memory the caller frees, write a few lines of C that copy
  it into a buffer you pass, or return it as a `Pointer` and free it with
  another extern fn.
- **`bytes`** is a pointer to the bytes and their length. It's valid
  during the call only, and C doesn't change it (`const`). `bytes?`'s
  none is `NULL` and `0`. A function can't return `bytes`, since their
  length can't come back with them: return an extern type or a `Pointer`,
  and read it with more functions.
- **What C allocates** is C's. An extern type's release frees it when
  Asuka is done with it; anything else, an extern fn you call.

## Threads

An extern call runs on the thread that makes it: the main thread in a
component, a handler or a task, so a call that takes long holds the
interface, as any synchronous work does. `spawn` runs one on another
thread, with values only (an extern type's value can't go: it's the
main thread's).

```asuka
extern fn hash_file(path: CString) -> UInt64

async fn hashed(path: string) -> u64 {
  return await spawn { hash_file(path) }
}
```

A callback that C keeps, to call later or from a thread of its own,
needs two functions of `asuka_ffi.h`, which every `[c]` source can
include:

```c
#include <stddef.h>
#include <stdint.h>

#include "asuka_ffi.h"

static void (*listener)(void *ctx, int64_t value);
static void *listener_ctx;

void on_change(void (*f)(void *ctx, int64_t value), void *ctx) {
    if (listener_ctx) asuka_callback_release(listener_ctx);
    asuka_callback_retain(ctx); // before the call returns
    listener = f;
    listener_ctx = ctx;
}

void stop_listening(void) {
    if (listener_ctx) asuka_callback_release(listener_ctx);
    listener = NULL;
    listener_ctx = NULL;
}
```

`asuka_callback_retain` keeps the function until
`asuka_callback_release`, which may come from any thread. A kept
callback C calls outside an extern call, or from another thread, is
posted to the main thread and returns zero at once: the function runs
there soon after, under the component that passed it (its errors go to
that component's boundary), and not at all once that component is gone.
A callback made inside `spawn` is valid during its call only.

## Building

`asuka.toml`'s `[c]` says what C the app compiles and what it links;
each platform's table adds to it.

| Key | What it is |
|---|---|
| `sources` | C files, globs relative to `asuka.toml` (`c/*.c`, `c/**/*.c`). |
| `include` | Directories the sources include from (`-I`). |
| `defines` | Macros: `NAME` or `NAME=value` (`-D`). |
| `flags` | More of the compiler's flags (`-Wno-unused-function`). |
| `libraries` | Libraries linked where the platform has them: `sqlite3` is `-lsqlite3`; a path (`lib/libfoo.a`) is that file. |
| `frameworks` | Apple's frameworks, on iOS and macOS (`Security`). |

```toml
[c]
sources = ["c/*.c", "vendor/sqlite3.c"]
include = ["vendor"]
defines = ["SQLITE_THREADSAFE=0", "SQLITE_OMIT_LOAD_EXTENSION"]

[c.macos]
frameworks = ["Security"]

[c.windows]
libraries = ["bcrypt"]

[c.web]
defines = ["SQLITE_OS_OTHER=1"]
```

The tables are `[c.ios]`, `[c.android]`, `[c.macos]`, `[c.windows]`,
`[c.linux]` and `[c.web]`. The sources are compiled for each target
with the platform's compiler (Xcode's, the NDK's, Zig's for the web and
cross builds), optimized for a release, with your warnings as they are.

- **iOS**: the C, and the libraries and frameworks, are linked into the
  app's executable. The SDK has
  `libz`, `libsqlite3`, `libxml2`, `libc++` and Apple's frameworks.
- **Android**: into the app's library, for each ABI. The NDK has `libz`,
  `libm`, `liblog`, `libandroid` and the other stable NDK libraries;
  anything else comes as sources or as a prebuilt `.a` per ABI
  ([Prebuilt libraries](#prebuilt-libraries)).
- **macOS, Windows and Linux**: into the app's executable.
- **The web**: into the WebAssembly module, built for WASI. There are no
  system libraries there and no threads, so a library comes as sources,
  and `[c.web]` says what changes (`defines`, other `sources`), or
  prebuilt for WASI.

A package's C is its own `asuka.toml`'s `[c]`, paths relative to the
package: an app that imports it builds it too. Its own packages
(`./util`) share the app's `[c]`.

## Downloaded sources

A library you don't want in your repository, such as SQLite's
amalgamation, can be downloaded by the build instead.
`[c.download.<name>]` names an archive and pins its SHA-256, and a path
in `[c]` that starts with `$<name>` is in it:

```toml
[c]
sources = ["c/*.c", "$sqlite/sqlite3.c"]
include = ["$sqlite"]

[c.download.sqlite]
url = "https://www.sqlite.org/2026/sqlite-amalgamation-3540100.zip"
sha256 = "0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9"
strip = 1
```

| Key | What it is |
|---|---|
| `url` | The archive: `https://`, a `.zip`, `.tar.gz` or `.tgz`. |
| `sha256` | Its SHA-256 (`shasum -a 256 file.zip` prints it). An archive with another is refused. |
| `strip` | How many leading directories its paths lose: `1` for an archive whose files are all in one top directory, as most are. |

The first build that needs it downloads it once, checks it and unpacks
it into Asuka's cache, where every project shares it. Later builds, and
other projects with the same archive, use it without the network.
`$sqlite` alone is the archive's top directory, and `$sqlite/...` a
file or a glob in it. Downloads work in `sources`, `include`, the paths
in `libraries` and `[c.prebuilt]`, so a vendor's built libraries can
come this way too.

## Prebuilt libraries

A vendor that ships its library built rather than as sources is named
in `[c.prebuilt]`: a static library for each target, and its headers in
`include`. A build links the one for its target, and the externs call it
as they call your own C.

```toml
[c]
include = ["vendor/acme/include"]

[c.prebuilt]
ios-arm64 = "vendor/acme/ios-arm64/libacme.a"
ios-simulator = "vendor/acme/ios-simulator/libacme.a"  # arm64 and x86_64 in one
macos = "vendor/acme/macos/libacme.a"                  # arm64 and x86_64 in one
android-arm64 = "vendor/acme/android-arm64/libacme.a"
android-arm = "vendor/acme/android-arm/libacme.a"
android-x64 = "vendor/acme/android-x64/libacme.a"
linux-x64 = "vendor/acme/linux-x64/libacme.a"
linux-arm64 = "vendor/acme/linux-arm64/libacme.a"
windows-x64 = "vendor/acme/windows-x64/acme.lib"
windows-arm64 = "vendor/acme/windows-arm64/acme.lib"
web = "vendor/acme/wasm32/libacme.a"

[c.android]
libraries = ["log"]
```

```asuka
extern fn acme_version() -> CString
```

- **The keys** are the targets: `ios-arm64`, `ios-simulator-arm64`,
  `ios-simulator-x64`, `macos-arm64`, `macos-x64`, `android-arm64`,
  `android-arm`, `android-x64`, `linux-x64`, `linux-arm64`,
  `windows-x64`, `windows-arm64` and `web`. `ios-simulator` and `macos`
  name a universal library (one `lipo` made) for both of the platform's
  architectures; a target's own key comes first.
- **A value** is a library's path relative to `asuka.toml`, or a list of
  them. Each is a static library: `.a`, or on Windows `.a` or `.lib`.
- **A target it has no library for** doesn't build, and says so
  (`[c.prebuilt] has no library for android-x64`). `asuka test` and
  `asuka run` link this computer's (`macos-arm64`, `linux-x64`...), so
  name it too to test the calls.
- **What the library needs of the system** goes in the platform's
  tables, as for sources: `libraries` (`log` on Android, `z`) and
  `frameworks` (`Security`).
- **Build it** for the platforms' oldest versions Asuka runs on (iOS 16,
  macOS 11, Android 7), and for the web against WASI's C library
  (`wasm32-wasi`), with no threads.

The libraries are linked into the app, as your own C is: there's nothing
to find, embed or sign beside it as it runs, and what it doesn't call is
left out. A package's `[c.prebuilt]` is published with it, the
libraries in its tarball.

A vendor's dynamic library isn't linked from `[c]`. One for iOS or
macOS (an XCFramework of a framework) and one for Android (an `.so` per
ABI, or an AAR) ship in a [native package](native.md): a binary target
of its Swift package, the `jniLibs` or dependencies of its Gradle
module. Each platform's own build embeds, signs and loads them, and the
package's Swift and Kotlin call them.

## Developing

- `asuka dev` builds the player with the app's C. A change to a C file
  or a header, to `asuka.toml`, or to an extern declaration builds the
  player again and relaunches it; a change to the rest of the Asuka only
  reloads it, keeping its state.
- `asuka test` and `asuka run` build their runner with the C, for this
  computer (the `[c.macos]`, `[c.linux]` or `[c.windows]`
  table), so a test calls the C the app does. `asuka run file.asuka`
  reads the `asuka.toml` beside the file.
- The compiler checks every declaration: a type that can't cross says
  why and what to write instead (`parameter s is a string: it crosses
  to C as a CString`, `field name of extern struct User can't be a
  CString: a struct crosses by value, and the string's memory wouldn't
  outlive the call`).
- C's own mistakes (a wrong signature, a freed pointer) are C's: the
  compiler can't see C's side, so declare the types C's header declares,
  and test the calls with `asuka test`.

## How it works

For each package with extern declarations, the compiler generates the
glue that converts a call's arguments, calls the C function, and
converts its result, and that lets C call Asuka's callbacks. A call
works the same under `asuka dev` and in a release. The glue declares
each C function from its extern declaration, so it needs none of the
library's headers.
