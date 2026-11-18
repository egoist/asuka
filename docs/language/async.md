# Async

Work that waits (the network, a file, a timer) is `async`, and waiting
for it is `await`:

```asuka
async fn loadThread(id: string) throws -> Thread {
  let (messages, people) = try await all(api.messages(id), api.people(id))
  let html = await spawn { renderMarkdown(messages) }
  return Thread(messages, people, html)
}
```

- Calling an async function needs `await`. Handlers, tasks and tests
  may await; a component's body and its `let`s may not. A closure is
  async when it's passed as an async function or when its body awaits,
  and `async () => …` marks it so ([Functions and
  closures](functions.md)).
- **The main thread.** Everything runs on it, and `await` lets other work
  run while yours waits; state can change there and only there.
- `all(a, b)` runs calls at once and waits for every result; `race(a, b)`
  takes the first and cancels the rest; `sleep(300ms)`;
  `withTimeout(5s) { … }`, whose value is its block's (`let page = try
  await withTimeout(5s) { return try await fetch(url) }`), and which
  throws when the time runs out. They're structured: what they cancel has
  stopped, its `defer`s run, before they return or throw.
  `await never()` waits for nothing but the task's end: a stream that
  ends only when its reader goes, or a task that awaits something before
  it holds it (`try await register(); defer { release() }; await
  never()`). What a component holds without awaiting is a `cleanup { }`
  ([Cleanups](components.md#cleanups)).
- `all(items, f, limit:)` calls `f`, an async function, with each of
  `items` at once, and gives their results in the items' order: `let
  pages = try await all(urls, u => try await http.get(u))`. `limit: 8`
  runs at most 8 at a time, the next starting as one finishes; left out
  (or 0), all of them start at once. As `all(a, b)`, it's structured: the
  first that throws cancels the others and starts no more, and it throws
  once they've stopped; the task that awaits it going cancels them all.
  Each runs on the main thread as a task of its own, so it can wait on
  the network or a file and change state; work for another thread is
  `spawn`'s, which `f` may await.

  ```asuka
  async fn fetchAll(urls: [string]) throws -> [string] {
    let pages = try await all(urls, u => try await http.get(u), limit: 4)
    return pages.map(p => p.text())
  }
  ```
- `spawn { … }` runs a block on another thread, for work that would hold
  the main thread up (parsing, image processing). It may use values only,
  not state, stores, views, native objects or C pointers, nor a file's
  `let` that holds one (a database): the compiler says so if it does. On
  the web it runs on one of the page's Web Workers, which make the
  file's values for themselves, and nothing else. Work that keeps
  something between calls (an index, a database it opens) is a
  [worker](#workers).
- `for await message in socket.messages { … }` reads a stream, a
  `Stream<T>`: values that come over time. `Stream(send => { … })`
  makes one: the block runs as a task when the stream is first read,
  and each `send(x)` is a value; the stream ends when the block
  returns, and fails with what it throws. Leaving the loop (`break`, an
  error, the owner going) cancels the block. A stream has one reader.

  ```asuka
  fn levels() -> Stream<float> = Stream(send => {
    try await battery.watch(changed: level => send(level))
  })
  ```
- `live(initial, source)` is a value a stream keeps current, as a
  function that reads it: read in a binding, it follows `source()`'s
  stream from the first such read until the last reader goes (then the
  stream is cancelled: nothing watches the platform for no one); read
  elsewhere, it's the last value known. It's how a package offers what
  the platform changes, such as the network's state:

  ```asuka
  let level = live(1.0, levels)
  pub fn batteryLevel() -> float = level()
  ```
- **Cancellation.** A task belongs to what started it (a handler of a
  component's event, to the component that declares it:
  [Components](components.md)). When that goes (the user leaves the
  screen), the task is cancelled: the `await` it's at ends it, its
  `defer`s run, and no error is reported.

## Workers

`spawn` runs a block once, on another thread. A `worker` keeps a value
there: a type whose instances each run on a thread of their own, with
vars that are theirs alone.

```asuka check
worker SearchIndex {
  var words: [string: [int]] = [:]

  pub fn add(id: int, text: string) {
    for w in text.split(" ") {
      let k = w.lower()
      words[k] = (words[k] ?? []) + [id]
    }
  }
  pub fn find(q: string) -> [int] = words[q.lower()] ?? []
}

