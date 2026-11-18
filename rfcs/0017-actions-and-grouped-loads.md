# 0017: Changes the screen shows at once, and loads that land together

- **Status**: Draft
- **Created**: 2026-11-18
- **Related**: 0014 (loads hold what they show: `.pending` only before
  the first answer, `loading`), 0012 (what the UI follows: dependencies
  compared), 0013 (per-path tracking: writes notify their path), 0011
  (places: `?.` writes, `mut fn`), 0015 (diagnostics with codes: the
  `long-hold` and `load-waterfall` detectors), lang/LOWERING.md
  (Components: `load`), core/src/ui/control.c (`ak_loader`); Solid 2.0's
  async model (its MIGRATION.md: `action`, `createOptimistic`,
  `createOptimisticStore`, `refresh`, `affects`, `isPending`,
  `<Reveal order="together">`), SolidStart's and Remix's revalidation
  after a mutation, TanStack Query's `invalidateQueries`, React 19's
  `useOptimistic`

## Summary

0014 made a `load` hold what it shows while it asks again. Two things a
screen of any size does with loaded data are still written by hand:

1. **A change shown before the server confirms it.** Toggling a todo,
   renaming a note, liking a post: the screen should show the change at
   once and undo it if the save fails. Today that's a copy of the
   loaded value in state, kept in sync by hand, rolled back in a
   `catch`.
2. **Telling every load that read the old data to ask again.** A save
   on an edit screen leaves the list screen under it showing the old
   title, because only the edit screen's own `load` has a handle to
   `reload()`.

And one more that 0014 left open:

3. **Loads that land together.** A profile and its posts, loaded side
   by side, swap in their answers one at a time on every change of
   `id`: for a moment the screen shows the new user over the old user's
   posts.

This RFC adds, for each:

1. `x.optimistic(f)`: what a load shows, changed by `f` at once, until
   the load's next answer replaces it, or the code that made it throws.
2. `reload f(args)` and `reload f`: every live `load` of that call, or
   of any call of `f`, asks again; `await reload …` waits for their
   answers.
3. `load (a(), b())`: one load of several calls, each run again only
   when what it reads changes, whose answers are shown together.

```asuka
screen Todos {
  let todos = load api.todos()

  List {
    for t in todos.value ?? [] key t.id {
      Toggle {
        on: t.done
        title: t.title
        on change(done) {
          todos.optimistic(ts => ts.map(x => if x.id == t.id { marked(x, done) } else { x }))
          try await api.setDone(t.id, done)   // a throw undoes it
          await reload api.todos              // this list, and every other one, asks again
        }
      }
    }
  }
}

fn marked(t: Todo, done: bool) -> Todo {
  var c = t
  c.done = done
  return c
}

screen Profile(id: string) {
  let page = load (api.user(id), api.posts(id))   // shown together

  match page {
    .pending => Spinner()
    .failed(e) => ErrorView(e) { on retry { page.reload() } }
    .ready((u, posts)) => ProfileView(u, posts) { opacity: if page.loading { 0.6 } else { 1 } }
  }
}
```

No new kind of value, no suspension, no transitions: a `Loading<T>` is
still one value a `match` covers, its writes are still ordinary writes,
and what's in flight is still `loading`.

## Motivation

### A change before the server says so

What every app writes today, to toggle a loaded todo at once:

```asuka
screen Todos {
  let todos = load api.todos()
  state shown: [Todo]? = none        // a copy, to change before the save lands

  let list = shown ?? todos.value ?? []
  List {
    for t in list key t.id {
      Toggle {
        on: t.done
        on change(done) {
          let before = list
          shown = list.map(x => if x.id == t.id { marked(x, done) } else { x })
          do {
            try await api.setDone(t.id, done)
            todos.reload()
          } catch e {
            shown = before               // undone by hand
            throw e
          }
        }
      }
    }
  }
}
```

It's wrong in three ways a reader can't see:

