# invalid-define

构建没有给出对应类型的值的 `@define`。

编译器报告的错误：修正之前程序无法构建。

## 原因

`@define("KEY")` 是构建在编译时交给程序的值：来自 asuka.toml 的 `[defines]`、某个变体的 `[defines]`，或者 `--define KEY=VALUE`。键是由字母、数字和 `_` 组成的字符串字面量；后面可以跟一个默认值，同样是字面量，构建没有给出值时就用它，它也决定了这个值的类型，构建给出的值必须是这个类型。

## 如何修正

在 asuka.toml 的 `[defines]` 里（或者用 `--define`）给这个键一个值，或者写上默认值：`@define("API_URL", "https://example.com")`。

## 错误示例

```asuka
let server = @define("API_URL")
```

## 正确示例

`app.asuka`

```asuka
let server = @define("API_URL")
```

`asuka.toml`

```toml
[defines]
API_URL = "https://api.example.com"
```
