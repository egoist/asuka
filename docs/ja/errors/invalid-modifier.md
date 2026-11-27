# invalid-modifier

当てはまらない場所にある `pub`、`mut`、`async`、`throws`、型パラメーター。

コンパイラーが報告するエラーです。直すまでプログラムはビルドできません。

## 理由

`mut fn` は呼び出し対象の値を変えるので、ストアの fn（もともとストアの状態を変えられる）や static fn（値がない）には付けず、`async` にもできません。ワーカーの fn は待たず、C の関数はジェネリック、async、throw のいずれにもできず、テストとプレビューは `pub` にできません。

## 直し方

メッセージが示す修飾子を外します。

## 誤った例

```asuka
store Counter {
  state n = 0
  pub mut fn bump() { n += 1 }
}
```

## 正しい例

```asuka
store Counter {
  state n = 0
  pub fn bump() { n += 1 }
}
```
