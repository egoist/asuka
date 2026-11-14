# iCloud

`asuka/sdk/icloud` 让 iOS 和 macOS 上的应用使用用户的 iCloud。它分为两部分。**键值存储**保存少量的值（设置、上次读到的页、连续打卡的天数），这些值会跟着用户同步到他的每一台设备上。**文档**是应用放在 iCloud Drive 里的文件，“文件” App 和访达会把它们显示在一个以应用命名的文件夹里。它对应 Swift 中的 NSUbiquitousKeyValueStore 和应用的 ubiquity 容器，也就是 react-native-cloud-storage 和 expo-icloud-storage 为 Expo 应用提供的能力。

```go
size := icloud.Signal("textSize", 17.0) // 在每台设备上都一样

dir, err := icloud.Documents()          // 应用在 iCloud Drive 里的文件夹
err = icloud.WriteFile(filepath.Join(dir, "2026-11-15.txt"), []byte("Rain."))
files := icloud.UseFiles(icloud.DocumentsScope) // 文件列表，始终保持最新
```

iCloud 是 Apple 的服务。在 Android、Windows、Linux 和 Web 上，每个调用都会以 `icloud.ErrNotAvailable` 失败：在这些平台上，请通过你自己的服务器来同步。

[examples/journal](../../examples/journal) 把日记保存在 iCloud Drive 里，每天一个文本文件，文字大小则存在键值存储里。

## 账户

`icloud.CurrentAccount` 告诉你用户是否登录了 iCloud、是否为这个应用开启了 iCloud（`Available`），以及登录的是哪个账户（`ID`）；不能在主线程上调用。`icloud.UseAccount` 把同样的信息作为 signal 提供给组件，在组件存活期间跟随账户的变化。`icloud.WatchAccount` 则在每次变化时调用一个函数：用户登录或退出、为应用关闭了 iCloud，或者换了另一个用户登录。

```go
account := icloud.UseAccount()
ui.Show(signal.Func(func() bool { return !account.Get().Available }), func() ui.Node {
	return ui.StaticText("Sign in to iCloud to keep your journal on all your devices.")
})
```

`ID` 是 iCloud 身份令牌（identity token）的哈希：在同一台设备上，同一个账户的 ID 始终相同（它不是用来在设备之间比较的）。把它保存下来；如果应用启动时发现换成了另一个 ID，就按 Apple 的建议，把上一个用户的 iCloud 带来的数据放到一边。

没有账户时，文档相关的调用会以 `icloud.ErrNoAccount` 失败。键值存储仍然可以在本机照常使用，等用户登录后再同步。

## 键值存储

`Get`、`Set`、`Delete` 和 `All` 按键读写值，不能在主线程上调用。值可以是任何 `encoding/json` 能编码的东西。`Set` 会立刻把值保存在本机，再随着 iCloud 的同步保存到用户的其他设备上。设为 `nil` 会删除这个键。

```go
icloud.Set("lastPage", 42)
var page int
found, err := icloud.Get("lastPage", &page)
```

`icloud.Signal(key, def)` 把一个键的值作为 signal 提供给组件，就像 `asuka/sdk/store` 的 `Signal` 之于设备本地的值。它一开始是 `def`，读到 iCloud 保存的值之后就变成那个值，除非应用在此期间已经设置过它。应用设置的每个值都会被保存下来。在组件存活期间，它会跟随其他设备所做的修改（其他设备删除了这个键时，它会回到 `def`）。从 iCloud 拿到的值不会再被写回去。

`icloud.OnChange` 在主线程上调用一个函数，传入 iCloud 做出的每一次修改。每次修改都带有一个 `Reason` 和它改动的键：

| Reason | 发生了什么 |
| --- | --- |
| `ServerChange` | 另一台设备修改了这些值 |
| `InitialSync` | 第一次下载，此前应用的存储是空的：新设备，或者重新安装了应用 |
| `QuotaViolation` | 应用的值超出了 iCloud 的上限：需要删掉一些 |
| `AccountChange` | 换了另一个账户登录，它的值替换了上一个账户的值 |

两台设备设置同一个键时，以最后一次设置为准。离线的设备重新联网后，才会收到其他设备的修改，`OnChange` 也是在那时得知。`Sync` 把本机的修改写入磁盘，并让 iCloud 尽快去查找其他设备的修改。iCloud 会自行同步，所以只在用户要求时（比如下拉刷新）才调用它。

键值存储只适合存放小的值。每个键最长 64 字节，每个值最大 1 MB：超出时，`Set` 会以 `ErrKeyTooLong` 或 `ErrTooLarge` 失败。一个应用的所有值合计最多 1 MB、1024 个键：超出后，iCloud 不再同步它们，`OnChange` 会收到 `QuotaViolation`。数据更多时请使用文档。

