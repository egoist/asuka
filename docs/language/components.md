# Components

A component is a piece of interface, declared with its parameters in
its header and everything else in its body:

```asuka
component MessageRow(message: Message) {
  prop compact = false            // a named prop, with its default
  prop accessory: View? = none    // a slot: a view the caller passes
  event archive                   // something it reports
  state expanded = false          // its own state

  Menu {
    longPress: true               // a context menu: a long press, a secondary click
    actions: [.button(title: "Archive", role: .destructive, run: () => { emit archive })]
    Row {
      gap: 12; padding: [12, 16]
      Avatar(message.from)
      Column {
        grow: 1
        Text(message.subject) { font: .headline; lines: 1 }
        if !compact { Text(message.preview()) { color: .secondaryLabel } }
      }
      accessory
    }
  }
}
```

Using it looks the same as using a built-in element. Header parameters
go in the parentheses. Everything else goes in the block:

- props: `name: value`
- handlers: `on name { … }`
- two-way bindings: `bind name: target`
- styles: `style: card`, or a list, `style: [card, pink]`
- children

```asuka
MessageRow(m) {
  compact: true
  accessory: Badge(3)
  on archive { mailbox.archive(m.id) }
}
```

Each thing has one place: a header parameter can't be set in the block,
and a prop can't be passed in the parentheses.

A component that takes children declares `prop children: View`; what's
in its caller's block goes there. A named slot is `name { … }` in the
block (`toolbar { … }`).

`emit name(args)` raises an event; the caller's `on name(args) { }`
handles it. An event nobody handles does nothing; its name read as a
value is whether the caller handles it (`if click { … }`: a tray icon
whose click opens its menu unless the caller takes it).

