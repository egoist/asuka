# Components

A component is a piece of interface, declared with its parameters in
its header and everything else in its body:

```asuka
component MessageRow(message: Message) {
  prop compact = false            // a named prop, with its default
  prop accessory: View? = none    // a slot: a view the caller passes
  event archive                   // something it reports
  state expanded = false          // its own state

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
  contextMenu {
    Button("Archive") { role: .destructive; on press { emit archive } }
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

- `state x = …` is state the component owns.
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

## Handlers and statements

Code runs in handlers (`on press { … }`), functions, `task` and `effect`
blocks, and tests. There, reading state gives its current value, and
writing it updates what reads it:

```asuka
on press {
  count += 1
  todos.push(Todo(title: draft))
  todos[0].done = true      // writes todos: only the rows that read todos[0] update
  draft = ""
}
```

Writes in a handler show together when it ends (or when it reaches an
`await`).

Statements: `let`, `var`, `let … else`, assignments (`=`, `+= -= *= /=
%=`, `&= |= ^= <<= >>=`, `??=`), `if` and `if let`, `for x in xs` (an
array, a set), `for (k, v) in m`, `for (i, x) in xs.enumerated()`,
`while` and `while let`, `match`, `return`, `break`, `continue`,
`throw`, `do { } catch { }`, `defer { }`, `emit`
([Control flow](control-flow.md#control-flow)), and `fn` declarations ([Functions and
closures](#functions-and-closures)).

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

```asuka
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

## Effects and tasks

```asuka
effect { storage.set("draft", draft) }        // now, and when draft changes
task { try? await mailbox.refresh() }         // once, while the component is shown
task(query) { results = await search(query) } // and again when query changes
```

An effect runs at once and again whenever what it read changes. A task
is async (see below): it starts when the component appears, restarts
when what's in its parentheses changes (cancelling the last run), and
is cancelled when the component goes. The `app` block takes them too:
the app's own, for as long as it runs (a refresh loop, a setting that
follows the app's state: `effect { setColorScheme(scheme) }`).
