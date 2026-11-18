# 数据

应用的数据放在几个地方：小数据用 `std/storage`，记录用 `std/sqlite` 存进 SQLite，机密信息用 `platform/secure` 存进平台的钥匙串，用户各台设备共享的值用 `platform/icloud` 存进 iCloud，文档的文件用 `platform/document`。本页逐一介绍它们，也介绍如何用 `std/http` 访问网络、用 `std/watch` 监视文件，以及哪些工作会在主线程之外进行。

## 数据存在哪里

每个包都把数据放在平台自家应用放数据的地方：

| | `std/storage`（以及 `std/store`） | `std/sqlite` 用普通名字打开的数据库 |
|---|---|---|
| iOS | UserDefaults | 应用容器里的 Application Support |
| macOS | UserDefaults | `~/Library/Application Support/<bundle ID>` |
| Android | SharedPreferences | 应用的 files 目录 |
| Windows | `%LOCALAPPDATA%\<name>` 里的 `store.json` | `%LOCALAPPDATA%\<name>` |
| Linux | `$XDG_DATA_HOME/<id>` 里的 `store.json` | `$XDG_DATA_HOME/<id>`（即 `~/.local/share/<id>`；在 Flatpak 里是沙盒自己的目录） |
| Web | `localStorage` | 浏览器的源私有文件系统（没有时用 IndexedDB） |

`<name>` 是 `asuka.toml` 的 `name`，`<id>` 是它的 `id`。`storage.dirName()` 会告诉你，在应用当前运行的平台上这些值存在哪里。

在 `asuka dev` 下，桌面应用的数据目录是 `asuka dev` 自己的一个目录（在用户的缓存目录里，每个应用一个），所以开发时试过的东西永远不会和已安装版本的数据混在一起。

## 小数据：std/storage

