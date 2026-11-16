# 0007: A C core and the Asuka language

- **Status**: Accepted (in progress, see "Milestones")
- **Created**: 2026-11-16
- **Related**: 0001, 0002 (kept state and views across reloads), 0003
  (rejected: patching Go), 0006 (rejected: code push), DESIGN.md

## Summary

Asuka's core (signals, the node tree and its flush, layout, the ops
encoder) is rewritten in C, and apps are written in a language of their
own, **Asuka**: a statically typed, declarative language for UI with
enough of a general language (structs, enums, functions, async) to
write a whole app in. The compiler, written in Go, is part of the
`asuka` CLI. It compiles an app to **bytecode** that a small VM in the
C core runs in development (a save reloads in milliseconds, with every
signal and every native view kept), and to **C** that is built with the
core for release.

What stays: the ops wire and its schema (`ops/schema.go`), the platform
hosts in Swift, Kotlin and JavaScript (`platform/*`), which only ever
see encoded batches and call the C ABI of `asuka.h`, the desktop hosts
in Go (`host/darwin`, `host/windows`, `host/linux`), which drive the C
core through the same ABI (purego, no cgo), and the CLI. What goes: the
Go packages an app imported (`signal`, `ui`, `layout`, `app`, `colors`,
`fonts`, `uitest`), `mobile/`, `host/web`, and apps written in Go. The
project is experimental; nothing is kept for compatibility.

## Motivation

- **Reloads that keep everything.** 0003 found that Go can't load new
  code into a running process without cgo and `plugin`, and that the
  closures UI code is made of can't be patched. A VM we own can swap a
  component's code and keep its state cells and its native views.
- **Code push** (0006) becomes possible for the same reason: bytecode
  can be downloaded; Go machine code can't.
- **Size and start-up.** No Go runtime, no garbage collector, no
  goroutine scheduler on the phone; the web build is a small `.wasm`
  instead of several megabytes of Go runtime.
- **Bugs the type checker can see.** Go can't tell a signal read once
  from one that's tracked, or a write from a goroutine from one on the
  main thread; `asuka vet` guesses. A language whose component bodies
  are declarations, and whose background blocks can't capture state,
  makes both compile errors.
- **One embeddable core.** A C library with a C ABI is what Swift,
  Kotlin, JavaScript (through WebAssembly) and every desktop toolkit
  can call without a second runtime.

## Decisions

