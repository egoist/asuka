# long-hold

A load has held old content for seconds with nothing saying so.

Reported by the core as the app runs, in development builds (`asuka dev`, `asuka run`, `asuka test`), once per place in the code: release builds don't check it.

## Why

A load that runs again keeps what it showed until the new answer lands (its `.ready` stays), and a grouped load's call that answered holds its answer until its slower siblings do. That keeps the screen still, but when the answer takes seconds and nothing reads `.loading`, the user looks at the last answer with no sign that another is coming.

## How to fix

Read `x.loading` while it holds (dim the content, show a ring), or show a spinner in its place.

## Wrong

```asuka
async fn fetchName(id: int) -> string {
  await sleep(3s)
  return "user {id}"
}

component Profile {
  state id = 1
  let user = load fetchName(id)
  Column {
    match user {
      .pending => Text("…")
      .failed(e) => Text(e.message)
      .ready(name) => Text(name)
    }
    Button("Next") { on press { id += 1 } }
  }
}

test "a long hold" {
  let app = mount(Profile())
  await app.idle()
  app.expect("user 1")
  app.tap("Next")
  await app.idle()
  app.expect("user 2")
}
```

## Right

```asuka
async fn fetchName(id: int) -> string {
  await sleep(3s)
  return "user {id}"
}

component Profile {
  state id = 1
  let user = load fetchName(id)
  Column {
    match user {
      .pending => Text("…")
      .failed(e) => Text(e.message)
      .ready(name) => Text(name) { opacity: if user.loading { 0.5 } else { 1 } }
    }
    Button("Next") { on press { id += 1 } }
  }
}

test "a hold that says so" {
  let app = mount(Profile())
  await app.idle()
  app.expect("user 1")
  app.tap("Next")
  await app.idle()
  app.expect("user 2")
}
```
