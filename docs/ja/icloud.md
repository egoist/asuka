# iCloud

`asuka/sdk/icloud` を使うと、iOS と macOS のアプリからユーザーの iCloud を使えます。このパッケージは二つの部分からなります。**キーバリューストア**は小さな値（設定、最後に読んだページ、連続記録など）を保存し、その値はユーザーのどのデバイスにも届きます。**書類**は iCloud Drive にあるアプリのファイルで、「ファイル」アプリや Finder では、アプリの名前のフォルダーに表示されます。Swift でいえば NSUbiquitousKeyValueStore とアプリの ubiquity コンテナーに、Expo アプリでいえば react-native-cloud-storage や expo-icloud-storage が提供するものにあたります。

```go
size := icloud.Signal("textSize", 17.0) // どのデバイスでも同じ値

dir, err := icloud.Documents()          // iCloud Drive にあるアプリのフォルダー
err = icloud.WriteFile(filepath.Join(dir, "2026-11-15.txt"), []byte("Rain."))
files := icloud.UseFiles(icloud.DocumentsScope) // 常に最新の一覧
```

iCloud は Apple のサービスです。Android、Windows、Linux、Web では、どの呼び出しも `icloud.ErrNotAvailable` で失敗します。これらのプラットフォームでは、自前のサーバーを通じて同期してください。

[examples/journal](../../examples/journal) は、一日一つのテキストファイルとして日記を iCloud Drive に保存し、文字の大きさをキーバリューストアに保存します。

## アカウント

`icloud.CurrentAccount` は、ユーザーが iCloud にサインインしていて、アプリの iCloud がオンになっているか（`Available`）と、それがどのアカウントか（`ID`）を返します。メインスレッド以外から呼びます。`icloud.UseAccount` は同じ情報をコンポーネント用のシグナルにしたもので、コンポーネントが存在するあいだ、アカウントの変化に追従します。`icloud.WatchAccount` は、アカウントが変わるたびに関数を呼びます。変わるのは、ユーザーがサインインやサインアウトをしたとき、アプリの iCloud をオフにしたとき、別のユーザーがサインインしたときです。

```go
account := icloud.UseAccount()
ui.Show(signal.Func(func() bool { return !account.Get().Available }), func() ui.Node {
	return ui.StaticText("Sign in to iCloud to keep your journal on all your devices.")
})
```

`ID` は iCloud の ID トークン（identity token）のハッシュです。一つのデバイスの上では、同じアカウントはいつも同じ ID になります（デバイス間で比べるためのものではありません）。この ID を保存しておき、アプリの起動時に別の ID になっていたら、Apple が勧めているとおり、前のユーザーの iCloud から得たデータは使わずに取り分けておいてください。

アカウントがない場合、書類の呼び出しは `icloud.ErrNoAccount` で失敗します。キーバリューストアはデバイス上でそのまま動き続け、ユーザーがサインインすると同期されます。

## キーバリューストア

`Get`、`Set`、`Delete`、`All` は、キーごとに値を読み書きします。メインスレッド以外から呼びます。値には、`encoding/json` でエンコードできるものなら何でも使えます。`Set` は値をすぐにデバイスに保存し、ユーザーのほかのデバイスには iCloud が同期したときに保存されます。`nil` を設定するとキーが削除されます。

```go
icloud.Set("lastPage", 42)
var page int
found, err := icloud.Get("lastPage", &page)
```

`icloud.Signal(key, def)` は、キーの値をコンポーネント用のシグナルにしたものです。`asuka/sdk/store` の `Signal` がデバイスに保存する値を扱うのと同じように使えます。シグナルは `def` で始まり、iCloud が保持している値を読み込むとその値になります。ただし、読み込むまでのあいだにアプリが値を設定していた場合はそのままです。アプリが設定した値はそのつど保存されます。コンポーネントが存在するあいだは、ほかのデバイスでの変更に追従します（ほかのデバイスがキーを削除すると `def` に戻ります）。iCloud から受け取った値を書き戻すことはありません。

`icloud.OnChange` は、iCloud が値を変更するたびに、メインスレッドで関数を呼びます。変更には `Reason` と、変更されたキーが含まれます。

