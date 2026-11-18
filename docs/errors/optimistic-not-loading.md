# optimistic-not-loading

`optimistic` on what isn't a load.

An error the compiler reports: the program doesn't build until it's fixed.

## Why

`x.optimistic(f)` shows a change of what a load shows until the load's next answer replaces it: it's a layer over the source of truth. A state is its own source of truth, with nothing coming to replace a change.

## How to fix

Change the state itself (`notes.push(n)`), or call `optimistic` on the load's `let`.

## Wrong

```asuka
struct Note {
  title: string
}

component Notes {
  state notes: [Note] = []
  Column {
    for n in notes key n.title {
      Text(n.title)
    }
    Button("Add") {
      on press { notes.optimistic(ns => ns + [Note(title: "new")]) }
    }
  }
}
```

## Right

```asuka
struct Note {
  title: string
}

component Notes {
  state notes: [Note] = []
  Column {
    for n in notes key n.title {
      Text(n.title)
    }
    Button("Add") {
      on press { notes.push(Note(title: "new")) }
    }
  }
}
```