store Notes {
  let index = SearchIndex()
  state results: [int] = []

  pub async fn add(id: int, text: string) { await index.add(id, text) }
  pub async fn search(q: string) { results = await index.find(q) }
}
```

- A worker's `var`s, `let`s and parameters (`worker Index(language:
  string)`) are its own: only its fns read and change them, on its
  thread. It has no `state` (views don't read it), props, events or
  views.
- Its `pub fn`s, called from outside, are async: `await
  index.find(q)`. Their arguments go over as values, the calls run one
  at a time in the order they were made, and the result, or the error a
  `throws` fn throws, comes back as a value. Inside the worker they're
  ordinary functions that don't await; its other fns are its own.
- What crosses is what `spawn` takes: values, never state, stores,
  views, closures or native objects; a worker's fns don't read the main
  thread's state either. It may make and keep a C handle of its own (an
  [extern type](../c.md#types-c-owns), a database it opens): it never
  leaves the worker, and it's released there.
- A worker belongs to the component or store that made it. When that
  goes, the worker stops: the call it's running finishes, and those
  waiting are cancelled, which their callers' tasks see as their own
  cancellation, not an error. One that nothing owns stops once nothing
  refers to it and the calls it was given have run.
- On the web, each worker is a Web Worker of its own.

## Data that loads

`load` keeps an async result in a component:

```asuka
screen Profile(id: string) {
  let user = load api.user(id)   // runs again when id changes

  match user {
    .pending => Spinner()
    .failed(e) => ErrorView(e) { on retry { user.reload() } }
    .ready(u) => ProfileCard(u) { opacity: if user.loading { 0.6 } else { 1 } }
  }
}
```

A `Loading<T>` is `.pending`, `.ready(value)` or `.failed(error)`, and
it's `.pending` only before its first answer. When what its arguments
read changes, or `.reload()` runs it again, it keeps the case it's in
until the new answer comes: `ProfileCard` stays on screen with its state
and its scroll position, and the run that's going stale is cancelled.
An answer equal to the last changes nothing.

- `.value` is what it shows, none before the first answer. A run that
  fails keeps it, so `.failed` can show the old content beside the
  error.
- `.loading` is true while a run is in flight, the first one included:
  dim the content, or show a ring in a corner. Reading it doesn't follow
  the value, and a `match` doesn't follow it.

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

As the user types, the list keeps the last results, its scroll position
and its rows' state until the new ones land, and the ring says a search
is running. What else reads `query` shows the new text at once.

To start over instead (one person's profile shouldn't show the last
one's while the next loads), read `.loading` in the `match`'s place:

```asuka
if user.loading {
  Spinner()
} else {
  match user {
    .pending => Spinner()
    .failed(e) => ErrorView(e) { on retry { user.reload() } }
    .ready(u) => ProfileCard(u)
  }
}
```

More often the content is its own already: a screen pushed per id, a
row per key, each with a `load` of its own.

### Changes shown at once

A toggle, a rename, a like: the screen should show the change before
the server says so, and take it back if the save fails.
`x.optimistic(f)` shows what `x` shows, changed by `f`, at once:

```asuka check
struct Todo {
  id: int
  title: string
  done: bool = false
}

async fn fetchTodos() -> [Todo] {
  await sleep(100ms)
  return [Todo(id: 1, title: "Milk"), Todo(id: 2, title: "Bread")]
}

async fn saveDone(id: int, done: bool) throws {
  await sleep(100ms)
}

fn marked(t: Todo, done: bool) -> Todo {
  var c = t
  c.done = done
  return c
}

