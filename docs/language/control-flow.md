# Control flow

`if` and `match` are values too. As one, an `if` needs an `else`, and a
block's value is its last expression:

```asuka
let label = if unread > 0 { "{unread} new" } else { "Inbox" }
let icon = match (online, unread) {
  (false, _) => "wifi.slash"
  (true, 0) => "tray"
  _ => "tray.full"
}
```

A branch whose value says nothing of its type, an empty `[]` or `[:]`
or an implicit `.case`, takes the other branch's: `if let c = chosen {
[c] } else { [] }` is a `[Story]`.

A tuple pattern (`(true, 0)`) matches each element with a pattern of
its own. Matching an optional tuple, it matches the tuple when there's
one, as a case pattern does (`none` is an arm of its own): `if let
(name, age) = found { … }`, `let (x, y) = cursor else { return }`.

A name with a type (`c: Circle`) tests what a value of an interface
is: it matches when the value is of that type, and binds it as one, as
a struct's pattern (`Circle(radius)`) and an enum's case written with
its type (`Unit.big(n)`) do: `if let c: Circle = shape { … }`,
`let sq: Square = shape else { return }`. A `match` over an interface
ends with `_`; see [Interfaces and
generics](functions.md#interfaces-and-generics).

`for` goes over an array, a set, a map's `(key, value)` pairs, a
string's chars (`for c in name`: [Values and types](types.md#values-and-types)), a
lazy sequence (a `Seq`: [Values and types](types.md#lazy-sequences)), or a
range:
`a..<b` is the ints from `a` up to `b`, without it, and `a..b` with it
(`0..<3` is `[0, 1, 2]`, and a `for` over a range makes no array).
`while cond { … }` loops while cond holds; `break` and `continue` leave
the loop, or go on to its next turn.

`for` also goes over a value of your own types: an iterator, whose type
has `mut fn next() -> T?` (`for` calls it on a copy, until it gives
`none`), or a sequence, whose `fn iter()` gives an iterator or a `Seq`
(`for` calls it once). The prelude's `Iterator<T>` names such a type,
for constraints: `fn sum<I: Iterator<int>>(xs: I) -> int`. An
iterator's `none` is its end, so one whose elements may be none (a
`T?`) gives a `Seq` instead: `fn iter() -> Seq<int?> = slots.lazy()`.

```asuka check
struct Node {
  name: string
  children: [Node] = []
}

struct Walk {
  stack: [Node]

  mut fn next() -> Node? {
    let n = stack.pop() else { return none }
    stack = stack + n.children.reversed()
    return n
  }
}

extend Node {
  fn walk() -> Walk = Walk(stack: [self])
}

