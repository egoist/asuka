# cleanup-reads-what-it-follows

effect 的 cleanup 读取了 effect 所跟随的东西。

编译器报告的警告：程序仍能构建，但值得看一看。

## 原因

effect 里的 `cleanup { }` 会在 effect 下一次运行之前执行，而 effect 之所以再次运行，正是因为它跟随的东西变了。在 cleanup 里读取它，读到的是新值：`cleanup { leave(room) }` 离开的是正要加入的房间，而不是这次运行加入的那个。

## 如何修正

把这次运行的值存进一个 `let`，在 cleanup 里读它：先 `let r = room`，再 `cleanup { leave(r) }`。

## 错误示例

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

## 正确示例

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
