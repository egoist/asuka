# optimistic-impure

状態を読む、await する、または書き込む楽観的な変更。

コンパイラーが報告するエラーです。直すまでプログラムはビルドできません。

## 理由

楽観的な変更は、その下にあるものが変わるたびに（load の次の答え、下の層が外れたとき）再び実行されます。毎回、渡された値から同じ変更を返す必要があります。バインディングと同じく、表示する値の関数です。読む状態はそのときには変わっているかもしれず、書き込みはもう一度行われてしまいます。

## 直し方

必要なものは先に `let` に読み込み、それをクロージャで使います。書き込みは変更の中ではなく、ハンドラーで行います。

## 誤った例

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

## 正しい例

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
