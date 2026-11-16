# Lowering Asuka

How the compiler turns Asuka (docs/language/, RFC 0007's grammar) into
bytecode (lang/bytecode) that the VM (core/src/vm) runs. Release builds
compile that bytecode to C (lang/cgen): each synchronous function
becomes a C function that calls the same instruction bodies the
interpreter does (core/src/vm/ops.h), and async functions stay on the
VM. So the bytecode is the one contract, and both backends mean the
same thing by it (core/vm_test.go runs every module both ways).

## Pipeline

```
lang/syntax   source -> AST (parser, printer)
lang/check    AST -> typed program: scopes, types, the reactive analysis
lang/ir       typed program -> IR: functions of three-address code over
              typed virtual registers; reactive constructs as runtime
              calls; async marked; retain/release implicit in kinds
lang/bytecode IR -> module (register allocation per kind, jump patching,
              handler tables, line tables)
lang/cgen     bytecode -> C (release)
              typed registers (the VM's opcodes, registers allocated per
              kind as it lowers, jumps to labels); reactive constructs
              as runtime calls; async marked; retain/release implicit in
              kinds. Encode writes the module: labels to offsets,
              handler tables, line tables (it lives in lang/ir because
              the IR's instructions are lang/bytecode's opcodes)
lang/cgen     IR -> C (release)
lang/compile  the driver: a package and its imports -> one module
```

A program is compiled whole: the app's packages, their dependencies and
the prelude (lang/prelude: types.asuka, enums.asuka, elements.asuka, and
the built-in functions and methods) into one module.

## Types to kinds

| Asuka type | Kind | Representation |
|---|---|---|
| `int`, `Duration` (ns), `Color` (ops.Color), `Length` | int | as is; a Length packs its unit (0 auto, 1 points, 2 percent) in bits 32-39 and its float32 value in bits 0-31 |
| payloadless enums | int | the case's index (a wire enum's value) |
| `bool` | bool | 0 or 1 |
| `float` | float | |
| `string` | ref | ak_str |
| `bytes` | ref | ak_bytes |
| `[T]` | ref | ak_arr, element kind T's, shape T's |
| `[K: V]` | ref | ak_map |
| `Set<T>` | ref | ak_map of T's keys to void values, flagged AK_MAP_SET (core/src/rt/set.c) |
| struct, tuple | ref | ak_struct with its shape |
| enum with payloads, or declared `: Error` or having an interface | ref | ak_enum (payloadless cases: shared immortals) |
| `T?`, T a reference | ref | NULL for none |
| `T?`, T a scalar | ref | NULL, or an ak_box |
| `fn(...)` | ref | ak_closure |
| store | ref | an ak_struct of the store's cells, memos and methods' captures |
| `View` | ref | ak_view (the UI runtime's) |
| `Graphics`, `Path`, a catalog | ref | the UI runtime's objects (core/src/ui/canvas.c, i18n.c) |
| `Ref` | ref | a cell holding a `View`, or none |
| `Task<T>` | ref | ak_future |
| `Loading<T>` | ref | the runtime's loader (see "load") |
| interface | ref | a struct or enum value; calls go through CALLI |
| `void` | void | |

Generic code is compiled once per instantiation (monomorphized). A
generic store too: its function, its env's shape and its fns are made
per instantiation (`Box<int>`, `Box<[string]>`), and a let of its type
parameters is a memo, so its env holds references whatever T is.

A type parameter constrained to interfaces (`<T: Shape & Hashable>`)
has their methods and fields: the checker records `x.area()` on a T as
CallIface (and `x.name` as SelIfaceField), and the lowering reads the
receiver's type with the instantiation's types: a struct's or an enum's
method is a CALL of it (an enum without payloads, an int, too), a
struct's field GETF, an interface's value CALLI as ever. `e.message` of
a `T: Error` is the error's message as for the concrete error type.

Shapes are named so that JSON and hot reload find them: a type of the
root package or the prelude by its name (`Todo`), of another package by
its path (`./model.Todo`), an instantiation with its arguments
(`Box<int>`), a tuple by its elements (`(int,string)`), a component's or
a store's env by the component's name.

A map's keys are what the runtime's maps hash: ints (and what's stored
as one: bools, payloadless enums, durations, colors) and strings.

## Registers

Each register has one kind for the whole function. The lowering
allocates them as it goes: a temp lives until the end of the statement
that made it, a local until the end of its block (a reference local is
cleared there, so a value someone changes next is unique, and changed in
place), and a run of consecutive registers of given kinds (a call's
arguments, a struct's fields, a closure's captures) is reused only for
the same kinds. An argument block's references are cleared after the
call for the same reason.

An expression's value may be a local's own register, borrowed: whoever
changes a value copies it first, or changes it in place through its
owner (see "Changing values in place").

## Changing values in place

`x.f = v`, `xs[i] = v`, `m[k] = v` and the builtins that change their
receiver (`push`, `insert`, `removeAt`, `removeWhere`, `pop`, `clear`,
`sort`, `remove`) first compute every index and argument, then reach the
value through its owner:

- a local: SETF/SETIDX/MSET on its register; an inout builtin gets it
  moved (MOV, no reference counted) into the first register of its
  argument block and moved back after, so a value only the local holds
  is changed in place;
- a boxed var: VGET, change, VSET;
- state (a cell: a component's or a store's, or a file's): CTAKE,
  change, CPUT, with a handler that puts it back if the change throws;
- a field or an element of those: read it (GETF, IDX), change it, write
  it back (SETF, SETIDX).

A builtin that may raise an error (`removeAt` out of range, one that
calls a closure) runs in a handler that moves the receiver back first.

## Functions

- A function is a FuncDef; its parameters are its first registers.
  Methods take `self` first. A `static fn` of a struct or an enum is a
  function of the package's (check.Function.Static its type), called as
  any (`Money.of(…)`, `.of(…)` where the type is expected): no `self`. A closure captures a value as it is when
  the closure is made; a `var` it captures that anything changes (the
  closure, or the code around it) is a box (VBOX, VGET, VSET), so they
  share one variable. A `spawn` copies a box's value instead (VGET as
  it's made: values only). A closure stored in a var it captures (one
  that calls itself through it) holds the box that holds it: a cycle
  the checker warns of, pointing at a local fn.
- A local fn (`fn walk(…)` in a block) is a function of its own; the
  fns declared one after another in a block are a group, which shares
  one list of captures (the outer locals its fns use, and those the
  local fns they call capture; check.LocalGroup). Each takes its
  arguments, then the group's captures as the code around holds them
  (a var's box, a value, a row item's accessor), then the env when the
  group uses one. A call of a local fn, from its group or anywhere it's
  in scope, is a CALL passing them on, so recursion and the group's
  calls of each other make no closure and no cycle. Used as a value, a
  local fn is a closure capturing them, whose function CALLs it.
- Default arguments are filled in at the call site (a default may use
  only package-level names; a generic function's is lowered with the
  call's type arguments). An enum case's labeled payload left out takes
  its type's zero (check.HasZero: false, 0, KSTR "", an empty NEWA or
  NEWM, none, a payloadless enum's 0); a function's has none, so it's
  required.
- A closure's function takes exactly the parameters its caller passes:
  an event handler the event's values (those it doesn't name are
  unused registers), a row builder (item, index), a key function the
  item, a thunk or a builder none. It captures what its body uses of
  the function that makes it, in its capture list's order, and loads
  them (CAP) as it starts.
- A struct's or an enum's methods take `self` first; inside them, the
  fields and methods are in scope by their bare names. A struct's
  methods can't change it (it's a value): they return a new one.
- `throws` sets FThrows; `async` sets FAsync. Calling an async function
  is a plain CALL: it runs on the caller's coroutine (a task) and
  suspends it if it waits. AWAIT waits on a future: a `Task<T>`, what an
  async builtin returns (`time.sleep`, `http.get`), `all`, `race`,
  `spawn`.
