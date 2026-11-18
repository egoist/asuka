# 0016: `cleanup { }`: what undoes an effect's run, or goes with its owner

- **Status**: Built
- **Created**: 2026-11-18
- **Related**: 0012 (what the UI follows: `effect(a, b)`, effect
  writes), 0010 (headless components, whose parts register with their
  parents), lang/LOWERING.md (Components, Refs), core/src/signal
  (`ak_on_cleanup`, `ak_sig_owner_clean`), core/src/vm/react.c
  (`ak_vm_cleanup`), the `CLEANUP` opcode; Solid 2.0's split effect
  (docs.solidjs.com/v2: `createEffect(compute, apply)`, whose apply
  returns its cleanup), Solid's `onCleanup`, React's `useEffect`
  cleanup, Swift's and Go's `defer`

## Summary

An effect that registers something can't unregister it. Nothing in the
language runs code when an effect is about to run again, or when the
component it belongs to goes. A `defer` written in an effect's body
runs at the end of each run, as `defer` does in any block. What people
write instead is a task that holds:

```asuka
task {
  let id = bus.listen("refresh", e => reload())
  defer { bus.unlisten(id) }
  await never()
}
```

That's a coroutine, an owner and a future kept for as long as the
component is shown, to run one line when it goes. It follows only what's
in its parentheses, and readers have to know the `await never()` idiom.
std and sdk contain about twenty of these.

This RFC adds one statement:

```asuka
effect {
  let id = bus.listen("refresh", e => reload())
  cleanup { bus.unlisten(id) }
}
```

- **In an effect's body**, `cleanup { … }` runs before the effect's next
  run and when the effect is disposed. It's Solid 2.0's returned
  cleanup and React's `useEffect` cleanup.
- **In a component's, a store's or the `app`'s body**, it runs when that
  goes. It's Solid's `onCleanup` at the owner's level.
- **Nowhere else.** Handlers, functions, tasks and tests already have
  `defer` for their own ends, and nothing owns what they'd register.

The core already does all of this. `ak_on_cleanup` runs a cleanup
"when the current owner is disposed, or before the current computation
re-runs". The VM's `CLEANUP` opcode is documented as running its
closure "when the current owner is disposed or its effect re-runs", and
cgen compiles it. Today the compiler emits it only to clear a `Ref`. This
RFC gives it syntax and checks.

## Motivation

**`defer` in an effect doesn't undo the run.** Here is what an effect
whose body defers does on the fake host:

```asuka
effect(n) {
  defer { log = log + "d{n} " }
  log = log + "r{n} "
}
```

Pressing a button that increments `n` once shows `log: r0 d0 r1 d1`.
Each run's `defer` runs as the run ends. That's right for `defer`: an
effect's body is a block, and `defer` protects the rest of its block
(LOWERING.md, "Errors"). It is no help to code that registers something
for as long as the run's values hold.

**The hold is everywhere a part registers with its parent.** In std and
sdk, 17 component tasks await nothing but `never()`. They exist only to
run a `defer` when the component goes:

- **std/base** (15): headless parts register with what contains them. A
  disclosure's items register with their accordion, a menu with its
  menubar, and the parts of fields, selects and controls likewise.
- **sdk/tray** (2): the icon, `task(id)`, and the Dock menu each remove
  themselves from the host.

Two more holds await the host before they hold: sdk/hotkey's
registration and sdk/update's window, which asks for the app's info.
Those are really tasks. So are the 15 holds inside `Stream(send => { … })`,
in the fakes of sensors, location, bluetooth and purchase. All of those
stay. The component holds read like this:

```asuka
// std/base/disclosure.asuka
task {
  defer { a.items.remove(i.value) }
  await never()
}

// std/base/menu.asuka
task {
  if let b = ctx.bar { b.add(ctx) }
  defer { if let b = ctx.bar { b.remove(ctx) } }
  await never()
}
```

