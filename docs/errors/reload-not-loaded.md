# reload-not-loaded

`reload` of what no load calls.

An error the compiler reports: the program doesn't build until it's fixed.

## Why

`reload f(…)` asks again the loads whose operand is a call of `f`: the call is the key, by the function it calls. A function no `load` in the program calls, or what isn't a function (a closure, a value's method), reaches nothing.

## How to fix

Reload the function a `load` calls (`reload api.notes`), or a load by its handle (`x.reload()`).

## Wrong

```asuka
async fn fetchNotes() -> [string] {
  await sleep(1ms)
  return []
}

async fn fetchTags() -> [string] {
  await sleep(1ms)
  return []
}

component Notes {
  let notes = load fetchNotes()
  Button("Refresh") {
    on press { await reload fetchTags() }
  }
}
```

## Right

```asuka
async fn fetchNotes() -> [string] {
  await sleep(1ms)
  return []
}

component Notes {
  let notes = load fetchNotes()
  Button("Refresh") {
    on press { await reload fetchNotes() }
  }
}
```