值以属性列表（property list）的形式保存，这样应用自己的 Swift 代码、小组件或 watch 应用都能读取：JSON 的字符串、数字、布尔值、数组和对象，分别存成属性列表里对应的类型。对象中的 null 会被略去，在 Go 里解码出来的结果不变；数组中的 null 则无法保存。Swift 代码（应用自己的、它的小组件的）以 data 或 date 写入的值，会以 base64 和 RFC 3339 文本的形式出现，Go 可以把它们分别解码成 `[]byte` 和 `time.Time`。

## 文档

应用在 iCloud 里的文件放在它的**容器**中：容器是设备上由 iCloud 同步的目录。`icloud.Container(id)` 返回一个容器的目录；传入 `""` 时返回应用的第一个容器。默认情况下，应用只有一个容器 `iCloud.<id>`（`<id>` 是应用的 ID）；`asuka.json` 的 `icloud.containers` 可以指定其他容器，其中第一个是默认容器。第一次调用可能要花一些时间，因为 iCloud 要先把容器准备好。

`icloud.Documents()` 返回第一个容器的 `Documents` 目录，不存在时会创建它。其中的内容会出现在 iCloud Drive 里（iOS 上的“文件” App、Mac 上的访达），位于一个以应用命名的文件夹中，用户可以在里面打开、重命名和移动文件。容器里其余的部分只属于应用自己。

### 列出文件

`icloud.Files(scope)` 列出应用的文件：`DocumentsScope` 列出各容器 `Documents` 中的文件，`DataScope` 列出其余的文件。列表中也包括其他设备创建、但还没有下载到本机的文件。`WatchFiles` 先用当前的列表调用一个函数，之后每当列表变化时再调用它：有文件从其他设备同步过来，有文件被添加、删除、重命名、下载或上传，或者下载有了进展。`UseFiles` 把这个列表作为 signal 提供给组件。两者用的都是 iCloud 的元数据查询（NSMetadataQuery）：某个范围有了第一个监听者时启动，最后一个监听者离开时停止。

`File` 包含它的 `Path`、`Name`、`Size`、`Created` 和 `Modified`，以及它的位置和状态：

| 字段 | 含义 |
| --- | --- |
| `Status` | `NotDownloaded`（只在 iCloud 里）、`Downloaded`（本机的版本比 iCloud 的旧）、`Current` |
| `Downloading`、`DownloadProgress` | 是否正在下载，以及已经下载的比例（0 到 1） |
| `Uploading`、`UploadProgress`、`InCloud` | 是否正在上传到 iCloud、已经上传的比例，以及 iCloud 里是否已经有它 |
| `Conflicts` | 其他设备同时保存了它的其他版本 |
| `DownloadError`、`UploadError` | 上一次下载或上传失败的原因：设备空间不足、iCloud 已满 |

### 读取和写入

用 `ReadFile`、`WriteFile`、`Remove` 和 `Rename` 读写应用的文件，不能在主线程上调用。它们会和 iCloud 自己的读写相互协调（NSFileCoordinator），因此同步时不会碰到写了一半的文件，应用也不会读到 iCloud 正在替换的文件。文件不在本机时，`ReadFile` 会先下载它并等待下载完成。`WriteFile` 一次写入整个文件，并创建所需的目录，之后由 iCloud 上传。`os` 包的函数也能处理已经下载的文件，但可能会和同步发生竞争。

`Download` 开始下载一个文件（或一个文件夹里的内容），下载的进度可以从它的 `File` 上看到。`Evict` 把文件从本机移除，但在 iCloud 里保留，以腾出空间：下次读取时会重新下载。

`MoveIn(local, path)` 把本机的一个文件移进 iCloud，放到应用某个容器中的 `path` 处。文件会从原来的位置移走，出现在用户的其他设备上。`MoveOut(path, local)` 把一个文件从 iCloud 移出到本机，用户的其他设备上就不再有它。路径不在应用的容器内时，以 `ErrNotContainer` 失败；文件不存在时，返回的错误满足 `errors.Is(err, fs.ErrNotExist)`。

### 冲突

两台设备同时修改了同一个文件（或者其中一台是在离线时修改的），iCloud 会把其中一个版本保留为这个文件，其余的作为与它冲突的版本保存起来，此时文件的 `Conflicts` 为 true。`icloud.Conflicts(path)` 返回这些版本：每个版本有用来读取它的 `Path`、它的 `Modified`，以及保存它的设备名 `Device`。`icloud.Resolve(path, keep)` 用来解决冲突：`keep` 会成为这个文件，传 `nil` 则文件保持原样；其他版本会在每台设备上被丢弃。如果要合并，先用 `WriteFile` 写入合并后的内容，再调用 `Resolve(path, nil)`。

