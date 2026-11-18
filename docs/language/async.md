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

- `.value` is the latest value, none before the first. A run that fails
  keeps it, so `.failed` can show the old content beside the error.
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
