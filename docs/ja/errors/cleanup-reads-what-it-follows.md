# cleanup-reads-what-it-follows

エフェクトが追従するものを読むエフェクトの cleanup。

コンパイラーが報告する警告です。プログラムはビルドできますが、確認する価値があります。

## 理由

エフェクトの `cleanup { }` は次の実行の前に走ります。次の実行が起きるのは、追従しているものが変わったからです。それを cleanup で読むと新しい値が返ります。`cleanup { leave(room) }` が抜けるのは、この実行で入った部屋ではなく、これから入る部屋です。

## 直し方

この実行の値を `let` に取っておき、cleanup ではそれを読みます。`let r = room` としてから `cleanup { leave(r) }` と書きます。

## 誤った例

```asuka
component Room {
  prop room = "lobby"
  effect(room) {
    print("join {room}")
    cleanup { print("leave {room}") }
  }
  Text(room)
}
```

## 正しい例

```asuka
component Room {
  prop room = "lobby"
  effect(room) {
    let r = room
    print("join {r}")
    cleanup { print("leave {r}") }
  }
  Text(room)
}
```
