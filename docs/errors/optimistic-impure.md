# optimistic-impure

An optimistic change that reads state, awaits or writes.

An error the compiler reports: the program doesn't build until it's fixed.

## Why

An optimistic change runs again whenever what's under it changes: the load's next answer, a change below it that goes. It must give the same change each time, from what it's given: a function of the value shown, as a binding is. What it reads of state may be different by then, and what it writes would be written again.

## How to fix

Read what it needs before, into a `let`, and use that in the closure; write in the handler, not in the change.

## Wrong

```asuka
struct Todo {
  id: int
  done: bool
}

async fn fetchTodos() -> [Todo] {
  await sleep(1ms)
  return [Todo(id: 1, done: false)]
}

fn marked(t: Todo) -> Todo {
  var c = t
  c.done = true
  return c
}

component Todos {
  state picked = 1
  let todos = load fetchTodos()
  Button("Done") {
    on press {
      todos.optimistic(ts => ts.map(t => if t.id == picked { marked(t) } else { t }))
      todos.reload()
    }
  }
}
```

## Right

```asuka
struct Todo {
  id: int
  done: bool
}

async fn fetchTodos() -> [Todo] {
  await sleep(1ms)
  return [Todo(id: 1, done: false)]
}

fn marked(t: Todo) -> Todo {
  var c = t
  c.done = true
  return c
}

component Todos {
  state picked = 1
  let todos = load fetchTodos()
  Button("Done") {
    on press {
      let id = picked
      todos.optimistic(ts => ts.map(t => if t.id == id { marked(t) } else { t }))
      todos.reload()
    }
  }
}
```
