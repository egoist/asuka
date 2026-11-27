# invalid-modifier

在不适用的地方写了 `pub`、`mut`、`async`、`throws` 或类型参数。

编译器报告的错误：修正之前程序无法构建。

## 原因

`mut fn` 修改的是调用它的那个值，所以 store 的 fn（它们本来就能修改 store 的状态）和 static fn（没有值）都不需要它，它也不能是 `async`。worker 的 fn 不等待，C 函数不能是泛型、async 或会抛错的，测试和预览也不能是 `pub`。

## 如何修正

去掉消息里指出的那个修饰符。

## 错误示例

```asuka
store Counter {
  state n = 0
  pub mut fn bump() { n += 1 }
}
```

## 正确示例

```asuka
store Counter {
  state n = 0
  pub fn bump() { n += 1 }
}
```
