# Data

An app keeps its data in a few places: small values in
`std/storage`, records in SQLite with `std/sqlite`, secrets in the
platform's keychain with `platform/secure`, values the user's devices
share in iCloud with `platform/icloud`, and a document's file with
`platform/document`. This page covers each, the network through
`std/http`, watching files with `std/watch`, and what runs off the
main thread.

## Where data lives

Each package keeps its data where the platform's own apps keep theirs:

| | `std/storage` (and `std/store`) | `std/sqlite`'s databases, by a plain name |
|---|---|---|
| iOS | UserDefaults | Application Support, in the app's container |
| macOS | UserDefaults | `~/Library/Application Support/<bundle ID>` |
| Android | SharedPreferences | the app's files directory |
| Windows | `store.json` in `%LOCALAPPDATA%\<name>` | `%LOCALAPPDATA%\<name>` |
| Linux | `store.json` in `$XDG_DATA_HOME/<id>` | `$XDG_DATA_HOME/<id>` (`~/.local/share/<id>`; in a Flatpak, the sandbox's own) |
| The web | `localStorage` | the browser's origin private file system (IndexedDB without it) |

`<name>` is `asuka.toml`'s `name`, and `<id>` its `id`.
`storage.dirName()` says where the values are on the platform the app
runs on.

Under `asuka dev`, a desktop app's data directory is one of `asuka
dev`'s own (in the user's cache directory, one for each app), so what
you try while developing never mixes with an installed build's data.

## Small values: std/storage

`std/storage` keeps small values across launches: settings, a
signed-in user's name, the tab last open. A `storage.Value` is a
[store](language/stores.md#stores) whose `value` is kept: it starts with what
was kept, or the default the first time, and each value it's set to is
kept. Reading it is tracked, as any state's is, and writing it shows:

```asuka
import std/storage

let wifiOnly = storage.Value("wifiOnly", false)

component DownloadSettings {
  Column {
    Toggle { bind on: wifiOnly.value; title: "Download on Wi-Fi only" }
    if wifiOnly.value { Text("Large files wait for Wi-Fi") { color: .secondaryLabel } }
  }
}
```

A value is anything JSON can say: numbers, strings, arrays, structs,
enums. A type the default doesn't say is written:

```asuka
struct Account {
  id: string
  email: string
}

let accounts: storage.Value<[Account]> = storage.Value("accounts", [])

fn signIn(a: Account) {
  accounts.value.push(a)
}
```

What's kept goes to the platform's preferences: UserDefaults on iOS and
macOS, SharedPreferences on Android, `localStorage` on the web, and on
Windows and Linux a file, `store.json`, written a moment after a change.
`storage.flush()` writes now, before the app may be ended (a background
task's last step). `value.reset()` forgets what's kept. What's kept is
read when the store is made: a value that no longer decodes as the type
(the type changed) gives the default.

`std/store` is the same without a store: `store.read(key, default)`,
`store.write(key, value)` and `store.remove(key)`, for code that keeps a
value at a moment of its choosing.

It's not a database: for many or large records, use SQLite.

## Records: std/sqlite

`std/sqlite` is the app's SQLite database. It's SQLite itself, one
release built the same way for every platform and compiled into the
app: import the package, and there's nothing to install. Asuka runs
each statement whole, off the main thread, and decodes the rows it
answers straight into the type asked for. [`examples/notes`](../examples/notes) uses it.

### Open and migrate

```asuka
import std/sqlite

struct Note {
  id: int
  text: string
  done: bool
}

fn openDB() throws -> sqlite.DB {
  let db = try sqlite.open("notes.db")
  try db.migrate([
    // 1
    "create table notes (id integer primary key, text text not null, done bool not null default 0)",
    // 2
    "alter table notes add column created integer not null default 0",
  ])
  return db
}

let db = (try? openDB())!
```

`sqlite.open(name)` opens the database (creating it): a plain name is a
file in the app's data directory (see [Where data lives](#where-data-lives)),
an absolute path is that file, and `":memory:"` is a database in memory.
A file is in write-ahead logging, with foreign keys on.

`db.migrate(steps)` brings the schema up to date. The steps are the
database's versions in order: each one's SQL (statements separated by
semicolons) runs once, in a transaction, and its number is kept in
`PRAGMA user_version`. A database at version 1 runs step 2 and on; a new
one runs them all. Add steps at the end, and never change one that has
shipped. It runs on the main thread, so call it as the app starts. On
the web, where the page's main thread can't wait for SQLite's worker,
`open` and `migrate` go to the worker without waiting and run before
the statements after them: a database that didn't open fails each
statement with why, and a migration that failed fails the next one.

### Query

```asuka
async fn undone() throws -> [Note] = try await db.query("select * from notes where done = ? order by id", [false])

async fn noteCount() throws -> int {
  let n: [int] = try await db.query("select count(*) from notes", [])
  return n.first() ?? 0
}
```

`db.query<T>(sql, args)` runs a query off the main thread and gives its
rows as `T`s. A struct's fields are filled by the columns' names (`select
*` fills `Note`); a field no column has is its zero (`0`, `""`, `false`,
`[]`, `none`), and a column no field has is left. Any other `T` is the
first column's: `[int]` for a count, `[string]` for names.

A column fits its field as it can: an integer fits an `int`, a `float`, a
`bool` (not zero) or a `string`; a real fits a `float` or an `int`; text
fits a `string`, `bytes`, a number it spells, a payloadless enum by its
case's name, and, holding JSON, a struct, an array or a map; a blob fits
`bytes` or JSON; null is the field's zero, or `none` for an optional. One
that can't fit fails the query with the column's name.

### Arguments

A statement's arguments are a tuple or an array, by position (`?`,
`?1`), or a struct or a `[string: T]`, by name (`:name`, `@name`,
`$name`) and position:

```asuka
async fn add(text: string) throws {
  try await db.exec("insert into notes (text, created) values (?, ?)", (text, 1700000000))
}

struct Rename {
  id: int
  text: string
}

async fn rename(r: Rename) throws {
  try await db.exec("update notes set text = :text where id = :id", r)
}
```

Each is an `int`, a `float`, a `bool` (stored as 0 or 1), a `string`, `bytes`
(a blob), an enum's case without a payload (its index), or an optional of
one (`none` is null). `[]` is no arguments.

### Exec

`db.exec(sql, args)` runs a statement (insert, update, delete, create...),
or several separated by semicolons, off the main thread, after the
statements before it: a query that follows an `exec` sees what it wrote.
A transaction is SQL: `begin`, the statements, `commit`, in one `exec`.

### Live queries

`db.watch<T>(sql, args)` is a live query: a store of its rows, fetched
off the main thread now, and again each time a write through the
database commits to a table it reads. Made in a component, it stops with
the component.

```asuka
let search = "select * from notes where ?1 = '' or instr(lower(text), lower(?1)) > 0 order by done, id desc"

component Notes {
  state query = ""
  let notes: sqlite.Live<Note> = db.watch(search, [query])

  Column {
    TextField { bind text: query; placeholder: "Search" }
    if notes.loading { Text("Loading…") }
    if let e = notes.error { Text(e) { color: .red } }
    for n in notes.rows key n.id { Text(n.text) }
  }
}
```

A `Live<T>` has `rows`, the latest rows; `loading`, true until the first
fetch is done; and `error`, what the last fetch failed with (none after
one that worked). They're state: what shows them follows them.

A live query reruns only for the tables it reads. It learns them as
SQLite prepares it, the tables behind a view included, and reruns when a
write to one of them commits, including rows written by triggers and by
foreign keys' actions. A write to another table doesn't run it.

Its arguments may read state, as `[query]` does: when they change, the
`let` makes the live query again, which starts from its SQL's last rows
while the new ones load, so a list doesn't empty as the user types.

`db.changed()` runs every live query again: for a change the database
can't see (another process's, the file replaced).

### Errors

What SQLite returns is an error whose message is SQLite's, with its
result code: `sqlite: UNIQUE constraint failed: users.email (2067)`.

```asuka
async fn signUp(email: string) -> string {
  do {
    try await db.exec("insert into users (email) values (?)", [email])
    return ""
  } catch e {
    if e.message.contains("UNIQUE constraint failed") { return "That email is taken" }
    return e.message
  }
}
```

### The library

SQLite is release 3.54.1 on every platform, with FTS5, R*Tree, Geopoly,
the math functions, `dbstat`, `STAT4` and `soundex` built in, loadable
extensions left out, and temporary tables kept in memory. It's compiled
into each app that imports `std/sqlite`, on every platform, as the rest
of the app is: there's no library to install, download or ship beside
it, and an app that doesn't import `std/sqlite` doesn't carry it.
`asuka dev`, `asuka test` and `asuka run` have it too. Its own names
stay inside the app, so a system SQLite that another library loads into
the same process (GTK's, on Linux) doesn't mind it. On the web it's the
same SQLite built to WebAssembly, in a worker of the page's
([SQLite on the web](web.md#sqlite)).

## Secrets: platform/secure

`platform/secure` keeps small secrets, tokens and passwords, encrypted by the
platform. It's for secrets, not data: keep everything else in the store
or a database.

- **iOS**: the Keychain, as generic passwords under the app's bundle ID,
  readable after the device's first unlock, and on this device only.
- **Android**: each value is encrypted (AES-GCM) with a key in the Android
  Keystore, which never leaves it, and kept in the app's private
  preferences.
- **macOS**: the login keychain, as generic passwords under the app's
  bundle ID (its name, without one), which Keychain Access shows. An app
  signed ad hoc can use it, as `asuka dev`'s and a build without a
  Developer ID are.
- **Windows**: the Credential Manager, as the user's generic credentials
  named `<name>/<key>`, kept on this computer (they don't roam), which
  Control Panel's Credential Manager shows. A value is at most 2560 bytes.
- **Linux**: the user's keyring, through libsecret and the Secret
  Service (GNOME Keyring, KWallet, KeePassXC): an item of the app's own
  schema (its ID) for each key, labeled `<name>: <key>`, in the default
  collection, which the keyring's app (GNOME's Passwords and Keys) shows. Without
  libsecret or a keyring running, the calls fail, saying so.
- **The web**: a browser has no keychain. Each value is encrypted
  (AES-GCM, 256 bits) under a key WebCrypto made non-extractable, and
  kept with it in the app's IndexedDB database, so no script can read the
  key's bytes, and the values aren't on the disk in the clear. Anything
  running in the app's origin can still have the key decrypt them: it's
  a web app's best, not a keychain. It needs https or localhost.

`secure.read(key)` is a key's value (`none` if there's none),
`secure.write(key, value)` keeps one, and `secure.remove(key)` forgets
it. Each waits for the platform without holding the main thread, and
throws what it says (a locked keyring, no keyring at all); where the
platform has no code for it (`asuka test`), an `unsupported` error.

```asuka
import platform/secure

component SignedIn {
  state status = ""

  task {
    do {
      status = if (try await secure.read("token")) != none { "Signed in" } else { "Signed out" }
    } catch e {
      status = e.message
    }
  }

  Column {
    Text(status)
    Button("Sign out") {
      on press {
        try? await secure.remove("token")
        status = "Signed out"
      }
    }
  }
}

async fn keepToken(token: string) throws {
  try await secure.write("token", token)
}
```

## Values the user's devices share: platform/icloud

On iOS and macOS, `icloud.Value(key, default)` is a store kept in iCloud's
key-value store, read again as another device changes it, as a
`storage.Value` is kept on the device:

```asuka
import platform/icloud

let textSize = icloud.Value("textSize", 17.0)
```

`icloud.account()` and `icloud.files(scope)` follow the account and the
app's files in iCloud, and `readFile`, `writeFile`, `conflicts` and
`resolve` work with them: see [iCloud](icloud.md). Elsewhere, a value
keeps its default and what it's set to, on the device.

## A document's file: platform/document

`document.open(path, read:, write:)` is a file's content as a store: read
off the main thread with the app's `read`, saved with its `write` a
moment after each `edit`, reverted, and a conflict when another program
changed the file since it was read:

```asuka
import platform/document

component Editor(path: string) {
  let doc = document.open(path, read: data => data.string(), write: text => text.bytes())

  Column {
    if let text = doc.content {
      TextField { text: text; multiline: true; on change(t) { doc.edit(t) } }
    }
    if doc.state == .conflict {
      Button("Keep Mine") { on press { await doc.saveAnyway() } }
      Button("Take Theirs") { on press { await doc.revert() } }
    }
  }
}
```

## The network: std/http

`std/http` sends requests through the platform's own client, so
proxies, trusted certificates, caches and cookies work as the system's
do. A request waits, so it's `async`:

```asuka
import std/http

struct Release {
  name: string
}

async fn latest() throws -> string {
  let r = try await http.get("https://api.github.com/repos/egoist/asuka/releases/latest")
  if !r.ok() { throw Error("HTTP {r.status}") }
  let rel: Release = try r.json()
  return rel.name
}
```

A component shows an answer with `load`: `.pending` until the first
one comes, then that one stays on screen while a reload runs, and
`.loading` says one is running
([Data that loads](language/async.md#data-that-loads)).

```asuka
component LatestRelease {
  let name = load latest()

  Row {
    match name {
      .pending => ProgressRing()
      .failed(e) => Text(e.message)
      .ready(n) => Text(n)
    }
    Button("Refresh") {
      disabled: name.loading
      on press { name.reload() }
    }
  }
}
```

A save that changes what loads show names them by the call:
`await reload latest()` asks every load of `latest()` again, on any
screen, and waits for their answers; `x.optimistic(f)` shows the change
before the server says so
([Changes shown at once](language/async.md#changes-shown-at-once),
[Reloading what a change touched](language/async.md#reloading-what-a-change-touched)).

In a browser, a request to another origin needs that origin's CORS
headers.

A large answer (a video, an archive) goes into a file as it comes:
`http.download(url, path, progress:)` writes it beside the path and
renames it into place once it's all in, calling `progress` with the
bytes so far and the whole's (-1 when the server doesn't say) a few
times a second. Its `timeout` is the longest wait for more, not for the
whole; cancelling its task stops it and leaves no file. URLSession on
iOS and macOS, `HttpURLConnection` on Android, WinHTTP and libsoup on
Windows and Linux stream it to the file; in a browser it's read from
fetch's stream into the app's files.

```asuka
import std/fs
import std/http

component Download(url: string) {
  state got = 0
  state total = -1
  state problem = ""

  task(url) {
    let file = fs.join([fs.appDir(), "downloads", "video.mp4"])
    do {
      try fs.mkdirAll(fs.dir(file))
      let r = try await http.download(url, file, progress: (n, all) => {
        got = n
        total = all
      })
      if !r.ok() { problem = "HTTP {r.status}" }
    } catch e {
      problem = e.message
    }
  }

  Column {
    Progress { value: if total > 0 { got.float() / total.float() } else { none } }
    if !problem.isEmpty() { Text(problem) }
  }
}
```

An OAuth sign-in on a desktop redirects to a server of the app's on the
loopback interface (RFC 8252): `http.loopback(path:, page:)` listens at
a port the system picks, its `url` the redirect URI to give;
`next()` waits for the browser to come back, answers it with `page`
("You can close this window"), and gives the URL it came to, its query
the code; `close()` stops it. Android and iOS can listen too, while the
app runs; a browser can't (it throws there): a web app's sign-in
redirects to its own site.

```asuka
import std/http

async fn signInBack(authorize: string) throws -> string {
  let lb = try await http.loopback()
  defer { lb.close() }
  openURL("{authorize}&redirect_uri={lb.url}")
  let back = try await lb.next() // http://127.0.0.1:port/callback?code=…
  return back
}
```

### Bodies as they come

A body that comes over time (the reply a language model writes, a log,
a feed) is read as it's sent. `http.stream(url, method:, headers:,
body:)` answers as soon as the response's head is in: its `status`,
`url` and `headers`, and its `body`, a stream of the pieces as the
network delivers them. `lines()` reads them as lines of text,
`events()` as server-sent events (`text/event-stream`, as a browser's
`EventSource` reads it: each an `Event` with its `event`, `data` and
`id`), and `text()` the whole. `http.events(url, …)` does both: it asks
for events (a `POST` too, as language models' APIs stream their
replies), throws `HTTPError.status` for a status that isn't a success,
and with `reconnect: true` connects again after the server's `retry`,
sending the last event's ID, as `EventSource` does. Leaving the loop, or
the task going, stops the request; `timeout` is the longest wait for
the head and then for each piece.

```asuka
import std/http
import std/json

struct Prompt {
  model: string
  input: string
  stream: bool = true
}

screen Ask {
  state reply = ""
  state done = false

  title: "Ask"

  task {
    let prompt = json.encode(Prompt(model: "a-model", input: "Tell me a story"))
    for await e in http.events("https://example.com/v1/responses", method: "POST", headers: ["content-type": "application/json"], body: prompt.bytes()) {
      if e.data == "[DONE]" { break }
      reply = reply + e.data
    }
    done = true
  }

  Text(reply)
}
```

Each platform's own client streams it: URLSession, `HttpURLConnection`,
`fetch`'s body stream, WinHTTP and libsoup. The lines and events are
read the same everywhere. On iOS and macOS the head comes with the
body's first bytes: URLSession gives it no sooner.

## Live connections: std/websocket

`std/websocket` keeps a connection open both ways (RFC 6455):
`websocket.connect(url, headers:, protocols:)` answers once the server
accepted it, with a `WebSocket`. `send(text)` and `sendBinary(data)`
send, `messages` is a stream of what comes (`.text` or `.binary`),
which ends when the server closes the connection normally and throws
`WebSocketError.closed(code, reason)` when it closes it otherwise, and
`close(code:, reason:)` closes it. Each platform's own client carries
it: URLSession, OkHttp, the browser's `WebSocket`, WinHTTP and libsoup;
a browser sends no headers of the app's.

```asuka
import std/websocket

screen Chat {
  state lines: [string] = []
  state ws: websocket.WebSocket? = none

  title: "Chat"

  task {
    let socket = try await websocket.connect("wss://example.com/chat", protocols: ["chat"])
    ws = socket
    for await m in socket.messages {
      match m {
        .text(t) => { lines.push(t) }
        .binary(b) => { lines.push("{b.count()} bytes") }
      }
    }
  }

  Column {
    for (i, l) in lines.enumerated() key i { Text(l) }
    Button("Wave") {
      on press {
        if let s = ws { try? await s.send("👋") }
      }
    }
  }
}
```

## Watching files: std/watch

`watch.path(dir)` follows changes to a folder (or a file): a folder the
app shows, a project open in an editor, a settings file another program
writes. Its `events` is a stream of batches of changes:

```asuka
import std/watch

component Folder(dir: string) {
  state changes: [string] = []
  state problem = ""

  task(dir) {
    do {
      let w = try await watch.path(dir, ignore: [".*", "node_modules/"])
      for await events in w.events {
        for e in events { changes.push("{e.op} {e.path}") }
      }
    } catch e {
      problem = e.message
    }
  }

  Column {
    if !problem.isEmpty() { Text(problem) }
    for (i, c) in changes.enumerated() key i { Text(c) }
  }
}
```

A watch keeps what it saw of the files and, each time the system says
something changed, looks again and compares: the events are the
differences, not every step that made them. A file written many times
in a row is one `write`; one created and removed in between is nothing;
a file replaced at its path, as editors and atomic saves do, is a
`write`; a file or folder that moved within the folder is a `rename`
with its `oldPath`. Treat events as hints to read the files again. When
the system drops changes, the watch looks at everything again and the
batch starts with an `overflow`.

`ignore` takes `.gitignore`'s simple patterns (`*.log`, `node_modules/`,
`/dist`, `src/**/gen`); `recursive: true` watches the folders in it too,
and `attributes: true` reports permission changes. On Linux each folder
watched counts against `fs.inotify.max_user_watches`; raise it for a
large tree.
Phones and the web have nothing to watch with: `watch.path` throws an
unsupported error there. Leaving the loop (or the component going)
stops the watch. [`examples/files`](../examples/files) shows a folder
that updates live.

## The main thread

The interface runs on the main thread, and work that waits there would
freeze it. For data:

- **Off the main thread, awaited**: `db.exec`, `db.query` and a live
  query's fetches; `icloud`'s calls; `document`'s reads and writes;
  `http`'s requests. Their `await` lets the interface go on.
- **On the main thread, quick**: `sqlite.open` and `db.migrate` (as the
  app starts); `storage.Value` and `std/store`, which read and write
  the platform's preferences (on Windows and Linux, memory: the file is
  written a moment later).

Work of your own that would hold the main thread up (parsing a large
file, resizing an image) goes in `spawn { … }`, which runs it on
another thread ([Async](language/async.md#async)).
