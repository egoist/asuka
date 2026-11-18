# optimistic-not-loading

对不是 load 的东西调用了 `optimistic`。

编译器报告的错误：修正之前程序无法构建。

## 原因

`x.optimistic(f)` 会在 load 的下一个结果到来之前，显示对 load 所显示内容的一个改动：它是叠在数据源之上的一层。状态本身就是自己的数据源，没有什么会来替换它的改动。

## 如何修正

直接修改状态（`notes.push(n)`），或者对 load 的 `let` 调用 `optimistic`。

## 错误示例

```asuka
struct Note {
  title: string
}

component Notes {
  state notes: [Note] = []
  Column {
    for n in notes key n.title {
      Text(n.title)
    }
    Button("Add") {
      on press { notes.optimistic(ns => ns + [Note(title: "new")]) }
    }
  }
}
```

## 正确示例

```asuka
struct Note {
  title: string
}

component Notes {
  state notes: [Note] = []
  Column {
    for n in notes key n.title {
      Text(n.title)
    }
    Button("Add") {
      on press { notes.push(Note(title: "new")) }
    }
  }
}
```
