# follows-nothing

`state x()` 没有可跟随的东西。

编译器报告的错误：修正之前程序无法构建。

## 原因

`state x(a, b)` 的括号说明这个状态要跟随什么。括号是空的就什么也不跟随，而这正是只读一次的值（`peek`）已经表达的意思。

## 如何修正

写出要跟随的东西（`state x(a) = …`），或者写成 `state x = peek(…)`。

## 错误示例

```asuka
component Counter {
  state n() = 0
  Text("{n}")
}
```

## 正确示例

```asuka
component Counter {
  state n = 0
  Text("{n}")
}
```