- `spawn { body }` is CALLB `async.spawn(closure, resultKind)`. The
  checker allows captures of values only (no cells, stores, views,
  closures holding them, tasks), and no reads of module state in what
  the body calls (directly, or through what those call). Its value is
  its `return`'s, or its block's last expression's.
- `all(f(a), g(b))` evaluates the calls' arguments, starts each call as
  a task (TASK of a closure capturing them), and awaits CALLB
  `async.all` of the array of their futures. Its tasks return
  references (a scalar boxed), so the one array of results holds them
  all; the tuple is made of its elements (UNBOX for scalars).
  `race(…)`'s calls have one type, and its tasks return it as it is.
  `all(tasks)` and `race(tasks)` over a `[Task<T>]` pass it through.
- `sleep(d)`, `never()`, `all`, `race` and `idle` are futures awaited at once:
  AWAIT follows the CALLB. `await` of a call of an async function is
  that CALL; of a `Task<T>`, AWAIT. `await` of something that doesn't
  wait does nothing.

## Errors

- `throw e` is THROW (of a copy: THROW moves its register).
- An error type (`struct E: Error`, `enum E: Error`) is stored as an
  enum whose case 0 no value has: an error enum's cases are 1, 2, ...,
  and an error struct is the one case 1 whose payload is its fields
  (read with GETP; its fields can't be assigned). So a caught error's
  ECASE tells the three kinds apart: a user error is not 0; a runtime
  error (`ak_shape_error`: division by zero, an index, JSON, `Error("…")`)
  and the cancellation are structs, 0. Of those, a runtime error equals
  (EQ) `error(error.message(e))`, the error made of its own message; the
  cancellation doesn't.
- Every error shape has two methods CALLI calls: `__etype`, an id of its
  type, and `message` (its `message` field, its `fn message() ->
  string`, or its type's and case's name). `e.message` on an `Error` is
  CALLI `message` for a user error, `error.message` for a runtime one.
- A `do { } catch` is a handler range over the `do` block. Its code,
  placed after the range (so a THROW from it goes to the handlers
  around, not to its own), CATCHes the error, rethrows the cancellation
  at once (no catch sees it: it unwinds to the task's end, through every
  `defer`), then tests the clauses in order: `catch`, `catch e`, `catch
  e: Error` take any error; `catch e: T` compares `__etype`; `catch
  T.case(p)` that and the case and its payload's patterns. None
  matching THROWs it on.
- `try?` is a handler that turns any error but the cancellation into
  none.
- `x catch … { … }` is a `do`'s handler range over x, lowered into the
  expression's register; each clause's block puts its value there (its
  last expression), or leaves. A void one (`try save() catch { … }`)
  is a statement's. Clauses that don't take every error leave THROW
  for the rest, as a `do`'s.
- `throw`, `return`, `break` and `continue` as values (`x ?? throw E`)
  lower as their statements; what they give is never read.
- `let p = v else { … }` tests v as `if let` does (JNONE for a name, the
  pattern's tests otherwise), jumping to the else block, which leaves;
  the names are bound after it. A condition chain (`if let a = x, a >
  0`) is its clauses' tests in order, each jumping to the else.
- `defer { }` protects the rest of its block: its code is emitted at the
  block's end, before each return, break or continue that leaves it,
  and in a handler that runs it and THROWs the error on.
- Handlers are listed innermost first.
- A test's body is in a handler that rethrows a user error as a runtime
  error of its message, so the runner (which reads runtime errors'
  messages) reports it.

## Operators

The int operators are instructions: `+ - * / %` ADDI, SUBI, MULI, DIVI,
MODI (the last two throw on a zero divisor), `& | ^` BAND, BOR, BXOR,
`~x` BNOT, `<< >>` SHL and SHR (ops.h's `ak_op_shift`: a count of 64 or
more shifts every bit out, `>>` keeping the sign; a negative count
throws, as Go panics). A compound assignment (`x <<= 1`, `xs[i] |= m`)
is its operator on the place's value, written back as `=` is. `b[i]` of
bytes is CALLB `bytes.at` (an int; out of range throws, as IDX does).
`>>` is two adjacent `>` tokens the parser joins, so types' `>>` close
two generic argument lists.

## Patterns

A pattern is tested (testPattern: jumps to the arm's end unless the
value matches), then bound (bindPattern: declares its names), so an
`if let`, a match's arm and a `catch T.case(…)` share them:

| Pattern | Tested | Bound |
|---|---|---|
| `x`, `_` | nothing | `x` is the value |
| `0`, `"a"`, `true` | EQ with the literal (of an optional: JNONE first, then its value) | |
| `none` | JSOME fails | |
| `(a, b)` | each element (GETF) | each element's names |
| `.case(p, q)`, `T.case(label: p)` | ECASE (an int enum: the value), each payload (GETP) | each payload's names |
| `P(x: 0, y)` (a struct's, check.Info.FieldPats: each argument's field) | each named field's (GETF) | each named field's names |
| `a \| b` | each in turn | (none) |

In a view's match, each name an arm binds is a memo reading its part of
the subject (extract), so the arm follows it.

The checker says whether a match covers every value as Maranget's
"Warnings for pattern matching" does (lang/check/patterns.go): the arms
without guards as rows of normalized patterns (an enum's cases, a
struct's or a tuple's one constructor, an optional's none and some, a
bool's true and false; literals of other types never cover them), and
what's missing back as patterns for the message.

## Strings and values as text

`"a {x} b"` is INTERP over a block of its parts: the text between
interpolations (KSTR), and each value: ints, floats and bools as they
are (INTERP prints them), strings as they are, and the rest as text: a
payloadless enum by its case's name (an array of the names, frozen in a
global), a `Duration` in milliseconds (`300ms`), an optional as `none`
or its value, an error as its message, anything else as its JSON
(`json.encode` with its descriptor). `print(x)` prints the same text.

A multi-line string's text drops its first line when that's blank, and
the indentation of its closing quotes' line from every line.

## Module state and constants

- `state` at the top of a file is a module global holding a cell, made
  in the module's init function (CELLK keyed `package.name`).
- A top-level `let` that reads state is a memo global, made in init; one
  that doesn't is a constant.
- Constants of reference kind (string literals aside, which are already
  in the string table): a struct literal with constant fields, an array
  of constants, a struct's `let` (Font.title), are built once in init
  into a global and frozen (`rt.freeze`, which makes them and what they
  hold immortal, so spawn's threads may share them). Today the lowering
  does it for top-level lets and types' lets; literals in functions are
  built where they are (hoisting them is an optimization still to do).
- Init makes each package's globals, the packages in dependency order
  and each package's globals by what their initializers read (a cycle
  is the checker's error). It first calls `init.strings`, which makes
  the frozen tables of names the lowering asked for (enums' case names).

## Reactivity

Reads, by what's read:

| Read of | In a reactive context (a binding, a memo, an effect) | In a handler or function |
|---|---|---|
| a cell (`state`) | CGET: tracked | CGET (the runtime runs handlers untracked) |
| a memo (`let` in a component, a store, a file) | MREAD | MREAD |
| a parameter or prop of a component | AGET of its accessor | AGET |
| a `for` row's item, a `match` arm's payload | AGET | AGET |

Writes: `x = e` is CSET. A write into part of a cell's value
(`todos[i].done = true`, `form.name = n`, `todos.push(t)`) takes the
value out (CTAKE), changes it in place (SETF, SETIDX, an inout builtin
on the register), and puts it back (CPUT, which always notifies). The
index and the new value are computed before the CTAKE.

A **binding** is any expression the UI follows: a prop's value, an
`if`'s condition, a `for`'s items, a `match`'s subject, a `let`. The
compiler makes it a closure of no arguments (a thunk) and passes it to
the runtime, which runs it as a render effect, or as a memo for `let`.
An expression that reads nothing reactive (a literal, a constant, a
local of the enclosing function, arithmetic on those) is evaluated once
and passed as a value instead (`ak_ui_set` rather than `ak_ui_bind`).

## Components

A component compiles to a function returning a View. Its arguments
(all references) are its header parameters, then its props, then its
events' handlers, in declaration order:

- A parameter or prop is passed as an **accessor**: ACCV of the value
  when the caller's expression is constant, ACCC of a cell or memo when
  it's one read alone, ACCF of a thunk otherwise. A prop the caller
  leaves out is none, and the callee's default applies (evaluated in the
  callee, as an accessor too).
- A prop of type `View` (a slot, `children`) is passed as a builder: a
  closure that builds the view when called, so it's built in the
  callee's owner, where the callee places it, only when it's shown.
- An event's handler is a closure, or none (emit then does nothing).

Its members live in its **env**, a struct of the component's shape:
a field per parameter, prop and event (their accessors, builders and
handlers), per state (its cell), per let (its memo, its loader, or its
value when it reads nothing reactive) and per use. The closures the
component makes reach its members through the env: those made before
the env is whole (its lets' memos) capture a box (VBOX) the env is put
in (VSET) once it's made, the rest the env itself. Its `fn` members are
functions taking the env first. A store is the same, and its value is
its env. In a store's fns,
`self` is the env.

The function's body, in order:

1. `state x = e`: CELLK with the key `Component.x` (the runtime adds the
   instance's path: its place in the tree, the row's key).
2. `let y = e`: MEMO of a thunk (or a constant when it reads nothing).
3. `use x: T`: CALLB `ui.use(key)`, the key being T's type name.
   `provide e`: CALLB `ui.provide(key, value)`, on the component's owner,
   before the views are built.
4. `fn` members: closures over the component's cells and memos.
5. `effect { }`: EFFECT of a closure. `task { }`: CALLB `ui.task`, which
   starts the closure as a task when the component mounts.
   `task(a, b) { }` is an effect that reads `a` and `b` and starts the
   task in its own owner, so a re-run cancels the run before.
6. The view items: one element is the component's view; several make a
   fragment (CALLB `ui.fragment`, then `ui.add` each).

`screen` is a component whose root is the `Screen` element: the
screen's prop sets and slots go to that element, its view items are the
element's children.

`emit e(args)` calls the handler argument with args when it isn't none
(CALLC; its result register is a reference, so the future of an async
handler started from C is released). An event's name read as a value is
whether the caller handles it: ISNONE of the handler, NOT.

A store's constructor is the same function without the views: it
returns its env. Its `task { }` is TASK (a store has no mount), owned by
the owner that made the store, so disposing that owner cancels it.

## Elements

A built-in element (lang/prelude/elements.go) lowers to the UI runtime's
protocol (core/src/ui/ui.h), as builtins. Their names and arguments are
listed in one place, lang/ir/uimap.go; the numbers (AK_EL_, AK_KEY_,
AK_EV_) are lang/prelude's ElementID, KeyID and EventID, which match
core/src/ui/elements_gen.h:

| Source | Lowered |
|---|---|
| `Button("Add") { … }` | `v = ui.new(AK_EL_BUTTON)`; `ui.set(v, AK_KEY_BUTTON_TITLE, "Add")` |
| `color: c` (constant) | `ui.set(v, AK_KEY_COLOR, c)` |
| `color: active ? .accent : .label` | `ui.bind(v, AK_KEY_COLOR, thunk)` |
| `on press { … }` | `ui.on(v, AK_EV_BUTTON_PRESS, closure)` (async when it awaits) |
| `bind text: q` | `ui.bind(v, AK_KEY_TEXT_FIELD_TEXT, thunk{CGET q})` and `ui.on(v, AK_EV_TEXT_FIELD_CHANGE, closure(t){CSET q t})` |
| `style: [card, chip(.red), if on { pink }]` | see "Styles" |
| children | each built, then `ui.add(v, child)` |
| children of a `LazyChildren` element (`Tab`, `Sheet`, `Destination`, `Window`, `Boundary`, `MenuBar`) | `ui.lazy(v, builder)`, last: one builder of them all (its own FuncCtx, the checker's `CtxBuilder` at the first child), which the runtime builds when they show, under an owner of its own (a `Route`, whose builder would take its `Params`, still adds its children in place) |
| the app block's first `Window` | `ui.main_window(v)` right after its `ui.new`, before its keys: the main window, its keys the root's options, its children the root's content |
| `toolbar { … }` | `ui.slot(v, AK_KEY_SCREEN_TOOLBAR, fragment)` |
| a `Lazy` slot (`Popover`'s `content { … }`) | `ui.slot(v, key, builder)`: a closure returning the View, built as it shows |
| `on error(e, reset) { views }` (an event that `Builds`) | `ui.on(v, ev, closure)`, the closure returning the View it builds |
| `Text(x) as t` | `t` is a local Ref of the enclosing view (see "Refs") |
| `ref: r` | the view put in the Ref r (see "Refs") |

A `Screen`'s `actions`, `toolbar` and `primary` slots hold Buttons:
the runtime sends them as the platform's bar items (title or icon, a
`menu`, `disabled`), and runs a Button's `press` (and its `href`) when
its item is chosen; a Button that changes, or one a `for` or an `if` in
the slot adds, sends the bar again.

Values cross as their kinds (rt.h). Conversions the checker inserts:
a number where an `Edges` is expected is `Edges(all)`; `[v, h]` and `[t,
r, b, l]` are Edges; a number where a `Length` is expected is points;
`.label` where a `Color` is expected is the semantic color.

A component that shows one element (check's `TakesStyle`: its body's
views are one call, of an element that takes the style, a package's
native component, or a component that takes it) takes the style props
from its caller. They're set on the view the component's function
returns, after it returns: `ui.set_outer(v, key, x)`, or
`ui.bind_outer(v, key, thunk)` when x reads what changes. The view keeps
a bit per style key its caller set; the component's own sets of that key
(its bindings running again) don't apply after.

## Styles

A style's body is checked once, where it's declared (check/styles.go),
in a context of its own (`CtxStyle`) whose locals are its parameters and
its `use`s (`StyleLocal`): its props' values are bindings of that
context, capturing them. An element's `style:` list (and the package's
default style for its kind, first) is resolved where the element is
written (`ElementCall.Style`) and lowered where it's built (ir/style.go):

1. **Applications.** Each style applied (`card`, `chip(tint)`) is an
   application: each parameter's argument (or its default) is evaluated
   once into a local, a value when it reads nothing reactive, else a
   memo (MEMO of its thunk) the props read with MREAD; each `use` is a
   `ui.use(key)`. The application's locals are the registers the style's
   values capture.
2. **Flattening.** The list becomes the props it sets, in order, each
   with its application and the conditions it's under: an entry inside
   an `if`'s branch or a `match`'s arm is under "this branch is taken";
   a style's variant block (on an element that takes variants) is under
   its state.
3. **Props under no condition** are set as the block's: the last of a
   key, `ui.set` or `ui.bind` (`ui.set_outer`/`ui.bind_outer` for a
   component's caller's), and none of a key the block sets.
4. **Layers.** Each run of props under the same conditions is a layer:
   `l = ui.layer(v, tier, state)`, `ui.layer_set(v, l, key, x)` or
   `ui.layer_bind(v, l, key, thunk)`, and `ui.layer_when(v, l, cond)`,
   cond a closure that's true while every condition's branch is taken
   (an `if`'s earlier conditions false and its own true; a `match`'s
   earlier arms not matching and its own matching). A layer leaves out a
   key the block sets, or a later prop under no condition sets.

| Source | Lowered |
|---|---|
| `style: [card, pink]` | card's and pink's sets, a later one of a key replacing an earlier: `ui.set(v, AK_KEY_BACKGROUND, pink's)` |
| `style: chip(tint)` (tint state) | `m = MEMO(thunk{tint})`; `ui.bind(v, AK_KEY_BACKGROUND, thunk{MREAD m})` |
| `style: [card, if on { pink }]` | card's sets; `l = ui.layer(v, AK_LAYER_OWN, 0)`, `ui.layer_when(v, l, closure{on})`, `ui.layer_set(v, l, AK_KEY_BACKGROUND, …)` |
| `style: match k { 1 => a, _ => b }` | a layer per arm, its condition "k matches this arm and no earlier one" |
| `style: button` (holding `pressed { opacity: 0.7 }`) on a View | `l = ui.layer(v, AK_LAYER_STATE, AK_STATE_PRESSED)`, `ui.layer_set(v, l, AK_KEY_OPACITY, 0.7)` |
| `pressed { style: raised; opacity: 0.6 }` | raised's sets as `ui.variant` (the block's variant of the state), under the block's |
| a component's caller's `style:` | as above, on the view the component returns: `ui.set_outer`, layers of tier `AK_LAYER_OUTER` |

The runtime (core/src/ui/interaction.c) keeps a view's layers and lays,
for each key a layer sets, the last of the layers that are on over the
view's own value: by tier (own, then the caller's, then the states'),
then a state layer by its state, then as they were made. A key a
component's caller set takes nothing from the component's own layers.
A layer going off puts each key back; turning one on or off allocates
nothing. The variants are layers too: `ui.variant` sets the block's
layer of a state, made the first time, after the style's.

## Control flow in views

| Source | Lowered |
|---|---|
| `if c { A } else { B }` | `ui.show(thunk{c}, builder{A}, builder{B})` |
| `if let u = user { A }` | `ui.show(thunk{user != none}, builder{ let u = memo(user!); A })` |
| `if let a = x, let b = f(a), b > 0 { A }`, `if let .ready(n) = s { A }` | `let m = memo{ the chain's tests; (a, b) or none }; ui.show(thunk{m != none}, builder{ let a = memo(m!.0); let b = memo(m!.1); A })`; a chain without names: `ui.show(thunk{its tests}, builder{A})` |
| `for x in xs key k { R }` | `ui.for(thunk{xs}, closure(x){k}, closure(xAcc, iAcc){R})`; `x` in R is AGET xAcc |
| the same, directly in a `List` | `ui.lazy_for(list, …)` with the same arguments |
| `match s { .a => A; .b(p) => B }` | `ui.switch(thunk{the index of the arm s matches, -1 for none}, closure(arm){ switch on it: A, or { let p = memo(payload of s); B } })` |

Each builder and row runs under its own owner, which the runtime
disposes when the branch or row goes.

The selector of a match returns the arm (not the case), so literal,
tuple, struct and `|` patterns work as in statements; a match in a view takes
no guards. A `for` without `key` passes none, and the runtime keys rows
by their index. Several view items (a component's body, a branch, a
row) make a fragment (`ui.fragment`, `ui.add` each); a `View?` (an
optional slot) is added only when it isn't none.
## Interaction states and variants

A container's (`View`, `Row`, `Column`, `SafeArea`: the elements whose
`Variants` is set in lang/prelude/elements.go) block may hold variants:
a block named after an interaction state (prelude.Variants: `hovered`,
`pressed`, `dragging`, `dragOver`), holding style prop sets only. The
parser reads `pressed { … }` as a named slot; the checker knows it's a
variant from the element. Each prop set in it lowers as a prop set does,
with the state:

| Source | Lowered |
|---|---|
| `pressed { opacity: 0.6 }` | `ui.variant(v, AK_STATE_PRESSED, AK_KEY_OPACITY, 0.6)` |
| `hovered { color: c }` (c reactive) | `ui.variant_bind(v, AK_STATE_HOVERED, AK_KEY_COLOR, thunk)` |
| `style: card` inside a variant | the style's sets, into the variant, under its own |
| `View as card { … }` then `card.hovered` | `card` is a Ref (see "Refs"); the read is CGET of it, then `ui.state(view, AK_STATE_HOVERED)`, a tracked bool (false while it holds none) |
| `on pressedChange(p) { … }` | `ui.on(v, AK_EV_VIEW_PRESSED_CHANGE, closure)`, as any event |

The checker allows style keys only (not the element's own, dragItems or
dropTypes, nor keys and actions, which don't change with a state), and
variants only in an element whose `Variants` is set: anywhere else a
variant's name is a slot that doesn't exist, and says so.

The runtime lays the variants of the states the view is in over its own
values, in the states' order (a later one wins), whole values (a `font`
in a variant replaces the font): they're layers of the state tier (see
"Styles"). A view reports a state from the first
variant, read or handler that asks for it, and leaves each state it's in
when its owner is disposed. `ref` names any element, so `card.hovered`
works on any view the host reports states for.

## Refs

A `Ref` is a view's handle: a cell (CELL of none) the view goes in.
`Ref()` makes one; `ref: r` on an element (a pseudo-prop the checker
reads, on elements and native components only) puts the element's view
in r right after `ui.new`: CSET, and a CLEANUP of the building owner
that sets it back to none when it still holds that view. `Element as
name` is the same with a Ref the lowering makes (CELL, before the
element), a local of the enclosing view function: the element's own
bindings, its children and its later siblings see it. Reading the view
is CGET, so a binding that reads a state through a Ref follows both the
view coming and going and the state. A Ref's members are built-in
methods (methods.go, receiver ref) on the view it holds: none, a
command does nothing and a property is its zero (JNONE around the
CALLB; a tuple's or a struct's zero is one of its fields' zeros, NEWS); an async one (`eval`) passes none on, and the runtime fails its
future. A top-level `let` of a Ref (or a Path) isn't frozen: it changes.

## Graphics

`Canvas(draw)` takes a closure `fn(Graphics)`; `Shape(path)` one of
`fn(float, float) -> Path` (the width and height it's laid out at).
Both run in a render effect once the element knows its size (after the
frames, as a layout handler does), and again when it changes or what
they read changes; only a drawing that differs from the last is sent.

`Graphics` and `Path` are built-in reference types (runtime objects, not
values): a Graphics is valid only while its draw runs; a Path is a
builder whose methods add to it and return it, so they chain. Their
methods are builtins taking the object first:

| Source | Lowered |
|---|---|
| `g.width`, `g.height` | `canvas.width(g)`, `canvas.height(g)` (floats) |
| `g.path()` | `canvas.path(g)`: one of the canvas's paths, emptied, reused from draw to draw (drawing again allocates nothing) |
| `Path()` | `path.new()` |
| `p.move(x, y)`, `.line`, `.quad(cx, cy, x, y)`, `.cubic(...)`, `.rect`, `.roundedRect(x, y, w, h, r)`, `.ellipse`, `.arc(cx, cy, r, start, end, clockwise: true)`, `.close()` | `path.move(p, x, y)`, ..., `path.rounded_rect`, `path.arc` (radians, clockwise from +x), `path.close(p)`; each returns p |
| `g.fill(p, color)`, `g.stroke(p, color, StrokeStyle(...))` | `canvas.fill(g, p, c)`, `canvas.stroke(g, p, c, style?)` |
| `g.fill(p, LinearGradient(...))`, `RadialGradient` | `canvas.fill_gradient(g, p, gr)`, `canvas.fill_radial(g, p, gr)` (the checker picks the overload by the argument's type, an implicit member being a color) |
| `g.stroke(p, gradient, style)` | `canvas.stroke_gradient(g, p, style?, gr)` / `canvas.stroke_radial` (the paint comes second, as fill's; the builtin's order is the method's Order) |
| `g.save()`, `.restore()`, `.translate(x, y)`, `.scale(x, y)`, `.rotate(deg)`, `.opacity(a)`, `.clip(p)`, `.transform(a, b, c, d, tx, ty)`, `.evenOdd(on)` | `canvas.save(g)`, ..., `canvas.even_odd(g, on)` |
| `g.text(s, x, y, CanvasText(...))` | `canvas.text(g, s, x, y, style?)` |

`Shader(program)` takes the compiled program as `bytes`: the checker
compiles `shader("""…""")`, a Kage program written as a literal without
interpolations (a raw `r"""…"""` keeps Go's braces), with
internal/shadercode's Compile and Encode (which carry the uniforms'
lane table), reporting its errors; the lowering makes the bytes of a
string constant (KSTR, CALLB `str.to_bytes`). `uniforms: [Uniform(name:,
value:)]` set its variables by name; `colorUniform(name, color)` is
`canvas.color_uniform`.

## Animation and frames

| Source | Lowered |
|---|---|
| `animate { body }` | `ui.animate(none, closure{body})` |
| `animate(.spring) { body }` | `ui.animate(anim, closure{body})`, anim an `Animation` |
| `animate(() => …)`, `animate(anim, () => …)` | the same, the closure as written |
| `await animate(…) { body }` | `ui.animate_wait(anim, closure{body})`, AWAIT on the future it returns: settled once the animation is complete (its delay and runs; at once when nothing is mounted, or it repeats forever) |
| `animating()` | `ui.animating()` (whether the changes made now animate) |
| `let stop = onFrame(t => …)` | `ui.on_frame(closure(t: Duration))`, which returns the stop closure; the frames stop with the current owner too |
| `frames()` | `ui.frames()`: a `Stream<Duration>` whose producer is an `ui.on_frame` that sends each frame's time, waiting until the stream is closed (the loop ends, the task goes), which stops it |

`body` runs synchronously, its writes batched; the props and frames the
next flush changes are sent between AnimateBegin and AnimateEnd.
`Animation.done` is called once the animation is complete. animate is
no async function: a handler or a plain fn calls it without waiting,
and `await` (where waiting is allowed) makes the checker mark the call
(Call.Awaited). At the start of a statement, `animate { … }` parses as a
slot named animate: the checker reads a slot named after a function, in
a statement, as that call (Info.SlotCalls). onFrame's closure doesn't
make a `let` of it a memo: what it reads is the closure's.

## Keys, drags, drops, accessibility

`keys: [Shortcut(key: "down", run: () => …)]` and `actions:
[A11yAction(name: "Delete", run: …)]` are style props: any element
takes them (a flattened container gets a view). `useKeys([...])` in a
component is `ui.use_keys(keys)`: the root's keys while the component
lives, the last one's winning. A container's `dragItems: [DragItem]`
(often a binding) makes it draggable and `dropTypes: [string]` a drop
target; their events are `dragPhase`, `dropOver`, `dropMove` and `drop`.
`on adjust(by)` makes a view adjustable for screen readers.

## Media and the system

Commands on a view are a Ref's methods, builtins taking the view it
holds (see "Refs"):

| Source | Lowered |
|---|---|
| `video.play()`, `.pause()`, `.seek(s)` | `ui.play(v)`, `ui.pause(v)`, `ui.seek(v, s)` |
| `web.run(js)`, `.goBack()`, `.goForward()`, `.reload()`, `.stopLoading()` | `ui.web_eval(v, js)`, `ui.web_navigate(v, 0..3)` (Method.Consts) |
| `try await web.eval(js)`, `try await webCookies(url)` | `ui.web_eval_result(v, js)`, `ui.web_cookies(url)`: AWAIT on the future |
| `field.insert(s)`, `.format(.bold, on)`, `.toggleFormat(.bold)`, `.setLink(url)` | `ui.insert_text`, `ui.format(v, f, on)`, `ui.toggle_format(v, f)`, `ui.set_link` |
| `field.focus()`, `.blur()`, `scroll.scrollTo(x, y, animated: true)` | `ui.focus`, `ui.blur`, `ui.scroll_to` |
| `share(text)`, `try await shareFiles(paths, mime:, uti:, title:, from: ref)`, `haptic(.success)`, `copy(text)`, `openURL(url)`, `announce(text)`, `a11yFocus(ref)` | `ui.share`, `ui.share_files` (from: the Ref's view, or none), `ui.haptic`, `ui.copy`, `ui.open_url`, `ui.announce`, `ui.a11y_focus` (not called while the Ref holds none) |
| `useKeys([…])`, `onOpenURL(u => …)`, `onOpenFiles(ps => …)`, `onShare(items => …)` | `ui.use_keys`, `ui.on_open_url`, `ui.on_open_files`, `ui.on_share`: items of a component's body (or the app's block), called in its function before its views, so they hold while it lives |
| the app's `on openURL(url) { }`, `on files(paths) { }`, `on share(items) { }`, `on quit(e) { }`, `on secondInstance(args, dir) { }` | `ui.on_open_url`, `ui.on_open_files`, `ui.on_share`, `ui.on_quit`, `ui.on_second_instance` of the handler, in the app's function (check.AppEvents) |

These functions are a table (lang/check/runtime.go's RuntimeFuncs), as
the built-in methods are: a name, a signature over the prelude's types,
the builtin, whether it's async or throws; a Ref argument crosses as its
view.

A host module's method (one Asuka doesn't wrap) is `ui.call(module,
method, args)`, a future of the answer's bytes.

## Native components

A `native component X(…) { prop …; event … }` of a native package
(docs/native.md) lowers as a component whose body is one native view:

| Source | Lowered |
|---|---|
| making it | `v = ui.native(type)`, type `"<module ID>#X"`, the module ID the platforms' generated code registers (as `native.new`'s cls: check.Package.NativeClass) |
| a header parameter or a prop, constant | `ui.native_set(v, "Name", value, desc)`, desc its type's descriptor (native.h's) |
| a header parameter or a prop, reactive | `ui.native_bind(v, "Name", thunk, desc)` |
| `on name(a, b) { … }` | `ui.native_on(v, "Name", closure, desc)`, desc its parameters' descriptors, in order |
| the style | `ui.set` / `ui.bind` of style keys on v |

Names cross exported (`selected` is "Selected", `changed` "Changed"):
internal/nativegen's names, which the platforms' generated code decodes
props and sends events by. A prop the caller leaves out is sent its
declared default, when it has one. The handlers are set before the
props (a view may send an event as a prop is set); the runtime sends the
props after the listening, then each again only when its encoding
changes. Props cross as bytes (native.go's type 5) in the native codec;
an event's values are its one value "v", decoded parameter by parameter.
Each host finds the kind's view by the package's path: iOS and macOS
(AsukaNative.makeView) and Android (AsukaModules.makeView) through the
provider of the path, the web through registerModule's, Windows and
Linux through native.ViewFactory, under which the package's generated
desktop Go (implement<X>) registers it.

## Languages

The compiler (lang/compile) finds the app's catalogs (asuka.toml's
`[i18n]`, else `locales/`), checks them with internal/i18ngen (what's
wrong is a diagnostic at its file and line; a translation's missing
messages are warnings), and compiles them with package i18n
(`Catalog.CompileWith`): the blob, the keys sorted, each key's arguments
in its source message's order, each language's number formats and, when
a message has a date or the app imports std/i18n, its dates (CLDR's
names, and the patterns of every style pair and of the messages'
skeletons, in 12 and 24 hours). The checker gets `tr` (check.Catalog):
each source key, its arguments typed by its placeholders (`int`, a
number for `double` and `num`, a date for `DateTime`, a string
otherwise). The blob is a string constant; init makes the catalog global
before anything else, `i18n.catalog(blob.bytes())`, which registers it
(a reload's replaces the last): its languages become the app's. An app
without catalogs that imports std/i18n gets an implicit one, English's
formats and no messages, and no `tr`.

| Source | Lowered |
|---|---|
| `tr.inboxTitle(unread: n)` | `i18n.say(catalog, key, (n,), none)`, key the index of `inboxTitle`; the tuple of the arguments in the key's order, of their own kinds: an int or a float for number, plural and selectordinal arguments, an int (nanoseconds since 1970; a `Time`'s instant) for a date, a string otherwise; none without arguments |
| `i18n.locale()`, `i18n.setLocale(tag)`, `i18n.languages()`, `i18n.isRTL()`, ... (std/i18n) | the package's `native fn`s: `i18n.locale`, `i18n.set_locale`, `i18n.languages`, `i18n.is_rtl`, `i18n.region`, `i18n.format_locale`, `i18n.format_number`, `i18n.format_int`, `i18n.format_date`, `i18n.autonym` |

`i18n.say` reads the language shown, tracked: a binding that says a
message follows the language as it follows its arguments, and the
checker counts a call of tr (and std/i18n's natives that read the
locale) as reading state, so a function that says one is followed too
and spawn can't call it. A key missing in a language falls back as
`Catalog.Format` does (its CLDR parent locales, then the source); a key
none has says its name. Numbers and dates are written in the language
shown's formats (the catalog's best match for it), dates in the device's
time zone and the user's hour cycle.

## load

`let user = load api.user(id)` is CALLB `ui.load(thunk)`, where the thunk
reads the arguments (tracked) and returns the future of the call (the
async function started as a task). The runtime re-runs the thunk when
what it read changes, cancelling the last task, and exposes a
`Loading<T>` whose `.pending`, `.ready(v)`, `.failed(e)`, `.value` and
`.reload()` the prelude declares as builtins (`load.state`, `load.value`,
`load.reload`); its state is a cell, so reading it is tracked.

## Navigation and commands

`push(X(…))`, `present(X(…))`: CALLB `ui.push(builder)`, `ui.present(builder)`.
`pop()`, `dismiss()`, `field.focus()`, `scroll.scrollTo(…)`: builtins
taking the ref. The rest (core/src/ui/builtins.c has their arguments):

| Source | Lowered |
|---|---|
| `onBack { … return true }` (a component's statement) | `ui.on_back(closure)`: the closure returns whether it took the user's going back from the nearest pushed screen, sheet or popover (else the app's); it goes with the component |
| `depth()` | `ui.depth()`: the nearest stack's screens |
| `alert("Delete?", message: m, actions: [.button(…)], fields: […])` | `ui.alert(title, message, actions, actionSheet, fields)`; `actions` a closure when they read state (a button disabled while a field is empty), followed while it shows |
| `snackbar("Deleted", action: .button("Undo") { … })` | `ui.snackbar(message, action, duration, onDismiss)`, returning the closure that dismisses it |
| `w.show()`, `hide()`, `minimize()`, `maximize()`, `restore()`, `center()`, `close()` on `Window(…) as w` (a `Ref`; another view's: the window it's in) | `ui.window_do(w, action)`, the `WindowAction` a Const; `close()` is the user's closing (it asks) |
| `w.setSize(width, height)`, `w.setPosition(x, y)` | `ui.window_set(w, x, y, action)` |
| `w.setFullScreen(on)`, `setAlwaysOnTop(on)`, `setClickThrough(on)` | `ui.window_toggle(w, on, onAction, offAction)` |
| `w.focus()` | `ui.focus(w)`, which focuses a `Window`'s window |
| `w.visible`, `focused`, `minimized`, `maximized`, `fullScreen`, `alwaysOnTop`, `clickThrough` | `ui.window_flag(w, bit)` (the `WindowState` bit a Const), followed: the first read makes the core listen for `WindowState` |
| `w.position`, `w.size`, `w.windowState` | `ui.window_pair(w, 0 or 1)` (a `(float, float)`), `ui.window_state(w)` (a `WindowState`), followed |
| `closeWindow()` | `ui.window_close()`: the window the calling code is in, without asking |
| `onCloseRequest(fn() -> bool)` (a body's item) | `ui.on_close_request(closure)` |
| `quit()`, `relaunch()` | `ui.quit()`, `ui.relaunch()`: the `Quit` command to the root |
| `onQuit(e => …)`, `onSecondInstance((args, dir) => …)` (a body's items), the app block's `on quit(e) { }`, `on secondInstance(args, dir) { }` | `ui.on_quit(closure)`, `ui.on_second_instance(closure)`: handlers that go with the current owner; `asuka_quit_request` runs the first with a `QuitEvent` (its `cancel` and `canceled` closures the runtime's), the `SecondInstance` event the second |
| `navigate(href)`, `navigate(X(…))`, `replace(href)` | `ui.navigate(href, mode)` (0 navigate, 1 push, 2 replace); `X(…)` a routed screen (below): its href |
| `router.back()`, `router.dismiss()`, `router.canGoBack()`, `router.href()`, `router.setQuery(q)` (`router` a package of the universe: check.RouterFuncs) | `ui.router_back`, `ui.router_dismiss`, `ui.can_go_back` and `ui.router_href` (both followed: they read the router's address signal, written as its history changes), `ui.set_query` |
| `runAction(.button(…))` | `ui.run_action(action)`: the action's run (its `run` an `async fn()?`), unless it's disabled, under the calling code's owner; an async one's future is left to run, as a handler's is. Actions the runtime runs (menus, alerts, swipes, snackbars) run under the owner of the view or the code that made them (`ak_ui_call_under`) |
| `mainWindow()` | `ui.main_window_ref()`: a Ref of the root's view, which window commands take as the main window's (`focus()` on it brings the window forward) |

## Routes

A `Router` maps addresses to screens, as the Go ui's did: every screen
of it has an href (`/inbox/18`, `/search?q=go`), which the web shows
in its address bar, a link opens, and a reload comes back to. Routes
are elements, so they're declared where the app's structure is:

```asuka
app {
  Router(initial: "/inbox") {
    Route("/", redirect: "/inbox")
    Route(layout: .tabs) {
      Route("/inbox", layout: .stack, title: "Inbox", icon: "tray") {
        Inbox                                    // its screen's own path: ""
        Thread                                   // ":id"
        Route(":id/reply", sheet: true) { p => Reply(id: p.get("id")) }
      }
      Route("/settings", layout: .stack, title: "Settings", icon: "gear") {
        Settings
      }
    }
    Route("/admin", layout: .stack, guard: { to in signedIn ? "" : "/login?next={to}" }) { Users }
  }
}

screen Inbox at "" { … }
screen Thread(id: int) at ":id" { … }    // id: from the path, parsed
```

- `Router(initial:) { … }` is `ui.new(AK_EL_ROUTER)`, its keys, then
  `ui.add` of each `Route`; a `Route` with routes in it likewise. A
  Router's and a Route's keys are header parameters (`Route("/inbox",
  layout: .stack, title: "Inbox")`): a route table reads as one. A
  route's layout makes it a navigator of its children (`.stack`,
  `.tabs`, `.split`, `.slot`), `.group` (the default) only gathers them
  under its path and guard.
- `Route(path) { p => View }` (`_ => View` when it reads none) is
  `ui.lazy(route, closure(pAcc){…})`, last: the runtime builds it for
  each screen of the route, with its `Params` (an accessor, as a for's
  row item: a kept screen follows its query, `setQuery`).
- A screen declared with `at "pattern"` is routable: named in a
  `Route`'s block, it's a `Route` of its pattern (relative to the
  parent's, unless it starts with `/`) whose builder reads each of the
  screen's parameters from the path's parameters, else the query's,
  converted to its type (`int`, `float`, `bool`, an enum's case name,
  `string`); a value that doesn't convert, or a parameter without a
  default the address doesn't say, throws `RouteError` to the nearest
  `Boundary` (lang/ir/routes.go, over types.asuka's `routeParam`,
  `routeInt`, ...). Its title is the screen's own.
- `navigate(Thread(id: 7))`, a Button's `href: Thread(id: 7)`: the
  compiler knows Thread's full pattern from the one Router that routes it
  (`/inbox/:id`) and lowers the href to `ui.href_of("/inbox/:id",
  ["7"])`, the screen's other parameters the call gives its query
  (`?sort=oldest`, `url.escape`d); a screen two Routers route, or none,
  is an error there (check's routeHrefs, once every Router is checked).
- Guards are closures `fn(string) -> string` (the href asked for; `""`
  lets the user in, an href sends them there), asked as the router
  navigates. Redirects fill the route's parameters into theirs.
- The router is the nearest one around the code (a context the router
  gives what it builds), else the main window's first.

The runtime (core/src/ui/router.c) is the Go ui's router: the navigation
state described, then reconciled; Location commands with how the
history moved; a browser's back and forward (the Location event); links
the app is opened with.

## Tests

A `test` block is a function in Module.Tests (async when it awaits).
`expect(cond)` and `expect(cond, message)` throw a runtime error when
cond is false, saying what failed and where (`expected count == 2
(app.asuka:12): got 1 and 2`, a comparison's sides shown). `mount(view)` is
CALLB `test.mount(builder)`, which mounts the view on the fake host and
returns an app handle. `app.tap(text)`, `app.type(text, into:)`,
`app.expect(texts...)`, `app.expectNo(text)`, `app.idle()` are builtins
on it (`test.tap`, ...); a failed expectation throws, and the runner
reports the error with its line.

## Programs

The root package's `fn main` (check.Program.Main) is lowered as any
function and recorded as Module.Main, which the module's encoding
carries after App (-1: none). lang/cgen emits, for a module with one, a
weak C `main()` that calls `asuka_program_main` (on Windows with no
argv: the core reads the UTF-16 command line), and, for an app's
program (Module.Data, not encoded), a constructor that names the app's
data folder (`ak_program_data`). A compiled function notes the line an
error leaves it from (`at_`, set on the error's branch only) and calls
`ak_frame_hook` as it unwinds: the trace a program reports. `-> never`
is Check's Never; a call of a function returning it ends a block.

## The desktop

`MenuBar`, `Menu`, `MenuItem`, `MenuSeparator`, `WindowCloseGuard` and
`WindowKeys` are elements, lowered as any (`ui.new`, `ui.set`/`ui.bind`
of their keys, `ui.on`), none of them a view: core/src/ui/menubar.c
builds a `MenuBar`'s children (`ui.lazy`) where a `Menu` made under it
(`ui.new` finds the bar's context) is one of its menus, and encodes them
into the `MenuBar` command at the flush when one of them changed;
window.c registers a `WindowCloseGuard` (its `when`, its `closeRequest`)
and a `WindowKeys`' keys with the window they're built in.
`platform/tray` is plain Asuka over the `tray` host module (sdk/tray).

In tests, `mountApp()` is CALLB `test.mount_app`, which mounts the
module's app function on the fake host (the runner gives it with
`ak_uitest_set_app`), and the desktop's test methods (`app.windows()`,
`closeWindow(title)`, `windowActions(title)`, `setWindowState`,
`focusWindow`, `pressKey`, `menuBar()`, `chooseMenuItem(path)`,
`trays()`, `clickTray(id)`, `chooseTrayItem(id, path)`, `dockMenu()`,
`requestQuit(sessionEnding:)`, `quits()`, `secondInstance(args, dir)`)
are `test.*` builtins over core/host/fake/desktop.c.

## Hot reload

State cells are made with CELLK: the runtime looks the key up in what
the last run kept. On a reload, the runtime keeps every keyed cell's
value (converted to the new module's types by field and case names; a
value whose type changed in a way that can't be converted starts
again), disposes the old app, frees the old module once nothing of it
is left, and mounts the new one.

## JSON

`json.decode(text)` (std/json), whose T is what's expected of it
(`let notes: [Note] = try json.decode(text)`: the grammar has no type
arguments in expressions), `json.encode(value, indent: 0)` and the
network's `.json()` lower to the builtins `json.decode(text, desc)` and
`json.encode(value, desc, indent)`, where desc is T's descriptor
(core/src/rt/json.h) as a string constant. For the runtime to read and
write a struct's fields by name, each field's name in its shape carries
its descriptor after a NUL (`"title\x00s"`, `"tags\x00[s"`), and the same
for enum cases' labelled payloads; a payloadless enum JSON reads or
writes gets a shape of its name whose cases name its values.

## Native packages

A `native interface X { fn m(a: A) -> R }` (docs/native.md) lowers to an
object type (a reference: the runtime's ak_native_obj) and its methods:

- `X()` (making one) is CALLB `native.new(cls, iface, errors)`: cls is
  the class each platform's generated code registers for the package
  (its module ID), iface the interface's index in the package, errors
  the package's declared errors (an array of Error values, constants).
- A call `x.m(a)` is CALLB `native.call(x, method, args, argsDesc,
  retDesc, errors)`: method the method's index from 1, args a tuple of
  the arguments, the descriptors (core/src/native/native.h: rt/json.h's
  plus `o` objects, `F<n>…` callbacks, `V` views, `v` nothing) string
  constants. A `Ref` argument crosses as the view it holds at the call
  (CGET), which the codec writes as the id its host knows it by (0: none);
  each platform's generated code turns the id into its view. An
  `async` method is `native.start` with the same arguments, then AWAIT;
  cancelling the task cancels the call on the platform's side.
- Every method `throws` (the platform's side may fail); the checker
  requires `try` at calls.

A package's `native component X(p: P) { prop q: Q = d; event e(a: A) }`
(the prelude's are the runtime's elements) lowers to the Native kind of
view:

- `X(…) { … }` is CALLB `ui.native("<class>#X")`, then each prop by its
  name as the platforms' generated code knows it (its first letter upper
  case: `"Q"`) and its descriptor (as a method's parameter's):
  `ui.native_set(v, name, desc, value)` when the value reads nothing
  reactive, else `ui.native_bind(v, name, desc, thunk)`. A prop left
  out goes at its default; one with none isn't sent. The style's keys
  are any element's (`ui.set`, `ui.bind`).
- `on e(a) { … }` is `ui.native_on(v, "E", desc, handler)`, desc the
  parameters' descriptors in order: the host's Native event named `E`
  calls it with its `v` value decoded by them.
- The core (core/src/ui/native_view.c) sends the Type before the props,
  a prop only as its bytes change, and listens for Native events once a
  handler is set. The fake host shows the view by its component's name,
  and a test's tap on it is its `Press` event.

A native interface can't be generic, has methods only, and no struct
declares it. The class is the package's name as internal/nativegen
names it: asuka.toml's `name`, else its directory's. The errors array is
a global init.strings makes: every enum declared `: Error` in the
package, its cases in order (a case with values stands as an error of
its name).

A package's `native component V(p: P) { prop q: Q = d; event e(x: X) }`
used as an element is a view the platform's generated code makes:

- `ui.native(kind)`: a Native view (ops' kind) of kind `<class>#V`,
  which every host finds in the package's code (the class's provider,
  or package native's registry on the desktop).
- Each parameter and prop, set or else its default, is
  `ui.native_set(v, name, value, desc)` or, when it reads what changes,
  `ui.native_bind(v, name, thunk, desc)`: name its exported name
  (`q` → `Q`, as internal/nativegen names it), desc its descriptor. The
  runtime encodes it as a native call's value and sends it as a
  Native.Prop (type 5, bytes) only when the encoding changed.
- `on e(x) { … }` is `ui.native_on(v, "E", handler, desc)`: the
  Native event named E carries the values encoded under "v", decoded by
  desc (the parameters' descriptors in order) for the handler.
- `bind q: s` is `ui.native_bind` of s and a handler of the event
  `qChange(value: Q)`, which the checker requires, writing s.
- The style keys are `ui.set`/`ui.bind`, as any element's.

`asuka native generate` writes each platform's side (Swift on iOS and macOS, Kotlin,
JavaScript, Go on Windows and Linux, whose hosts are Go) from the same declarations, in
native/codec.go's wire format.

## C from Asuka

`extern` declarations (RFC 0008, docs/c.md) are C's. The checker
resolves their types twice: the Asuka type the program sees (`int` for
an `Int32`, `string` for a `CString`) and its C side (check.CType), kept
in Function.Extern, Field.C, and an extern type's TypeName (ExternType,
its Release).

| Source | Lowered |
|---|---|
| `extern fn f(a: Int32, s: CString) -> Int64` | a function whose body is CALLB `ffi.<package>.f(a, s)` (check.ExternBuiltin), the parameters as they are |
| a function returning an `extern struct S` | the same, with one more argument: GGET of S's template, a frozen zero `S` init makes (init.strings), whose shape the glue makes the result with |
| `extern type T` | a reference (the runtime's ak_ffi_obj, core/src/vm/ffi.h): an opaque pointer, its release, its name |
| `extern struct S { … }` | a struct, as any |
| `Pointer` | an int |

lang/cffi generates the glue from the checked program: per extern fn a
builtin that converts its arguments (an `Int8` is `(int8_t)a.i`, a
`CString` the string's data, `bytes` its data and length, an extern type
its pointer, a struct its fields, a callback an ak_ffi_cb), calls the C
function between ak_ffi_enter and ak_ffi_leave, and converts the result
(copying a `CString`, wrapping an extern type's pointer); per callback
signature a trampoline that makes Asuka values of C's arguments and calls
ak_ffi_cb_call; and asuka_ffi_register. A core built with the glue
(AK_FFI) calls asuka_ffi_register in ak_vm_builtins_init, before any
module loads, so the bytecode's CALLBs and lang/cgen's C (which calls
builtins as the VM does) find the wrappers by name.

## The standard library's native fns

In a standard library or platform SDK package (`std/<name>` in std/,
`platform/<name>` in sdk/), `native fn f(…) ->
R` has no body: the runtime implements it as the builtin `<name>.f` (a
method's `native fn` in `struct T`: `<name>.T.f`, the receiver first). Its
function's body is CALLB of that builtin with the parameters (AWAIT of
the future it returns when it's `async`), so it's called, passed and
compiled as any function. A generic one (`native fn rows<T>(a: bytes) ->
[T]`) passes, after the parameters, each type argument's descriptor
(rt/json.h's; `-` for a type JSON can't say), as a string constant of
its instantiation; std/flag's `spec<T>()` gets, instead, what
lang/ir/flag.go writes of T as JSON (its fields' kinds, defaults
written out, doc comments and short names; its commands), and an error
at the field for what a command line can't say. Other packages reach platform code through
native interfaces instead.

The builtins are C, in core/src/std (a file and a table per package,
registered with the rest); the package's Asuka writes the rest of its
API on them. A builtin makes only what C can without the module's
shapes: scalars, strings, bytes, and arrays of those (or optionals of
them); the Asuka side builds its structs (std/time's `Time` from an
instant and an offset, std/fs's `Entry` from a name and `[isDir,
size, modified]`). A failure is a runtime error, or for typed errors a
status the Asuka side reads and throws as its own enum (std/fs's
`FsError`, from the thread's last failure). Builtins may run on spawn's
threads, so they keep no state that isn't per thread.

A few standard library structs are made when the app compiles (lang/ir's
`StructHooks`, registered by lang/compile): a literal of one, its first
argument a string literal without interpolation, has the rest of its
fields filled by the hook from that string, as constants (`svg.Icon`'s
`data`, `width` and `height` from its markup, parsed by internal/svgparse and
encoded as the drawing the platforms' views decode; `shader.Program`'s
`code` and `glsl` from its Kage source, internal/shadercode's Compile
and GLSL). Any other first
argument, or one the hook rejects, is a compile error at it.

## Streams

`Stream(produce)` is CALLB `stream.new(produce, kind)` (kind: the
values' ak_kind); `for await x in s { … }` is: s into a local; at the
top, AWAIT CALLB `stream.next(s)` (a bool: whether a value came; it
throws what the producer threw), JF to the end, CALLB `stream.value(s)`
for x, the body, back to the top; at the end CALLB `stream.close(s)`,
which cancels the producer (core/src/rt/stream.c). An error leaving the
loop (the body's, the producer's) closes it too, in a handler around the
loop that throws the error on.

## Live values

`live(initial, source)` is CALLB `live.new(initial, source, kind)`,
which returns a closure that reads the value (core/src/vm/live.c): a
memo over a signal the stream's values write. The memo's first tracked
read queues the stream's start (on the ready queue: a producer's first
steps may write state, which a memo's update can't); its unwatched
hook cancels the stream. An untracked read returns the last value and
starts nothing.

## Imports

The compiler (lang/compile) finds each import: `./path` beside the
importing file's package; `std/json`, `std/math` and `std/embed` in
the checker itself; any other `std/<name>` in std/ and `platform/<name>`
in sdk/ (the CLI's copies: packages std and sdk, std.Package); an
`asuka/<name>`, the prefix both had once, is an error that names the
package's path now; anything else through the project's asuka.lock
(internal/pkgs.Imports), by the importing package's dependencies, so
two packages may import different majors under one name. A package's
symbols are qualified by its import path (a dependency's relative
imports by the dependency's path), so two packages' `./util` are two
packages.

## Prelude functions the runtime provides

| Asuka | Lowered |
|---|---|
| `environment()` | CALLB `ui.environment` (an Environment, tracked: read in a binding, it follows the screen) |
| `push(v)`, `present(v)` | CALLB `ui.push(builder)`, `ui.present(builder)` (the argument as a builder) |
| `pop()`, `dismiss()` | CALLB `ui.pop`, `ui.dismiss` |
| `sleep(d)` | CALLB `time.sleep(ns)`, AWAIT |
| `never()` | CALLB `async.never` (a future nothing resolves; cancelling its task rejects it), AWAIT |
| `all(a, b, …)`, `race(…)` | the calls as tasks (TASK), CALLB `async.all`/`async.race` over an array of them, AWAIT |
| `spawn { … }` | CALLB `async.spawn(closure, kind)`, a Task |
| `print(s)` | CALLB `print` |
## Sets

A set is the runtime's insertion-ordered map with void values, flagged
AK_MAP_SET (rt.h's `ak_set_*`): copied on write, compared (EQ: the same
elements, in any order), deep-copied for spawn, frozen and kept across
reloads as a map is.

| Source | Lowered |
|---|---|
| `["a", "b"]` where a `Set<string>` is expected, `Set<T>()`, a left-out payload's zero | CALLB `set.of(kind, elements…)` |
| `Set(xs)` | CALLB `set.from(xs)` (the first of equal elements first) |
| `s.contains(x)`, `insert`, `remove`, `isEmpty`, `union`, `intersection`, `subtracting`, `isSubset(of:)`, `isSuperset(of:)` | `set.has`, `set.insert` and `set.remove` (inout, as an array's `push`), `set.is_empty`, `set.union`, ... |
| `s.count()` | LEN |
| `for x in s { … }` | s into a local (a change in the body copies it), then i = 0 and, at the top, i = CALLB `set.next(s, i)` (the first live entry from i, -1 past the last), x = CALLB `set.elem(s, i)`: no allocation |
| `for x in s key x { … }` in a view | `ui.for` of a thunk returning `map.keys(s)` |
| JSON, `<T` | an array of the elements; decoding one, an element written twice is one |

## Built-in methods

The built-in types' methods are a table (lang/check/methods.go's
Methods): a receiver (an array, a map, a string, bytes, a float, a
Duration, an optional, mount's app, a Loading), a name, a signature over the receiver's type
variables, and the builtin (core/src/rt/lib.c) CALLB calls, the
receiver first, then the arguments in the builtin's order. An inout
method changes its receiver in place (see "Changing values in place").
A signature may name the prelude's types (StrokeStyle, TextFormat):
each checker reads it with its own prelude (methodSig). A method may be
a property (Field: `g.width`, `card.hovered`, no parentheses), append
constants to the builtin's arguments (Consts: the state ui.state reads;
Extra: `map`'s result's element kind, last, or a bool: `padStart` and
`padEnd` are one builtin, `str.pad`),
and share its arity with overloads the checker picks by an argument's
type (g.fill's color or gradient). The receivers are arrays, maps,
strings, bytes, floats, colors, optionals, mount's app, a Loading, a
Graphics, a Path and a Ref. A few the lowering does itself: `count()`
of an array or a map is LEN,
`isNone()` ISNONE, a map's `get(k)` MGET, `enumerated()` a loop making
`(index, element)` tuples (and `for (i, x) in xs.enumerated()` makes
none: it's a loop over the indexes). Closures passed to them don't
throw and don't await.

## Notes

- A `Length` from a number computed at run time is `length.points` of
  it (a constant one is packed when compiling).
- The runtime reports a user error's message only where the lowering
  asks for it (tests, `e.message`): `ak_error_message` reads a struct's
  `message` field, and user errors are enums; it could call the shape's
  `message` method (VM shapes have it) so that boundaries and the
  app's error reports show them too.
