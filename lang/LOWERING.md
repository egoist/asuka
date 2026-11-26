# Lowering Asuka

How the compiler turns Asuka (docs/language/, RFC 0007's grammar) into
bytecode (lang/bytecode) that the VM (core/src/vm) runs. Release builds
compile that bytecode to C (lang/cgen): each function becomes C that
does what the interpreter's bodies for its instructions do
(core/src/vm/ops.h), on its registers as C locals (below, Release
builds: C); one that may suspend becomes a resumable one (Functions
that suspend, in C). So the bytecode is the one contract, and both
backends mean the same thing by it (core/vm_test.go and lang/compile's
programs run every module both ways).

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
lang/ir       the optimizer (opt*.go): passes over the lowered code,
              below, Optimization
lang/cgen     IR -> C (release)
lang/compile  the driver: a package and its imports -> one module
```

A program is compiled whole: the app's packages, their dependencies and
the prelude (lang/prelude: types.asuka, enums.asuka, elements.asuka, and
the built-in functions and methods) into one module, for one platform
(lang/target): each package's files for it, and `@platform` its case.

## Optimization

The lowering emits code that's easy to check, not cheap to run: a temp
for each value, then a copy into its place; a constant loaded into a
register for each use; a comparison into a bool, then a jump on it; an
element taken out of its array and put back around a change of one of
its fields. lang/ir's optimizer (`Program.Optimize`, opt*.go) rewrites
that, before the module is encoded, so the VM and the C both run the
result. It's on for `asuka run`, `asuka test` and `asuka build`; off
for an app under `asuka dev` (it costs about as much as the rest of a
compile, and a reload should be quick), for coverage (its counters are
the statements'), with `compile.Options.NoOptimize`, and with
`ASUKA_OPT=0` in the environment.

What it keeps, whatever it changes: each result; each error, its
message, and the line and frames its trace names; when a named value
that isn't data alone (a native object's, a closure, a task: what its
drop does is the program's to see) is let go of; and it lets nothing go
later than the lowering does, if it can tell (a value kept longer would
make a change in place copy what it changes). A reference register's
write is deleted or moved only when the register is known to hold none
there; MOV of a reference moves it (no copy), and is never treated as
one. What the runtime makes from a value and keeps (a Seq's cursor)
holds a reference to it, never a pointer borrowed for as long as some
register holds it.

First, once (opt_release.go), on the code as lowered: a temp's value,
or a local's whose type is data alone (numbers, strings, bytes, and the
arrays, maps, sets, optionals, tuples, structs and enums of them), is
let go of where it's dead, before something that may allocate (a call,
a string or a value made) would find it still held: CLR right after its
last read, or, read in a loop, as the way out of the loop starts. Go's
collector frees what's unreachable likewise, and Swift ends a value's
lifetime at its last use: `n += s.split(" ").count() + s.fields().count()`
holds one array at a time, not both, and an array a loop read is freed
before what comes after the loop is made. The lowering records where
each register is a temp or such a local (Func.tempSpans); a register
an element or a field was just read out of (IDX, GETF...) isn't
cleared there, as the C may read that element through a pointer
borrowed from it (lang/cgen's places).

The passes, run in rounds until they change nothing (three at most),
each over a function's control flow graph (blocks start at jump and
handler targets; an instruction that may throw has an edge to its
innermost handler, with the registers as they were before it):

- Dead handlers: a handler whose range has nothing that throws goes,
  and with it its code (unreachable code goes too).
- Jumps: a jump to a jump goes where that one goes; one to the next
  instruction goes; one to a return is the return; `JF c, L1; JMP L2;
  L1:` is `JT c, L2`; and a loop's jump back to its test is the test,
  inverted (one jump a turn).
- Copies: a read of a register copied from another (MOV, MOVR), neither
  written since, reads that one (a reference is read borrowed); a
  call's one argument too, when the callee isn't a mut fn (a mut fn's
  receiver goes back into its register).
- Constants: a scalar's known value is propagated, what it computes is
  folded (not an overflow, a division by zero, a shift by a negative
  count: those stay, to throw as they run), a branch on it is decided.
- Selection: an int operation with an operand that fits 16 bits takes
  it as an immediate (ADDIS, SUBIS, MULIS, DIVIS and MODIS for a divisor
  that's neither 0 nor -1, ADDIK for a wrapping one); a comparison whose
  result only a jump reads is fused into the jump (JLTI, JEQIS...: ints
  only, as a float's NaN makes `!(a < b)` not `b <= a`); `NOT c; JT c`
  is `JF`.
- Common subexpressions: a scalar computed again in a block from
  registers not written since (a field read twice: `b.vx * b.vx`) is a
  copy of the first.
- Scalar replacement: a struct or a tuple of scalars only, made (NEWS)
  and only read by its fields (GETF) before its register is written
  again, isn't made: each GETF reads the register the field was made
  from (none written on the way). A small function returning
  `(int, bool)`, inlined, allocates nothing.
- Coalescing: `OP t, ...; MOV d, t` is `OP d, ...` when nothing reads t
  after and nothing between reads or writes d (or throws to a handler
  that reads it); a reference only when t held none and is let go of
  right after, before d is written.
- Elements: `xs[i].f` (IDX of a struct, its only reads GETFs) is IDXF,
  which reads the field where it is; `xs[i].f = v` (IDX, SETIDX of none,
  SETF, SETIDX back) is SETIF, which copies the array and then the
  element only if they're shared, as the four did. The index is checked
  by the first, where IDX checked it (what's between is pure).
- Dead code: a pure instruction whose scalar result nothing reads goes;
  a new value (NEWS, NEWA, CLOS...) into a reference register that held
  none, likewise; CLR of a register known none; a reference's copy that
  nothing reads before it's cleared (`MOVR d, s ... CLR d`, s not
  written between), in the block or on every way on (an inlined
  argument's, cleared after its body).

Then, once per program, inlining (opt_inline.go), callees first: a
call of a small function (40 instructions, 24 registers at most; not
async, not a mut fn, no handlers or peeks, not calling itself or its
caller), and a CALLC of a closure a CLOS made (the last write of the
closure's register on every way to the call: in the same block, or
before a loop the call is in; its captures' registers not written on
the way, or, scalars, copied as it's made), is replaced with the body, in
registers of the caller's (one set per callee: its bodies never run at
once): the arguments copied in as a frame takes them, each return a
copy of the result and a jump past the body, the body's references
cleared after it as its frame's are as it returns. A parameter the body
never writes (sets, moves out of, changes in place, passes in a block)
isn't copied in: the body reads the caller's argument register, or the
register that argument was just copied from in the call's block (not
written since), so passing a value to an inlined function retains and
releases nothing. A closure the call was on is then often never read,
and isn't made. Not into a function
that may suspend, nor in a handler's range when the callee has
reference registers (an error caught there would keep them until the
caller ends). The callee's line numbers stay on its instructions, and
the caller's `Inlines` (bytecode.Inline: a range, the function, the
call's line; nested ones listed first) say whose they are: the VM's
traceback, `ak_vm_here` and lang/cgen's frame hooks name the inlined
function's frame, then the caller's at the call's line, as if it had
been called. (A program's recursion goes as deep as it did, but for the
frames inlined.)

Last, a constant's load in a loop that's its register's only write,
which no path reads before, moves to the function's start, and the
registers are renumbered into as few as the code still names (in order:
a block of them stays one).

## @platform and @debug

`@platform` (syntax.AtExpr) is checked as the prelude's `Platform`, the
same on every platform, and lowered as the constant case of the
compile's platform (ir.Options.Platform; KSMALL, an int enum). `@debug`
is checked as a `bool` and lowered as the compile's ir.Options.Debug
(KSMALL 0 or 1): true for asuka dev, run and test (appbuild.Debug), false
for build, publish and benches. Lowering folds what they decide
(lang/ir/fold.go) and lowers only what's taken:

| Source | Lowered |
|---|---|
| `@platform == .linux`, `!=`, `@debug`, `!`, and `&&`, `\|\|` of them, `true`, `false` | KSMALL 0 or 1; a side that doesn't decide the whole drops out (`true && x` is `x`); `f() && false` still runs `f()` |
| `if c { A } else if d { B } else { C }`, as a statement or a value | only the branch taken; a chain's clauses before its first `let` likewise |
| `match @platform { … }` (statement, value, view) | the arms up to the first that matches without a guard, without those whose case can't; in a view, one arm with no names is built once, without `ui.switch` |
| `if c { A } else { B }` among views | the taken branch's builder called once (CALLC), without `ui.show`: the other's views never exist |
| what follows, in its block, a statement that leaves (`return`, `break`, `continue`, `throw`, a `never` value; an `if` or `match` whose every branch that may run leaves, a folded one's taken branch only) | nothing (Lowerer.leaves): `if !@debug { return x }` and the rest of a release's block isn't in it |

What's left out is never lowered: its functions, strings, constants and
builtins aren't in the module. The checker doesn't fold: reachability,
exhaustiveness (but for `.unknown`, which `@platform` never is) and
every branch's types are the same on every platform.

## Types to kinds

| Asuka type | Kind | Representation |
|---|---|---|
| `int`, `Duration` (ns), `Color` (ops.Color), `Length` | int | as is; a Length packs its unit (0 auto, 1 points, 2 percent) in bits 32-39 and its float32 value in bits 0-31 |
| `char` | int | its code point (a Unicode scalar value); `'a'` a constant, `char(n)` CALLB `char.of` (an error for what isn't one), its methods `char.*` builtins over rt's Unicode tables (unicode.h's ak_ucat: the general category and White_Space); JSON's descriptor `r`, a string of one code point |
| `i8` … `u64` | int | canonical: sign-extended (`i8`, `i16`, `i32`) or zero-extended (`u8`, `u16`, `u32`) from the type's width; a `u64` its 64 bits (past int's largest, a negative int64) |
| payloadless enums | int | the case's index (a wire enum's value) |
| `bool` | bool | 0 or 1 |
| `float` | float | |
| `string` | ref | ak_str: its bytes after its header; appended to in place when its holder's alone (`s += x`: a buffer that doubles), a split's pieces made in shared slabs, a short one a header and one 16-byte copy, its slab's count of them kept as it's filled and let go of together by the array that dies with them (rt.h's ak_pieces, core/src/rt/str.c, obj.c's free_strs) |
| `bytes` | ref | ak_bytes |
| `[T]` | ref | ak_arr, element kind T's, shape T's; its elements 8-byte values, but a `[bool]`'s a byte each and a sized integer's below 64 bits (`[u8]`, `[i32]`...) its own width (ak_elem: packed, holding no references; a map's keys or values of a sized type are values) |
| `[K: V]` | ref | ak_map |
| `Set<T>` | ref | ak_map of T's keys to void values, flagged AK_MAP_SET (core/src/rt/set.c) |
| `BTree<K, V>` (the standard library's only) | ref | ak_tree: a B-tree of ak_tnode nodes, its keys ordered in C (core/src/rt/tree.c; see "B-trees") |
| `Seq<T>` | ref | an ak_seq, the runtime's immutable description of a lazy sequence (core/src/rt/seq.c) |
| struct, tuple | ref | ak_struct with its shape |
| enum with payloads, or declared `: Error` or having an interface | ref | ak_enum (payloadless cases: shared immortals) |
| `T?`, T a reference | ref | NULL for none |
| `T?`, T a scalar | ref | NULL, or an ak_box |
| `fn(...)` | ref | ak_closure |
| store | ref | an ak_struct of the store's cells, memos and methods' captures |
| worker | ref | its handle (core/src/rt/worker.c): its thread and queue; its state, an env, is its thread's |
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

A map's keys are what the runtime's maps hash (ak_val_hash, as
ak_val_eq compares): ints (and what's stored as one: bools, payloadless
enums, durations, colors), strings, and the structs, enums, tuples and
optionals made of them (references: hashed by their parts). The VM
makes every map of reference keys flagged AK_MAP_STRKEYS; what reads
keys as strings (std/sqlite's arguments) goes by the type's descriptor.

## Registers

Each register has one kind for the whole function. The lowering
allocates them as it goes: a temp lives until the end of the statement
that made it, a local until the end of its block (a reference local is
cleared there, so a value someone changes next is unique, and changed in
place; the optimizer lets a temp's value, or a data local's, go after
its last read: see "Optimization"), and a run of consecutive registers
of given kinds (a call's arguments, a struct's fields, a closure's
captures) is reused only for the same kinds. An argument block's references are cleared after the
call for the same reason.

An expression's value may be a local's own register, borrowed: whoever
changes a value copies it first, or changes it in place through its
owner (see "Changing values in place"). What a change's value, its
arguments, a `let`'s value or a condition read on the way (a struct's
field, for `items[i] = items[j]` in a mut fn) is cleared (CLR) once the
value is computed, not at the statement's end: held, it would be a
second reference to the place about to change, which would be copied.
A let's or an assignment's value, a temp, is cleared once the names or
the place hold it (so `n = n.kids[i]` coalesces into one IDX into n).

## Changing values in place

`x.f = v`, `xs[i] = v`, `m[k] = v` and the builtins that change their
receiver (`push`, `insert`, `removeAt`, `removeWhere`, `pop`, `clear`,
`sort`, `remove`) first compute every index and argument, then reach the
value through its owner:

- a local: SETF/SETIDX/MSET on its register; an inout builtin gets it
  moved (MOV, no reference counted) into the first register of its
  argument block and moved back after, so a value only the local holds
  is changed in place;
- a boxed var (a closure's, a worker's): VTAKE (the reference moved out
  of the box, none left in it, as a cell's CTAKE), change, VSET, with a
  handler that puts it back if the change throws; a scalar, or a change
  whose code may read the var (below), VGET, change, VSET;
- state (a cell: a component's or a store's, or a file's): CTAKE,
  change, CPUTP with the place's path (see "Reactivity"), with a handler
  that puts it back if the change throws (CPUTP saying anything below
  the place may have changed);
- a field or an element of those: read it (GETF, IDX), and when it's a
  reference take it out (none put in its place, so it's unique and
  changed in place, not copied), change it, write it back (SETF,
  SETIDX), with a handler that puts it back if the change throws.

A builtin that calls a closure (`removeWhere`, `sort(by:)`) on state
reads the cell (CPEEK) rather than taking it, and puts the changed copy
back: the closure may read that state, and sees it as it was.

A builtin that may raise an error (`removeAt` out of range, one that
calls a closure) runs in a handler that moves the receiver back first.

A map's entry (`m[k]?.f = v`, `m[k, default: v].push(x)`) is reached
with MGETJ: when it's there, taken out as a field's value is (MSET of
none, the change, MSET back); when it's missing, `m[k, default: v]`
evaluates v there, and only then, and the change MSETs it, while
`m[k]?.…` leaves the map as it is.

## Methods that change

A `mut fn` (RFC 0011) is lowered as a method whose function has
bytecode.FMut: its first parameter, self, is inout. Its call is an
update of the receiver's place, as `x.f = v` and the inout builtins are
(`lvalue`, lang/ir/stmt.go; lang/ir/mut.go): the arguments are computed,
the place reached (a cell taken out, CTAKE; an element or a field taken
out of its place), and the receiver moved (MOV, no reference counted)
into the first register of the call's argument block. The VM's
`ak_vm_push` moves it into the callee's register 0 without retaining it
and clears the caller's, so a value only the place held stays unique
and the body's SETF and SETIDX change it in place; as the frame ends,
returned or thrown, `ak_vm_pop` moves register 0 back into the caller's
register (by its index in the coroutine's registers, which stays as
they grow; a call from C, by its address). A compiled mut fn moves
`A[0]` in and back itself; the interpreter calls one with a copy of the
argument block, so that what it puts back lands where the caller's
register is after the call (it may have moved the registers). An async
caller compiled to C calls a mut fn from C, never through the run loop
(a mut fn never suspends). The caller moves the receiver back into its
place after the call, and in a handler around the call when it throws:
what the body changed before the throw stays. On state that's one
CPUTP: one write, of the receiver's place, which tells the readers of
everything below it (the body's changes aren't recorded), or, when the
cell was read rather than taken out (the callee reads state), one CSET,
compared path by path.

When the callee may read state (check.FuncCtx.ReadsState: it reads a
package's state or a store, or calls a closure or an interface's method
that may), a cell is read (CPEEK) rather than taken out, so the body
sees its value, and the change copies it. A mut fn called by its bare
name, or on self, in another updates register 0. A closure made in one
captures self's value (self is never boxed). Through an interface's
value, it's CALLI: the method found has FMut, and the VM moves the
receiver the same way.

## Writes through ?.

A place whose path has a `?.` (`inbox[id]?.read = true`, `user?.mail?
.archive()`, `tags?.push(t)`) is lowered in two steps (lang/ir/
places.go). First the path is read as it is, with its prepared indexes,
up to its last `?.` (CPEEK, GETF, IDX, MGET), taking nothing out, and a
none jumps past the whole change: nothing is written, the value on the
right isn't computed, and a cell's readers aren't told (a put tells
them what it changed, but the probe doesn't even take the cell out). Then the change runs as any (the value computed,
the place taken out), and tests again where it is (lvSome: JNONE past
the change, a scalar UNBOXed and BOXed back), since computing the value
may have changed the place. A call through `?.` gives its result made
optional, none when it didn't happen. `a?.items[0]` reads, in a chain,
as `a?.b` does: none when `a` is.

`if var x = v` is `if let` whose name is declared mutable: a local
register holding a copy.

## Extensions

An extension's method (check.Function.Ext) is a function whose first
parameter is its receiver, compiled per receiver type (funcIndex's key
has it; its type parameters are the receiver's, check.ExtSubst) and
called with CALL, as a struct's method is. Its static fns are the
package's functions; its constants are globals made in init, frozen, as
a type's are. A bare name in a method's body that's self's method or
property (a built-in one, or an extension's) is checked and lowered as
`self.name` (check.Info.ImplicitSelf).

A type has an interface through an extension where the checker sees the
conversion (check.Info.ExtImpls): `implement` enters that function in
the shape's table of methods, and a type parameter's method resolves to
it. A shape has one table in a program, so two packages' extensions
that give one type the same method, both making it an interface's, are
the checker's error. An enum without payloads that `extend E:
Interface` gives an interface is stored as a reference (an ak_enum)
everywhere, as one that declares it is.

## Iteration

`for x in v` over a value whose type has `next() -> T?` copies v into a
hidden var (check.ForInfo.IterVar), and each turn calls `next()` on it
as a mut fn's call on a var (the iterator changed in place), JNONE to
the loop's end, the value unboxed into the item. A sequence (`fn
iter()`) is held in a hidden var too, and `iter()` called once.
Arrays, sets, maps and ranges keep their own loops.

## Loops' names

`break name` and `continue name` (check.Info.Labels: the loop) jump to
the named loop's end or next turn (`b.loops`' entry whose statement it
is), after running the defers of every loop they leave, innermost
first, as a `return` runs a function's.

## Functions

- A function is a FuncDef; its parameters are its first registers.
  Methods take `self` first. A `static fn` of a struct or an enum is a
  function of the package's (check.Function.Static its type), called as
  any (`Money.of(…)`, `.of(…)` where the type is expected): no `self`.
  A generic type's (or a generic extension's) takes the type's
  parameters before its own (check.Function.TypeParams), so it's
  compiled per call's type arguments as a generic function is. A closure captures a value as it is when
  the closure is made; a `var` it captures that anything changes (the
  closure, or the code around it) is a box (VBOX, VGET, VSET), so they
  share one variable. A `spawn` copies a box's value instead (VGET as
  it's made: values only). A closure stored in a var it captures (one
  that calls itself through it) holds the box that holds it: a cycle
  the checker warns of, pointing at a local fn.
- A FuncDef's `Locals` (bytecode.Local) name its registers for the
  debugger (core/src/vm/debug.c), in code the optimizer leaves alone
  (asuka dev's: ir.Options.Locals): each parameter, local, capture, a
  method's `self`, a component's members in its function and its env as
  `self` in its closures, with the register, the pcs `[From, To)` where
  the name holds it (from the instruction after the one that sets it to
  where its block, statement or block of arguments lets the register
  go: `fb.setVar`, `endLocal`), how the register holds the value
  (`Via`: the value; a box, for a var closures share; a state's cell, a
  let's memo, an accessor) and its type's descriptor (`Desc`: rt/json.h's,
  a duration `d` and a color `c`). The optimizer drops them: its passes
  move values between registers and instructions, and renumber them.
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
  fields and methods are in scope by their bare names. A `fn` can't
  change its value; a `mut fn` can (see "Methods that change").
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

## Release builds: C

lang/cgen writes each function's bytecode as C a C compiler optimizes as
it would a hand-written function's: what the interpreter decides as it
runs (a register's kind, a field's, whether to release) is decided as
the C is written, from the bytecode's kinds.

- Registers. Each is a C local of its own (`ak_val r<n>`), which the C
  compiler keeps in a machine register; a reference starts as none, and
  the function's end releases its references, statically (a parameter
  it only reads is borrowed: neither retained as it starts nor released
  as it ends). What takes an argument block (a builtin, the VM, a
  closure, CLOS's captures) gets a copy of the registers (`B_`), copied
  back after a call, for what an inout callee changed; nothing takes a
  register's address, so none is kept in memory. A function of more
  than 384 registers (a big module's init) has them as an array, as the
  VM's frame: C compilers take too long to allocate thousands of locals.
- Calls. A synchronous function is three C functions: its body
  (`fN_`), which takes its arguments as C values of their kinds (a mut
  fn's receiver by its address), then its closure, and returns `ak_ret`
  (its result, and whether it threw) in two machine registers (the
  arguments first: the first is in the register the result goes back
  in, so returning it moves nothing); its entry with the arguments in an
  array (`fN_c`, an `ak_direct_body`); and its VM entry (`fN`, an
  `ak_native_body`), which the interpreter and builtins call. A CALL of a
  compiled function calls its body. The body checks the stack as it
  starts, as Go's prologue does: the C stack past the thread's limit is
  a stack overflow (ak_cg_overflow), the error a register's test. On
  arm64 the limit is in x28, which the app's C is compiled without
  (cbuild.AppFlags: `-ffixed-x28`), as Go keeps its g in R28: the check
  is `cmp sp, x28; b.lo`, and no register of a recursing function's
  holds the limit, no call passes it; each way in from the core (fN,
  fN_c, an async body's fN) sets x28 and puts the core's back (it's
  callee-saved: the core keeps its own across calls into the app).
  Elsewhere the limit is the body's argument after its closure (ops.h's
  AK_CG_*: the C is the same). A function that calls nothing doesn't
  check: it can't recurse, and its frame fits in what the limit leaves.
  The depth of compiled calls isn't counted: they go as deep as the
  thread's stack, as Go's and Swift's do (the VM's frames stop at
  10,000). A CALLC of a compiled function's closure, or a CALLI of a
  compiled method, whose arguments' kinds are its parameters'
  (`ak_func.sig`: their count and C types, one compare; a closure keeps
  a copy of its function's, `ak_closure.sig`, so the test reads the
  closure only) calls its body too, the arguments in registers; else its
  array entry; else the closure's invoke. Other calls test the thread's
  pending error, its slot found once a function (ak_cg_pending: a
  thread-local's read is a call on Apple's systems). An error leaves a
  function through ak_cg_unwind, a cold function (it tells
  ak_frame_hook of the frame): the C compiler takes every way to an
  error as unlikely, so a call's test of its result falls through when
  it didn't throw.
- Instructions. Checked int arithmetic, comparisons, an index's range
  and a value's uniqueness are tested inline (ops.h's value-based
  bodies, the interpreter's too); a scalar element or field is read and
  written as its C type. The pure math builtins (`math.sqrt`, `floor`,
  `abs`...) are C expressions, not calls through the module's table; a
  string's `byteCount` is its length, and its `equals`, `compare`,
  `contains`, `hasPrefix` and `hasSuffix` call the runtime's functions
  directly (no argument block, no test of the pending error).
- Places (lang/cgen/places.go). Along a straight path (each instruction
  the only way into the next): an element or a field read into a
  register that only reads it before it's cleared, with nothing on the
  way that changes what it's in (a call isn't given it; what it's read
  from is a register of its own, which holds a reference to it, so no
  call changes it in place or frees it), is a borrowed C pointer (no
  retain, no release; the register stays none, so an error on the way
  releases nothing); likewise a global no function but the module's
  init writes (a file's let), whatever runs on its way, to its CLR or
  the function's return; an element taken out, changed by SETFs and put back
  (Changing values in place) with nothing on the way that can throw is
  changed where it is, the array and the element made unique there; a
  take-out or a put-back is a move; and an array made unique, an index
  checked or an element made unique stays known until something could
  share, resize or replace it, so its checks aren't repeated. The
  optimizer's IDXF and SETIF are planned as the forms they replace:
  IDXF reads through the element where it is (its result borrowed as a
  field's read is, when only read), and SETIF makes the array and the
  element unique and checks the index only where nothing before on the
  path has, so `b.vx += ...; b.vy += ...` on one element checks once.
  Every element is read and written as its array stores it, through
  one set of helpers (insn.go's elemRef, elemSlot, loadElem, storeElem):
  a reference (a struct...) as a pointer (AK_REFS), not through the
  ak_val union (a C compiler takes a union's member for a char's, which
  every store of a field may change, so the element's pointer would be
  read again after each); a float as a double; a bool as a [bool]'s
  byte (AK_BOOLS: a [bool] is always packed); an int as a value, or,
  in a function whose code has sized integers (bytecode.FSized: the
  lowering sets it where a type it sees is one or an array of them, and
  an inlined body brings it), the array's storage asked (only a kind is
  known: an [u8]'s elements are ints too), a sized integer's at its
  width. Elsewhere every array of ints holds values, read and written
  as them (ak_arr_vals, which panics on a packed array in a build with
  assertions, the tests').
- Facts across the control flow (lang/cgen/flow.go): what a check made
  known holds where every way in has it, and a loop's tests make indexes
  known in range: an int known not negative (a constant, a length, a
  checked sum, +1 of one less than an int), `i < n` from a jump's test
  (JLTI's way, JLEI's other, JT and JF on an LTI's register), `n` an
  array's length (LEN, which an element's change keeps), an index an
  instruction took; so `for i in 0..<xs.count()`, `(i + 1)..<n` and
  `c + 1 < n && xs[c + 1]` check no index, and `i + 1` of an index is an
  unchecked add. And an int's magnitude (|x| < 2^b): a length or an
  index an instruction took below 2^32 (ak_arr's len is a uint32), a
  constant its own, a sum or a difference of known ones below 2^(the
  larger b + 1), a product 2^(their sum); an ADDI, SUBI or MULI (or
  immediate form) known below 2^63 so can't overflow and isn't checked
  (sieve's `j += i`, of two indexes). Where the ways in disagree it's
  widened to an index's, then dropped. Unchecked, an op of two registers
  is opaque to the C compiler's loop analysis (AK_CG_OPAQUE, an empty
  asm), as the checked op's result was: given a counter stepped by
  another register as a plain add, clang works out the inner loop's
  trip count (a 128-bit division) on each turn of the outer loop. A loop whose first path (what runs every turn) changes
  an array's element in place has the array made unique as it's entered,
  and the element at an index it doesn't change (in range there): one
  check before the loop, not one a turn. None of it depends on how the
  elements are stored: a packed array is made unique as a copy just as
  packed (ak_arr_mut), and the elements made unique are structs, which
  are values.
- Strings in the frame (lang/cgen/stackstr.go). A string an INTERP or
  a CONCAT (not `s += x`) makes whose value goes nowhere but into what
  reads it in place is built in a buffer in the function's C frame
  (rt.h's ak_strbuf, 63 bytes and the NUL) when it fits, as Go's
  concatstrings writes a result that doesn't escape into its caller's
  tmpBuf: no allocation, nothing freed. The value is followed forward
  over the control flow (handlers' edges too) as the registers that may
  hold it: a MOV or MOVR adds its destination, any other write takes
  the register out, and each instruction reading one must read it in
  place and keep nothing: LEN, EQ, NE, LTS, ISNONE, JNONE, JSOME, a
  map's lookup by it (MGET, MGETJ), an INTERP's part, and the builtins
  that only read (byteCount, count, equals, compare, contains,
  hasPrefix, hasSuffix, find, isEmpty, toInt, toFloat, hash, print, and
  std/os's Writer.write, os.put: an intrinsic, rt.h's ak_stdio_put, a
  copy into the stream's buffer). Anything else (a return, a store, a
  capture, a call, a throw, a mut fn's receiver, which goes back to its
  caller) is an escape, and the string is made on the heap; so is one
  made again while a register other than the one it's made into still
  holds the last one (a loop carrying it into its next turn), or while
  that one is among its parts (`s = "{s}."`). A loop's line, its
  register not cleared after its last read, is made in the frame each
  turn: the write drops the last one. The string is immortal, so a release does nothing, and
  flagged (AK_STR_STACK): ak_retain panics on one in the tests' builds
  (AK_TEST; what this analysis missed would be a keep); a copy into
  another register is ak_retain_local's, and a copy for another thread
  copies it. Not in a function that may suspend (its registers move to
  the heap), and eight a function at most.
- Write cursors (lang/cgen/cursor.go). A fused loop's appends into the
  array it made (arr.append_room, arr.append_own: an array's map,
  filter, compactMap) store through a pointer kept in a C local (W<n>_,
  with the room's end, E<n>_), opened as the loop is fallen into: each
  append read the array's buffer and count and stored the count back,
  which the C compiler can't keep in registers (the element's store may
  alias them, and an overflow check can leave between). A filter's
  append compares with the end, and on none left writes the count back,
  grows the array (ak_op_append_slow) and opens the cursor again. The
  count is written back on every way out of the loop: falling out of
  its end, a jump out or an error (each through a stub, X<n>_, after
  the function's code). A loop has one when it's entered only by
  falling into its first instruction, has no handler in it and no
  return, and every instruction in it naming the array's register is
  one of the appends' moves (lang/ir's appendReg): nothing else reads,
  copies or passes the array while its count is stale.
- The optimizer's other instructions are C of their own: an immediate
  (ADDIS, DIVIS...) a literal, a division by a constant the C
  compiler's multiply, a fused comparison (JLTI, JGEIS...) one `if`.
  An error in a body inlined into the function goes first through a
  trampoline (`I<pc>`) that tells `ak_frame_hook` (ak_cg_unwind) of
  the inlined function's frame, then on with the call's line, so a
  trace reads as the VM's does; every error's path sets the line it
  left from (`at_`).

## Functions that suspend, in C

A release build compiles a function that may suspend (FAsync, or one
with an AWAIT or a YIELD) as other languages compile coroutines: to a
resumable C function (core/src/vm/vm_int.h's ak_async_body), on the
interpreter's frames.

- Its frame is on the coroutine, pushed as the interpreter pushes one
  (ak_vm_push), and its pc means what the interpreter's does: where the
  frame resumes. Its body starts with a switch on it: 0, an AWAIT's own
  pc (the AWAIT runs again, its future settled), or the pc after a
  YIELD or a call. Its registers are the body's C locals: it moves them
  out of the frame as it starts (ak_op_enter) and back when it leaves
  the frame waiting (ak_op_leave).
- It returns how it left: returned (its result), threw (no handler of
  its took the error: it reported its line to ak_frame_hook, as a
  synchronous function does), suspended (an AWAIT whose future is
  pending, a YIELD), or pushed a frame the run loop is to run.
- A CALL of a function that may suspend, a CALLC of a VM closure that
  the interpreter would push, a CALLI of a method not compiled to a
  plain C function push the callee's frame on the coroutine, as the
  interpreter does. A compiled callee runs at once, in C (ak_op_acall):
  calls that don't wait cost a C call and a frame, pushed inline
  (ak_op_apush: room for it and its registers, not a mut fn, is a few
  stores; else ak_vm_push), and read no thread-local: the C stack's
  limit is the coroutine's (ak_op_lim: the thread's, kept as it's first
  asked) and the depth of direct calls is counted on it (ak_co.adepth),
  so a call that doesn't wait costs about 140 instructions, not 230. Past
  AK_VM_ADEPTH_MAX such calls deep, with the C stack near its limit, or
  for an interpreted callee, the caller leaves its frame waiting on the
  callee's and the run loop runs it, so async recursion takes no C
  stack. When the callee suspends, each compiled caller leaves its frame
  too, to resume after the call.
- The run loop (core/src/vm/run.c's ak_vm_run) runs a coroutine's top
  frame: a compiled one's body, else the interpreter, which hands back
  when the top frame is compiled. When a body returns, it pops the
  frame and stores the result in the caller's register, which the
  caller loads as it resumes; when it throws, the error goes to the
  first caller that takes it: an interpreted one's handler covering the
  call, or a compiled one, which resumes after the call with the error
  pending and goes to its handler there. A release build compiles every
  function, so it never interprets.

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
MODI (the last two throw on a zero divisor), `-x` NEGI, `& | ^` BAND,
BOR, BXOR, `~x` BNOT, `<< >>` SHL and SHR (ops.h's `ak_op_shift`: a
count of 64 or more shifts every bit out, `>>` keeping the sign; a
negative count throws, as Go panics).

`**` is POWI (by squaring, checked; a negative exponent throws) and,
of floats, POWF (C's `pow`; a float's power of an integer converts it,
I2F or U2F).

The sized integers (`i8` … `u64`) are ints with a type's code, an
immediate (check.IntCode, rt.h's ak_int_type: log2 of its bytes, 4 more
when unsigned): `+ - * / %` ADDN, SUBN, MULN, DIVN, MODN (unsigned
division for an unsigned type), `-x` NEGN, `**` POWN, `<< >>` SHLN and
SHRN (logical for an unsigned type), which keep a value canonical and
throw past the type's range as ADDI does past int's. A shift's count and
a power's exponent may be of another integer type: SHLN, SHRN and POWN
take both codes (the value's, and the count's in the high 4 bits; a
u64's count past int's largest is past every width), and an int's
`x << n` of a sized n is SHLN too. `& | ^` of two canonical values are
canonical: BAND, BOR, BXOR. `~x` is BNOT, then WRAP (its low bits at
the type, sign- or zero-extended) for `u8`, `u16` and `u32`; `&+ &- &*`
are WADDI, WSUBI, WMULI then WRAP. `<` and `<=` of a u64 are LTU and LEU;
the others' order is the int's (LTI), as are `==` and hashing (the same
bits). A conversion is CONVN (to type t & 15 from t >> 4: throws when it
doesn't fit), WRAP (`truncating:`), CLAMPN (`clamping:`), `int.exactly`
(`exactly:`, a box or none), nothing at all when it widens; from a
float F2N (toward zero, saturating) and `int.exactly_float`; `float(x)`
of a u64 U2F. A literal is its type's: its bits (a u64 past int's largest
too) in a KINT, checked against the type's range as it compiles. `T.min`
and `T.max` are constants, `T.parse(text, radix:)` CALLB `int.parse(text,
radix, code)`. The builtins that read a value's type (`toString(radix:)`,
`padded`, the checked methods, `abs`, an array's `sum`, `str.fmt_int` of
a u64) take the code after their arguments (check.Method's Extra
"intcode"); `sorted()`, `sort()`, `min()` and `max()` of u64s pass none,
0 and 1 (ordered unsigned), and `a..b` of u64s `arr.range(a, b,
through, 1)`; `for i in a..<b` of u64s tests with LTU and LEU. A u64's
text is TOSTR of kind 5 (bc.KU64Text, never a register's kind); its
descriptor, as every sized type's, `n` and its code (`n7`), which JSON,
CSV, TOML and SQLite read with its range.

ADDI, SUBI, MULI and NEGI are checked: ops.h's `ak_op_addi` and its kin
compute with `__builtin_add_overflow` (sub, mul: one branch on the CPU's
overflow flag, predicted not taken) and on overflow throw "integer
overflow" through `ak_op_overflow` (out of line, cold), returning false:
the VM goes to `unwind`, cgen's C to the handler, as for `ak_op_divi`'s
zero, which also throws for the smallest int `/ -1` (its `% -1` is 0).
The destination isn't written on overflow. `&+ &- &*` are WADDI, WSUBI,
WMULI: `uint64_t` arithmetic, which wraps. The compiler's own counters
stay ADDIK, which wraps and can't overflow: a loop's index is below a
length, and `for i in a..b` tests `i == b` before its ADDIK (the loop
ends at b, Int.max too) instead of `i <= b` after it. `a..b` as a value
is `arr.range(a, b, 1)`, which takes b without computing b + 1.
`abs(x)` and an array's `sum()` (lib.c's `math.absi`, `arr.sum`) throw
as the operators do; `addingChecked`, `subtractingChecked` and
`multipliedChecked` are `int.add_checked` and its kin: the result boxed,
or none. Nothing folds int arithmetic at compile time but a negated
literal (`-5` is KSMALL -5); `-0x8000_0000_0000_0000`, the one that
would overflow, is a checker error, as an out-of-range literal is.

A compound assignment (`x <<= 1`, `xs[i] |= m`, `h &*= p`, `n **= 2`)
is its operator on the place's value, written back as `=` is; one a
struct's or an enum's method does (`t += 1h`, check.Info.AssignOps)
is that method's call (the place read, not taken out, for the method may
read it: a state's cell is CPEEKed and put back once, CPUTP). The operators a
struct's or an enum's methods do (check.Info.Operators) are calls of
them: `+` add, `-` sub or minus, `*` mul or times, `/` div or quo, `%`
rem, `**` pow, the one whose parameter's type is the right operand's
(check.findOperator), and `-x` neg (check.Info.Negations); `x *= y` uses
the one that gives x's type back. `<` and its kin
on a struct or an enum call its `compare` and test the int against 0,
in a generic's body too (check.CompareMethod of T's type); an enum
without payloads compares its cases (an int, or ECASE of a reference
one). `sorted()` and `sort()` of such elements pass `compare` as a
closure and 1 (a comparison, not a less-than) to `arr.sorted`. `t.0` is
GETF of the tuple's field 0 (SelTupleElem), and a place as a field is.
`b[i]` of
bytes is CALLB `bytes.at` (an int; out of range throws, as IDX does).
`>>` is two adjacent `>` tokens the parser joins, so types' `>>` close
two generic argument lists. `**` binds tighter than a prefix operator
before it: the parser reads a prefix operator's operand as a power
(`-2 ** 2` is `-(2 ** 2)`), and `**`'s right side as a prefix
expression (`2 ** -1`), right to left.

`x ?? d` jumps over d when x isn't none (JSOME, or JNONE and UNBOX when
the whole isn't optional). `m[k] ?? d` of a map whose values aren't
optional is MGETJ, which reads the value as it is (a scalar unboxed:
MGET would box it) or jumps to d when k is missing.

## Patterns

A pattern is tested (testPattern: jumps to the arm's end unless the
value matches), then bound (bindPattern: declares its names), so an
`if let`, a match's arm and a `catch T.case(…)` share them:

| Pattern | Tested | Bound |
|---|---|---|
| `x`, `_` | nothing | `x` is the value |
| `0`, `"a"`, `true` | EQ with the literal (of an optional: JNONE first, then its value) | |
| `none` | JSOME fails | |
| `(a, b)` | each element (GETF; of an optional: JNONE first, then its value) | each element's names |
| `.case(p, q)`, `T.case(label: p)` | ECASE (an int enum: the value), each payload (GETP) | each payload's names |
| `P(x: 0, y)` (a struct's, check.Info.FieldPats: each argument's field) | each named field's (GETF) | each named field's names |
| `c: Circle` (a TypedPat) | the type test (below); of an optional `T?` of `T`, JNONE | `c` is the value |
| `a \| b` | each in turn | (none) |

In a view's match, each name an arm binds is a memo reading its part of
the subject (extract), so the arm follows it.

## Interfaces: type tests and provided methods

An interface's value is the struct's or the enum's own reference, and
its ak_shape is its dynamic type. A type test (check.Info.TypeTests: a
struct's pattern, a typed name or an enum's case written with its type,
matched with a value of an interface or a type parameter, or a `let c:
Circle = v else`) is a shape comparison: ISSHAPE r, x, s (the tested
type's shape; false for none), JF to the arm's end, then the pattern's
parts read as of that type (GETF, ECASE and GETP) and its name bound to
the same register. An `Error`'s is its `__etype` (ECASE first, as `catch
e: T` tests it): errors keep it, since their ids also cross the FFI and
the web's encoding.

Against a type parameter, it's decided as the instantiation is lowered
(testType's static case): the type it's instantiated with is the tested
one (no test; an optional's none still JNONEs) or never is (a JMP, and
what's after the test is lowered with a register of the tested type's
kind). A match's arms that never match aren't lowered, nor those after
one that always does without a guard (matchArms, after platformArms);
an `if let` that never matches lowers its condition's clauses up to it,
for what they do, then only the else (letNever). An interface
instantiation (`T` = `Shape`) tests with ISSHAPE.

An interface's method with a body is provided (check.IfaceMethod's
Provided, a Function whose Recv is the interface). Its body is checked
with `self` a type parameter of the interface (Iface.Self) and lowered
per type it's called for, as a generic is: funcIndex with the type as
the receiver and the interface's type arguments as targs, `self`'s
register of the type's kind. methodFor picks what a type answers a
method with: its own, unless the method is provided and the type's own
isn't of its signature (or there's none), then the interface's for it.

- `implement`, at a conversion to an interface, fills the shape's
  method table by name, as for any method: CALLI (ak_vm_method) is
  unchanged. A shape has one table, so two interfaces that give a type
  different methods of one name (both provided, or a provided one and
  the type's own of another signature) are an error when the program
  is compiled, naming both (methodFrom says whose a provided one is).
- A call on a type parameter (CallIface) is a CALL of methodFor's
  function for the instantiation's type; on an interface's value, CALLI.
- A type parameter's value converts to an interface it's constrained
  by (a provided method passing `self` on): implement of the
  instantiation's type, an error for an enum without payloads that
  doesn't declare it (an int). When the type argument is itself an
  interface `J` and the call is of a provided method `J` hasn't, `J`
  flows into the method's interface (flow): every type converted to
  `J`, before and after, is implemented for it too, so CALLI finds the
  method on `J`'s values.

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
(`json.encode` with its descriptor): a tuple's is `(` (its elements by
its shape's field descriptors, `(1,"a")`) and a map's from other than
strings `m` K V (each key as it's written, `{1:"a"}`), descriptors only
print writes. `print(x)` prints the same text. A part with a format
(`{x:>8.2f}`) is the value itself, its kind in INTERP's kinds flagged
(0x80) and followed by the format's length and text: INTERP writes it
by the format (rt/lib.c's ak_strb_fmt), no string of its own.

`a + b + c`, a chain of three or more string `+`s however grouped, is
one INTERP of its operands (computed into its block) with no kinds at
all, which means every part is a string: the runtime sums their
lengths and copies each once. `s += x` and `s = s + x + ...`, `s` a
local string var the other parts don't read, append to `s`'s own
register (CONCAT s, s, x), which changes the string in place when `s`
holds it alone; the parts are all computed first.

A multi-line string's text drops its first line when that's blank, and
the indentation of its closing quotes' line from every line.

## Module state and constants

- `state` at the top of a file is a module global holding a cell, made
  in the module's init function (CELLK keyed `package.name#hash`: see
  Hot reload).
- A top-level `let` that reads state is a memo global, made in init; one
  that doesn't is a constant. A constant whose value is a literal number,
  duration, color or bool (`let nsPerSecond = 1_000_000_000`, a negated
  number too) is read as that literal (KINT, KFLOAT, KSMALL), not from
  its slot (GGET): the optimizer and the C compiler see the constant (a
  division by it is a multiply).
- `for c in s` over a string is the array loop over `str.codepoints(s)`
  (check.ForInfo.Chars); `s.chars()` is that array, `s.lazy()` a Seq of
  them; `s + c` CONCATs the char's text.
- A top-level `var` (check.VarGlobal) is a global of its kind, made in
  init (not frozen) and read with GGET, written with GSET; changed in
  place (`log.push(x)`, `cache[k] = v`, a mut fn) by GTAKE (the reference
  moved out, none left), the change, GSET back, with a handler that puts
  it back if the change throws, as a boxed var's VTAKE. A scalar, or a
  change whose code may read the var, is GGET, change, GSET. It isn't
  reactive (no cell), and the checker keeps spawn and workers from it
  (check.touchesVars). On the web's spawn workers, a var a spawn could
  never read is still made when it's a value, so the lets made from it
  are the same there.
- Constants of reference kind (string literals aside, which are already
  in the string table): a struct literal with constant fields, an array
  of constants, a struct's `let` (Font.title), are built once in init
  into a global and frozen (`rt.freeze`, which makes them and what they
  hold immortal, so spawn's threads may share them): top-level lets and
  types' lets.
- A constant literal anywhere else (a function's, a handler's, a
  parameter's default) is hoisted the same way: an array, a set or a map
  of constants (empty too), a tuple, a struct (its omitted fields'
  defaults constant too: `Style()`), a case with a payload, nested as
  deep as they go. Constants are literals: numbers, strings without
  interpolations, bools, none, payloadless cases, colors. `init.strings`
  builds each one into a global of its own (the node and its type: a
  generic function's isn't hoisted), and the function reads it (GGET):
  each call makes nothing. A copy that's changed is copied first, as
  any shared value is (a frozen one is never changed in place).
- Init makes each package's globals, the packages in dependency order
  and each package's globals by what their initializers read (a cycle
  is the checker's error). It first calls `init.strings`, which makes
  the frozen tables of names the lowering asked for (enums' case names).

## Reactivity

Reads, by what's read:

| Read of | In a reactive context (a binding, a memo, an effect) | In a handler or function |
|---|---|---|
| a cell (`state`), whole | CGET: tracked | CGET (the runtime runs handlers untracked) |
| a path below a cell (`user.name`, `todos[i].done`, `cart[k]`, `picked.contains(x)`) | CPATH: tracked, following the path; CLEN for `count()`, `isEmpty()` | CPATH, navigating |
| a follow (a `state` that follows) | CGET: the cell is a follow's, which it brings up to date (a memo's pull), tracked; a path below it, CPATH, read whole | CGET, brought up to date |
| a memo (`let` in a component, a store, a file) | MREAD | MREAD |
| a parameter or prop of a component | AGET of its accessor | AGET |
| a `for` row's item, a `match` arm's payload | AGET | AGET |
| a path below a parameter, a prop, a row's item, a `let` that's a path | AGETP: following the path when the accessor's is one | AGETP |

Writes: `x = e` is CSET. A write into part of a cell's value
(`todos[i].done = true`, `form.name = n`, `todos.push(t)`) takes the
value out (CTAKE), changes it in place (SETF, SETIDX, an inout builtin
on the register), and puts it back with CPUTP and the place's path. The
index and the new value are computed before the CTAKE.

**Paths** (rfcs/0013). A read of state follows its path: the checker
spells an expression's path below a state or an accessor
(check.StatePath: fields, `?.` fields, indexes, `get(i)`, `first()`, map
keys and `get(k)`, `has(k)`, a set's `contains(x)`, `count()`,
`isEmpty()`), and the lowering, from the outside in, reads the longest
one at once: `CPATH a, cell, P, base`, P a string constant spelling the
steps (core/src/vm/path.h: a letter each, a field's index after a
field's, a kind after a key's), their indexes and keys from consecutive
registers from base. An index or a key that calls something stops the
path (it's computed before the cell is read). `m[k] ?? d` and `if let v
= m[k]` read `has` then the value as it's stored (`h`, `v`), both
following k. Tracking, CPATH finds or makes the path's node under the
cell and subscribes to it; untracked, it navigates.

CPUTP's last step says what the change was: `o`, an assignment, its old
value kept (GETF, IDX, MGETJ with whether the key was there, before the
SETF, SETIDX or MSET) and compared, so an equal write notifies no one;
`r`, an inout builtin, which records what it changed (rt.h's
`ak_change`: the first index and whether the length changed, or the
key); `a`, anything below the place (a mut fn's call, a compound change
of a map's entry, the handler of a change that threw). A whole write
(CSET) compares the old and new values at each followed path, skipping
what's the same object.

Collection methods on a path, in a binding, an effect or a file's
initializer, with a closure literal (`count(where)`, `any`, `all`,
`find`, `index`, `filter`, `map`, `sorted(by)`), are loops over the path
(ir/pathloops.go): the length read with CLEN, the closure's body
inlined with its parameter the element's path at the loop's index (a
pathVar), so each of its reads is a CPATH. Its step is `I`, not `i`:
what it reads through the element, along the fields after it, follows a
column, that field path of every element, one node for them all (not a
node per element: `count(t => t.done)` over 10,000 items keeps two
nodes, the length and the dones). What a method returns is read whole
(`find`'s item, `filter`'s kept ones, each by its own path; every
element for `sorted(by)`, a column of the whole elements, then sorted as
the builtin does). A key's function in a `for` over a path reads its
item the same way. Elsewhere they're the builtins.

A **binding** is any expression the UI follows: a prop's value, an
`if`'s condition, a `for`'s items, a `match`'s subject, a `let`. The
compiler makes it a closure of no arguments (a thunk) and passes it to
the runtime, which runs it as a render effect, or as a memo for `let`.
An expression that reads nothing reactive (a literal, a constant, a
local of the enclosing function, arithmetic on those) is evaluated once
and passed as a value instead (`ak_ui_set` rather than `ak_ui_bind`).

`peek(e)` (rfcs/0012) is `UNTRACK`, `e`, `RETRACK n`: the VM keeps the
token `ak_untrack_begin` returned on the coroutine's stack, and RETRACK
ends those its frame made past the first n (n: the peeks still open
there, 0 at a peek's end). A frame records the stack's depth as it's
pushed, and an error unwinding it ends what it began; a handler's
target whose range made an UNTRACK starts with `RETRACK n`, n the peeks
open at the target. cgen keeps them in the function's locals (`U_`),
ended on its unwind path, or, in a function that may suspend, on the
coroutine as the VM does. A peek of what reads nothing reactive is `e`,
and `await` can't be inside one (the checker's). The checker doesn't
count what's under a peek as read: a binding of it alone is a value, a
fn that reads state only under one doesn't make its caller's binding
reactive (spawn still can't call it).

A **follow** (rfcs/0012) is a state whose initializer reads what
changes: a cell (`FOLLOWK`) whose value a memo of the initializer
(core/src/signal's `ak_follow`) computes on its first read, and again
when what it read changed, unless equal. A write (`CSET`, `CTAKE` then
`CPUT`) first brings it up to date (`ak_follow_refresh`), so a write
after an input changed stands, then replaces the value and notifies its
readers (`ak_follow_set`). Its sources stay linked when its last reader
goes; a follow whose initializer ran and read nothing (a caller's
constant) lets go of it, a plain state from then on.

## Components

A component compiles to a function returning a View. Its arguments
(all references) are its header parameters, then its props, then its
events' handlers, in declaration order:

- A parameter or prop is passed as an **accessor**: ACCV of the value
  when the caller's expression is constant, ACCC of a cell or memo when
  it's one read alone, ACCP when it's a path whose indexes and keys read
  nothing that changes (`TodoRow(todos[0])`, `TodoRow(todo)` of a row's
  item: what the callee reads through it follows its paths, AGETP),
  ACCF of a thunk otherwise. A prop the caller
  leaves out is none, and the callee's default applies (evaluated in the
  callee, as an accessor too).
- A prop of type `View` (a slot, `children`) is passed as a builder: a
  closure that builds the view when called, so it's built in the
  callee's owner, where the callee places it, only when it's shown.
- An event's handler is a closure, or none (emit then does nothing).
  An async one (its block awaits) is a task of the component that
  declares it, the receiver, whoever emits it: CALLB `ui.handler(closure,
  owner)` wraps it in a closure that starts it as a task held by owner
  (disposing the owner cancels it) and run under the owner current where
  it's made (its contexts and catcher). owner is what the receiver's env
  keeps (below); outside a component (the app block, a test, a plain
  fn) it's none, and the owner where it's made holds it too.

Its members live in its **env**, a struct of the component's shape:
a field per parameter, prop and event (their accessors, builders and
handlers), per state (its cell), per let (its memo, its loader, or its
value when it reads nothing reactive) and per use. A component that
declares an async handler of another component's event (check's
`Component.HoldsHandlers`) has one more, after its members: the owner
it was made under (CALLB `ui.owner`, first thing in its function), which
holds those handlers' tasks; the closures between it and such a handler
capture the env to reach it. The closures the
component makes reach its members through the env: those made before
the env is whole (its lets' memos) capture a box (VBOX) the env is put
in (VSET) once it's made, the rest the env itself. Its `fn` members are
functions taking the env first. A store is the same, and its value is
its env. In a store's fns,
`self` is the env.

The function's body, in order:

1. `state x = e`: CELLK with the key `Component.x#hash` (the runtime
   adds the instance's path: its place in the tree, the row's key).
   When `e` reads what changes (check's `Member.Follows`): FOLLOWK with
   the same key and a thunk of `e`, which takes the state's current
   value (an optional, none the first time) when `e` reads the state's
   own name (`Member.Self`; a state that doesn't follow binds it to
   none). `state x(a, b) = e`: the dependencies' memos (below), and a
   thunk that reads them, then `e` between UNTRACK and RETRACK. A kept
   value (hot reload) is a write: it stands until what `e` reads
   changes. A file's state is the same, in init.
2. `let y = e`: MEMO of a thunk (or a constant when it reads nothing).
   A `let` whose value is a path (check's `Member.Path`: `let draft =
   form.draft`, made after what it reads) is an ACCP of it instead, no
   memo: reads of it are AGET, reads through it AGETP.
3. `use x: T`: CALLB `ui.use(key)`, the key being T's type name.
   `provide e`: CALLB `ui.provide(key, value)`, on the component's owner,
   before the views are built.
4. `fn` members: closures over the component's cells and memos.
5. `effect { }`: EFFECT of a closure. `task { }`: CALLB `ui.task`, which
   starts the closure as a task when the component mounts.
   `task(a, b) { }` is an effect that reads `a` and `b` and starts the
   task in its own owner, so a re-run cancels the run before.
   `effect(a, b) { B }` is an effect that reads `a` and `b`, then calls
   a closure of `B` untracked (CALLU). Their dependencies, and a
   state's (`state x(a, b)`), are each a memo made with the component
   (MEMO of its thunk; ir's `deps`), which the follower reads (MREAD),
   so it runs again only when the dependency's value changes: a state
   or a let read alone is read directly (CGET, MREAD). The effect's
   closure is named `Component/effect`, which the run-time guard's
   panic says as "Component's effect at file:line".
   `cleanup { B }` (rfcs/0016) is CLEANUP of a closure of B (named
   `…/cleanup`), which registers it with the owner current there
   (`ak_on_cleanup`): in the body, the owner the instance is built
   under, which runs it as it's disposed; in an effect's body, where
   it's reached, the effect's own, which runs it before the effect's
   next run (`ak_sig_owner_clean`) and as it's disposed. The app's and
   a store's are the same. Cleanups run last registered first, after
   what their owner made.
6. The view items: one element is the component's view; several make a
   fragment (CALLB `ui.fragment`, then `ui.add` each).

`screen` is a component whose root is the `Screen` element: the
screen's prop sets and slots go to that element, its view items are the
element's children.

`emit e(args)` calls the handler argument with args when it isn't none
(CALLC; its result register is a reference: a sync handler's none, an
async one's future, which `ui.handler`'s closure returns). In an async
function the emit waits for it (JNONE past, then AWAIT the future), so
its error is the emitter's; elsewhere the future is released and the
task goes on alone, an error it lets out going to its owner's catcher.
Either way the task is the receiver's: cancelling the emitter stops its
wait, not the handler. An event's name read as a value is whether the
caller handles it: ISNONE of the handler, NOT.

A store's constructor is the same function without the views: it
returns its env. Its `task { }` is TASK (a store has no mount), owned by
the owner that made the store, so disposing that owner cancels it.

A prop the component assigns (check's `Member.Assigned`: it has
`event <prop>Change(x: T)`, rfcs/0010) is read through a **model**
(vm/react.c's "Models"), an accessor of the component's: right after
its default, `ui.model(acc, bound)` wraps the caller's accessor, and the
model is the member's register (and its env field) from then on, so
every read, and every child it's passed to, goes through it. The
component's function takes, after its events, one more argument per
such prop: whether the caller binds it (a string when it does, none
when not; a routed screen's are none).

| Source | Lowered |
|---|---|
| `open = x` (or an element's `bind` of `open`) | `ui.model_bound(m)`; bound: `openChange(x)`'s handler called (the caller's bind writes its state, which the model reads); not: `ui.model_set(m, x)`, then the handler, if any |
| reading `open` | AGET of the model: bound, the caller's accessor; not, a follow of the caller's accessor (as a state that follows is): the caller's value until assigned, the caller's again when it changes |

Assigning one allocates nothing beyond a state's write.

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
| the app block | `ui.main_kind(k)` as its function starts, before its views: its main window is its content's (0), its first `Window`'s (1), or none (2), when nothing it can show is a view of the main window's (lang/ir/mainwindow.go: its items, the components they call and what their slots are given; an `if` folded as the app compiles takes its branch) |
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
| the same, xs a path (a state, a parameter, a prop, a row's item, or a path below one, made of what doesn't change) | `ui.for_path(xsAcc, key, closure(xAcc, iAcc){R})`, key the item's fields spelled as a path when k is a field path of x (`key x.id`), else `closure(xAcc, i){k}`: it follows the length and the keys, not the items; a row's item is its element's accessor at its index cell |
| the same, directly in a `List` | `ui.lazy_for(list, …)` (`ui.lazy_for_path`) with the same arguments |
| `match s { .a => A; .b(p) => B }` | `ui.switch(thunk{the index of the arm s matches, -1 for none}, closure(arm){ switch on it: A, or { let p = memo(payload of s); B } })` |

Each builder and row runs under its own owner, which the runtime
disposes when the branch or row goes.

The selector of a match returns the arm (not the case), so literal,
tuple, struct and `|` patterns work as in statements; a match in a view takes
no guards. A `for` without `key` over a set or a range passes a key
function of the item (the element, the number: check's
`ForInfo.KeyItem`); over an array, which is then a constant one (one
that reads what changes needs its key: the checker's error), none, and
the runtime keys rows by their index. Several view items (a component's body, a branch, a
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

**Variants a component declares** (`variant checked = checked`,
rfcs/0010) have a program-wide id each (the lowerer's, from 1). The
component's function, once its one element's view `v` is built,
declares each on it: `ui.variant_declare(v, id, thunk{checked})`. Its
caller's blocks of them, and a style's applied to it (a name resolved
against the component, then the component its element is, outward
in), are layers of the variant tier, made after every other layer of
the caller's:

| Source | Lowered |
|---|---|
| `Checkbox { checked { background: c } }` | `l = ui.layer(v, AK_LAYER_VARIANT, 0)`, `ui.layer_variant(v, l, id)` (on while the declared condition holds), `ui.layer_set`/`ui.layer_bind` of its props (its `style:` first) |
| a style's `checked { … }` under a condition (`if hot { s }`) | the layer's condition is a closure of the condition and `ui.variant_read(v, id)`, through `ui.layer_when` |
| `pressed { … }` in a component's block (its element a container) | a layer of the state tier, made after the component's own |
| `Checkbox as box`, then `box.checked` | `box` is the Ref of the view the component returns (set after it returns); the read is CGET of it, then `ui.variant_read(view, id)`, tracked (false while it holds none) |

The variant tier's layers are made in their components' order: an
inner component's (the one the outer shows) first, then each
component's in its declaration order, a style's before the block's of
the same variant; the core lays the last one on over the others. A
style's block of a variant the component doesn't declare (or one that
isn't `pub`, from another package) makes nothing.

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

A `shader Name(params) { … }` declaration is a component
(lang/check/shaderlang.go): its parameters, its props `animated` and
`children`, and a body of the prelude's `Shader(program)` with
`uniforms` made from the parameters (`colorUniform` a Color's,
`canvas.color_uniform`; `Uniform(name:, value:)` a number's; a bool an
int's 0 or 1, AGSL having no bool uniforms). Its program is the
`bytes` the checker compiles from the declaration's functions: written
as Kage, a statement a line and every name of the shader's renamed (x_
a local, f_ a function, c_ a constant, U_ a uniform), then
internal/shadercode's Compile and Encode (which carry the uniforms'
lane table), a Kage error reported at the line's statement; the
lowering makes the bytes of a string constant (KSTR, CALLB
`str.to_bytes`).

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
| `try await web.eval(js)` | `ui.web_eval_result(v, js)`: AWAIT on the future |
| `field.insert(s)`, `.format(.bold, on)`, `.toggleFormat(.bold)`, `.setLink(url)` | `ui.insert_text`, `ui.format(v, f, on)`, `ui.toggle_format(v, f)`, `ui.set_link` |
| `field.focus()`, `.blur()`, `scroll.scrollTo(x, y, animated: true)` | `ui.focus`, `ui.blur`, `ui.scroll_to` |
| platform/device's `share(text)`, `shareFiles(paths, mime:, uti:, title:, from: ref)`, `haptic(.success)`, `copy(text)`, `openURL(url)`; platform/accessibility's `announce(text)`, `focus(ref)`; platform/webview's `cookies(url)`; platform/appearance's `setColorScheme(s)` | the packages' `native fn`s (below): `device.share`, `device.coreShareFiles` (from: the Ref, its view read by the builtin), `imageedit.coreCapture` (the Ref's view, drawn by the host: a future of the file's path), `device.haptic`, `device.copy`, `device.openURL`, `accessibility.announce`, `accessibility.focus` (nothing while the Ref holds none), `webview.cookies` (a future), `appearance.setColorScheme`; std/markdown's and std/base's own (`markdown.openLink`, `base.say`) are the same builtins, so the standard library needs no platform package |
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
| `i18n.locale()`, `i18n.setLocale(tag)`, `i18n.languages()`, `i18n.isRTL()`, ... (std/i18n) | the package's `native fn`s: `i18n.locale`, `i18n.set_locale`, `i18n.languages`, `i18n.is_rtl`, `i18n.region`, `i18n.format_locale`, `i18n.format_number`, `i18n.format_decimal`, `i18n.format_int`, `i18n.format_date`, `i18n.autonym` |

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
`Loading<T>` whose `.pending`, `.ready(v)`, `.failed(e)`, `.value`,
`.loading` and `.reload()` the prelude declares as builtins (`load.state`,
`load.value`, `load.loading`, `load.reload`); a `match` reads its case
with `load.state`, a case's payload with `load.value` or `load.error`.

The loader holds what it shows (RFC 0014): `.pending` only before the
first answer; a run after that keeps the case it's in until it answers,
and an answer equal to the last (`ak_val_eq`) changes nothing. It has two
signals: the state, value and error, which a `match` and `.value` read
and only an answer that changes them writes; and `busy`, which
`.loading` (`SelLoading`, as `.value` is) reads and a run writes as it
starts and ends. Both are tracked reads; neither follows the other.
What it shows (`.value`) is a cell of its own, made with the first
answer and written (compared, path by path: rfcs/0013) only when what it
shows changes; a match doesn't follow it. A view's `match x { .ready(p)
=> … }` over a load's `let` binds what `p` names to accessors of that
cell (`load.shown`, then ACCP for a tuple's elements) instead of memos:
reads through them are paths, so a `for t in p key t.id` follows its
keys and a row its fields.

`x.optimistic(f)` (RFC 0017) is CALLB `load.optimistic(x, f)`. The
loader keeps a stack of layers, each `f`, its result over what's under
it, the run whose answer takes it out (the run in flight, or the next)
and the task that made it (the running task's future, `ak_vm_task_
current`): a cleanup on that task's owner takes it out when the task
failed (its future isn't done as the owner goes) or ended with no run
in flight or due (`optimistic-unconfirmed`); synchronous code's is
checked from the ready queue. What it shows is the answer with the
layers applied, written once per change: a layer added applies itself
on top, one that goes re-applies those above it, an answer re-applies
all that stay, in the batch that writes it, so nothing flashes. The
checker keeps `f` a value function (`optimistic-impure`: the state it
reads, through what it calls, and what it writes).

`reload f(args)` and `reload f` (RFC 0017) are CALLB
`load.reload_all(key, given, kinds, args…)`, whose result is a future
(a `Task<void>`, which `await` waits for). The key is the lowering's
number for the function the loads call (`check.Reload.Func`: a declared
function, or a store's method, so every instance's loads share it),
from 1 by declaration. A load of such a call says its key from its
thunk, after it computes the arguments and before it starts the task:
`load.key(key, kinds, args…)` (the arguments as the call takes them,
defaults filled, the receiver aside; kinds a digit each). The core keeps
every such loader in the app's registry (control.c), a hash of lists by
key, with its arguments' latest values; `reload_all` runs each one of
the key, whose arguments are `ak_val_eq` to `args` when it's given
them, as `x.reload()` does, in one batch, and its future resolves once
each has answered, failed, or gone. A loader leaves the registry as its
owner disposes it. The checker reports a `reload` of a function no
`load` in the program calls (`reload-not-loaded`, once the program is
whole) and a call's errors as `reload-arguments`.

`let x = load (c₁, c₂, …)` (RFC 0017) is CALLB `ui.load_group(combine,
kind, thunk₁, thunk₂, …)`: a thunk per call, as a single load's (its
own reads, its own key), and `combine`, a closure of the calls' results
that makes the tuple (NEWS). The checker types it `Loading<(A, B, …)>`
and keeps each element a call (`group-not-calls`). The core's loader has
a slot per call, each an effect with its own run, ticket and answer, its
own `again` (`x.reload()` writes all, `reload f(…)` the slots of `f`'s
key): a slot's answer is held until no slot runs, then the loader lands
them in one batch, `combine`d only when one changed. It's `.pending`
until each slot answered once, `.failed` while one's last run failed
(the first in source order), its value kept; `.loading` while a slot
runs. A single load is a loader of one slot, no `combine`.

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
| `window.close()` | `ui.window_close()`: the window the calling code is in, without asking |
| `onCloseRequest(fn() -> bool)` (a body's item) | `ui.on_close_request(closure)` |
| `quit()`, `relaunch()` | `ui.quit()`, `ui.relaunch()`: the `Quit` command to the root |
| `onQuit(e => …)`, `onSecondInstance((args, dir) => …)` (a body's items), the app block's `on quit(e) { }`, `on secondInstance(args, dir) { }` | `ui.on_quit(closure)`, `ui.on_second_instance(closure)`: handlers that go with the current owner; `asuka_quit_request` runs the first with a `QuitEvent` (its `cancel` and `canceled` closures the runtime's), the `SecondInstance` event the second |
| `navigate(href)`, `navigate(X(…))`, `replace(href)` | `ui.navigate(href, mode)` (0 navigate, 1 push, 2 replace); `X(…)` a routed screen (below): its href |
| `router.back()`, `router.dismiss()`, `router.canGoBack()`, `router.href()`, `router.setQuery(q)` (`router` a package of the universe: check.RouterFuncs) | `ui.router_back`, `ui.router_dismiss`, `ui.can_go_back` and `ui.router_href` (both followed: they read the router's address signal, written as its history changes), `ui.set_query` |
| `runAction(.button(…))` | `ui.run_action(action)`: the action's run (its `run` an `async fn()?`), unless it's disabled, under the calling code's owner; an async one's future is left to run, as a handler's is. Actions the runtime runs (menus, alerts, swipes, snackbars) run under the owner of the view or the code that made them (`ak_ui_call_under`) |
| `window.main()`, `window.current()` (`window` a package of the universe, as `router` is: check.WindowFuncs) | `ui.main_window_ref()`: a Ref of the root's view, which window commands take as the main window's (`focus()` on it brings the window forward; nothing in an app without one); `ui.current_window_ref()`: a Ref of the window the calling code is in (its `Window`, else the root's view) |

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

A test with parameters (a property test) is a function of them: its
entry in Module.Tests lists each parameter's name and JSON descriptor
(core/src/rt/json.h), and the runner (core/runner/prop.c) generates
their values from the descriptors, calls the function with them, and
shrinks a failing input. A `bench` is in Module.Tests too, flagged: its
function takes an int, how many times to run what it measures. That's
its `measure { … }` block, or its whole body: lowered in a loop of that
many turns between CALLB `bench.start` and `bench.stop`, which read the
clock and the allocation counters. `keep(x)` is CALLB `keep` (`keep.ref`
for a reference: it retains it), a call the C compiler can't see
through, so a result nothing reads is still computed.

`expectSnapshot(x)` is CALLB `test.snapshot(text, where)` and
`expectInlineSnapshot(x, "…")` is `test.inline_snapshot(text, want,
has, file, line, col)`, text being x itself for a string and otherwise
CALLB `test.show(x, desc)`, desc x's descriptor with `d` for a duration
and `c` for a color where they aren't a struct's or an enum's fields.

With coverage (ir.Options.Cover, lang/ir/cover.go), each statement of
the files it names, and each value that ends a block, a match's arm, a
closure or an `fn … = value`, is preceded by CALLB `cover.hit(i)`, i the
site's counter: the first time a node is lowered it gets one (a generic
function's instantiations share it). Tests' and benches' bodies aren't
counted. Once everything is lowered, the statements of the functions
nothing reached get counters nothing counts, and each if's and match's
arms are said in counters (an arm's first statement's; an else left out
is the if's count less its other arms'): Program.Cover. Without the
option nothing is emitted.

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
is left, and mounts the new one. A key ends with `#` and a hash of the
state's initializer as the formatter prints it (ir's initKey), so a
state whose initializer was edited starts from the new one, and one
only laid out differently keeps its value. A restart
(`asuka_dev_restart`) keeps none.

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

A value where std/json's Value is expected converts to one (check's
toJSONValue): an int, a float, a bool or a string is NEWE of its case,
none `.null`, and anything else CALLB `json.value(v, desc, vdesc)`
(lang/ir/jsonvalue.go): json.c's ak_json_value walks it by its
descriptor as the encoder does and builds the Value directly, each case
what writing it and reading that back as a Value would give (a whole
float `.float`, a u64 or a BigInt past int's range `.float`, NaN and
the infinities `.null`, a type's own toJSON what it gives), its strings
and any Values in it shared. std/template's render converts its data
the same way.

A type's own JSON (check.Own: `fn toJSON() -> J`, `static fn
fromJSON(json: J) throws -> T`) keeps its descriptor (`S<name>;`): its
shape carries the functions (lang/ir/own.go's `#json` and `#fromjson`
methods, their names followed by a NUL and J's descriptor, which the
loader reads as the shape's hooks and `jdesc`), and json.c, meeting a
value of such a shape, writes what its toJSON gives as J (held until
it's written), or reads a J and makes it one by fromJSON (whose error
is the decode's). Its own `equals` and `hash` (`#eq`, `#hash`) are what
ak_val_eq and ak_val_hash call for its values wherever they are (an
array's ==, a map's keys); `==` of two values of the type, known as it
compiles, CALLs equals itself. Its `description` (`#show`) is what
interpolation and print CALL, and what a snapshot shows (show.c). A
type with a compare (check.CompareMethod) has it as `#cmp`, what a
B-tree orders its keys of the type by (core/src/rt/tree.c). The
runtime calls them through `ak_shape_call` (core/src/vm/load.c's): the
function of the module that loaded the shape. A generic type's come
from extensions of its package constrained by Equatable, Hashable and
Comparable only (check.OwnOf merges those an instantiation meets).
std/bigint's BigInt keeps json.c's own case below: no type a toJSON
gives writes a number of any size.

std/bigint's `BigInt` (its descriptor `Sstd/bigint.BigInt;`, as any
struct's) is the runtime's to write: json.c writes it as a number of
all its digits, which print and interpolation show too, and reads one
from such a number or a string of digits, through link.c's
`ak_link_bigint` (std.h's `ak_bigint_json`, in core/src/std/bigint.c).
It's NULL in a build without std/bigint, so lowering a BigInt's
descriptor names one of the package's builtins (`bigint.bigText`): a
program that only decodes one links its C too.

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
- std/decimal's `Decimal` is a struct of two ints to the compiler and the
  codec (`S…;`: its coefficient, its exponent); internal/nativegen alone
  knows it (Kind Decimal), and gives each platform its decimal type, the
  runtimes' `decimal()` reading and writing it without trailing zeros.
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
resolves their types twice: the Asuka type the program sees (`i32` for
an `Int32`, `string` for a `CString`) and its C side (check.CType), kept
in Function.Extern, Field.C, and an extern type's TypeName (ExternType,
its Release).

| Source | Lowered |
|---|---|
| `extern fn f(a: Int32, s: CString) -> Int64` | a function whose body is CALLB `ffi.<package>.f(a, s)` (check.ExternBuiltin), the parameters as they are; C's integers are the integer types of their width and sign (an `Int32` an `i32`, a `UInt64` a `u64`), so the glue's `(int32_t)a.i` and `(int64_t)r` keep a value canonical |
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
method's `native fn` in `struct T`: `<name>.T.f`, the receiver first;
`native(prefix) fn f`: `prefix.f`, a table of its own, which a build
links only when its app calls one of the table's builtins, for what costs
more than the rest of its package: std/crypto's `crypto_pk.verifySignature`). Its
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
encoded as the drawing the platforms' views decode). Any other first
argument, or one the hook rejects, is a compile error at it.

## Streams

`Stream(produce)` is CALLB `stream.new(produce, kind)` (kind: the
values' ak_kind); `for await x in s { … }` is: s into a local; at the
top, CALLB `stream.ready(s)` (a value is queued, and the task isn't
cancelled: most turns, a producer sending ahead), JT past the wait;
else AWAIT CALLB `stream.next(s)` (a bool: whether a value came; it
throws what the producer threw), JF to the end; then CALLB
`stream.value(s)` for x, the body, back to the top; at the end CALLB
`stream.close(s)`, which cancels the producer (core/src/rt/stream.c). A
next while values are queued answers with a settled future the stream
keeps, not a new one. An error leaving the loop (the body's, the
producer's) closes it too, in a handler around the loop that throws the
error on.

## Workers

A worker (RFC 0011) is a value on a thread of its own: a handle on the
main thread, its state on its thread (core/src/rt/worker.c).

- Its state is its **env**, a struct of the worker's shape: a field per
  parameter and `let` (its value) and per `var` (a box, VBOX), made by
  its init function (`W.init`, its parameters its arguments) on its
  thread. Its fns are functions taking the env first, as a store's do;
  in them (and in the init) a `let` or a parameter is GETF of the env, a
  var GETF then VGET, and a change of a var's value goes through its
  box (VSET; VTAKE for a change in place: "Changing values in place").
  The env never changes, only what its boxes hold.
- `W(args)` is CALLB `worker.new(init)`, init a closure capturing the
  arguments (defaults filled in) whose function CALLs `W.init` with
  them: worker.new deep-copies it (as spawn does its closure) and runs
  the copy on the worker's new thread, which keeps the env it returns.
  The worker belongs to the current owner (`ak_on_cleanup`), as a
  store's tasks do.
- `w.f(args)` from outside (check's CallWorker) is CALLB
  `worker.call(w, call, kind)`, call a closure capturing the arguments
  whose one parameter is the env and whose function CALLs `W.f(env,
  args…)`, then AWAIT of the future it returns (a void result's AWAIT
  discards). The call is deep-copied onto the worker's queue and run on
  its thread in order; its result (or error) is deep-copied back, and
  the main thread settles the future (`ak_post`). In `all`, `race` and
  `load`, a worker's call is a task's as any async call is (`taskOf`:
  the handle and the arguments computed first). Inside the worker, `f(…)`
  is a plain CALL with the env.
- A closure's copy (the init's, a call's) and the state share what the
  copy carries once the state keeps it (an argument stored in a var),
  so the copy is let go of on the worker's thread, as the state is;
  what it was copied from holds the module and is let go of on the main
  thread once the work is done (the init's when the thread has ended, a
  call's once its result is delivered).
- Its owner's disposal stops it: the call running finishes, those
  queued fail with the cancellation. Its handle's last release stops it
  once its queue ran. Either way the thread lets go of the env and ends,
  and the scheduler is held until it has (`ak_sched_hold`).
- On the web (core/src/abi/web.c, platform/web/src/workers.js), each
  worker is a Web Worker: worker.new sends its init's closure as a job
  (spawn's encoding, `ak_keep_encode`), which there makes the env
  (`asuka_worker_new`); each call is a job whose function takes it,
  answered as spawn's (`asuka_spawn_done`), a call cancelled before it
  ran with `ANSWER_CANCELLED`. Where there are no threads and no Web
  Workers, a worker runs in place: its env on the main thread, each call
  run as it's made.

The checker keeps what crosses to values (`sendable`, spawn's rule):
a `pub fn`'s parameters and result, and a worker's parameters; its body
and fns read no state, stores or a file's `let` that isn't a value, and
call nothing that does (as spawn's check, at finalize), make no worker
or native object, don't spawn and don't await. Its `extern` handles are
its own: made, used and released on its thread.

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
| `print(x)` | CALLB `print` of x's text (a line) |
| `print(a, b, separator:, terminator:)`, `eprint(…)` | one INTERP of the values' texts and the separators: CALLB `print` (print's default terminator: a line), else CALLB `print.text(fd, text)` with the terminator (eprint's fd 2), which a test's FakeOS captures (lang/ir/print.go) |
| `hashOf(x)` | CALLB `hash(x, kind)`: ak_val_hash, a map's keys' |
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

## B-trees

std/collections' SortedMap and SortedSet keep their entries in a
`BTree<K, V>` (check.Tree), a type only the standard library names: the
runtime's ak_tree (core/src/rt/tree.c), a header (its count, how its
keys are ordered, its root) over nodes that hold their keys, values and
children in one allocation (63 keys at most), a set's tree keys only.
Nodes are counted, shared by the trees copied from one another, and a
change copies the shared ones on its path (path copying), so a copy is
O(1) and a change O(log n). A node's references are one run of its
slots, NULL past what's used, which obj.c's walks (destroying,
copying, freezing) go through as an array's; a tree compares, hashes
and is kept across a reload (keep.c's 'T': its entries in order, read
back appended, not compared) by its entries.

| Source | Lowered |
|---|---|
| `newTree<K, V>()`, `newSet<T>()` (std/collections' native fns) | CALLB `collections.newTree(K's descriptor, V's)`, `collections.newSet(T's)`: an empty tree whose keys are ordered by their descriptor (rt.h's ak_tree_mode: an int's, a u64's, a float's, a string's <, in C; a struct's or an enum's own compare, its shape's #cmp hook, an enum's without one by its cases; a payloadless enum's with one by the shape its descriptor names, found as the tree's made, kept with it) |
| `t.get(key, by)`, `has`, `near`, `nearKey`, `end`, `endKey`, `count` | CALLB `tree.get`... (Methods' "tree"); by, a less-than or none, orders the keys when it's given; `near` and `end` get, last, a frozen empty `[(K, V)]` (Extra "entryProto", fb.emptyConst: made once in init.strings), whose elements' shape they make the entry by |
| `t.put(key, value, by)`, `add`, `take`, `delete` | inout, as an array's push: the tree moved out of its place (a SortedMap's field), changed in place when it's unique, put back |
| `t.entries(from, after, to, by, limit)`, `keys`, `values` | CALLB `tree.range(…, proto, what)`: an array like proto (Extra "proto") of the entries from the first key at or after from (after: past it) before to, limit at most; SortedMap's range and lazy are a Seq of runs of them, each from the last one's key on, twice as long as the one before up to 1024 |

## Lazy sequences

A `Seq<T>` (check.Seq) is the runtime's: each source and step is a
builtin making an ak_seq that holds the one before (core/src/rt/seq.c),
and nothing is computed until something reads it.

| Source | Lowered |
|---|---|
| `xs.lazy()`, a map's, a set's, a string's | CALLB `seq.of_array`, `seq.of_map`, `seq.of_set`, `seq.of_string` |
| `(a..<b).lazy()`, `(a..b).lazy()` | CALLB `seq.range(a, b, through)`: no array (lang/ir/seq.go's lazyRange) |
| `Seq(first:next:)`, `Seq(state:next:)`, `Seq(repeating:count:)`, `Seq(from:by:)` | CALLB `seq.successors`, `seq.unfold` (the state's kind passed), `seq.repeat` (count -1: no end), `seq.from` |
| a step, `s.map(f)`... | CALLB `seq.map(s, f, …)`: a new ak_seq |
| `for x in s { … }` | CALLB `seq.iter(s)` into a local (a cursor, let go of as the loop's scope ends), then at the top `seq.next(it)` (a bool; false at the end, and it raises what a step raised), JF to the end, `seq.value(it)` for x; a type's `iter()` that gives a `Seq` likewise, the value held in a hidden var (check.ForInfo.SeqVar) and `iter()` called once |
| a pipeline the lowering sees, read by a consumer: `(a..<b).lazy()` or `xs.lazy()`, then `map`, `filter`, `compactMap`, `take`, `skip`, `takeWhile`, `skipWhile`, `onEach`, then `reduce`, `sum`, `count`, `toArray`, `first`, `find`, `any` or `all` | a loop (lang/ir/seqfuse.go), no ak_seq: each element through every step, in order, before the next is taken; a take that has its count ends the loop before the source is read again; the closures made once before the loop (their captures held to its end) and called in it, which the optimizer inlines; the consumer's result in a register of its own until the end |

Every method whose result holds a `Seq<X>` (Method.Extra "seqProto")
gets, last, an empty `[X]`: the runtime reads the elements' kind and
shape from it (a tuple's, to make the pairs `enumerated`, `zip` and a
map's source give). A consumer that makes a collection (Extra "proto":
`toArray`, `toSet`, `toMap`) gets an empty one of its result, which it
fills. A read opens a cursor over the chain, a tree of nodes mirroring
it, each holding its current element: an element goes through every
step before the next is pulled, and a node that has its answer (`take`,
`find`, `any`) stops pulling. A step's closure doesn't throw (the
checker says so: it runs later); a consumer's may, and the consumer is a
Rethrows method. Ordering by a key (Needs "ComparableU": `sorted(key:)`,
`min(key:)`, a Seq's too) passes, before U's kind, U's compare method as
a closure, or none when U is ordered as it is; the keys are computed
once each, and sorted with their indexes (stable).

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
Graphics, a Path, a Ref and a BTree (std/collections'). A method's
signature takes a type in parentheses as itself (`(fn(K, K) -> bool)?`),
two or more as a tuple. A few the lowering does itself: `count()`
of an array or a map is LEN, `isEmpty()` of an array, a map, a set, a
string or bytes LEN and a test of 0,
`isNone()` ISNONE, a map's `get(k)` MGET, `enumerated()` a loop making
`(index, element)` tuples (and `for (i, x) in xs.enumerated()` makes
none: it's a loop over the indexes), and an array's `map`, `filter`,
`compactMap`, `reduce`, `count(where:)`, `any`, `all`, `find` and
`index(where:)` loops over it (lang/ir/seqfuse.go), the closure made
once and called in the loop (inlined by the optimizer), the result
appended to with `arr.append` (push without recording a change: no
state holds it; `arr.reserve` makes map's room once, and a map whose
elements are values, not packed, appends with `arr.append_room`: the
room is there and the array is the loop's own, so compiled it's a
store and a count, ops.h's ak_op_append_room_*; a filter's and a
compactMap's, into the array the loop made, append with
`arr.append_own`: compiled, only its room is checked, the array known
to be unique and of values, ak_op_append_own_*). The result is
made as the method's would be (its element type's storage: a map to
`[u8]` packed); compiled, a bool is appended as its byte
(ak_arr_push_bool), an int, a float or a reference by ops.h's
ak_op_append_*, which returns the array rather than taking its
address (the array stays in a register across the loop; a packed one
goes the slow way, ak_arr_push_slow). Each push
reads the array's cap on its own (ak_arr_room): read with len as one
8-byte word right after the last push stored len's 4 bytes, the load
can't be forwarded from that store and waits for it (3× a map's
cycles). Closures passed to
them don't throw and don't await.

## Notes

- A `Length` from a number computed at run time is `length.points` of
  it (a constant one is packed when compiling).
- The runtime reports a user error's message only where the lowering
  asks for it (tests, `e.message`): `ak_error_message` reads a struct's
  `message` field, and user errors are enums; it could call the shape's
  `message` method (VM shapes have it) so that boundaries and the
  app's error reports show them too.
