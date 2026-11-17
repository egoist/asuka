# 0013: Per-path tracking

- **Status**: Draft
- **Created**: 2026-11-18
- **Related**: 0007 (the Asuka language), 0012 (what the UI follows),
  lang/LOWERING.md (Reactivity, Components), core/src/signal,
  core/src/vm/react.c, core/src/ui/control.c (`for`); Solid's stores,
  Vue 3's `reactive`, Svelte 5's `$state`, ReactiveScript's ST-2

## Summary

A read of state follows the whole state today. `Text(user.name)` runs
again when `user.avatar` changes; `todos[1].done = true` runs every
reader of `todos`, and a `for` over `todos` calls its key for every row.
Solid's stores, Vue 3's `reactive` and Svelte 5's `$state` all follow
what was read, property by property: a change to one field of one row
costs what reads that field.

This RFC makes Asuka's reads as narrow as what the code reads:

- **A read follows its path.** `user.name` follows `user`'s `name`;
  `todos[i].done` follows that element's `done`; `todos.count()`
  follows the length; `cart[sku]` follows that key; `picked.contains(id)`
  follows that member. A value read whole (`save(user)`) follows it
  whole, as today.
- **A write notifies its path.** `todos[1].done = true` notifies the
  readers of `todos[1].done`, of `todos[1]` and of `todos` read whole,
  and no one else. A write of an equal value notifies no one. `todos =
  fetched` compares what's read, path by path, and notifies only the
  paths whose values changed.
- **Rows follow their place.** A `for` over a path follows the order of
  its keys, not its items: a write inside an item doesn't run the `for`
  at all. A row's item, a component's prop, and a `let` that's a path
  pass the path on, so `TodoRow(todo)`'s `todo.title` follows only that
  title.
- **Collection methods follow what their closure reads.**
  `todos.count(t => t.done)` follows each `done` and the length, not
  the titles.

Values stay values. Nothing a program sees changes: no proxies, no
identity, no `$state`-style wrapper, copy on write as now. What changes
is what each read subscribes to, and what each write notifies.

## Motivation

On the fake host today, with a `print` in each binding and in the `for`'s
key:

```asuka
struct User { name: string; avatar: string }
struct Todo { id: int; title: string; done: bool }

state u = User(name: "a", avatar: "x")
state todos = [Todo(id: 1, …), Todo(id: 2, …), Todo(id: 3, …)]
let doneCount = todos.count(t => t.done)

Text(show("name", u.name))
for t in todos key keyOf(t) { Text(show("title {t.id}", t.title)) }
Text(show("count", "{doneCount}"))
Button("avatar") { on press { u.avatar = "y" } }
Button("toggle") { on press { todos[1].done = true } }
```

| Press | Runs today | Should run |
|---|---|---|
| avatar | `name` | nothing |
| toggle | `key 1`, `key 2`, `key 3`, `count`, `title 2` | `count` |

The causes, in the code:

