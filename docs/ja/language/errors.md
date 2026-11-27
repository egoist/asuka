# エラー

失敗する可能性のある関数には `throws` を付け、呼び出すときには `try` が必要です。

```asuka
enum MailError: Error { offline, server(code: int) }

async fn send(m: Message) throws {
  if !network.online { throw MailError.offline }
  …
}

on press {
  do {
    try await send(draft)
    dismiss()
  } catch MailError.offline {
    showOffline = true
  } catch e {
    alert("Couldn't send", message: e.message)
  }
}

let n = try? parse(text)   // エラーが投げられたら none
```

式の後ろに `catch` を書くと、代わりの値を用意できます。普段は式の値になり、式の中でエラーが投げられたときは `catch` のブロックの値（`if` と同じく、最後の式）になります。ブロックがその場を離れる場合は別です。

```asuka
let port = try parsePort(text) catch e {
  log(e.message)
  80
}
let size = try await fileSize(path) catch { return }
try save(draft) catch { failed = true }   // 文として使う：値は要らない
```

節は `do` のものと同じです（`catch`、`catch e`、`catch e: ParseError`、`catch MailError.server(code)`）。複数を続けて書くことができ、エラーを受け取った最初の節が実行されます。どの節も受け取らなかったエラーはそのまま先へ投げられるので、`try` と同じく、囲んでいる関数に `throws` が必要です。すべてのエラーを受け取る節があれば、式全体がエラーを投げなくなるので、`try` を置けない場所でも使えます。たとえばコンポーネントの `let`（`let port = try parsePort(text) catch { 80 }` は `text` が変わるたびに更新されます）や、エラーを投げてはいけないクロージャの中です。`try await` とも、値としての `throw` とも組み合わせられます（`let user = cache[id] ?? throw MailError.missing(id)`）。

エラーの `message` は、`message` フィールド、`fn message() -> string`、またはそのエラーを言語での書き方で書いたものです。型の名前、ケースの名前、ペイロードからなり、各値は埋め込みが値の中身を書くのと同じように書かれます（`CountError.tooMany(limit: 2)`、`ParseError(line: 3, text: "x")`、`MailError.offline`）。デバッガーの表示と同じで、デバッガーと同じように長すぎる部分は `…` で切り詰められます（200 バイトを超えた部分、120 バイトを超える文字列、10 項目を超えるリスト、3 段より深い入れ子）。メッセージは読まれたときに作られます。`Error("…")` はメッセージからエラーを作ります。タスクのキャンセルは `catch` では捕まりません。キャンセルはタスクの最後まで進み、その途中で `defer` を実行していきます。

`Error` は、`catch` と同じように、エラー型のケースでも照合できます：`match e { MailError.offline => …; MailError.status(code) => …; _ => e.message }`（`Error` はどんなエラーでもありうるので、`_` で終えます）。

ハンドラーやタスクで捕まえなかったエラーは、そのコンポーネントを囲むいちばん近い `Boundary` に届きます。`Boundary` がなければアプリのもの（開発中は画面に表示し、リリースビルドではログに記録します）に届きます。

```asuka
Boundary {
  Feed()
  on error(e) { Text("Something went wrong: {e.message}") }
}
```
