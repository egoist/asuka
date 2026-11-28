# invalid-define

A `@define` the build gives no value of its type for.

An error the compiler reports: the program doesn't build until it's fixed.

## Why

`@define("KEY")` is a value the build gives the program as it compiles: asuka.toml's `[defines]`, a variant's, or `--define KEY=VALUE`. Its key is a string literal of letters, digits and `_`; a default after it, a literal too, is its value when the build gives none, and says its type, which the build's value must have.

## How to fix

Give the key a value in asuka.toml's `[defines]` (or with `--define`), or write a default: `@define("API_URL", "https://example.com")`.

## Wrong

```asuka
let server = @define("API_URL")
```

## Right

`app.asuka`

```asuka
let server = @define("API_URL")
```

`asuka.toml`

```toml
[defines]
API_URL = "https://api.example.com"
```
