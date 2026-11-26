# 在 Asuka 中调用 C

应用可以直接调用 C：无论是应用自己的 C 代码、某个库的，还是系统的。你用 `extern` 声明在 Asuka 里写明一个 C 函数接收什么、返回什么，编译器会生成负责转换值并调用它的胶水代码。这里没有 libffi，也没有序列化：一次调用只做参数类型真正需要的转换，所以传一个 `Int32` 没有任何开销，传字符串时直接传 Asuka 手里已有的那个指针。

```asuka
extern fn zlibVersion() -> CString
extern fn crc32_of(data: bytes) -> UInt32

fn checksum(text: string) -> u32 = crc32_of(text.bytes())
```

这些 C 代码会在每个平台上编进应用（在 Web 上编进它的 WebAssembly 模块），也会编进 `asuka test` 的运行器。编译和链接哪些内容，由 `asuka.toml` 的 `[c]` 决定。

如果一个库提供的是 C API（SQLite、某个编解码器、厂商的 SDK），或者某段代码用 C 写更快，又或者要和其他程序共用代码，就用 C。如果要用的东西只有平台自己的语言才能访问（UIKit、Android 的 API、DOM），请改写一个[原生包](native.md)。

## 第一次调用

应用的 C 代码放在应用目录里，按惯例放在 `c/` 下，并在 `asuka.toml` 里列出：

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

`asuka dev` 会构建一个包含这些 C 代码的开发 App，`asuka test` 运行测试时也会带上它们。extern fn 的调用方式和普通函数一样：在 Asuka 看来，`stats_sum` 就是 `fn(bytes) -> int`。

## 声明 C 代码

这些声明写在文件顶层，和包里的其他代码放在一起，也可以加 `pub`。它们的名字就是 C 里的名字。

### 函数

`extern fn name(params) -> Result` 声明 C 函数 `name`。它没有函数体，不能是泛型，不能是 `async`，也不能 `throws`：C 没有可以抛出的错误，所以会失败的函数应当返回一个状态值，由 Asuka 这边读取（愿意的话，再把它当作自己的错误抛出）。参数可以有默认值，在调用处填入。

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

### C 持有的类型

`extern type Name with release fn` 表示一个由 C 给出、由 Asuka 持有的指针（`sqlite3 *`、某个句柄）。Asuka 从不查看它的内部。当指向它的最后一个引用消失时，Asuka 会调用 `fn` 来释放它；`fn` 必须是同一个包里的 extern fn，并且只接收一个 `Name`。你也可以自己调用这个函数，它会立即释放，而且只释放一次：之后再用到这个值的调用会失败，报错 `the Db was released`。不写 `with release` 的话，Asuka 什么都不会释放。

```asuka
extern type Db with release db_close

extern fn db_open(path: CString) -> Db?
extern fn db_close(db: Db)
extern fn db_changes(db: Db) -> Int32

fn changes(path: string) -> int {
  if let db = db_open(path) {
    return int(db_changes(db))
  } // 最后一个引用消失时调用 db_close(db)
  return -1
}
```

返回 `Db?` 的函数在 C 返回 `NULL` 时得到 `none`。返回 `Db` 的函数在 C 返回 `NULL` 时会失败（`db_open returned NULL`），所以只要 C 可能返回 `NULL`，就声明成 `?`。两个 extern type 的值持有同一个指针时，它们相等。

### 结构体

`extern struct Name { fields }` 是一个 C 结构体，按值传入和返回。它的字段只能是 C 的值类型：数字、`bool`、`Pointer` 和其他 extern struct。在 Asuka 看来，它就是一个由这些字段对应的 Asuka 类型组成的结构体，创建和读取方式与其他结构体相同，也可以加上默认值和方法。字段按 C 的顺序排列，所以声明顺序必须和 C 结构体中的顺序一致。

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

### 回调

函数类型的参数就是回调：C 会拿到一个函数指针和它的上下文，也就是两个参数，调用这个函数时把上下文作为第一个参数传入。

