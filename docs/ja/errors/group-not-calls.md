# group-not-calls

`load (…)` の要素が、どれも呼び出しというわけではない。

コンパイラーが報告するエラーです。直すまでプログラムはビルドできません。

## 理由

`load (a(), b())` は複数の呼び出しからなる 1 つの load です。それぞれ、引数が読むものが変わったときだけ再び実行され、答えはまとめて表示されます。値には、実行するものも待つものもありません。

## 直し方

呼び出しだけをまとめ（`load (api.user(id), api.posts(id))`）、値はそれらの呼び出しに渡すか、load の横に置きます。

## 誤った例

```asuka
async fn fetchUser(id: int) -> string {
  await sleep(1ms)
  return "user {id}"
}

component Profile(id: int) {
  let page = load (fetchUser(id), id)
  Text("{page.loading}")
}
```

## 正しい例

```asuka
async fn fetchUser(id: int) -> string {
  await sleep(1ms)
  return "user {id}"
}

async fn fetchPosts(id: int) -> [string] {
  await sleep(1ms)
  return []
}

component Profile(id: int) {
  let page = load (fetchUser(id), fetchPosts(id))
  Text("{page.loading}")
}
```
