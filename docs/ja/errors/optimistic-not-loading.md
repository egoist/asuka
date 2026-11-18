# optimistic-not-loading

load でないものに `optimistic` を呼んだ。

コンパイラーが報告するエラーです。直すまでプログラムはビルドできません。

## 理由

`x.optimistic(f)` は、load の次の答えが置き換えるまで、load が表示するものへの変更を表示します。信頼できる元データの上に重ねる層です。状態はそれ自体が元データで、変更を置き換えに来るものはありません。

## 直し方

状態そのものを変える（`notes.push(n)`）か、load の `let` に `optimistic` を呼びます。

## 誤った例

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

## 正しい例

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