fn count(root: Node) -> int {
  var n = 0
  for node in root.walk() { if node.name.hasSuffix(".asuka") { n += 1 } }
  return n
}
```

`as name` after a `for`'s or a `while`'s head names the loop: `break
name` and `continue name` leave it, or go on to its next turn, from any
loop inside it (the `defer`s of every loop they leave run). Not from a
closure; and a name that's declared and never used, or used and not
declared, is an error.

```asuka
for row in grid as rows {
  for cell in row {
    if cell == target { found = true; break rows }
    if cell.isEmpty() { continue rows }
  }
}
```

`if let` and `while let` take an optional's value, or match a pattern,
and run their block when there's one: `if let u = user { … }`, `while
let job = queue.pop() { … }`, `if let .ready(v) = state { … }`. Their
condition may be a chain: lets and bools, separated by commas, in any
order. Each sees what the ones before it bound, and the block runs
when all of them hold (they're tried in order, up to the first that
doesn't):

```asuka
if let user = session.user, user.isAdmin, let team = teams[user.teamId] {
  Text("{team.name}'s admin")
} else {
  Text("Not an admin")
}
```

`if var` is `if let` whose name is a copy that may change, for code that
changes several fields and writes them back: `if var m = draft { m.read
= true; m.tags.push("seen"); draft = m }`.

`let … else` binds a value or leaves. Its `else` block runs when the
optional is none (or the pattern doesn't match), and must leave:
`return`, `throw`, `break`, `continue` or `panic(…)` (the compiler says
so when it can go on). After it, the name holds the value, not an
optional:

```asuka
fn summary(id: string) -> string {
  let message = cache[id] else { return "(deleted)" }
  let .ready(body) = message.body else { return message.subject }
  return "{message.subject}: {body}"
}
```

`throw`, `return`, `break` and `continue` may go where a value does:
they leave instead of giving one (their type is `never`, which fits
any). After `??`, they say what to do with none:

```asuka
let user = cache[id] ?? throw MailError.missing(id)
for line in lines {
  let n = line.toInt() ?? continue
  total += n
}
```

They go likewise in an `if`'s or a `match` arm's branch, and as a
closure's body: `x => x ?? throw Error("empty")`.

## Changing what may not be there

A `?.` chain that starts at a `var` or state, through fields,
subscripts and `?.`, may be assigned (`=`, `+=`, …) and `mut`-called.
When it meets `none`, nothing happens: nothing is written (the value on
the right isn't even computed), and state's readers aren't told.

```asuka
inbox[id]?.read = true          // nothing when there's no such message
session.user?.name = draft
todos.first()?.toggle()          // an error: first() gives a value, not a place
```

`m[k, default: v]` is a map's entry, or `v` when it has none. Read, it's
a `V` (not a `V?`); written (`=`, `+=`, a `mut fn`, `push`), it writes
`m[k]`, and `v` is computed only when the entry is missing. Counting and
grouping are one line each:

```asuka check
fn tally(words: [string]) -> ([string: int], [int: [string]]) {
  var counts: [string: int] = [:]
  var byLength: [int: [string]] = [:]
  for w in words {
    counts[w, default: 0] += 1
    byLength[w.count(), default: []].push(w)
  }
  return (counts, byLength)
}
```

## Code for one platform

`@platform` is the platform the app is built for, a `Platform`:
`.iOS`, `.android`, `.macOS`, `.windows`, `.linux` or `.web`. It's a
constant, known as the app compiles: it isn't tracked, and it goes
wherever a value does, a file's top level and a component's body
included. A branch it decides that a platform doesn't take is no part
of that platform's build: not its code, not its strings, not its
views.

```asuka check
import platform/device

let saveKey = if @platform == .macOS { "⌘S" } else { "Ctrl+S" }

component Toolbar {
  prop link: string

  Row {
    if @platform == .iOS || @platform == .android {
      Button("Share") { on press { device.share(link) } }
    }
    match @platform {
      .macOS | .windows | .linux => Text("Save with {saveKey}")
      _ => Text("Saved as you go")
    }
  }
}
```

That's so of an `if` and its `else if`s, of a `match @platform` (which
needn't cover `.unknown`: it's never that), of `&&`, `||` and `!` over
`@platform`'s comparisons, and of an `if` among views, whose other
branch's views never exist. Every branch is still checked, whichever
platform the app is built for: a platform's APIs are the same
declarations on every platform, so a mistake in the Linux branch is an
error when you build for iOS too. For declarations of a platform's
own, a file of its own is often clearer: see [Files for one
platform](packages.md#files-for-one-platform).

`environment().platform` is the platform a view runs on, as the system
says it (and as a test's `mount(…, platform:)` does): it's tracked, for
views. What to build for a platform is `@platform`'s. An Apple TV app
is built from the iOS app (`@platform` is `.iOS` there): a TV is
`environment().tv`'s to tell ([TVs](../tv.md)); an Apple Watch app
too, and a watch is `environment().watch`'s ([Apple
Watch](../watch.md)).

## Code for development builds

`@debug` is whether the app is a development build, a `bool`: true
under `asuka dev`, `asuka run` and `asuka test`, false in what `asuka
build` and `asuka publish` make. It's a constant too, folded as
`@platform` is: what a release doesn't take, a debug menu, a staging
server's address, logging you need only while you work, is no part of
it.

```asuka check
let server = if @debug { "https://staging.example.com" } else { "https://example.com" }

component About {
  prop version: string

  Column {
    Text("Version {version}")
    if @debug && @platform == .iOS {
      Text("Development build, talking to {server}")
    }
  }
}
```

`!@debug` folds as well, and so do `&&` and `||` of it with `@platform`'s
comparisons. What follows a branch that's taken and returns is left out
too: after `if !@debug { return cached }`, the rest of the block isn't
in a release. That's so of `@platform`'s branches as well. Both branches are checked in every build, so the release
branch can't break unseen while you develop in the other one.

Tests are a development build, as `asuka dev`'s is: `asuka test
--release` runs them with `@debug` false, as the app ships, and so does
`asuka run --release` for a program. Benches (`asuka test --bench`)
time what a release runs, with `@debug` false.
