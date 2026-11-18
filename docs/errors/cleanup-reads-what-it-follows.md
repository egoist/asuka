# cleanup-reads-what-it-follows

An effect's cleanup that reads what the effect follows.

A warning the compiler reports: the program builds, and it's worth a look.

## Why

An effect's `cleanup { }` runs before the effect's next run, which happens because what it follows changed. Reading that in the cleanup reads the new value: `cleanup { leave(room) }` leaves the room being joined, not the one the run joined.

## How to fix

Keep the run's value in a `let` and read that in the cleanup: `let r = room`, then `cleanup { leave(r) }`.

## Wrong

```asuka
component Room {
  prop room = "lobby"
  effect(room) {
    print("join {room}")
    cleanup { print("leave {room}") }
  }
  Text(room)
}
```

## Right

```asuka
component Room {
  prop room = "lobby"
  effect(room) {
    let r = room
    print("join {r}")
    cleanup { print("leave {r}") }
  }
  Text(room)
}
```