| 理由 | 起きたこと |
| --- | --- |
| `ServerChange` | ほかのデバイスが値を変更した |
| `InitialSync` | 初回のダウンロード。アプリのストアが空だった場合（新しいデバイス、アプリの再インストール） |
| `QuotaViolation` | アプリの値が iCloud の上限を超えた。いくつか削除してください |
| `AccountChange` | 別のアカウントがサインインし、前のアカウントの値がそのアカウントの値に置き換わった |

二つのデバイスが同じキーに値を設定した場合は、最後に設定したほうが残ります。オフラインだったデバイスは、オンラインに戻ったときにほかのデバイスの変更を受け取り、そのとき `OnChange` に通知されます。`Sync` は、デバイスでの変更をディスクに書き込み、ほかのデバイスの変更を早めに確認するよう iCloud に促します。iCloud は自動で同期するので、`Sync` はユーザーが求めたとき（引っぱって更新したときなど）だけ呼んでください。

ストアは小さな値のためのものです。キーは 64 バイトまで、値は 1 MB までで、これを超えると `Set` は `ErrKeyTooLong` や `ErrTooLarge` で失敗します。一つのアプリの値は合わせて 1 MB、1024 キーまでです。これを超えると iCloud は値を同期しなくなり、`OnChange` に `QuotaViolation` が届きます。それより大きなデータには書類を使ってください。

値はプロパティリストとして保存されるので、アプリの Swift、ウィジェット、Apple Watch のアプリからも同じように読めます。JSON の文字列、数値、真偽値、配列、オブジェクトは、プロパティリストの対応する型になります。オブジェクトの中の null は省かれますが、Go でデコードした結果は変わりません。配列の中の null は保存できません。Swift のコード（アプリ自身のものや、そのウィジェットのもの）がデータ（data）や日付（date）として書いた値は、それぞれ base64 と RFC 3339 のテキストとして届くので、Go では `[]byte` と `time.Time` にデコードできます。

## 書類

iCloud にあるアプリのファイルは、アプリの**コンテナー**に置かれます。コンテナーは、iCloud が同期する、デバイス上のディレクトリです。`icloud.Container(id)` はコンテナーのディレクトリを返し、`""` を渡すとアプリの最初のコンテナーのディレクトリを返します。デフォルトでは、アプリのコンテナーは一つで、アプリの ID の前に `iCloud.` を付けたものです。`asuka.json` の `icloud.containers` でほかのコンテナーを指定でき、その最初のものがデフォルトになります。初回の呼び出しは、iCloud がコンテナーを準備するため、時間がかかることがあります。

`icloud.Documents()` は最初のコンテナーの `Documents` ディレクトリを返し、なければ作ります。ここに置いたものは、iCloud Drive（iOS では「ファイル」アプリ、Mac では Finder）のアプリの名前のフォルダーに表示されます。ユーザーはそこでファイルを開いたり、名前を変えたり、移動したりできます。コンテナーのそれ以外の部分は、アプリだけが使います。

### ファイルの一覧

`icloud.Files(scope)` は、アプリのファイルを一覧にします。`DocumentsScope` はコンテナーの `Documents` にあるファイルを、`DataScope` はそれ以外のファイルを一覧にします。一覧には、ほかのデバイスが作ったまま、まだダウンロードされていないファイルも含まれます。`WatchFiles` は一覧を渡して関数を呼び、一覧が変わるたびにまた呼びます。一覧が変わるのは、ほかのデバイスからファイルが届いたとき、ファイルが追加、削除、名前の変更、ダウンロード、アップロードされたとき、ダウンロードが進んだときです。`UseFiles` は、一覧をコンポーネント用のシグナルにしたものです。どちらも iCloud のメタデータクエリ（NSMetadataQuery）を使います。クエリはスコープごとに、最初に監視が始まったときに開始し、最後の監視が終わったときに停止します。

`File` には `Path`、`Name`、`Size`、`Created`、`Modified` と、ファイルがどこにあるかを表すフィールドがあります。

