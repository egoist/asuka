# プログラム

Asuka ではアプリとライブラリのほかに、**プログラム**も作れます。ウィンドウもビューも持たないコマンドラインツールです。パッケージのファイルが `fn main()` を宣言していれば、そのパッケージはプログラムになります。Go の `package main` や Cargo の `src/main.rs` と同じ考え方です。プログラムはアプリと同じようにビルドされ、同じ言語と同じ標準ライブラリ（ファイル、JSON、時刻、SQLite、HTTP など）を使い、macOS、Windows、Linux のネイティブな実行ファイルにコンパイルされます。

```asuka
import std/os

fn main() {
  let name = os.args().first() ?? "world"
  print("Hello, {name}!")
}
```

## プログラムの作成

```sh
asuka new --bin greet   # asuka.toml と、main とテストを持つ main.asuka
cd greet
asuka run -- Ada        # Hello, Ada!
asuka test              # テストブロックを実行
asuka build             # 実行ファイル dist/greet
```

`asuka new` はアプリを、`asuka new --lib` はライブラリを、`asuka new --bin` はプログラムを作ります。アプリのプロジェクトも、それぞれ専用のディレクトリにプログラムを持てます。[アプリに付属するプログラム](#アプリに付属するプログラム)を参照してください。

## main

プログラムは `fn main()` から始まります。引数は受け取らず（コマンドライン引数は `os.args()` で読みます）、値も返しません（終了コードは `os.exit(code)` で指定します）。`async` にすることも、エラーを投げることもできます。`fn main` を持つパッケージには `app` ブロックを書けず、コンポーネントも宣言できません。プログラムはビューを表示しないからです。エントリーポイントになるのは、プログラムを構成するパッケージの `main` だけです。インポートしたパッケージにある `main` は、ほかの関数と変わりません。

```asuka
import std/fs

async fn main() throws {
  let text = try await spawn { try fs.readText("notes.txt") }
  print("{text.lines().count()} lines")
}
```

プログラムは次のように動きます。

- まず各パッケージの値（`let`）が作られ、続いて `main` が実行されます。`async` の `main` は task として実行されます。アプリと同じく、`await` で待っているあいだもほかの処理は進み、メインスレッドを止めることはありません。
- `main` が戻ると、プログラムは終了コード 0 で終わります。その前に、開始したまま動いているもの（読み取り中の stream、パッケージの task）がキャンセルされ、それらの `defer` が実行されます。
- `main` が投げた（または捕まえなかった）エラーで、プログラムは 1 で終わります。stderr には `error: ` に続いてエラーのメッセージが出力され、投げられた場所が内側の関数から順に並びます。

  ```text
  error: bad name fail
      check (main.asuka:18)
      main (main.asuka:8)
  ```

- `os.exit(code)` はその場で `code` を終了コードとして終了します。stdout と stderr はフラッシュされますが、`defer` やほかの task は実行されません。
- `print` は stdout に 1 行書き、`os.eprint` は stderr に書きます。

## プロセス：std/os

| 関数 | 内容 |
|---|---|
| `os.args()` | コマンドライン引数（プログラム自身は含まない） |
| `os.program()` | 起動されたときのプログラム名やパス（`argv[0]`） |
| `os.env(name)` | 環境変数の値。設定されていなければ `none` |
| `os.environ()` | すべての環境変数（`[string: string]`） |
| `os.cwd()` | 作業ディレクトリ |
| `os.exit(code)` | プロセスをただちに終了する（型は `never`） |
| `os.readLine()` | stdin から行末を除いた 1 行。終わりに達すると `none`。行が来るまで待つ |
| `os.readAll()` | stdin の残り全部（`bytes`） |
| `os.lines()` | 届いた順の stdin の行（`Stream<string>`）。別のスレッドで読むので、そのあいだもプログラムは進む |
| `os.eprint(text)` | stderr に 1 行書く |
| `os.writeTo(.stdout, text)`、`writeBytesTo` | テキストやバイト列を、行末を付けずにそのまま `.stdout` か `.stderr` に書く |
| `os.terminal(.stdout)` | ストリームがパイプやファイルではなく端末（人が見ている）かどうか。色やプロンプトは端末のときだけ出す |

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

スマートフォンと Web のアプリでは、`os.args()` も stdin も空で、`os.exit` はエラーを投げます。そこでのアプリは自分のプロセスを終了できないからです。デスクトップアプリの `os.args()` は、そのアプリ自身のプロセスの引数です。

## コマンドラインのフラグ：std/flag

`flag.parse<T>(os.args())` は、コンパイラーが `T` について知っていることをもとに、コマンドラインを `T` に読み込みます。フィールドと同期させておくフラグの一覧表は要りません。

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

- **デフォルト値のある**フィールドはオプションです。`dryRun` は `--dry-run` になります。ドキュメントコメントがヘルプの説明になり、その最初の単語が `-x`（英字か数字 1 文字）なら、それが短い名前になります。
- `bool` はスイッチです（`--verbose`、`-v`）。デフォルトでオンのもの（`color: bool = true`）は `--no-color` でオフにでき、`--color=false` とも書けます。
- `int`、`float`、`string`、ペイロードを持たない enum のケース（`--unit words`。ケース名をケバブケースにしたもの）は値を取ります。`--jobs 8`、`--jobs=8`、`-j8`、`-j 8` のどれでもかまいません。配列は何度でも指定でき（`--skip a --skip b`）、オプショナルは省略できます。
- 短いスイッチはまとめて書けます。`-vf` は `-v -f` と同じで、`-vu words` なら値は `-u` に渡ります。
- **デフォルト値のない**フィールドは位置引数で、フィールドの順に並びます。配列は残りをすべて受け取るので最後に置き、オプショナルは省略できるので、省略できないものの後に置きます。
- `--` でオプションの終わりを示します。その後ろは `-x` で始まっていても位置引数です。
- `-h` と `--help` はヘルプを表示して 0 で終了し、`--version` は asuka.toml の `version` を表示して 0 で終了します。
- 指定に誤りがあると、何が問題かと使い方を表示して 2 で終了します。

  ```text
  error: unknown option --jbos (did you mean --jobs?)

  Usage: wc [options] [<files>...]

  For more, try 'wc --help'.
  ```

構造体のドキュメントコメントは、ヘルプの最初の行になります。

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

パーサーが扱えないものは、そのフィールドでのコンパイルエラーになります。書き下されていないデフォルト値（関数の戻り値など）、マップ、短い名前としての `-h`、デフォルト値のないスイッチがそうです。`flag.tryParse<T>(args)` は表示も終了もせずに `parse` と同じ処理をし、`flag.FlagError`（`.help(text)`、`.version(text)`、`.usage(problem, usage, program)`）を投げます。`flag.help<T>()` はヘルプの文字列を返します。

### サブコマンド

各ケースがオプションの構造体を持つ enum は、`git add` や `git commit` のようなサブコマンドの集まりになります。`flag.parse<Command>(args)` で読むか、その enum 型のフィールドを構造体に置きます。フィールドは構造体自身のオプションの後に来ます。ケースのドキュメントコメントがヘルプの説明になり、`tool add --help` はそのサブコマンド自身のヘルプを表示します。

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

## ほかのプログラムの実行：std/command

`std/command` はほかのプログラムを実行します。引数はリストで、シェルを通さずにそのまま渡されます。空白や `;` を含むファイル名も 1 つの引数です。ブロックする処理（パイプの読み取り、終了の待機）はほかのスレッドで行われるので、メインスレッドは待たされません。

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

- `command.run(program, args, cwd:, env:, clearEnv:, stdin:, check:)` はプログラムを最後まで実行し、`Output` を返します。`code`、`stdout`、`stderr`（バイト列。`text()` と `errorText()` でテキストに、`ok()` もあります）を持ちます。`stdin:` の内容はそのプログラムに渡され、`check: true` にすると 0 以外の終了コードが `CommandError.failed` になります。
- `command.status(program, args, …)` は、実行中のプログラム自身の stdin、stdout、stderr をそのまま使わせて（出力はそのまま表示されます）実行し、終了コードを返します。
- `command.start(program, args, …, stdin:, stdout:, stderr:)` はプログラムを開始し、`Process` を返します。`pid`、`stdout()` と `stderr()`（バイト列のストリーム）または `lines()` と `errorLines()`、`write(text)` と `closeStdin()`、`wait()`、`output()`（すべて読んでから終了コードを取る）、`kill(.term)` があります。各ストリームは `Stdio` で、`.piped`（stdout と stderr のデフォルト）、`.inherit`、`.null`（stdin のデフォルト）のいずれかです。
- プログラム名がパスでなければ `PATH` から探します。`env:` は実行中のプログラムの環境変数に上書きされます（`clearEnv: true` ならそれだけになります）。見つからないプログラムは `CommandError.notFound` になります。
- プログラムを待っている（`run`、`output`、`wait`）task がキャンセルされると、そのプログラムも終了します。
- macOS と Linux では、シグナルで終わったプログラムの終了コードは 128 にシグナル番号を足した値です。Windows では `CreateProcess` と同じ規則でプログラムを探します（`.exe` が補われます）。`.bat` や `.cmd` は、`cmd /c` をプログラムとして実行します。

`command.shell("…")` はありません。シェルは渡されたテキストの引用符や `$`、`;` を解釈するため、ファイル名がコマンドに化けてしまうからです。シェルの 1 行を意図して実行したいときは、シェルそのものを実行します：`command.run("sh", ["-c", line])`。

プログラムとデスクトップアプリ（macOS、Windows、Linux）で使えます。スマートフォンと Web のアプリはほかのプログラムを実行できません（`CommandError.unsupported`）。

## 端末：std/term

`std/term` は、プログラムの出力にスタイルを付けたり、端末の前にいる人に質問したりするためのパッケージです。次のプログラムはプロジェクトを作ります。プロジェクト名を尋ね、既存のディレクトリを上書きする前に確認し、出力の色は `--color` フラグでオンにもオフにもできます。

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

- **スタイル。** `term.bold(text)`、`dim`、`italic`、`underline` と 8 つの色（`red`、`green`、`yellow`、`blue`、`magenta`、`cyan`、`white`、`black`）は、テキストを端末の制御コードで囲みます。いくつかを同時に指定するには `term.Style` を使います。値として保存して使い回すこともできます：`let warn = term.Style(color: .yellow, bold: true)` としておき、`warn.paint(text)` を呼びます。`Style` には明るい色（`.brightRed`）と `background` もあります。スタイル付きのテキストを別のスタイル付きテキストの中に入れると、内側は自分のスタイルを保ち、その後ろでは外側のスタイルが続きます。
- **スタイルが表示される条件。** 出力先のストリームが端末で、`NO_COLOR` が設定されておらず（[no-color.org](https://no-color.org)）、`TERM` が `dumb` でないときだけ表示されます。`FORCE_COLOR` が設定されていれば、パイプへの出力でも表示されます（`0` か `false` ならオフ）。スタイルの関数はどれも、テキストを書き出すストリームを `stream:` で受け取ります（省略すると `.stdout`）。`term.colors(.stderr)` で判断の結果がわかり、`term.setColors(.always)`、`.never`、`.auto` でそれを上書きできます。対象は両方のストリーム、または `stream:` で指定した一方です。オプションの構造体に `color: term.ColorMode = .auto` というフィールドを書けば、`--color always|never|auto` フラグになります。Windows では、最初に出力するときにコンソールが制御コードを解釈するように設定されます（仮想ターミナル処理を有効にします）。
- **質問。** `term.prompt(question, default:)` は質問をして、入力された行を前後の空白を除いて返します。何も入力しなければ `default`（`[demo]` のように表示されます）、`none` は stdin が終わったこと（Ctrl-D、閉じたパイプ）を表します。`term.confirm(question, default: false)` は `[y/N]`（デフォルトが true なら `[Y/n]`）と尋ね、y、yes、n、no か空の答えが返るまで繰り返します。`term.secret(question)` は端末のエコーを切って 1 行読みます。パスワードやトークンの入力向けです。行を読み終えたとき、task がキャンセルされたとき、Ctrl-C でプログラムが終了したときのいずれでも、エコーは元に戻ります。質問は git や gh と同じく stderr に書かれるので、出力をパイプに流しても（`new | tee log`）stdout にはプログラム本来の出力だけが残ります。これらは `async` です。行は別のスレッドで読まれ、そのあいだもプログラムのほかの task は動き続けます。
- **サイズ。** `term.width()` と `term.height()` は端末の列数と行数です（`COLUMNS` と `LINES` が数値ならそちらが優先）。どのストリームも端末でなければ `none` になります：`term.width() ?? 80`。

テストでは `os.fake(stdin:)` で質問に答え、質問そのものは `o.stderr()` で確認できます。テストのストリームは端末ではないのでスタイルは付きませんが、`os.fake(env: ["FORCE_COLOR": "1"])` や `term.setColors(.always)` で指定すれば付きます。見るのは `os.fake` の `env:` だけなので、テストを実行する環境の変数がテストの出力を変えることはありません。スマートフォンと Web には端末がありません。スタイルは付かず、`prompt` は `none` を返し、`width` も `none` です。

## ファイルとディレクトリ：std/fs

プログラムもアプリと同じく `std/fs` でファイルを読み書きします（[データ](data.md)）。さらに、コマンドラインツールがファイルを探すのに必要な関数があります。

- `fs.home()`：ユーザーのホームディレクトリ（`HOME`、Windows では `USERPROFILE`。テストでは、`os.fake` の `env:` にあればその値）。
- `fs.tempDir()`：一時ファイル用のディレクトリ（`TMPDIR`、なければ `/tmp`。Windows では `GetTempPath` が返すもの）。
- `fs.abs(path)`：`path` を作業ディレクトリを基準に絶対パスにし、正規化したもの。
- `fs.walk(dir, skip:)`：ディレクトリの下にあるものすべてを、深さを問わず `Entry`（`path`、`name`、`isDir`、`size`、`modified`）のリストで返します。ディレクトリごとに、まずそれ自身、続いてその中身が名前順に並びます。`skip:` には中に入らないディレクトリの名前を書きます。ディレクトリへのリンクの中にも入りません。

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

ディスクにアクセスする呼び出し（`walk`、`readText`、`exists` など）は、システムが答えるまでブロックします。大きなディレクトリツリーをたどるときは `spawn { }` の中で呼べば、プログラムのほかの task は動き続けます。失敗すると、`std/fs` のほかの呼び出しと同じく `FsError` を投げます。一覧を読む権限のないディレクトリは `.denied(path)` です。

## 複数を同時に

`all(items, f, limit:)` は、各要素で非同期関数を同時に呼び出し、結果を要素の順に返します。ダウンロード、リクエスト、ほかのプログラムの実行を並べて行えます。`limit:` は同時に動かす数です。最初のエラーが残りをキャンセルし、それらが止まってから `all` がそのエラーを投げます（[非同期処理](language/async.md#非同期処理)を参照）。

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

## 形の決まっていない JSON

形がわかっている JSON は構造体で読みます（`json.decode<T>`、[データ](data.md)を参照）。ツールの設定ファイルやパイプで渡される任意の内容のように形がわからないものは `json.Value` で扱います。`json.parse(text)` はどんな JSON もそのまま読み込み、`get(key)`、`at(i)`、`asString()`、`asInt()` などで中身を見たり、`match` でケース（`.object(members)`、`.array(elements)`、`.string(s)` など）を取り出したりできます。表示しても `json.encode` で書き出しても JSON になります。

```asuka check
import std/json
import std/os

/// stdin の JSON のうち、パスの位置にある値を表示する：pick user.emails.0
async fn main() throws {
  var v = try json.parse(os.readAll().string())
  for step in (os.args().first() ?? "").split(".") {
    if step.isEmpty() { continue }
    let next = if let i = step.toInt() { v.at(i) } else { v.get(step) }
    v = next ?? json.Value.null
  }
  match v {
    .string(s) => print(s)       // 引用符なしのテキストとして
    _ => print(json.encode(v, indent: 2))
  }
}
```

## 設定ファイルとデータ：TOML と CSV

`std/toml` は、`std/json` が JSON を読むのと同じように設定ファイルを構造体に読み込みます。`toml.decode<Config>(text)` はフィールド名で値を読み（`min-qty` や `min_qty` と書かれたキーも `minQty` に入ります）、ファイルにないオプショナルは `none` になります。`toml.encode(config)` で書き戻し、`toml.parse(text)` はどんなドキュメントも `json.Value` として読みます。ファイルにないフィールドは JSON と同じく空の値（0、`""`、none）になり、構造体のデフォルト値にはなりません。省略されうる設定はオプショナルにするか、デフォルト値で構造体を作っておき、ファイルがあるときだけ読み込みます。間違いがあれば場所を示します：`toml: 2:11: min-qty: expected an integer, not a string`。

`std/csv` は行を読みます。`csv.parse(text)` は `[[string]]` として、`csv.decode<T>(text)` はヘッダー行にもとづいて構造体として読み（セルはフィールドの型で読まれます：int、float、bool、列挙型のケース。オプショナルの空のセルは `none`）、`csv.encode(rows)` はヘッダーとともに書き戻します。メモリーに収まらないほど大きいファイルは、行のストリームから 1 行ずつ読めます：`csv.stream<T>(os.lines())`。次の例は、パイプで渡された売上データを `~/.config/sales/config.toml` の設定に従って集計します。

```toml
currency = "USD"
min-qty = 2
```

```
item,qty,price
"Tea, green",2,3.5
Cake,1,4.25
```

```asuka check
import std/csv
import std/fs
import std/os
import std/toml

/// パイプで渡された CSV の売上を集計します。
struct Config {
  currency: string = "EUR"
  /// 個数がこれより少ない行は数えません。
  minQty: int? = none
}

struct Sale {
  item: string
  qty: int
  price: float
}

async fn main() throws {
  let path = fs.join([fs.home(), ".config", "sales", "config.toml"])
  var config = Config()
  if fs.exists(path) {
    config = try toml.decode<Config>(try await spawn { try fs.readText(path) })
  }
  var total = 0.0
  for await sale in csv.stream<Sale>(os.lines()) {
    if sale.qty >= (config.minQty ?? 1) {
      total += sale.price * float(sale.qty)
    }
  }
  print("{total} {config.currency}")
}
```

`separator: "\t"` でタブ区切りのデータを読めます。`comment: "#"` はその文字で始まる行を飛ばし、`flexible: true` は行ごとにフィールド数が違ってもよいことにします。

## 実行

`asuka run` はプログラムの `main` をすぐに実行するので、ビルドを待つ必要がありません。`--` の後ろの引数はプログラムに渡され、プログラムの終了コードでそのまま終了します。

```sh
asuka run -- --jobs 8 a.txt b.txt
asuka run --bin notes -- add "Buy milk"   # asuka.toml の [bin] にあるプログラムの 1 つ
```

`asuka dev` はプログラムを実行し、保存するたびに実行し直します（`cargo watch -x run` と同じ流れです）。保存したときに前の実行がまだ続いていれば、先にそれを止めます。`fn main` を持たないファイルやパッケージに対しては、`asuka run` はこれまでどおりテストブロックを実行し、アプリの画面を表示します。

## ビルド

`asuka build` はプログラムをコンパイルし、このコンピューター用の実行ファイルを `dist/` に作ります。

```sh
asuka build                          # dist/greet
asuka build --target linux-amd64     # dist/linux-amd64/greet
asuka build --target windows-arm64   # dist/windows-arm64/greet.exe
```

`--target` は `darwin`、`linux`、`windows` のいずれかと、`amd64` か `arm64` の組み合わせです。このコンピューター以外のシステム向けには、デスクトップアプリと同じく Zig でビルドします。実行ファイルはシステムが許す範囲で単体で動きます。Linux では静的にリンクされます（musl、Zig を使う場合）。ただし、使っているものがシステムのライブラリを読み込む場合（std/http の libsoup）は除きます。Windows ではシステムの DLL だけに、macOS ではシステムのライブラリだけに依存します。

プラットフォームのコードを持つパッケージも、どのデスクトップでもプログラムから使えます。`std/http` はアプリと同じく、macOS では URLSession、Windows では WinHTTP、Linux では libsoup でリクエストを送ります。ただし、ウィンドウはありません。SQLite（`std/sqlite`）は、アプリと同じく実行ファイルに組み込まれます。

### アプリに付属するプログラム

VS Code が `code` コマンドを付けているように、アプリはプログラムを付属させられます。ターミナルからアプリのデータを扱うコマンドです。1 つのパッケージに `app { }` と `fn main()` の両方は置けないので、プログラムはそれぞれ専用のディレクトリに置き、asuka.toml で宣言します。

```toml
[bin.notes]
path = "cli"
```

プログラムは、ほかのパッケージと同じように相対パスでプロジェクトのパッケージをインポートします（`import ../model`）。`asuka run --bin notes` で実行し（プログラムが 1 つだけなら `--bin` は省略できます）、`asuka test` はアプリのテストと一緒に各プログラムのテストも実行し、`asuka build --bin notes` はそのプログラムだけをビルドします。アプリに付属するプログラムは、データをアプリと同じ場所に保存します（std/fs の `appDir` と、その中の SQLite データベース）。そのため両者は同じデータを読み書きします。開発中の `asuka run --bin` は `asuka dev` のデータを使います。

デスクトップアプリのビルドは、プログラムをアプリの中に入れます。

| ビルド | 置き場所 | ユーザーの PATH への追加 |
|---|---|---|
| macOS | `Notes.app/Contents/Helpers/notes`。アプリと同じアーキテクチャで、アプリと一緒に署名される | ユーザーが（またはアプリが案内して）リンクを作る：`ln -s /Applications/Notes.app/Contents/Helpers/notes /usr/local/bin/notes` |
| Linux | インストール先の `bin/notes`。アプリの実行ファイルの隣にあり、tarball と Flatpak にも入る | アプリと一緒に `~/.local/bin` や `/usr/bin` に入る。Flatpak では `flatpak run --command=notes <id>` |
| Windows | アプリの `.exe` の隣の `notes.exe` | インストーラーがアプリのフォルダーを `PATH` に追加する |

スマートフォンと Web は `[bin]` を無視します。[examples/notes](../../examples/notes) は `notes` コマンドを付属させたアプリで、このコマンドでアプリのメモの一覧表示、追加、完了ができます。

### ほかのツールチェーンがビルドするプログラム

ほかのツールチェーンがビルドするプログラム（Go で書いたサーバー、Rust で書いたツールなど、アプリが起動するもの）も同じようにアプリに入ります。その `[bin]` にはディレクトリの代わりにビルドするコマンドを書き、アプリをビルドするターゲットに対して実行されます。プレースホルダーはツールチェーンごとの名前でターゲットを表します。Go には `{goos}` と `{goarch}`、Rust にはトリプルの `{target}`。`{os}` と `{arch}` は Asuka の名前（`macos`、`x64`）で、自分のスクリプト向けです。`{out}` に書き出せないビルド（cargo など）は、`output` でプログラムを置く場所を指定します（一覧は [asuka.toml](asuka-toml.md#bin) にあります）。

```toml
[bin.notes-server]
build = ["go", "build", "-o", "{out}", "./cmd/server"]
env = { GOOS = "{goos}", GOARCH = "{goarch}", CGO_ENABLED = "0" }

[bin.notes-index]
build = ["cargo", "build", "--release", "--manifest-path", "index/Cargo.toml", "--target", "{target}"]
output = "index/target/{target}/release/notes-index{exe}"
```

そのシステム上でビルドするとき、`{target}` はそのシステムのトリプル（Windows では `x86_64-pc-windows-msvc`）です。クロスビルドでは、相手のシステムの SDK なしでビルドできるトリプル（`x86_64-pc-windows-gnu`、MinGW。`x86_64-unknown-linux-musl`、静的リンク）になります。Rust のターゲットは `rustup target add` で入れ、クロスビルドにはそのリンカーも必要です。

プレースホルダーが望む引数にならないときは、`build` で自分のスクリプトを実行し、実行時に引数を決めさせられます。ターゲットは環境変数（`ASUKA_OS`、`ASUKA_ARCH`、`ASUKA_EXE`、`ASUKA_TARGET`、`ASUKA_GOOS`、`ASUKA_GOARCH`）にあり、プログラムは `ASUKA_OUT` に書き出します。次のスクリプトは、どこでも静的リンクの Linux プログラムをビルドし、Mac では feature を有効にします。

```toml
[bin.notes-index]
build = ["sh", "index/build.sh"]
```

```sh
#!/bin/sh
# index/build.sh: プロジェクトのディレクトリで、ターゲットごとに 1 回実行されます。
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

スクリプトはアプリを実行するマシンではなく、ビルドするマシンで動きます。Mac から Windows 版をビルドするときは Mac の `sh` で実行されます。Windows でビルドするなら、Windows が実行できるものを指定します。`["pwsh", "-File", "index/build.ps1"]`（出力先は `$env:ASUKA_OUT`）や、任意の言語のプログラム `["go", "run", "./tools/build-index"]` などです。

アプリは `command.bundled(name)` で自分のプログラムを見つけます。ビルドの中のパスで、ない場合（スマートフォン、Web、テスト）は none です。`asuka dev` は起動時にビルドし、アプリは同じ方法で見つけます。

```asuka
import std/command

async fn startServer() throws -> command.Process {
  let server = command.bundled("notes-server") ?? "notes-server" // なければ PATH から
  return try command.start(server, ["serve"])
}
```

## テスト

プログラムのテストブロックは、アプリと同じく `asuka test` で実行します。`main` は関数なので、テストでは `os.fake` を使って本物のプロセスと同じように実行します。`os.fake` は引数、環境変数、stdin を与え、書き出された内容を保存します。

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

`o.run(main)` は、プログラムが終わるはずの終了コードを返します。0、エラーを投げたときの 1（メッセージは `o.stderr()` に入ります）、または `os.exit` が指定した値です。このときの `os.exit` は、プロセスではなくテスト内の `main` をそこで終わらせます。fake がなければ、テストの `os.args()` と stdin は空です。`command.fake()` はほかのプログラムの代わりに応答するので、それらは実際には実行されません。

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

## プラットフォームごとの対応

| | macOS | Windows | Linux | スマートフォン、Web |
|---|---|---|---|---|
| プログラム（`asuka build`） | ○ | ○ | ○ | × |
| `os.args`、`os.exit`、stdin | ○ | ○ | ○ | アプリでは空、`exit` はエラーを投げる |
| `std/command` | ○ | ○ | ○ | `unsupported` |
| `std/flag` | ○ | ○ | ○ | ○（どんなリストでも解析できる） |
| `std/term` | ○ | ○ | ○ | 端末がない（スタイルなし、`prompt` は `none`） |
