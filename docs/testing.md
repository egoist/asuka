# Testing

An app's tests are `test` blocks beside its code. `mount` mounts a
component without a device, a window or a display, and the test drives
it as a user would, by what the screen shows:

```asuka
component Counter {
  state count = 0
  Column {
    Text("Count: {count}")
    Button("Add") { on press { count += 1 } }
  }
}

test "Add counts" {
  let app = mount(Counter())
  app.tap("Add")
  app.tap("Add")
  app.expect("Count: 2")
}
```

```sh
asuka test                    # the app or package in this directory
asuka test --run Add          # the tests whose names contain Add
asuka test path/to/app
```

```
ok   Add counts
FAIL Remove empties the list (app.asuka:41)
     the screen doesn't show "No items"
     View 0,0 390x844
       Text 0,0 390x20 "Item 1"
asuka: 1 of 2 tests failed
```

It runs anywhere the CLI does, in CI too, with no simulator or emulator:
`asuka test` compiles the package with its tests and runs them on this
computer; the first run builds the test runner, which needs a C
compiler. It exits with 1 when a test fails. For the
platforms' real views, see [testing on the
platforms](#testing-on-the-platforms) below.

`asuka run file.asuka` compiles one file and runs it the same way: its
tests, if it has any, then its app, printing what its screen shows, for
trying out a component or a function without making an app.

## How it works

`mount` mounts the view on a stand-in for the platform, which keeps the
views the app makes instead of making the platform's. Everything else
is your app as it runs: its state, effects and tasks, Asuka's layout,
navigation, lists binding their rows, sheets and
alerts.

The app runs as on an iPhone (iOS, 390 × 844 points). `asuka test
--platform android` (or `mac`, `windows`, `linux`, `web`) says it's
another platform, for what the app does by platform: the tests are
compiled for it too, with its packages' files for that platform and
`@platform` its own. The tests are a development build, as `asuka dev`'s
is: [`@debug`](language/control-flow.md#code-for-development-builds) is
true, and `asuka test --release` makes it false, as in what you ship.

What isn't the platform's is how text is measured: 8 points a
character on 20-point lines, whatever the font. Frames come from
Asuka's real layout, so where views are relative to each other holds;
their sizes to the point don't.

Time moves only when nothing else can run: a `sleep(5s)` in a test, or
a task's timer, takes no time, and a test that waits for something that
never comes fails at once ("the test never finished").

What the core reports as the app runs (a load that waited for another
for nothing, a long hold with no sign of it: [Errors and their
codes](development.md#errors-and-their-codes)) `asuka test` prints
where it happens, with its code; `asuka test --json` writes it, and each
test's result (with a property test's failing input), each bench's and
each file's coverage, as a line of JSON.

Beyond tests of examples, `asuka test` runs [property
tests](#property-tests) with inputs it generates, [fuzzes](#fuzzing)
them, compares values with [snapshots](#snapshots), times
[benchmarks](#benchmarks) and measures [coverage](#coverage), for an
app, a package or a program alike.

## What the screen shows

Tests name things by the text a user sees: a text, a button's title, a
field's text or placeholder, an accessibility label, a screen's title,
a tab, an alert's button. Text matches exactly.

```asuka
app.expect("Inbox")       // fails the test unless it shows
app.expectNo("Message 1") // fails it if it does
print(app.snapshot())     // what the screen shows, a view a line
```

Only what a user could see counts:

- the top screen of each stack (the screens under it don't show);
- the selected tab's page, and every tab's title (and badge) in the tab bar;
- a split's sidebar: each destination's title and badge;
- the sheets, alerts and popovers presented, over the rest;
- a list's rows that fit its height: Asuka binds a list's rows as the
  platform asks for them, and so does `asuka test`, as a collection
  view would;
- nothing hidden.

When a check fails, the message has the screen: each view that shows,
with its kind, frame and texts, so you can see what was there instead,
and `asuka test` says where the test is (`app.asuka:41`).

## Doing what a user does

Each action settles before it returns: what it changed is laid out,
lists are bound, and what the app's tasks posted has run.

```asuka
app.tap("Add")                      // a button, a row, a tab, a toggle, a segment, a menu's item
app.type("ada@example.com", into: "Email")  // the field with that placeholder or label
app.type("milk", into: "Search notes")      // a screen's search field, by its placeholder
app.tapLink("https://asuka.dev")     // the link in a RichText
app.share([SharedItem(kind: .link, text: "https://asuka.dev")])  // another app shares it: onShare hears it
await app.idle()                    // until nothing is left to run: tasks, timers, loads
```

A tap goes where a finger would. A text in a row taps the row, and a
text in a button taps the button. A view with a menu opens it, and the
next `tap` chooses from it.

## Property tests

A test with parameters is a property test: it says what holds for any
values of its parameters' types, and `asuka test` runs it with values it
generates.

```asuka check
struct Span {
  start: int
  end: int
}

fn merge(a: Span, b: Span) -> Span {
  return Span(start: min(a.start, b.start), end: max(a.end, b.end))
}

test "a merge covers both spans" (a: Span, b: Span) {
  let m = merge(a, b)
  expect(m.start <= a.start && m.start <= b.start)
  expect(m.end >= a.end && m.end >= b.end)
}

test "reversing twice gives the list back" (xs: [int], s: string) {
  expect(xs.reversed().reversed() == xs)
  expect(s.reversed().reversed() == s)
}
```

Each runs with 100 inputs. When one fails, `asuka test` shrinks it: it
tries simpler ones (shorter lists and strings, smaller numbers, `none`,
an enum's first case) and keeps the simplest that still fails, which it
prints with the seed that found it:

```
FAIL a page holds two rows at most (pages.asuka:9)
     expected page.count() <= 2 (pages.asuka:10): got 3 and 2
     input (case 4 of seed 0, shrunk in 3 steps):
       rows = ["", "", ""]
     rerun it: asuka test --run 'a page holds two rows at most' --seed 0
```

The inputs come from a seed, 0 unless `--seed` says another, so every
run (a CI's too) tries the same ones and a failure happens again;
`--seed random` tries new ones and prints its seed, and `--cases 1000`
tries more. What it generates:

| Type | Values |
| --- | --- |
| `int`, `Duration` | small numbers most often, then up to ±1000, the limits (0, ±1, 255, 65535, the 32-bit and 64-bit ones), any |
| `float` | whole numbers, fractions, any finite value, and ±0, the infinities and NaN |
| `bool` | both |
| `string` | letters most often, then any of ASCII, other scripts, emoji, combining marks, NUL, line breaks, quotes and braces; up to 64 characters |
| `bytes`, `[T]`, `Set<T>`, `[string: T]` | up to 64 elements, few most often |
| `T?` | `none`, or a `T` |
| structs, enums | their fields' values; each case, with its values; a type that holds itself stops nesting |

A parameter is data: a view, a closure or a store has no values to
generate ([test-parameter](errors/test-parameter.md)). Make the rest in
the test, from what it takes, and return early from an input the
property isn't about: `if xs.isEmpty() { return }`.

## Fuzzing

```sh
asuka test --fuzz                       # each property test, 10 seconds
asuka test --fuzz --run parse --fuzztime 5m
asuka test --fuzz --fuzztime 100000x    # 100,000 inputs each
```

`--fuzz` runs the property tests, each with new inputs until
`--fuzztime` is up: a time, or a number of inputs. It sees which of the
package's code each input reaches, and keeps changing those that reached
code no other did, so it finds what random inputs rarely would: a
parser's deep branches, a format's corner cases. An input that fails is
shrunk, printed, and saved in `testdata/fuzz/<test>/`, as JSON:

```json
{
  "s": "abcd"
}
```

Commit it: every `asuka test` runs a test's saved inputs before the ones
it generates, so what was fixed stays fixed. You can write them too: a
JSON object of the test's parameters by name (a float JSON has no
number for is `NaN`, `Infinity` or `-Infinity`).

## Snapshots

`expectSnapshot(value)` compares what a value prints as with the
snapshot kept for it, and fails the test when they differ. The first
run writes it, in `testdata/snapshots/` (a file for each source file's
tests), which you commit:

```asuka
test "the list shows today's todos" {
  expectSnapshot(store.today())
  let app = mount(TodoList())
  expectSnapshot(app.snapshot())   // what the screen shows
}
```

```
snapshot "the list shows today's todos 1"
  [
    Todo(title: "Buy milk", done: false, tags: ["errands"]),
    Todo(title: "Call Ada", done: true, tags: []),
  ]
```

`expectInlineSnapshot(value)` keeps it in the call instead: the first
run writes it there, as `asuka fmt` would.

```asuka
expectInlineSnapshot(total(cart))
// after the first run:
expectInlineSnapshot(total(cart), "42.5")
```

A string is its own snapshot, as it is. Any other value prints as the
language writes it: `"a"` with its escapes, `1.0`, `1500ms`, `[1, 2]`,
`["a": 1]`, `Set([1])`, `Todo(title: "Milk", done: false)`,
`.circle(radius: 2.0)`, `none`; a map's entries and a set's elements
sorted, so the order they were added in doesn't matter; and an item a
line when they don't fit on one.

When a value differs from its snapshot, the test fails and says how:

```
FAIL the list shows today's todos (todos.asuka:12)
     expectSnapshot (todos.asuka:13): what it shows isn't its snapshot (asuka test --update writes what it shows)
     - the snapshot
     + what it shows
       [
     -   Todo(title: "Buy milk", done: false, tags: ["errands"]),
     +   Todo(title: "Buy oat milk", done: false, tags: ["errands"]),
         Todo(title: "Call Ada", done: true, tags: []),
       ]
```

`asuka test --update` (`-u`) writes what the tests show instead, and,
when every test ran and passed, removes the snapshots no test takes any
more. In CI (where `CI` is set), a snapshot that isn't stored yet fails
the test rather than being written. A property test can't take a
snapshot: it runs with every value it generates.

## Benchmarks

A `bench` is a benchmark: what it does, timed.

```asuka check
fn fib(n: int) -> int {
  if n < 2 { return n }
  return fib(n - 1) + fib(n - 2)
}

bench "fib 20" {
  keep(fib(20))
}

bench "sorting 1,000 ints" {
  var xs: [int] = []
  for i in 0..<1000 {
    xs.push(i * 7919 % 1000)
  }
  measure {
    keep(xs.sorted())
  }
}
```

```sh
asuka test --bench                 # every bench
asuka test --bench --run sort      # those whose names contain sort
```

```
bench fib 20                    109µs/op ±2%          0 allocs/op        0 B/op  (10 × 520)
bench sorting 1,000 ints       14.7µs/op ±4%          3 allocs/op    15.7 KB/op  (10 × 3,964)
```

`--bench` runs each bench's body many times: once, then more each run,
until a run takes a tenth of `--benchtime` (1s); then ten runs of that
many, and it prints their median time for one, how far the fastest and
slowest are from it, and what one allocates: objects and bytes. What
comes before `measure { … }` sets up and isn't timed; without it, the
whole body is. `keep(x)` is `x`: give it what the work computes, or the
compiler may leave out work whose result nothing reads.

Benches run compiled to C, as a release build is (`@debug` is false), so
they time what your app ships; `--vm` runs them on the VM that `asuka dev` uses. The
time is measured, never checked: a bench fails only on what it checks
(`expect`) or an error, and a plain `asuka test` runs each once, as a
test, to see that it still works. `--benchtime 100x` runs exactly 100
iterations, once.

A bench that mounts a view also says what reached the screen in each
iteration, which a real device would have to do: the batches of changes
sent to the platform, their operations and bytes, the views the
platform measured, and the views laid out. Drive the app as a user
does (`app.tap`) or from a frame (`app.frame`, with `onFrame` in the
app), so that one iteration's changes are one batch, as a tap's are:

```asuka
bench "a new row" {
  let app = mount(Inbox())
  measure { app.tap("Add") }
}
```

```
bench a new row                 41.2µs/op ±3%         84 allocs/op     6.1 KB/op  (10 × 2,410)  host: 1 batches, 9 ops, 312 B, 2 measures; 6 nodes laid out
```

In a bench, `app.ops()` doesn't list what the timed part sent.

To see what a change does, keep a run and compare:

```sh
asuka test --bench --json > before.json
# … change the code …
asuka test --bench --compare before.json
```

Each line then ends with the time before and the change (`was
24.1µs/op: -15.8%`), and says "within noise" when the two runs' ranges
overlap.

## Coverage

```sh
asuka test --cover                            # a summary per file
asuka test --cover-lcov coverage/lcov.info    # for editors and CI
asuka test --cover-html coverage.html         # each file, what ran
```

```
coverage: 85.7% of lines (12 of 14), 75.0% of branches (6 of 8)
  todos.asuka   85.7%  lines 12/14  branches 6/8  not run: 31, 40-41
```

`--cover` counts which lines of the package's code run as its tests do
(its own code: not its tests' bodies, nor what it imports), and each
branch: an `if`'s arms (the `else` it leaves out too) and a `match`'s.
A line ran when a statement on it ran; its branches say what in it
didn't. A function nothing calls never ran. A project's programs
(`[bin.<name>]`) are measured by their own tests.

`--cover-lcov` writes lcov's format, which editors (VS Code's Coverage
Gutters) and CI services (Codecov, Coveralls) read; `--cover-html`
writes a page of the source, what ran in green and what didn't in red.
Without `--cover`, nothing is counted.

## Platform services and native packages

A [native package](native.md)'s interface is the platform's, which
`asuka test` hasn't: its calls throw, as on a platform without the
service. Give your components what they use through a store or a
`provide`, and the test its own:

```asuka
test "archiving removes the row" {
  let app = mount(Inbox(), provide: Mailbox.fake([sample]))
  await app.idle()
  app.tap("Archive")
  app.expectNo("Hello")
}
```

`provide:` takes a store, or a list of them for a view that uses
several: `mount(Inbox(), provide: [Mailbox.fake([sample]), Session()])`.

## Testing on the platforms

`asuka test` tests your app's behavior: what shows, and what happens
when the user acts. How each platform draws it is the platform's. To
see your app on the platforms:

- `asuka dev --platform ios|android|web` runs it on the simulator, an
  emulator or a phone, or in a browser, and reloads it as you save.
- `asuka test --gui` runs the app's flows there, a user's steps
  through it: see [GUI tests](#gui-tests-flows).
- On a desktop, `ASUKA_SNAPSHOT=out.png` writes the window to a PNG
  once it has shown, without taking the focus, then quits
  (`ASUKA_DUMP=1` writes its view tree beside it). `ASUKA_SCRIPT`
  runs steps first (`tap:Add`, `type:hello`):
  see [Development](development.md#snapshots-and-scripts).
- On Linux, under Xvfb (`xvfb-run -a dbus-run-session`), that works
  with no display, in CI.

## GUI tests: flows

A flow is a user's steps through the app, run on a platform's player:
the simulator, an emulator, a phone, headless Chrome, or this computer.
Each is a file in the app's `flows` folder, with one step a line:

```
# flows/note.flow: a message's note, written in a sheet.
tap Message 20
see No note yet.
tap Add a note
type hello
tap Save
see Note: hello
back
notsee Note: hello
```

`asuka test --gui` runs them as `asuka dev` runs the app, each from a
fresh start with every `state` at its initial value, and says how each
went:

```
asuka test --gui --platform ios       # the simulator (--device picks one)
asuka test --gui --platform android   # an emulator or a phone
asuka test --gui --platform web       # headless Chrome
asuka test --gui                      # this computer
```

| Step | What it does |
|---|---|
| `tap <text>` | Presses what shows the text: a button, a toggle, a view that takes presses (a row), or a text field, which then takes what's typed. |
| `type <text>` | Types into the text field tapped last (else the first one), as the user's typing does. |
| `submit` | Presses return in that field. |
| `back` | Goes back from the top screen, as its back button does. |
| `see <text>` | Something on the screen shows the text, all of it or as part of a longer one. |
| `notsee <text>` | Nothing on the screen shows it. |
| `wait <time>` | Waits (`500ms`, `2s`). |

A step that fails is tried again until it passes or `--timeout` (10s)
runs out, since the app may still be loading or waiting on the network.
Then the flow fails, with the step's line and what the screen shows. A
screen under the top one doesn't count as showing. `--run text` runs
only the flows whose names contain the text. The steps go through
Asuka's own views, as the platform's events would come, so a flow runs
the same everywhere, and what it shows is the platform's widgets.
