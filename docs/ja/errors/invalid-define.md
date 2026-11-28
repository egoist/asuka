# invalid-define

ビルドがその型の値を与えていない `@define`。

コンパイラーが報告するエラーです。直すまでプログラムはビルドできません。

## 理由

`@define("KEY")` は、ビルドがコンパイル時にプログラムへ渡す値です。asuka.toml の `[defines]`、バリアントの `[defines]`、または `--define KEY=VALUE` から来ます。キーは英字、数字、`_` からなる文字列リテラルです。その後ろに書く既定値もリテラルで、ビルドが値を与えないときはそれが使われ、値の型も決めます。ビルドが与える値はその型でなければなりません。

## 直し方

asuka.toml の `[defines]`（または `--define`）でキーに値を与えるか、既定値を書きます：`@define("API_URL", "https://example.com")`。

## 誤った例

```asuka
let server = @define("API_URL")
```

## 正しい例

`app.asuka`

```asuka
let server = @define("API_URL")
```

`asuka.toml`

```toml
[defines]
API_URL = "https://api.example.com"
```
