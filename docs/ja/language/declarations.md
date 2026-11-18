# 宣言

```asuka
let pi = 3.14159             // 定数
var total = 0                // 変数（関数とハンドラーの中で）
state count = 0              // 状態：UI が追従する値
state shown = count          // 書き込まれるまで count に追従する状態
```

型は推論されます。書いたほうがわかりやすいところでは明示します（`let ids: [int] = []`）。

`type` は、ファイルのトップレベルで型に名前を付けます（`type Pair<A, B> = (A, B)`、`type Check = fn(string) -> bool`）。これは同じ型の別名（エイリアス）なので、`Pair<int, string>` は `(int, string)` そのものです。

## 構造体

```asuka check
struct Message {
  id: string
  subject: string
  read: bool = false

  fn preview() -> string = subject.slice(0, 40)
}

let m = Message(id: "1", subject: "Hello")
let n = Message(id: m.id, subject: m.subject, read: true)
```

構造体は、フィールドを名前付きか順番どおりに渡して呼び出すことで作ります。デフォルト値のあるフィールドは省略できます。メソッドからはフィールドを名前でそのまま参照できます。`fn` は値を読むだけですが、`mut fn` は値を変更できます。フィールド（とその一部）に代入したり、ほかの `mut fn` を呼んだり、`self` に新しい値を代入したりできます。

```asuka check
struct Item {
  id: int
  qty: int = 1
}

struct Cart {
  items: [Item] = []

  mut fn add(i: Item) {
    if let n = items.index(x => x.id == i.id) { items[n].qty += 1 } else { items.push(i) }
  }
  mut fn clear() { self = Cart() }
  fn total() -> int {
    var sum = 0
    for i in items { sum += i.qty }
    return sum
  }
}

fn twoApples() -> int {
  var cart = Cart()
  cart.add(Item(id: 1))
  cart.add(Item(id: 1))
  return cart.total()
}
```

`mut fn` は、変更できるものに対して呼び出します。`var`、状態（コンポーネント、store、アプリのもの）、またはそれらのフィールド、要素、マップのエントリー（`carts[0].add(i)`、`order.cart.clear()`）です。`let`、引数、prop、呼び出しの戻り値に対しては呼び出せず、コンパイラーがそう指摘します。状態に対して呼び出した場合、フィールドをいくつ変更しても、その状態の一回の変更として扱われます。更新されるのは `todos[i]` を読む部分だけで、すべての行ではありません。`mut fn` は `async` にできません（`await` をまたいで途中まで変更された値が、そのあいだに動くすべてのハンドラーから見えてしまうためです。非同期の変更は store の `fn` にします）。エラーを投げることはでき、その場合も投げる前に加えた変更は残ります。`mut fn` の中で作ったクロージャーは、作った時点の `self` の値を持ち、それを変更することはできません。

構造体の本体に書いた `let` はその型の定数（`Font.title`）になり、`Font` が求められる場所ならどこでも `.title` と書けます。

構造体や列挙型の本体に書いた `static fn` は、`self` を持たない、その型の関数です。ほかの関数と同じく名前付き引数とデフォルト値を使って型に対して呼び出せ（`Money.of(dollars: 2)`）、その型が求められる場所では `.of(…)` とも書けます。

```asuka check
struct Money {
  cents: int

  static fn of(dollars: int, cents: int = 0) -> Money = Money(cents: dollars * 100 + cents)
}

let price: Money = .of(dollars: 4, cents: 50)
```

構造体は型パラメーターを取れます。型引数は作るときに推論されますが、ジェネリック関数と同じように名前の後に書くこともできます（ストアも同様です）。

```asuka check
struct Page<T> {
  items: [T]
  next: string? = none
}

let page = Page(items: [1, 2, 3])     // Page<int>
let empty = Page<string>(items: [])   // ほかに手がかりがないときは書き出す
```

`match` と `if let` では、構造体パターンで構造体のフィールドを名前でマッチさせられます。`y` とだけ書くとフィールド `y` を束縛し、`y: py` なら `py` という名前で束縛し、`x: 0` は値にマッチします。書かなかったフィールドは何にでもマッチします。パターンは入れ子にできます。

```asuka
match line {
  Line(from: Point(x: 0, y: 0), dashed: true) => "dashed, from the origin"
  Line(from: Point(x: 0, y), to) => "from {y} on the y axis to {to.x}"
  Line(dashed) => "a line"
}
```

