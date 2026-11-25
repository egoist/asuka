# 値と型

| 型 | 値 |
|---|---|
| `int` | 64 ビット整数：`42`、`1_000`、`0xff`、`0b1010`、`0o17` |
| `i8`、`i16`、`i32`、`u8`、`u16`、`u32`、`u64` | 8、16、32、64 ビットの整数。符号付き（`i`）と符号なし（`u`）があります：[整数型](#整数型) |
| `float` | 64 ビット浮動小数点数：`3.14`、`1e-3` |
| `bool` | `true`、`false` |
| `string` | UTF-8 のテキスト：`"Hello, {name}"` |
| `char` | 1 つの Unicode コードポイント（スカラー値）：`'a'`、`'\n'`、`'\u{1F600}'` |
| `bytes` | 生のバイト列 |
| `[T]` | 配列：`[1, 2, 3]`、`[]` |
| `[K: V]` | マップ（挿入した順に保持）：`["a": 1]`、`[:]` |
| `Set<T>` | セット（重複しない値の集まり、挿入した順に保持）：セットが求められる場所では `["a", "b"]` |
| `(A, B)` | タプル：`(1, "one")` |
| `T?` | オプショナル：`T` か `none` |
| `Duration` | `300ms`、`5s`、`2min`、`1h`（ほかに `us`、`ns`） |
| `Color` | `#7c3aed`、`#7c3aed80`、セマンティックカラー（`.label`、`.accent` など） |
| `Length` | `100`（ポイント）、`50%`、`.auto` |
| `fn(A) -> B` | 関数とクロージャ |

文字列の波括弧の中には、任意の式を書けます（`"{count} items"`、`"Total: {price * qty}"`）。`\{` は波括弧そのものを表します。`r"…"` はエスケープも埋め込みもない raw 文字列です。`"` を含む raw 文字列は `r#"…"#` と書きます。これは、開始時と同じ数の `#` が続く `"` で終わります（`r#"say "hi""#`、`r##"a "# b"##`）。`"""…"""` は複数行にまたがる文字列で（1 行目が空ならその行は含まれず、閉じ引用符の行のインデントも取り除かれます）、中の `"` や `""` はそのまま書けます。`r"""…"""` は複数行の raw 文字列です。数値、ブール値、文字列以外の値は、テキストとして表示すると次のようになります。型が自分の `description()` を持てばそれ（[宣言](declarations.md#等しさハッシュテキストjson)）、列挙型のケースはその名前、オプショナルは `none` か中の値、エラーはそのメッセージ、色は書いたとおり（`#0a84ff`、`.accent`）、`Duration` はミリ秒（`1500ms`）、それ以外は JSON です。JSON で表せないものも同じように書き出されます。タプルは要素を括弧で囲んだ形になり、各要素は配列の要素と同じように書かれます（`(1,"a","red")`）。キーが文字列でないマップは、各キーをそれ自身の書き方で書きます（`{1:"a"}`）。関数はテキストにならないので、その型になります（`<fn(int) -> int>`）。

補間では、Python や Rust と同じように、コロンの後ろに書き方を指定できます：`"{price:>8.2f}"`。書式は `[[fill]align][sign][#][0][width][,|_][.precision][type]` です。`<`、`>`、`^` は幅の中で左揃え、右揃え、中央揃えにし、その前に埋め文字を書けます（省略すると空白）。`+` は正の数にも符号を付け、`0` は符号の後ろを 0 で埋めます。`,` か `_` は整数（と浮動小数点数の整数部）の桁を区切ります。`.2` は浮動小数点数の小数点以下の桁数で、テキストでは残す最大の文字数です。型は、整数を 16 進、8 進、2 進で書く `x`、`X`、`o`、`b`（`#` を付けると前に `0x`、`0o`、`0b` を書きます）か、数値を固定小数点、指数、パーセントで書く `f`、`e`、`%` です。数値は右揃え、それ以外の値はテキストとして左揃えになり、埋め文字、揃え、幅、精度だけを指定できます。書式はコンパイラーが読むので、値に合わない書式は書いた場所でエラーになります（[format-spec](../errors/format-spec.md)）。

```asuka check
fn row(name: string, count: int, share: float) -> string = "{name:<10}{count:>6,}{share:>8.1%}"

let price = "{1234.5:,.2f}"          // 1,234.50
let hex = "{255:#06x}"               // 0x00ff
let title = "{"Total":*^11}"         // ***Total***
let table = row("asuka", 12500, 0.4) // asuka     12,500   40.0%
```

幅は文字（コードポイント）の数で数え、ターミナルでの表示幅ではありません。ターミナルの桁数で数えるには（漢字・かな・ハングルや絵文字は 2 桁）、`std/term` の `textWidth`、`padEnd`、`fit` を使います（[プログラム](../programs.md#揃った表)を参照）。

`print(a, b, c)` は、値を補間と同じ見た目で、スペースで区切って標準出力（アプリのログ）に 1 行書きます。`eprint` は標準エラーに書きます。Swift の `print` と同じく、値のあとに `separator:`（値のあいだに書くもの）と `terminator:`（最後に書くもの。指定しなければ改行）を渡せます：`print(x, y, separator: ", ")`、`print("Saving…", terminator: "")`。

タプルの要素は位置で読めます（`pair.0`、`pair.1`）。`var` のタプルなら、その場で変更もできます（`pair.0 += 1`）。タプルにない位置（2 要素のタプルの `pair.2`）はコンパイルエラーです。分解して読むこともできます：`let (count, label) = pair`、`for (i, x) in xs.enumerated()`、あるいは `match` のタプルパターン（[制御フロー](control-flow.md#制御フロー)を参照）。

マップのキーとセットの要素は `Hashable` でなければなりません。整数、文字列、ブール値、バイト列、`Duration`、色と、それらからなる列挙型、構造体、タプル、オプショナル、配列、マップ、セットがそうです（自分自身を含む構造体や列挙型も、ほかの部分が `Hashable` なら `Hashable` です。マップはキーと値の両方が `Hashable` である必要があります）。浮動小数点数は NaN が自分自身と等しくないので `Hashable` ではなく、関数も違います。

```asuka
struct Cell {
  row: int
  col: int
}

let marks: [Cell: string] = [Cell(row: 0, col: 2): "x"]
let seen: Set<(int, int)> = [(0, 0), (1, 2)]
```

## 演算子

結合の強いものから順に並べています。同じ行の演算子は優先順位が同じで、左から順に結合します（`**` と `??` は右から）。

| 演算子 | 意味 |
|---|---|
| `**` | べき乗：`2 ** 10` は 1024 |
| `!x`、`-x`、`~x` | 否定、符号反転、整数のビット反転 |
| `<<`、`>>` | 整数のビットを左へ、右へシフト（右シフトは符号付き整数の符号を保つ） |
| `*`、`/`、`%`、`&`、`&*` | 乗算、除算、剰余、ビット AND、ラップアラウンドする乗算 |
| `+`、`-`、<code>&#124;</code>、`^`、`&+`、`&-` | 加算（文字列や配列の連結にも使う）、減算、ビット OR、ビット XOR、ラップアラウンドする加算と減算 |
| `..`、`..<` | 範囲：`1..3`、`0..<n` |
| `==`、`!=`、`<`、`<=`、`>`、`>=` | 比較（連ねて書くことはできません） |
| `&&` | かつ |
| <code>&#124;&#124;</code> | または |
| `??` | オプショナルの中の値。`none` ならデフォルト値 |

`<`、`<=`、`>`、`>=` で順序づけられるのは、整数、浮動小数点数、文字列、`Duration`、値を持たないケースだけの列挙型（ケースを宣言した順）、それに `compare` メソッドを持つ型です。構造体と列挙型は、算術演算子もメソッド `add`、`sub`、`mul`、`div`、`rem`、`pow`、`neg` から得られます（[宣言](declarations.md#構造体)を参照）。

`**` は上の表のほかの演算子より強く結合し、`-` よりも強く結合します。また、右から結合します。Python や数学と同じです。`-2 ** 2` は `-(2 ** 2)` で -4、`2 ** 3 ** 2` は `2 ** 9` で、`(-2) ** 2` は 4 です。整数のべき乗は底と同じ型になり、指数には任意の型の 0 以上の整数を取ります（負の指数はエラーです。`2 ** -1` は整数になりません）。浮動小数点数のべき乗は、指数に浮動小数点数も整数も取れます（`2.0 ** 0.5`、`x ** 2`）。代入には `**=` を使います。

ビット演算子が取るのは整数で、両辺は同じ型でなければなりません。シフト量は任意の型の整数です。型のビット幅以上シフトするとすべてのビットが押し出され（負の数の `>>` は -1 になります）、負の数でシフトするとエラーになります。`>>` は、符号付き整数では符号を保ち、符号なし整数では 0 で埋めます。これらの演算子と `b[i]`（バイトを整数として読む）を使えば、Bluetooth のキャラクタリスティックや NFC のレコードといったバイナリデータを読み書きできます（整数をまるごと、どちらのバイトオーダーでも読み書きするなら [`std/encoding`](../std/encoding.md)）。

```asuka
let raw = b[1] | b[2] << 8                         // リトルエンディアンの uint16
let celsius = if raw & 0x8000 != 0 { raw - 0x10000 } else { raw }
let out = bytes([flags, value & 0xff, value >> 8])
```

`int` が表せるのは −2⁶³ から 2⁶³ − 1 までです。`+`、`-`、`*`、`**` の結果がこの範囲を超えると、ゼロ除算（`/`、`%`）と同じようにエラー “integer overflow” が投げられます。`catch` で受け取れ、どこでも受け取られなければいちばん近い `Boundary` に届きます（[エラー](errors.md#エラー)を参照）。最小の `int` の `-x` と `abs(x)`、`/ -1`、範囲を超える配列の `sum()` も同じです。個数やサイズ、金額がオーバーフローしたら、それは起きたその場で見つかるバグであって、黙って保存される間違った数ではありません。どの整数型も同じように、それぞれの範囲で検査されます。`u8` の `255 + 1` はエラーを投げます。

```asuka
let big = 9223372036854775807
let n = try big + 1 catch { 0 }               // 0：エラーが投げられた
let next = big.addingChecked(1) ?? big          // オーバーフローなら none：big になる
```

`int` の範囲を超える数には、`std/bigint` の `BigInt` を使います。任意の大きさの整数で、算術演算子も比較もそのまま使えます（[標準ライブラリ](../stdlib.md)を参照）。

```asuka check
import std/bigint.{BigInt}

let big = BigInt.of(9223372036854775807)
let past = big * big + BigInt.one           // 85070591730234615847396907784232501250
let digits = "{BigInt.factorial(30)}"       // 265252859812191058636308480000000
```

`&+`、`&-`、`&*` は代わりにラップアラウンドします。最大の `int` を超えると、64 ビットの 2 の補数どおりに最小値の側から続きます（`9223372036854775807 &+ 1` は `-9223372036854775808`）。固定幅の型は、それぞれのビット幅でラップアラウンドします（`u8` の `255 &+ 1` は 0）。ハッシュ、チェックサム、バイナリプロトコルが求めるのはこの動作です。代入には `&+=`、`&-=`、`&*=` を使います。

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

オーバーフローを値として扱いたいときは、`a.addingChecked(b)`、`subtractingChecked`、`multipliedChecked` を使います。結果は `int?`（`u8` なら `u8?`、ほかの型も同様）で、オーバーフローすると none です。シフトとビット演算子が結果のせいでエラーを投げることはありません。範囲外の浮動小数点数に `int(x)` を使うと、いちばん近い `int` になります（NaN は `0`）。

## 整数型

`int` のほかに、ビット幅の決まった整数が、符号付きと符号なしの両方あります。

| 型 | 値 |
|---|---|
| `i8`、`i16`、`i32` | −2⁷ から 2⁷ − 1、−2¹⁵ から 2¹⁵ − 1、−2³¹ から 2³¹ − 1 |
| `int` | −2⁶³ から 2⁶³ − 1（`i64` はありません。`int` がそれにあたります） |
| `u8`、`u16`、`u32`、`u64` | 0 から 2⁸ − 1（255）、2¹⁶ − 1、2³² − 1、2⁶⁴ − 1 |

個数、インデックス、サイズ、セント単位の金額には `int` を使います。ほかの型は、それ自体に決まった幅があるもののためのものです。バイト（`u8`）、ピクセルのチャンネル、ファイル形式やプロトコルのフィールド、ハッシュ、`int` の範囲を超える ID（`u64`）、C の関数の値（`Int32` は `i32`：[Asuka から C を呼び出す](../c.md)）などです。どの型も自分の範囲をそのまま守ります。`int` と同じく範囲を超える算術はエラーを投げ、JSON、CSV、TOML、SQLite からその型に読み込む値も検査されます。

整数リテラルは期待される型になり、その型に収まらなければなりません：`let level: u8 = 200`、`level + 1`、`let big: u64 = 18446744073709551615`。`let x: u8 = 256` や `let y: u8 = -1` は、書いた場所でエラーになります。期待される型がなければ `int` です。

型の違う 2 つの整数を 1 つの演算子で組み合わせることはできません。`int` と `float` を混ぜられないのと同じで、`int` と `u8` も混ぜられません。`level` が `u8`、`offset` が `int` なら `level + offset` はエラーで（[mixed-numbers](../errors/mixed-numbers.md)）、`int` を取るところに `u8` を渡すのもエラーです。どちらにするかは変換で書きます。

```asuka check
fn conversions(n: int, level: u8, ratio: float) -> [u8] {
  let total = int(level) + n        // 拡大：必ず収まる
  let a = u8(n)                     // n が収まらなければエラー（"integer overflow"）
  let b = u8(exactly: n) ?? 0       // u8?：収まらなければ none
  let c = u8(truncating: n)         // 下位 8 ビット：u8(truncating: 300) は 44、-1 なら 255
  let d = u8(clamping: n)           // いちばん近い値：下回れば 0、上回れば 255
  let e = u8(ratio)                 // 浮動小数点数：0 の方向へ切り捨て、範囲外ならいちばん近い値
  return [a, b, c, d, e, u8(clamping: total)]
}
```

`u8.min` と `u8.max` は型の範囲です（`int.max` は 9223372036854775807）。`u64.parse(text)` は、`toInt()` が `int` を読むのと同じようにテキストから値を読み（`radix:` も使えます）、整数でないときや収まらないときは none を返します。`int` の最大値を超える `u64` も、表示、解析、比較、除算、JSON への書き出しで、そのままの数として扱われます（`18446744073709551615`）。

`-x` と `abs(x)` が取るのは符号付き整数です。符号なし整数には負の数がありません（`0 &- x` はラップアラウンドします）。整数型には `int` のメソッド（`toString(radix:)`、`padded(width)`、`float()`、`abs()`、チェック付きのメソッド）があり、書式指定（`"{id:x}"`）が使えます。数として順序づけとハッシュができ（マップやセットのキー、ジェネリクスでの `Comparable` と `Hashable`）、範囲も作れます：`for i in u16(0)..<count`。

`bytes` から読んだバイトは `int` です（`b[i]`、0 から 255）。そのため、それに対する算術は 8 ビットでラップアラウンドしません。`b.toArray()` はバイト列を `[u8]` として返し、`bytes(xs)` は `[int]` からと同じように `[u8]` からもバイト列を作ります。

## 組み込みメソッド

文字列、バイト列、配列、セット、マップ、オプショナルにはメソッドがあります。値そのものを変更するメソッド（`push`、`pushFirst`、`insert`、`removeAt`、`removeWhere`、`pop`、`popFirst`、`clear`、`sort`、`remove`）は、`var` か状態に対してだけ呼べます。

| 型 | メソッド |
|---|---|
| `[T]` | `count()`、`count(where)`、`isEmpty()`、`first()`、`last()`、`get(i)`（`T?` を返す）、`push(x)`、`pushFirst(x)`、`insert(x, at: i)`、`removeAt(i)`、`removeWhere(p)`、`pop()`、`popFirst()`、`clear()`、`map(f)`、`flatMap(f)`（`f` が返す配列を順につなげる）、`compactMap(f)`（`f` の結果から none を除く）、`filter(p)`、`reduce(initial, f)`、`any(p)`、`all(p)`、`index(p)`（`int?` を返す）、`find(p)`、`contains(x)`、`sum()`、`min()`、`max()`（`T?` を返す。`min(by:)`、`max(by:)` は「より小さい」の比較を、`min(key:)`、`max(key:)` はキーを取る）、`unique()`、`grouped(by: f)`（`[K: [T]]` を返す）、`chunked(n)`、`zip(other)`（短いほうの長さまでペアにする）、`reversed()`、`slice(from, to)`（`to` を省くと末尾まで）、`prefix(n)`、`suffix(n)`、`sorted()`、`sorted(by)`、`sorted(key:, descending:)`、`sort()`、`sort(by)`、`sort(key:, descending:)`、`enumerated()`、`joined(separator:)`（文字列の配列）、`lazy()`（`Seq<T>` を返す）、`+` |
| `Set<T>` | `count()`、`isEmpty()`、`contains(x)`、`insert(x)`（新しく加わったかを返す）、`remove(x)`（あったかを返す）、`union(s)`、`intersection(s)`、`subtracting(s)`、`isSubset(of: s)`、`isSuperset(of: s)`、`lazy()`。`Set(xs)` は配列の重複しない要素、`Set<T>()` は空のセット |
| `[K: V]` | `m[k]`（`V?` を返す）、`m[k] = v`、`count()`、`isEmpty()`、`has(k)`、`get(k)`、`remove(k)`、`keys()`、`values()`（挿入した順）、`mapValues(f)`、`filter((k, v) => …)`、`lazy()`（`(key, value)` の組） |
| `string` | `count()`（文字数）、`byteCount()`、`isEmpty()`、`upper()`、`lower()`、`trim()`、`trimStart()`、`trimEnd()`、`trimPrefix(s)`、`trimSuffix(s)`、`contains(s, ignoreCase:)`、`hasPrefix(s, ignoreCase:)`、`hasSuffix(s, ignoreCase:)`、`equals(s, ignoreCase:)`、`compare(s, ignoreCase:)`（負、ゼロ、正のいずれか）、`foldCase()`、`replace(old, new, count:)`（count を指定しなければすべて置き換え）、`split(sep, count:)`、`fields()`（空白で区切った単語）、`lines()`、`find(s)`、`findLast(s)`、`findAny(chars)`（バイト単位の位置。見つからなければ none）、`slice(from, to)`（文字単位。`to` を省くと末尾まで）、`byteSlice(from, to)`（バイト単位：find が返す位置。`to` を省くと末尾まで）、`padStart(width, with:)`、`padEnd(width, with:)`、`toInt()`、`toInt(radix:)`、`toFloat()`、`repeat(n)`、`chars()`、`graphemes()`、`lazy()`（char の `Seq<char>`）、`reversed()`、`bytes()`、`+` |
| `bytes` | `b[i]`（0 から 255 の `int`。範囲外はエラー）、`count()`、`isEmpty()`、`get(i)`（`int?` を返す）、`slice(from, to)`（`to` を省くと末尾まで）、`concat(b)`、`string()`（UTF-8 として読み、不正な部分は U+FFFD になる）、`hex()` と `hex(separator)`（`"04 a1"`）、`toArray()`（`[u8]` を返す）。`bytes()` は空のバイト列、`bytes([0x04, 0xa1])` はそのバイト列、`bytes(xs)` は `[u8]` のバイト列、`s.bytes()` は文字列の UTF-8 |
| `int`、`i8` … `u64` | `float()`、`abs()`（符号付きの型）、`toString(radix:)`（`255.toString(radix: 16)` は `"ff"`）、`padded(width)`（`7.padded(2)` は `"07"`）、`addingChecked(o)`、`subtractingChecked(o)`、`multipliedChecked(o)`（`int?`、`u8` なら `u8?`：オーバーフローなら none）。`T.min`、`T.max`、`T.parse(text, radix:)`（`T?` を返す） |
| `float` | `fixed(digits)`、`rounded()`、`floor()`、`ceil()`、`int()`（0 の方向へ切り捨て）、`abs()` |
| `Color` | `Color.parse(text)`（`#rrggbb`、`#rgb`、`#rrggbbaa`：`Color?` を返す）、`Color.rgb(r, g, b, alpha:)`。`opacity(a)`：元の不透明度の a 倍で描く（セマンティックカラーはセマンティックカラーのまま）。`hex()`（`"#0a84ff"`。セマンティックカラーはその名前：`".accent"`） |
| `Duration` | `nanoseconds()`、`milliseconds()`（整数）、`seconds()`、`minutes()`、`hours()`（浮動小数点数）、`rounded(to:)`、`truncated(to:)`、`+`、`-`、`* n`（整数でも浮動小数点数でも：`1.5 * 1h`）、`/ n`、`/ d`（別の Duration：何個分か。浮動小数点数） |
| `Length` | `* n`（`width: 50% * 2`：長さを拡大縮小） |
| `T?` | `isNone()`、`isSome()` |

渡した関数を呼び出すメソッド（`map`、`filter`、`reduce`、`sorted(by:)`、`grouped(by:)`、マップの `mapValues` など）は、エラーを投げる関数も受け取れます。その場合、関数が投げたエラーをそのまま投げるので、呼び出しには `try` が必要です。エラーを投げない関数を渡したときは、メソッドも投げません。

```asuka
fn port(text: string) throws -> int = text.toInt() ?? throw Error("not a port: {text}")

fn ports(lines: [string]) throws -> [int] = try lines.map(l => try port(l))
```

いくつかを組み合わせると、次のようになります。

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
  let pages = sales.chunked(50)                               // [[Sale]]、50 件ずつ
  var out = ["{best?.region ?? "none"} sold the most"]
  for (region, total) in totals { out.push("{region}: {total}") }
  for (i, page) in pages.enumerated() { out.push("page {i + 1}: {page.count()}") }
  out.push("{codes.reduce(0, (sum, c) => sum + c)}")
  return out
}
```

`unique()` は各要素を最初に現れた位置に一度だけ残し、`grouped(by:)` もキーを最初に現れた順に並べます。どちらも値（`grouped` ではキー）が `Hashable` である必要があります。`min()` と `max()` は `sorted()` と同じ順序で比べ、等しい要素があれば最初のものを返します。`chunked(n)` の `n` が 1 未満だと、範囲外の `xs[i]` と同じくエラーになります。

`sorted(key:)` は、関数が各要素から求めるキーの順に並べます。キーは `Comparable` で、要素ごとに一度だけ求められます（`people.sorted(key: p => p.age)`）。`descending: true` を付けると大きいものから並び、どちらの向きでも安定ソートなので、キーが等しい要素は元の順序を保ちます。`sort(key:)` は `var` をその場で並べ替え、`min(key:)` と `max(key:)` はキーが最小、最大の要素を返します（等しければ最初のもの）。一つのキーでは決まらない順序には、「より小さい」の比較を取る `sorted(by:)` を使います。

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

配列は両端キューでもあります。`pushFirst(x)` と `popFirst()` は、`push(x)` と `pop()` が末尾を変えるのと同じように先頭を変え、どれも O(1)（償却。先頭を一度変えた配列は、先頭に余白を持ちます）です。`insert(x, at: 0)` と `removeAt(0)` も同じです。それ以外の位置での挿入や削除では、短い側の要素だけが動きます。つまりキューは配列そのもので、`queue.push(job)` で入れて `queue.popFirst()` で取り出します。

`toInt(radix:)` は文字列全体を 2 から 36 までの基数で読みます。英字は大文字でも小文字でもよく、先頭に符号を付けられます（`"ff".toInt(radix: 16)` は 255、`"-101".toInt(radix: 2)` は -5）。読めなければ none です。逆に `toString(radix:)` は整数をその基数で、英字は小文字で書き出します。基数が 2 から 36 の範囲外ならエラーです。

`min`、`max`、`abs`、`float(n)`、`int(x)`、それにほかの整数型への変換（`u8(x)`）は関数です。それ以外（`math.sqrt`、`math.pow`、`math.pi` など）は `std/math` にあります。

`upper()` と `lower()` は Unicode の規則で文字を変換し、どの言語でも結果は同じです。`"straße".upper()` は `"STRASSE"` になり、単語の末尾の `Σ` は小文字で `ς` になります。`ignoreCase: true` は二つの文字列のケースフォールディング（`foldCase()`。`"Straße"` も `"STRASSE"` も `"strasse"` になります）を比べます。Go の `strings.EqualFold` や Python の `casefold` と同じ考え方です。一致は文字の切れ目で始まって切れ目で終わる必要があるため、`"ß"` は `"s"` で始まるとはみなされません。どれも特定の言語の規則には従いません。トルコ語の点のない ı や点付きの İ もほかと同じように変換され（`"İ".lower()` は `i` と結合用の点）、文字列の正規化もしません（`"é"` と、`e` に結合アクセントを付けたものは別物です）。大文字小文字を区別せずに名前を並べ替えるには、`compare` を `sorted(by:)` に渡します。`foldCase()` は大文字小文字を無視するキーになります。

```asuka check
fn byName(names: [string]) -> [string] = names.sorted(by: (a, b) => a.compare(b, ignoreCase: true) < 0)

fn sameUser(a: string, b: string) -> bool = a.trim().equals(b.trim(), ignoreCase: true)

fn isMarkdown(path: string) -> bool = path.hasSuffix(".md", ignoreCase: true)
```

`count()` と `chars()` はコードポイントを数えます。`graphemes()` は読み手が一文字と感じる単位を返します。Unicode のテキスト分割（UAX #29）に従うので、`"e\u{301}"`、国旗（`"🇯🇵"`）、ZWJ でつないだ家族の絵文字はそれぞれ一つです。

`char` は 1 つのコードポイントで、Rust の char と同じです。`for c in s` がたどり、`s.chars()` が順に返すのがこれです（`e` とアクセントのように複数のコードポイントからなる書記素は文字列で、`graphemes()` が返します）。単一引用符で書き（`'a'`、`'\''`、`'\u{301}'`）、コードポイントで比較・順序づけされ、マップのキーにもなります。出力するとそれ自身になり、JSON では 1 文字の文字列です。`c.isLetter()`、`isDigit()`、`isNumber()`、`isWhitespace()`、`isUpper()`、`isLower()`、`isPunctuation()`、`isSymbol()` は Unicode のカテゴリーを読み、どの文字体系にも使えます（`'٣'.isDigit()`）。`upper()` と `lower()` は結果が 1 つの char になるときに変換します（`'ß'.upper()` は `'ß'` のままで、`"ß".upper()` は `"SS"`）。`code()` はその番号で、`char(n)` は番号 n の char です（n がコードポイントでなければエラー）。`s + c` と `s += c` で文字列に char を足せます。

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


`toInt()` と `toFloat()` は文字列全体を読み、読めなければ none を返します。符号は付けても付けなくてもよく、その後に数字が続き、前後に余計なものがあってはいけません（`" 12"` は none なので、先に `trim()` します）。`toInt()` が読むのは整数（`"+7"`、`"-42"`）だけで、`toFloat()` は小数部と指数（`"2.5"`、`".5"`、`"1e-3"`）、それに浮動小数点数を表示したときの形（`"Infinity"`、`"NaN"`）も読みます。どちらも、ユーザーの言語にかかわらず小数点は `.` です。

## 遅延シーケンス

`xs.lazy()` は配列の要素を `Seq<T>`、つまり遅延シーケンスにします。その `map`、`filter`、`take` などは別の `Seq` を作るだけで、何も計算しません。計算するのは、それを読むもの（`for`、`toArray()`、`first()`、`reduce` など）です。要素は一つずつ、すべての段階をまとめて通って求められ、途中で配列は作られません。そのため、百万個の要素に処理をつなげて最後に三つだけ残すなら、計算されるのはその三つと、それを見つけるまでに必要だった要素だけです。終わりのないシーケンスも作れます。

```asuka check
fn squaresUnder(limit: int) -> [int] = Seq(from: 1).map(n => n * n).takeWhile(n => n < limit).toArray()

fn firstLong(lines: [string]) -> string? = lines.lazy().map(l => l.trim()).find(where: l => l.count() > 80)

fn fibonacci() -> Seq<int> = Seq(state: (0, 1), next: s => (s.0, (s.1, s.0 + s.1)))
```

マップの `lazy()` は `(key, value)` の組を、セットのものは要素を、文字列のものは文字を、範囲のもの（`(0..<n).lazy()`）は数を、配列を作らずに返します。`Seq(…)` は関数からシーケンスを作ります。

| 作り方 | 要素 |
|---|---|
| `Seq(from: n, by: step)` | `n`、`n + step`、……と終わりなく続きます。`by` を省くと 1 です |
| `Seq(first: x, next: f)` | `x`、`f(x)`、`f(f(x))`、……と、`f` が none を返すまで続きます |
| `Seq(state: s, next: f)` | `f(s)` が次の要素と次の状態の組 `(x, s2)` を返し、終わりでは none を返します |
| `Seq(repeating: x, count: n)` | `x` を `n` 回。`count` を省くと終わりなく続きます |

| | メソッド |
|---|---|
| 新しい `Seq` を返す | `map(f)`、`filter(p)`、`compactMap(f)`、`flatMap(f)`（`f` が返す `Seq` の要素を順につなげる）、`enumerated()`、`zip(other)`、`take(n)`、`skip(n)`、`takeWhile(p)`、`skipWhile(p)`、`chunked(n)`、`windows(n)`（隣り合う `n` 個ずつ。`[1, 2, 3]` の `windows(2)` は `[1, 2]`、`[2, 3]`）、`onEach(f)`（要素が読まれるたびに `f` を呼ぶ）、`unique()`、`concat(other)` |
| 読む | `toArray()`、`toSet()`、`toMap()`（`(key, value)` の組から）、`count()`、`count(where:)`、`first()`、`last()`、`find(where:)`、`any(where:)`、`all(where:)`、`contains(x)`、`reduce(initial, f)`、`sum()`、`min()`、`max()`、`min(by:)`、`max(by:)`、`min(key:)`、`max(key:)`、`joined(separator:)` |

`take`、`find`、`first`、`any`、`all` は答えが出た時点で要素を取り出すのをやめ、その先の要素は一切計算しません。`Seq` は読むたびに先頭から計算されます（`for` で二回回せば二回計算します）。ほかの値と同じく、作った時点のデータ元の値を保持するので、あとで配列を変更しても `Seq` は変わりません。

段階の関数はシーケンスが読まれるときに実行されるので、エラーを投げられません。投げる必要があるときは `for` の中で行うか、配列の `map` を使います。読むときにその場で呼ばれる関数（`reduce`、`find(where:)`、`any`、`count(where:)`、`min(key:)` など）は投げてもよく、その場合は配列のメソッドと同じく、呼び出しがそのエラーを投げます。

優先度付きキューと、キーの順に保つマップとセットは、`std/collections` の `Heap`、`SortedMap`、`SortedSet` です（[標準ライブラリ](../stdlib.md)を参照）。

## 値のセマンティクス

文字列、配列、セット、マップ、構造体、列挙型は**値**です。代入したり引数に渡したりすると、見かけ上はコピーが渡されます。コピーのコストはわずかです。コピーどうしは、どれかが変更されるまで同じメモリを共有します。また、一か所からしか持たれていない値は、その場で変更されます。

```asuka
var a = [1, 2, 3]
var b = a
b.push(4)        // a は [1, 2, 3] のまま
```

ストア、クロージャ、タスク、ビューは**参照**です。コピーしても同じものを指します。

null はありません。ないかもしれない値はオプショナルで表します。

```asuka
let user: User? = none
if let u = user { Text(u.name) }
let name = user?.name ?? "Guest"
let first = items.first()!        // none ならクラッシュする。none になりえないときだけ書く
```

`?.` のあとは、チェーンの残りの部分も none で止まります。`user?.boss.name` は `string?` です。

オプショナルは入れ子になりません。値はあるかないかのどちらかです。`int??` と書くとエラーになり（`?` は一つで足ります）、ジェネリックな `T?` の `T` がすでにオプショナルなら、`T?` はその `T` です。