```asuka
extern fn each_row(sql: CString, row: fn(CString?, Int64) -> bool) -> Int32

fn names(sql: string) -> [string] {
  var out: [string] = []
  each_row(sql, (name, id) => {
    out.push(name ?? "#{id}")
    return out.count() < 100 // 返回 false 就停止
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

回调的参数可以是数字、`bool`、`CString`、`CString?`、`Pointer` 和 `Pointer?`，返回值可以是这些数字类型之一、`bool`、`Pointer`，或者不返回。回调不能是 `async`，也不能抛出错误。`(fn(…))?` 类型的参数在值为 none 时传 `NULL`。

如果 C 在 extern 调用的过程中、在发起调用的那个线程上调用回调，回调会立即执行，C 拿到它的返回值。如果回调失败了（比如下标越界），C 之后对它的调用什么也不做，直接返回零；等 C 返回后，这次 extern 调用会抛出那个错误。回调函数只在这次调用期间有效。如果 C 需要把它留下来，见[线程](#线程)。

## 类型

| 声明中写的 | C | Asuka 看到的 |
|---|---|---|
| `Int8`、`Int16`、`Int32`、`Int64` | `int8_t` … `int64_t` | `i8`、`i16`、`i32`、`int` |
| `UInt8`、`UInt16`、`UInt32`、`UInt64` | `uint8_t` … `uint64_t` | `u8`、`u16`、`u32`、`u64` |
| `i8` … `u64`、`int` | 与对应的 C 名称相同 | 它们自己 |
| `Float32` | `float` | `float` |
| `float` | `double` | `float` |
| `bool` | `bool` | `bool` |
| `CString`、`CString?` | `const char *` | `string`、`string?` |
| `bytes`、`bytes?`（仅限参数） | `const uint8_t *, size_t`：两个参数 | `bytes` |
| `Pointer`、`Pointer?` | `void *` | `Pointer`：不透明，用 `==` 比较 |
| `extern type T`、`T?` | 一个指针 | `T` |
| `extern struct S` | 按值传递的结构体 | `S`，一个结构体 |
| `fn(…) -> R`、`(fn(…) -> R)?`（仅限参数） | `R (*)(void *ctx, …), void *ctx` | 一个函数 |

C 的整数就是 Asuka 中宽度和符号都相同的整数，所以值在两个方向上都完整地传递：`UInt64` 回来时是一个完整的 `u64`，`Int32` 参数接收 `i32`。把 `int` 传给 `Int32` 参数时，要在传入的地方转换：`i32(n)`（放不下时报错）、`i32(truncating: n)` 或 `i32(clamping: n)`，这样放不下的值不会在不知不觉中被截断。`Int32` 这类名称只能写在 extern 声明里；在其他地方它们是 `i32` 等类型（写 `let n: Int32` 会报错，并说明这一点）。`Pointer` 是 Asuka 中一个独立的类型：它是 C 给出的值，Asuka 可以持有、比较、再传回给 C，但从不读取其中的内容。

## 字符串、字节与内存

- **传给 C 的 `CString`** 就是字符串自身的字节，末尾带一个 NUL，不做任何复制。它只在这次调用期间有效，C 如果要保留它，就得自己复制一份。字符串内部含有 NUL 时，C 读到的内容会在那个 NUL 处截止。
- **C 返回的 `CString`** 会被复制成一个 Asuka 字符串。Asuka 不会释放 C 的内存：函数返回的字符串由 C 负责释放，因为大多数 C API 返回的都是它们自己持有的字符串（`sqlite3_errmsg`、`zlibVersion`）。如果某个函数返回的内存需要调用方来释放，可以写几行 C，把它复制到你传入的缓冲区里；或者把它作为 `Pointer` 返回，再用另一个 extern fn 释放。
- **`bytes`** 是指向这些字节的指针加上它们的长度。它只在调用期间有效，C 也不会修改它（`const`）。`bytes?` 为 none 时传 `NULL` 和 `0`。函数不能返回 `bytes`，因为长度没法跟着一起返回：请返回一个 extern type 或 `Pointer`，再用别的函数读取。
- **C 分配的内存**归 C 管。extern type 的 release 函数会在 Asuka 用完时释放它；其他内存则由你调用某个 extern fn 来释放。

## 线程

extern 调用在发起它的线程上运行：在组件、事件处理函数或 task 里，这个线程就是主线程。所以一次耗时很长的调用会卡住界面，和任何同步工作一样。`spawn` 可以把调用放到另一个线程上运行，但只能使用值（extern type 的值不能带过去：它属于主线程）。

```asuka
extern fn hash_file(path: CString) -> UInt64

async fn hashed(path: string) -> u64 {
  return await spawn { hash_file(path) }
}
```

如果 C 要把回调留下来，稍后再调用，或者在它自己的线程上调用，就需要用到 `asuka_ffi.h` 里的两个函数。每个 `[c]` 源文件都可以包含这个头文件：

```c
#include <stddef.h>
#include <stdint.h>

