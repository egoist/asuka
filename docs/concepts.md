# Concepts

An Asuka app is components that declare what they show, and state that
says what changes. This page covers how they fit: the app, components
and their state, what updates when state changes, effects and tasks,
showing and repeating views, owners and cleanup, the main thread, and
the environment. [The Asuka language](language-tour.md) is the reference for
the syntax.

## An app

`app { … }` is the app's root:

```asuka
component Greeting(name: string) {
  Text("Hello, {name}")
}

app {
  SafeArea {
    padding: 16
    gap: 12
    Text("Hello") { font: .largeTitle }
    Greeting("Ada")
  }
}
```

The root fills the screen on a phone, under the notch and the home
indicator, and the window on a desktop; `SafeArea` keeps its content
clear of what covers it, and of the keyboard (see [Layout and
style](layout.md#the-safe-area-the-keyboard-and-the-window)).

How it starts depends on the platform:

- **iOS and Android**: the platform starts the app, which mounts the
  root.
- **macOS, Windows and Linux**: the app opens its main window, mounts
  the root in it, and runs until the app quits.
- **The web**: the page loads the app, which mounts the root in the
  page.

Under `asuka dev`, each save reaches the running app in milliseconds,
keeping its state and its views.

## Components

A component is a piece of interface. Its header takes its parameters;
its body declares its props, events and state, and its views:

```asuka
component Counter(label: string) {
  prop step = 1
  event changed(count: int)
  state count = 0

  Row {
    gap: 12
    Text("{label}: {count}")
    Button("Add") {
      on press {
        count += step
        emit changed(count)
      }
    }
  }
}
```

A component's body is **declarations**, not code that runs once and is
forgotten. `Text("{label}: {count}")` says what the text is now and
later; there's no way to read `count` once by mistake and show a value
that never updates.

## State

`state` is a value the interface follows:

```asuka
state count = 0
state draft = ""
state todos: [Todo] = []
```

Writing it (`count += 1`, `todos.push(t)`, `todos[0].done = true`)
updates what reads it, and only that. A write of the value it already
has does nothing.

A state whose value reads what changes follows it: `state draft = title`
is `title` until it's written, and `title` again when `title` changes.
`state draft = peek(title)` takes `title` once, and `state page(query) =
1` goes back to 1 whenever `query` changes ([State that
follows](language/components.md#state-that-follows)).

State in a component is the component's: each instance has its own, and
it goes with the instance. `state` at the top of a file is the app's: one
for the whole app. A **store** groups state with the functions that
change it, for views that share it:

```asuka
pub store Cart {
  state items: [Item] = []
  let total = items.sum(i => i.price)

  pub fn add(i: Item) { items.push(i) }
}
```

### Derived values

`let` in a component, a store or a file derives a value from others:

```asuka
state first = "Ada"
state last = "Lovelace"
let full = "{first} {last}"
let left = todos.count(t => !t.done)
```

It's computed when it's read, and again only when something it read
changed; reading it twice costs once. What it reads is what the code
says: `todos.count(t => !t.done)` reads each item's `done` and how many
items there are, so renaming an item doesn't compute `left` again. A
`let` can't write state.

## What updates

Each prop that reads state is a binding of its own. When the state it
read changes, that binding runs again and sends that one prop to the
native view; nothing else runs.

```asuka
component Score {
  state score = 0

  Column {
    Text("Score: {score}")          // updates when score changes
    Text("Keep going")              // sent once
    Button("Point") { on press { score += 1 } }
  }
}
```

Pressing *Point* sets `score`. The first `Text`'s binding runs again
and sends its new text. The component doesn't run again, the second
`Text` isn't touched, and there's no tree to compare: the change goes
from the state to the one prop that reads it.

A binding follows as much of a state as it reads, and no more.
`Text(user.name)` follows the user's name: changing `user.avatar`
doesn't run it. `todos[1].done = true` runs what reads that `done` (and
what reads the whole list), not what reads another item or this item's
title; `todos.count()` follows how many there are, `cart[sku]` that
key, `picked.contains(id)` that member. A value read whole, passed to a
function or compared, is followed whole. Replacing a whole value
(`todos = fetched`) runs only what reads a part that's different.

Writes in a handler show together when it ends (or when it reaches an
`await`): three writes, one update.

## Effects and tasks

An `effect` runs code now, and again when what it read changes; with
values in parentheses, when they change, and only then:

```asuka
effect { storage.set("draft", draft) }
effect(selected) { analytics.log("selected", selected) }
```

An effect can't write state it follows (it would run again after its
own write): the compiler says so, and what says it instead.

What an effect sets up, a `cleanup { … }` in its body undoes. It runs
before the effect runs again, and when the effect goes with its
component:

```asuka
effect(roomId) {
  let room = roomId
  chat.join(room)
  cleanup { chat.leave(room) }
}
```

A cleanup reads state as it is when it runs, after what the effect
follows has changed. Keep the run's values in a `let`, as `room` is
here; the compiler warns when a cleanup reads what its effect follows.

A `task` is async work a component does while it's shown: it starts when
the component appears, and is cancelled when it goes. With values in
parentheses, it starts again when they change, cancelling the run
before. What's in the parentheses is compared: `task(user.id)` starts
again when the id changes, not when the user's name does.

```asuka
task { messages = try await api.inbox() }
task(query) {
  await sleep(300ms)
  results = try await api.search(query)
}
```

`load` keeps an async result as state, with its error and whether it's
loading. Once it has an answer, the answer stays on screen while the
next one loads (see [The Asuka
language](language/async.md#data-that-loads)).

## Showing and repeating

`if` shows views while a condition holds; `match` shows one of several;
`for … key` shows a row per item:

```asuka
if loggedIn {
  Profile(user)
} else {
  Button("Log in") { on press { present(Login()) } }
}

match status {
  .loading => Spinner()
  .failed(e) => Text(e.message)
  .ready(items) => ItemList(items)
}

for todo in todos key todo.id {
  TodoRow(todo)
}
```

A `for` keeps a row per key: a row whose item changed updates its
bindings, a new item makes a row, a removed one disposes its row, and a
row that moved is moved, not made again. A `for` over items that change
needs its `key`; a range, a set and a constant list key their rows by
what they are.

A `for` over a state follows the order of its items, not the items:
changing a field of one item runs neither the `for` nor the other rows,
only that row's bindings that read the field. A row's item, passed on
(`TodoRow(todo)`), stays that item's: `todo.title` in `TodoRow` follows
that title. Keying by a field of the item (`key todo.id`) is the
cheapest key: the `for` follows all of the keys as one.

### When something fails

An error a handler or a task doesn't catch goes to the nearest
`Boundary` around it, which shows what it says instead of its content:

```asuka
Boundary {
  Feed()
  on error(e) { Text("Something went wrong: {e.message}") }
}
```

With no `Boundary`, the app's own reports it: in development the player
shows it, and in a release it's logged.

### Long lists

`for` directly in a `List` builds only the rows on screen and reuses
them as they scroll, so a list of 100,000 costs what's visible. See
[Components](components.md#lists).

## Owners and cleanup

Everything a component makes (its state, bindings, effects and tasks)
belongs to an **owner**: the component instance's. Each `if` branch,
`for` row, `match` case, stack screen, tab and sheet has its own, under
its parent's. Disposing an owner stops its bindings and effects, cancels
its tasks, and disposes the owners under it.

Owners are disposed when:

- an `if` switches branches: the old branch's;
- a `for` item's key leaves the array: its row's;
- a `match` changes case: the old case's;
- a screen is popped (the back button, a swipe, `pop()`): the screen's;
- a sheet closes: its content's.

`cleanup { … }` in a component's body runs when its owner is disposed,
and one in an effect's body runs then too, as well as before each of the
effect's next runs. A store's and the app's body take one as well.
Cleanups run last registered first, after what their owner made has
stopped: the effects' cleanups first, then the body's. That's where to
undo what a component registered:

```asuka
component Tab(value: string) {
  use tabs: TabsContext
  effect { tabs.add(value) }
  cleanup { tabs.remove(value) }
  Text(value)
}
```

A task that's cancelled ends at the `await` it's waiting at, running
its `defer`s on the way: that's where to stop what it started.

```asuka
component Clock {
  state now = time.now()

  task {
    let ticker = time.ticker(1s)
    defer { ticker.stop() }
    for await t in ticker.ticks { now = t }
  }

  Text(now.format("HH:mm:ss")) { font: .title.semibold() }
}
```

Shown with `if showClock { Clock() }`, the ticker stops when the clock
is hidden, and a new one starts when it's shown again.

## The main thread

Components, handlers, effects, tasks and every state write run on the
platform's main thread: the UI thread on iOS and Android, the process's
first thread on macOS, Windows and Linux, and the page's thread in a
browser. Don't hold it up: while it's busy, the app can't draw, scroll
or take input.

Work that waits (the network, a file, a timer) is `async`: `await` lets
the main thread run while it waits, and the code after it runs there
again, so it can write state.

```asuka
on press {
  loading = true
  defer { loading = false }
  do {
    let page = try await http.get("https://example.com").text()
    status = "{page.count()} characters"
  } catch e {
    status = e.message
  }
}
```

Work that computes for long (parsing, image processing) goes in
`spawn { … }`, which runs it on another thread. It may use values, not
state or views (the compiler says so if it does), and its result comes
back to the main thread with `await`:

```asuka
let html = await spawn { markdown.render(source) }
```

## The environment

`environment()` is what the platform says about where a component
shows: read it in a binding or a `let`, and it follows rotation, the
keyboard, dark mode and the text size. It's the environment of the
screen the component is in (a stack screen, a tab, a sheet, or the
root).

```asuka
let landscape = environment().width > environment().height
```

Its fields:

- `width`, `height`: the screen's size, in points. It's the app's
  window, not the device: in split screen (iPad's Split View, Stage
  Manager, Android's split screen) it's the app's part, and on a
  desktop or in a browser it's the window's content; it changes as the
  user resizes it.
- `safeTop`, `safeRight`, `safeBottom`, `safeLeft`: the safe area's
  insets. A desktop window has none: they're 0.
- `keyboard`: how much of the bottom the on-screen keyboard covers.
- `scale`: pixels per point: 2 or 3 on a phone, 2 on a Retina Mac, the
  display's scaling on Windows and Linux (1.5 at 150%).
- `fontScale`: the user's text size (Dynamic Type, Android's font scale,
  Windows' text size, GNOME's text scaling); 1 is the default.
- `dark`: dark mode is on.
- `reduceMotion`: the user asked for less motion. See
  [Motion](motion.md).
- `screenReader`: VoiceOver, TalkBack, Narrator or Orca is on. See
  [Accessibility](accessibility.md#is-a-screen-reader-on).
- `widthClass`, `heightClass`: how much room the window gives the app
  along each axis, as the platform classes it: `.compact`, `.medium` or
  `.expanded`. Branch on them, not on a width of your own, to put a
  sidebar beside the content or a list beside its detail. iOS reports
  UIKit's size classes, compact or regular (as expanded); Android,
  Material's window size classes. macOS, Windows, Linux and the web take
  Material's from the window's size.
- `locale`, `languages`, `timeZone`: the user's regional formats, the
  languages they read, and their time zone. Most apps read them through
  [their catalogs and formats](i18n.md).
- `platform`: the system the app runs on: `.iOS`, `.android`, `.macOS`,
  `.windows`, `.linux` or `.web`.

Semantic colors and fonts follow dark mode and the text size on their
own; read the environment for what they don't cover.

```asuka
if environment().widthClass == .expanded {
  Row { grow: 1; Sidebar(); Detail() }
} else {
  ItemList()
}
```

The platform's views look as they should by themselves; branch on
`platform` for what an app draws itself, to follow each platform's
design, as [`examples/usage`](../examples/usage) draws UIKit's inset
grouped lists on iOS and Material 3's on Android.

## Values that persist

State lives as long as the app runs. The store
(`import std/store`) keeps small values, such as settings, across
launches: `read` gives the kept value (or a default), `write` keeps one.
An effect keeps the state's every value:

```asuka
import std/store

state units = store.read("units", "metric")
effect { store.write("units", units) }
```

Values are kept as JSON in the platform's own preferences, so numbers,
strings, arrays and structs all work. See [Data](data.md) for the store and for databases. Under
`asuka dev`, every state's value is also kept across saves, with no
code (see [Getting started](getting-started.md#run-it)).
