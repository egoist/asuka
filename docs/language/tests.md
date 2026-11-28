# Tests

```asuka
test "archiving removes the row" {
  let app = mount(Inbox(), provide: Mailbox.fake([sample]))
  await app.idle()
  app.tap("Archive")
  app.expectNo("Hello")
}
```

`expect(cond)` fails a test when cond is false, saying what failed and
where (with a comparison's two sides). JSON comes from `std/json`:
`let notes: [Note] = try json.decode(text)` reads what's expected of it
(or `json.decode<[Note]>(text)`), `json.encode(notes)` writes it. Where a `json.Value` (any JSON) is
expected, a value JSON can say is one, as it's written: a map of values
of mixed types is a `[string: json.Value]` (`["port": port, "tls":
true]`), whose numbers stay numbers. A
generic function may read or write its `T` as JSON; each call then
needs a `T` JSON can say, which the compiler checks where it's called.

`asuka test` runs a package's tests with no device: it lays views out as
the platforms do, measures text at a fixed width per character, and time
moves only when nothing else can run (a `sleep(5s)` in a test takes no
time). `mount` returns the app, to drive and check: `tap`, `longPress`,
`type(_, into:)`, `submit`, `toggle`, `slide`, `select`, `swipe(_,
action:)`, `refresh`, `scrollTo`, `back`, `dismiss`, `pickDate`, `pickRange`, `remove`, `chooseSuggestion`,
`pickColor`, `expect`, `expectNo`, `texts()`, `disabled(text)`,
`until`, `waitFor`, `idle`; `in(.navigationBar)`, `in(.sidebar)` and
`nextTo(label)` narrow what a text names; with a router, `mount(…, at:
"/inbox/1")`, `href`, `history()`, `goBack()`, `goForward()`. See
[Testing](../testing.md).

Tests go in any file, and in a package's **test files**, whose names
end in `_test.asuka` (`parser_test.asuka`; `tray_linux_test.asuka` for
one platform). A test file's declarations and imports, its fixtures and
helpers, are part of the package only when its own tests run (`asuka
test`, `asuka check`, an editor): a build has none of them, nor any
`test`, `bench` or `preview` block ([Previews](components.md#previews)),
and a package that imports this one never sees them. So a
test file may import what only its tests need without that coming with
the app.

A test with parameters is a property test, run with values `asuka test`
generates for them; a failing input is shrunk to the simplest that still
fails. `bench "name" { … }` is a benchmark, timed by `asuka test
--bench`; its `measure { … }` is the part timed, after what sets it up,
and `keep(x)` keeps a result from being optimized out.
`expectSnapshot(value)` and `expectInlineSnapshot(value, "…")` compare
what a value prints as with its snapshot, which `asuka test` writes.

```asuka check
fn fib(n: int) -> int {
  if n < 2 { return n }
  return fib(n - 1) + fib(n - 2)
}

test "fib grows" (n: int) {
  if n < 2 || n > 30 { return }
  expect(fib(n) >= fib(n - 1))
}

test "the first ones" {
  expectInlineSnapshot([fib(1), fib(2), fib(3), fib(10)], "[1, 1, 2, 55]")
}

bench "fib 20" {
  keep(fib(20))
}
```
