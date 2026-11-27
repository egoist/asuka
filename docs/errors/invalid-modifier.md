# invalid-modifier

`pub`, `mut`, `async`, `throws` or type parameters where they don't apply.

An error the compiler reports: the program doesn't build until it's fixed.

## Why

A `mut fn` changes the value it's called on, so a store's fns (which change its state as they are) and a static fn (which has no value) don't take it, and it can't be `async`. A worker's fns don't await, a C function isn't generic, async or throwing, and a test or a preview isn't `pub`.

## How to fix

Drop the modifier the message names.

## Wrong

```asuka
store Counter {
  state n = 0
  pub mut fn bump() { n += 1 }
}
```

## Right

```asuka
store Counter {
  state n = 0
  pub fn bump() { n += 1 }
}
```