A handler that awaits belongs to the component that declares it, the
one that receives the event. It runs until it finishes or that
component goes, whoever emitted the event and whatever becomes of the
emitter: a sheet's `on done { shown = false; try await save(draft) }`
closes the sheet and goes on saving. It runs where it's declared (a
`push` reaches that place's stack, and an error it lets out goes to the
`Boundary` around it there). An `emit` in an async function waits for
the handler to finish and gets its error; elsewhere (a handler that
doesn't await, a plain `fn`), `emit` starts it and goes on. Cancelling
the code that's waiting stops the wait, not the handler.

A component that shows one element (its body's views are that one
element, or another component that does) takes the style props too:
what its caller gives goes on that element, over what the component
sets there.

```asuka
component Card(title: string) {
  Column {
    padding: 16; radius: 12; background: .secondaryBackground
    Text(title)
  }
}

Card("Plan") { padding: 8; width: 240 }   // padding 8, radius 12
```

## Variants a component declares

A component that shows one element can name its states for its caller
to style: `variant name = condition`, a bool that's kept up to date like
a `let`. Its caller writes `name { … }` in its block, style props laid
over that element while the condition holds, as `pressed { … }` is laid
over a `View` while it's pressed:

```asuka
pub component Checkbox {
  prop checked = false
  event checkedChange(checked: bool)
  prop disabled = false
  prop children: View

  pub variant checked = checked
  pub variant disabled = disabled

  View {
    role: .button
    on press { if !disabled { checked = !checked } }
    children
  }
}

Checkbox {
  bind checked: agree
  padding: 4; radius: 4; border: 1
  checked { background: .accent; borderColor: .accent }
  disabled { opacity: 0.4 }
  Text("I agree")
}
```

- A variant's name is its own: it may be a prop's or a state's, as
  `checked` is here. It can't be an interaction state's (`pressed`) or
  a keyword's.
- The variants are laid over everything else on the element: its own
  style, its caller's, and the interaction states. Of two that hold, the
  one declared later wins (`disabled` over `checked` here).
- A `pub variant` is for another package's callers to style; one that
  isn't stays its own package's.
- A style may hold a variant's block too (`style box { checked { … }
  }`): applied to an element whose component declares the variant, the
  block follows it; anywhere else it does nothing.
- The caller's block of a component takes the interaction states
  (`pressed { … }`, `hovered { … }`) when the element it shows is a
  container.
- `Checkbox as box` names the view of the element it shows: `box.checked`
  reads the variant, tracked, as `card.hovered` reads a state.

## A component's body is declarations

A component's body is **not** code that runs once and is forgotten.
Each line declares something that stays true:

- `state x = …` is state the component owns. When its value reads what
  changes, it follows that ([State that follows](#state-that-follows)).
- `let y = …` is derived from what it reads, and kept up to date
  (computed only when read, and only again when what it read changed).
- Every prop value of every element is kept up to date the same way:
  `Text("Count: {count}")` changes when `count` does, and nothing else
  does.
- `if`, `for` and `match` show what their condition says, now and
  later.

When `count` changes, the only thing that runs again is what read it:
that `Text`'s text, and the `if`. No component runs again, and the
platform's views are touched only where something changed.

## State that follows

A state's value says what it starts as. When that value reads what
changes (a parameter, a prop, other state, a `let`), the state follows
it: it's that value until it's written, and that value again when what
it read changes.

```asuka
component Edit(title: string) {
  state draft = title          // title until it's typed in; title again when it changes
  state first = peek(title)    // title once: a seed

  TextField { bind text: draft }
}
```

`state x(a, b) = e` follows `a` and `b` only: `e` again when one of them
changes, whatever else `e` reads.

```asuka
let results = items.filter(i => i.name.contains(query))
state highlighted(results) = 0                  // back to the top when the results change
```

In its own value, a state's name is its current value, as an optional
(`none` the first time). That keeps a value while it still holds, and
replaces it when it doesn't; the state's type is written:

```asuka
// The selection stays while it's in the results, and moves to the
// first one when it isn't.
state selected: string? = if let s = selected, results.any(r => r.id == s) { s } else { results.first()?.id }
```

In one handler, a write and a change of what the state follows: the
later one wins. Read right after what it follows changed, it's already
the new value.

`peek(e)` is `e`, read without following it, wherever a value is
followed: `Text("{count} (from {peek(count)})")` updates when `count`
changes, and its second read stays as it was. Handlers, tasks, an
`effect(…)`'s body and tests follow nothing, so `peek` there is an
error.

## Handlers and statements

Code runs in handlers (`on press { … }`), functions, `task` and `effect`
blocks, and tests. There, reading state gives its current value, and
writing it updates what reads it:

```asuka
on press {
  count += 1
  todos.push(Todo(title: draft))
  todos[0].done = true      // writes todos[0].done: only what reads it (or todos[0], or todos whole) updates
  draft = ""
}
```

Writes in a handler show together when it ends (or when it reaches an
`await`).

Statements: `let`, `var`, `let … else`, assignments (`=`, `+= -= *= /=
%=`, `&= |= ^= <<= >>=`, `&+= &-= &*=`, `??=`), `if` and `if let`, `for x in xs` (an
array, a set), `for (k, v) in m`, `for (i, x) in xs.enumerated()`,
`while` and `while let`, `match`, `return`, `break`, `continue`,
`throw`, `do { } catch { }`, `defer { }`, `emit`
([Control flow](control-flow.md#control-flow)), and `fn` declarations ([Functions and
closures](functions.md#functions-and-closures)).

## Two-way bindings

Controls show a value and change it. `bind` passes them state to do both:

```asuka
state query = ""
state notify = true

TextField { bind text: query; placeholder: "Search" }
Toggle { bind on: notify; title: "Notifications" }
```

Without `bind`, a control only reports: `TextField { text: name; on
change(t) { name = t.trim() } }`.

A component's prop can be bound the same way when the component reports
it with `event <prop>Change`: `bind value: when` on a component with
`prop value` and `event valueChange(value: T)` passes the state in, and
writes it as the component emits the change:

```asuka check
component Rating {
  prop value = 0
  event valueChange(value: int)

  Stepper {
    value: float(value)
    on change(v) { emit valueChange(int(v)) }
  }
}

component Review {
  state stars = 3

  Rating { bind value: stars }
}
```

A component may also assign such a prop, as if it were its own state,
in its handlers, fns, effects and tasks (and through an element's
`bind`). What the assignment does depends on its caller:

- Bound (`bind open: shown`): the assignment emits `openChange`, which
  writes the caller's state; the prop goes on reading that state. A
  caller that doesn't write it keeps the prop as it was.
- Not bound (a value, or nothing): the value is the component's own
  from then on, and `openChange` is still reported to a caller that
  handles it. When the caller's value changes, it replaces the
  component's.

```asuka
component Disclosure(title: string) {
  prop open = false
  event openChange(open: bool)
  prop children: View

  Column {
    Button(title) { on press { open = !open } }
    if open { children }
  }
}

