# format-spec

An interpolation's format that doesn't fit its value.

An error the compiler reports: the program doesn't build until it's fixed.

## Why

A format after a colon in an interpolation (`"{price:>8.2f}"`) says how its value is written: `[[fill]align][sign][#][0][width][,|_][.precision][type]`, as Python's and Rust's say. A sign, `#`, `0`, grouping and a type are a number's (`x`, `X`, `o`, `b` an int's digits, `f`, `e`, `%` a float's); other values are written as their text, which takes a fill, an alignment, a width and a precision (the most characters). The compiler reads the format, so a mistake shows where it's written.

## How to fix

Write the format for the value's type, as the message says: `{n:>6}`, `{x:.2f}`, `{n:#x}`, `{name:<12}`.

## Wrong

```asuka
fn price(cents: int) -> string = "{cents:.2}"
```

## Right

```asuka
fn price(cents: int) -> string = "{float(cents) / 100:.2f}"
```
