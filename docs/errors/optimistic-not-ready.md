# optimistic-not-ready

`optimistic` on a load that isn't ready.

Reported by the core as the app runs, in development builds (`asuka dev`, `asuka run`, `asuka test`), once per place in the code: release builds don't check it.

## Why

An optimistic change is a function of what a load shows. Pending (before its first answer) or failed, it shows nothing to change: the call does nothing.

## How to fix

Change what it shows once it's `.ready`: offer the action in the `.ready` arm, or check `x.value` first.

## Wrong

```asuka
async fn fetchCount() -> int {
  await sleep(10ms)
  return 1
}

component Counter {
  let count = load fetchCount()
  Column {
    Text("{count.value ?? 0}")
    Button("Add") {
      on press {
        count.optimistic(n => n + 1)
        count.reload()
      }
    }
  }
}

test "a change before the answer" {
  let app = mount(Counter())
  app.tap("Add")
  await app.idle()
  app.expect("1")
}
```

## Right

```asuka
async fn fetchCount() -> int {
  await sleep(10ms)
  return 1
}

component Counter {
  let count = load fetchCount()
  Column {
    match count {
      .pending => Text("…")
      .failed(e) => Text(e.message)
      .ready(n) => Button("{n}") {
        on press {
          count.optimistic(c => c + 1)
          count.reload()
        }
      }
    }
  }
}

test "a change of what it shows" {
  let app = mount(Counter())
  await app.idle()
  app.tap("1")
  await app.idle()
  app.expect("1")
}
```
