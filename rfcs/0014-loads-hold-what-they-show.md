# 0014: Loads hold what they show

- **Status**: Draft
- **Created**: 2026-11-18
- **Related**: 0012 (what the UI follows), 0013 (per-path tracking),
  docs/language/async.md ("Data that loads"), core/src/ui/control.c
  (loaders); Solid 2.0's holds ("Solid 2.0: The Long Hold",
  solidjs.com/blog), React's `useTransition`, SwiftUI's `.task(id:)`

## Summary

`load` goes back to `.pending` every time it runs again. The pattern the
docs teach,

```asuka
let user = load api.user(id)
match user {
  .pending => Spinner()
  .failed(e) => ErrorView(e) { on retry { user.reload() } }
  .ready(u) => ProfileCard(u)
}
```

replaces the profile with a spinner whenever `id` changes or `reload()`
runs, disposes `ProfileCard` and everything it held (its state, its
scroll position, its tasks), and builds it again when the answer lands.
The docs say `.value` "is kept while it reloads, so the screen doesn't
flash", which is true only of a program that reads `.value` instead of
matching.

Solid 2.0 made the opposite the rule: once something has shown content,
a new question holds it on screen until the answer lands, and
`isPending` says the answer is coming. This RFC does the same for
`load`:

- `.pending` is only **before the first answer**.
- A load that runs again **keeps its case**, `.ready(old)` or
  `.failed(e)`, until the new run answers.
- `user.loading` is **true while a run is in flight**, its own signal,
  for the program that wants to say so (dim the content, a spinner in
  the corner).
- An answer equal to the last notifies nothing.

```asuka
match user {
  .pending => Spinner()                                   // the first load only
  .failed(e) => ErrorView(e) { on retry { user.reload() } }
  .ready(u) => ProfileCard(u) { opacity: if user.loading { 0.6 } else { 1 } }
}
```

## Motivation

On the fake host, a card with state of its own inside `.ready`:

```asuka
component P {
  state id = 1
  let user = load fetchName(id)
  Column {
    match user {
      .pending => Text("spinner")
      .failed(e) => Text("failed")
      .ready(n) => Card(n)          // Card has state open = false
    }
    Button("next") { on press { id += 1 } }
  }
}
```

Open the card, press *next*:

```
["spinner","next"]          the card is gone
["user 2 shut","next"]      a new card: open is false again
```

`loader_run_fn` sets the loader to `AK_LOAD_PENDING` at the start of
every run (control.c), so `ui.switch` takes the `.pending` arm and
disposes the `.ready` arm's owner. The same happens on `reload()` of the
same id, on a pull to refresh, and on a load whose arguments read
anything that changes: a search box, a filter, a page.

Every platform's own pattern holds instead. UIKit's and Android's
refresh controls keep the list while they spin; SwiftUI's
`.task(id:)` writing into `@State` keeps the old value until the new one
is assigned; React's `useTransition` and Solid 2.0's holds keep the
committed UI and expose a pending flag. A spinner that replaces content
the user is reading, and a view rebuilt from scratch, is what a
framework does when nothing says otherwise. Here something should.

## Decisions

| Question | Decision | Why |
|---|---|---|
| When `.pending` | Before the first answer only | It means "nothing to show yet"; after that there is something. Solid 2.0's `Loading` boundary is "disarmed" once it has shown content. |
| A run after the first | Keeps `.ready(old)` or `.failed(e)` until it answers | The content stays, with its views and state. |
| A run that fails after a value | `.failed(e)`, with `.value` still the last value | The error is news; a program can still show the old content beside it. |
| Saying a run is in flight | `user.loading: bool`, tracked, its own signal | Solid's `isPending`. Its own signal, so reading it doesn't follow the value and a `match` doesn't follow it. |
| An answer equal to the last | Notifies nothing (`ak_val_eq`) | A refresh that changes nothing runs nothing. |
| A run cancelled by a newer one | Unchanged: the newer one's answer is the one shown | As today. |
| Other readers of what the load's arguments read | Update at once | Asuka has no transitions (0012): a handler's writes are visible to what reads them. The screen may show the new `id` beside the old profile for the length of the load; `user.loading` is how it says so. SwiftUI's `.task(id:)` does the same. |
| Starting over on purpose | Read `user.loading` in the `match`'s place, or give the content its own instance (a screen per id, a keyed row) | Both say it; no flag on `load`. |

