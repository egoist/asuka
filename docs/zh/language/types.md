# 值与类型

| 类型 | 值 |
|---|---|
| `int` | 64 位整数：`42`、`1_000`、`0xff`、`0b1010`、`0o17` |
| `float` | 64 位浮点数：`3.14`、`1e-3` |
| `bool` | `true`、`false` |
| `string` | UTF-8 文本：`"Hello, {name}"` |
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

字符串的花括号里可以放任意表达式：`"{count} items"`、`"Total: {price * qty}"`。`\{` 表示一个花括号字符；`r"…"` 是原始字符串，没有转义也没有插值；要在原始字符串里写 `"`，用 `r#"…"#`：它在一个 `"` 后面跟着与开头同样多的 `#` 时才结束（`r#"say "hi""#`、`r##"a "# b"##`）。`"""…"""` 可以跨多行（第一行为空时不算在内，结束引号所在那一行的缩进也会去掉），里面的 `"` 和 `""` 照原样保留；`r"""…"""` 是跨多行的原始字符串。不是数字、布尔值或字符串的值，显示为文本时是这样的：枚举成员显示为它的名字，可选值显示为 `none` 或其中的值，错误显示为它的消息，颜色按写法显示（`#0a84ff`、`.accent`），时长以毫秒显示（`1500ms`），其他值显示为 JSON。JSON 表示不了的值也按同样的方式写出：元组写成括号里的各个元素，每个元素的写法和数组元素一样（`(1,"a","red")`）；键不是字符串的 map，每个键按它自己的写法写出（`{1:"a"}`）；函数没有文本形式，显示为它的类型（`<fn(int) -> int>`）。

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

下表按结合从紧到松排列；同一行的运算符优先级相同，从左往右结合（`??` 从右往左）：

| 运算符 | 作用 |
|---|---|
| `!x`、`-x`、`~x` | 取反、取负、把整数的每一位取反 |
| `<<`、`>>` | 把整数的位左移、右移（右移保留符号） |
| `*`、`/`、`%`、`&`、`&*` | 乘、除、取余、按位与、回绕乘法 |
| `+`、`-`、<code>&#124;</code>、`^`、`&+`、`&-` | 加（也用于拼接字符串和数组）、减、按位或、按位异或、回绕加法和减法 |
| `..`、`..<` | 范围：`1..3`、`0..<n` |
| `==`、`!=`、`<`、`<=`、`>`、`>=` | 比较（不能连写） |
| `&&` | 与 |
| <code>&#124;&#124;</code> | 或 |
| `??` | 可选值里的值，为 none 时取默认值 |

