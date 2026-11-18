# optimistic-unconfirmed

An optimistic change went with its task, with no run of its load to confirm it.

Reported by the core as the app runs, in development builds (`asuka dev`, `asuka run`, `asuka test`), once per place in the code: release builds don't check it.

## Why

`x.optimistic(f)` shows a change until the load answers again. When the task that made it ends and the load isn't running or asked to (no `reload`), nothing will ever confirm it: keeping it would hide every later answer, so it goes, and the screen takes back what it showed.

## How to fix

Before the task ends, ask the load again: `await reload f(…)`, or `x.reload()`.

## Wrong

```asuka
async fn fetchCount() -> int {
  await sleep(10ms)
  return 1
}

async fn save(n: int) {
  await sleep(10ms)
}

component Counter {
  let count = load fetchCount()
  Column {
    Text("{count.value ?? 0}")
    Button("Add") {
      on press {
        count.optimistic(n => n + 1)
        await save(2)
      }
    }
  }
}

test "a change nothing confirms" {
  let app = mount(Counter())
  await app.idle()
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

async fn save(n: int) {
  await sleep(10ms)
}

component Counter {
  let count = load fetchCount()
  Column {
    Text("{count.value ?? 0}")
    Button("Add") {
      on press {
        count.optimistic(n => n + 1)
        await save(2)
        count.reload()
      }
    }
  }
}

test "a change its load's answer replaces" {
  let app = mount(Counter())
  await app.idle()
  app.tap("Add")
  await app.idle()
  app.expect("1")
}
```