- `shown` is never cleared, so once set it hides every later answer:
  the list stops updating. Clearing it after `reload()` returns clears
  it before the reload's answer lands (0014: `reload()` starts a run, it
  doesn't wait for it), and the old list flashes back for a moment.
- Two toggles in flight: the second's `before` is the first's
  optimistic list, so the first one's failure "undoes" to a list that
  still has the second's change, and the second's failure resurrects
  the first's.
- `rows` of `shown` and `rows` of `todos.value` are different states:
  per-path tracking (0013) can't narrow a toggle to the row, so every
  row runs again.

Solid 2.0 has `createOptimistic` and `createOptimisticStore` for this,
"signals and stores whose writes revert when a transition completes";
React 19 has `useOptimistic`. Both come down to the same rule: an
optimistic value is a layer over the source of truth that goes when the
truth comes back.

### Who else read it

A note renamed on its edit screen: the edit screen calls
`note.reload()`. The list screen under it, in the same stack, has its
own `load api.notes()`, which nothing told. The workarounds are a store
that both screens read (so loads move out of the screens that show
them) or an event the list listens for (so every mutation knows every
screen that might care).

SolidStart (`revalidate(key)`), Remix (every loader after an action),
and TanStack Query (`invalidateQueries`) all solve it the same way:
the mutation names what it changed, by the query that reads it, not by
the screens that show it. Solid 2.0's `refresh(source)` does it for
one source it has a handle to.

### Loads that land at different times

```asuka
screen Profile(id: string) {
  let user = load api.user(id)
  let posts = load api.posts(id)

  match (user, posts) {
    (.ready(u), .ready(ps)) => ProfileView(u, ps)
    (.failed(e), _) => ErrorView(e)
    (_, .failed(e)) => ErrorView(e)
    _ => Spinner()
  }
}
```

On the first load this is right: a `match` over both shows the spinner
until both answer. When `id` changes, each holds its old answer (0014)
and swaps in its new one when it lands: the user changes, then a moment
later the posts do. Solid 2.0's `<Reveal order="together">` coordinates
sibling `<Loading>` boundaries for this; 0014 rejected transitions,
which would also do it, because they need writes that aren't visible
until they commit.

`load all(api.user(id), api.posts(id))` lands together, but it's one
call: a change to what either reads runs both, and a reload of one is a
reload of both.

## Decisions

