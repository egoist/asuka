# iCloud

`platform/icloud` 让 iOS 和 macOS 上的应用使用用户的 iCloud。它分为两部分。**键值存储**保存少量的值（设置、上次读到的页、连续打卡的天数），这些值会跟着用户同步到他的每一台设备上。**文档**是应用放在 iCloud Drive 里的文件，“文件” App 和访达会把它们显示在一个以应用命名的文件夹里。它对应 Swift 中的 NSUbiquitousKeyValueStore 和应用的 ubiquity 容器，也就是 react-native-cloud-storage 和 expo-icloud-storage 为 Expo 应用提供的能力。

```asuka
import platform/icloud

let size = icloud.Value("textSize", 17.0)   // 在每台设备上都一样

component Entries {
  let files = icloud.files(.documents)      // 文件列表，始终保持最新

  Column {
    Text("Text size: {size.value}")
    for f in files key f.path {
      Text(f.name)
    }
    Button("Write today's") {
      on press {
        let dir = try await icloud.documents()   // 应用在 iCloud Drive 里的文件夹
        try await icloud.writeFile("{dir}/2026-11-15.txt", "Rain.".bytes())
      }
    }
  }
}
```

iCloud 是 Apple 的服务。在 Android、Windows、Linux 和 Web 上没有账户，文件列表是空的，`Value` 只保留它的默认值和在本机上设置的值，其他调用都会抛出 "icloud: not available"：在这些平台上，请通过你自己的服务器来同步。

[examples/journal](../../examples/journal) 把日记保存在 iCloud Drive 里，每天一个文本文件，文字大小则存在键值存储里。

## 账户

`icloud.account()` 会被追踪：它告诉你用户是否登录了 iCloud、是否为这个应用开启了 iCloud（`available`），以及登录的是哪个账户（`id`）。在绑定里读取它时，只要还有地方在读，它就会跟随账户的变化：用户登录或退出、为应用关闭了 iCloud，或者换了另一个用户登录。`icloud.currentAccount()` 只读取一次，不会被追踪。

```asuka
import platform/icloud

component SignInHint {
  let account = icloud.account()

  if !account.available {
    Text("Sign in to iCloud to keep your journal on all your devices.")
  }
}
```

`id` 是不透明的，来自 iCloud 的身份令牌（identity token）：在同一台设备上，同一个账户的 ID 始终相同（它不是用来在设备之间比较的）。把它保存下来；如果应用启动时发现换成了另一个账户，就按 Apple 的建议，把上一个用户的 iCloud 带来的数据放到一边。

没有账户时，文档相关的调用会抛出错误。键值存储仍然可以在本机照常使用，等用户登录后再同步。

## 键值存储

`icloud.Value(key, default)` 是一个 store，它的 `value` 就是某个键的值：一开始是 `default`，读到 iCloud 保存的值之后就变成那个值；在使用期间，它会跟随其他设备所做的修改。设置它的 `value` 会把值保存到 iCloud：立刻保存在本机，再随着 iCloud 的同步保存到用户的其他设备上。值可以是任何 JSON（`std/json`）能表示的东西：数字、字符串、数组、结构体。

```asuka
import platform/icloud

let lastPage = icloud.Value("lastPage", 0)

component Reader {
  Column {
    Text("Page {lastPage.value}")
    Button("Next page") { on press { lastPage.value += 1 } }
  }
}
```

`icloud.remove(key)` 从存储中删除一个键。

两台设备设置同一个键时，以最后一次设置为准。离线的设备重新联网后，才会收到其他设备的修改，它的 `Value` 也是在那时跟着变化。iCloud 会自行同步；这个包既没有让它尽快同步的调用，也不提供 iCloud 做出的修改及其原因（另一台设备的修改、第一次下载、账户变化、超出配额）的列表。

键值存储只适合存放小的值。每个键最长 64 字节，每个值最大 1 MB；一个应用的所有值合计最多 1 MB、1024 个键：超出后，iCloud 不再同步它们。数据更多时请使用文档。

值以属性列表（property list）的形式保存，这样应用自己的 Swift 代码、小组件或 watch 应用都能读取：JSON 的字符串、数字、布尔值、数组和对象，分别存成属性列表里对应的类型。对象中的 null 会被略去，解码出来的结果不变；数组中的 null 则无法保存。

## 文档

应用在 iCloud 里的文件放在它的**容器**中：容器是设备上由 iCloud 同步的目录。`icloud.container(id)` 返回一个容器的目录；传入 `""`（默认值）时返回应用的第一个容器。默认情况下，应用只有一个容器 `iCloud.<应用的 ID>`；`asuka.toml` 的 `icloud.containers` 可以指定其他容器，其中第一个是默认容器。第一次调用可能要花一些时间，因为 iCloud 要先把容器准备好。

