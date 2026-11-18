# optimistic-unconfirmed

楽観的な変更が、load の確認する実行がないままタスクとともに外れた。

アプリの実行中にコアが報告します。検査するのは開発ビルド（`asuka dev`、`asuka run`、`asuka test`）だけで、コードの場所ごとに 1 回だけ報告されます。リリースビルドでは検査しません。

## 理由

`x.optimistic(f)` は load が再び答えるまで変更を表示します。それを作ったタスクが終わるとき、load が実行中でも実行を求められてもいなければ（`reload` がない）、確認するものは来ません。残せばその後の答えをすべて隠してしまうので外れ、画面は表示した変更を取り消します。

## 直し方

タスクが終わる前に load をもう一度求めます：`await reload f(…)`、または `x.reload()`。

## 誤った例

```asuka
async fn fetchCount() -> int {
  await sleep(10ms)
  return 1
}

async fn save(n: int) {
  await sleep(10ms)
}

component Counter {
  let count = load fetchCount()
  Column {
    Text("{count.value ?? 0}")
    Button("Add") {
      on press {
        count.optimistic(n => n + 1)
        await save(2)
      }
    }
  }
}

test "a change nothing confirms" {
  let app = mount(Counter())
  await app.idle()
  app.tap("Add")
  await app.idle()
  app.expect("1")
}
```

## 正しい例

```asuka
async fn fetchCount() -> int {
  await sleep(10ms)
  return 1
}

async fn save(n: int) {
  await sleep(10ms)
}

component Counter {
  let count = load fetchCount()
  Column {
    Text("{count.value ?? 0}")
    Button("Add") {
      on press {
        count.optimistic(n => n + 1)
        await save(2)
        count.reload()
      }
    }
  }
}

test "a change its load's answer replaces" {
  let app = mount(Counter())
  await app.idle()
  app.tap("Add")
  await app.idle()
  app.expect("1")
}
```
