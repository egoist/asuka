# reload-arguments

`reload f(…)` with arguments `f` doesn't take.

An error the compiler reports: the program doesn't build until it's fixed.

## Why

`reload f(a)` reaches the loads of `f` whose arguments equal `a`: they're compared as `f`'s parameters, so they're checked as a call of `f` is.

## How to fix

Give the arguments `f` takes, as its loads call it, or none: `reload f` reaches every load of `f`.

## Wrong

```asuka
async fn fetchNote(id: int) -> string {
  await sleep(1ms)
  return "note {id}"
}

component Note(id: int) {
  let note = load fetchNote(id)
  Button("Refresh") {
    on press { await reload fetchNote("{id}") }
  }
}
```

## Right

```asuka
async fn fetchNote(id: int) -> string {
  await sleep(1ms)
  return "note {id}"
}

component Note(id: int) {
  let note = load fetchNote(id)
  Button("Refresh") {
    on press { await reload fetchNote(id) }
  }
}
```