`icloud.documents()` 返回第一个容器的 `Documents` 目录。其中的内容会出现在 iCloud Drive 里（iOS 上的“文件” App、Mac 上的访达），位于一个以应用命名的文件夹中，用户可以在里面打开、重命名和移动文件。容器里其余的部分只属于应用自己。

### 列出文件

`icloud.files(scope)` 会被追踪：它列出应用的文件，`.documents` 是各容器 `Documents` 中的文件，`.data` 是其余的文件。列表中也包括其他设备创建、但还没有下载到本机的文件。在绑定里读取时，列表会随文件的变化而变化：有文件从其他设备同步过来，有文件被添加、删除、重命名、下载或上传，或者下载有了进展。它用的是 iCloud 的元数据查询（NSMetadataQuery）：有了第一个读取者时启动，最后一个读取者离开时停止。`icloud.list(scope)` 只列出一次，不会被追踪。

`File` 包含它的 `path`、`name`、`dir`、`size`、`created` 和 `modified`（都是 `time.Time`），以及它的位置和状态：

| 字段 | 含义 |
| --- | --- |
| `status` | `.notDownloaded`（只在 iCloud 里）、`.downloaded`（本机的版本比 iCloud 的旧）、`.current` |
| `downloading`、`downloadProgress` | 是否正在下载，以及已经下载的比例（0 到 1） |
| `uploading`、`uploadProgress`、`uploaded` | 是否正在上传到 iCloud、已经上传的比例，以及 iCloud 里是否已经有它 |
| `conflicts` | 其他设备同时保存了它的其他版本 |
| `downloadError`、`uploadError` | 上一次下载或上传失败的原因：设备空间不足、iCloud 已满 |

### 读取和写入

用 `icloud.readFile(path)`、`writeFile(path, data)`、`delete(path)` 和 `rename(from:, to:)` 读写应用的文件。它们会和 iCloud 自己的读写相互协调（NSFileCoordinator），因此同步时不会碰到写了一半的文件，应用也不会读到 iCloud 正在替换的文件。文件不在本机时，`readFile` 会先下载它并等待下载完成。`writeFile` 一次写入整个文件，并创建所需的目录，之后由 iCloud 上传。`std/fs` 的函数也能处理已经下载的文件，但可能会和同步发生竞争。

`icloud.download(path)` 开始下载一个文件，下载的进度可以从它的 `File` 上看到。`icloud.evict(path)` 把文件从本机移除，但在 iCloud 里保留，以腾出空间：下次读取时会重新下载。

`icloud.moveIn(from:, to:)` 把本机的一个文件移进 iCloud（移到容器里的某个路径）：iCloud 会上传它，原来的位置上就没有了。`icloud.moveOut(from:, to:)` 把 iCloud 里的文件移出到本机，只归这台设备所有：如果它还不在本机，会先下载下来，然后在所有设备的 iCloud 里删除。

### 冲突

两台设备同时修改了同一个文件（或者其中一台是在离线时修改的），iCloud 会把其中一个版本保留为这个文件，其余的作为与它冲突的版本保存起来，此时文件的 `conflicts` 为 true。`icloud.conflicts(path)` 返回这些版本：每个 `Version` 有用来读取它的 `path`、它的 `modified`，以及保存它的设备名 `device`。`icloud.resolve(path, keeping:)` 用来解决冲突：`keeping` 指定的版本会成为这个文件，不指定则文件保持原样；其他版本会在每台设备上被丢弃。如果要合并，先用 `writeFile` 写入合并后的内容，再调用 `resolve(path)`。

```asuka
import platform/icloud

async fn keepTheOther(path: string) throws {
  let vs = try await icloud.conflicts(path)
  if let v = vs.first() {
    try await icloud.resolve(path, keeping: v)   // 保留另一台设备的版本
  }
}
```

### 分享链接

`icloud.link(path)` 生成一个文件的公开链接，和 iCloud 云盘的“拷贝链接”一样：任何拿到链接的人都能下载它的一份副本。它的 `url` 是链接，`expires` 是链接失效的时间（none：只要文件还在就一直有效）。可以用分享面板（`device.share`）把它发出去。

## asuka.toml 与 entitlement

