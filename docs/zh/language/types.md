# 值与类型

| 类型 | 值 |
|---|---|
| `int` | 64 位整数：`42`、`1_000`、`0xff`、`0b1010`、`0o17` |
| `i8`、`i16`、`i32`、`u8`、`u16`、`u32`、`u64` | 8、16、32 和 64 位整数，分有符号（`i`）和无符号（`u`）两种：见[整数类型](#整数类型) |
| `float` | 64 位浮点数：`3.14`、`1e-3` |
| `bool` | `true`、`false` |
| `string` | UTF-8 文本：`"Hello, {name}"` |
| `char` | 一个 Unicode 码位（标量值）：`'a'`、`'\n'`、`'\u{1F600}'` |
| `bytes` | 原始字节 |
| `[T]` | 数组：`[1, 2, 3]`、`[]` |
| `[K: V]` | map，按插入顺序保存：`["a": 1]`、`[:]` |
| `Set<T>` | 集合：一组互不相同的值，按插入顺序保存；在需要集合的地方写 `["a", "b"]` |
| `(A, B)` | 元组：`(1, "one")` |
| `T?` | 可选值：一个 `T`，或者 `none` |
| `Duration` | `300ms`、`5s`、`2min`、`1h`（还有 `us`、`ns`） |
| `Color` | `#7c3aed`、`#7c3aed80`，以及语义色：`.label`、`.accent` …… |
| `Length` | `100`（点）、`50%`、`.auto` |
| `fn(A) -> B` | 函数和闭包 |

字符串的花括号里可以放任意表达式：`"{count} items"`、`"Total: {price * qty}"`。`\{` 表示一个花括号字符；`r"…"` 是原始字符串，没有转义也没有插值；要在原始字符串里写 `"`，用 `r#"…"#`：它在一个 `"` 后面跟着与开头同样多的 `#` 时才结束（`r#"say "hi""#`、`r##"a "# b"##`）。`"""…"""` 可以跨多行（第一行为空时不算在内，结束引号所在那一行的缩进也会去掉），里面的 `"` 和 `""` 照原样保留；`r"""…"""` 是跨多行的原始字符串。不是数字、布尔值或字符串的值，显示为文本时是这样的：类型有自己的 `description()` 时用它（见[声明](declarations.md#相等哈希文本和-json)），可选值显示为 `none` 或其中的值，错误显示为它的消息，枚举成员显示为它的名字和负载（`green`、`circle(radius: 1.5)`），其余的值就按语言里的写法显示，和 Swift 打印值的方式一样：颜色和时长按字面写法（`#0a84ff`、`.accent`、`1500ms`、`2s`），结构体写成类型名加各个字段（`Point(x: 1, y: 2)`），元组、数组、map 和 set 写成它们的字面量（`(1, "a")`、`[1, 2]`、`["a": 1]`、`[:]`、`Set([1, 2])`），bytes 显示为字节数（`<3 bytes>`）。它们里面的值和代码里的写法一样：字符串带引号和转义（`"say \"hi\" \{x}"`），浮点数带小数点（`2.0`），char 带单引号（`'a'`），枚举成员带点（`.red`），以及 `none`；里面某个值的类型有自己的 `description()` 时用它，有自己的 `toJSON()` 时用它给出的值。显示的总是完整的值，不管多长、嵌套多深（要写成 JSON 用 `json.encode`）。函数没有文本形式，显示为它的类型（`<fn(int) -> int>`；在其他值里面显示为 `<fn>`）。

插值可以像 Python 和 Rust 那样，在冒号后面说明值怎么写出来：`"{price:>8.2f}"`。格式是 `[[fill]align][sign][#][0][width][,|_][.precision][type]`：`<`、`>`、`^` 表示在宽度内左对齐、右对齐、居中，前面可以写填充字符（默认是空格）；`+` 让正数也带上加号；`0` 在符号之后用 0 补足宽度；`,` 或 `_` 给整数（以及浮点数的整数部分）分组；`.2` 是浮点数小数点后的位数，对文本来说是最多保留的字符数。类型可以是 `x`、`X`、`o`、`b`，把整数写成十六进制、八进制或二进制（加上 `#` 会在前面写出 `0x`、`0o`、`0b`），也可以是 `f`、`e`、`%`，把数字写成定点小数、指数形式或百分比。数字默认右对齐，其他值按它的文本左对齐，只能指定填充字符、对齐、宽度和精度。格式由编译器读取：和值对不上的格式会在写的地方报错（[format-spec](../errors/format-spec.md)）。

```asuka check
fn row(name: string, count: int, share: float) -> string = "{name:<10}{count:>6,}{share:>8.1%}"

let price = "{1234.5:,.2f}"          // 1,234.50
let hex = "{255:#06x}"               // 0x00ff
let title = "{"Total":*^11}"         // ***Total***
let table = row("asuka", 12500, 0.4) // asuka     12,500   40.0%
```

宽度按字符（码位）计算，而不是按终端里显示的宽度。要按终端的列数算（中日韩文字和 emoji 占两列），用 `std/term` 的 `textWidth`、`padEnd` 和 `fit`，见[程序](../programs.md#对齐的表格)。

`print(a, b, c)` 把各个值按插值显示的样子写成标准输出（应用的日志）上的一行，值之间用空格隔开；`eprint` 则写到标准错误。和 Swift 的 `print` 一样，值后面可以加 `separator:`（值之间写什么）和 `terminator:`（最后写什么，默认是换行）：`print(x, y, separator: ", ")`、`print("Saving…", terminator: "")`。

元组的元素可以按位置读取：`pair.0`、`pair.1`；如果元组存在 `var` 里，也可以就地修改（`pair.0 += 1`）。写一个元组没有的位置（对一对值写 `pair.2`）是编译错误。也可以把元组拆开：`let (count, label) = pair`、`for (i, x) in xs.enumerated()`，或者在 `match` 里用元组模式（见[控制流](control-flow.md#控制流)）。

map 的键和集合的元素必须满足 `Hashable`：整数、字符串、布尔值、字节串、时长、颜色，以及由它们组成的枚举、结构体、元组、可选值、数组、map 和集合（包含自身的结构体或枚举，只要其余部分满足，也满足；map 要求键和值都满足）。浮点数不满足，因为 NaN 不等于它自己；函数也不满足：

```asuka
struct Cell {
  row: int
  col: int
}

let marks: [Cell: string] = [Cell(row: 0, col: 2): "x"]
let seen: Set<(int, int)> = [(0, 0), (1, 2)]
```

## 运算符

下表按结合从紧到松排列；同一行的运算符优先级相同，从左往右结合（`**` 和 `??` 从右往左）：

| 运算符 | 作用 |
|---|---|
| `**` | 乘方：`2 ** 10` 是 1024 |
| `!x`、`-x`、`~x` | 取反、取负、把整数的每一位取反 |
| `<<`、`>>` | 把整数的位左移、右移（有符号整数右移时保留符号） |
| `*`、`/`、`%`、`&`、`&*` | 乘、除、取余、按位与、回绕乘法 |
| `+`、`-`、<code>&#124;</code>、`^`、`&+`、`&-` | 加（也用于拼接字符串和数组）、减、按位或、按位异或、回绕加法和减法 |
| `..`、`..<` | 范围：`1..3`、`0..<n` |
| `==`、`!=`、`<`、`<=`、`>`、`>=` | 比较（不能连写） |
| `&&` | 与 |
| <code>&#124;&#124;</code> | 或 |
| `??` | 可选值里的值，为 none 时取默认值 |

`<`、`<=`、`>` 和 `>=` 可以比较整数、浮点数、字符串、时长、成员不携带值的枚举（按成员声明的顺序），以及带有 `compare` 方法的类型；结构体和枚举也能从自己的方法 `add`、`sub`、`mul`、`div`、`rem`、`pow` 和 `neg` 获得算术运算符（见[声明](declarations.md#结构体)）。

`**` 比上表里的其他运算符结合得都紧，`-` 也不例外，而且从右往左结合，和 Python 以及数学里一样：`-2 ** 2` 是 `-(2 ** 2)`，即 -4；`2 ** 3 ** 2` 是 `2 ** 9`；`(-2) ** 2` 是 4。整数的乘方和底数是同一类型，指数可以是任意类型的整数，但必须大于或等于 0（负指数是一个错误：`2 ** -1` 不是整数）；浮点数的乘方，指数可以是浮点数，也可以是整数（`2.0 ** 0.5`、`x ** 2`）。`**=` 是对应的赋值写法。

按位运算符只接受整数，而且两边必须是同一类型。移位数可以是任意类型的整数；移位数达到或超过该类型的位宽时，所有位都会被移出（负数右移得到 -1），移位数为负数则是一个错误。`>>` 对有符号整数保留符号，对无符号整数在高位补 0。有了这些运算符和 `b[i]`（以整数形式读出一个字节），程序就能读写二进制数据，比如蓝牙特征值或 NFC 记录（[`std/encoding`](../std/encoding.md) 能按任一字节序读写整个整数）：

```asuka
let raw = b[1] | b[2] << 8                         // 小端序的 uint16
let celsius = if raw & 0x8000 != 0 { raw - 0x10000 } else { raw }
let out = bytes([flags, value & 0xff, value >> 8])
```

`int` 的取值范围是 −2⁶³ 到 2⁶³ − 1。`+`、`-`、`*`、`**` 的结果超出这个范围时，会像除以零（`/`、`%`）一样抛出错误 “integer overflow”：`catch` 可以捕获它；没有地方捕获时，它会交给最近的 `Boundary`（见[错误](errors.md#错误)）。对最小的 `int` 取负（`-x`）、取绝对值（`abs(x)`）、除以 `-1`，以及数组的 `sum()` 超出范围，也都一样。计数、大小或价格一旦溢出，就是一个在出错的地方暴露出来的 bug，而不是一个被悄悄存下来的错误数字。每种整数类型都按同样的方式检查，只是用各自的范围：`u8` 的 `255 + 1` 会抛出错误。

```asuka
let big = 9223372036854775807
let n = try big + 1 catch { 0 }               // 0：抛出了错误
let next = big.addingChecked(1) ?? big          // 溢出时为 none：得到 big
```

超出 `int` 范围的数，可以用 `std/bigint` 的 `BigInt`：它是任意大小的整数，同样支持算术运算符和比较（见[标准库](../stdlib.md)）：

```asuka check
import std/bigint.{BigInt}

let big = BigInt.of(9223372036854775807)
let past = big * big + BigInt.one           // 85070591730234615847396907784232501250
let digits = "{BigInt.factorial(30)}"       // 265252859812191058636308480000000
```

`&+`、`&-` 和 `&*` 则会回绕：结果超过最大的 `int` 时，按 64 位二进制补码的规则从最小值接着算下去（`9223372036854775807 &+ 1` 等于 `-9223372036854775808`）；固定位宽的类型则在各自的位宽处回绕（`u8` 的 `255 &+ 1` 是 0）。哈希、校验和以及二进制协议需要的正是这种行为；`&+=`、`&-=` 和 `&*=` 是对应的赋值写法：

```asuka
fn fnv1a(data: bytes) -> int {
  var h = 0xcbf2_9ce4_8422_2325
  for i in 0..<data.count() {
    h ^= data[i]
    h &*= 0x100_0000_01b3
  }
  return h
}
```

想把溢出当作一个值来处理时，用 `a.addingChecked(b)`、`subtractingChecked` 和 `multipliedChecked`：它们返回 `int?`（对 `u8` 返回 `u8?`，依此类推），溢出时为 none。移位和按位运算符不会因为结果而抛出错误；对超出范围的浮点数调用 `int(x)`，得到离它最近的 `int`（NaN 得到 `0`）。

## 整数类型

除了 `int`，还有固定位宽的整数，分有符号和无符号两种：

| 类型 | 值 |
|---|---|
| `i8`、`i16`、`i32` | −2⁷ 到 2⁷ − 1、−2¹⁵ 到 2¹⁵ − 1、−2³¹ 到 2³¹ − 1 |
| `int` | −2⁶³ 到 2⁶³ − 1（没有 `i64`：它就是 `int`） |
| `u8`、`u16`、`u32`、`u64` | 0 到 2⁸ − 1（255）、2¹⁶ − 1、2³² − 1、2⁶⁴ − 1 |

计数、索引、大小以及以分为单位的金额都用 `int`；其他类型留给本身就有固定位宽的东西：一个字节（`u8`）、像素的一个通道、文件格式或协议里的字段、哈希值、超出 `int` 范围的 ID（`u64`），以及 C 函数用到的值（`Int32` 就是 `i32`，见[在 Asuka 中调用 C](../c.md)）。每种类型都严格保持自己的范围：和 `int` 一样，算术结果超出范围就会抛出错误；JSON、CSV、TOML 和 SQLite 读进这种类型的值，也会经过同样的检查。

整数字面量的类型由此处期望的类型决定，而且必须放得下：`let level: u8 = 200`、`level + 1`、`let big: u64 = 18446744073709551615`；`let x: u8 = 256` 和 `let y: u8 = -1` 会在写的地方报错。没有期望的类型时，它就是 `int`。

类型不同的两个整数不能放在同一个运算符两边：`int` 和 `u8` 不行，就像 `int` 和 `float` 不行一样。`level` 是 `u8`、`offset` 是 `int` 时，`level + offset` 是一个错误（[mixed-numbers](../errors/mixed-numbers.md)），在需要 `int` 的地方传入 `u8` 也是。要用转换写明想要哪种：

```asuka check
fn conversions(n: int, level: u8, ratio: float) -> [u8] {
  let total = int(level) + n        // 扩宽：总是放得下
  let a = u8(n)                     // n 放不下时是一个错误（"integer overflow"）
  let b = u8(exactly: n) ?? 0       // 一个 u8?：放不下时为 none
  let c = u8(truncating: n)         // 取低 8 位：u8(truncating: 300) 是 44，-1 得到 255
  let d = u8(clamping: n)           // 取最接近的值：低于范围得 0，高于范围得 255
  let e = u8(ratio)                 // 浮点数：向零取整，超出范围时取最接近的值
  return [a, b, c, d, e, u8(clamping: total)]
}
```

`u8.min` 和 `u8.max` 是类型的范围（`int.max` 是 9223372036854775807）；`u64.parse(text)` 从文本读出一个值，就像 `toInt()` 读出 `int` 一样（也支持 `radix:`），文本不是整数或放不下时返回 none。超过 `int` 最大值的 `u64`，在打印、解析、比较、除法和写入 JSON 时都按它本来的数值处理（`18446744073709551615`）。

`-x` 和 `abs(x)` 只接受有符号整数：无符号整数没有负数（`0 &- x` 会回绕）。各种整数类型都有 `int` 的方法：`toString(radix:)`、`padded(width)`、`float()`、`abs()` 以及那几个 checked 方法；它们都能使用格式说明（`"{id:x}"`），按数值排序和哈希（可以作 map 和集合的键，在泛型里满足 `Comparable` 和 `Hashable`），也能构成范围：`for i in u16(0)..<count`。

从 `bytes` 读出的字节是 `int`（`b[i]`，0 到 255），所以对它做算术不会在 8 位处回绕；`b.toArray()` 把这些字节作为 `[u8]` 返回，`bytes(xs)` 能从 `[u8]` 构造字节串，和从 `[int]` 构造一样。

## 内置方法

字符串、bytes、数组、集合、map 和可选值都有方法。其中会修改值本身的方法（`push`、`pushFirst`、`insert`、`removeAt`、`removeWhere`、`pop`、`popFirst`、`clear`、`sort`、`remove`）只能用在 `var` 或状态上：

| 类型 | 方法 |
|---|---|
| `[T]` | `count()`、`count(where)`、`isEmpty()`、`first()`、`last()`、`get(i)`（返回 `T?`）、`push(x)`、`pushFirst(x)`、`insert(x, at: i)`、`removeAt(i)`、`removeWhere(p)`、`pop()`、`popFirst()`、`clear()`、`map(f)`、`flatMap(f)`（把 `f` 返回的数组依次接起来）、`compactMap(f)`（`f` 的结果中去掉 none）、`filter(p)`、`reduce(initial, f)`、`any(p)`、`all(p)`、`index(p)`（返回 `int?`）、`find(p)`、`contains(x)`、`sum()`、`min()`、`max()`（返回 `T?`；`min(by:)`、`max(by:)` 接受一个“小于”比较，`min(key:)`、`max(key:)` 按键比较）、`unique()`、`grouped(by: f)`（返回 `[K: [T]]`）、`chunked(n)`、`zip(other)`（两两配对，长度取较短的一方）、`reversed()`、`slice(from, to)`（省略 `to` 时到末尾）、`prefix(n)`、`suffix(n)`、`sorted()`、`sorted(by)`、`sorted(key:, descending:)`、`sort()`、`sort(by)`、`sort(key:, descending:)`、`enumerated()`、`joined(separator:)`（用于字符串数组）、`lazy()`（得到 `Seq<T>`）、`+` |
| `Set<T>` | `count()`、`isEmpty()`、`contains(x)`、`insert(x)`（返回是否新加入）、`remove(x)`（返回原本是否存在）、`union(s)`、`intersection(s)`、`subtracting(s)`、`isSubset(of: s)`、`isSuperset(of: s)`、`lazy()`；`Set(xs)` 是数组中互不相同的元素，`Set<T>()` 是空集合 |
| `[K: V]` | `m[k]`（返回 `V?`）、`m[k] = v`、`count()`、`isEmpty()`、`has(k)`、`get(k)`、`remove(k)`、`keys()`、`values()`（按插入顺序）、`mapValues(f)`、`filter((k, v) => …)`、`lazy()`（它的 `(key, value)` 对） |
| `string` | `count()`（字符数）、`byteCount()`、`isEmpty()`、`upper()`、`lower()`、`trim()`、`trimStart()`、`trimEnd()`、`trimPrefix(s)`、`trimSuffix(s)`、`contains(s, ignoreCase:)`、`hasPrefix(s, ignoreCase:)`、`hasSuffix(s, ignoreCase:)`、`equals(s, ignoreCase:)`、`compare(s, ignoreCase:)`（负数、零或正数）、`foldCase()`、`replace(old, new, count:)`（默认替换全部，count 指定次数）、`split(sep, count:)`、`fields()`（按空白分出的词）、`lines()`、`find(s)`、`findLast(s)`、`findAny(chars)`（字节偏移，找不到时为 none）、`slice(from, to)`（按字符；省略 `to` 表示到末尾）、`byteSlice(from, to)`（按字节：即 find 给出的偏移；省略 `to` 表示到末尾）、`padStart(width, with:)`、`padEnd(width, with:)`、`toInt()`、`toInt(radix:)`、`toFloat()`、`repeat(n)`、`chars()`、`graphemes()`、`lazy()`（它的 char，一个 `Seq<char>`）、`reversed()`、`bytes()`、`+` |
| `bytes` | `b[i]`（返回 `int`，取值 0 到 255；越界是一个错误）、`count()`、`isEmpty()`、`get(i)`（返回 `int?`）、`slice(from, to)`（省略 `to` 表示到末尾）、`concat(b)`、`string()`（按 UTF-8 解码，无效的部分变成 U+FFFD）、`hex()` 和 `hex(separator)`（`"04 a1"`）、`toArray()`（返回 `[u8]`）；`bytes()` 是空的字节序列，`bytes([0x04, 0xa1])` 是这些字节，`bytes(xs)` 是一个 `[u8]` 里的字节，`s.bytes()` 是字符串的 UTF-8 字节 |
| `int`、`i8` … `u64` | `float()`、`abs()`（用于有符号整数）、`toString(radix:)`（`255.toString(radix: 16)` 是 `"ff"`）、`padded(width)`（`7.padded(2)` 是 `"07"`）、`addingChecked(o)`、`subtractingChecked(o)`、`multipliedChecked(o)`（返回 `int?`，对 `u8` 返回 `u8?`：溢出时为 none）；`T.min`、`T.max`、`T.parse(text, radix:)`（返回 `T?`） |
| `float` | `fixed(digits)`、`rounded()`、`floor()`、`ceil()`、`int()`（向零取整）、`abs()` |
| `Color` | `Color.parse(text)`（`#rrggbb`、`#rgb`、`#rrggbbaa`：返回 `Color?`）、`Color.rgb(r, g, b, alpha:)`；`opacity(a)`：以原本不透明度的 a 倍绘制（语义颜色仍然是语义颜色）；`hex()`（`"#0a84ff"`；语义颜色则是它的名字：`".accent"`） |
| `Duration` | `nanoseconds()`、`milliseconds()`（整数）、`seconds()`、`minutes()`、`hours()`（浮点数）、`rounded(to:)`、`truncated(to:)`、`+`、`-`、`* n`（整数或浮点数：`1.5 * 1h`）、`/ n`、`/ d`（另一个 Duration：它能容纳几个，一个浮点数） |
| `Length` | `* n`（`width: 50% * 2`：缩放一个长度） |
| `T?` | `isNone()`、`isSome()` |

会调用你传入的函数的方法（`map`、`filter`、`reduce`、`sorted(by:)`、`grouped(by:)`、map 的 `mapValues` ……）都接受会抛出错误的函数，这时它们会把函数抛出的错误原样抛出，所以调用要加 `try`。传入的函数不会抛出错误时，它们也不会。

```asuka
fn port(text: string) throws -> int = text.toInt() ?? throw Error("not a port: {text}")

fn ports(lines: [string]) throws -> [int] = try lines.map(l => try port(l))
```

下面是其中几个方法的实际用法：

```asuka check
struct Sale {
  region: string
  amount: int
}

fn report(sales: [Sale]) -> [string] {
  let byRegion = sales.grouped(by: s => s.region)
  let totals = byRegion.mapValues(ss => ss.map(s => s.amount).sum())
  let best = sales.max(by: (a, b) => a.amount < b.amount)
  let codes = ["7", "x", "42"].compactMap(c => c.toInt())    // [7, 42]
  let pages = sales.chunked(50)                               // [[Sale]]，每组 50 个
  var out = ["{best?.region ?? "none"} sold the most"]
  for (region, total) in totals { out.push("{region}: {total}") }
  for (i, page) in pages.enumerated() { out.push("page {i + 1}: {page.count()}") }
  out.push("{codes.reduce(0, (sum, c) => sum + c)}")
  return out
}
```

`unique()` 让每个元素只保留一次，留在它第一次出现的位置；`grouped(by:)` 的键也按第一次出现的顺序排列。两者都要求值是 `Hashable` 的（`grouped` 要求的是键）。`min()` 和 `max()` 的排序方式与 `sorted()` 相同，遇到相等的元素时取第一个。`chunked(n)` 的 `n` 小于 1 时会报错，就像 `xs[i]` 越界一样。

`sorted(key:)` 按函数给每个元素算出的键排序，键必须是 `Comparable` 的，并且每个元素只算一次：`people.sorted(key: p => p.age)`。加上 `descending: true` 就从大到小排；无论哪个方向，排序都是稳定的，键相等的元素保持原来的先后顺序。`sort(key:)` 原地排序一个 `var`，`min(key:)` 和 `max(key:)` 则给出键最小或最大的那个元素（键相等时取第一个）。如果顺序不是由某一个键决定的，就用 `sorted(by:)`，传入一个“小于”比较。

```asuka check
struct Player {
  name: string
  score: int
  joined: int
}

fn ranking(players: [Player]) -> [string] {
  let best = players.sorted(key: p => p.score, descending: true)
  let newest = players.max(key: p => p.joined)
  return best.map(p => p.name) + [newest?.name ?? ""]
}
```

数组同时也是双端队列：`pushFirst(x)` 和 `popFirst()` 修改数组的开头，就像 `push(x)` 和 `pop()` 修改末尾一样，每次都是 O(1)（均摊：数组在开头被修改过之后，会在开头预留空间）；`insert(x, at: 0)` 和 `removeAt(0)` 也一样。在其他位置插入或删除时，只移动较短一侧的元素。所以队列就是一个数组：`queue.push(job)`，再 `queue.popFirst()`。

`toInt(radix:)` 按 2 到 36 之间的进制读取整个字符串，字母大小写均可，前面可以带正负号（`"ff".toInt(radix: 16)` 是 255，`"-101".toInt(radix: 2)` 是 -5），读不出来时返回 none；`toString(radix:)` 则把整数按进制写出来，字母用小写。进制不在 2 到 36 之间时会报错。

`min`、`max`、`abs`、`float(n)`、`int(x)` 以及其他整数类型的转换（`u8(x)`）都是函数；其余的（`math.sqrt`、`math.pow`、`math.pi` ……）在 `std/math` 里。

`upper()` 和 `lower()` 按 Unicode 的规则转换每个字母，在任何语言下结果都一样：`"straße".upper()` 是 `"STRASSE"`，单词末尾的 `Σ` 转成小写是 `ς`。`ignoreCase: true` 比较的是两个字符串的大小写折叠（`foldCase()`：`"Straße"` 和 `"STRASSE"` 都折叠成 `"strasse"`），和 Go 的 `strings.EqualFold`、Python 的 `casefold` 一样；匹配必须从完整的字符开始、在完整的字符结束，所以 `"ß"` 并不以 `"s"` 开头。这些方法都不遵循某种语言自己的规则：土耳其语的无点 ı 和带点 İ 和其他地方一样转换（`"İ".lower()` 是 `i` 加一个组合用的点），文本也不会做规范化（`"é"` 和 `e` 加组合重音符是不同的）。不区分大小写地给名字排序，可以把 `compare` 交给 `sorted(by:)`；`foldCase()` 能生成忽略大小写的键：

```asuka check
fn byName(names: [string]) -> [string] = names.sorted(by: (a, b) => a.compare(b, ignoreCase: true) < 0)

fn sameUser(a: string, b: string) -> bool = a.trim().equals(b.trim(), ignoreCase: true)

fn isMarkdown(path: string) -> bool = path.hasSuffix(".md", ignoreCase: true)
```

`count()` 和 `chars()` 按码位计数；`graphemes()` 给出读者眼中的一个个字符，依据的是 Unicode 的文本分段规则（UAX #29）：`"e\u{301}"`、一面国旗（`"🇯🇵"`）、用 ZWJ 连起来的家庭 emoji 都各算一个。

`char` 是一个码位，和 Rust 的 char 一样：`for c in s` 遍历的、`s.chars()` 按顺序给出的就是它（由几个码位组成的字素，比如 `e` 加重音符号，是字符串：用 `graphemes()`）。它写在单引号里（`'a'`、`'\''`、`'\u{301}'`），按码位比较和排序，可以做 map 的键；打印出来就是它自己，在 JSON 里是只有一个字符的字符串。`c.isLetter()`、`isDigit()`、`isNumber()`、`isWhitespace()`、`isUpper()`、`isLower()`、`isPunctuation()` 和 `isSymbol()` 读取 Unicode 的分类，适用于任何文字（`'٣'.isDigit()`）；`upper()` 和 `lower()` 在结果仍是一个 char 时做映射（`'ß'.upper()` 仍是 `'ß'`：`"ß".upper()` 才是 `"SS"`）；`code()` 是它的编号，`char(n)` 给出编号为 n 的 char（n 不是码位时是错误）。`s + c` 和 `s += c` 把一个 char 加到字符串上。

```asuka check
fn initials(name: string) -> string {
  var out = ""
  var start = true
  for c in name {
    if c.isWhitespace() {
      start = true
    } else if start {
      out += c.upper()
      start = false
    }
  }
  return out
}

fn digits(s: string) -> int = s.chars().filter(c => c.isDigit()).count()

fn kind(c: char) -> string = match c {
  'a' | 'e' | 'i' | 'o' | 'u' => "vowel"
  _ => if c.isLetter() { "letter" } else { "other" }
}
```


`toInt()` 和 `toFloat()` 读的是整个字符串，读不出来就是 none：可选的正负号，接着是数字，前后不能有别的东西（`" 12"` 是 none，先 `trim()`）。`toInt()` 只认整数（`"+7"`、`"-42"`）；`toFloat()` 还认小数和指数（`"2.5"`、`".5"`、`"1e-3"`），以及浮点数打印出来的样子（`"Infinity"`、`"NaN"`）。无论用户的语言是什么，小数点都是 `.`。

## 惰性序列

`xs.lazy()` 把数组的元素变成一个 `Seq<T>`，也就是惰性序列。在它上面调用 `map`、`filter`、`take` 等方法，只会得到另一个 `Seq`，什么也不计算；等到有东西读取它时（`for`、`toArray()`、`first()`、`reduce` ……），才一个一个地算出元素，每个元素一次走完所有步骤，中间不产生数组。因此，对一百万个元素做一串操作、最后只留三个时，只会计算这三个以及找到它们所需的那些元素；序列也可以没有尽头：

```asuka check
fn squaresUnder(limit: int) -> [int] = Seq(from: 1).map(n => n * n).takeWhile(n => n < limit).toArray()

fn firstLong(lines: [string]) -> string? = lines.lazy().map(l => l.trim()).find(where: l => l.count() > 80)

fn fibonacci() -> Seq<int> = Seq(state: (0, 1), next: s => (s.0, (s.1, s.0 + s.1)))
```

map 的 `lazy()` 给出它的 `(key, value)` 对，集合的给出元素，字符串的给出字符，区间的（`(0..<n).lazy()`）给出其中的数字，而且不会先创建数组。`Seq(…)` 则用一个函数生成序列：

| 写法 | 元素 |
|---|---|
| `Seq(from: n, by: step)` | `n`、`n + step`、……，没有尽头；不写 `by` 时步长是 1 |
| `Seq(first: x, next: f)` | `x`、`f(x)`、`f(f(x))`、……，直到 `f` 返回 none |
| `Seq(state: s, next: f)` | `f(s)` 返回下一个元素和下一个状态 `(x, s2)`，结束时返回 none |
| `Seq(repeating: x, count: n)` | `x` 重复 `n` 次；不写 `count` 时没有尽头 |

| | 方法 |
|---|---|
| 得到新的 `Seq` | `map(f)`、`filter(p)`、`compactMap(f)`、`flatMap(f)`（把 `f` 返回的各个 `Seq` 的元素依次接起来）、`enumerated()`、`zip(other)`、`take(n)`、`skip(n)`、`takeWhile(p)`、`skipWhile(p)`、`chunked(n)`、`windows(n)`（每相邻的 `n` 个：`[1, 2, 3]` 的 `windows(2)` 是 `[1, 2]`、`[2, 3]`）、`onEach(f)`（读到每个元素时调用 `f`）、`unique()`、`concat(other)` |
| 读取它 | `toArray()`、`toSet()`、`toMap()`（由 `(key, value)` 对组成）、`count()`、`count(where:)`、`first()`、`last()`、`find(where:)`、`any(where:)`、`all(where:)`、`contains(x)`、`reduce(initial, f)`、`sum()`、`min()`、`max()`、`min(by:)`、`max(by:)`、`min(key:)`、`max(key:)`、`joined(separator:)` |

`take`、`find`、`first`、`any` 和 `all` 一旦得到答案就不再往下取元素，后面的元素根本不会被计算。每次读取 `Seq` 都从头开始（用 `for` 遍历两次，就会计算两次）。它保存的是创建时数据源的值，和其他值一样：之后再修改那个数组，不会影响它。

步骤里的函数要等到读取序列时才运行，因此不能抛出错误：需要抛出时，在 `for` 里做，或者用数组的 `map`。读取时立即调用的函数（`reduce`、`find(where:)`、`any`、`count(where:)`、`min(key:)` ……）可以抛出，这时调用会把它抛出的错误原样抛出，和数组的方法一样。

优先队列，以及按键的顺序保存的 map 和集合，是 `std/collections` 的 `Heap`、`SortedMap` 和 `SortedSet`（见[标准库](../stdlib.md)）。

## 值语义

字符串、数组、集合、map、结构体和枚举都是**值**：赋值或传参时，从你能观察到的行为来看，得到的都是一份副本。副本的开销很小：多个副本共享同一块内存，直到其中一个被修改；如果一个值只有一处持有，修改时会直接原地修改。

```asuka
var a = [1, 2, 3]
var b = a
b.push(4)        // a 仍然是 [1, 2, 3]
```

store、闭包、task 和视图是**引用**：复制出来的仍是同一个东西。

语言里没有 null。可能缺失的值用可选值表示：

```asuka
let user: User? = none
if let u = user { Text(u.name) }
let name = user?.name ?? "Guest"
let first = items.first()!        // 为 none 时会崩溃：只在确定不可能为 none 时这样写
```

`?.` 之后，链条剩下的部分遇到 none 也会停下：`user?.boss.name` 的类型是 `string?`。

可选值不会嵌套：一个值要么有，要么没有。写 `int??` 是错误（一个 `?` 就够了）；泛型的 `T?` 里，如果 `T` 本身已经是可选值，`T?` 就是这个 `T`。
