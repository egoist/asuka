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
  `await never()` waits for nothing but the task's end: a task that
  holds something while its component is shown (`defer { release() };
  await never()`), or a stream that ends only when its reader goes.
- `spawn { … }` runs a block on another thread, for work that would hold
  the main thread up (parsing, image processing). It may use values only,
  not state, stores, views, native objects or C pointers, nor a file's
  `let` that holds one (a database): the compiler says so if it does. On
  the web it runs on one of the page's Web Workers, which make the
  file's values for themselves, and nothing else.
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
- **Cancellation.** A task belongs to what started it. When that goes (the
  user leaves the screen), the task is cancelled: the `await` it's at
  ends it, its `defer`s run, and no error is reported.

## Data that loads

`load` keeps an async result in a component:

```asuka
screen Profile(id: string) {
  let user = load api.user(id)   // reloads when id changes

  match user {
    .pending => Spinner()
    .failed(e) => ErrorView(e) { on retry { user.reload() } }
    .ready(u) => ProfileCard(u)
  }
}
```

A `Loading<T>` is `.pending`, `.ready(value)` or `.failed(error)`;
`.value` is the latest value (kept while it reloads, so the screen
doesn't flash), `.reload()` runs it again. When what its arguments read
changes, it runs again and the run that's going stale is cancelled.
