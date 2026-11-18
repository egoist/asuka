# 声明

```asuka
let pi = 3.14159             // 常量
var total = 0                // 变量（在函数和处理函数中）
state count = 0              // 状态：界面会跟随变化的值
state shown = count          // 在被写入之前跟随 count：跟随的状态
```

类型会自动推断；在有助于阅读的地方可以写出来：`let ids: [int] = []`。

`type` 在文件顶层给类型起一个名字：`type Pair<A, B> = (A, B)`、`type Check = fn(string) -> bool`。它只是同一个类型的别名：`Pair<int, string>` 就是 `(int, string)`。

## 结构体

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

创建结构体的方式是调用它，按名字或按顺序传入各个字段；有默认值的字段可以省略。结构体的方法直接用名字访问它的字段。`fn` 只读取这个值；`mut fn` 则可以修改它：给字段（以及字段的各个部分）赋值、调用它的其他 `mut fn`，或者给 `self` 赋一个新值。

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

`mut fn` 只能在可以改变的东西上调用：`var`、状态（组件的、store 的、应用的），或者它们的字段、元素、映射的条目（`carts[0].add(i)`、`order.cart.clear()`）。不能在 `let`、参数、prop 或调用的返回值上调用，编译器会指出来。在状态上调用时，不管改了多少个字段，这次调用都只算对该状态的一次修改：只有读取 `todos[i]` 的地方会更新，而不是每一行都更新。`mut fn` 不能是 `async` 的（如果一个值在 `await` 前后只改了一半，等待期间运行的每个处理函数都会看到它；异步的修改应该写成 store 的 `fn`）。它可以抛出错误：抛出之前做的修改会保留。在 `mut fn` 里创建的闭包拿到的是 `self` 当时的值，并且不能修改它。

结构体主体里的 `let` 是这个类型的常量（`Font.title`），在任何需要 `Font` 的地方都可以简写成 `.title`。

结构体或枚举主体里的 `static fn` 是这个类型自己的函数，没有 `self`：在类型上调用，和普通函数一样可以用具名参数和默认值（`Money.of(dollars: 2)`），在任何需要这个类型的地方也可以写成 `.of(…)`。

```asuka check
struct Money {
  cents: int

  static fn of(dollars: int, cents: int = 0) -> Money = Money(cents: dollars * 100 + cents)
}

let price: Money = .of(dollars: 4, cents: 50)
```

结构体可以带类型参数，创建时会自动推断；也可以像泛型函数那样，把类型参数写在名字后面（store 也一样）：

```asuka check
struct Page<T> {
  items: [T]
  next: string? = none
}

let page = Page(items: [1, 2, 3])     // 一个 Page<int>
let empty = Page<string>(items: [])   // 没有别的信息可推断时，直接写出来
```

在 `match` 和 `if let` 里，结构体模式按字段名匹配结构体的字段：单写一个 `y` 会绑定字段 `y`，`y: py` 把它绑定为 `py`，`x: 0` 匹配一个值，没有写出的字段匹配任何值。模式可以嵌套：

```asuka
match line {
  Line(from: Point(x: 0, y: 0), dashed: true) => "dashed, from the origin"
  Line(from: Point(x: 0, y), to) => "from {y} on the y axis to {to.x}"
  Line(dashed) => "a line"
}
```

结构体和枚举可以从它们的方法获得运算符：`fn add(o: T) -> R` 就是 `a + b`，`fn sub(o: T) -> R`（或 `minus`）就是 `a - b`，`fn mul(o: T) -> R`（或 `times`）就是 `a * b`，`fn div(o: T) -> R`（或 `quo`）就是 `a / b`，`fn rem(o: T) -> R` 就是 `a % b`，`fn neg() -> R` 就是 `-a`，`fn compare(o: T) -> int`（负数、零或正数）就是 `<`、`<=`、`>` 和 `>=`，也让 `sorted()` 和 `T: Comparable` 可用（见[函数](functions.md#接口与泛型)）。`T` 可以和值本身的类型不同（`Money * int`）；一个运算符有两个方法名时，两个方法各接受一种 `T`：`Decimal` 的 `mul(o: Decimal)` 和 `times(n: int)` 分别对应 `price * rate` 和 `price * 3`。反过来的顺序（`3 * price`）不会自动推出。只要方法返回的还是 `a` 的类型，`a += b` 就是借助这些方法的 `a = a + b`（`-=`、`*=`、`/=`、`%=` 也一样），可以用在 var、状态、字段和元素上（状态只改变一次）。`std/time` 的 `Time` 就是这样加上一个 `Duration`、减去另一个 `Time` 的：

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

结构体不会逐个字段地排序：它的顺序是什么意思，由它的 `compare` 来决定。

## 扩展

`extend` 可以给一个类型添加方法、`mut fn`、`static fn` 和常量。这个类型可以是内置类型（`string`、`int`、`[T]`、`[K: V]`、`Set<T>`、`Color`、`Duration`），也可以是本包或其他包里的类型。

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

扩展的方法看待这个值的方式和类型自己的方法一样：可以访问它的字段，也可以直接用名字调用它的方法（`lower()` 就是 `self.lower()`）。扩展里定义的、属于该类型的常量，在任何需要这个类型的地方都可以写成 `.name`，和类型自己的常量一样，也可以写成 `Color.brand`；`static fn` 也是如此。扩展不能添加字段，也不能添加状态；它添加的名字也不能和类型自己的成员重名：类型的布局和成员始终由它的声明决定。泛型扩展把类型参数写在 `extend` 后面（`extend<K: Hashable, V> [K: V]`）。

扩展添加的东西只在它所在的包里可见，其中 `pub` 的成员在导入这个包的包里也可见，除此之外都不可见：依赖不会改变你代码里 `x.trim()` 的含义。如果你能看到的两个扩展给同一个类型添加了同名的成员，使用它的地方就会报错，并指出这两个包。

类型也可以通过扩展获得接口，前提是这些扩展在当前位置可见：`extend Money { fn area() -> float … }` 让 `Money` 成为一个 `Shape`。写成 `extend Money: Shape { … }` 就是明确声明这一点，编译器会像检查 `struct Money: Shape` 一样检查它；其他包里没有携带值的枚举，也是通过这种方式获得接口的。一个类型在整个程序里同名的方法只能有一个：如果两个包的扩展给同一个类型添加了同一个方法，而且都让它成为某个接口的实现，就会报错。

## 枚举

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

枚举的 `mut fn` 同样可以修改这个值，通常是给 `self` 赋值：

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

在类型已知的地方，枚举的成员写作 `.name`。`match` 必须覆盖所有成员（或者以 `_ =>` 结尾）；模式可以绑定成员携带的值（`.from(p)`，也可以按标签绑定：`.server(code: c)`），可以匹配字面量（`0`、`"x"`），可以同时匹配多个成员（`.a | .b`），可以嵌套（`.from(Person(vip: true))`），还可以带守卫条件（`.from(p) if p.vip =>`）。编译器检查 `match` 是否覆盖了所有情况时会深入嵌套的模式，并把缺少的情况写成模式告诉你：`match doesn't cover .key(_, false)`。成员带标签的值如果有零值（`false`、`0`、`""`、空数组或空映射、`none`、枚举的第一个成员），构造时可以省略：`.button(title: "Copy", run: copy)` 就省略了 `Action.button` 的 `icon`、`role`、`disabled` 和 `checked`。

成员携带的值可以是这个枚举本身，因此枚举可以表示一棵树：

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
