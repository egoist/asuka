# group-not-calls

`load (…)` 里的元素不都是调用。

编译器报告的错误：修正之前程序无法构建。

## 原因

`load (a(), b())` 是由几个调用组成的一个 load：每个调用只在它的参数所读取的内容变化时重新运行，它们的结果一起显示。其中的一个值既没有东西可运行，也没有东西可等待。

## 如何修正

只把调用放进组里（`load (api.user(id), api.posts(id))`），把值传给这些调用，或者放在 load 旁边。

## 错误示例

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

## 正确示例

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