#include "asuka_ffi.h"

static void (*listener)(void *ctx, int64_t value);
static void *listener_ctx;

void on_change(void (*f)(void *ctx, int64_t value), void *ctx) {
    if (listener_ctx) asuka_callback_release(listener_ctx);
    asuka_callback_retain(ctx); // 在这次调用返回之前
    listener = f;
    listener_ctx = ctx;
}

void stop_listening(void) {
    if (listener_ctx) asuka_callback_release(listener_ctx);
    listener = NULL;
    listener_ctx = NULL;
}
```

`asuka_callback_retain` 会一直保留这个函数，直到调用 `asuka_callback_release`，后者可以在任何线程上调用。被保留的回调如果是在 extern 调用之外、或者在另一个线程上被 C 调用，就会被投递到主线程，并立即返回零：函数随后很快会在主线程上、在传入它的那个组件之下执行（它的错误交给那个组件的边界处理）；如果那个组件已经不在了，它就根本不会执行。在 `spawn` 里创建的回调只在那次调用期间有效。

## 构建

`asuka.toml` 的 `[c]` 写明应用要编译哪些 C 代码、链接哪些库；每个平台的表会在它的基础上追加内容。

| 键 | 说明 |
|---|---|
| `sources` | C 文件，相对于 `asuka.toml` 的 glob（`c/*.c`、`c/**/*.c`）。 |
| `include` | 源文件的头文件搜索目录（`-I`）。 |
| `defines` | 宏：`NAME` 或 `NAME=value`（`-D`）。 |
| `flags` | 额外的编译器参数（`-Wno-unused-function`）。 |
| `libraries` | 在平台提供它们时链接的库：`sqlite3` 就是 `-lsqlite3`；写路径（`lib/libfoo.a`）就链接那个文件。 |
| `frameworks` | Apple 的框架，用于 iOS 和 macOS（`Security`）。 |

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

平台的表有 `[c.ios]`、`[c.android]`、`[c.macos]`、`[c.windows]`、`[c.linux]` 和 `[c.web]`。源文件会针对每个目标，用该平台的编译器来编译（Xcode 的、NDK 的；Web 和交叉编译用 Zig 的），发布构建会开启优化，你的警告设置保持原样。

- **iOS**：C 代码以及库和框架都链接进应用的可执行文件。SDK 自带 `libz`、`libsqlite3`、`libxml2`、`libc++` 以及 Apple 的各个框架。
- **Android**：为每个 ABI 编进应用的库。NDK 自带 `libz`、`libm`、`liblog`、`libandroid` 以及其他稳定的 NDK 库；其他库需要以源码形式提供，或者为每个 ABI 提供预编译的 `.a`（见[预编译的库](#预编译的库)）。
- **macOS、Windows 和 Linux**：编进应用的可执行文件。
- **Web**：编进 WebAssembly 模块，目标是 WASI。那里没有系统库，也没有线程，所以库要么以源码形式提供，再用 `[c.web]` 写明要改什么（`defines`、换一组 `sources`），要么是为 WASI 预编译好的。

包的 C 代码由它自己 `asuka.toml` 里的 `[c]` 描述，路径相对于包目录：导入它的应用也会构建这些代码。应用自己的子包（`./util`）共用应用的 `[c]`。

## 下载的源码

不想放进仓库的库（比如 SQLite 的 amalgamation），可以让构建去下载。`[c.download.<name>]` 指定一个压缩包并固定它的 SHA-256，`[c]` 里以 `$<name>` 开头的路径就指向这个包里的内容：

```toml
[c]
sources = ["c/*.c", "$sqlite/sqlite3.c"]
include = ["$sqlite"]

[c.download.sqlite]
url = "https://www.sqlite.org/2026/sqlite-amalgamation-3540100.zip"
sha256 = "0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9"
strip = 1
```

| 键 | 含义 |
|---|---|
| `url` | 压缩包的地址：`https://`，格式为 `.zip`、`.tar.gz` 或 `.tgz`。 |
| `sha256` | 它的 SHA-256（用 `shasum -a 256 file.zip` 可以算出来）。哈希不一致的压缩包会被拒绝。 |
| `strip` | 去掉路径开头的几层目录：大多数压缩包的文件都放在同一个顶层目录里，这时写 `1`。 |

