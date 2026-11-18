# Declarations

```asuka
let pi = 3.14159             // a constant
var total = 0                // a variable (in functions and handlers)
state count = 0              // state: a value the UI follows
state shown = count          // count, until it's written: state that follows
```

Types are inferred; write one where it helps: `let ids: [int] = []`.

`type` names a type, at the top of a file: `type Pair<A, B> = (A, B)`,
`type Check = fn(string) -> bool`. It's another name for the same type
(an alias): a `Pair<int, string>` is an `(int, string)`.

## Structs

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

A struct is made by calling it with its fields, by name or in order;
fields with a default may be left out. Its methods see its fields by
their names. A `fn` reads the value; a `mut fn` may change it: assign
its fields (and their parts), call its other `mut fn`s, or assign
`self` a new value.

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

A `mut fn` is called on what can change: a `var`, state (a component's,
a store's, the app's), or a field, an element or a map's entry of one
(`carts[0].add(i)`, `order.cart.clear()`). Not on a `let`, a parameter,
a prop or what a call returns: the compiler says so. On state, the call
is one change of that state, however many fields it changes: what reads
`todos[i]` updates, not every row. A `mut fn` can't be `async` (a value
half changed across an `await` would be seen by every handler that runs
meanwhile: an async change is a store's `fn`). It may throw: what it
changed before the throw stays. A closure made in one has `self`'s
value as it was then, and can't change it.

`let` in a struct's body is a constant of the
type (`Font.title`), which `.title` names wherever a `Font` is
expected. `static fn` in a struct's or an enum's body is a function of
the type's, with no `self`: called on the type with named arguments and
defaults as any function (`Money.of(dollars: 2)`), or as `.of(…)`
wherever the type is expected.

```asuka check
struct Money {
  cents: int

  static fn of(dollars: int, cents: int = 0) -> Money = Money(cents: dollars * 100 + cents)
}

let price: Money = .of(dollars: 4, cents: 50)
```

A struct may take type parameters, inferred where it's made, or
written out after its name, as a generic function's are (a store's
too):

```asuka check
struct Page<T> {
  items: [T]
  next: string? = none
}

let page = Page(items: [1, 2, 3])     // a Page<int>
let empty = Page<string>(items: [])   // written out, where nothing else says it
```

A struct pattern matches a struct's fields by their names, in `match`
and `if let`: `y` alone binds the field `y`, `y: py` binds it as `py`,
`x: 0` matches a value, and the fields it leaves out match anything.
Patterns nest:

```asuka
match line {
  Line(from: Point(x: 0, y: 0), dashed: true) => "dashed, from the origin"
  Line(from: Point(x: 0, y), to) => "from {y} on the y axis to {to.x}"
  Line(dashed) => "a line"
}
```

A struct or an enum takes operators from its methods: `fn add(o: T) ->
R` is `a + b`, `fn sub(o: T) -> R` (or `minus`) is `a - b`, `fn mul(o:
T) -> R` (or `times`) is `a * b`, `fn div(o: T) -> R` (or `quo`) is `a /
b`, `fn rem(o: T) -> R` is `a % b`, `fn neg() -> R` is `-a`, and `fn
compare(o: T) -> int` (negative, zero or positive) is `<`, `<=`, `>`
and `>=`, `sorted()`, and `T: Comparable`
([Functions](functions.md#interfaces-and-generics)). `T` may be another
type than the value's (`Money * int`), and an operator with two names
takes one `T` each: a `Decimal`'s `mul(o: Decimal)` and `times(n: int)`
are `price * rate` and `price * 3`. The other order (`3 * price`) isn't
derived. `a += b` is `a = a + b` with them (and `-=`, `*=`, `/=`, `%=`),
when the method gives `a`'s type back, on a var, state, a field or an
element (state changes once). That's how `std/time`'s `Time` adds a
`Duration` and subtracts another `Time`:

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

A struct isn't ordered field by field: what its order means is its
`compare`'s to say.

## Extensions

`extend` adds methods, `mut fn`s, `static fn`s and constants to a type:
a built-in one (`string`, `int`, `[T]`, `[K: V]`, `Set<T>`, `Color`,
`Duration`), the package's own, or another package's.

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

An extension's methods see the value as the type's own do: its fields,
and its methods by their bare names (`lower()` is `self.lower()`). A
constant of the type's is `.name` wherever the type is expected, as a
type's own constants are, and `Color.brand` too; a `static fn` likewise.
An extension adds no fields and no state, and a name it adds can't be
the type's own: the type's layout and members stay its declaration's.
A generic extension takes its type parameters after `extend`
(`extend<K: Hashable, V> [K: V]`).

What an extension adds is visible in its package, and its `pub` members
in the packages that import it, and nowhere else: a dependency doesn't
change what `x.trim()` means in your code. Two extensions you see that
add one name to a type are an error where you use it, naming both
packages.

A type has an interface through what extensions give it, where they're
visible: `extend Money { fn area() -> float … }` makes `Money` a
`Shape`. `extend Money: Shape { … }` says so, and the compiler checks
it as it does `struct Money: Shape`; it's how an enum without payloads
from another package gets an interface. A type has one method of a name
in a program: two packages' extensions that give a type the same
method, both of which make it an interface's, are an error.

## Enums

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

An enum's `mut fn` changes the value too, often by assigning `self`:

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

Where the type is known, a case is written `.name`. `match` must cover
every case (or end with `_ =>`); patterns bind payloads (`.from(p)`, by
their labels too: `.server(code: c)`), match literals (`0`, `"x"`),
several cases (`.a | .b`), nest (`.from(Person(vip: true))`) and take
guards (`.from(p) if p.vip =>`). The compiler follows nested patterns
when it checks that a match covers everything, and says what's missing
as patterns: `match doesn't cover .key(_, false)`. A case's labeled payload with a zero value
(`false`, `0`, `""`, an empty array or map, `none`, an enum's first case)
may be left out where it's made: `.button(title: "Copy", run: copy)`
leaves `Action.button`'s `icon`, `role`, `disabled` and `checked` so.

A case's payload may hold the enum itself, so an enum can be a tree:

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
