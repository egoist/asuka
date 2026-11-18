# Programs

Besides apps and libraries, Asuka makes **programs**: command-line
tools, with no window and no views. A program is a package whose files
declare `fn main()`, as Go's `package main` and Cargo's `src/main.rs`
are. It's built as an app is: the same language, the same
standard library (files, JSON, time, SQLite, HTTP...), compiled to a
native executable for macOS, Windows and Linux.

```asuka
import std/os

fn main() {
  let name = os.args().first() ?? "world"
  print("Hello, {name}!")
}
```

## Making one

```sh
asuka new --bin greet   # asuka.toml, and main.asuka with main and a test
cd greet
asuka run -- Ada        # Hello, Ada!
asuka test              # its test blocks
asuka build             # an executable, dist/greet
```

`asuka new` makes an app, `asuka new --lib` a library, `asuka new
--bin` a program. A project that's an app can ship programs too, in
directories of their own: see [An app's programs](#an-apps-programs).

## main

`fn main()` is where the program starts. It takes no parameters (the
arguments are `os.args()`) and returns nothing (`os.exit(code)` says the
exit code); it may be `async` and may throw. A package with `fn main`
has no `app` block and declares no components: a program shows no
views. Only the package a program is made of has an entry: a `main` in
a package it imports is a function as any other.

```asuka
import std/fs

async fn main() throws {
  let text = try await spawn { try fs.readText("notes.txt") }
  print("{text.lines().count()} lines")
}
```

The program runs as follows:

- The packages' values are made (their `let`s), then `main` runs. An
  async `main` runs as a task: `await` lets other work run while it
  waits, as in an app, and nothing holds the main thread.
- When `main` returns, the program ends with the exit code 0. What it
  started and left running (a stream it was reading, a task of a
  package's) is cancelled first: their `defer`s run.
- An error `main` throws, or doesn't catch, ends it with 1: `error: `
  and the error's message go to stderr, with where it was thrown, the
  innermost function first:

  ```text
  error: bad name fail
      check (main.asuka:18)
      main (main.asuka:8)
  ```

- `os.exit(code)` ends it at once with `code`: stdout and stderr are
  flushed, but `defer`s and other tasks don't run.
- `print` writes a line to stdout; `os.eprint` to stderr.

## The process: std/os

| Function | What it does |
|---|---|
| `os.args()` | the arguments, without the program itself |
| `os.program()` | the program as it was started (`argv[0]`) |
| `os.env(name)` | an environment variable's value, `none` when it isn't set |
| `os.environ()` | every environment variable, `[string: string]` |
| `os.cwd()` | the working directory |
| `os.exit(code)` | ends the process at once (its type is `never`) |
| `os.readLine()` | a line of stdin without its line ending, `none` at its end; it waits for the line |
| `os.readAll()` | the rest of stdin, as `bytes` |
| `os.lines()` | stdin's lines as they come (a `Stream<string>`), read on another thread: the program goes on meanwhile |
| `os.eprint(text)` | a line on stderr |
| `os.writeTo(.stdout, text)`, `writeBytesTo` | text or bytes as they are, without a line ending, to `.stdout` or `.stderr` |
| `os.terminal(.stdout)` | whether a stream is a terminal (a person), not a pipe or a file: colors and prompts only for one |

```asuka
import std/os

async fn main() throws {
  var total = 0
  for await line in os.lines() {
    total += line.toInt() ?? 0
  }
  print(total)
  if os.env("DEBUG") != none {
    os.eprint("summed stdin")
  }
}
```

On a phone and the web, an app's `os.args()` is empty, stdin is empty
and `os.exit` throws: an app there can't end its own process. A desktop
app's `os.args()` are its own process's.

## Command-line flags: std/flag

`flag.parse<T>(os.args())` reads the command line into a `T`, from what
the compiler knows of `T`: no table of flags to keep in step with the
fields.

```asuka
import std/flag
import std/os

enum Unit { lines, words, bytes }

/// Counts what's in files.
struct Options {
  /// -v Say each file's count.
  verbose: bool = false
  /// -u What to count.
  unit: Unit = .lines
  /// How many files at once.
  jobs: int = 4
  /// Names to leave out.
  skip: [string] = []
  /// The files to count.
  files: [string]
}

fn main() {
  let o = flag.parse<Options>(os.args())
  print("{o.unit} in {o.files.count()} files, {o.jobs} at once")
}
```

- A field **with a default** is an option: `dryRun` is `--dry-run`. Its
  doc comment is its line in the help; a first word `-x` (a letter or a
  digit) is its short name.
- A `bool` is a switch: `--verbose`, `-v`; `--no-color` turns off one
  that's on by default (`color: bool = true`), and `--color=false` says
  it too.
- An `int`, a `float`, a `string`, or an enum's case without a payload
  (`--unit words`: its name in kebab case) takes a value: `--jobs 8`,
  `--jobs=8`, `-j8`, `-j 8`. An array may be given again and again
  (`--skip a --skip b`); an optional may be left out.
- Short switches group: `-vf` is `-v -f`, and `-vu words` gives `-u`
  its value.
- A field **without a default** is an argument, in the order of the
  fields: an array takes the rest (it's the last), an optional may be
  left out (after the ones that may not).
- `--` ends the options: what comes after is arguments, `-x` or not.
- `-h` and `--help` print the help and exit with 0; `--version` prints
  asuka.toml's `version` and exits with 0.
- A mistake prints what's wrong, with the usage, and exits with 2:

  ```text
  error: unknown option --jbos (did you mean --jobs?)

  Usage: wc [options] [<files>...]

  For more, try 'wc --help'.
  ```

The struct's doc comment is the help's first line:

```text
Counts what's in files.

Usage: wc [options] [<files>...]

Arguments:
  [<files>...]  The files to count.

Options:
  -v, --verbose                   Say each file's count.
  -u, --unit <lines|words|bytes>  What to count. (default: lines)
      --jobs <n>                  How many files at once. (default: 4)
      --skip <skip>...            Names to leave out.
  -h, --help                      Show this help.
      --version                   Show the version.
```

What the parser can't take is a compile error at the field: a default
that isn't written out (a function's result), a map, `-h` as a short
name, a switch without a default. `flag.tryParse<T>(args)` is
`parse`'s work without printing or exiting: it throws a
`flag.FlagError` (`.help(text)`, `.version(text)`, `.usage(problem,
usage, program)`); `flag.help<T>()` is the help.

### Commands

An enum whose cases carry structs of options is a set of commands, as
`git add` and `git commit` are: `flag.parse<Command>(args)`, or a field
of a struct of the enum's type, which comes after the struct's own
options. A case's doc comment is its line in the help; `tool add
--help` shows the command's own.

```asuka
import std/flag
import std/os

struct AddOptions {
  /// -f Add ignored files too.
  force: bool = false
  /// The files to add.
  paths: [string]
}

struct CommitOptions {
  /// -m The commit's message.
  message: string = ""
}

/// A tiny version control.
enum Command {
  /// Add files to the index.
  add(AddOptions)
  /// Record the index.
  commit(CommitOptions)
  /// Show the working tree's state.
  status
}

struct Cli {
  /// -q Say less.
  quiet: bool = false
  command: Command
}

fn main() {
  let cli = flag.parse<Cli>(os.args())
  match cli.command {
    .add(o) => print("adding {o.paths}")
    .commit(o) => print("committing: {o.message}")
    .status => print("on main")
  }
}
```

## Other programs: std/command

`std/command` runs other programs. Their arguments are a list, passed
as they are, never through a shell: a file name with a space or a `;` is
one argument. What blocks (reading a pipe, waiting for the end) happens
on other threads, so the main thread doesn't wait.

```asuka
import std/command

async fn main() throws {
  let out = try await command.run("git", ["status", "--short"])
  print(out.text())

  let p = try command.start("ping", ["-c", "3", "example.com"])
  for await line in p.lines() {
    print("> {line}")
  }
  print("ping exited with {await p.wait()}")
}
```

- `command.run(program, args, cwd:, env:, clearEnv:, stdin:, check:)`
  runs a program to its end and returns an `Output`: its `code`,
  `stdout` and `stderr` (bytes; `text()` and `errorText()` as text,
  `ok()`). `stdin:` is given to it; `check: true` makes a code that
  isn't 0 a `CommandError.failed`.
- `command.status(program, args, …)` runs one with the program's own
  stdin, stdout and stderr (what it prints shows) and returns its code.
- `command.start(program, args, …, stdin:, stdout:, stderr:)` starts one
  and returns a `Process`: its `pid`, `stdout()` and `stderr()` (streams
  of bytes) or `lines()` and `errorLines()`, `write(text)` and
  `closeStdin()`, `wait()`, `output()` (everything, then the code) and
  `kill(.term)`. Each stream is a `Stdio`: `.piped` (the default for
  stdout and stderr), `.inherit` or `.null` (stdin's default).
- A program is looked for on `PATH` unless it's a path; `env:` goes over
  the program's environment (`clearEnv: true`: it alone). No such
  program is `CommandError.notFound`.
- A task that's cancelled while it waits for a program (`run`,
  `output`, `wait`) ends the program too.
- An exit code a signal caused is 128 and the signal's number on macOS
  and Linux. On Windows a program is found as `CreateProcess` finds it
  (`.exe` added); a `.bat` or `.cmd` runs through `cmd /c` given as the
  program.

There's no `command.shell("…")`: a shell reads quotes, `$` and `;` in
the text it's given, which is how a file name becomes a command. To run
a shell's line on purpose, run the shell: `command.run("sh", ["-c",
line])`.

It runs in programs and in desktop apps (macOS, Windows, Linux); an app
on a phone or the web can't run other programs
(`CommandError.unsupported`).

## The terminal: std/term

`std/term` styles what a program prints and asks the person at the
terminal. This one makes a project: a name asked for, a question before
it overwrites, and colors a `--color` flag can turn on or off.

```asuka check
import std/flag
import std/fs
import std/os
import std/term

/// Makes a project.
struct Options {
  /// When to use colors.
  color: term.ColorMode = .auto
  /// Where to make it (the name asked for, if not given).
  dir: string?
}

async fn main() throws {
  let o = flag.parse<Options>(os.args())
  term.setColors(o.color)
  let name = await term.prompt("Project name:", default: "demo") ?? "demo"
  let target = try fs.abs(o.dir ?? name)
  if fs.exists(target) && !(await term.confirm("{target} exists. Overwrite?")) {
    os.eprint("{term.red("error:", stream: .stderr)} nothing was made")
    os.exit(1)
  }
  try await spawn { try fs.mkdirAll(target) }
  print("{term.green("✓")} made {term.bold(target)}")
}
```

```text
$ new
Project name: [demo] notes
✓ made /home/ann/notes
```

- **Style.** `term.bold(text)`, `dim`, `italic`, `underline` and the
  eight colors (`red`, `green`, `yellow`, `blue`, `magenta`, `cyan`,
  `white`, `black`) wrap text in the terminal's codes. `term.Style`
  says several at once and is a value to keep: `let warn =
  term.Style(color: .yellow, bold: true)`, then `warn.paint(text)`. It
  has the bright colors (`.brightRed`) and a `background`. Styled text
  inside styled text keeps its own style, and the outer one goes on
  after it.
- **When styles show.** Only when the stream is a terminal, `NO_COLOR`
  isn't set ([no-color.org](https://no-color.org)) and `TERM` isn't
  `dumb`; `FORCE_COLOR` turns them on even into a pipe (`0` or `false`:
  off). Each style function takes `stream:` (`.stdout` unless said),
  the stream it's printed to. `term.colors(.stderr)` says what was
  decided, and `term.setColors(.always)`, `.never` or `.auto` overrides
  it, for both streams or for one (`stream:`). A field `color:
  term.ColorMode = .auto` is a `--color always|never|auto` flag. On
  Windows, the console is set to show the codes (its virtual terminal
  processing) the first time.
- **Questions.** `term.prompt(question, default:)` asks and returns the
  line typed, without spaces around it: an empty answer is `default`
  (shown as `[demo]`), and `none` means stdin ended (Ctrl-D, a closed
  pipe). `term.confirm(question, default: false)` asks `[y/N]` (`[Y/n]`
  when the default is true) until it gets y, yes, n, no or nothing.
  `term.secret(question)` reads a line with the terminal's echo off, for
  a password or a token; the echo comes back when the line is read, when
  the task is cancelled and when Ctrl-C ends the program. The questions
  go to stderr, as git's and gh's do, so stdout stays the program's
  output when it's piped (`new | tee log`). They're `async`: the line is
  read on another thread, and the program's other tasks go on.
- **Size.** `term.width()` and `term.height()` are the terminal's
  columns and rows (`COLUMNS` and `LINES` first, when they're numbers),
  `none` when no stream is a terminal: `term.width() ?? 80`.

A test answers the questions with `os.fake(stdin:)`, and finds them in
`o.stderr()`. Its streams aren't terminals, so nothing is styled, unless
the test says otherwise with `os.fake(env: ["FORCE_COLOR": "1"])` or
`term.setColors(.always)`; only `os.fake`'s `env:` counts, so the
environment the tests run in doesn't change what they print. On a phone
and the web there's no terminal: no styles, `prompt` gives `none` and
`width` is `none`.

## Files and directories: std/fs

A program reads and writes files with `std/fs`, as an app does
([Data](data.md)), and finds them with what a command-line tool needs:

- `fs.home()`: the user's home directory (`HOME`, `USERPROFILE` on
  Windows; in a test, `os.fake`'s `env:` when it has them).
- `fs.tempDir()`: the directory for temporary files (`TMPDIR`, else
  `/tmp`; on Windows, `GetTempPath`'s).
- `fs.abs(path)`: `path` made absolute against the working directory,
  and cleaned.
- `fs.walk(dir, skip:)`: everything under a directory, at every depth,
  as `Entry`s (`path`, `name`, `isDir`, `size`, `modified`): each
  directory, then what's in it, in name order. `skip:` names directories
  it doesn't go into; it doesn't go into links to directories either.

```asuka check
import std/fs

async fn main() throws {
  let root = try fs.abs(".")
  let entries = try await spawn { try fs.walk(root, skip: [".git", "node_modules"]) }
  let sources = entries.filter(e => !e.isDir && fs.ext(e.name) == ".asuka")
  print("{sources.count()} files in {root}")
  let config = fs.join([fs.home(), ".config", "count", "config.toml"])
  if fs.exists(config) {
    print("using {config}")
  }
}
```

The calls that touch the disk (`walk`, `readText`, `exists`...) block
until the system answers: a large tree is walked in `spawn { }`, so
the program's other tasks go on. A failure is an `FsError`, as for every
call of `std/fs`: a directory it may not list is `.denied(path)`.

## Many at once

`all(items, f, limit:)` calls an async function with each item at once
and gives the results in the items' order: downloads, requests, other
programs run side by side. `limit:` says how many run at a time; the
first error cancels the rest, and `all` throws it once they've stopped
([Async](language/async.md#async)).

```asuka check
import std/flag
import std/http
import std/os

/// Downloads pages, a few at a time.
struct Options {
  /// -j How many at once.
  jobs: int = 4
  /// The pages' URLs.
  urls: [string]
}

async fn main() throws {
  let o = flag.parse<Options>(os.args())
  let sizes = try await all(o.urls, u => try await http.get(u).body.count(), limit: o.jobs)
  for (u, n) in o.urls.zip(sizes) {
    print("{n}\t{u}")
  }
}
```

## JSON of any shape

A struct reads JSON whose shape the program knows (`json.decode<T>`,
[Data](data.md)). What it can't know, a tool's config or whatever is
piped in, is a `json.Value`: `json.parse(text)` reads any JSON as it is,
`get(key)`, `at(i)` and `asString()`, `asInt()`... look inside, and a
`match` takes its cases (`.object(members)`, `.array(elements)`,
`.string(s)`...). It prints, and `json.encode` writes it, as JSON.

```asuka check
import std/json
import std/os

/// Prints what's at a path in the JSON on stdin: pick user.emails.0
async fn main() throws {
  var v = try json.parse(os.readAll().string())
  for step in (os.args().first() ?? "").split(".") {
    if step.isEmpty() { continue }
    let next = if let i = step.toInt() { v.at(i) } else { v.get(step) }
    v = next ?? json.Value.null
  }
  match v {
    .string(s) => print(s)       // as text, not quoted
    _ => print(json.encode(v, indent: 2))
  }
}
```

## A web service: std/http

`http.serve` makes a program an HTTP/1.1 server, as Go's `net/http`
and `Bun.serve` do: each request goes to a handler, an async function
that returns a `Reply`. A `Router` sends requests to handlers by
method and path, `:name` taking a segment of the path.

```asuka check
import std/http
import std/os

struct Note {
  id: int
  text: string
}

struct Draft {
  text: string
}

async fn main() throws {
  var notes: [Note] = []
  let api = http.Router()
    .get("/notes", req => .json(notes))
    .get("/notes/:id", req => {
      if let n = notes.find(n => n.id == req.param("id").toInt()) {
        return .json(n)
      }
      return .text("no such note", status: 404)
    })
    .post("/notes", req => {
      let draft = try req.json<Draft>()
      let note = Note(id: notes.count() + 1, text: draft.text)
      notes.push(note)
      return .json(note, status: 201)
    })
  let port = (os.env("PORT") ?? "8080").toInt() ?? 8080
  let server = try await http.serve(port, api.handler(), host: "0.0.0.0")
  print("listening on {server.url}")
  await server.wait()
}
```

```sh
$ curl -d '{"text":"milk"}' localhost:8080/notes
{"id":1,"text":"milk"}
$ curl localhost:8080/notes/1
{"id":1,"text":"milk"}
```

- `http.serve(port, handler, host:, limits:, onError:)` listens at
  `port` (0: one the system picks, which `server.port` says) and
  returns the `Server` once it does. `host` is `127.0.0.1` unless said:
  only this computer's clients reach it. `"0.0.0.0"` takes every IPv4
  interface's (a container, another machine), `"::"` IPv6's too. An
  address it can't listen at (a port in use) is `ServerError.listen`.
- Each request's handler runs on the main thread as a task of its own,
  so it may await what any task does (a database, `http.get`, another
  program) while the others go on. Reading and parsing requests and
  writing replies happen on the server's own thread; many connections
  are served at once, and kept open between requests.
- A `Request` has its `method`, `path` (as sent, escapes kept),
  `rawQuery` and `query(name)`, `headers` by lower-case name and
  `header(name)` in any case, its `body` (read whole first), `text()`
  and `json<T>()`, the client's `remote` address, and the `params` a
  route matched (`param(name)`).
- A `Reply` is made by `.text`, `.json`, `.html`, `.data` and
  `.redirect`, or as `Reply(status: 204)`; a header's value with lines
  in it is sent once per line (several Set-Cookie). `.stream(body)`
  sends a body as it's made, in chunks, and `.events(stream)`
  server-sent events, which `http.events` and a browser's EventSource
  read; a client that goes away while they're sent cancels the handler.
- A route's pattern has text, `:name` (one segment) and `*name` (the
  rest of the path). Routes are tried in the order they were added; a
  path none has is answered 404, one whose routes take other methods 405
  (with `Allow`), and a GET route answers HEAD too.
- A handler that throws is answered 500 Internal Server Error, and its
  error goes to `onError` (stderr when there's none).
- `http.Limits(headerSize:, bodySize:, connections:, headerTimeout:,
  readTimeout:, idleTimeout:)` says how much it takes and how long it
  waits: 64 KiB of headers, a 16 MiB body, 10000 connections, 30 s for
  a request's headers and for each wait in its body, 60 s between
  requests. What goes past is answered (431, 413, 408) and its
  connection closed.
- `server.close(grace: 10s)` stops it: it stops accepting at once, lets
  the requests in flight be answered, and after `grace` cancels the
  handlers still running (their `defer`s run). `server.wait()` waits
  until it's closed. The server belongs to the task that started it,
  so when `main` returns it closes: `main` awaits `server.wait()`.

HTTPS is a reverse proxy's (Caddy, nginx): it terminates TLS and passes
the requests on over HTTP to a server on `127.0.0.1`, the client's
address in `X-Forwarded-For`. A desktop app serves a local API the same
way, from a task, and a phone's app while it runs; a web page can't
listen (`ServerError.unsupported`).

A handler is a function, so a test calls it, or the router, with a
`Request`, no socket between: `let r = try await
api.handle(http.Request(method: "GET", path: "/notes/1"))`.

## Running

`asuka run` runs a program's `main` at once, with no build to wait for: the arguments after `--` are its own, and it exits with the
program's code.

```sh
asuka run -- --jobs 8 a.txt b.txt
asuka run --bin notes -- add "Buy milk"   # one of asuka.toml's [bin] programs
```

`asuka dev` runs it, and runs it again with each change you save
(`cargo watch -x run`'s loop): a run still going when you save is
stopped first. For a file or a package without `fn main`, `asuka run`
runs its test blocks and shows what its app shows, as before.

## Building

`asuka build` compiles a program to an executable for this computer, in
`dist/`:

```sh
asuka build                          # dist/greet
asuka build --target linux-amd64     # dist/linux-amd64/greet
asuka build --target windows-arm64   # dist/windows-arm64/greet.exe
```

`--target` is `darwin`, `linux` or `windows`, and `amd64` or `arm64`;
another system than this computer's is built with Zig, as the desktop
apps are. The executable stands alone, as far as the system allows: on
Linux it's linked statically (musl, with Zig) unless what it uses loads the
system's libraries (std/http's libsoup); on
Windows it needs only the system's DLLs; on macOS, the system's
libraries.

A package with platform code works in a program on every desktop:
`std/http` sends its requests with URLSession on macOS, WinHTTP on
Windows and libsoup on Linux, as in an app, with no window. SQLite
(`std/sqlite`) is compiled into the executable, as into an app.

### An app's programs

An app can ship programs, as VS Code ships `code`: a command that works
on the app's data from a terminal. A package can't have both `app { }`
and `fn main()`, so each program is a directory of its own, named in
asuka.toml:

```toml
[bin.notes]
path = "cli"
```

The program imports the project's packages relatively, as any package
does (`import ../model`). `asuka run --bin notes` runs it (`--bin` can
be left out when it's the only one), `asuka test` runs every
program's tests with the app's, and `asuka build --bin notes` builds it
alone. An app's program keeps its data where the app does (std/fs's
`appDir`, a SQLite database in it), so the two read and write the same;
under development, `asuka run --bin` uses `asuka dev`'s.

A desktop app's build puts its programs in the app:

| Build | Where | On the user's PATH |
|---|---|---|
| macOS | `Notes.app/Contents/Helpers/notes`, for the app's architecture, signed with the app | a link the user makes, or the app offers to: `ln -s /Applications/Notes.app/Contents/Helpers/notes /usr/local/bin/notes` |
| Linux | the prefix's `bin/notes`, beside the app's, in the tarball and the Flatpak | installed in `~/.local/bin` or `/usr/bin` with the app; a Flatpak's as `flatpak run --command=notes <id>` |
| Windows | `notes.exe` beside the app's `.exe` | the app's folder added to `PATH` by its installer |

Phones and the web leave `[bin]` out. [examples/notes](../examples/notes)
is an app with a `notes` command, which lists, adds and checks off the
app's notes.

### Programs another toolchain builds

A program another toolchain builds goes in the app the same way: a
server written in Go, a tool in Rust, which the app runs. Its `[bin]`
names the command that builds it instead of a directory, run for the
target the app is built for. Placeholders name the target in each toolchain's own
words: `{goos}` and `{goarch}` for Go, `{target}`, the triple, for
Rust; `{os}` and `{arch}` are Asuka's (`macos`, `x64`), for a script of
your own. A build that can't write to `{out}`, as cargo can't, says
where it leaves the program with `output` ([asuka.toml](asuka-toml.md#bin)
lists them all):

```toml
[bin.notes-server]
build = ["go", "build", "-o", "{out}", "./cmd/server"]
env = { GOOS = "{goos}", GOARCH = "{goarch}", CGO_ENABLED = "0" }

[bin.notes-index]
build = ["cargo", "build", "--release", "--manifest-path", "index/Cargo.toml", "--target", "{target}"]
output = "index/target/{target}/release/notes-index{exe}"
```

`{target}` is the system's own triple when the app is built on that
system (`x86_64-pc-windows-msvc` on Windows), and when it's cross-built,
one that builds without the other system's SDK (`x86_64-pc-windows-gnu`,
MinGW; `x86_64-unknown-linux-musl`, static). The Rust targets come from
`rustup target add`, and a cross build needs a linker for them too.

When the placeholders aren't the arguments you want, `build` can run a
script of your own, which decides them as it runs: the target is in its
environment (`ASUKA_OS`, `ASUKA_ARCH`, `ASUKA_EXE`, `ASUKA_TARGET`,
`ASUKA_GOOS`, `ASUKA_GOARCH`), and it writes the program to `ASUKA_OUT`.
This one builds a static Linux program everywhere, and turns on a
feature for the Mac:

```toml
[bin.notes-index]
build = ["sh", "index/build.sh"]
```

```sh
#!/bin/sh
# index/build.sh: run in the project's directory, once per target.
set -e
target=$ASUKA_TARGET
features=
case $ASUKA_OS in
  linux)
    case $ASUKA_ARCH in
      x64) target=x86_64-unknown-linux-musl ;;
      arm64) target=aarch64-unknown-linux-musl ;;
    esac ;;
  macos) features="--features metal" ;;