第一次需要它的构建会下载一次，校验后解压到 Asuka 的缓存里，所有项目共用。之后的构建，以及用到同一个压缩包的其他项目，都不再需要联网。单独的 `$sqlite` 指压缩包的顶层目录，`$sqlite/...` 指其中的文件或通配符。`sources`、`include`、`libraries` 里的路径以及 `[c.prebuilt]` 都能这样引用下载的内容，所以厂商编译好的库也可以用这种方式获取。

## 预编译的库

如果厂商提供的是编译好的库而不是源码，就在 `[c.prebuilt]` 里列出来：每个目标一个静态库，头文件目录写在 `include` 里。构建时会链接对应目标的那一个，extern 声明调用它的方式和调用你自己的 C 代码一样。

```toml
[c]
include = ["vendor/acme/include"]

[c.prebuilt]
ios-arm64 = "vendor/acme/ios-arm64/libacme.a"
ios-simulator = "vendor/acme/ios-simulator/libacme.a"  # arm64 和 x86_64 合在一个文件里
macos = "vendor/acme/macos/libacme.a"                  # arm64 和 x86_64 合在一个文件里
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

- **键**是目标：`ios-arm64`、`ios-simulator-arm64`、`ios-simulator-x64`、`macos-arm64`、`macos-x64`、`android-arm64`、`android-arm`、`android-x64`、`linux-x64`、`linux-arm64`、`windows-x64`、`windows-arm64` 和 `web`。`ios-simulator` 和 `macos` 指的是一个通用库（用 `lipo` 合成），同时包含该平台的两种架构；如果某个目标有自己的键，优先用它。
- **值**是库相对于 `asuka.toml` 的路径，也可以是多个路径组成的列表。每个都必须是静态库：`.a`，在 Windows 上也可以是 `.lib`。
- **没有对应库的目标**无法构建，并会说明原因（`[c.prebuilt] has no library for android-x64`）。`asuka test` 和 `asuka run` 链接的是本机对应的那一个（`macos-arm64`、`linux-x64` 等），所以想测试这些调用，也要把本机的目标写上。
- **库对系统的依赖**和源码一样写在各平台的表里：`libraries`（Android 上的 `log`、`z`）和 `frameworks`（`Security`）。
- **构建这些库时**，要面向 Asuka 支持的最低系统版本（iOS 16、macOS 11、Android 7）；Web 版要基于 WASI 的 C 库（`wasm32-wasi`）构建，并且不使用线程。

这些库和你自己的 C 代码一样链接进应用：运行时不需要在应用旁边查找、嵌入或签名任何东西，应用没用到的部分也会被去掉。包的 `[c.prebuilt]` 会随包一起发布，库文件就在包的 tarball 里。

厂商的动态库不能通过 `[c]` 链接。iOS 或 macOS 的动态库（一个 framework 的 XCFramework）和 Android 的动态库（每个 ABI 一个 `.so`，或者一个 AAR）要放在[原生包](native.md)里：作为它的 Swift 包的 binary target，或者放进它 Gradle 模块的 `jniLibs` 或依赖中。各平台自己的构建会负责嵌入、签名和加载它们，再由包里的 Swift 和 Kotlin 代码调用。

## 开发

- `asuka dev` 会构建带有应用 C 代码的开发 App。修改 C 文件或头文件、`asuka.toml` 或者某个 extern 声明，会重新构建开发 App 并重新启动它；修改其余的 Asuka 代码只会热重载，状态保留。
- `asuka test` 和 `asuka run` 会为当前这台电脑（对应 `[c.macos]`、`[c.linux]` 或 `[c.windows]` 表）构建带有这些 C 代码的运行器，因此测试调用的就是应用所用的那份 C。`asuka run file.asuka` 读取文件旁边的 `asuka.toml`。
- 编译器会检查每一条声明：如果某个类型无法跨过边界，它会说明原因以及应该怎么写（`parameter s is a string: it crosses to C as a CString`、`field name of extern struct User can't be a CString: a struct crosses by value, and the string's memory wouldn't outlive the call`）。
- C 自己的错误（签名写错、使用已释放的指针）由 C 负责：编译器看不到 C 那一侧，所以请按照 C 头文件里的声明来写类型，并用 `asuka test` 测试这些调用。

## 工作原理

对每个包含 extern 声明的包，编译器都会生成胶水代码：它负责转换调用的参数、调用 C 函数、再转换返回值，也让 C 能够调用 Asuka 的回调。无论在 `asuka dev` 下还是在发布构建中，调用的方式都一样。胶水代码根据 extern 声明来声明每个 C 函数，因此不需要库的任何头文件。
