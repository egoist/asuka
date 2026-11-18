# Functions and closures

```asuka
fn area(width: float, height: float = 1) -> float {
  return width * height
}

fn double(x: int) -> int = x * 2      // one expression

area(3, height: 4)
let squares = [1, 2, 3].map(x => x * x)
let sorted = people.sorted((a, b) => a.age < b.age)
```

Arguments are positional, then by name. A closure is `x => expr`, `(a,
b) => expr` or `x => { statements }`. It shares the variables it
captures with the code around it: what one changes, the other sees.

```asuka
var n = 0
let inc = () => { n += 1 }
inc()
inc()                // n is 2
```

State is shared the same way. Only `spawn` copies what it uses (values
only, as they are when it starts), since it runs on another thread.

A closure is async, and throws, as the function type it's passed as
says (`async fn(int) -> int`). Where nothing says, as for a closure kept
in a `let`, its body decides: it's async if it awaits, and it throws if
an error can leave it. `async` before it says so up front, whether it
awaits yet or not, so that adding an `await` later changes nothing for
its callers; such a closure can't be passed where a function that
can't wait is expected.

```asuka
let refresh = async () => {
  await store.reload()
}
await refresh()
```

When a call misses its `await` or its `try` because of what a closure's
body does, the error says where in the body.

The built-in methods that call a closure at once, an array's `map`,
`filter`, `reduce`, `sorted(by:)` and the rest, take one that throws: the
call then throws what the closure throws, and needs its `try`, as
Swift's `rethrows` does. With a closure that doesn't throw, it doesn't.

```asuka
let counts = try lines.map(l => try parse(l))
let sorted = try names.sorted(by: (a, b) => try rank(a) < try rank(b))
```

A `fn` may be declared in a function's, a handler's, a closure's or a
method's body too. It sees what's around it as a closure does, and it
may call itself and the fns declared next to it in its block (and they
it):

```asuka
fn total(root: Node) -> int {
  var sum = 0
  fn walk(n: Node) {
    sum += n.size
    for c in n.children { walk(c) }
  }
  walk(root)
  return sum
}
```

It's a value like any function (`items.map(walk)`). Its arguments go
in order, without labels or defaults, and it isn't generic.

A function that never returns (it always throws, panics or ends the
program) says so with `-> never`, as `os.exit` does; a call of one ends
a block as a `return` would, and goes where any value does: `let port =
text.toInt() ?? fail("not a port")`.

That's how a function calls itself. A closure kept in a var it calls
itself through (`var walk = …; walk = n => { … walk(c) … }`) holds the
var, and the var holds it: neither is ever freed, since nothing
collects such a cycle, and the compiler warns of it. A local fn calls
itself without holding itself.

## Interfaces and generics

An interface is a set of methods and fields; a type has it if it has
them (nothing to declare).

```asuka check
interface Shape { fn area() -> float }

fn total<T>(items: [T], size: fn(T) -> float) -> float {
  var sum = 0.0
  for i in items { sum += size(i) }
  return sum
}
```

An interface is a type too: a value of one is any value that has its
methods, and calling one calls that value's.

```asuka
let shapes: [Shape] = [Circle(radius: 1), Square(side: 2)]
let areas = shapes.map(s => s.area())
```

An interface may ask for a `mut fn` (`mut fn next() -> T?`, as the
prelude's `Iterator<T>` does): a type's `mut fn` has it, and so does a
`fn` of the same signature. A `mut fn` isn't an interface's plain `fn`,
which is called on any value. A type has an interface through what
[extensions](declarations.md#extensions) give it, too, where they're
visible.

An interface's method may have a body: the interface provides it. A
type has the interface when it has the methods without bodies; a
provided one is the type's own when it declares one of the same
signature, and the interface's otherwise. Inside it, `self` is the
interface's value, with its methods and fields by their bare names:

```asuka check
interface Shape {
  fn area() -> float
  fn describe() -> string = "a shape of {area()}"
}

struct Square {
  side: float
  fn area() -> float = side * side
}

struct Circle {
  radius: float
  fn area() -> float = 3.14159 * radius * radius
  fn describe() -> string = "a circle of {radius}"
}

fn describeAll(shapes: [Shape]) -> [string] = shapes.map(s => s.describe())
```

A provided method is called on the interface's values and on type
parameters constrained by it (`T: Shape`, below); a `Square` that isn't
seen as a `Shape` has no `describe()`. A type that two interfaces each
provide a method of one name to, and that has none of its own, can't be
both: the compiler says so, and the type declares its own.

What an interface's value is, its type, is matched with the patterns of
the types that have the interface: a struct's (`Circle(radius)`), a
name with a type (`c: Circle`), or an enum's case written with its type
(`Unit.big(n)`). A `match` over an interface ends with `_`, since any
type may have it:

```asuka
fn label(s: Shape) -> string = match s {
  Circle(radius: 0) => "a point"
  c: Circle => "a circle of {c.radius}"
  Square(side) => "a square of {side}"
  _ => s.describe()
}

if let c: Circle = shape { … }
let sq: Square = shape else { return }
```

The type must have the interface (a `Shape` can't be a `Point` that has
no `area()`), and an enum without payloads has it only when it declares
it (`enum Unit: Shape`). A value of a concrete type needs no test: `c:
Circle` against a `Circle` is an error. Against a type parameter, the
test is decided as the function is compiled for each type, and an arm
that can't match isn't in the program. `catch e: ParseError` is the
same test, of an `Error` ([Errors](errors.md)).

Generic functions and types take type parameters. A parameter's
constraints are the program's interfaces and `Equatable` (what `==`
compares: the built-in values, and structs, enums, tuples, arrays, maps
and optionals whose parts are), `Comparable` (what `<` orders: ints,
floats, strings, durations, enums without payloads by the order of
their cases, and structs and enums with `fn compare(other: T) -> int`;
a struct isn't ordered field by field) and `Hashable` (what a map's key
can be: the built-in values but floats, and the structs, enums, tuples,
optionals, arrays, maps and sets made of them; see [Values and
types](types.md#values-and-types)), several joined by `&`. What a type argument must have, its values in
the generic code have: the interfaces' methods and fields, called
directly on each type it's compiled for.

```asuka
fn sortByArea<T: Shape>(xs: [T]) -> [T] = xs.sorted((a, b) => a.area() < b.area())

fn largestOnce<T: Shape & Equatable>(xs: [T]) -> [T] { … }

struct Labeled<T: Shape> {
  item: T
  fn label() -> string = "{item.area()}"
}
```

A generic function's type arguments are inferred from its arguments
and from what's expected of its result; `f<T>(…)` writes them out where
nothing says them: `json.decode<[Note]>(text)`, `flag.parse<Options>(args)`.

A generic is compiled for each type it's used with, so it can't come
back to itself with a bigger one: `fn depth<T>(x: T) -> int` calling
`depth([x])` would need `depth<[int]>`, then `depth<[[int]]>`, without
end, and so would `struct Nested<T> { more: Nested<[T]>? }`. The
compiler says where; a type that holds itself takes the same arguments
(`Tree<T>` in `Tree<T>`).

A type argument has an interface when it has its methods and fields: a
struct or an enum (an enum without payloads too), an interface asking
for them, or another type parameter constrained so. A method is the
interface's when it takes the same parameters: one that takes more,
even with defaults (`greet(loud: bool, times: int = 1)` for `greet(loud:
bool)`), isn't, since a call through the interface passes only its own. `T: Error` reads an
error's `message`.