esac
cargo build --release --manifest-path index/Cargo.toml --target "$target" $features
cp "index/target/$target/release/notes-index$ASUKA_EXE" "$ASUKA_OUT"
```

The script runs where the app is built, not where it runs: a Windows
build from a Mac runs it with the Mac's `sh`. To build on Windows, name
something Windows runs: `["pwsh", "-File", "index/build.ps1"]` (where
`$env:ASUKA_OUT` is the output), or a program in any language,
`["go", "run", "./tools/build-index"]`.

The app finds its programs with `command.bundled(name)`: the path in
the build, or none (a phone, the web, a test). `asuka dev` builds them
as it starts and the app finds them there the same way:

```asuka
import std/command

async fn startServer() throws -> command.Process {
  let server = command.bundled("notes-server") ?? "notes-server" // else on PATH
  return try command.start(server, ["serve"])
}
```

## Testing

A program's test blocks run with `asuka test`, as an app's do. `main`
is a function: a test runs it as the process would with `os.fake`,
which gives it its arguments, its environment and its stdin, and keeps
what it writes:

```asuka
import std/os

fn main() {
  let name = os.args().first() ?? "world"
  print("Hello, {name}!")
}

test "greets whom it's given" {
  let o = os.fake(args: ["Ada"], env: ["LANG": "en"], stdin: "")
  expect(await o.run(main) == 0)
  expect(o.stdout() == "Hello, Ada!\n")
}
```

`o.run(main)` returns the exit code the program would end with: 0, 1
for an error it throws (its message in `o.stderr()`), or what `os.exit`
said, which ends the test's `main` there instead of the process. Without
a fake, a test's `os.args()` and stdin are empty. `command.fake()`
answers for other programs, which then don't run:

```asuka
import std/command

test "reads git's branch" {
  let git = command.fake()
  git.answer("git", ["branch", "--show-current"], stdout: "main\n")
  let out = try await command.run("git", ["branch", "--show-current"])
  expect(out.text() == "main\n")
  expect(git.calls == [["git", "branch", "--show-current"]])
}
```

## What runs where

| | macOS | Windows | Linux | Phones, the web |
|---|---|---|---|---|
| A program (`asuka build`) | yes | yes | yes | no |
| `os.args`, `os.exit`, stdin | yes | yes | yes | an app: empty, `exit` throws |
| `std/command` | yes | yes | yes | `unsupported` |
| `std/flag` | yes | yes | yes | yes (it parses any list) |
| `std/term` | yes | yes | yes | no terminal: no styles, `prompt` gives `none` |
| `http.serve` | yes | yes | yes | a phone's app: while it runs; the web: `unsupported` |
