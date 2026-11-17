# 0011: The everyday language

- **Status**: Draft
- **Created**: 2026-11-18
- **Related**: 0007 (the Asuka language), lang/LOWERING.md,
  docs/language/

## Summary

Asuka's core holds up: values with copy on write, owners that dispose
what they made, enums with exhaustive `match`, structured async and
`@platform`. Its gaps show up in the model and business code around the
views, which every app of any size has. This RFC fills the ones that
come up most often, in the order they come up:

1. **`mut fn`**: a struct's or an enum's method that changes it, called
   on a `var`, on state or on a part of one.
2. **Type tests on interface values**: `match shape { Circle(radius)
   => … }`, `if let c: Circle = shape`, as `catch` already does for
   errors.
3. **Writes through `?.`**, and a map's entry with a default:
   `inbox[id]?.read = true`, `counts[word, default: 0] += 1`.
4. **Extensions**: `extend string { … }`, `extend Color { let brand =
   #7c3aed }`, on any type, built in or another package's.
5. **Provided methods in interfaces**: a method with a body that a
   type may have its own of.
6. **Iteration over your own types**: `for x in v` when `v` has `mut
   fn next() -> T?`, or `fn iter()` that returns such a type.
7. **Labeled loops**: `for row in rows as outer { … break outer }`.
8. **Operators from `mul`, `div`, `rem` and `neg`**, beside today's
   `add`, `sub` and `compare`.
9. **Integer overflow is an error**, as dividing by zero is; `&+`,
   `&-`, `&*` wrap.
10. **`std/decimal`**: exact decimal numbers, for money.
11. **`worker`**: a long-lived value on a thread of its own, called
    from the main thread with values in and values out.

```asuka
struct Cart {
  items: [Item] = []

  mut fn add(i: Item) {
    if let n = items.index(x => x.id == i.id) { items[n].qty += 1 } else { items.push(i) }
  }
  fn total() -> Decimal {
    var t = Decimal.zero
    for i in items { t = t + i.price * i.qty }
    return t
  }
}

extend Color { let brand = #7c3aed }

store Shop {
  state cart = Cart()
  state orders: [string: Order] = [:]
  pub fn buy(i: Item) { cart.add(i) }                          // only what read cart updates
  pub fn markShipped(id: string) { orders[id]?.shipped = true }  // nothing when there's none
}

fn describe(s: Shape) -> string = match s {
  Circle(radius) => "a circle of {radius}"
  sq: Square => "a square of {sq.side}"
  _ => "a shape of {s.area()}"
}
```

Each part stands alone and is useful at once; together they make the
language one an app's whole codebase can be written in without the
workarounds below. Nothing here adds classes, shared mutable references
or a garbage collector: values stay values, and what's shared stays a
store's or a worker's, owned and disposed explicitly.

## Motivation