- A cell is one signal (`ak_node` in react.c's `cell`). `CGET` subscribes
  to it whatever the expression does with the value next; `CPUT`, the
  end of every write into part of a cell (`todos[i].done = true`,
  `form.name = n`, `todos.push(t)`), notifies it unconditionally.
- A `for` (control.c's `for_run`) reads its items whole, then
  reconciles: the key closure for every item, a hash table of the old
  rows, the longest increasing run, and `ak_row_update`, which sets each
  row's item cell. That's O(n) VM calls for a write to one field of one
  item, on every write. With 10,000 rows it's 10,000 key calls per
  checkbox.
- A row's item is one cell, so a row that reads `t.title` and `t.done`
  runs both when either changes.
- A memo over a collection (`count(where)`, `filter`, `any`) runs again
  on any write to it, though it reads one field of each item.

Equality already cuts what reaches the screen: an unchanged title
doesn't send an op. What it doesn't cut is the work: the bindings, the
memos, the keys. On a phone, in a list, at 120 Hz, that's the budget.

## Decisions

| Question | Decision | Why |
|---|---|---|
| What a read follows | Its path from the state: fields, indexes, map keys, set members, a length | Solid's, Vue's and Svelte 5's granularity, without their proxies: Asuka's compiler sees the path. |
| What a value read whole follows | Everything under it, as today | A value passed to a function, compared, interpolated or kept has been read whole. |
| Where a path's subscriptions live | A tree of nodes under the cell, made on a path's first read in a tracked context, freed when its last reader goes | Costs what's read, not what's held: a list of 10,000 items with 20 rows on screen has nodes for 20 rows. |
| What a write notifies | Its path's node, the nodes above it, and the nodes below it | The value at each of those changed; a sibling's didn't. |
| An equal write | Notifies nothing | `todos[i].done = true` when it's true is free. Today `CPUT` always notifies. |
| Replacing the whole state | Compares the old and new values at each followed path, skipping subtrees that are the same object | Solid's `reconcile`, for free: a refetched list notifies the rows that changed. Shared subtrees (copy on write) compare in O(1). |
| A mutating method (`push`, `insert`, `removeWhere`, `sort`, a set's `insert`) | Reports the first index it changed and whether the length changed; the write notifies those | `push` doesn't touch rows 0 to n-1, so their readers don't run. |
| A `for` over a path | Follows the order of its keys: the length, and each item's key. A field-path key (`key t.id`) follows one signal for all items | A write inside an item doesn't change the rows; only a change of order, length or a key reruns the `for`. |
| A row's item, a prop, a `let` that's a path | Carries the path, so reads through it are paths too | `TodoRow(todo)` with `todo.title` follows that title, as Solid's props do. |
| Closures in collection methods | On a path, in a tracked context, with a closure literal: its parameter is the element's path; elements the method returns are read whole | `count(t => t.done)` follows the `done`s; `filter` also follows the items it keeps, so its result is never stale. |
| Functions | Take values | A `fn`'s parameters are values; a path stops there. Making every function generic over paths is what proxies do, and what this RFC avoids. |
| Enums | Read whole | A payload's fields are rarely the hot path; `match` reads the case and binds the payload. |
| Moving rows | A row's place is part of its path; a moved row's bindings run again and re-subscribe at the new index | Equality cuts their ops. Paths by key instead of index would tie the cell to the `for`'s key function. |

## The model

A **path** is a state, then steps:

| Step | Written | Followed |
|---|---|---|
| a field | `.name` | that field |
| an index | `[i]`, `.first()`, `.last()`, `.get(i)` | that position |
| a key | `[k]`, `.get(k)`, `.has(k)` | that key (present or not) |
| a member | `.contains(x)` on a set | that member's presence |
| a length | `.count()`, `.isEmpty()` | the length |
| through an optional | `?.` | as the step after it |

A read is the longest path its expression spells. `todos[i].done` is
one read of a path three steps long; `todos.filter(…)` is a read of
`todos` whole, unless the closure rule below applies; `format(user)`
reads `user` whole.

A **write** is to a path too: `x = v` to the state, `x.f = v` and
`x[i] = v` to theirs, `x.push(v)` and the other mutating methods to
`x`, with the range the method reports. A write notifies:

- the readers of the path written, when its value changed;
- the readers of each path above it (their value changed with it);
- the readers of each followed path below it: all of them for a write
  inside an item (they're few), and only those whose value changed for
  a write of the whole state (compared, skipping shared subtrees);
- for a mutating method, the readers of the length when it changed, and
  of the indexes from the first it changed.

Nothing else. Readers of `todos[2]`, of `todos[1].title` and of
`u.name` above run only when theirs changed.

## Paths in the language

Paths reach as far as the compiler sees them, without the program
saying anything:

```asuka
store Inbox {
  pub state messages: [Message] = []
  pub state picked: Set<string> = Set<string>()
  pub let unread = messages.count(m => !m.read)     // follows each read flag and the length

  pub fn pick(id: string) {
    if !picked.remove(id) { picked.insert(id) }     // writes that member
  }
}

screen Messages {
  use inbox: Inbox
  for m in inbox.messages key m.id {                // follows the ids and the length
    MessageRow(m)                                   // m is inbox.messages[i]
  }
}

component MessageRow(m: Message) {
  use inbox: Inbox
  Row {
    Text(m.subject)                                 // inbox.messages[i].subject
    if inbox.picked.contains(m.id) { Text("✓") }    // that member only
    Button("Pick") { on press { inbox.pick(m.id) } }
  }
}
```

Marking one message read runs `unread` and that row's bindings that
read `read`, nothing else. Picking one runs that row's `if`, not ten
thousand.

- **Rows.** In a `for` over a path, the item is the path to its
  element: `m` above is `inbox.messages[i]`, with `i` the row's place.
- **Props and parameters.** A component's argument that's a path is
  passed as one: inside `MessageRow`, `m.subject` is
  `inbox.messages[i].subject`. An argument that isn't (a literal, a
  computed value) is a value, as today.
- **`let`.** A `let` whose value is a path (`let draft = form.draft`)
  is that path: no memo, and `draft.title` is a path read. A `let` that
  computes is a memo, as today.
- **Collection methods.** On a path, in a tracked context, with a
  closure literal: `count(where)`, `any`, `all`, `find`, `index`,
  `filter`, `sorted(by)` and `map` read their element as a path inside
  the closure, and follow the length. The ones that return elements
  (`find`, `filter`, `sorted(by)`) also follow, whole, each element
  they return.
- **Writes through `bind`.** `TextField { bind text: form.email }`
  reads and writes `form.email`.
- **What stops a path.** A `fn`'s argument, an interpolation, an
  operator, a comparison, a `match` subject, an enum's payload, a value
  kept in a `let` that computes, a value captured by a closure that
  isn't a collection method's: all read whole.

A handler, a task, a test, `spawn` and `peek` (0012) read without
following, so none of this applies there: their reads are plain.

## Lowering and the core

### Path nodes

A cell keeps, beside its signal, a tree of **path nodes**, made on
demand:

```c
// One node per followed path below a cell. Its signal's readers are the
// path's; its children are the steps below it, by kind: fields in an
// array by field index, indexes in an array by index, map keys and set
// members in a hash, the length in a slot of its own.
typedef struct ak_pnode {
	ak_node node;
	struct ak_pnode *parent;
	ak_step step;          // kind, and the field index, the index or the key
	ak_pchildren kids;
} ak_pnode;
```

- A node is made on its path's first read in a tracked context and
  freed, with the nodes above it that have neither readers nor
  children, when its last reader goes: signals get the `unwatched`
  callback memos already have.
- A re-run that reads the paths it read before finds their nodes: no
  allocation in the steady state, as the graph's links are reused now.
  Tests count it (`ak_alloc_count`).
- The cell's own signal stays, as the root: reading the state whole.

### Reads

`CPATH a, cell, P, base` reads the value at path `P`: a path constant
listing the steps, field indexes inline, indexes and keys from registers
from `base`. Tracking, it finds or makes the path's node and subscribes
to it; untracked, it navigates the value and makes nothing. A length is
`CLEN a, cell, P, base`. The checker marks each read's longest path; the
lowering emits `CPATH` where it emitted `CGET` and the `GETF`/`IDX`/
`MGET` chain after it.

A path that doesn't exist yet (an index past the end, a missing key)
subscribes to its node and returns what the plain read would (throws,
or none): a later write that makes it exist notifies it.

### Accessors with paths

An accessor (`ACCC`, `ACCF`, `ACCV` today) gets a fourth kind,
`ACCP`: a base (a cell, or another accessor with a path) and steps.
`AGET` of one reads its path; `AGETP a, acc, P, base` reads a path below
it. A row's item is an `ACCP` of the `for`'s path and the row's index
cell, so a row that moves runs the bindings that read through it again,
at the new index; their values are equal, so they send nothing.

### Writes

The lowering already writes into part of a cell as `CTAKE`, the
changes in place (`SETF`, `SETIDX`, a mutating builtin), and `CPUT`.
`CPUT` becomes `CPUTP cell, v, P, base`: the path written, so the write
notifies as the model says. Before the change, the lowering compares
the old and new values at the written place (a scalar compare, or
`ak_val_eq`, which returns at once when they're the same object) and
skips the notification when they're equal.

The array, map and set builtins that change their receiver in place
(`push`, `insert`, `removeAt`, `removeWhere`, `pop`, `clear`, `sort`,
`m[k] = v`, a map's `remove`, a set's `insert` and `remove`) record
what they changed in the VM's state: the first index and whether the
length changed, or the key or member. `CPUTP` reads it.

`CSET` (the whole state written) walks the followed tree, comparing the
old and new values at each node and notifying the ones that changed. Two
values that are the same object end the walk under them, so a refetched
list whose unchanged items compare equal notifies only what changed, and
a value changed by copy on write compares the untouched part in O(1).

### `for` over a path

When a `for`'s items are a path, `ui.for` gets the path instead of a
thunk, and follows:

- the length;
- each item's key. A key that's a field path of the item (`key t.id`,
  `key t.meta.id`, the common case) registers the field path with the
  array's node, which gives it one **keys** signal: a write to `[i].id`
  under it, or a change of length or order, notifies it. Any other key
  is read through each item's path, one node per item.

A write inside an item notifies neither, so the `for` doesn't run. When
it does run, it reconciles as today, minus `ak_row_update`'s item cells:
a row's item is its path, and its index cell is all that changes.

Under a `List`, rows exist only while they're on screen, so do their
nodes; the keys signal is one per `for`.

### Collection methods

`count(where)` and the rest, on a path in a tracked context with a
closure literal, are lowered as loops over the path: the length read
with `CLEN`, each element's reads with `CPATH` at `[i]` and the
closure's steps. No closure is made and no element is retained to call
it; the loop runs in the binding's or memo's frame. Elsewhere (untracked,
not a path, a closure value) they're the builtins they are now.

### Following states and the rest of 0012

A following state (0012) is a cell whose value a memo may replace: its
paths are a cell's, and a replacement is a whole write, compared as
`CSET`'s. `peek(path)` reads untracked. An `effect(…)`'s or `task(…)`'s
dependency that's a path follows that path: `task(device.id)` follows
the id without a memo of its own.

## Tests

- **Counting runs on the fake host**: the motivation's program asserts
  what runs for each press (`avatar`: nothing; `toggle`: `count`), and
  the same for rows under props, `let` paths, collection methods, sets
  and maps.
- **The core's C tests** for path nodes: subscription and pruning,
  notification by path, equal writes, whole writes with shared
  subtrees, each mutating builtin's range, a missing index or key that
  starts existing, owner disposal; under ASan and UBSan, with
  allocations counted in the steady state.
- **Ops**: the existing op-stream tests keep passing; per-path tracking
  changes what runs, never what's shown.
- **`examples/list10k`**: toggling a row runs a constant number of
  bindings and no key calls, measured as counted work, not time.

## Alternatives

- **Proxies** (Solid's stores, Vue's `reactive`, Svelte 5's `$state`).
  A read returns a live view that records the property read next. They
  need identity: two reads of the same element return the same proxy,
  and a value held anywhere changes when the state does. Asuka's values
  are values, copied on write, safe to send to `spawn`; a proxy in a
  `let`, a closure or another thread would be a reference that changes
  under its holder. The compiler sees the paths that proxies discover at
  run time, so it can do their work without their semantics.
- **Lenses at run time.** A read in a tracked context returns a small
  object carrying (cell, path), and every op that takes a value checks
  for one. Costs a branch on every `GETF` and `IDX`, an object per read
  until cached, and the same identity questions once a lens escapes into
  a value.
- **Comparing on write only** (keep one signal per cell, compare deeply
  before notifying). An equal write already notifies nothing (`CSET`);
  the problem is the writes that aren't equal, which still run every
  reader.
- **Selectors written by the program** (Redux's `useSelector`, Solid's
  `createSelector`, a `let` per field). Each one is the program doing
  by hand what the compiler can see, and forgetting one costs nothing
  visible but time.
- **Paths by key instead of index** for rows, so a move re-subscribes
  nothing. The cell would need the `for`'s key function to map an index
  to a key on each write; a moved row's bindings re-running is the
  smaller cost, and equality cuts their ops.

## Open questions

- **Memory under a `for` without a field-path key.** One node per item
  for its key. Measure on `examples/list10k` with a computed key; if it
  matters, the error 0012 gives for a missing key can suggest a field.

## Milestones

1. **Path nodes in the core.** `ak_pnode`, the `unwatched` callback on
   signals, notification by path, equal writes, whole writes with
   shared subtrees; C tests.
2. **Reads and writes.** The checker's longest paths, `CPATH`, `CLEN`,
   `CPUTP`, the mutating builtins' ranges, cgen for each; the fake-host
   counting tests for `user.name` and `todos[i].done`.
3. **Accessors with paths.** `ACCP`, rows' items, props and `let`
   paths; the row tests.
4. **`for` over a path.** The keys signal, reconcile without item
   cells; `examples/list10k`'s counts.
5. **Collection methods.** The loops; the `count`, `filter` and `find`
   tests, including a kept item whose title changes.
6. **Docs.** LOWERING.md's Reactivity and Components, DESIGN.md's
   signals section, docs/concepts.md's "what updates when state
   changes", and its zh and ja pages.