| フィールド | 意味 |
| --- | --- |
| `Status` | `NotDownloaded`（iCloud にだけある）、`Downloaded`（iCloud のものより古いバージョン）、`Current` |
| `Downloading`、`DownloadProgress` | ダウンロード中かどうかと、ダウンロード済みの割合（0〜1） |
| `Uploading`、`UploadProgress`、`InCloud` | iCloud にアップロード中かどうか、アップロード済みの割合、iCloud にあるかどうか |
| `Conflicts` | ほかのデバイスが同時に別のバージョンを保存した |
| `DownloadError`、`UploadError` | 最後のダウンロードやアップロードが失敗した理由（デバイスの空き容量不足、iCloud の容量不足など） |

### 読み書き

アプリのファイルは `ReadFile`、`WriteFile`、`Remove`、`Rename` で読み書きします。メインスレッド以外から呼びます。これらは iCloud 自身の読み書きと協調する（NSFileCoordinator）ので、同期処理が書きかけのファイルを読むことも、iCloud が置き換えている途中のファイルをアプリが読むこともありません。`ReadFile` は、デバイスにないファイルなら先にダウンロードし、それを待ちます。`WriteFile` はファイル全体を一度に書き込み、必要なディレクトリも作ります。書き込んだファイルは iCloud がアップロードします。ダウンロード済みのファイルには `os` パッケージの関数も使えますが、同期とのあいだで競合状態（レース）になることがあります。

`Download` は、ファイル（またはフォルダーの中身）のダウンロードを始めます。進み具合は、そのファイルの `File` でわかります。`Evict` は、ファイルを iCloud に残したままデバイスから取り除き、空き容量を増やします。取り除いたファイルは、次に読み込むときに再びダウンロードされます。

`MoveIn(local, path)` は、デバイス上のファイルを iCloud に移します。移動先は、アプリのコンテナーのいずれかにある `path` です。ファイルは元の場所から移り、ユーザーのほかのデバイスに現れます。`MoveOut(path, local)` はファイルを iCloud からデバイス上に移し、ユーザーのほかのデバイスからはそのファイルがなくなります。アプリのコンテナーの外にあるパスを渡すと `ErrNotContainer` で失敗し、ファイルがない場合は、`errors.Is(err, fs.ErrNotExist)` が真になるエラーで失敗します。

### 競合

二つのデバイスが同時に（あるいは一方がオフラインのあいだに）ファイルを変更すると、iCloud は一方をそのファイルとして残し、残りをそのファイルと競合するバージョンとして保存します。このとき、ファイルの `Conflicts` は true になります。`icloud.Conflicts(path)` は、競合しているバージョンを返します。各バージョンには、内容を読むための `Path`、`Modified`、保存したデバイスの名前（`Device`）があります。`icloud.Resolve(path, keep)` で競合を解決します。`keep` がそのファイルになり、`nil` を渡すとファイルはそのまま残ります。どちらの場合も、ほかのバージョンはすべてのデバイスから削除されます。内容をマージするには、マージした結果を `WriteFile` で書き込んでから `Resolve(path, nil)` を呼びます。

```go
vs, _ := icloud.Conflicts(path)
if len(vs) > 0 {
	icloud.Resolve(path, &vs[0]) // ほかのデバイスのものを残す
}
```

### リンクの共有

`icloud.Link(path)` は、ファイルの公開リンクと、その有効期限を返します。ファイルがまだアップロードされていなければ、先にアップロードします。リンクを知っていれば誰でも、その時点のファイルのコピーをダウンロードできます。

## asuka.json とエンタイトルメント

アプリに付与されるものは、アプリが何を呼んでいるかで決まり、それは Go のコードから判断されます。キーバリューストアの関数を呼ぶアプリには、そのエンタイトルメント（`com.apple.developer.ubiquity-kvstore-identifier`。値はチームの ID とアプリの ID）が付きます。書類の関数を呼ぶアプリには、iCloud Documents（`com.apple.developer.icloud-services` の `CloudDocuments`）とアプリのコンテナーが付き、Info.plist の `NSUbiquitousContainers` によって、最初のコンテナーの `Documents` が iCloud Drive に表示されます。アカウントについて問い合わせるだけのアプリにも、ストアのエンタイトルメントが付きます。これがないとアカウントを確認できないためです。

```json
"icloud": {
  "containers": ["iCloud.com.example.journal"],
  "driveName": "Journal"
}
```