```go
vs, _ := icloud.Conflicts(path)
if len(vs) > 0 {
	icloud.Resolve(path, &vs[0]) // 保留另一台设备的版本
}
```

### 分享链接

`icloud.Link(path)` 返回一个文件的公开链接及其过期时间；文件还没上传时，会先上传。任何拿到链接的人都能下载这个文件当前内容的一份副本。

## asuka.json 与 entitlement

应用会获得哪些配置，取决于它调用了什么，这是从它的 Go 代码里找出来的。调用了键值存储函数的应用，会获得键值存储的 entitlement（`com.apple.developer.ubiquity-kvstore-identifier`，值为团队 ID 加上应用的 ID）。调用了文档函数的应用，会获得 iCloud Documents（`com.apple.developer.icloud-services`：`CloudDocuments`）和它的容器，Info.plist 里的 `NSUbiquitousContainers` 也会让第一个容器的 `Documents` 显示在 iCloud Drive 中。只查询账户的应用会获得键值存储的 entitlement，因为没有它就看不到账户。

```json
"icloud": {
  "containers": ["iCloud.com.example.journal"],
  "driveName": "Journal"
}
```

| 键 | 类型 | 默认值 | 说明 |
| --- | --- | --- | --- |
| `containers` | 字符串数组 | `["iCloud.<id>"]` | 应用的 iCloud 容器，第一个是默认容器（`Container("")` 和 `Documents` 用的就是它）。 |
| `driveName` | 字符串 | 应用的 `name` | 第一个容器在 iCloud Drive 里的文件夹名。 |
| `driveHidden` | 布尔值 | `false` | 不在 iCloud Drive 中显示应用的文档：它们照样同步，但只有应用自己能看到。 |

要在设备上运行或做发布构建，应用的 App ID 需要在 developer.apple.com（Identifiers）上开启 iCloud 能力；用到文档的应用，还要勾选 iCloud Documents 并加上应用的容器。Xcode 为 iPhone 上的 `asuka dev` 生成开发描述文件时，会自动完成这些设置。带 iCloud 的描述文件里写的是通配符（各项服务的 `*`、键值存储的 `TEAMID.*`），构建时会像 Xcode 那样，把它们签成应用实际使用的值。`asuka build` 会告诉你描述文件缺了什么：这项能力，或者某个容器。

iCloud Drive 只有在 `build` 号变化后，才会识别新的文件夹或新的名称：修改 `icloud` 之后，记得增大 `build`。

## 在 asuka dev 中

iCloud 按应用自己的 ID 保存它的值和文档，而开发 App 的 ID 不是应用的 ID。因此，对于导入了这个包的应用，`asuka dev` 在 iOS 模拟器和 iPhone 上都会构建应用本身来运行，而不使用热重载模式，并且会告诉你这一点。要在 iOS 模拟器上试用 iCloud，请在模拟器的“设置”里登录 Apple 账户，并打开 iCloud Drive。

在 Mac 上，iCloud 的 entitlement 要求应用用描述文件签名（`asuka build --profile`，或 `asuka.json` 的 `mac.profile`），而 `asuka dev` 不会这样签名：没有描述文件时，这些调用会以 `ErrNotAvailable` 失败，并说明原因。

## 各平台上的表现

| 平台 | iCloud |
| --- | --- |
| iOS | NSUbiquitousKeyValueStore、应用的 ubiquity 容器、NSMetadataQuery、NSFileCoordinator 和 NSFileVersion |
| macOS | 和 iOS 一样，编译进应用的 Swift 库；导入了这个包的应用，构建时会生成这个库。需要带 iCloud 的描述文件 |
| Android、Windows、Linux、Web | 不支持：`ErrNotAvailable`。Windows 版 iCloud 会同步用户的 iCloud Drive，但既不向应用提供键值存储，也不提供它们的容器；Web 上的 CloudKit JS 同样两者都没有 |

这个包不使用 CloudKit 的数据库（记录、查询、共享）。

## 测试

`h.ICloud()` 给假宿主一个 iCloud（`fake.ICloud`），由测试来扮演用户的其他设备。它的容器是 `Root` 下的目录，应用像在设备上一样读写它们。`Change` 像另一台设备那样设置一个值。`Arrive` 送来一个已经列出、但还没下载的文件。`Conflict` 保存一个冲突的版本。`SignIn` 和 `SignOut` 切换账户，`ExceedQuota` 让键值存储超出上限。这些操作都会像在设备上一样，作为事件送达应用。`Value`、`Downloaded`、`Watching` 和 `Path` 用来检查应用做了什么。

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

没有调用 `h.ICloud()` 时，假宿主没有 iCloud，这些调用会像在 Android 上一样以 `ErrNotAvailable` 失败。
