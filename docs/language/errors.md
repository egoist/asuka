# Errors

A function that can fail says `throws`, and calling it needs `try`:

```asuka
enum MailError: Error { offline, server(code: int) }

async fn send(m: Message) throws {
  if !network.online { throw MailError.offline }
  …
}

on press {
  do {
    try await send(draft)
    dismiss()
  } catch MailError.offline {
    showOffline = true
  } catch e {
    alert("Couldn't send", message: e.message)
  }
}

let n = try? parse(text)   // none if it throws
```

`catch` after an expression gives a fallback: the expression's value,
or when what's in it throws, the value of the catch's block (its last
expression, as an `if`'s), unless the block leaves:

```asuka
let port = try parsePort(text) catch e {
  log(e.message)
  80
}
let size = try await fileSize(path) catch { return }
try save(draft) catch { failed = true }   // a statement: no value to give
```

Its clauses are `do`'s: `catch`, `catch e`, `catch e: ParseError`,
`catch MailError.server(code)`, several in a row; the first that takes
the error runs. An error none takes goes on, so the function must throw,
as for `try`. One whose clauses take every error can't throw, so it goes
where a `try` can't: in a component's `let` (`let port = try
parsePort(text) catch { 80 }`, kept up to date as `text` changes), or a
closure that mustn't throw. It works with `try await`, and with `throw`
as a value: `let user = cache[id] ?? throw MailError.missing(id)`.

An error's `message` is its `message` field, its `fn message() ->
string`, or the error as it's written: its type's and case's names and
its payload, each value as interpolation writes what's inside another
(`CountError.tooMany(limit: 2)`, `ParseError(line: 3, text: "x")`,
`MailError.offline`), as the debugger shows it, cut short as the
debugger cuts a value (past 200 bytes, a string past 120, a list past
10 items, parts nested deeper than 3: `…`). It's made when it's read.
`Error("…")` makes an error of a message. A `catch` never sees a task's cancellation: it goes on to the
task's end, running the `defer`s on its way.

An `Error` is matched by an error type's cases too, as a `catch` takes
them: `match e { MailError.offline => …; MailError.status(code) => …; _
=> e.message }` (an `Error` can be any error, so the match ends with
`_`).

An error a handler or a task doesn't catch goes to the nearest
`Boundary` around the component, and to the app's (shown in
development, logged in release) when there is none:

```asuka
Boundary {
  Feed()
  on error(e) { Text("Something went wrong: {e.message}") }
}
```