構造体と列挙型は、メソッドから演算子を得ます。`fn add(o: T) -> R` は `a + b` に、`fn sub(o: T) -> R`（または `minus`）は `a - b` に、`fn mul(o: T) -> R`（または `times`）は `a * b` に、`fn div(o: T) -> R`（または `quo`）は `a / b` に、`fn rem(o: T) -> R` は `a % b` に、`fn neg() -> R` は `-a` に、`fn compare(o: T) -> int`（負、ゼロ、正）は `<`、`<=`、`>`、`>=` になり、`sorted()` や `T: Comparable` にも使われます（[関数](functions.md#インターフェースとジェネリクス)を参照）。`T` は値そのものと違う型でもかまいません（`Money * int`）。名前が二つある演算子では、それぞれのメソッドが別の `T` を受け取ります。`Decimal` の `mul(o: Decimal)` と `times(n: int)` が、`price * rate` と `price * 3` になります。逆の順序（`3 * price`）は導かれません。メソッドが `a` と同じ型を返すなら、`a += b` はこれらのメソッドによる `a = a + b` で（`-=`、`*=`、`/=`、`%=` も同じ）、変数、状態、フィールド、要素に使えます（状態の変更は一度だけです）。`std/time` の `Time` が `Duration` を足し、別の `Time` を引けるのはこのためです。

```asuka check
struct Money {
  cents: int

  fn add(o: Money) -> Money = Money(cents: cents + o.cents)
  fn times(n: int) -> Money = Money(cents: cents * n)
  fn neg() -> Money = Money(cents: -cents)
  fn compare(o: Money) -> int = cents - o.cents
}

let total = Money(cents: 250) + Money(cents: 99) * 2
let over = total > Money(cents: 300)
let refund = -total

fn withTip(bill: Money) -> Money {
  var paid = bill
  paid += Money(cents: 100)
  return paid
}
```

構造体がフィールドごとに順序づけられることはありません。その順序が何を意味するかは、`compare` が決めます。

## 拡張

`extend` は、型にメソッド、`mut fn`、`static fn`、定数を追加します。対象は組み込みの型（`string`、`int`、`[T]`、`[K: V]`、`Set<T>`、`Color`、`Duration`）でも、同じパッケージの型でも、ほかのパッケージの型でもかまいません。

```asuka check
extend string {
  fn slug() -> string = lower().fields().joined(separator: "-")
}

extend Color {
  let brand = #7c3aed
  let brandSoft = #7c3aed.opacity(0.15)
}

extend<T: Comparable> [T] {
  fn median() -> T? = if isEmpty() { none } else { sorted()[count() / 2] }
}

component Title {
  prop text: string
  Text(text.slug()) { color: .brand }
}
```

拡張のメソッドからは、その型自身のメソッドと同じように値が見えます。フィールドを参照でき、メソッドも名前だけで呼び出せます（`lower()` は `self.lower()` のことです）。その型の定数は、型自身の定数と同じく、その型が求められる場所ならどこでも `.name` と書け、`Color.brand` とも書けます。`static fn` も同様です。拡張はフィールドも状態も追加できず、追加する名前を型自身のメンバーと同じにすることもできません。型のレイアウトとメンバーは、あくまで型の宣言が決めます。ジェネリックな拡張は、型パラメーターを `extend` の後に書きます（`extend<K: Hashable, V> [K: V]`）。

拡張が追加したものは、そのパッケージの中で見え、`pub` なメンバーはそのパッケージをインポートしたパッケージからも見えます。それ以外からは見えません。そのため、依存先のパッケージがあなたのコードの `x.trim()` の意味を変えてしまうことはありません。見えている二つの拡張が同じ型に同じ名前を追加している場合は、それを使った場所でエラーになり、両方のパッケージが示されます。

型は、見えている拡張が与えたものによってインターフェースを持つこともできます。`extend Money { fn area() -> float … }` によって `Money` は `Shape` になります。`extend Money: Shape { … }` と書けばそれを明示でき、コンパイラーは `struct Money: Shape` と同じように確認します。ほかのパッケージのペイロードのない列挙型にインターフェースを持たせるには、この書き方を使います。一つのプログラムの中で、型が持てる同じ名前のメソッドは一つだけです。二つのパッケージの拡張が同じ型に同じメソッドを与え、どちらもそれをインターフェースの実装にしている場合はエラーになります。

## 列挙型

```asuka
enum Filter {
  all
  unread
  from(Person)

  fn matches(m: Message) -> bool = match self {
    .all => true
    .unread => !m.read
    .from(p) => m.from == p
  }
}

let f: Filter = .unread
```

列挙型の `mut fn` も値を変更できます。多くの場合は `self` に代入します。

```asuka check
enum Light {
  red
  green
  yellow

  mut fn next() {
    self = match self {
      .red => .green
      .green => .yellow
      .yellow => .red
    }
  }
}
```

型がわかっている場所では、ケースを `.name` と書きます。`match` はすべてのケースを網羅しなければなりません（または `_ =>` で終えます）。パターンでは、ケースの値を束縛したり（`.from(p)`。ラベルでも書けます：`.server(code: c)`）、リテラルにマッチさせたり（`0`、`"x"`）、複数のケースをまとめたり（`.a | .b`）、入れ子にしたり（`.from(Person(vip: true))`）、ガードを付けたり（`.from(p) if p.vip =>`）できます。`match` がすべての値を網羅しているかを調べるとき、コンパイラーは入れ子のパターンの中までたどり、足りないものをパターンの形で示します（`match doesn't cover .key(_, false)`）。ケースのラベル付きの値のうち、ゼロ値（`false`、`0`、`""`、空の配列やマップ、`none`、列挙型の最初のケース）があるものは、作るときに省略できます。`.button(title: "Copy", run: copy)` は `Action.button` の `icon`、`role`、`disabled`、`checked` を省略しています。

ケースの値には、その列挙型自身も持たせられます。そのため、列挙型で木構造を表せます。

```asuka check
enum Expr {
  num(int)
  add(Expr, Expr)

  fn eval() -> int = match self {
    .num(n) => n
    .add(a, b) => a.eval() + b.eval()
  }
}
```
