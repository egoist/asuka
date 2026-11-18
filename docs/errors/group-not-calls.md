# group-not-calls

`load (…)` of what isn't each a call.

An error the compiler reports: the program doesn't build until it's fixed.

## Why

`load (a(), b())` is one load of several calls, each run again only when what its arguments read changes, their answers shown together. A value in it has nothing to run or to wait for.

## How to fix

Group the calls only (`load (api.user(id), api.posts(id))`), and pass the values to them, or beside the load.

## Wrong

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

## Right

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