Each item is something an app does all the time, what happens today
(the compiler's own words), and the workaround.

- **A method that changes its value.** `struct Counter { n: int; fn
  inc() { n += 1 } }` is "can't assign to n: a struct's methods can't
  change it (it's a value); return a new one". Assigning a field from
  outside works (`c.n += 1`, `todos[0].done = true`), so the rule only
  moves the logic out of the type: a `Cart`'s `add` becomes `cart =
  cart.adding(item)` at every call, or a free function over a `var`.
  Swift's `mutating`, Rust's `&mut self` and C#'s `ref` structs exist
  for this; with copy on write it costs nothing, since a value one place
  holds is changed in place already.
- **What's in an interface value.** `let x: Shape = Square(side: 2);
  match x { Square(side) => … }` is "a Shape can't be a Square". An
  `[Shape]`, a `[Message]` of several kinds, an `[Event]` from a
  package: once a value is the interface, nothing can ask what it is.
  `catch e: ParseError` and `match e { MailError.offline => … }` already
  do this for `Error`; nothing else can. The workaround is an enum
  around every type, which closes what an interface is for.
- **A change to an entry that may not be there.** `inbox[id]?.read =
  true` is "can't assign to through ?.: unwrap it first (if let)". So
  it's `if let m = inbox[id] { var c = m; c.read = true; inbox[id] =
  c }`: a lookup, a copy and a write for one bool. Counting is `counts[w] = (counts[w] ?? 0) + 1`. Both are in
  every store.
- **A method on a type you don't own.** `fn string.slug()` doesn't
  parse, and there's no other form: a helper for `string`, `[T]`,
  `Color` or another package's `Message` is a free function, so
  `title.trim().slug().upper()` is `slug(title.trim()).upper()`. A
  design system's colors and fonts can't be `.brand` and `.display`
  where a `Color` and a `Font` are expected, as the built-in ones are.
- **A default for an interface's method.** "an interface's methods have
  no bodies". Every type that has `Shape` writes its own `describe()`,
  or the interface loses it.
- **Looping over a tree, a page, a cursor.** `for x in v` is "for goes
  over an array, a set, a map or a range, not a Walk". A tree is
  flattened into an array first; paginated results and a database's
  rows are read whole, or by a `while let x = it.next()` that today
  can't be written either, since `next` would change `it`.
- **Leaving two loops.** `outer: for …` is a prop in an element's block,
  so there's no label: a search over a grid sets a flag and tests it.
- **Arithmetic on a type of the app's.** `+`, `-` and the comparisons
  come from `add`, `sub` and `compare`; `*` and `/` from nothing, so a
  `Money` or a vector is `price.times(3)`.
- **Overflow.** `int` wraps: `9223372036854775807 + 1` is
  `-9223372036854775808`, silently. Hashes want that; an app's counts,
  sizes, prices and timestamps never do, and a wrapped one is a wrong
  number shown or saved rather than an error in development.
- **Money.** `platform/pay`'s `Item.amount` is a `string` ("12.75")
  because there's nothing exact to make it. `float` isn't (`0.1 + 0.2`),
  and cents in an `int` break with currencies of three decimals and
  with rates.
- **A long-lived thread.** `spawn { }` runs one block and ends. A search
  index, an audio analyzer, a sync engine or a database the main thread
  mustn't wait on is state that lives on its own thread and answers
  calls. Today it's rebuilt on every `spawn`, or kept on the main thread.

## Decisions

| Question | Decision | Why |
|---|---|---|
| Methods that change | `mut fn`, in structs, enums and extensions; called on a place (a `var`, state, a field or element of one) | Swift's `mutating`; the call reads as a method and the compiler knows what's written. |
| A `mut fn` that throws | What it changed before the throw stays | A rollback would copy the value up front for every throwing call; Swift does the same. The caller that wants all or nothing changes a copy and assigns it. |
| A `mut fn` and `await` | Not async | A value that's half changed across an `await` is seen by every handler that runs meanwhile. An async change is a store's `fn`, as today. |
| Type tests | Patterns: a struct's pattern (`Circle(radius)`), a typed name (`c: Circle`), an enum's case (`Gesture.tap(at)`); in `match`, `if let`, `let … else` | What `catch` already takes for errors, so one rule for both. No `as` or `is` expression: `as` names things (imports, views, loops). |
| Writes through `?.` | A place's `?.` chain may be assigned or `mut`-called; it does nothing when it meets none | Swift's optional chaining assignment; the write tells nothing to state's readers when it doesn't happen. |
| A map's default | `m[k, default: v]`, a place | Swift's; counting and grouping are one line. |
| Extensions | `extend T { … }`: methods, `mut fn`s, `static fn`s and constants; no fields | Kotlin's and Swift's reach without their pitfalls: no stored state, no overriding, visible only where the package that declares it is imported. |
| Defaults in interfaces | A method with a body in an interface is provided: a type that has its own uses its own | Structural interfaces keep working (a type needn't have it); the interface grows without breaking its types. |
| Iteration | Structural: `mut fn next() -> T?`, or `fn iter()` returning such a type; prelude `Iterator<T>` names it for constraints | Uses `mut fn`; no new kind of type. Generators (`yield`) later, if ever. |
| Labels | `for … as name`, `while … as name`; `break name`, `continue name` | `name:` is a prop in an element's block; `as` already names things. |
| Operators | `mul`, `div`, `rem`, `neg` join `add`, `sub`, `compare` | The same mechanism, completed. |
| Overflow | `+`, `-`, `*` on `int` throw a runtime error, as `/` by zero does; `&+`, `&-`, `&*` wrap | Swift traps, Asuka already throws for `/`: one rule for both. One predicted branch on the CPU's overflow flag; an error in development beats a wrong number in production. |
| Decimal | `std/decimal`, written in Asuka over `int`s, with `mul`/`div` and the rest | Exact base 10 for money, the same on every platform, no native code. |
| Threads that live | `worker Name { … }`: a type whose instances run on their own thread, owned as a store is; its `pub fn`s are async from outside, values in and out | An actor without shared memory: `spawn`'s rule (values only), kept alive. |

## The language

### `mut fn`

```asuka
struct Cart {
  items: [Item] = []

  mut fn add(i: Item) {
    if let n = items.index(x => x.id == i.id) { items[n].qty += 1 } else { items.push(i) }
  }
  mut fn clear() { self = Cart() }
}

enum Light {
  red
  green
  yellow

  mut fn next() { self = match self { .red => .green; .green => .yellow; .yellow => .red } }
}

var cart = Cart()
cart.add(apple)
state light = Light.red
on press { light.next() }
```

- A `mut fn` may assign its fields (and their parts), call the
  value's other `mut fn`s, and assign `self`. A `fn` can't, as now.
- It's called on a **place**: a `var`, state (a component's, a
  store's, the app's), or a field, element or map entry of one
  (`todos[i].toggle()`, `cart.items[0].bump()`). Not on a `let`, a
  parameter, a prop, or a value a call returns: "add changes its value:
  call it on a var or state".
- On state, it's a write of that state, as the assignments in its body
  would be from outside: `todos[i].toggle()` updates what reads
  `todos[i]`, not every row. It's one write however many fields it
  changes, shown when the handler ends as any write.
- It can't be `async`. It may `throw`: what it changed before the throw
  stays (see Decisions).
- A closure made inside it captures `self`'s value as it is then, and
  can't change it: a closure that outlives the call can't write a
  place the caller owns.
- In a struct's or an enum's body, and in an `extend`'s. An interface
  may ask for one (`mut fn next() -> T?`); a type's `fn` of the same
  signature has it too.

### Type tests on interface values

A value of an interface type is matched by the patterns of the types
that have it:

```asuka
fn describe(s: Shape) -> string = match s {
  Circle(radius: 0) => "a point"
  Circle(radius) => "a circle of {radius}"
  sq: Square => "a square of {sq.side}"
  _ => "a shape of {s.area()}"
}

if let c: Circle = shape { … }
let sq: Square = shape else { return }
for e in events { if let Gesture.tap(at) = e { … } }
```

- A struct pattern (`Circle(radius)`) matches when the value is that
  struct, then its fields as today. A typed name (`c: Circle`) binds the
  value as that type. An enum's case, written with its type when the
  value is an interface (`Gesture.tap(at)`), matches when the value is
  that enum and that case.
- An enum without payloads is an interface value only when it declares
  the interface (`enum Unit: Shape`), as today, so only such enums'
  cases match there.
- A `match` over an interface ends with `_` (an interface is open: any
  type may have it). `catch` keeps its clauses, and is the same rule
  for `Error`.
- The pattern's type must have the interface: `Circle(radius)` against
  a `Shape` when `Circle` has no `area()` is "a Shape can't be a Circle:
  it has no area()". Against a value of a concrete type, a type test is
  an error ("s is always a Circle").
- Against a type parameter (`T: Shape`), it's decided as the function
  is compiled for each type (functions are compiled per type
  argument), and a branch that can't match is dropped, as
  `@platform`'s are.
- `let c: Circle = shape` without `if` or `else` is "a Shape isn't
  always a Circle: if let c: Circle = shape, or let … else".

### Writes through `?.`, and a map's default

```asuka
inbox[id]?.read = true                  // nothing when there's no such message
session.user?.name = draft
todos.first()?.toggle()                 // an error: first() isn't a place
counts[word, default: 0] += 1
groups[key, default: []].push(item)
```

- A `?.` chain that starts at a place, and goes through fields,
  subscripts and `?.`, is a place: it may be assigned (`=`, `+=`, …),
  and its `mut fn`s called. When the chain meets `none`, nothing
  happens: nothing is written, and state's readers aren't told.
- `m[k, default: v]` is a map's entry, or `v` when there's none. Read,
  it's a `V` (not a `V?`). Written (`=`, `+=`, a `mut fn`, `push`), it
  writes `m[k]`; `v` is evaluated only when the entry is missing.
- `if var x = optional { … }` binds a copy that may change, for code
  that changes several fields and writes back: as `if let`, with a
  `var`.

### Extensions

```asuka
extend string {
  fn slug() -> string = lower().fields().joined(separator: "-")
}

extend Color {
  let brand = #7c3aed
  let brandSoft = #7c3aed.opacity(0.15)
}

extend Font { let display = Font(size: 34, weight: .bold) }

extend<T: Comparable> [T] {
  fn median() -> T? = if isEmpty() { none } else { sorted()[count() / 2] }
}

extend mail.Message {
  fn isFromVIP(vips: Set<string>) -> bool = vips.contains(from.address)
  mut fn archive() { folder = .archive }
}

Text(title.slug()) { color: .brand; font: .display }
```

- `extend T { … }` adds `fn`s, `mut fn`s, `static fn`s and constants
  (`let`) to any type: a built-in one (`string`, `int`, `[T]`, `[K:
  V]`, `Color`, `Duration`), the package's own, or another package's.
  A constant of `T`'s type is `.name` wherever a `T` is expected, as a
  struct's own are.
- No fields, no state, no `init`: the type's layout is its own.
- An extension's members see the value's fields and methods as the
  type's own would (`from.address`), the public ones of a type from
  another package.
- `pub` members are visible in packages that import the declaring
  package, and only there; others are the package's own. A name can't
  be the type's own (an error at the `extend`), and two extensions an
  importer sees with one name are an error where it's used, naming
  both packages: `slug is extended by ./text and @acme/strings; import
  one`.
- A type has an interface through its extensions too, where they're
  visible: `extend Money { fn area() -> float … }` makes `Money` a
  `Shape` in the packages that see it. `extend Money: Shape { … }`
  says so, and checks it, as `struct Money: Shape` does; it's how an
  enum without payloads from another package gets an interface.
- `extend` is a declaration at a file's top level. Generic extensions
  take their parameters after `extend`: `extend<T: Hashable> Set<T>`.

### Provided methods in interfaces

```asuka
interface Shape {
  fn area() -> float
  fn describe() -> string = "a shape of {area()}"
}

struct Square { side: float; fn area() -> float = side * side }
struct Circle {
  radius: float
  fn area() -> float = 3.14159 * radius * radius
  fn describe() -> string = "a circle of {radius}"
}

let shapes: [Shape] = [Square(side: 2), Circle(radius: 1)]
shapes.map(s => s.describe())   // ["a shape of 4.0", "a circle of 1.0"]
```

- A method with a body is provided. A type has the interface when it
  has the methods without bodies; the provided ones are its own where
  it declares them (with the same signature), and the interface's
  otherwise.
- They're called on values of the interface and on type parameters
  constrained by it (`T: Shape`): `s.describe()`. A `Square` that isn't
  seen as a `Shape` has no `describe()`: that's what an extension is
  for.
- A provided method sees the interface's methods and fields through
  `self`, and may call the others provided.

### Iteration

```asuka
struct Walk {
  stack: [Node]

  mut fn next() -> Node? {
    let n = stack.pop() else { return none }
    stack = stack + n.children.reversed()
    return n
  }
}

extend Node { fn walk() -> Walk = Walk(stack: [self]) }

for n in root.walk() { if n.name.hasSuffix(".asuka") { count += 1 } }
```

- `for x in v` takes, beside arrays, sets, maps and ranges, a value
  whose type has `mut fn next() -> T?` (an iterator: `for` changes a
  copy of it) or `fn iter() -> I` where `I` is one (a sequence:
  `for` calls `iter()` once).
- The prelude declares `interface Iterator<T> { mut fn next() -> T? }`,
  for constraints: `fn sum<I: Iterator<int>>(xs: I) -> int`.
- `for await` stays streams'. Arrays, sets, maps and ranges keep their
  own loops: no iterator is made for them.

### Labeled loops

```asuka
for row in grid as rows {
  for cell in row {
    if cell == target { found = true; break rows }
    if cell.isEmpty() { continue rows }
  }
}
```

`as name` after a `for`'s or a `while`'s header names the loop;
`break name` and `continue name` leave it, or go on to its next turn,
from any loop inside it (not from a closure). A name used and not
declared, or declared and not used, is an error.

### Operators

A struct (or an extension) takes `*` from `fn mul(o: T) -> R`, `/`
from `div`, `%` from `rem` and unary `-` from `fn neg() -> R`, beside
`add`, `sub` (`minus`) and `compare`. The argument's type may differ
from the value's (`Money * int`); the operator's other order (`int *
Money`) isn't derived.

### Integer overflow

`+`, `-` and `*` on `int`, and `-x` of the smallest `int`, throw on
overflow, as dividing by zero does today: an error ("integer overflow")
that a `catch` takes, and that goes to the nearest `Boundary`, or the
app's, when nothing does. `&+`, `&-` and `&*` wrap, for hashes,
checksums and binary protocols; the shifts and the bitwise operators
are unchanged. `int(x)` of a float out of range still gives the
nearest.

`a.addingChecked(b)`, `subtractingChecked` and `multipliedChecked` give
an `int?`, `none` on overflow, for code that handles it as a value
(`std/decimal` does).

### `std/decimal`

```asuka
import std/decimal.{Decimal}

let price: Decimal = .parse("19.99")!
let total = price * 3 + Decimal.parse("4.50")!          // 64.47
let each = (total / 4).rounded(places: 2, mode: .halfEven)
Text(each.formatted(currency: "EUR"))                  // the locale's
```

A `Decimal` is a signed coefficient and a base-10 exponent (an `int`
each: 18 significant digits), exact for `+`, `-` and `*`; `/` and
`rounded` take a number of places and a rounding mode. It's a struct
written in Asuka, so it's the same on every platform, with no native
code; `formatted` uses the core's CLDR tables, as `i18n`'s numbers do.
`platform/pay`'s amounts and `platform/purchase`'s prices become
`Decimal`s (see Migration).

### `worker`

```asuka
worker SearchIndex {
  var words: [string: [int]] = [:]

  pub fn add(id: int, text: string) {
    for w in text.fields() { words[w.lower(), default: []].push(id) }
  }
  pub fn find(q: string) -> [int] = words[q.lower()] ?? []
}

store Notes {
  let index = SearchIndex()
  state results: [int] = []

  pub async fn search(q: string) { results = await index.find(q) }
}
```

- A `worker` declares a type whose instances each run on a thread of
  their own. Its `var`s are its own, read and written only there; no
  `state` (views don't read it).
- Its `pub fn`s, called from outside, are `async`: the arguments go
  over as values, the call runs on the worker's thread in the order
  calls were made, and the result comes back as a value. Inside, they're
  ordinary functions: they don't await.
- What crosses is `spawn`'s rule: values only, never state, stores,
  views, closures or native objects. A worker may make and keep a C
  handle of its own (an `extern` type, a database it opens): it never
  leaves the worker.
- A worker belongs to the component or store that made it. Disposing
  that stops it: the call running finishes, the ones waiting are
  cancelled (their callers' tasks see cancellation, not an error).
- On the web, a worker is a Web Worker running the core's wasm, as
  `spawn`'s are.

## Checking

New errors, each with what to do:

- Assigning a field, or calling a `mut fn`, in a `fn`: "move is a fn:
  declare it mut fn to change the value".
- Calling a `mut fn` on what isn't a place: "add changes its value:
  call it on a var or state (cart is a let)".
- An `async mut fn`: "a mut fn can't be async: change a store's state
  from an async fn instead".
- A type test that can't match: "a Shape can't be a Circle: it has no
  area()"; one that always does: "s is always a Circle".
- A `match` over an interface without `_`.
- `extend` declaring a name the type has, or a field; two visible
  extensions with one name, where it's used.
- `?.` assignment whose chain doesn't start at a place.
- A loop label unused, undeclared, or used from a closure.
- `&+` on a type that isn't `int`.
- A worker's `pub fn` taking or returning what isn't a value; a worker
  with `state`.

## Lowering and the core

Each part builds on machinery that's there; lang/LOWERING.md gets a
section for each as it's built.

- **`mut fn`.** Checked as a method (`methodCall1`, lang/check/call.go)
  whose receiver must pass `checkLvalue` (lang/check/stmt.go), the rule
  assignment already uses; in its body, `selfMember`s and `self` become
  assignable (today's "a struct's methods can't change it" is raised
  there, and stays for `fn`). Lowered through the IR's places
  (`lvalue`, lang/ir/stmt.go): the call is an `update` of the receiver's
  place, as `x.f = v` and the inout built-ins (`push`, `sort`) are. The
  receiver moves into the call's first argument register
  (`callInout`'s convention), so it stays unique and `SETF`/`SETIDX` in
  the body change it in place (`ak_struct_mut` and `ak_arr_mut` copy
  only when it's shared); the caller takes it back from that register
  when the call returns. cgen passes the slot's address. On state, the
  place is a cell: `CTAKE`, the call inside the `protect` handler that
  `CPUT`s on an error, `CPUT`. That handler is what makes "what it
  changed before the throw stays" free. Readers narrow as they do for
  `todos[0].done = true` today: rows and memos compare (`ak_val_eq`), so
  a row whose element kept its pointer isn't updated. Nothing allocates
  beyond what the body does.
- **Type tests.** An interface value is the struct's or enum's own
  reference (LOWERING.md's table of kinds), and its `ak_shape` is its
  dynamic type, so a test is a shape comparison: a new `ISSHAPE r, k`
  (a shape constant) beside `JNONE`, then the struct pattern's fields
  as `testPattern` does now; an enum's case adds the case tag's
  comparison. Errors keep `__etype` (their ids also cross the FFI and
  the web's encoding); `catch` and `match` on `Error` may move to
  `ISSHAPE` later. The checker's `structPattern`
  (lang/check/patterns.go) and the `CasePat` branch (stmt.go) accept a
  type that has the scrutinee's interface where they demand the same
  type today. Under monomorphization (`ifaceInvoke`), a type parameter's
  test is decided at compile time and the dead branch isn't emitted.
- **Provided methods.** `l.implement` (lang/ir/lower.go) fills a
  shape's method table, by name, when a type converts to an interface;
  for a provided method the type lacks, it enters the interface's body
  compiled for that shape (its `self` the concrete type, its calls to
  the interface's methods direct `CALL`s). `CALLI`'s lookup by name
  (`ak_vm_method`) is unchanged. lang/check/decls.go stops rejecting
  bodies in interfaces and checks them with `self` as the interface.
- **Writes through `?.`.** A new place kind, `lvOptional`: `prepare`
  reads the chain up to each `?.` without taking it, `JNONE` jumps past
  the whole update. The test comes first, so on a cell a none never
  reaches `CTAKE`/`CPUT` (`CPUT` always writes the signal). The
  checker's "can't assign to through ?." (stmt.go) becomes the check
  that the chain starts at a place. `m[k, default: v]` is `lvIndex`
  with a default: `MGET`, `v` when none, the update, `MSET`. `if var`
  is `if let` with the binding a `var`.
- **Extensions.** Their members are functions with the receiver first,
  called as `funcCall` calls a method (`CALL`, not `CALLI`). The
  checker's method lookup (`methodCall1`) gains a step after the type's
  own members and before the built-in table: the extensions visible in
  the calling package, by its imports. Constants are a file's `let`s,
  found by `.name` where the type is expected. A type that has an
  interface through an extension enters the extension's function in its
  shape's table at the conversion (`implement`); since a shape has one
  table per program, two packages whose extensions give one type the
  same method name and both convert it to an interface are an error when
  the app is compiled, naming both.
- **Iteration.** `forStmt` (lang/check/stmt.go) gets `ForIter`: the
  value (or `iter()`'s result) is copied into a hidden `var`, and each
  turn is `next()` called as a `mut fn` on it, `JNONE` to the loop's
  end. The other forms keep their counters.
- **Labels.** The parser reads `as name` after a loop's header (it's
  not an expression operator, so it's unambiguous); the checker's loop
  counter becomes a stack of names; the IR's `b.loops` entries get the
  name, and `branch()` runs the `defer`s of every loop it leaves, as it
  does for one now.
- **Operators.** The method names expression checking looks for
  (`add`; `sub`, `minus`; `compare`: lang/check/expr.go) gain `mul`,
  `div`, `rem` and `neg`. Lowered as calls, as `+` on a struct is.
- **Overflow.** `ADDI`, `SUBI`, `MULI` and `NEGI` become checked: an
  inline `ak_op_addi` (core/src/vm/ops.h) with
  `__builtin_add_overflow`, raising `ak_throw_msg("integer overflow")`
  and going to the handler, as `ak_op_divi` does for zero (the VM's
  `goto unwind`, cgen's `goto <handler>`). Every toolchain the core
  builds with (clang, Zig's clang, the NDK's, emcc's) has the builtins.
  `WADDI`, `WSUBI`, `WMULI` keep today's wrapping (`uint64_t` casts) for
  `&+`, `&-`, `&*`, and for the compiler's own counters (`ADDIK` in a
  `for` over a range can't overflow and stays unchecked).
  lang/bytecode/opcodes.go, then `go generate ./lang/bytecode`. The
  checked methods are built-ins (lang/check/methods.go, core/src/rt).
- **`std/decimal`.** Plain Asuka over `int`s and the checked methods;
  `formatted` through `std/i18n`'s number formatting. No core changes.
- **Workers.** `spawn` already runs closures on a pool of threads
  (core/src/rt/spawn.c): `ak_deep_copy` of the captures in, of the
  result back, `ak_post` to resolve the future on the main thread,
  `ak_sched_hold` while it's out; on the web, a Web Worker with
  `ak_keep_encode` (core/src/abi/web.c). A worker is that, kept alive:
  `core/src/rt/worker.c`, a thread of its own per worker with a queue
  (the pool's mutex and condition variable), whose state object is
  made, read and freed on that thread only, so its reference counts
  stay non-atomic. A `pub fn` call deep-copies its arguments onto the
  queue and returns a future, resolved by `ak_post` as `spawn`'s is.
  The owner's disposal enqueues a stop; calls still queued resolve as
  cancelled. On the web, one Web Worker per worker, its calls as
  messages. The checker's values-only rule is `spawn`'s
  (`spawnState`, lang/check/expr.go), applied to a worker's `pub fn`s'
  parameters and results, and to everything its body reads from
  outside.

Tests: the checker's errors in lang/check/testdata/errors (`// ERROR`
lines), each part's behavior in lang/compile/testdata/run (on the VM and
compiled to C, compared to `// Output:`), leaks in
lang/compile/leaks_test.go for `mut fn` on cells and for workers, the
parser's and the printer's golden files (lang/syntax/testdata), the
tour's code compiled by `TestDocExamples`. In the core: a `mut fn` on a
state array counted (ops and allocations: one write, no copy when
unique), a `?.` write on none notifying nothing, overflow on the VM and
in C, a worker under ASan, UBSan and TSan, disposed with calls queued.

