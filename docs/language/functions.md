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

```asuka
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

Generic functions and types take type parameters. A parameter's
constraints are the program's interfaces and `Equatable`, `Comparable`
and `Hashable` (structs and enums have those when their fields do),
several joined by `&`. What a type argument must have, its values in
the generic code have: the interfaces' methods and fields, called
directly on each type it's compiled for.

```asuka
fn sortByArea<T: Shape>(xs: [T]) -> [T] = xs.sorted((a, b) => a.area() < b.area())

fn largestOnce<T: Shape & Hashable>(xs: [T]) -> [T] { … }

struct Labeled<T: Shape> {
  item: T
  fn label() -> string = "{item.area()}"
}
```

A generic function's type arguments are inferred from its arguments
and from what's expected of its result; `f<T>(…)` writes them out where
nothing says them: `json.decode<[Note]>(text)`, `flag.parse<Options>(args)`.

A type argument has an interface when it has its methods and fields: a
struct or an enum (an enum without payloads too), an interface asking
for them, or another type parameter constrained so. `T: Error` reads an
error's `message`.