screen Todos {
  let todos = load fetchTodos()

  match todos {
    .pending => ProgressRing()
    .failed(e) => Text(e.message)
    .ready(ts) => List {
      for t in ts key t.id {
        Toggle {
          on: t.done
          title: t.title
          on change(done) {
            let id = t.id
            todos.optimistic(ts => ts.map(x => if x.id == id { marked(x, done) } else { x }))
            try await saveDone(id, done)   // a throw undoes it
            await reload fetchTodos()      // its answer replaces it
          }
        }
      }
    }
  }
}
```

- The change is a layer over the load's answer. `match`, `.value` and
  the rows see it; what reads the paths it changed runs again, nothing
  else: the toggle above runs its own row.
- It goes at the first of: the load's next answer, from a run that was
  in flight or started after it (a `reload()`, a change of what its
  arguments read), shown in the same frame, so the old value never
  flashes back; a throw out of the handler, the `fn` or the `task` that
  made it, undone before the error goes on; and the end of that code
  with no run in flight or asked for. That last one is a mistake, which
  a development build says (optimistic-unconfirmed): ask the load again
  before it ends.
- Several stack in the order they were made, and each goes on its own:
  two toggles in flight, the first one failing, undo only the first.
- `f` is a function of what's shown: it runs again when what's under it
  changes, so it reads no state and writes none
  ([optimistic-impure](../errors/optimistic-impure.md)). Read what it
  needs into a `let` first, as `id` above.
- On a load that isn't `.ready` there's nothing to change, and it does
  nothing.

### Reloading what a change touched

A note renamed on its edit screen: the list screen under it has a load
of its own, which `note.reload()` doesn't reach. `reload` names what a
change touched by the call that reads it, and every load of that call
asks again, wherever it is: any component, any screen in any stack,
any window.

```asuka check
struct Note {
  id: int
  title: string
}

store Api {
  state notes = [Note(id: 1, title: "Milk")]

  async fn list() -> [Note] {
    await sleep(100ms)
    return notes
  }

  async fn note(id: int) -> Note {
    await sleep(100ms)
    return notes.find(n => n.id == id) ?? Note(id: id, title: "")
  }

  async fn rename(id: int, title: string) {
    await sleep(100ms)
    if let i = notes.index(n => n.id == id) { notes[i].title = title }
  }
}

screen Notes {
  use api: Api
  let notes = load api.list()

  List {
    for n in notes.value ?? [] key n.id {
      Button(n.title) { on press { push(EditNote(n.id)) } }
    }
  }
}

screen EditNote(id: int) {
  use api: Api
  let note = load api.note(id)

  Button("Rename") {
    on press {
      await api.rename(id, "Oat milk")
      await reload api.list       // the list under this screen
      await reload api.note(id)   // this note's loads, not the others'
      pop()
    }
  }
}

app {
  provide Api()
  Stack { Notes() }
}
```

- `reload f(args)` reaches the loads of `f` whose arguments are equal
  to `args` (`==`, as dependencies are compared); `reload f`, every
  load of `f`, whatever its arguments. A store's method reaches the
  loads of every instance's.
- Each one runs as `x.reload()` does: it holds what it shows, and its
  `.loading` is true until it answers.
- `await reload …` waits until every load it reached has answered or
  failed (one that goes meanwhile counts): their failures are each
  load's to show, and it throws only when the waiting is cancelled.
  Without `await`, the loads run and the code goes on.
- The call is the key: a load of a function or a store's method. One
  of a closure's call, or of a value's method, has its `x.reload()`.

### Loads that land together

A profile and its posts, loaded side by side, swap in their answers one
at a time when `id` changes: for a moment the new person shows over the
last one's posts. `load (a(…), b(…))` is one load of several calls,
whose answers are shown together:

```asuka check
struct User {
  name: string
}

async fn fetchUser(id: string) -> User {
  await sleep(100ms)
  return User(name: "Ana")
}

async fn fetchPosts(id: string, page: int) -> [string] {
  await sleep(300ms)
  return ["Hello"]
}

screen Profile(id: string) {
  state n = 1
  let page = load (fetchUser(id), fetchPosts(id, page: n))   // shown together

  Column {
    match page {
      .pending => ProgressRing()
      .failed(e) => Text(e.message)
      .ready((u, posts)) => Column {
        Text(u.name) { font: .title }
        for p in posts key p { Text(p) }
      }
    }
    Button("More") {
      disabled: page.loading
      on press { n += 1 }
    }
  }
}
```

- It's a `Loading<(A, B, …)>`, matched as any other: `.ready((u, posts))`.
- Each call keeps its own dependencies: `n` above runs `fetchPosts`
  alone, and the user stays. A change of `id` runs both, and the new
  user is held until the new posts land.
- `.pending` until every call answered once; `.failed(e)` when one
  failed, the first in source order, `.value` keeping the last whole
  tuple; `.loading` while any call runs.
- `page.reload()` runs every call; `reload fetchPosts(id, page: n)` runs
  that one; `page.optimistic(p => (p.0, p.1 + [draft]))` changes the
  tuple.
- Each element is a call: a value has nothing to wait for
  ([group-not-calls](../errors/group-not-calls.md)).