`std/storage` 用来在多次启动之间保存小数据：设置、已登录用户的名字、上次打开的标签页。`storage.Value` 是一个 [store](language/stores.md#store)，它的 `value` 会被保存下来：一开始是上次保存的值，第一次则是默认值；之后每次给它赋值都会被保存。和任何状态一样，读取它会被跟踪，写入它会立刻显示出来：

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

值可以是 JSON 能表示的任何东西：数字、字符串、数组、结构体、枚举。默认值看不出类型时，把类型写出来：

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

保存的内容放进平台的偏好设置：iOS 和 macOS 上是 UserDefaults，Android 上是 SharedPreferences，Web 上是 `localStorage`，Windows 和 Linux 上是一个文件 `store.json`，改动后稍等片刻才写入。`storage.flush()` 会立即写入，适合在应用可能被结束之前调用（比如后台任务的最后一步）。`value.reset()` 会忘掉保存的值。保存的值在 store 创建时读取：如果它已经不能解码成这个类型（类型改了），得到的就是默认值。

`std/store` 做的是同样的事，只是不包成 store：`store.read(key, default)`、`store.write(key, value)` 和 `store.remove(key)`，适合在你自己选定的时机保存某个值的代码。

它不是数据库：记录很多或很大时，用 SQLite。

## 记录：std/sqlite

`std/sqlite` 是应用的 SQLite 数据库。它就是 SQLite 本身，同一个版本在每个平台上用同样的方式构建，并编译进应用：导入这个包就行，不需要另外安装任何东西。Asuka 每次执行一整条语句，在主线程之外运行，并把返回的行直接解码成你要的类型。[`examples/notes`](../../examples/notes) 用的就是它。

### 打开与迁移

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

`sqlite.open(name)` 打开数据库（不存在就创建）：普通名字是应用数据目录里的一个文件（见[数据存在哪里](#数据存在哪里)），绝对路径就是那个文件，`":memory:"` 是内存中的数据库。文件数据库使用预写式日志（WAL），并打开外键约束。

`db.migrate(steps)` 把表结构更新到最新。steps 是数据库按顺序排列的各个版本：每一步的 SQL（用分号分隔多条语句）只运行一次，在一个事务里执行，版本号记在 `PRAGMA user_version` 里。处于版本 1 的数据库会从第 2 步开始执行；新数据库会全部执行。新步骤加在末尾，已经发布的步骤不要再改。它在主线程上运行，所以在应用启动时调用。在 Web 上，页面的主线程不能等待 SQLite 的 worker，所以 `open` 和 `migrate` 会发给 worker 而不等回复，并在之后的语句之前运行：没能打开的数据库，之后的每条语句都会失败并说明原因；迁移失败时，下一条语句会失败。

### 查询

```asuka
async fn undone() throws -> [Note] = try await db.query("select * from notes where done = ? order by id", [false])

async fn noteCount() throws -> int {
  let n: [int] = try await db.query("select count(*) from notes", [])
  return n.first() ?? 0
}
```

`db.query<T>(sql, args)` 在主线程之外运行查询，把结果行作为若干个 `T` 返回。结构体的字段按列名填充（`select *` 会填满 `Note`）；没有对应列的字段取零值（`0`、`""`、`false`、`[]`、`none`），没有对应字段的列会被忽略。其他类型的 `T` 取第一列：计数用 `[int]`，名字用 `[string]`。

列会尽量转换成字段的类型：整数可以转成 `int`、`float`、`bool`（非零为真）或 `string`；实数可以转成 `float` 或 `int`；文本可以转成 `string`、`bytes`、它所写的数字、按 case 名对应的无负载枚举，以及（当文本是 JSON 时）结构体、数组或映射；blob 可以转成 `bytes` 或 JSON；null 是字段的零值，可选类型则是 `none`。转换不了的列会让查询失败，并给出列名。

### 参数

语句的参数可以是元组或数组，按位置对应（`?`、`?1`）；也可以是结构体或 `[string: T]`，按名字（`:name`、`@name`、`$name`）以及位置对应：

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

每个参数可以是 `int`、`float`、`bool`（存成 0 或 1）、`string`、`bytes`（blob）、没有负载的枚举 case（存它的序号），或者它们的可选类型（`none` 就是 null）。`[]` 表示没有参数。

### 执行

`db.exec(sql, args)` 在主线程之外运行一条语句（insert、update、delete、create……），或用分号分隔的多条语句，并且排在它之前的语句之后：紧跟在 `exec` 后面的查询能看到它写入的内容。事务就是 SQL：在一次 `exec` 里写 `begin`、各条语句、`commit`。

### 实时查询

`db.watch<T>(sql, args)` 是一个实时查询：一个装着结果行的 store，现在就在主线程之外查询一次，之后每当通过这个数据库对它读过的表的写入提交时，再查询一次。在组件里创建时，它会随组件一起停止。

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

`Live<T>` 有 `rows`（最新的结果行）、`loading`（第一次查询完成前为 true）和 `error`（上次查询失败的原因，成功一次之后为 none）。它们都是状态：显示它们的地方会跟着变化。

实时查询只为它读过的表重新运行。它在 SQLite 准备语句时得知这些表，包括视图背后的表；当对其中某张表的写入提交时它就重新运行，触发器和外键动作写入的行也算在内。对别的表的写入不会让它重新运行。

它的参数可以读取状态，就像 `[query]` 那样：参数变化时，`let` 会重新创建这个实时查询。新的实时查询会先用同一条 SQL 上次的结果，等新结果到来，所以用户打字时列表不会变空。

`db.changed()` 让所有实时查询重新运行：用于数据库自己看不到的变化（另一个进程的修改、文件被替换）。

### 错误

SQLite 返回的错误，消息就是 SQLite 的消息，后面带上结果码：`sqlite: UNIQUE constraint failed: users.email (2067)`。

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

### 库文件

SQLite 在每个平台上都是 3.54.1 版，内置 FTS5、R*Tree、Geopoly、数学函数、`dbstat`、`STAT4` 和 `soundex`，不支持可加载扩展，临时表放在内存里。在每个平台上，它都和应用的其余部分一样，编译进每个导入了 `std/sqlite` 的应用：不需要在应用旁边安装、下载或附带任何库；没有导入 `std/sqlite` 的应用也不会带上它。`asuka dev`、`asuka test` 和 `asuka run` 里同样有它。它自己的符号只留在应用内部，所以即使另一个库把系统的 SQLite 加载进了同一个进程（比如 Linux 上的 GTK），两者也互不干扰。在 Web 上，它是同一个库编译成的 WebAssembly，运行在页面的一个 worker 里（见 [Web 上的 SQLite](web.md#sqlite)）。

## 机密信息：platform/secure

`platform/secure` 保存少量机密信息，比如令牌和密码，由平台负责加密。它是用来放机密的，不是用来放数据的：其他东西请放在 store 或数据库里。

- **iOS**：钥匙串，以应用的 bundle ID 存为通用密码，设备首次解锁后可读，并且只留在本机。
- **Android**：每个值都用 Android Keystore 里的密钥加密（AES-GCM），密钥从不离开 Keystore，加密后的值存在应用私有的偏好设置里。
- **macOS**：登录钥匙串，以应用的 bundle ID（没有的话用应用名）存为通用密码，可以在“钥匙串访问”里看到。用临时签名（ad hoc）签的应用也能用它，`asuka dev` 的应用和没有 Developer ID 的构建都是这样签的。
- **Windows**：凭据管理器，存为当前用户的通用凭据，名为 `<name>/<key>`，只保存在这台电脑上（不会漫游），可以在控制面板的凭据管理器里看到。一个值最多 2560 字节。
- **Linux**：用户的密钥环，通过 libsecret 和 Secret Service（GNOME Keyring、KWallet、KeePassXC）：每个键一个条目，属于应用自己的 schema（它的 ID），标签为 `<name>: <key>`，放在默认集合里，可以在密钥环的应用（GNOME 的“密码和密钥”）里看到。没有 libsecret 或没有运行中的密钥环时，调用会失败，并说明原因。
- **Web**：浏览器没有钥匙串。每个值都用 WebCrypto 生成的不可导出密钥加密（AES-GCM，256 位），和密钥一起存在应用的 IndexedDB 数据库里，所以任何脚本都读不到密钥的字节，值也不会以明文落在磁盘上。不过在应用的源里运行的任何代码仍然可以让这个密钥替它解密：这是 Web 应用能做到的最好程度，并不是钥匙串。它需要 https 或 localhost。

`secure.read(key)` 返回某个键的值（没有时为 `none`），`secure.write(key, value)` 保存一个值，`secure.remove(key)` 删除它。它们都会等待平台完成，但不占住主线程，平台报的错（密钥环已锁定、根本没有密钥环）会原样抛出；平台没有对应代码时（`asuka test`）抛出 `unsupported` 错误。

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

## 用户各设备共享的值：platform/icloud

在 iOS 和 macOS 上，`icloud.Value(key, default)` 是一个保存在 iCloud 键值存储里的 store，另一台设备修改它时会重新读取，就像 `storage.Value` 保存在本机一样：

```asuka
import platform/icloud

let textSize = icloud.Value("textSize", 17.0)
```

`icloud.account()` 和 `icloud.files(scope)` 会跟踪账户和应用在 iCloud 里的文件，`readFile`、`writeFile`、`conflicts` 和 `resolve` 用来操作这些文件：见 [iCloud](icloud.md)。在其他平台上，值保持默认值以及你在本机给它设置的值。

## 文档的文件：platform/document

`document.open(path, read:, write:)` 把一个文件的内容变成一个 store：用应用的 `read` 在主线程之外读取，每次 `edit` 之后稍等片刻用它的 `write` 保存，可以还原；如果在读取之后另一个程序改了这个文件，就成为冲突：

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

## 网络：std/http

`std/http` 通过平台自己的客户端发送请求，所以代理、受信任的证书、缓存和 cookie 都和系统的行为一致。请求需要等待，所以它是 `async` 的：

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

组件用 `load` 显示请求的结果：第一次结果到来之前是 `.pending`，之后重新加载期间，上一次的结果会继续留在屏幕上，`.loading` 则表示有一次请求正在进行（见[需要加载的数据](language/async.md#需要加载的数据)）。

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

改动了 load 所显示内容的保存操作，用调用来指明它们：`await reload latest()` 会让任何页面上 `latest()` 的每一个 load 再请求一次，并等它们的结果；`x.optimistic(f)` 会在服务器确认之前就显示改动，`load (a(), b())` 会把几个结果一起显示（[立刻显示的改动](language/async.md#立刻显示的改动)、[重新加载改动影响到的数据](language/async.md#重新加载改动影响到的数据)、[一起到达的 load](language/async.md#一起到达的-load)）。

在浏览器里，向其他源发请求需要那个源返回 CORS 头。

较大的响应（视频、压缩包）可以边收边写进文件：`http.download(url, path, progress:)` 先写到路径旁边的文件里，全部收完后再重命名到位，每秒几次以已收到的字节数和总字节数（服务器没说时为 -1）调用 `progress`。它的 `timeout` 是等待后续数据的最长时间，而不是整个下载的时间；取消它所在的 task 就会停止下载，不留下文件。iOS 和 macOS 上用 URLSession，Android 上用 `HttpURLConnection`，Windows 和 Linux 上用 WinHTTP 和 libsoup，都是直接流式写入文件；在浏览器里，则从 fetch 的流中读出，写进应用的文件。

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

桌面应用的 OAuth 登录会重定向到应用自己在回环接口上的服务器（RFC 8252）：`http.loopback(path:, page:)` 在系统挑选的端口上监听，它的 `url` 就是要提供的重定向 URI；`next()` 等待浏览器回来，用 `page` 应答（"可以关闭这个窗口了"），并给出浏览器访问的 URL，其中的查询参数就是授权码；`close()` 停止监听。Android 和 iOS 在应用运行期间也可以监听；浏览器不行（在那里会抛出错误）：Web 应用的登录应重定向到它自己的站点。

```asuka
import std/http
import platform/device

async fn signInBack(authorize: string) throws -> string {
  let lb = try await http.loopback()
  defer { lb.close() }
  device.openURL("{authorize}&redirect_uri={lb.url}")
  let back = try await lb.next() // http://127.0.0.1:port/callback?code=…
  return back
}
```

### 边到边读的响应体

随时间陆续到达的响应体（语言模型逐字写出的回复、日志、信息流）可以边发边读。`http.stream(url, method:, headers:, body:)` 在响应头一到就返回：带有 `status`、`url`、`headers`，以及 `body`——一个按网络送达顺序给出各个片段的流。`lines()` 把它读成一行行文本，`events()` 读成服务器发送事件（`text/event-stream`，读法和浏览器的 `EventSource` 一样：每个都是带 `event`、`data` 和 `id` 的 `Event`），`text()` 读出整个响应体。`http.events(url, …)` 把这两步合在一起：它请求事件（也可以用 `POST`，语言模型的 API 就是这样流式返回回复的），状态码不是成功时抛出 `HTTPError.status`；加上 `reconnect: true`，就会像 `EventSource` 那样按服务器给的 `retry` 重新连接，并带上最后一个事件的 ID。离开循环或任务结束都会停止请求；`timeout` 是等待响应头、之后等待每个片段的最长时间。

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

每个平台都用自己的客户端来流式读取：URLSession、`HttpURLConnection`、`fetch` 的响应体流、WinHTTP 和 libsoup。行和事件的解析在各处都完全一样。在 iOS 和 macOS 上，响应头会和响应体的第一批字节一起到达：URLSession 不会更早给出它。

## 双向连接：std/websocket

`std/websocket` 保持一条双向的连接（RFC 6455）：`websocket.connect(url, headers:, protocols:)` 在服务器接受连接后返回一个 `WebSocket`。`send(text)` 和 `sendBinary(data)` 发送消息；`messages` 是收到的消息流（`.text` 或 `.binary`），服务器正常关闭连接时结束，以其他方式关闭时抛出 `WebSocketError.closed(code, reason)`；`close(code:, reason:)` 关闭连接。连接由各平台自己的客户端承载：URLSession、OkHttp、浏览器的 `WebSocket`、WinHTTP 和 libsoup；浏览器不会发送应用自己的请求头。

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

## 监视文件：std/watch

`watch.path(dir)` 跟踪一个文件夹（或文件）的变化：应用显示的文件夹、编辑器里打开的项目、另一个程序会写入的设置文件。它的 `events` 是一个按批到来的变化流：

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

监视会记住它看到的文件，每当系统说有东西变了，就再看一遍并比较：事件是前后的差异，而不是造成差异的每一步。一个文件连续写了很多次，只是一次 `write`；创建后又删掉的文件什么都不算；在原路径上被替换的文件（编辑器和原子保存就是这么做的）是一次 `write`；在文件夹内移动的文件或文件夹是一次 `rename`，带上它的 `oldPath`。把事件当作“该重新读文件了”的提示。系统丢掉变化时，监视会把所有东西重新看一遍，这一批以一个 `overflow` 开头。

`ignore` 接受 `.gitignore` 的简单模式（`*.log`、`node_modules/`、`/dist`、`src/**/gen`）；`recursive: true` 也监视其中的子文件夹，`attributes: true` 会报告权限的变化。在 Linux 上，每个被监视的文件夹都会占用 `fs.inotify.max_user_watches` 的一个名额；监视很大的目录树时，请调高这个上限。手机和 Web 上没有可用的监视方式：`watch.path` 会抛出 unsupported 错误。离开循环（或组件被移除）就会停止监视。[`examples/files`](../../examples/files) 展示了一个实时更新的文件夹。

## 主线程

界面运行在主线程上，在主线程上等待的工作会让界面卡住。对数据来说：

- **在主线程之外，用 await 等待**：`db.exec`、`db.query` 和实时查询的每次查询；`icloud` 的调用；`document` 的读写；`http` 的请求。它们的 `await` 让界面可以继续运行。
- **在主线程上，很快完成**：`sqlite.open` 和 `db.migrate`（在应用启动时）；`storage.Value` 和 `std/store`，它们读写的是平台的偏好设置（在 Windows 和 Linux 上是内存：文件稍后才写）。

你自己的、会拖住主线程的工作（解析大文件、缩放图片）放进 `spawn { … }`，它会在另一个线程上运行（[异步](language/async.md#异步)）。