Each one keeps a task for the component's whole life: its `task_t` and
effect (control.c's `ak_ui_task`), an owner (`ak_vm_task_own`), a
coroutine with its frame, and a future. A menu of twenty items holds
twenty of them. With `cleanup`, each part needs one closure and one slot
in the owner's cleanups.

**A task follows only its parentheses.** `task(a, b) { }` restarts when
`a` or `b` changes. To re-register when anything the registration reads
changes, you have to list it all by hand. `effect { }` follows what it
reads (rfcs/0012) and is checked for writing what it follows. That's the
form a registration wants, once it can say how to undo itself.

**Functions that return their own undo have nowhere to call it.**
`update.onInstalled(f)` returns the `fn()` that stops it, and
`location`'s events return `hold() -> fn()`. In a component, the stop
function either goes in a hold task or is never called.

**Hot reload leaks what effects registered.** `asuka dev` disposes the
app's root on a reload. A task's `defer` runs then. Something an effect
registered stays registered, so after ten saves the handler runs ten
times. With `cleanup`, the root's disposal undoes it.

**Solid 2.0 made this the effect's shape.** Its `createEffect(compute,
apply)` has the same split as Asuka's `effect(a, b) { B }`: the
parentheses are the compute half, and the body, which follows nothing,
is the apply half. Solid's apply returns a function that runs "before
the next apply run or on disposal". `onCleanup` stays for owner-level
teardown and isn't meant for the apply half. Asuka has the split but
nothing for the apply half to leave behind.

## Decisions

| Question | Decision | Why |
|---|---|---|
| Syntax | A statement, `cleanup { … }`, a soft keyword like `effect` and `task` | It reads as what it does, in the place it's set up, beside the values it undoes. A soft keyword takes no name from programs. |
| Not `defer` | `defer` keeps its meaning everywhere | It already means "at the end of this block" inside an effect, consistently. Changing it only in effect bodies would make the same line mean two things. |
| Not a returned closure (Solid's) | No `return () => …` from an effect | An effect's body is statements that return nothing. A return would also have to be the body's last word, so a cleanup couldn't sit beside the line it undoes, under the `if` that made it. |
| Where, in an effect | Anywhere in its body's own blocks (`if`, `for`, `match`, `do`), not in a closure or a `defer` within it | It registers with the effect's owner when it's reached. A cleanup under an `if` undoes what that branch did, and one in a loop runs once per iteration it registered. A closure runs later, under whoever calls it. |
| Where, in a body | As a declaration in a component's, a screen's, a store's or the `app`'s body, beside `task` and `effect` | It runs when that instance goes. Views stay views: a cleanup can't appear inside an `if` in a view. |
| Nowhere else | Not in handlers, `fn`s, tasks, tests, bindings or files | A handler runs once per event, so a cleanup there would pile up one per press on its component's owner. A `fn`'s owner is its caller's. A task has `defer`, which runs as it's cancelled. A file has no owner. |
| When, in an effect | Before each next run, after the change that caused it, and when the effect's owner is disposed | `ak_sig_owner_clean` runs at the start of every effect run, so the core already orders it. |
| When, in a body | When the owner the instance was built under is disposed: an `if` switching branches, a `for` row's key leaving, a `match` changing arms, a screen popped, a sheet closed, a reload | A component has no owner of its own (it's built under its branch's, row's or screen's). Its lifetime is that owner's, so its cleanups are too. |
| Order | Last registered first. What an owner made goes before its own cleanups. | That's `ak_sig_owner_clean`'s order: children (last made first), then cleanups (last registered first). It's `defer`'s order too. |
| What it sees | The values it captures from its run, as any closure does. Reading state reads it as it is now. | Closures capture what's around them, and state is read when the code runs. Before a re-run, what the effect follows has already changed, hence the warning below. |
| Throwing | A cleanup can't throw; it catches | It runs while its owner goes, often as an `if` switches mid-flush, and the cleanups after it must still run. Swift's `defer` can't throw either. |
| Awaiting | A cleanup can't await | It runs at once, as the owner goes. Async teardown is a task's `defer`. |
| Writing state | Allowed, as a task's `defer` writes today (lang/compile/testdata/ui/taskdefers.asuka). In an effect's cleanup, a write of what the effect follows is the effect's (`effect-writes-what-it-follows`). | A cleanup runs at the start of the effect's run, so a write it makes of what the effect follows dirties the effect while it runs, a pass late. |
| Reading state | Untracked | `ak_sig_owner_clean` runs cleanups with no active computation. What a cleanup reads is never what its effect follows. |
| Once, while shown | `effect() { … }`: empty parentheses follow nothing, so it runs once as the component is built, and its cleanups as it goes | React's `useEffect(fn, [])`, Solid 2.0's `onSettled`. A registration usually calls something that reads and writes what it registers with (a list of listeners, the next id). `effect { }` would follow that, and the check above rejects it; `peek` around each call would be the only way out. `effect()` was an error (`follows-nothing`, "an effect that runs once is effect { … } reading nothing, or a task { }"); with cleanups it says something neither of those can. |
| A slot named `cleanup` | In a call's block (an element's, a component's), `cleanup { … }` is still a slot or a variant of that name | Programs that fill one keep working, and a call's block holds no statements. A cleanup written there where no such slot exists is told what a cleanup is. |

## The language

```asuka
// a listener for as long as the component is shown: once
effect() {
  let id = bus.listen("refresh", e => reload())
  cleanup { bus.unlisten(id) }
}

