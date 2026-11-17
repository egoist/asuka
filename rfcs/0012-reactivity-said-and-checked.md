# 0012: What the UI follows, said and checked

- **Status**: Draft
- **Created**: 2026-11-18
- **Related**: 0007 (the Asuka language), 0010 (headless components:
  props a component assigns), lang/LOWERING.md (Reactivity,
  Components), core/src/signal, docs/language/components.md;
  ReactiveScript (github.com/solidjs/reactivescript, Solid 2.0's
  semantics as a language)

## Summary

A component's body is declarations: each line says something that stays
true. Four places in the language break that promise without saying so.
Each one compiles, runs, and shows the wrong thing later:

1. **A state made from what changes is a snapshot.** `state draft =
   title` copies `title` once; when the caller's `title` changes,
   `draft` doesn't, and nothing says it won't.
2. **What an effect or a task follows is everything it touches.**
   `task(u.id) { … }` restarts, cancelling its run, whenever any field
   of `u` changes, not just `id`; `effect { … }` follows every state
   read by every function it calls.
3. **An effect may write what it reads.** `effect { if n > 3 { n = 3 }
   }` converges, one pass late, after a handler has already seen `n`
   at 4; `effect { runs += 1 }` panics with "effects still dirty after
   100 passes".
4. **A `for` in a view keeps its rows by position.** Without `key`, a
   row's state stays at its place when items move: open the second row,
   insert an item at the top, and the new first row is the open one.

This RFC makes each one either say what it means or fail to compile:

1. `state x = e` **follows** `e` when `e` reads what changes: it is
   `e` until it's written, and `e` again when what `e` read changes.
   `state x(a, b) = e` follows `a` and `b` only. `peek(e)` reads once,
   for a seed. In its own initializer, a state's name is its current
   value.
2. `effect(a, b) { … }` follows `a` and `b` only, as `task(a, b)` does,
   and what's in the parentheses of either is compared: `task(u.id)`
   restarts when the id changes.
3. An effect that writes a state it follows is a compile error, which
   names the form that says it: a `let`, a following state, or the
   handler.
4. A `for` in a view over what changes needs its `key`.

```asuka
component Picker(items: [Item]) {
  state query = ""
  let results = items.filter(i => i.name.contains(query))
  state highlighted(results) = 0                  // back to the top when results change
  state selected: string? = if let s = selected, results.any(r => r.id == s) { s } else { results.first()?.id }
  state draft = peek(items.first()?.name ?? "")   // a seed, read once
  state preview: Preview? = none

  effect(selected) { analytics.log("selected", selected) }  // only selected
  task(selected) { if let id = selected { preview = try await api.preview(id) } }

  List {
    for item in results key item.id { Row(item) }
  }
}
```

Nothing here adds a runtime the app doesn't already have: a following
state is a memo the program may also write; a dependency is a memo; the
checks are the compiler's.

## Motivation