Disclosure("Details") { Text("…") }                  // its own
Disclosure("Details") { bind open: shown; Text("…") } // the caller's
```

A prop changes whole: `open = …`, or `items = items + [x]`, not
`items.push(x)`. A prop without its change event can't be assigned.

## Lists of views

```asuka
for todo in todos key todo.id {
  TodoRow(todo) { on delete { todos.removeWhere(t => t.id == todo.id) } }
}
```

`for` keeps a row per key. A row whose item changed updates its own
bindings. A new item makes a row, and a removed one disposes its row. A
row that moved is moved, not rebuilt. Directly under a `List`, rows exist
only while they're on screen, so a list of 100,000 costs what's
visible.

A `for` over items that change says what identifies one, with `key`:
without it, a row's state would stay where it is while the items under
it move. Some need none:

```asuka
for i in 0..<count { Dot(i) }                      // a range: the number is the key
for tag in tags { Chip(tag) }                      // a set: the element is
for day in ["Mon", "Tue", "Wed"] { Text(day) }     // constant: nothing moves
for (i, x) in xs.enumerated() key i { Slot(i, x) } // the position, on purpose
```

## Effects and tasks

```asuka
effect { storage.set("draft", draft) }        // now, and when draft changes
effect(selected) { log(selected, user.plan) } // now, and when selected changes: only
task { try? await mailbox.refresh() }         // once, while the component is shown
task(query) { results = await search(query) } // and again when query changes
task(user.id) { profile = try await load(user.id) } // when the id changes, not the name
```

An effect runs at once and again whenever what it read changes, through
the functions it calls. What it read is as much as the code says:
`effect { log(user.name) }` runs again when the name changes, not the
avatar. `effect(a, b)` runs again when `a` or `b`
changes, and its body follows nothing it reads; `effect() { }` follows
nothing at all, and runs once, as the component is built. A task is async
([Async](async.md#async)): it starts when the component appears, restarts
when what's in its parentheses changes (cancelling the last run), and
is cancelled when the component goes. What's in the parentheses of an
effect or a task is compared: a change that leaves it equal does
nothing, so `task(user.id)` goes on when the user's name changes. The
`app` block takes them too: the app's own, for as long as it runs (a
refresh loop, a setting that follows the app's state: `effect {
appearance.setColorScheme(scheme) }`).

An effect may write state, but not state it follows: `effect { if n > 3
{ n = 3 } }` would run again after its own write, a pass late, after a
handler already saw `n` at 4. The compiler says so, and what says it
instead: a state that follows (`state n: int = min(n ?? 0, 3)`,
`state selected(results) = 0`), an `effect(…)` that follows something
else, or the handler that changes what it reads. A task may write what
it follows: it restarts by cancelling, and ends.

### Cleanups

```asuka
effect() {
  let id = bus.listen("refresh", e => reload())
  cleanup { bus.unlisten(id) }          // once: as the component goes
}
effect(roomId) {
  let room = roomId
  chat.join(room)
  cleanup { chat.leave(room) }          // before the next run, and as it goes
}
cleanup { drafts.save(draft) }          // as the component goes
```

`cleanup { … }` in an effect's body runs before the effect's next run
and when the effect goes: what the run set up, it undoes. What's set up
once, while the component shows, is an `effect() { }`'s: what it calls
may read and write what it registers with (a list of listeners) without
the effect following it. It may be
under an `if` (it undoes only what that branch did) or in a loop (once
for each time it was reached), and several run last first. In a
component's, a store's or the `app`'s body, beside its effects and
tasks, it runs when the instance goes, after its effects' cleanups.

A cleanup is a closure: it sees the run's locals, it doesn't await or
throw (it runs as its owner goes, and the cleanups after it still have
to: catch with `try?` or `do { } catch { }`), and what it reads isn't
followed. It reads state as it is when it runs. Before the effect runs
again, that's after what the effect follows has changed, so
`cleanup { chat.leave(roomId) }` would leave the new room: keep the
run's value in a `let`, and the compiler warns when you don't
([cleanup-reads-what-it-follows](../errors/cleanup-reads-what-it-follows.md)).
What it writes counts as its effect's writes.

Handlers, functions, tasks and tests don't take one: a task undoes what
it started with `defer`, which runs as it ends or is cancelled, and a
handler or a function has no owner of its own to run it.