## The language

`Loading<T>` stays an enum with the same three cases; what changes is
when it's in each:

| | Before | After |
|---|---|---|
| Made | `.pending` | `.pending` |
| First run answers | `.ready(v)` / `.failed(e)` | `.ready(v)` / `.failed(e)` |
| Runs again (arguments changed, `reload()`) | `.pending` | unchanged |
| That run answers | `.ready(v)` / `.failed(e)` | `.ready(v)` / `.failed(e)`, nothing when equal |

New: `loading`, a property like `value`:

```asuka
let results = load api.search(query)

TextField { bind text: query; placeholder: "Search" }
if results.loading { ProgressRing() }
match results {
  .pending => Spinner()
  .failed(e) => Text(e.message)
  .ready(items) => List { for r in items key r.id { ResultRow(r) } }
}
```

Typing replaces the results when the new ones land; the list, its
scroll position and its rows' state stay meanwhile, and the ring says a
search is running.

To start over instead (a different person's profile shouldn't show the
last one's for a moment):

```asuka
if user.loading {
  Spinner()
} else {
  match user { … }
}
```

or, more often, the content is already its own instance: a screen
pushed per id, a row per key.

## Lowering and the core

- The loader (`ak_loader`, control.c) gets a second signal, `busy`, and
  a `running` flag. A run sets `running` and writes `busy`; it no longer
  sets `AK_LOAD_PENDING`. `loader_done` clears `running`, writes `busy`,
  and sets the state and value, writing `node` only when the state
  changed or the value isn't equal to the last (`ak_val_eq`, which
  returns at once for the same object). A cancelled run changes
  nothing; the newer run is running.
- `user.loading` is a new selection (`SelLoading` with the name
  `loading`, beside `value`), lowered to the builtin `load.loading`,
  which reads `busy` and returns `running`. The checker marks it as
  reading state, as `value` is.
- `match user`, `.value` and `.error` lower as today; they read `node`,
  which a run no longer writes.

No allocation changes: the loader is made once, and a run already
allocates its ticket.

## Migration

The behavior changes for every `load`; no source does. The three in the
repo (`examples/checkout`'s wallet, `examples/gallery`'s device info,
`examples/launcher`'s icons) stop flashing when they run again. The docs
change where `load` is taught: docs/language/async.md ("Data that
loads"), docs/concepts.md, docs/data.md, and their zh and ja pages.

## Alternatives

- **Transitions** (Solid 2.0's and React's): hold the whole change, the
  write to `id` included, until every load it caused has answered, so
  nothing on screen is out of step. It needs writes that aren't visible
  until they commit, which 0012 decided against: Asuka's handlers are
  ordinary code. Per-load holds give the content's stability, and
  `loading` says what's out of step.
- **A fourth case**, `.reloading(old)`. Every `match` would handle it,
  almost always as `.ready`.
- **An option**, `load(keep: true) …`. The default should be the right
  one; starting over is the case to write out, and it already can be.
- **Keep today's behavior and teach `.value`.** Then `match`, the
  form the checker makes exhaustive, is the one that flashes.

## Milestones

1. **The core.** `busy`, `running`, the equal-answer check; C tests
   that a reload keeps the state, that `busy` alone is written while it
   runs, that a cancelled run changes nothing, and owner disposal.
2. **The language.** `loading` in the checker, the lowering and cgen;
   the motivation's program as a fake-host test (the card stays open,
   `loading` is true between the press and the answer); `asuka test`'s
   `idle()` still waits for loads.
3. **Docs.** The pages above, in all three languages.