| Question | Decision | Why |
|---|---|---|
| Where an optimistic change lives | On the load: `x.optimistic(f)`, `f` a function of what it shows | The layer belongs to the source of truth it's over; a state of one's own is the bug above. React's `useOptimistic` takes the same reducer form. |
| When it goes | When the load's next answer lands (a run started after it, or one in flight when it was made), or when the task that made it throws, whichever is first | "Until the truth comes back": 0014's hold makes the answer the moment to swap, so nothing flashes. A throw is the save failing. |
| A task that ends without a run to wait for | The layer goes when it ends | Nothing will confirm it; keeping it would hide the next answer forever. Reported in development (`optimistic-unconfirmed`). |
| Several at once | Layers stack in the order they were made; each goes on its own | The second toggle's failure undoes only the second. Solid's optimistic writes compose the same way within an action. |
| What it is to the reader | `match`, `.value` and rows see the layered value; `loading` is unchanged | One value: a row reads `todos.value[i].done` and follows that path (0013) whether it's the layer's or the answer's. |
| A new kind of action | None: a handler, a `fn` or a `task` that awaits is the action | Solid needs `action(function* …)` to know which async work a write belongs to; Asuka's handlers are already tasks with owners (0011, async handler ownership). |
| Naming what to reload | `reload f(args)`, `reload f`: by the function a `load` called, typed | SolidStart's and TanStack's keys, without strings: the call is the key, so a typo doesn't compile and the editor finds every load it reaches. |
| Which loads | Every live one in the app: any component, any screen in any stack, any window | A mutation shouldn't know which screens show its data. |
| Equal arguments | `reload f(a)` reaches the loads whose arguments are equal to `a` (`==`, as 0012 compares dependencies) | One rule for what "the same question" is. |
| Waiting | `await reload …` returns when every load it reached has answered (or failed: it doesn't throw for them) | Solid's `refresh` returns a promise of the next settled state. The failures are each load's own to show. |
| Pending | A reached load's `loading` is true until it answers | 0014's `loading` already means "a run is in flight"; Solid needed `affects()` because its `refresh` is quiet. |
| Grouping | `load (a(), b(), …)`: a tuple of calls, one `Loading<(A, B, …)>` | The shape a `match` already takes; each call keeps its own dependencies. |
| A group's run | Each call runs again only when what it reads changes; its answer is held until every call that's running has answered, then all are shown at once | Lands together without running what didn't change. |
| A group's failure | `.failed(e)`, the first call's error in source order; the others' answers are kept for `.value` | One case to match, as `all` gives. |
| Suspension | No | See Alternatives: a read that waits where it's read hides where the waiting is, and the `match` the checker makes exhaustive is what shows it. |

## The language

### `optimistic`

`x.optimistic(f)` on a `Loading<T>`, where `f: fn(T) -> T`:

```asuka
todos.optimistic(ts => ts + [draft])                         // add
todos.optimistic(ts => ts.filter(t => t.id != id))           // remove
note.optimistic(n => renamed(n, title))                      // rename (a fn like marked, above)
```

- What `todos` shows (`match`, `.value`, everything that reads them) is
  `f` applied to what it showed, at once, as a write: what reads the
  paths `f` changed runs again (0013), nothing else.
- It's `.ready` only: on `.pending` or `.failed` there's nothing to
  change, and it does nothing ("todos isn't ready: there's nothing to
  show changed", at run time in development, `optimistic-not-ready`).
- The layer goes, and what `todos` shows is its answer again, at the
  first of:
  - the load's next answer, from a run in flight when it was made or
    started after it (a `reload()`, a `reload api.todos`, a change of
    what its arguments read): the answer replaces the layered value in
    the same flush, so nothing flashes;
  - a throw out of the task that made it (the handler, the `fn`'s
    caller's task, the `task { }`): its write is undone, then the error
    goes where it goes;
  - the end of that task, when no run is in flight or was started by
    then (`optimistic-unconfirmed` in development).
- Several layers stack in the order they were made; one that goes is
  taken out from under the others, which are applied again to what's
  below.
- `f` runs once when the layer is made and again whenever what's under
  it changes (an answer that lands while another layer stays, a layer
  below that goes). It must be a value function: it can't read state,
  `await` or write (the checker's rule for a binding).

A `mut fn` form, for the common case of one field:

```asuka
todos.optimistic(ts => ts[i].done = done)   // an error: the closure gives no value
todos.optimistic { $0[i].done = done }      // not proposed: see Open questions
```

### `reload`

`reload` is an expression, like `load`, whose operand names a function
a `load` calls:

```asuka
reload api.todos()        // the loads of api.todos() (no arguments)
reload api.note(id)       // the loads of api.note with an equal id
reload api.note           // every load of api.note, whatever its arguments
await reload api.todos()  // and wait for their answers
```

- It reaches every live `load` in the app whose operand is a call of
  that function (with equal arguments, when they're given). A grouped
  load is reached through any of its calls, and runs only those.
- Each one runs again as `x.reload()` would: it holds what it shows,
  its `loading` is true until it answers (0014).
- It's async, as a call of an `async fn` is: `await reload …` waits
  until every load it reached has answered or failed (a load that goes
  away meanwhile counts as answered), and throws only when the waiting
  is cancelled; without `await`, the loads run again and the code goes
  on.
- A load whose operand isn't a call of a named function (a closure's
  call, a method of a value) can't be reached this way: it has its
  `x.reload()`.

`x.reload()` stays as it is: one load, by its handle.

### Grouped loads

`load (c₁, c₂, …)`, a parenthesized tuple of calls, is one load:

```asuka
let page = load (api.user(id), api.posts(id, page: n))
```

- Its type is `Loading<(A, B, …)>`, matched as any other:
  `.ready((u, posts))`.
- Each call has its own dependencies: changing `n` runs only
  `api.posts`; the user stays.
- A run's answers are held until every call that's running has
  answered; then all are shown together, in one write (one flush).
- `.pending` until every call has answered once; `.failed(e)` when one
  failed (the first in source order); `.value` the latest complete
  tuple, kept through a failure as 0014 keeps it.
- `loading` is true while any call is running. `page.reload()` runs
  every call; `reload api.posts(id, page: n)` runs that one.
- `optimistic` applies to the tuple: `page.optimistic(p => (p.0, p.1 +
  [draft]))`.

## Checking

New errors, each with its code (0015):

- `optimistic-not-loading`: `x.optimistic(f)` on what isn't a `load`'s
  value ("notes is a [Note], not a load: change it, it's state").
- `optimistic-impure`: `f` reads state, awaits or writes ("an
  optimistic change is a function of what's shown: pass what it needs
  in").
- `reload-not-loaded`: `reload f` where no `load` in the program calls
  `f` ("nothing loads api.todos: reload what a load calls"), and
  `reload` of what isn't a function or a call of one.
- `reload-arguments`: `reload f(…)` whose arguments don't fit `f`'s
  parameters (the call's ordinary errors).
- `group-not-calls`: `load (…)` whose elements aren't each a call
  ("load (a(), b()) groups calls: load (api.user(id), x) has x").

Run-time diagnostics, in development builds (0015's detectors):

- `optimistic-unconfirmed`: a layer went because its task ended with no
  run to confirm it. The usual fix: `await reload …` (or `x.reload()`)
  before the task ends.
- `optimistic-not-ready`: `optimistic` on a load that isn't `.ready`.
- `long-hold` (0015) counts a group's held answers too: a call that
  answered and waits for a slow sibling for more than 2 s, with nothing
  reading `loading`, says which call it waits for.

## Lowering and the core

- **The loader** (`ak_loader`, control.c) gets a stack of layers, each
  a closure, its owner (the task that made it) and the run it waits for
  (the loader's run counter at the time, or the next one). Its shown
  value is the answer with each layer applied, computed when a layer is
  added or goes and when an answer lands, and written to `node` as one
  write: a path write (0013) of what changed, compared as any write. No
  layers, nothing changes: the hot path is one test of an empty stack.
- `x.optimistic(f)` lowers to a builtin, `load.optimistic`, called with
  the loader and the closure; it registers a cleanup on the current
  task's owner (`ak_on_cleanup`, 0016) that takes the layer out when
  the task fails, and a completion that takes it out at the end unless
  a run is pending. `loader_done` takes out every layer waiting for a
  run at or before the one that answered, then writes the answer.
- **The registry for `reload`**: each loader whose operand is a call of
  a named function records the function's index and its arguments' last
  values (already compared for 0012's dependencies) in a table on the
  app, keyed by function, removed when the loader is disposed.
  `reload f(args)` is a builtin, `load.reload_all`, given the function's
  index and the arguments (or none): it runs each matching loader and
  returns a future that a counter resolves. Allocation: one table entry
  per live loader of a named call, made with the loader.
- **Grouped loads**: a loader with N slots, each its own dependencies
  (0012's per-dependency memos), its own run and ticket. A slot's
  answer goes into a pending tuple; the loader writes `node` when no
  slot is running. `load (…)` lowers to `ui.load_group` with the calls
  as closures; the checker gives it the tuple type.
- cgen: builtins, nothing new. No new opcodes.

## Tests

- The core (control.c's C tests, counted): a layer written and
  replaced by the next answer in one flush (one write, no flash); a
  layer undone by a throw; two layers, the first failing; a layer
  outliving a task with no run (`optimistic-unconfirmed` raised once);
  `reload` reaching loads in two roots, with equal and unequal
  arguments, one disposed meanwhile; a group whose second call is slow
  (one write when both land), a group run in part, a group's failure;
  allocations zero in the steady state with no layers.
- The language (lang/compile testdata/ui, on the VM and in C): the
  Summary's todo list (toggle, fail, toggle twice); the edit screen
  over the list screen in a stack, the list updated by `reload
  api.notes`; the profile, its posts' page changed without the user
  running again; every error above.
- Per-path (0013): a toggle under `optimistic` runs one row's bindings.

## Migration

None: everything here is new syntax or a new method. `reload` becomes
a keyword ("rename it: reload is a keyword since 0.N"); no `.asuka`
file in the repository uses it as a name. `x.reload()` keeps working.

Docs: docs/language/async.md ("Data that loads": changes shown at once,
reloading what a change touched, loads that land together),
docs/data.md, the tour, with zh and ja; the codes' pages (0015).

## Alternatives

- **Suspension** (Solid 2.0's model, React's `use`): a read of a value
  that isn't there yet waits where it's read, and a `<Loading>`
  boundary above catches it. It shows a spinner for several reads with
  one boundary, and makes async values read like sync ones. It's not
  for Asuka:
  - it hides where the waiting is: any read may wait, so the `match`
    the checker makes exhaustive (and the codes that point at a load's
    line) stop saying what's pending;
  - its strength is cheap teardown and rebuild of DOM under a boundary;
    with native views, keeping the content (0014) and saying `loading`
    is the better default;
  - grouping (above) and `match (a, b)` give the one-spinner case
    without it.
- **Transitions** (Solid 2.0's built-in ones, React's
  `startTransition`): hold every write until all the loads it causes
  have answered. 0014 rejected them: they need writes that aren't
  visible until they commit, and Asuka's handlers are ordinary code.
  Grouped loads give the "lands together" part for the loads that
  belong together, without holding anyone's writes.
- **Optimistic state** (Solid's `createOptimistic`, a signal whose
  writes revert): one more kind of state, and its value is a copy of
  the load's, which is the bug in Motivation. A layer on the load
  can't drift from it.
- **`affects(x)`** (Solid 2.0): marks a source pending while a mutation
  runs, because its `refresh` is quiet. `reload` isn't quiet: `loading`
  already says.
- **String keys** (SolidStart's `query(fn, "key")`, TanStack's
  `queryKey`): flexible, but a key is a second name for the call, kept
  in step by hand. The function and its arguments are the key.
- **Reload every load after every mutation** (Remix's default): no
  names at all, but it runs what didn't change, and Asuka has no
  boundary that says "a mutation happened".
- **`action(…)`** (Solid 2.0's `action(function* …)`): a wrapper that
  says which async work a write belongs to. A handler's task already
  says it (its owner), so the layer's lifetime needs no wrapper.

## Open questions

1. **A `mut fn` form.** `todos.optimistic(ts => ts.map(…))` is long for
   one field. With RFC 0011's places, a closure that changes its
   parameter in place (`todos.optimistic { $0[i].done = done }`, or a
   `mut` closure parameter) would read better. It needs a closure form
   the language doesn't have yet.
2. **`reload` of a store's method.** A store that loads (`store Api {
   pub async fn notes() … }`): `reload api.notes` names the method on a
   value. The proposal keys by the function, so every instance's loads
   are reached; keying by instance too is possible.
3. **Ordering a group's reveal** (Solid's `<Reveal order="sequential">`:
   each boundary in turn). Not proposed: `together` is the case that
   comes up; sequential reveal is a layout's concern.

## Milestones

1. **`optimistic`.** The loader's layers, `load.optimistic`, the
   cleanup and completion, the errors and run-time codes; tests on the
   VM and in C, per-path counted; docs.
2. **`reload f`.** The registry, `load.reload_all`, the keyword, the
   errors; the stack test; docs.
3. **Grouped loads.** The loader's slots, `ui.load_group`, the checker's
   tuple type; the profile test; `long-hold` for groups; docs.