ReactiveScript, the language Solid's author published in October 2026,
carries Solid 2.0's reactive rules as language rules and turns their
misuse into compile errors "the way Rust refuses a borrow". Most of its
design Asuka already has, reached independently (see [Already
there](#already-there)). Reading it rule by rule against Asuka found the
four gaps above. Each was reproduced on the fake host with `asuka run`
before writing this.

**A snapshot that looks like a declaration.**

```asuka
component Edit(title: string) {
  state draft = title
  Text(draft)
}
```

The caller renames `"a"` to `"b"`; the text stays `"a"`. The state's
initializer runs once, untracked, when the component is made (CELLK).
Every other line in the body follows what it reads, including a prop
the component assigns without a binding (0010), whose own value is
replaced when the caller's changes. This one line doesn't, and the
compiler can't tell a seed meant to be one from a declaration that
silently stops being true. There are 526 state declarations with an
initializer in the repo; which of them read what changes is the
checker's to list.

**Dependencies that aren't what's written.**

```asuka
struct User { id: int; name: string }

state u = User(id: 1, name: "a")
task(u.id) { print("task started") }
Button("rename") { on press { u.name = u.name + "x" } }
```

Two presses print "task started" three times. `task(a)` lowers to an
effect that reads `a` and restarts the task, and `u.id` reads the cell
`u`: any write to `u` restarts it, cancelling the work in flight. The
same holds of `effect { }`. The repo has 48 `task(…)`s, several over a
field (`task(device.id)` in platform/bluetooth, `task(win.focused)`,
`task(account.available)`); each is one unrelated field write away from
a cancelled connection or request.

`effect { }` follows everything its body reads, through every function
it calls: `effect { storage.set("draft", draft) }` is right, and
`effect { save(draft) }`, with a `save` that reads the user's settings,
also runs whenever the settings change. There's no way to say "this,
not what the body happens to touch", which `task(…)` already says.

**An effect that writes what it reads.**

```asuka
state n = 0
state seen = -1
effect { if n > 3 { n = 3 } }
Button("inc") { on press { n += 1; seen = n } }
```

After five presses the screen says `n=3 seen=4`: the handler saw its
own write, then the effect clamped `n` in the flush's next pass, and the
two disagree. A loop that doesn't converge is worse:

```asuka
effect { let _ = u.id; runs += 1 }
```

panics at run time, "signal: effects still dirty after 100 passes (an
infinite loop?), e.g. effect text", naming neither the effect nor its
line. Both are visible in the source: the body reads the state it
writes. `examples/interaction` uses the converging form on purpose, to
reset a selection when results change (`effect { let _ = results;
selected = 0 }`), because there is no other way to say it.

**Rows kept by position.**

```asuka
component Row(name: string) {
  state open = false
  Button("{name} {if open { "open" } else { "shut" }}") { on press { open = !open } }
}

for n in names { Row(n) }
Button("add") { on press { names.insert("a", at: 0) } }
```

Open `b`, add `a` at the top: the screen says `a open`, `b shut`. A
`for` without `key` keys its rows by index (`ak_ui_for`'s
`AK_K_INT`), so each row's state, tasks and native views stay at their
position while the items under them move. React warns of this,
SwiftUI's `ForEach` needs an `id`, and ReactiveScript refuses it
(`UNSTABLE_LIST_IDENTITY`). In Asuka it's silent.

## Decisions

| Question | Decision | Why |
|---|---|---|
| `state x = e` where `e` reads what changes | Follows `e`: `e` until written, `e` again when what it read changes | The body is declarations. A prop a component assigns already behaves this way (0010); Solid 2.0's `createSignal(fn)` and ReactiveScript's `source x = e` are the same thing. |
| A seed read once | `peek(e)`: reads `e` without following it, anywhere | The exception is the one written down. Preact's and ReactiveScript's name; `untrack` is the core's. |
| Following some things, not all | `state x(a, b) = e`: `e` again when `a` or `b` changes; `e` itself is read once each time | The parentheses of `task(a, b)`, the one place that already says what's followed. |
| Keeping a value while what it's made from changes | In its own initializer a state's name is its current value, `T?` (none the first time), not followed | Keeps a selection that's still there and replaces one that isn't, which today takes an effect that writes what it reads. Solid's `prev`, ReactiveScript's `pre x`. |
| A write and an input change in one handler | Whichever comes later in the handler wins | Handlers are ordinary code, read their own writes, and run top to bottom. |
| `effect(a, b) { }` | Runs now and when `a` or `b` changes; its body follows nothing | `task(a, b)`'s form; Solid 2.0's split effect (compute, then apply). |
| `effect { }` | Stays: it follows what its body reads | Right when the body reads only what matters (`effect { setColorScheme(scheme.value) }`), and short. |
| What's in parentheses changing | Compared with the equality memos use: `task(u.id)` restarts when the id changes, not when `u` does | What's written is what's followed. Each dependency is a memo, made once. |
| An effect writing what it follows | A compile error, naming the read and the write, and the form to use instead | The loop is in the source; the run-time guard finds it a hundred passes late and only when it doesn't converge. |
| A task writing what it follows | Allowed | A task is async and restarts by cancelling: `task(deleting) { …; deleting = none }` is a queue in state, and ends. |
| A `for` in a view without `key` | An error when the items read what changes; ranges, sets and maps key by what they already are | Rows by position are wrong as soon as items move; SwiftUI requires an id. A constant list never moves. |
| Position as the key, on purpose | `for (i, x) in xs.enumerated() key i` | Says it. |

## The language

### A state that follows

```asuka
state count = 0                              // its own: nothing to follow
state draft = title                          // title until written; title again when it changes
state draft = peek(title)                    // title once: a seed
state highlighted(results) = 0               // 0 again whenever results change
state page(query): int = 1                   // a type goes after the parentheses
```

A state's initializer is checked as a `let`'s is: when it reads
nothing reactive (a literal, a constant, a local of the enclosing
function), the state is its own, as every state is today. When it
reads state, a `let`, a prop or a parameter, a `live` value, a view's
state (`card.hovered`) or a variant, the state **follows** it:

- It starts as the initializer's value.
- A write (`x = v`, `x.field = v`, `x.push(v)`, a `bind`) replaces its
  value, and its readers update, as a state's do.
- When something the initializer read changes, the initializer runs
  again and its value replaces whatever was written. Equal values
  change nothing (the memo's equality).
- A write after an input changed, in the same handler, stands: the
  write brings the state up to date first, then replaces it. A write
  before the input changed is replaced, when it changes.
- Reading it in a handler right after writing what it follows gives the
  new value: the initializer runs then, not at the end of the handler.

`state x(a, b) = e` follows `a` and `b`, compared as an `effect(a, b)`'s
are; `e` runs again, without following what it reads, each time one of
them changes. `state x() = e` isn't a form: `peek(e)` is.

In its own initializer, the state's name is its current value, as an
optional: none the first time, then the last value, written or not. It
isn't followed (a state doesn't follow itself), and the state's type must
be written, since its initializer can't be checked without it:

```asuka
// The selection stays while it's in the results, and moves to the first
// one when it isn't.
state selected: string? = if let s = selected, results.any(r => r.id == s) { s } else { results.first()?.id }
```

When the state's type is itself optional, none the first time and none
as the current value are the same, which is what such an initializer
wants.

This holds wherever `state` is: a component's, a store's (whose
parameters are accessors, so `store Kept<T>(key: string, initial: T) {
pub state value: T = initial }` follows `initial`), and the app's at
the top of a file.

### `peek`

`peek(e)` is `e`, read without following it. It goes wherever an
expression does:

```asuka
Text("{count} (started at {peek(count)})")   // follows count, not the second read
let label = "{title}: {peek(settings.unit)}" // follows title only
state draft = peek(note.body)                // a seed
```

In a handler, a task, an `effect(a)`'s body, a test or a `spawn`,
nothing is followed, and `peek` is an error that says so ("a handler's
reads aren't followed: read count"). In a `fn` it's allowed: a function
called from a binding is followed through.

`peek` is a builtin function, as `load` is a builtin form: a name the
program can't declare.

### What an effect follows

```asuka
effect { storage.set("draft", draft) }        // what its body reads
effect(selected) { analytics.log("selected", selected, user.plan) }  // selected only
task(user.id) { profile = try await api.profile(user.id) }           // user.id only
```

`effect(a, b) { … }` runs when the component is made, and again after a
flush in which `a` or `b` changed. Its body reads `a`, `b` and anything
else without following it.

Each expression in the parentheses of `effect(…)`, `task(…)` and
`state x(…)` is compared by value: it's followed as a `let` would be,
and a change that leaves it equal doesn't run the effect, restart the
task or replace the state. `task(u.id)` restarts when `u.id` does.

### An effect that writes what it follows

An effect may write state, and the writes start the next pass of the
flush. It may not write a state it follows:

```
interaction/app.asuka:149: this effect writes selected, which it reads
  at 150: it would run again after its own write, a pass late.
  - to reset selected when results change: state selected(results) = 0
  - to keep it in range: state selected: int = min(selected ?? 0, results.count() - 1)
  - or write it in the handler that changes what it reads
```

What an `effect { }` follows is what its body reads and what the
functions and methods it calls read, as far as the compiler sees them:
the program's own, transitively, as async and throwing are inferred. A
write is an assignment to state or a part of it (`x = v`, `x.f = v`,
`x[k] = v`, a mutating call) in the body or those functions. An
`effect(a, b)` follows only `a` and `b`. A read under `peek` isn't
followed.

What the compiler can't see (a closure value called, a native
interface) the run-time guard still stops. Its panic names the effect's
component, source line and the state it wrote last: "Picker's effect at
picker.asuka:12 is still writing selected after 100 passes".

Tasks may write what they follow. A task restarts by cancelling, so
`task(deleting) { if let id = deleting { await remove(id); deleting =
none } }` restarts once and ends.

### Keys

```asuka
for item in results key item.id { Row(item) }      // a key: rows follow their item
for i in 0..<count { Dot(i) }                      // a range: the number is the key
for tag in tags { Chip(tag) }                      // a set: the element is
for (k, v) in totals { Line(k, v) }                // a map: its key is
for (i, x) in xs.enumerated() key i { Slot(i, x) } // the position, on purpose
for day in ["Mon", "Tue", "Wed"] { Text(day) }     // constant: nothing moves
```

A `for` among views whose items are an array that reads what changes
needs `key`:

```
inbox.asuka:30: rows are kept by position here, so a row's state stays
  where it is when messages move: say what identifies a message,
  key m.id
```

The suggestion names an `id` field when the item's type has one, else
the item itself when it's hashable. A `for` in a function or handler
is a loop, and takes no key, as today.

## Checking

- **Reactive initializers.** The checker already knows whether an
  expression reads anything reactive: that's what decides between
  `ak_ui_set` and `ak_ui_bind`. A state's initializer gets the same
  answer and records it (`Member.Follows`). A parameter, a prop or a
  store's parameter is reactive: the caller's accessor may be a
  binding.
- **The state's own name** resolves, inside its initializer only, to a
  local of type `T?`. Without a written type the initializer is
  "selected reads itself, so its type must be written: state selected:
  string? = …".
- **`peek`** is a builtin with one argument, its type the argument's;
  in a context that runs untracked (handler, task, `effect(…)` body,
  test, spawn) it's an error.
- **Dependencies** are expressions of any type with an equality.
- **Effect writes.** Each function gets two sets beside the effects
  it's already given: the states it reads and the states it writes,
  each its own and those of what it calls. A state is a declaration (a
  component's, a store's member, a top-level one), not an instance.
  An `effect { }` is an error when its body's write set meets its read
  set; an `effect(…)`, when it meets what the dependencies read. Reads
  under `peek` don't count. Writes into part of a state count as a
  write of the state, never as a read: `ctx.labels[k] = v` (std/base's
  select) reads nothing.
- **Keys.** A view `for` over an array whose expression is reactive,
  without `key`, is an error. `enumerated()` takes `key i`.

## Lowering and the core

### Following states

A following state is a **follow**: a cell whose value a memo can
replace. The core gets it in core/src/signal beside memos (a memo whose
value may be set):

```c
// ak_follow_new makes a follow owned by the current owner: a memo whose
// update recomputes the caller's value, and which ak_follow_set may also
// replace. A set first brings the follow up to date (it recomputes if
// what it read changed), then replaces the value and notifies its
// readers as a signal write does.
ak_follow *ak_follow_new(bool (*update)(void *ctx), void *ctx, const ak_comp_info *info);
bool ak_follow_read(ak_follow *f);
void ak_follow_set(ak_follow *f);
```

A read pulls, as a memo's does, so a reader never sees the state behind
its inputs within a pass: no extra pass, no glitch. A follow whose first
run read nothing (a parameter whose caller passed a constant) drops its
sources and is a cell from then on.

The VM:

- `state x = e` with a reactive `e`: `FOLLOWK` with the state's key and
  a thunk of `e` (`CELLK`'s key, so hot reload keeps it: a kept value is
  a write, which stands until an input changes). Reads are `CGET`, and
  `CGET` of a follow reads through it (one branch on the cell's kind);
  writes are `CSET` and `CTAKE`/`CPUT`, which call `ak_follow_set`.
- `state x(a, b) = e`: a memo per dependency, and the thunk reads them
  followed, then `e` untracked.
- The state's own name in its initializer: the thunk takes one argument,
  the follow's current value or none.
- A prop the component assigns without a binding (0010's model, today a
  render effect that sets its own value when the caller's accessor
  changes) becomes a follow of the caller's accessor: one mechanism.

### `peek`

Two opcodes, `UNTRACK` and `RETRACK`, around the expression. The VM
keeps the tokens `ak_untrack_begin` returns on a stack of its own; a
frame records the stack's depth on entry and restores it when an error
unwinds it, so a throw inside `peek` can't leave the rest of a binding
untracked. cgen emits `ak_untrack_begin`/`ak_untrack_end` with the end
on the error path. Constant-folds away when `e` reads nothing reactive.

### Dependencies

`effect(a, b) { B }`, `task(a, b) { B }` and `state x(a, b) = e` make a
memo per dependency when the component is made (none when the
dependency is one state or `let` read alone: it's read directly, as an
accessor is), and an effect that reads the memos, then runs `B` (or
starts the task, or recomputes `e`) between `ak_untrack_begin` and
`ak_untrack_end`. Steady state allocates nothing: the memos are made
once and compare as every memo does.

### Keys

A view `for` over a set or a map without `key` passes the element, or
the map's key, as the key function (`ak_ui_for` with its kind); over a
range, the number. Arrays without a key are only constant ones, whose
rows never move, and keep today's position keys.

### The guard

`AK_MAX_PASSES` stays. The lowering passes each effect's label as
"Component's effect at file:line" (the `ak_comp_info` labels memos
already have), and the signal graph records the last cell an effect
wrote, for the panic.

## Already there

ReactiveScript's other rules, and where Asuka has them:

| ReactiveScript | Asuka |
|---|---|
| Reactive by default, imperative scope as the escape (E-2, SC) | A component's body is declarations; statements only in `on`, `fn`, `task`, `effect`, tests |
| An equation is never written (W-1) | A `let` can't be assigned; writing in a binding is "what the UI follows is computed, not changed" |
| Value and `ref` parameters; a function that owns something runs once (E-9, RG-6) | A plain `fn` is followed through by the binding that calls it; components and stores take accessors and run once |
| Lazy equations, equality cuts (E-3, E-6) | Memos are lazy and cut equal values |
| Errors as values, failures discharged (E-12, R-f) | `load` is a `Loading<T>`: `match` on it is exhaustive |
| Keyed rows (ST-4) | `for … key`; this RFC makes the key required where it matters |
| Several writes, one update (W-3) | A handler's writes show together when it ends or awaits |

Two of its rules Asuka keeps the other way, on purpose:

- **A handler reads its own writes.** ReactiveScript follows Solid 2.0:
  a write isn't visible until the change commits, and reading a state
  the same run wrote is `STALE_OWN_READ`. That rule exists for
  transitions, where a change is held while async work finishes. Asuka
  has no transitions; its handlers are ordinary code, and `count += 1;
  save(count)` must save the new count.
- **Effects aren't required to name what they follow.** Solid 2.0 has
  only the split effect; `effect { }` stays because when its body reads
  only what matters it's the clearest form, and the check above catches
  the dangerous case.

## Migration

Each part lands with its migration, in one change, across everything
in the repo (std, sdk, examples, bench, conformance, the tests'
testdata):

- **Following states.** The checker lists every state whose initializer
  reads what changes (a flag of `asuka check` for the migration,
  removed when it lands). Each is a seed (`peek(…)`), a state that should
  follow (left as it is), or one that should follow something narrower
  (`state x(a) = …`). The language is unreleased, so there's no
  deprecation period: the behavior changes, and the docs and
  CHANGELOG say so.
- **Dependencies compared.** No source changes; `task(device.id)` and
  the like stop restarting on unrelated writes.
- **Effect writes.** 13 effects in the repo; the check names what each
  should be. `examples/interaction`'s reset becomes `state
  selected(results) = 0`.
- **Keys.** 220 `for` lines have no `key`; the view ones over reactive
  arrays get the key their items already have.

## Alternatives

- **Keep the snapshot, add `state x follows e`.** The default is the
  bug: a declaration that stops being true without saying so. Every
  framework that grew both (React's "reset state when a prop changes",
  eslint-plugin-solid's rule against `createSignal(props.x)`) ends up
  telling its users not to write the default.
- **Make every reactive initializer an error** until it's spelled
  (ReactiveScript's `SNAPSHOT_INITIALIZER`). ReactiveScript has two
  declaration operators, `:=` and `=`, to spell the two meanings;
  Asuka has one, and following is what a declaration says. `peek`
  marks the exception where it's written.
- **Only the split effect** (Solid 2.0). It doubles every effect that
  needs nothing split; the check is what makes `effect { }` safe.
- **Check effect writes at run time only.** That's today's guard. A
  loop that converges isn't caught at all, and shows two states that
  disagree (`n=3 seen=4` above).
- **Position as the default key, with a warning.** A row's state at
  the wrong item is wrong every time the items move, not a matter of
  style; the compiler's warnings are for what may be meant (a closure
  that keeps itself).
- **Implicit keys from an `id` field.** Magic on a field's name; the
  error suggests it instead, and writing it is three words.

## Not in this RFC

- **Per-path tracking.** ReactiveScript's stores notify `(object, key)`;
  Asuka's `todos[i].done = true` notifies every reader of `todos`
  (`CPUT`), and keyed rows compare their items. Worth measuring on
  `examples/list10k` before designing.
- **Selection in O(1).** `if row.id == selected` in each of 10,000 rows
  runs 10,000 bindings when `selected` changes (Solid's
  `createSelector`). Also to measure first.
- **Transitions, optimistic writes, `latest` and `pending?`.** Solid
  2.0's async model; `load` and `task` cover what apps here do today.
- **Rule IDs in errors and a cold-authoring eval.** ReactiveScript's
  errors cite the rule they enforce, and fresh agents writing apps from
  the docs alone measure how learnable it is. Both would help Asuka;
  they're about docs and tooling, not this design.

## Open questions

- Whether `effect(…)`'s dependencies should be readable by position in
  its body (ReactiveScript's `$0`). Not now: the names are in scope.

## Milestones

1. **Dependencies compared, and `effect(a, b)`.** Memos per dependency
   for `task(…)` and `effect(…)`; untracked bodies. Tests on the fake
   host count task starts and effect runs (the `task(u.id)` case above
   starts once). Docs: components.md, concepts.md, and their zh and ja
   pages.
2. **`peek`.** The opcodes, cgen, the unwind; tests that a throw inside
   `peek` leaves the binding tracked.
3. **Following states.** `ak_follow` with its C tests (pull without a
   glitch, set after an input change, no allocation per write, owner
   disposal), `FOLLOWK`, `state x(…)`, the state's own name, 0010's
   model moved onto it, hot reload keeping a follow's value; the
   migration. Docs: declarations, components, concepts, LOWERING.md.
4. **Effect writes.** Read and write sets, the error and its
   suggestions, the guard's label; the migration.
5. **Keys.** The error and its suggestion; the migration.