`<`、`<=`、`>` 和 `>=` 可以比较整数、浮点数、字符串、时长、成员不携带值的枚举（按成员声明的顺序），以及带有 `compare` 方法的类型；结构体和枚举也能从自己的方法 `add`、`sub`、`mul`、`div`、`rem` 和 `neg` 获得算术运算符（见[声明](declarations.md#结构体)）。

按位运算符只接受整数。移位 64 位或更多时，所有位都会被移出（负数右移得到 -1）；移位数为负数则是一个错误。有了这些运算符和 `b[i]`（以整数形式读出一个字节），程序就能读写二进制数据，比如蓝牙特征值或 NFC 记录：

```asuka
let raw = b[1] | b[2] << 8                         // 小端序的 uint16
let celsius = if raw & 0x8000 != 0 { raw - 0x10000 } else { raw }
let out = bytes([flags, value & 0xff, value >> 8])
```

`int` 的取值范围是 −2⁶³ 到 2⁶³ − 1。`+`、`-`、`*` 的结果超出这个范围时，会像除以零（`/`、`%`）一样抛出错误 “integer overflow”：`catch` 可以捕获它；没有地方捕获时，它会交给最近的 `Boundary`（见[错误](errors.md#错误)）。对最小的 `int` 取负（`-x`）、取绝对值（`abs(x)`）、除以 `-1`，以及数组的 `sum()` 超出范围，也都一样。计数、大小或价格一旦溢出，就是一个在出错的地方暴露出来的 bug，而不是一个被悄悄存下来的错误数字。

```asuka
let big = 9223372036854775807
let n = try big + 1 catch { 0 }               // 0：抛出了错误
let next = big.addingChecked(1) ?? big          // 溢出时为 none：得到 big
```

`&+`、`&-` 和 `&*` 则会回绕：结果超过最大的 `int` 时，按 64 位二进制补码的规则从最小值接着算下去（`9223372036854775807 &+ 1` 等于 `-9223372036854775808`）。哈希、校验和以及二进制协议需要的正是这种行为；`&+=`、`&-=` 和 `&*=` 是对应的赋值写法：

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

想把溢出当作一个值来处理时，用 `a.addingChecked(b)`、`subtractingChecked` 和 `multipliedChecked`：它们返回 `int?`，溢出时为 none。移位和按位运算符不会因为结果而抛出错误；对超出范围的浮点数调用 `int(x)`，得到离它最近的 `int`（NaN 得到 `0`）。

## 内置方法

字符串、bytes、数组、集合、map 和可选值都有方法。其中会修改值本身的方法（`push`、`insert`、`removeAt`、`removeWhere`、`pop`、`clear`、`sort`、`remove`）只能用在 `var` 或状态上：

| 类型 | 方法 |
|---|---|
| `[T]` | `count()`、`count(where)`、`isEmpty()`、`first()`、`last()`、`get(i)`（返回 `T?`）、`push(x)`、`insert(x, at: i)`、`removeAt(i)`、`removeWhere(p)`、`pop()`、`clear()`、`map(f)`、`flatMap(f)`（把 `f` 返回的数组依次接起来）、`compactMap(f)`（`f` 的结果中去掉 none）、`filter(p)`、`reduce(initial, f)`、`any(p)`、`all(p)`、`index(p)`（返回 `int?`）、`find(p)`、`contains(x)`、`sum()`、`min()`、`max()`（返回 `T?`；`min(by:)`、`max(by:)` 接受一个“小于”比较）、`unique()`、`grouped(by: f)`（返回 `[K: [T]]`）、`chunked(n)`、`zip(other)`（两两配对，长度取较短的一方）、`reversed()`、`slice(from, to)`（省略 `to` 时到末尾）、`prefix(n)`、`suffix(n)`、`sorted()`、`sorted(by)`、`sort()`、`sort(by)`、`enumerated()`、`joined(separator:)`（用于字符串数组）、`+` |
| `Set<T>` | `count()`、`isEmpty()`、`contains(x)`、`insert(x)`（返回是否新加入）、`remove(x)`（返回原本是否存在）、`union(s)`、`intersection(s)`、`subtracting(s)`、`isSubset(of: s)`、`isSuperset(of: s)`；`Set(xs)` 是数组中互不相同的元素，`Set<T>()` 是空集合 |
| `[K: V]` | `m[k]`（返回 `V?`）、`m[k] = v`、`count()`、`isEmpty()`、`has(k)`、`get(k)`、`remove(k)`、`keys()`、`values()`（按插入顺序）、`mapValues(f)`、`filter((k, v) => …)` |
| `string` | `count()`（字符数）、`byteCount()`、`isEmpty()`、`upper()`、`lower()`、`trim()`、`trimStart()`、`trimEnd()`、`trimPrefix(s)`、`trimSuffix(s)`、`contains(s)`、`hasPrefix(s)`、`hasSuffix(s)`、`replace(old, new, count:)`（默认替换全部，count 指定次数）、`split(sep, count:)`、`fields()`（按空白分出的词）、`lines()`、`find(s)`、`findLast(s)`、`findAny(chars)`（字节偏移，找不到时为 none）、`slice(from, to)`（按字符）、`byteSlice(from, to)`（按字节：即 find 给出的偏移）、`padStart(width, with:)`、`padEnd(width, with:)`、`toInt()`、`toInt(radix:)`、`toFloat()`、`repeat(n)`、`chars()`、`reversed()`、`bytes()`、`+` |
| `bytes` | `b[i]`（返回 `int`，取值 0 到 255；越界是一个错误）、`count()`、`isEmpty()`、`get(i)`（返回 `int?`）、`slice(from, to)`、`concat(b)`、`string()`（按 UTF-8 解码，无效的部分变成 U+FFFD）、`hex()` 和 `hex(separator)`（`"04 a1"`）；`bytes()` 是空的字节序列，`bytes([0x04, 0xa1])` 是这些字节，`s.bytes()` 是字符串的 UTF-8 字节 |
| `int` | `float()`、`abs()`、`toString(radix:)`（`255.toString(radix: 16)` 是 `"ff"`）、`padded(width)`（`7.padded(2)` 是 `"07"`）、`addingChecked(o)`、`subtractingChecked(o)`、`multipliedChecked(o)`（返回 `int?`：溢出时为 none） |
| `float` | `fixed(digits)`、`rounded()`、`floor()`、`ceil()`、`int()`（向零取整）、`abs()` |
| `Color` | `opacity(a)`：以原本不透明度的 a 倍绘制（语义颜色仍然是语义颜色）；`hex()`（`"#0a84ff"`；语义颜色则是它的名字：`".accent"`） |
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

`toInt(radix:)` 按 2 到 36 之间的进制读取整个字符串，字母大小写均可，前面可以带正负号（`"ff".toInt(radix: 16)` 是 255，`"-101".toInt(radix: 2)` 是 -5），读不出来时返回 none；`toString(radix:)` 则把整数按进制写出来，字母用小写。进制不在 2 到 36 之间时会报错。

`min`、`max`、`abs`、`float(n)` 和 `int(x)` 是函数；其余的（`math.sqrt`、`math.pow`、`math.pi` ……）在 `std/math` 里。

`toInt()` 和 `toFloat()` 读的是整个字符串，读不出来就是 none：可选的正负号，接着是数字，前后不能有别的东西（`" 12"` 是 none，先 `trim()`）。`toInt()` 只认整数（`"+7"`、`"-42"`）；`toFloat()` 还认小数和指数（`"2.5"`、`".5"`、`"1e-3"`），以及浮点数打印出来的样子（`"Infinity"`、`"NaN"`）。无论用户的语言是什么，小数点都是 `.`。

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
