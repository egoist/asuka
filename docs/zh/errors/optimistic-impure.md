# optimistic-impure

乐观改动读取了状态、await 了或者写入了。

编译器报告的错误：修正之前程序无法构建。

## 原因

只要它下面的内容变化（load 的下一个结果、它下面某一层的撤销），乐观改动就会重新运行。它每次都必须根据传给它的值给出同样的改动：像绑定一样，是所显示值的函数。到那时它读到的状态可能已经不同，而它的写入会被再写一次。

## 如何修正

先把需要的东西读进一个 `let`，在闭包里用它；写入放在处理函数里，不要放在改动里。

## 错误示例

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

## 正确示例

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