// one per room: leaving the last before joining the next
effect(roomId) {
  let sub = chat.join(roomId)
  cleanup { sub.leave() }
}

// what a function hands back to stop it
effect {
  let stop = update.onInstalled(r => notice = r.version)
  cleanup { stop() }
}

// only when the branch did something
effect {
  if following {
    let id = sensors.listen(.accelerometer, 100ms, s => sample = s)
    cleanup { sensors.unlisten(id) }
  }
}

// a part registering with what contains it (std/base's disclosure)
component AccordionTrigger {
  use a: AccordionContext
  use i: AccordionItemContext
  cleanup { a.items.remove(i.value) }
  …
}
```

The menu from the motivation becomes:

```asuka
effect {
  if let b = ctx.bar {
    b.add(ctx)
    cleanup { b.remove(ctx) }
  }
}
```

The `remove` goes with the `add` it undoes, and it runs only if the
`add` ran.

A task stays the tool for work that awaits, and its `defer` for undoing
that work: sdk/hotkey's registration awaits the host's answer before
it holds, so it stays a task.

### Reading what changed

```asuka
effect(roomId) {
  chat.join(roomId)
  cleanup { chat.leave(roomId) }   // warning: leaves the new room
}
```

When this cleanup runs before a re-run, `roomId` is already the new id.
Solid has the same trap with signals read in a cleanup. Here the
compiler warns (`cleanup-reads-what-it-follows`, below): keep the run's
value in a `let`, and read that.

```asuka
effect(roomId) {
  let id = roomId
  chat.join(id)
  cleanup { chat.leave(id) }
}
```

The `let` isn't needed on disposal (the component going), since nothing
has changed then. It's still the right form, and the warning doesn't
try to tell the two apart.

## Checking

The checker enters a new context, `CtxCleanup`, for a cleanup's block. It
is a closure: its parent is the effect or the body it's in, and its
locals are captured.

- **Placement** (`misplaced-statement`): the enclosing context must be
  `CtxEffect` (the body of `effect { }` or of `effect(…) { }`) and not
  under a `defer`. Otherwise the cleanup must be a declaration item of a
  component, screen, store or app body. Each message says what to write
  instead:
  - in a task: "a task's cleanup is a defer { }: it runs as the task
    ends or is cancelled";
  - in a handler: "a handler runs once per event, and nothing owns what
    it starts";
  - in a `fn`: "a fn runs under its caller's owner, which its body can't
    see: return what undoes it, and call that from the caller's
    cleanup";
  - in a closure, a `defer`, another cleanup, a test: each says so;
  - in a view's `if`: `statement-in-view`.
- **No await** (`await-outside-async`, with the hint "a cleanup runs at
  once, as its owner goes; async teardown is a task's defer").
- **No throw** (`unhandled-error`, with the hint "a cleanup can't throw:
  catch the error (try?, do { } catch { })").
- **Its writes** are its effect's for `effect-writes-what-it-follows`,
  as a local `fn`'s are (`closureUses`'s `CtxLocalFn` case).
- **Its reads** are not its effect's. `leave` makes a closure's reads
  its maker's "conservatively", since a lambda runs where it's passed.
  A cleanup runs untracked, so its reads mustn't mark the effect as
  reading state, or make it follow anything.
- **`cleanup-reads-what-it-follows`**, a new warning code: a cleanup in
  an effect that reads (directly, through a `fn` or through a `let`) a
  member or global the effect follows. Under `effect { }` that's what
  its body reads; under `effect(a, b)`, what its dependencies read. It
  names the state and where the effect reads it, and its fix is the
  `let`. Its page in lang/diag has the summary, the why, the fix, the
  wrong and right programs above, and its zh and ja text.

`cleanup` becomes a soft keyword: an item starting `cleanup {`, except
directly in a call's block, where it stays a slot or a variant of that
name (a component with `prop cleanup: View`, filled as `Card { cleanup
{ … } }`). Elsewhere it's a name. A statement `cleanup { }` that called a
function named `cleanup` with a trailing block is written `cleanup() {
}` now; no program in the repo had one.

- **A store made where its cleanups run**: an effect that makes a store
  (`let s = Session()`) disposes it before its next run, which runs the
  store's body cleanups. What they write, the effect writes: the call
  records the store's body as reached (a `callUse` of its context),
  which `effect-writes-what-it-follows` walks as it walks a call.

## Lowering and the core

`cleanup { B }` is a closure of B, named `Component/cleanup`, then
`CLEANUP` of it: the opcode the `Ref`s already use (ir/uiext.go's
`setRef`).

- **In an effect's body**, it lowers where it's reached, as the
  `effect` closure (`effect { }`) or the `effect body` closure called
  with CALLU (`effect(a, b)`) runs. Both run with the effect's owner
  current: `effect_update` sets `g->owner` to the effect's own, and
  CALLU only untracks. So `ak_vm_cleanup` registers with that owner, and
  `ak_sig_owner_clean` runs it at the start of the next run, or as the
  owner is disposed.
- **In a body**, it's step 5 of a component's function (LOWERING.md,
  "Components"), in source order with `effect` and `task`. The closure
  captures the env, as theirs do, and registers with the owner the
  instance is built under. A component that provides builds under its
  own owner (`ui.scope_begin`), and its cleanups go there, disposed with
  its parent's. A store's cleanup goes to the owner that made the store:
  a component's, or the memo's of the `let` that holds it. A `let` whose
  expression reads what changes (`if big { Room(…) } else { … }`) makes
  its store again, undoing the last one. A store's arguments are
  accessors, so an argument that changes doesn't remake the store, and
  the store follows it. A store made at a file's top level has no owner,
  and its cleanup never runs, as its tasks are never cancelled.
- **cgen** already compiles CLEANUP (`ak_vm_cleanup(R[n].p)`), so
  release builds need nothing new.
- **Errors**: `cleanup_run` already calls `raise_pending`. A run-time
  error in a cleanup (an index out of range) goes where an effect's
  goes: within an effect's run, to its owner's catcher once the run
  returns; on disposal, to the catcher of the code disposing. The
  checker leaves nothing else to throw.

There's no new opcode and nothing new in the wire. The core changes in
two places:

- **An owner stops following before its cleanups run.** `owner_dispose`
  ran an effect's cleanups, then marked it disposed and unlinked what it
  read. A cleanup that wrote what its effect followed (through a closure
  the checker can't see into) ran the dying effect again, which
  registered a cleanup under its disposed owner, which ran at once, and
  so on to the effect-loop panic. The computation is marked disposed and
  unlinked first now; a signal test (core/test/signal_test.c) disposes an
  effect whose cleanup writes what it read.
- A fix the cleanups' tests found, below.

**Found along the way.** Two bugs that already existed, both fixed here:

- A removed `for` row's item, read as its owner was disposed (its
  cleanups, its tasks' `defer`s), read the item now at the row's old
  index: removing 2 from `[1, 2, 3]` ran row 3's name in row 2's
  `defer`. `ak_row_free` disposed the row's owner before it marked its
  accessor gone (`ak_vm_acc_row_gone`), which is what makes it read its
  own last item. It now marks it first.
- A `for` over a path keyed by its item (`for id in ids key id` over a
  state of `[int]`) made a key function whose parameter was an `int`
  register, read as an accessor (AGET): invalid IR, which the VM ran and
  the validator rejected. An accessor variable's register is a
  reference now (ir/fb.go's `makeClosure`).

**Allocation.** A cleanup is one closure per registration, the same
cost as any closure the run makes (its captures are the run's locals),
and one slot in its owner's cleanups. An owner's vector keeps its
capacity across runs (`ak_vec_pop`), so the core allocates nothing to
register and run it. A C test counts that (core/test/ui_test.c): an
effect registering a cleanup, run 1,000 times, allocates nothing in the
core. That's against the hold it replaces, which
allocates at mount a task, an effect, an owner, a coroutine with its
frame and a future, and holds them all.

## Already there

| Piece | Where |
|---|---|
| Cleanups on owners, last registered first, after the owner's children | core/src/signal/owner.c: `ak_on_cleanup`, `ak_sig_owner_clean` |
| An effect cleans its owner before each run | core/src/signal/signal.c: `effect_update` |
| A cleanup registered under a disposed owner runs at once | `ak_on_cleanup` |
| The opcode and its runtime | `CLEANUP` (lang/bytecode/opcodes.go), `ak_vm_cleanup` and `cleanup_run` (core/src/vm/react.c), cgen.go |
| C tests of cleanups and disposal | `go test ./core` |

## Migration

No source has to change. What moved to cleanups:

- **std/base**: its 15 holds. The ones that remove an item by a prop
  (controls, disclosure, select) became a body `cleanup`, which reads
  the prop as the component goes, as the `defer` did. The ones that
  capture `let mine = id()` (menu, misc) became `effect() { let mine =
  id(); cleanup { … } }`: `id()` reads a `Ref`, which its own cleanup
  clears as the component goes, so it has to be captured while the
  component shows, and once, as the task did (an `effect { }` would
  follow the `Ref`, which the task that registers the item doesn't). fields' registration and menu's menubar entry became
  one `effect` each, its `cleanup` beside what it undoes: they now
  follow what they register (a field's name and its check), where the
  task registered once. A new test removes a listbox's item while it
  shows and checks that the arrows skip it; it fails without the
  cleanup.
- **sdk/tray**: the icon's `task(id)` became `effect(id)`. Its `defer`
  read `id` as it ran, which in a restart is the new id: it removed the
  new icon's entry and left the last one's, so `tray.anchor(oldId)`
  still found an icon. The cleanup captures the run's id. A test renames
  an icon and checks both anchors. The Dock menu's hold reads and writes
  `lastNumber` as it registers, which `effect { }` would follow: it's
  an `effect() { }`.
- **examples/services**: the chat screen's "while it shows" hold, an
  `effect()` with its cleanup.
- **docs/device.md**: `notify.present`'s example, `effect(id)` with its
  cleanup (`present` reads and writes the package's state, which
  `effect { }` would follow).
- The holds that await something before they hold (hotkey's
  registration, update's window) and every `Stream(send => …)` stay as
  they are, and docs/language/async.md says that's what `await never()`
  is for.
- **Docs**: docs/concepts.md ("Effects and tasks", "Owners and
  cleanup"), docs/language/components.md (a "Cleanups" section),
  docs/language/async.md, docs/device.md, their zh and ja pages, and
  `asuka guide`'s pages generated from them; the code's page in
  docs/errors.
- **Tools**: lang/syntax/GRAMMAR.md, the printer, the language server's
  completion (in bodies, and in an effect's statements), the VS Code
  grammar, lang/LOWERING.md.

## Alternatives

- **Solid 2.0's returned closure**, `effect { …; return () => … }`. It
  only works as the last statement, so the cleanup can't sit under the
  `if` that made what it undoes. It makes the effect's body a function
  with a result, when everywhere else it's statements. And a `return`
  before it (an early exit) would silently skip the cleanup.
- **`defer` in an effect means "before the next run".** It's the
  smallest change to write and the hardest to read: the same `defer`
  would run at the end of the block in a handler, and an unknown time
  later in an effect, and moving code between the two would change its
  meaning without a word.
- **A function, `onCleanup(() => …)`** (Solid 1's). Callable anywhere,
  and so correct only where an owner is current, which the call site
  doesn't show: in a `fn` it depends on the caller, and in a handler it
  piles up. A statement can be checked where it's written.
- **A trailing clause**, `effect { … } cleanup { … }` (like `do` and
  `catch`). It can't see the run's locals (the id it unregisters), which
  is the whole point.
- **Snapshot what the cleanup reads** as it's registered, so `chat.leave(roomId)`
  leaves the old room. That's correct more often, but it's a closure that
  captures state differently from every other closure, and it's invisible.
  The warning says it where it's written instead.
- **Keep the hold, document it.** It costs a task per part, follows
  only its parentheses, and asks readers to know an idiom whose words
  ("never") say the opposite of what the code is for.

## Not in this RFC

- **Cleanups in a view's branches** (`if open { cleanup { … } … }`).
  Views stay views; the component inside the branch has a body for it.
- **`onSettled`** (Solid 2.0's setup after the first render, which also
  returns a cleanup) is `effect() { }` with its `cleanup`: user effects
  already run once the component is built and its batch ends.
- **Cleanups for native objects** that a package hands out. A native
  interface's object is released by the runtime when nothing holds it;
  what it registers with the platform is the package's to undo, which
  this RFC gives it the words for.

## Milestones

1. **The checker and the lowering.** The soft keyword, the printer,
   `CtxCleanup`, placement, no await and no throw, writes counted to the
   effect and reads not. Lowering in effect bodies and component, store
   and app bodies. Fake-host tests (lang/compile/testdata/ui/cleanup.asuka):
   - before a re-run, the previous run's cleanup runs before the new
     run's body;
   - a cleanup under an `if` in the body runs only for runs that reached
     it;
   - several cleanups run last first;
   - an effect's and a body's cleanups run when an `if` switches, a row
     leaves, a `match` changes arm, a screen pops and a sheet closes;
   - a store's cleanup runs when its `let` makes it again, and not when
     an argument changes;
   - a body cleanup reads its props and state as the component goes;
   - an allocation count for a re-running effect.

   Errors tests for each misplacement.
2. **The warning.** `cleanup-reads-what-it-follows` in lang/diag, with
   its programs and its zh and ja text, then `go generate ./lang/diag`
   and `./cmd/asuka`.
3. **Migration.** std/base's and sdk's holds, with their tests and the
   view tests that drive them (`go test ./platform/macos -run
   StdSwift` and the like for std/base's parts).
4. **Docs.** The pages above, in all three languages, and the tools'
   keyword lists.
