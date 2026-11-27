# 基本概念

Asuka のアプリは、何を表示するかを宣言するコンポーネントと、何が変わるかを表す状態でできています。このページでは、これらがどう組み合わさるかを説明します。具体的には、アプリ、コンポーネントとその状態、状態が変わったときに何が更新されるか、エフェクトとタスク、ビューの表示の切り替えと繰り返し、オーナーとクリーンアップ、メインスレッド、そして環境です。構文のリファレンスは [Asuka 言語](language-tour.md)を参照してください。

## アプリ

`app { … }` はアプリのルートです。

```asuka
component Greeting(name: string) {
  Text("Hello, {name}")
}

app {
  SafeArea {
    padding: 16
    gap: 12
    Text("Hello") { font: .largeTitle }
    Greeting("Ada")
  }
}
```

ルートは、スマートフォンではノッチやホームインジケーターの下まで画面全体に広がり、デスクトップではウィンドウ全体に広がります。`SafeArea` を使うと、それらに覆われる部分やキーボードを避けてコンテンツを配置できます（[レイアウトとスタイル](layout.md#セーフエリアキーボードウィンドウ)を参照）。

アプリの起動のしかたはプラットフォームによって異なります。

- **iOS と Android**：アプリを起動するのはプラットフォームで、アプリがルートをマウントします。
- **macOS、Windows、Linux**：アプリがメインウィンドウを開いてそこにルートをマウントし、終了するまで動き続けます。
- **Web**：ページがアプリを読み込み、アプリがルートをページにマウントします。

`asuka dev` では、保存した変更は数ミリ秒で動いているアプリに反映され、アプリの状態もビューもそのまま残ります。

## コンポーネント

コンポーネントは UI の部品です。パラメーターはヘッダーで受け取り、プロパティ、イベント、状態、そしてビューは本体で宣言します。

```asuka
component Counter(label: string) {
  prop step = 1
  event changed(count: int)
  state count = 0

  Row {
    gap: 12
    Text("{label}: {count}")
    Button("Add") {
      on press {
        count += step
        emit changed(count)
      }
    }
  }
}
```

コンポーネントの本体は**宣言**であり、一度実行したら忘れられるコードではありません。`Text("{label}: {count}")` は、テキストが今どうなっていて、この先もどうなるかを表しています。うっかり `count` を一度だけ読んでしまい、更新されない値を表示してしまう、ということは起こりえません。

## 状態

`state` は、UI が追従する値です。

```asuka
state count = 0
state draft = ""
state todos: [Todo] = []
```

書き込む（`count += 1`、`todos.push(t)`、`todos[0].done = true`）と、それを読んでいるものが更新されます。更新されるのはそれだけです。今と同じ値を書き込んでも、何も起きません。

値が変わるものを読んでいる状態は、それに追従します。`state draft = title` は書き込まれるまでは `title` で、`title` が変わるとまた `title` に戻ります。`state draft = peek(title)` は `title` を一度だけ読み、`state page(query) = 1` は `query` が変わるたびに 1 に戻ります（[追従する状態](language/components.md#追従する状態)を参照）。

コンポーネントの中の状態は、そのコンポーネントのものです。インスタンスごとに別々の状態を持ち、インスタンスがなくなると一緒になくなります。ファイルの最上位に書いた `state` はアプリのもので、アプリ全体で一つだけです。**ストア**は、状態とそれを変更する関数をまとめ、それを共有するビューから使えるようにします。

```asuka
pub store Cart {
  state items: [Item] = []
  let total = items.sum(i => i.price)

  pub fn add(i: Item) { items.push(i) }
}
```

### 派生値

コンポーネント、ストア、ファイルの中の `let` は、ほかの値から値を導きます。

```asuka
state first = "Ada"
state last = "Lovelace"
let full = "{first} {last}"
let left = todos.count(t => !t.done)
```

`let` は読まれたときに計算され、その後は読んだもののどれかが変わったときだけ計算し直されます。二回読んでも、計算は一回分です。読むのはコードに書かれた部分だけです。`todos.count(t => !t.done)` は各項目の `done` と項目の数を読むので、項目の名前を変えても `left` は計算し直されません。`let` から状態に書き込むことはできません。

## 何が更新されるか

状態を読むプロパティは、それぞれが独立したバインディングです。読んだ状態が変わると、そのバインディングだけが再実行され、その一つのプロパティをネイティブビューに送ります。ほかには何も実行されません。

```asuka
component Score {
  state score = 0

  Column {
    Text("Score: {score}")          // score が変わると更新される
    Text("Keep going")              // 一度だけ送られる
    Button("Point") { on press { score += 1 } }
  }
}
```

*Point* を押すと `score` が変わります。一つ目の `Text` のバインディングが再実行され、新しいテキストを送ります。コンポーネントは再実行されず、二つ目の `Text` には手が触れられず、比較するツリーもありません。変更は、状態からそれを読んでいる一つのプロパティへ直接届きます。

バインディングが追うのは、状態のうち読んだ部分だけです。`Text(user.name)` はユーザーの名前を追うので、`user.avatar` を変えても再実行されません。`todos[1].done = true` で再実行されるのは、その `done` を読むもの（とリスト全体を読むもの）だけで、ほかの項目やこの項目のタイトルを読むものは動きません。`todos.count()` は項目の数を、`cart[sku]` はそのキーを、`picked.contains(id)` はその要素を追います。関数に渡したり比較したりして丸ごと読んだ値は、丸ごと追われます。値を丸ごと置き換えたとき（`todos = fetched`）に再実行されるのは、実際に変わった部分を読むものだけです。

ハンドラーの中での書き込みは、ハンドラーが終わったとき（または `await` に達したとき）にまとめて画面に反映されます。三回書き込んでも、更新は一回です。

## エフェクトとタスク

`effect` はコードをすぐに実行し、読んだものが変わるたびにまた実行します。丸括弧の中に値を書くと、それが変わったときだけまた実行します。

```asuka
effect { storage.set("draft", draft) }
effect(selected) { analytics.log("selected", selected) }
effect() { analytics.log("opened") }   // コンポーネントが作られるときに 1 回だけ
```

エフェクトは自分が追従している状態には書き込めません（書き込むと、自分の書き込みのあとにもう一度実行されてしまいます）。コンパイラーはこれをエラーにし、代わりの書き方を示します。

エフェクトが用意したものは、その本体に書いた `cleanup { … }` で元に戻します。cleanup は、エフェクトが次に実行される前と、エフェクトがコンポーネントと一緒になくなるときに実行されます。

```asuka
effect(roomId) {
  let room = roomId
  chat.join(room)
  cleanup { chat.leave(room) }
}
```

cleanup が読む状態は、cleanup が実行されたときの値です。そのときには、エフェクトが追従しているものはもう変わっています。ここでの `room` のように、その実行で使った値は `let` に取っておきます。cleanup がエフェクトの追従しているものを読むと、コンパイラーが警告します。

`task` は、コンポーネントが表示されている間に行う非同期の処理です。コンポーネントが現れると始まり、なくなるとキャンセルされます。丸括弧の中に値を書くと、それが変わるたびに前回の実行をキャンセルしてやり直します。丸括弧の中身は比較されるので、`task(user.id)` は id が変わったときにやり直し、ユーザーの名前が変わってもやり直しません。

```asuka
task { messages = try await api.inbox() }
task(query) {
  await sleep(300ms)
  results = try await api.search(query)
}
```

`load` は、非同期の結果を、エラーや読み込み中かどうかも含めて状態として保持します。一度結果が届けば、次の読み込みのあいだもその結果が画面に残ります（[Asuka 言語](language/async.md#読み込むデータ)を参照）。

## 表示の切り替えと繰り返し

`if` は条件が成り立つ間ビューを表示し、`match` はいくつかのうちの一つを表示し、`for … key` は項目ごとに行を一つ表示します。

```asuka
if loggedIn {
  Profile(user)
} else {
  Button("Log in") { on press { present(Login()) } }
}

match status {
  .loading => Spinner()
  .failed(e) => Text(e.message)
  .ready(items) => ItemList(items)
}

for todo in todos key todo.id {
  TodoRow(todo)
}
```

`for` はキーごとに行を一つ保ちます。項目が変わった行は自分のバインディングだけを更新し、新しい項目が加わると行が作られ、項目が削除されるとその行は破棄されます。移動した行は作り直されず、移動されるだけです。変わる項目を回す `for` には `key` が必要です。範囲、集合、定数のリストは、それ自体の値を行のキーにします。

状態を回す `for` が追うのは項目の並びで、項目の中身ではありません。ある項目のフィールドを変えても、`for` もほかの行も再実行されず、その行でそのフィールドを読むバインディングだけが動きます。行の項目を渡しても（`TodoRow(todo)`）、それはその項目のままです。`TodoRow` の中の `todo.title` は、その項目のタイトルを追います。項目のフィールドをキーにする（`key todo.id`）のがいちばん安上がりで、`for` はすべてのキーを一つのものとして追います。

### 失敗したとき

ハンドラーやタスクで捕まえなかったエラーは、それを囲むいちばん近い `Boundary` に届きます。`Boundary` は、中身の代わりにそのエラーに対して指定したものを表示します。

```asuka
Boundary {
  Feed()
  on error(e) { Text("Something went wrong: {e.message}") }
}
```

`Boundary` がなければ、アプリ自身がエラーを報告します。開発中は開発用アプリが画面に表示し、リリースビルドではログに記録します。

### 長いリスト

`List` の直下に書いた `for` は、画面に表示されている行だけを組み立て、スクロールに合わせてそれを再利用します。そのため 100,000 件のリストでも、コストは見えている分だけです。[コンポーネント](components.md#リスト)を参照してください。

## オーナーとクリーンアップ

コンポーネントが作るもの（状態、バインディング、エフェクト、タスク）はすべて、一つの**オーナー**、つまりそのコンポーネントのインスタンスのオーナーに属します。`if` の分岐、`for` の行、`match` のケース、スタックの画面、タブ、シートも、それぞれ親のオーナーの下に自分のオーナーを持ちます。オーナーを破棄すると、そのバインディングとエフェクトが止まり、タスクがキャンセルされ、その下のオーナーも破棄されます。

オーナーが破棄されるのは、次のときです。

- `if` が分岐を切り替えたとき：古い分岐のオーナー
- `for` の項目のキーが配列からなくなったとき：その行のオーナー
- `match` のケースが変わったとき：古いケースのオーナー
- 画面がポップされたとき（戻るボタン、スワイプ、`pop()`）：その画面のオーナー
- シートが閉じたとき：シートの中身のオーナー

コンポーネントの本体に書いた `cleanup { … }` は、そのオーナーが破棄されるときに実行されます。エフェクトの本体に書いたものもそのときに実行され、さらにエフェクトが次に実行されるたびに、その前にも実行されます。ストアと app の本体にも書けます。cleanup は登録されたのと逆の順番で、オーナーが作ったものが止まったあとに実行されます。先にエフェクトの cleanup、次に本体の cleanup です。エフェクトの cleanup はその実行で登録したものを取り消し、本体の cleanup はコンポーネントがなくなったことを伝えます。

```asuka
component Tab(value: string) {
  use tabs: TabsContext
  effect(value) {
    let v = value
    tabs.add(v)
    cleanup { tabs.remove(v) }
  }
  cleanup { tabs.closed(value) }
  Text(value)
}
```

キャンセルされたタスクは、そのとき待っている `await` で終わり、その途中で `defer` を実行します。タスクが始めた処理は、そこで止めます。

```asuka
component Clock {
  state now = time.now()

  task {
    let ticker = time.ticker(1s)
    defer { ticker.stop() }
    for await t in ticker.ticks { now = t }
  }

  Text(now.format("HH:mm:ss")) { font: .title.semibold() }
}
```

これを `if showClock { Clock() }` で表示すると、時計が非表示になったときにティッカーが止まり、再び表示されると新しいティッカーが動き始めます。

## メインスレッド

コンポーネント、ハンドラー、エフェクト、タスク、そして状態へのすべての書き込みは、プラットフォームのメインスレッドで実行されます。メインスレッドとは、iOS と Android では UI スレッド、macOS、Windows、Linux ではプロセスの最初のスレッド、ブラウザーではページのスレッドのことです。メインスレッドを止めないでください。メインスレッドがふさがっている間、アプリは描画も、スクロールも、入力の受け付けもできません。

待つ必要のある処理（ネットワーク、ファイル、タイマー）は `async` です。`await` で待っている間もメインスレッドはほかの処理を進められ、`await` の後のコードは再びメインスレッドで実行されます。そのため、そこで状態に書き込めます。

```asuka
on press {
  loading = true
  defer { loading = false }
  do {
    let page = try await http.get("https://example.com").text()
    status = "{page.count()} characters"
  } catch e {
    status = e.message
  }
}
```

長い計算（パース、画像処理）は `spawn { … }` に入れると、別のスレッドで実行されます。中で使えるのは値だけで、状態やビューは使えません（使っていればコンパイラーが教えてくれます）。結果は `await` でメインスレッドに戻ってきます。

```asuka
let html = await spawn { markdown.render(source) }
```

## 環境

`environment()` は、コンポーネントが表示される場所についてプラットフォームが伝える情報です。バインディングや `let` の中で読めば、画面の回転、キーボード、ダークモード、文字サイズの変化に追従します。返されるのは、そのコンポーネントがある画面（スタックの画面、タブ、シート、ルートのいずれか）の環境です。

```asuka
let landscape = environment().width > environment().height
```

フィールドは次のとおりです。

- `width`、`height`：画面のサイズ（ポイント単位）。デバイスではなく、アプリのウィンドウのサイズです。画面分割（iPad の Split View やステージマネージャ、Android の分割画面）ではアプリに割り当てられた部分のサイズに、デスクトップやブラウザーではウィンドウのコンテンツ領域のサイズになります。ユーザーがサイズを変えると、それに合わせて変わります。
- `safeTop`、`safeRight`、`safeBottom`、`safeLeft`：セーフエリアのインセット。デスクトップのウィンドウにはセーフエリアがないので、すべて 0 です。
- `keyboard`：画面の下端のうち、オンスクリーンキーボードに覆われている高さ。
- `scale`：1 ポイントあたりのピクセル数。スマートフォンでは 2 か 3、Retina ディスプレイの Mac では 2、Windows と Linux ではディスプレイの拡大率（150% なら 1.5）です。
- `fontScale`：ユーザーが設定した文字サイズ（iOS の Dynamic Type、Android のフォントサイズ、Windows の「テキストのサイズ」、GNOME のテキストの拡大率）。1 がデフォルトです。
- `dark`：ダークモードがオンかどうか。
- `reduceMotion`：ユーザーが動きを減らすよう求めているかどうか。[モーション](motion.md)を参照してください。
- `screenReader`：VoiceOver、TalkBack、ナレーター、Orca のいずれかがオンかどうか。[アクセシビリティ](accessibility.md#スクリーンリーダーが有効かどうか)を参照してください。
- `widthClass`、`heightClass`：ウィンドウがアプリに与えている広さを、軸ごとにプラットフォームの分類で表したもの。値は `.compact`、`.medium`、`.expanded` のいずれかです。コンテンツの横にサイドバーを置いたり、リストの横に詳細を並べたりするときは、独自に決めた幅ではなく、これらの値で分岐してください。iOS では UIKit のサイズクラス（compact か regular で、regular は expanded として扱います）を、Android では Material のウィンドウサイズクラスを返します。macOS、Windows、Linux、Web では、ウィンドウのサイズから Material の分類を求めます。
- `locale`、`languages`、`timeZone`：ユーザーの地域の書式、ユーザーが読める言語、そしてタイムゾーン。ほとんどのアプリは、[カタログとフォーマット](i18n.md)を通してこれらを使います。
- `platform`：アプリが動いているシステム。`.iOS`、`.android`、`.macOS`、`.windows`、`.linux`、`.web` のいずれかです。
- `tv`：アプリがテレビで動いているかどうか（Apple TV。このとき `platform` は `.iOS` です。それと Android TV）。タッチではなく、リモコンでフォーカスを動かして操作します。[テレビ](tv.md)を参照してください。
- `watch`：アプリがウォッチで動いているかどうか（Apple Watch。このとき `platform` は `.iOS` です）。画面は小さく、ちらっと見るもので、Digital Crown があります。[Apple Watch](watch.md) を参照してください。

セマンティックカラーとフォントは、ダークモードと文字サイズに自動で追従します。それらでまかなえない部分は、環境を読んで対応してください。

```asuka
if environment().widthClass == .expanded {
  Row { grow: 1; Sidebar(); Detail() }
} else {
  ItemList()
}
```

プラットフォームのビューは、何もしなくても本来の見た目になります。アプリが自分で描くものは、`platform` で分岐して各プラットフォームのデザインに合わせます。たとえば [`examples/usage`](../../examples/usage) では、iOS では UIKit の inset grouped リストを、Android では Material 3 のリストを描いています。

## 永続化される値

状態は、アプリが動いている間だけ存在します。ストア（`import std/store`）を使うと、設定のような小さな値をアプリの起動をまたいで保持できます。`read` は保存された値（なければデフォルト値）を返し、`write` は値を保存します。エフェクトを使えば、状態の値が変わるたびに保存できます。

```asuka
import std/store

state units = store.read("units", "metric")
effect { store.write("units", units) }
```

値は JSON としてプラットフォームの設定に保存されるので、数値、文字列、配列、構造体のどれでも使えます。ストアとデータベースについては[データ](data.md)を参照してください。なお `asuka dev` では、コードを何も書かなくても、すべての状態の値が保存をまたいで保持されます（[はじめに](getting-started.md#実行)を参照）。