| Question | Decision | Why |
|---|---|---|
| Core language | C17, no C++ | Every platform's toolchain and FFI speaks C; Win32 and GTK are C APIs; AppKit is reached through `objc_msgSend`. Each platform's own compiler builds it; Zig's `zig cc` cross-compiles it. |
| App language | Asuka, statically typed, declarative component bodies | See "The language". |
| Compiler written in | Go, in `lang/`, run by the CLI | The CLI is Go; the device never compiles. Fast to write, cross-platform, and the same binary runs the language server. |
| Dev execution | Bytecode on a register VM in the core | Reload without restarting, on every platform, physical devices included (iOS can't load new machine code). |
| Release execution | C compiled from the bytecode (lang/cgen), with the core | The bytecode is the one contract: a function's C calls the same instruction bodies the interpreter runs (core/src/vm/ops.h), so the backends can't drift; every VM test runs on both. Every function is native: a synchronous one a plain C function, an async one a resumable C function on the VM's coroutine frames, as other languages compile coroutines (lang/LOWERING.md). |
| Memory | Values with value semantics, copy on write, reference counted; owners free what they own | No GC pauses; no cycles by construction (see "Memory"). |
| Concurrency | `async`/`await` compiled to stackless coroutines; tasks owned by owners; `spawn` for worker threads, checked | No stack switching, so the same code runs on wasm; `await` marks where state may change. |
| Reactivity | Fine-grained signals (alien-signals' algorithm, Solid's owners), with dependencies resolved at compile time where they're static | As before, minus runtime tracking for the common case. |
| Layout | The CSS block, flexbox and grid engine, ported to C, checked against the same Chrome fixtures | Unchanged semantics. |
| C compiler | The system's by default (Xcode's clang for Apple platforms, the NDK's clang for Android, cc on Linux, clang on Windows); Zig's `zig cc` opted into (`ASUKA_CC=zig`), and used where only it can: cross-compiling a desktop build for another system, the web without an LLVM that targets wasm | The platforms' own compilers are what their SDKs are tested with and what developers already have; Zig is one download that cross-compiles to everything, for what the system's can't do. |
| Desktop hosts | Stay in Go, in process, driving the C core (a library, `libasuka`) through `asuka.h` with purego callbacks. **Superseded by RFC 0008**: no Go at run time; the macOS host in Swift, the Windows and Linux hosts in C. | They were already hosts behind the ops wire. RFC 0008 weighs what this missed: a second runtime in every desktop app, and package code in a language neither platform speaks. |
| Packages | The npm registry, with the `asuka` CLI as the client: its own resolver (one version per package, a lockfile), no Node, no `node_modules` | See "Packages". The registry gives scopes, auth, private registries, provenance and a CDN for free; npm's install model (nested duplicate versions, install scripts) doesn't fit a compiled language. |
| Installing the CLI | npm (`npm i -g @egoist/asuka`, `npx @egoist/asuka`: the unscoped `asuka` is taken), with a prebuilt binary per platform as an optional dependency (esbuild's pattern); also a shell installer and Homebrew | Most app developers already have npm; nothing is built on install. |

## The language

### Lexical structure

- Source files are UTF-8, extension `.asuka`. Statements and members
  end at a newline; `;` separates several on one line. A newline after
  an operator, a `,`, an opening bracket or `=>` doesn't end anything.
- Comments: `// line`, `/* block */`; `///` documents the declaration
  that follows.
- Identifiers: letters, digits, `_`; a type, component or enum's name
  starts with an upper-case letter by convention (required for
  components, so `Text(...)` in a view is never a plain call).
- Literals: `42`, `1_000`, `0xff`, `3.14`, `1e-3`; `true`, `false`,
  `none`; strings `"…"` with escapes (`\n \t \" \\ \{ \u{1F600}`) and
  interpolation `"Count: {count}"` (any expression inside `{}`), raw
  strings `r"…"`, multi-line strings `"""…"""`; durations `300ms`,
  `5s`, `2min`, `1h`; lengths `50%`, `1fr` (grid); colors `#rgb`,
  `#rrggbb`, `#rrggbbaa`.
- Keywords: `app as async await bind break catch component continue
  defer do effect else emit enum event false fn for if import in
  interface key let load match native none on prop provide pub return
  screen self spawn state store struct style task test throw throws
  true try type use var while`.

### Grammar

```ebnf
File        = { Import } { Decl } .
Import      = "import" ImportPath [ ".{" Ident { "," Ident } "}" ] [ "as" Ident ] .
ImportPath  = Ident { "/" Ident }                    (* a package: std/http *)
            | "./" Ident { "/" Ident } .              (* a directory beside this one *)

Decl        = [ "pub" ] ( Component | Screen | Store | Struct | Enum | Interface
            | Func | TypeAlias | StyleDecl | StateDecl | LetDecl | Native | App )
            | Test .

Component   = "component" Ident [ TypeParams ] [ Params ] Block .
Screen      = "screen" Ident [ Params ] Block .        (* a component whose root is a Screen *)
Store       = "store" Ident [ Params ] "{" { Member } "}" .
Struct      = "struct" Ident [ TypeParams ] [ ":" Ident { "," Ident } ] "{" { Field | Func } "}" .
Field       = Ident ":" Type [ "=" Expr ] .
Enum        = "enum" Ident [ TypeParams ] [ ":" Ident ] "{" Case { ("," | NL) Case } { Func } "}" .
Case        = Ident [ "(" [ Ident ":" ] Type { "," [ Ident ":" ] Type } ")" ] .
Interface   = "interface" Ident "{" { FuncSig | Field } "}" .
Func        = FuncSig ( Block | "=" Expr ) .
FuncSig     = [ "async" ] "fn" Ident [ TypeParams ] "(" [ Param { "," Param } ] ")"
              [ "throws" ] [ "->" Type ] .
Param       = Ident ":" Type [ "=" Expr ] .
Params      = "(" [ Param { "," Param } ] ")" .
TypeParams  = "<" Ident { "," Ident } ">" .
TypeAlias   = "type" Ident [ TypeParams ] "=" Type .
StyleDecl   = "style" Ident "{" { PropSet } "}" .
StateDecl   = "state" Ident [ ":" Type ] "=" Expr .
LetDecl     = "let" Pattern [ ":" Type ] "=" Expr .
Native      = "native" ( Interface | "component" Ident [ Params ] "{" { Member } "}" ) .
App         = "app" Block .
Test        = "test" String Block .                  (* its body is statements *)

(* A component's body, a store's body and an element's block hold items. *)
Block       = "{" { Item } "}" .
Item        = Member | ViewItem .
Member      = "prop" Ident [ ":" Type ] [ "=" Expr ]
            | "event" Ident [ "(" [ Param { "," Param } ] ")" ]
            | StateDecl | LetDecl
            | "use" Ident ":" Type
            | "provide" Expr
            | Func
            | "task" [ "(" Expr { "," Expr } ")" ] StmtBlock
            | "effect" StmtBlock .
ViewItem    = PropSet                                (* ident ":" expr *)
            | "on" Ident [ "(" [ Ident { "," Ident } ] ")" ] StmtBlock
            | "bind" Ident ":" LValue
            | "style" Ident
            | Ident Block                            (* a named slot: toolbar { … } *)
            | IfView | ForView | MatchView
            | Expr .                                 (* a View: an element or a View-typed value *)
PropSet     = Ident ":" Expr .
IfView      = "if" Cond Block [ "else" ( IfView | Block ) ] .
ForView     = "for" Pattern "in" Expr [ "key" Expr ] Block .
MatchView   = "match" Expr "{" { Pattern [ "if" Expr ] "=>" ( ViewItem | Block ) } "}" .
Cond        = Expr | "let" Pattern "=" Expr .        (* if let u = user *)

StmtBlock   = "{" { Stmt } "}" .
Stmt        = ( "let" | "var" ) Pattern [ ":" Type ] "=" Expr
            | LValue AssignOp Expr                   (* = += -= *= /= %= ??= *)
            | "if" Cond StmtBlock [ "else" ( IfStmt | StmtBlock ) ]
            | "for" Pattern "in" Expr StmtBlock
            | "for" "await" Pattern "in" Expr StmtBlock
            | "while" Cond StmtBlock
            | "match" Expr "{" { Pattern [ "if" Expr ] "=>" ( Stmt | StmtBlock ) } "}"
            | "do" StmtBlock { "catch" CatchPat StmtBlock }
            | "return" [ Expr ] | "throw" Expr | "break" | "continue"
            | "defer" StmtBlock
            | "emit" Ident [ "(" Args ")" ]
            | Expr .
CatchPat    = [ Ident [ ":" Type ] ] | QualifiedCase [ "(" Pattern { "," Pattern } ")" ] .

Expr        = Lambda | Binary .
Lambda      = ( Ident | "(" [ Param' { "," Param' } ] ")" ) "=>" ( Expr | StmtBlock ) .
Binary      = Unary { BinOp Unary } .                (* precedence below *)
Unary       = { "!" | "-" | "try" | "try?" | "await" } Postfix
            | "load" Postfix | "spawn" StmtBlock .
Postfix     = Primary { "." Ident | "?." Ident | Call | "[" Expr "]" | "!" } .
Call        = "(" [ Args ] ")" [ Block ] | Block .   (* a trailing block: an element's *)
Args        = Arg { "," Arg } .
Arg         = [ Ident ":" ] Expr .
Primary     = Literal | Ident | "self" | "." Ident    (* implicit member: .gray *)
            | "(" Expr { "," Expr } ")"               (* grouping or a tuple *)
            | "[" [ Expr { "," Expr } ] "]"           (* an array *)
            | "[" ":" "]" | "[" Expr ":" Expr { "," Expr ":" Expr } "]"   (* a map *)
            | "if" Cond Block "else" Block            (* an if expression *)
            | "match" Expr "{" { Pattern "=>" Expr } "}" .

Type        = ( Ident [ "<" Type { "," Type } ">" ] { "." Ident }
            | "[" Type "]" | "[" Type ":" Type "]"
            | "(" Type { "," Type } ")"
            | [ "async" ] "fn" "(" [ Type { "," Type } ] ")" [ "throws" ] [ "->" Type ] ) [ "?" ] .

Pattern     = "_" | Ident | Literal | "." Ident [ "(" Pattern { "," Pattern } ")" ]
            | QualifiedCase [ "(" Pattern { "," Pattern } ")" ]
            | "(" Pattern { "," Pattern } ")" | Pattern "|" Pattern .
```

Operator precedence, loosest first: `??`; `||`; `&&`; `== != < <= > >=`;
`..` `..<` (ranges); `+ -`; `* / %`; the unary operators; the postfix
ones. `??` is right-associative, the rest left-associative; comparisons
don't chain.

### Types

- Scalars: `int` (64-bit), `float` (64-bit), `bool`, `Duration`,
  `Color`, `Length`; `void`.
- Values (value semantics, copied on write): `string` (UTF-8,
  immutable), `bytes`, `[T]` arrays, `[K: V]` maps (insertion ordered),
  tuples `(A, B)`, `struct`s, `enum`s (cases with payloads), optionals
  `T?` (`none`).
- References (identity): `store`s, functions and closures, `Task<T>`,
  `Loading<T>`, native objects, `View`.
- `Error` is an interface with `message: string`; structs and enums
  declare they are errors with `: Error`.
- Generics on functions, structs, enums and type aliases; constraints
  are the built-in `Equatable`, `Comparable` and `Hashable`, which
  structs and enums get when all their fields have them.
- Interfaces are structural: a type has an interface when it has its
  methods and fields.
- Inference: a `let`, `var`, `state` or `prop` with a default needs no
  type; parameters and results of declared functions always have one;
  lambdas take theirs from where they're passed. An implicit member
  `.gray` takes its type from what's expected.

### Components, props and events

A component is declared with its positional parameters in its header
and its named props, events and state in its body:

```asuka
component MessageRow(message: Message) {
  prop compact = false
  prop accessory: View? = none
  event archive

  Row {
    gap: 12
    Text(message.subject)
    if !compact { Text(message.preview) { color: .secondaryLabel } }
    accessory
  }
}
```

Using it is a call with a trailing block: header parameters go in the
parentheses (positionally or by name), everything else in the block:
props as `name: value`, handlers as `on name { … }`, two-way bindings
as `bind name: target`, styles as `style name`, children and named
slots. Each thing has one place; setting a header parameter in the
block, or a `prop` in the parentheses, is an error.

- A prop of type `View` is a slot; the children of a block go to the
  component's `children: View` prop, which a component that takes
  children declares.
- A named slot is `name { … }` in the block (`toolbar { … }`).
- `emit name(args)` raises an event; the caller's `on name(args) { }`
  handles it. An event nobody handles does nothing.
- `bind value: query` passes `query` (a `state`, or a field of one) for
  the control to read and write: two-way only where it says so.

### Reactivity

**A component body is declarations, not code.** It isn't run once and
forgotten: each declaration is a live binding the compiler turns into
the signal graph.

- `state x = e`: a signal, owned by the component instance (or the
  store, or the app when it's at the top level of a file).
- `let y = e` in a component, store or file: derived, a memo. In
  statements, `let` is an ordinary immutable local and `var` a mutable
  one.
- Header parameters and props are accessors: reading one in a binding
  tracks what the caller passed.
- Every prop value of an element, every `if`, `for` and `match` in a
  view, is a binding that re-evaluates when what it read changes, and
  sends the host only what changed.
- `effect { }` runs its statements now and again whenever what they
  read changes; `task(a, b) { }` restarts when `a` or `b` changes,
  cancelling the run before; `task { }` runs once, while mounted.
- In handlers, functions and tasks, reading state gives its current
  value, untracked. Writes are batched: they show at the end of the
  handler, or at its next `await`.
- `for item in items key item.id { … }` keeps a row per key; a row's
  `item` follows its key's value, so a row whose item changed updates
  its bindings and nothing else. Directly under a `List`, the rows are
  virtualized (only what's on screen exists).
- Writing a field or an element of a state value (`todos[i].done =
  true`) writes the state: arrays and structs are copied on write, and
  untouched elements keep their identity, so only the rows that read
  the changed element re-run.

### Async

- `async fn` may `await`; calling one needs `await`, or `spawn`ing it.
  Handlers (`on`), `task`, `effect`'s cleanups and `test` bodies are
  async contexts; component bodies and memos are not.
- Every task belongs to the owner that started it (a component, a
  store, the app). Disposing the owner cancels the task: the next
  `await` it reaches unwinds it, running its `defer`s, without an
  error anyone sees.
- `await all(a, b)` runs calls concurrently; `await race(a, b)` takes
  the first; `withTimeout(5s) { }`, `sleep(300ms)`.
- `spawn { … }` runs a block on a worker thread and is a `Task<T>`
  (`await` it for its value). It may capture only values that can
  cross threads: no state, stores, views, tasks or closures that hold
  them. Everything else stays on the main thread, where the core runs.
- `load f(args)` in a component or store is a `Loading<T>`: `.pending`,
  `.ready(value)`, `.failed(error)` (match it), `.value` (the latest
  value, kept while reloading), `.reload()`. It re-runs when what its
  arguments read changes, cancelling the run that's going stale.
- `for await x in stream { }` reads an async stream (a socket's
  messages, a sensor); it ends with the owner.

### Errors

A function that can fail says `throws`; calling it needs `try` (which
passes the error on), `try?` (an optional: `none` on error), or a `do
{ } catch`. `throw e` with any `Error`. An error that leaves a handler
or a task goes to the nearest `Boundary { } on error(e) { }` around the
component, and to the app's default (logged; shown in development) when
there's none.

### Memory

- Values are reference counted and copied on write; a value with one
  owner is changed in place. Strings, arrays, maps, structs and enums
  with payloads are values.
- Closures capture values by value and state by reference. A state
  cell, a store and a task belong to an owner, and disposing the owner
  releases them: that breaks any cycle that went through them, and
  nothing else can make one (values can't refer to themselves).
- Hot paths (signal propagation, the flush, layout, op encoding) don't
  allocate in the steady state, as before.

### Styles

Styling props are ordinary props of the built-in elements: layout
(`width height minWidth … padding margin gap rowGap columnGap flex
grow shrink basis direction wrap align alignSelf justify position top
right bottom left inset aspectRatio display grid columns rows`) and
look (`background radius border borderColor opacity shadow color font
fontSize weight lines textAlign …`). A `style name { … }` declares a
reusable set, applied with `style name` in a block; props set after it
in the block override it. A style setting a prop an element doesn't
take is an error where it's applied.

### Navigation

`Stack { Inbox() }` is a navigation stack; `push(Screen(…))`, `pop()`,
`present(view)`, `dismiss()` act on the nearest stack or sheet of the
code that calls them. `screen Name(…) { }` is a component whose root
is a `Screen`, so its block takes the screen's props (`title`,
`largeTitle`, …) and slots (`toolbar`).

### Text and languages

`tr.inbox.title(unread: n)` reads an app's catalogs (docs/i18n.md),
checked at compile time. Every text prop is a binding, so changing the
app's language updates what's shown with no app code.

### Modules and packages

A directory of `.asuka` files is a package: its files see each other's
declarations. `pub` exports a declaration from its package. `import
std/http` imports a package of the standard library (`platform/camera`,
of the platform SDK) or a dependency;
`import ./components` one beside the importer; `.{A, B}` imports names
unqualified; `as` renames. The built-in elements, `Color`'s semantic
colors, fonts and the core functions (`push`, `sleep`, `all`, …) are in
the prelude.

### Packages

Packages share components, stores, native code (a `native` package's
Swift, Kotlin, JavaScript and C) and prebuilt libraries (plugins). They
live on the **npm registry**, but npm doesn't install them: the `asuka`
CLI is the client, and it needs no Node.

- **A package** is a directory with `asuka.toml` (`name`, `version`,
  `[dependencies]`, and what it is: a library, a native package, a
  plugin) and its `.asuka` sources. `asuka pkg publish` makes the npm
  package from it: `package.json` (name, version, `"asuka"` pointing at
  the manifest, `"files"`) and the tarball, and uploads it with the
  user's npm credentials (`~/.npmrc`, `NPM_TOKEN`), with provenance on
  CI. Scoped names (`@acme/charts`) work as on npm, and so do private
  registries (GitHub Packages, Verdaccio, a company's), set in
  `.npmrc`.
- **Depending** on one is `asuka add @acme/charts`, which writes it in
  `asuka.toml`:

  ```toml
  [dependencies]
  "@acme/charts" = "^1.2.0"
  ```

  and `import @acme/charts` in the source. Git dependencies
  (`"github:acme/charts#v1.2.0"`) and local paths (`"file:../charts"`)
  work too, for what isn't published.
- **Resolving** picks one version of each package for the whole app,
  by minimal version selection (Go's): the least version that meets
  every requirement, so a build never changes because someone
  published something. Two incompatible major versions are two
  packages (`@acme/charts` and `@acme/charts@2` imported under their
  own names), never two copies of one type. The result goes in
  `asuka.lock`, with each tarball's integrity hash, and the CLI checks
  every download against it.
- **Installing** fetches tarballs straight from the registry's HTTP API
  into a content-addressed cache (`~/.cache/asuka/pkg`), shared by every
  project, read in place: no `node_modules`, and no install scripts, ever.
  A plugin's prebuilt libraries are files its manifest names with their
  SHA-256 (docs/plugins.md), fetched the same way, per platform, only
  when a build needs them.
- **Versions** are semver. `asuka pkg publish` checks a version against
  the last one: a `pub` declaration removed or changed incompatibly
  needs a new major version (the checker compares the two packages'
  exported APIs).
- The standard library (`std/http`, `std/sqlite`, ...) and the
  platform SDK (`platform/camera`, ...) come with the CLI: they're
  versioned with it and need no install.

### Native code

`native interface Haptics { fn impact(style: Impact) }` and `native
component MapView(region: Region) { prop pins: [Pin] = []; event
regionChanged(region: Region) }` declare what each platform implements
in its own project (docs/native.md): Swift, Kotlin, JavaScript, and C
on the desktop. `asuka native generate` writes each platform's glue
from the declarations.

### Tests

`test "name" { }` is a test: `mount(view)` renders it on the fake host
and returns an app to drive: `tap("Add")`, `type("Hello", into:
"Title")`, `expect("Count: 1")`, `expectNo(…)`, `await idle()`.
`asuka test` runs a package's tests.

## The compiler (`lang/`, Go)

```
lang/syntax      tokens, the parser, the AST, the printer (asuka fmt)
lang/check       scopes, types, the reactive analysis, diagnostics
lang/ir          a typed, lowered IR: SSA-free three-address code with
                 explicit retain/release and async state points
lang/bytecode    the bytecode format and its writer (opcodes defined
                 here; go generate writes core/src/vm/opcodes.h)
lang/cgen        C from the bytecode (release builds)
lang/prelude     the built-in elements, generated from ops/schema.go
                 and the layout style table
lang/lsp         the language server (diagnostics, hover, completion,
                 go to definition, formatting)
```

Diagnostics carry a file, a line, a column and a range, a message and,
where it's obvious, a fix. The compiler is incremental per package; a
save recompiles the package that changed and those that import it.

## The C core (`core/`)

```
core/include/asuka.h    the C ABI hosts call (as platform/ios's today)
core/src/base           allocation, arenas, strings, vectors, hashing
core/src/signal         the reactive graph and owners
core/src/layout         block, flexbox and grid
core/src/ops            the batch encoder; ops_gen.h from the schema
core/src/ui             the node tree, the flush, built-in elements'
                        behavior (lists, navigation, text input, …)
core/src/vm             the bytecode loader and interpreter, values,
                        tasks
core/src/rt             the runtime both backends call: strings,
                        arrays, maps, formatting, time, JSON
core/src/std            the standard library's native fns: std/time,
                        url, regex, encoding, crypto, html, random, fs
core/src/abi            asuka_start, asuka_event, … (mobile, web,
                        desktop entry points)
core/host/fake          the test host: records ops, measures text at 8
                        points a character
core/test               C tests: signals, layout fixtures, op goldens,
                        the UI runtime, the VM
```

Rules (also in AGENTS.md): C17 with `-Wall -Wextra -Werror`; internal
symbols `ak_`, the ABI `asuka_`; no global mutable state outside one
`ak_core` (one per process today, but nothing assumes a single root);
everything on the main thread except `asuka_post` and the worker
threads `spawn` uses; no allocation on hot paths; tests under ASan and
UBSan.

## Development and release

- `asuka dev` compiles the app, starts the platform's player (the C
  core with the VM and the platform's host) and sends it the bytecode.
  A save sends the packages that changed; the VM swaps their code and
  keeps state cells (by component path and name) and native views
  (0001's matching, done in the core).
- `asuka build` compiles the app to C, builds it with the core for the
  target and packages it as today (`.ipa`, `.aab`, `.app`, …).
- `asuka test` runs `test` blocks on the fake host, through the VM.

## Milestones

Each ends with its tests green.

- [x] **L0** this RFC; the docs' language reference (`docs/language.md`).
- [x] **C0** the core in C: base, signal, layout (Chrome fixtures at
      parity with `known_failures.txt`), ops encoder (byte-identical to
      the golden files), the fake host.
- [x] **L1** the compiler front end: lexer, parser, printer, `asuka fmt`.
- [x] **L2** the checker, the IR, bytecode; the VM; the UI runtime for
      the v0 elements; `asuka test`: counter and todo pass their tests.
- [x] **M1** iOS and Android: the core built for both, the hosts
      unchanged, `asuka dev` with bytecode reloads, `asuka build`.
- [x] **W1** the web: the core as WebAssembly, `platform/web` unchanged.
- [x] **L3** the C backend for release builds; both backends pass the
      same conformance suite.
- [x] **E1** every element of the schema in the UI runtime; the
      examples ported to Asuka (what they still ask of the platforms:
      `examples/STDLIB.md`).
- [ ] **N1** native packages from `native` declarations; `sdk/` ported.
- [x] **P1** packages: `asuka add`, `asuka pkg publish`, the resolver,
      `asuka.lock`, the cache; the CLI on npm with prebuilt binaries.
- [x] **D1** the desktop hosts (Go) on the C core: `libasuka` loaded
      with purego, batches decoded by `ops`, events and list calls into
      the ABI; `asuka dev` and `asuka build` for macOS, Windows, Linux.
- [x] **X** the Go core, `mobile/`, `host/web` and Go apps removed; DESIGN.md,
      AGENTS.md and the docs (with `zh` and `ja`) describe the new
      Asuka. Its last step removed the Go app runtime that was left (about
      95,000 lines): `app`, `ui`, `signal`, `uitest`, `colors`, `fonts`,
      the Go SDK (`sdk/`), `host/` with `host/fake`, the Go layout engine
      (`layout`), and `internal/follow`, `crashrun`, `appdir`, `imagehash`
      and the `*spec` packages. What the build needs moved out of the
      runtime's namespaces: the icon parser to `internal/svgparse`, the
      Chrome CSS fixtures and their generator to `core/test/testdata/css`
      (the C test keeps its own known failures), the sqlite plugin's C and
      build tool to `std/sqlite`, the hash decoders' golden data to
      `core/test/testdata/imagehash`, `asuka-app.json`'s schema to
      `internal/appconfig`.
