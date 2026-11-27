# A tour of Asuka

Asuka apps are written in Asuka, a small language made for interfaces
that use each platform's own widgets. It's statically typed, compiles
ahead of time for release builds, and reloads in place as you save.
This page shows the language in the order you meet it; the [language
reference](language/README.md) has the whole of each part.

```asuka check
component Counter {
  state count = 0

  Column {
    gap: 12; padding: 24
    Text("Count: {count}") { font: .title }
    Button("Add") { on press { count += 1 } }
  }
}

app { Counter() }
```

A file is declarations: components, functions, types, state. `app { }`
is the app's root; the rest is in any order.

## Values

```asuka check
fn values() -> string {
  let name = "Asuka"               // a constant: its type is inferred
  var visits = 0                   // a variable
  visits += 1
  let greeting = "Hello, {name}"   // {…} puts a value in a string
  let ratio: float = 2.5
  let level: u8 = 200              // sized integers: i8 … i32, u8 … u64 (int is 64 bits)
  let area = 2 ** 10               // a power: 1024
  let tags = ["new", "work"]       // an array: [string]
  let ages = ["ana": 31, "bo": 28] // a map: [string: int]
  let nickname: string? = none     // an optional: a string or none
  let shown = nickname ?? name     // none's fallback
  return "{greeting} {visits} {ratio} {level} {area} {tags.count()} {ages["ana"] ?? 0} {shown}"
}
```

`var` is for functions and handlers: what's at a file's top level
doesn't change, unless it's `state`. Numbers of different types don't
mix: `int(level) + visits` converts the `u8` first, and arithmetic past
a type's range is an error, not a wrapped number. Values are copied, never shared:
changing a copy of an array leaves the original as it was. Strings, arrays and maps have the methods you'd
expect (`count()`, `contains`, `map`, `filter`, `sorted`, `split`...):
[Values and types](language/types.md).

## Functions

```asuka check
fn area(width: float, height: float = 1) -> float {
  return width * height
}

fn double(x: int) -> int = x * 2     // one expression

let a = area(3, height: 4)           // in order, then by name
let squares = [1, 2, 3].map(x => x * x)
```

Arguments come in order, then by name (`area(3, height: 4)`); a
parameter with a default may be left out. `x => …` is a closure. [Functions and
closures](language/functions.md)

## Structs and enums

```asuka check
struct Message {
  subject: string
  read: bool = false

  fn preview() -> string = subject.slice(0, 40)
}

enum Filter {
  all
  unread
  from(sender: string)
}

fn label(f: Filter) -> string = match f {
  .all => "Everything"
  .unread => "Unread"
  .from(sender) => "From {sender}"
}

let m = Message(subject: "Hello")
let n = label(.from(sender: "ana"))

struct Counter {
  n: int = 0

  mut fn bump() { n += 1 }
}

extend string {
  fn shout() -> string = upper() + "!"
}

fn bumped() -> string {
  var c = Counter()
  c.bump()
  return "{c.n}".shout()
}
```

A struct is fields and methods; a `mut fn` changes the value it's
called on (a `var`, or state). `extend` adds methods and constants to
any type, `string` and `Color` included. An enum's cases can carry
values, and `match` covers every case (the compiler says which one is
missing).
`if`, `match` and blocks are values too. [Declarations](language/declarations.md),
[Control flow](language/control-flow.md)

## Components and state

```asuka check
component Greeting(name: string) {
  prop excited = false                // a prop, with its default
  state draft = ""                    // the component's own state

  Column {
    gap: 8
    Text(if excited { "Hi, {name}!" } else { "Hi, {name}" })
    TextField { bind text: draft; placeholder: "Reply" }
    if !draft.isEmpty() {
      Button("Send") { on press { draft = "" } }
    }
  }
}

app { Greeting("Ana") { excited: true } }

preview "Greeting, dark" (dark: true) { Greeting("Ana") }
```

A component's header takes its parameters; its block holds its props,
its state and its views. What reads state is updated when it changes,
and only that: no diffing. A state made from what changes
(`state draft = name`) follows it until it's written; `peek(name)`
reads it once. `bind` makes a two-way binding, `on press`
handles an event, and `if` and `for` decide which views there are.
A `preview` is a component shown on its own, in the settings it names:
`asuka dev` shows it on the device in place of the app, and builds leave
it out.
[Components](language/components.md), [Stores](language/stores.md) for
state several components share.

## Async and errors

```asuka check
enum LoadError: Error {
  offline
  server(code: int)
}

async fn fetchCount(online: bool) throws -> int {
  if !online { throw LoadError.offline }
  await sleep(1s)
  return 42
}

component Badge {
  state count = 0
  state problem = ""

  task {
    do {
      count = try await fetchCount(online: true)
    } catch LoadError.offline {
      problem = "You're offline"
    } catch e {
      problem = e.message
    }
  }

  Text(if problem.isEmpty() { "{count}" } else { problem })
}
```

`async` work runs without holding the screen up, and `task { }` belongs
to the component that started it: it's cancelled when the component
goes. A function that can fail says `throws`, and its callers `try` it
(`try?` for none on an error). [Async](language/async.md),
[Errors](language/errors.md)

What a component shows from such a call is a `load`: `let n = load
fetchCount(online: true)` is `.pending`, `.ready(count)` or
`.failed(e)`, and keeps what it shows while it asks again.
`n.optimistic(f)` shows a change at once, `reload fetchCount` asks every
load of it again, wherever it is, and `load (a(), b())` lands several
calls together. [Data that loads](language/async.md#data-that-loads)

## Packages

```asuka check
import std/time

let today = time.now().format("EEE d MMM")
```

`pub` makes a declaration visible to other packages. The standard
library (`std/…`) and the platform SDK (`platform/…`) come with the
CLI: import a package and use it. A file named `tray_linux.asuka` is
part of its package only on Linux, and `@platform == .linux` is a
constant, so what's for one platform isn't in the others' builds;
`@debug`, true under `asuka dev`, leaves what's for development out of a
release.
[Files, packages and imports](language/packages.md), [the standard
library and the platform SDK](stdlib.md), [code for one
platform](language/control-flow.md#code-for-one-platform), [code for
development builds](language/control-flow.md#code-for-development-builds)

## Tests

```asuka check
component Counter {
  state count = 0
  Button("Add {count}") { on press { count += 1 } }
}

test "adding counts" {
  let app = mount(Counter())
  app.tap("Add 0")
  app.expect("Add 1")
}
```

`asuka test` runs a package's `test` blocks with no device: `mount`
shows a view, and the test taps, types and checks what it shows.
[Tests](language/tests.md)

## Next

- [The language reference](language/README.md): each part in full,
  from types and their methods to styles, navigation and native code.
- [Components](components.md): the elements apps are made of.
- [Concepts](concepts.md): what updates, owners, the main thread.