## Migration

- **Keywords.** `mut`, `extend` and `worker` become keywords. No `.asuka`
  file in the repository uses them as names (std, sdk, examples, the
  tests' testdata); `asuka fmt` can't rename for users, so the release
  notes say it, and the error at a keyword used as a name says
  "rename it: mut is a keyword since 0.N".
- **Overflow.** Code that relies on `int` wrapping changes `+`, `-`, `*`
  to `&+`, `&-`, `&*`. In the repository that's hashes and checksums in
  std (found by running each package's tests with the check, and by
  grepping for the FNV and CRC constants); for apps, the error names
  the line. A release with a warning (`asuka check --overflow`) can come
  first if that's too sharp.
- **Free functions and wrapper enums** keep working; nothing about
  today's code changes meaning except overflow.
- **`platform/pay` and `platform/purchase`** take `Decimal` amounts. The
  string fields stay one release, deprecated, read when the `Decimal`
  is zero.
- **Docs.** docs/language's pages (declarations.md: `mut fn`,
  extensions; functions.md: provided methods, type tests; types.md:
  operators, overflow; control-flow.md: patterns, labels, `?.` writes,
  iteration; async.md: workers; README.md's keywords), the tour,
  docs/stdlib.md for `std/decimal`, and their zh and ja translations.

## Alternatives

- **Methods that return a new value** (today's answer). Fine for small
  values, noisy for the rest: every call site assigns, and a store's
  `cart = cart.adding(x)` reads as a rebuild even when copy on write
  makes it in place.
- **Classes.** A reference type would give changeable methods and type
  tests at once, and bring back shared mutation, cycles and `weak`:
  what values and owners exist to rule out.
- **`as` and `is` expressions** (`shape as? Circle`, `shape is
  Circle`). Shorter for one test, but a second way to say what patterns
  say, and `as` already names things in three places. If `if let c:
  Circle = shape` proves clumsy in practice, `is` alone (a bool) is the
  smallest addition.
- **Rollback for a throwing `mut fn`.** Nicer for state, but a copy of
  the value before every call that can throw, on paths that otherwise
  allocate nothing.
- **Kotlin-style `fn string.slug()`.** One declaration per method, no
  place for constants or `static fn`s, and harder to make generic.
- **Extension methods visible everywhere once declared.** Simpler
  lookup, but an app's dependency could change what `x.trim()` means in
  the app; visibility through imports keeps it local.
- **Generators (`yield`) for iteration.** The nicest to write, but a
  function that suspends mid-loop is a second kind of async; an
  iterator struct is plain code, and `Stream` already covers the
  asynchronous case.
- **Overflow errors in development only.** The same program would
  compute differently built for release. Swift traps in every build;
  Rust traps only in debug builds, a difference its users meet as a
  bug that shows only in production.
- **A `Decimal` in the prelude, with a literal (`19.99d`).** Nicer to
  write; std first, the literal once the type has proven its shape.
- **Channels instead of workers.** More general, but a channel shared by
  tasks on two threads needs rules a worker's ordered calls don't; a
  worker's `Stream`-returning `pub fn` can come later for pushing
  values back.

## Open questions

1. **`&` for arguments.** A `mut fn` changes `self`; a function that
   changes an argument (`fn swap(a: &int, b: &int)`, Swift's `inout`)
   is the other half. Left out until a case needs it beyond `mut fn`.
2. **`is`.** Whether a boolean type test is worth a keyword beside the
   patterns.
3. **Overflow's default.** Throwing changes what existing code
   computes. The decision above is the RFC's; it wants the owner's yes
   before milestone 5.
4. **Exhaustive matches over closed interfaces.** A `sealed interface`
   whose types are its package's would let `match` drop `_`. Wanted by
   packages' event types; not needed yet.
5. **A worker's streams.** `pub fn progress() -> Stream<float>` from a
   worker, for an index build or an upload that reports as it goes.
6. **Iterator adapters.** `walk().filter(…).map(…)` without making
   arrays: lazy adapters on `Iterator<T>` in the prelude, or `std/iter`.
7. **Extensions' constants in styles.** Whether `extend Color` and
   `extend Font` constants should also be what 0009's themes give, or
   stay plain constants beside them.

## Milestones

1. **`mut fn`.** Checker, lowering (on vars, state, fields and
   elements), the errors; tests on the VM and compiled to C, state
   writes counted per reader. docs.
2. **Places through `?.`, `m[k, default:]`, `if var`.** Depends on 1's
   places.
3. **Type tests and provided methods.** Patterns over interface values
   in `match`, `if let`, `let … else`; provided methods; tests on the VM
   and in C, monomorphized branches dropped.
4. **Extensions**, then **iteration** (which uses 1 and 4) and
   **labeled loops** and the **operators**.
5. **Overflow.** The check in the VM and cgen, `&+ &- &*`, the
   checked methods, std migrated, the warning release if wanted.
6. **`std/decimal`**, and `platform/pay` and `platform/purchase` on it.
7. **`worker`.** The core's thread per worker (and the web's Web
   Worker), ordered calls, cancellation with its owner, the checker's
   values-only rule; tests under ASan and TSan.
