package diag

// The run time's codes: what the core reports as an app runs. effect-loop
// and write-in-memo are panics, in every build (a release build's crash
// report says which); the rest are the development builds' detectors
// (ak_diag_enabled: the player, asuka run, asuka test), reported once per
// site per run. Each one's Wrong is a program whose test block makes the
// core report it on the fake host, and its Right one that doesn't (go
// test ./lang/diag).
var (
	EffectLoop = def(Code{
		Name:    "effect-loop",
		RunTime: true,
		Panic:   true,
		Summary: "effects that keep dirtying each other in one flush",
		Why: `An effect that writes what it, or another effect, reads runs again. When
the writes never settle, the flush would never end: after 100 passes the
core stops it, naming the effect, its line, and the state it wrote last.
The compiler reports the loops it can see (effect-writes-what-it-follows);
this is one it can't, a write through a function value.`,
		Fix: `Give the value one source: compute it with a ` + "`let`" + ` or a state that follows,
or write it in the handler that changes what the effect reads.`,
		Wrong: `store Looper {
  state runs = 0
  state step: fn() = () => {}

  effect {
    let _ = runs
    step()
  }
}

test "a loop" {
  let l = Looper()
  l.step = () => { l.runs += 1 }
}
`,
		Right: `store Looper {
  state runs = 0
  state step: fn() = () => {}

  effect(step) {
    step()
  }
}

test "no loop" {
  let l = Looper()
  l.step = () => { l.runs += 1 }
  expect(l.runs == 1)
}
`,
	})

	WriteInMemo = def(Code{
		Name:    "write-in-memo",
		RunTime: true,
		Panic:   true,
		Summary: "state written while a `let` is computed",
		Why: `A ` + "`let`" + ` is computed when it's read, as often as what it reads changes; a
write there would change what others read in the middle of reading it.
The compiler reports the writes it sees (write-in-binding); this one came
through a function value.`,
		Fix: "Write state in a handler, a fn, a task or an effect; a `let` only reads.",
		Wrong: `store Counter {
  state n = 0
  state bump: fn() -> int = () => 0
  let shown = bump()
}

test "a write in a let" {
  let c = Counter()
  c.bump = () => {
    c.n += 1
    return c.n
  }
  print(c.shown)
}
`,
		Right: `store Counter {
  state n = 0
  let shown = n + 1
}

test "a let that reads" {
  let c = Counter()
  c.n += 1
  expect(c.shown == 2)
}
`,
	})

	LoadWaterfall = def(Code{
		Name:    "load-waterfall",
		RunTime: true,
		Summary: "a load that waited for another one it doesn't use",
		Why: `A load in content that appeared because another load answered starts
only after it. When its arguments don't read that answer, it could have
run at the same time, and the screen waits for the two one after the
other.`,
		Fix: "Start both loads in the same component, side by side, and pass the results down.",
		Wrong: `async fn fetchUser() -> string {
  await sleep(300ms)
  return "Ana"
}

async fn fetchPosts() -> [string] {
  await sleep(300ms)
  return ["Hello"]
}

component Posts {
  let posts = load fetchPosts()
  Text("{(posts.value ?? []).count()} posts")
}

component Profile {
  let user = load fetchUser()
  match user {
    .pending => Text("…")
    .failed(e) => Text(e.message)
    .ready(name) => Column {
      Text(name)
      Posts()
    }
  }
}

test "one after the other" {
  let app = mount(Profile())
  await app.idle()
  app.expect("1 posts")
}
`,
		Right: `async fn fetchUser() -> string {
  await sleep(300ms)
  return "Ana"
}

async fn fetchPosts() -> [string] {
  await sleep(300ms)
  return ["Hello"]
}

component Profile {
  let user = load fetchUser()
  let posts = load fetchPosts()
  match user {
    .pending => Text("…")
    .failed(e) => Text(e.message)
    .ready(name) => Column {
      Text(name)
      Text("{(posts.value ?? []).count()} posts")
    }
  }
}

test "side by side" {
  let app = mount(Profile())
  await app.idle()
  app.expect("1 posts")
}
`,
	})

	LongHold = def(Code{
		Name:    "long-hold",
		RunTime: true,
		Summary: "a load has held old content for seconds with nothing saying so",
		Why: `A load that runs again keeps what it showed until the new answer lands
(its ` + "`.ready`" + ` stays), and a grouped load's call that answered holds its
answer until its slower siblings do. That keeps the screen still, but
when the answer takes seconds and nothing reads ` + "`.loading`" + `, the user
looks at the last answer with no sign that another is coming.`,
		Fix: "Read `x.loading` while it holds (dim the content, show a ring), or show a spinner in its place.",
		Wrong: `async fn fetchName(id: int) -> string {
  await sleep(3s)
  return "user {id}"
}

component Profile {
  state id = 1
  let user = load fetchName(id)
  Column {
    match user {
      .pending => Text("…")
      .failed(e) => Text(e.message)
      .ready(name) => Text(name)
    }
    Button("Next") { on press { id += 1 } }
  }
}

test "a long hold" {
  let app = mount(Profile())
  await app.idle()
  app.expect("user 1")
  app.tap("Next")
  await app.idle()
  app.expect("user 2")
}
`,
		Right: `async fn fetchName(id: int) -> string {
  await sleep(3s)
  return "user {id}"
}

component Profile {
  state id = 1
  let user = load fetchName(id)
  Column {
    match user {
      .pending => Text("…")
      .failed(e) => Text(e.message)
      .ready(name) => Text(name) { opacity: if user.loading { 0.5 } else { 1 } }
    }
    Button("Next") { on press { id += 1 } }
  }
}

test "a hold that says so" {
  let app = mount(Profile())
  await app.idle()
  app.expect("user 1")
  app.tap("Next")
  await app.idle()
  app.expect("user 2")
}
`,
	})

	WideFanout = def(Code{
		Name:    "wide-fanout",
		RunTime: true,
		Summary: "one write ran more than 1,000 bindings and lets, more than once",
		Why: `A write runs again what read the state it wrote. A list whose rows each
read the whole array runs every row again when one item changes: one
row's change costs the whole list, every time.`,
		Fix: "Read what a row shows by its path (`todos[i].done`, `for t in todos key t.id`), or a set's `contains`, so a write runs only what reads it.",
		Wrong: `component Grid {
  state picked = 0
  Column {
    for i in 0..<1200 {
      Text(if picked == i { "picked {i}" } else { "{i}" })
    }
    Button("Pick") { on press { picked += 1 } }
  }
}

test "every row runs again" {
  let app = mount(Grid())
  app.tap("Pick")
  app.tap("Pick")
  app.expect("picked 2")
}
`,
		Right: `component Cell(i: int, picked: bool) {
  Text(if picked { "picked {i}" } else { "{i}" })
}

component Grid {
  state picked: Set<int> = []
  Column {
    for i in 0..<1200 {
      Cell(i, picked.contains(i))
    }
    Button("Pick") { on press { picked = [picked.count() + 1] } }
  }
}

test "only the rows that change" {
  let app = mount(Grid())
  app.tap("Pick")
  app.tap("Pick")
  app.expect("picked 2")
}
`,
	})

	EffectInEffectPass = def(Code{
		Name:    "effect-in-effect-pass",
		RunTime: true,
		Summary: "an effect's write made a binding run twice in one flush",
		Why: `Bindings run before effects in a flush. An effect that writes what a
binding reads makes the binding run again in the same flush: the screen
was laid out once with the value before, for nothing.`,
		Fix: "Compute the value with a `let`, or a state that follows what the effect read: it settles in one pass.",
		Wrong: `component Label {
  state name = "ana"
  state shown = ""
  effect { shown = name.upper() }
  Column {
    Text(shown)
    Button("Rename") { on press { name = "bo" } }
  }
}

test "a pass late" {
  let app = mount(Label())
  app.tap("Rename")
  app.expect("BO")
}
`,
		Right: `component Label {
  state name = "ana"
  let shown = name.upper()
  Column {
    Text(shown)
    Button("Rename") { on press { name = "bo" } }
  }
}

test "in one pass" {
  let app = mount(Label())
  app.tap("Rename")
  app.expect("BO")
}
`,
	})

	OptimisticUnconfirmed = def(Code{
		Name:    "optimistic-unconfirmed",
		RunTime: true,
		Summary: "an optimistic change went with its task, with no run of its load to confirm it",
		Why: `` + "`x.optimistic(f)`" + ` shows a change until the load answers again. When the task
that made it ends and the load isn't running or asked to (no
` + "`reload`" + `), nothing will ever confirm it: keeping it would hide every later
answer, so it goes, and the screen takes back what it showed.`,
		Fix: "Before the task ends, ask the load again: `await reload f(…)`, or `x.reload()`.",
		Wrong: `async fn fetchCount() -> int {
  await sleep(10ms)
  return 1
}

async fn save(n: int) {
  await sleep(10ms)
}

component Counter {
  let count = load fetchCount()
  Column {
    Text("{count.value ?? 0}")
    Button("Add") {
      on press {
        count.optimistic(n => n + 1)
        await save(2)
      }
    }
  }
}

test "a change nothing confirms" {
  let app = mount(Counter())
  await app.idle()
  app.tap("Add")
  await app.idle()
  app.expect("1")
}
`,
		Right: `async fn fetchCount() -> int {
  await sleep(10ms)
  return 1
}

async fn save(n: int) {
  await sleep(10ms)
}

component Counter {
  let count = load fetchCount()
  Column {
    Text("{count.value ?? 0}")
    Button("Add") {
      on press {
        count.optimistic(n => n + 1)
        await save(2)
        count.reload()
      }
    }
  }
}

test "a change its load's answer replaces" {
  let app = mount(Counter())
  await app.idle()
  app.tap("Add")
  await app.idle()
  app.expect("1")
}
`,
	})

	OptimisticNotReady = def(Code{
		Name:    "optimistic-not-ready",
		RunTime: true,
		Summary: "`optimistic` on a load that isn't ready",
		Why: `An optimistic change is a function of what a load shows. Pending (before
its first answer) or failed, it shows nothing to change: the call does
nothing.`,
		Fix: "Change what it shows once it's `.ready`: offer the action in the `.ready` arm, or check `x.value` first.",
		Wrong: `async fn fetchCount() -> int {
  await sleep(10ms)
  return 1
}

component Counter {
  let count = load fetchCount()
  Column {
    Text("{count.value ?? 0}")
    Button("Add") {
      on press {
        count.optimistic(n => n + 1)
        count.reload()
      }
    }
  }
}

test "a change before the answer" {
  let app = mount(Counter())
  app.tap("Add")
  await app.idle()
  app.expect("1")
}
`,
		Right: `async fn fetchCount() -> int {
  await sleep(10ms)
  return 1
}

component Counter {
  let count = load fetchCount()
  Column {
    match count {
      .pending => Text("…")
      .failed(e) => Text(e.message)
      .ready(n) => Button("{n}") {
        on press {
          count.optimistic(c => c + 1)
          count.reload()
        }
      }
    }
  }
}

test "a change of what it shows" {
  let app = mount(Counter())
  await app.idle()
  app.tap("1")
  await app.idle()
  app.expect("1")
}
`,
	})
)