| キー | 型 | デフォルト | 説明 |
| --- | --- | --- | --- |
| `containers` | 文字列の配列 | `["iCloud.<id>"]` | アプリの iCloud コンテナーです。最初のものがデフォルト（`Container("")`、`Documents`）になります。 |
| `driveName` | 文字列 | アプリの `name` | iCloud Drive での、最初のコンテナーのフォルダーの名前です。 |
| `driveHidden` | 真偽値 | `false` | アプリの書類を iCloud Drive に表示しません。書類は同期されますが、見えるのはアプリだけです。 |

実機向けのビルドやリリースビルドでは、developer.apple.com（Identifiers）で、アプリの App ID の iCloud ケイパビリティを有効にしておく必要があります。書類を使う場合は、iCloud Documents とアプリのコンテナーも有効にします。iPhone で `asuka dev` が使う開発用のプロファイルは Xcode が作り、そのときに Xcode がこの設定も済ませます。iCloud を含むプロファイルにはワイルドカード（サービスの `*`、ストアの `TEAMID.*`）が入っていますが、ビルドは Xcode と同じく、それをアプリが使う値にして署名します。プロファイルに足りないもの（ケイパビリティやコンテナー）は、`asuka build` が教えてくれます。

iCloud Drive が新しいフォルダーや新しい名前を反映するのは、`build` 番号が新しくなったときです。`icloud` を変更したら `build` を増やしてください。

## asuka dev の場合

iCloud はアプリの値と書類をアプリ自身の ID で管理しますが、開発用アプリの ID はアプリの ID とは違います。そのため `asuka dev` は、このパッケージをインポートしたアプリを、シミュレーターでも iPhone でも開発用アプリでは動かさず、アプリそのものとしてビルドし、そのことを表示します。シミュレーターで iCloud を試すには、シミュレーターの「設定」で Apple アカウントにサインインし、iCloud Drive をオンにしてください。

Mac では、iCloud のエンタイトルメントを使うには、アプリにプロビジョニングプロファイルで署名する必要があります（`asuka build --profile`、または `asuka.json` の `mac.profile`）。`asuka dev` はこの署名をしないので、プロファイルなしで動くアプリの呼び出しは `ErrNotAvailable` で失敗し、その理由を伝えます。

## プラットフォームごとの動作

| プラットフォーム | iCloud |
| --- | --- |
| iOS | NSUbiquitousKeyValueStore、アプリの ubiquity コンテナー、NSMetadataQuery、NSFileCoordinator、NSFileVersion |
| macOS | iOS と同じです。このパッケージをインポートするアプリのためにビルドが作る、アプリの Swift ライブラリの中で動きます。iCloud を含むプロビジョニングプロファイルが必要です |
| Android、Windows、Linux、Web | ありません（`ErrNotAvailable`）。iCloud for Windows はユーザーの iCloud Drive を同期しますが、アプリにはストアもコンテナーも提供しません。Web の CloudKit JS にも、どちらもありません |

このパッケージは CloudKit のデータベース（レコード、クエリ、共有）は使いません。

## テスト

`h.ICloud()` は、フェイクホストに iCloud（`fake.ICloud`）を持たせます。ユーザーのほかのデバイスの役はテストが務めます。コンテナーは `Root` の下のディレクトリで、アプリはデバイス上と同じようにそれを読み書きします。`Change` は、ほかのデバイスと同じように値を設定します。`Arrive` は、一覧には載っているもののまだダウンロードされていないファイルを届けます。`Conflict` は競合するバージョンを保存します。`SignIn` と `SignOut` はアカウントを切り替え、`ExceedQuota` はストアの上限を超えさせます。どれもデバイス上と同じく、イベントとしてアプリに届きます。アプリが何をしたかは、`Value`、`Downloaded`、`Watching`、`Path` で確かめます。

```go
var ic *fake.ICloud
a := uitest.Mount(t, Journal, uitest.Setup(func(h *fake.Host) {
	ic = h.ICloud()
	ic.Root = t.TempDir()
}))
ic.Arrive("Documents/2026-11-14.txt", []byte("Rain all day."))
a.WaitFor("2026-11-14")
a.Tap("2026-11-14")
a.WaitFor("Rain all day.")
```

`h.ICloud()` を呼ばなければ、フェイクホストには iCloud がなく、Android と同じく、呼び出しは `ErrNotAvailable` で失敗します。