应用会获得哪些配置，取决于它调用了什么。用到键值存储（`Value`、`remove`）的应用，会获得键值存储的 entitlement（`com.apple.developer.ubiquity-kvstore-identifier`，值为团队 ID 加上应用的 ID）。用到文档（`container`、`documents`、`files`、`list`、`readFile`、`writeFile`、`delete`、`rename`、`download`、`evict`、`conflicts`、`resolve`）的应用，会获得 iCloud Documents（`com.apple.developer.icloud-services`：`CloudDocuments`）和它的容器，Info.plist 里的 `NSUbiquitousContainers` 也会让第一个容器的 `Documents` 显示在 iCloud Drive 中。只查询账户的应用会获得键值存储的 entitlement，因为没有它就看不到账户。

```toml
[icloud]
containers = ["iCloud.com.example.journal"]
driveName = "Journal"
```

| 键 | 类型 | 默认值 | 说明 |
| --- | --- | --- | --- |
| `containers` | 字符串数组 | `["iCloud.<id>"]` | 应用的 iCloud 容器，第一个是默认容器（`container()` 和 `documents()` 用的就是它）。 |
| `driveName` | 字符串 | 应用的 `name` | 第一个容器在 iCloud Drive 里的文件夹名。 |
| `driveHidden` | 布尔值 | `false` | 不在 iCloud Drive 中显示应用的文档：它们照样同步，但只有应用自己能看到。 |

要在设备上运行或做发布构建，应用的 App ID 需要在 developer.apple.com（Identifiers）上开启 iCloud 能力；用到文档的应用，还要勾选 iCloud Documents 并加上应用的容器。Xcode 为 iPhone 上的 `asuka dev` 生成开发描述文件时，会自动完成这些设置。带 iCloud 的描述文件里写的是通配符（各项服务的 `*`、键值存储的 `TEAMID.*`），构建时会像 Xcode 那样，把它们签成应用实际使用的值。`asuka build` 会告诉你描述文件缺了什么：这项能力，或者某个容器。

iCloud Drive 只有在 `build` 号变化后，才会识别新的文件夹或新的名称：修改 `icloud` 之后，记得增大 `build`。

## 在 asuka dev 中

iCloud 按应用自己的 ID 保存它的值和文档。`asuka dev` 的开发 App 会构建成应用本身，带着应用的 ID 以及它所调用功能的 entitlement，所以无论在 iOS 模拟器还是 iPhone 上，iCloud 在其中都和在正式构建里一样可用。要在 iOS 模拟器上试用 iCloud，请在模拟器的“设置”里登录 Apple 账户，并打开 iCloud Drive。

在 Mac 上，iCloud 的 entitlement 要求应用用描述文件签名（`asuka build --profile`，或 `asuka.toml` 的 `mac.profile`），而 `asuka dev` 不会这样签名：没有描述文件时，这些调用会抛出错误，并说明原因。

## 各平台上的表现

| 平台 | iCloud |
| --- | --- |
| iOS | NSUbiquitousKeyValueStore、应用的 ubiquity 容器、NSMetadataQuery、NSFileCoordinator 和 NSFileVersion |
| macOS | 和 iOS 一样，编译进应用的 Swift 库；导入了这个包的应用，构建时会生成这个库。需要带 iCloud 的描述文件 |
| Android、Windows、Linux、Web | 不支持："icloud: not available"。Windows 版 iCloud 会同步用户的 iCloud Drive，但既不向应用提供键值存储，也不提供它们的容器；Web 上的 CloudKit JS 同样两者都没有 |

这个包不使用 CloudKit 的数据库（记录、查询、共享）。

## 测试

`asuka test` 里没有 iCloud：账户不可用，文件列表是空的，`Value` 只保留它的默认值和测试设置的值，其他调用都会像在 Android 上一样抛出 "icloud: not available"。`icloud.fake()` 会装上一个由测试驱动、已经登录的 iCloud：它的容器是应用的一个文件夹（`path("Documents/a.txt")` 就是其中的一个文件），测试来扮演用户的其他设备：`arrive(rel, data)` 是另一台设备新建的文件，出现在列表里、还没有下载；`change(key, value)` 是另一台设备对某个 `Value` 的修改；`conflict(rel, data)` 是另一台设备保存的文件版本；`signOut()` 和 `signIn(account)` 是用户的账户。

```asuka check
import platform/icloud

test "another device's file arrives" {
  let ic = icloud.fake()
  ic.arrive("Documents/today.txt", "Rain all day.".bytes())
  let files = try await icloud.list(.documents)
  expect(files.map(f => f.name) == ["today.txt"] && files[0].status == .notDownloaded)
  let text = (try await icloud.readFile(ic.path("Documents/today.txt"))).string()
  expect(text == "Rain all day." && ic.downloaded("Documents/today.txt"))
}
```
