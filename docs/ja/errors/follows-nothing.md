# follows-nothing

追従するもののない `state x()`。

コンパイラーが報告するエラーです。直すまでプログラムはビルドできません。

## 理由

`state x(a, b)` の括弧は、その状態が何に追従するかを示します。空なら何にも追従しませんが、それは 1 回だけ読む値（`peek`）ですでに表せます。

## 直し方

追従するものを書く（`state x(a) = …`）か、`state x = peek(…)` と書きます。

## 誤った例

```asuka
component Counter {
  state n() = 0
  Text("{n}")
}
```

## 正しい例

```asuka
component Counter {
  state n = 0
  Text("{n}")
}
```
