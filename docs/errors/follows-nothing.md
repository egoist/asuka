# follows-nothing

`state x()` with nothing to follow.

An error the compiler reports: the program doesn't build until it's fixed.

## Why

The parentheses of `state x(a, b)` say what the state follows. Empty, it would follow nothing, which is what a value read once (`peek`) already says.

## How to fix

Name what to follow, `state x(a) = …`, or write `state x = peek(…)`.

## Wrong

```asuka
component Counter {
  state n() = 0
  Text("{n}")
}
```

## Right

```asuka
component Counter {
  state n = 0
  Text("{n}")
}
```
