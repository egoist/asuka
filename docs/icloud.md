# iCloud

`platform/icloud` is the user's iCloud, for an app on iOS and macOS. It
has two parts. The **key-value store** keeps small values (settings, the
page last read, a streak) that follow the user to each of their devices.
The **documents** are the app's files in iCloud Drive, which the Files
app and Finder show in a folder with the app's name. It's what
NSUbiquitousKeyValueStore and an app's ubiquity container are in Swift,
and what react-native-cloud-storage and expo-icloud-storage give an
Expo app.

```asuka
import platform/icloud

let size = icloud.Value("textSize", 17.0)   // the same on every device

component Entries {
  let files = icloud.files(.documents)      // the list, kept up to date

  Column {
    Text("Text size: {size.value}")
    for f in files key f.path {
      Text(f.name)
    }
    Button("Write today's") {
      on press {
        let dir = try await icloud.documents()   // the app's folder in iCloud Drive
        try await icloud.writeFile("{dir}/2026-11-15.txt", "Rain.".bytes())
      }
    }
  }
}
```

iCloud is Apple's. On Android, Windows, Linux and the web there's no
account, the lists are empty, a `Value` keeps its default and what it's
set to on the device, and the calls throw "icloud: not available": sync
there through your own server.

[examples/journal](../examples/journal) keeps a journal in iCloud Drive,
one text file a day, with its text size in the key-value store.

## The account

`icloud.account()` is tracked: whether the user is signed in to iCloud
with iCloud on for the app (`available`), and which account it is
(`id`). Read in a binding, it follows the account while it's read: the
user signs in or out, turns iCloud off for the app, or another user
signs in. `icloud.currentAccount()` reads it once, untracked.

```asuka
import platform/icloud

component SignInHint {
  let account = icloud.account()

  if !account.available {
    Text("Sign in to iCloud to keep your journal on all your devices.")
  }
}
```

`id` is opaque, from iCloud's identity token: on a device, the same
account always has the same ID (it isn't meant to be compared between
devices). Keep it, and when the app starts with another, set aside what
the last user's iCloud gave, as Apple advises.

Without an account, the documents' calls throw. The key-value store
keeps working on the device, and syncs once the user signs in.

## The key-value store

`icloud.Value(key, default)` is a store whose `value` is a key's: it
starts as `default`, then has the value iCloud keeps once it's read, and
follows the changes other devices make, while it's in use. Setting its
`value` keeps it in iCloud: on the device at once, and on the user's
other devices as iCloud syncs. A value is anything JSON says
(`std/json`): a number, a string, an array, a struct.

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

`icloud.remove(key)` takes a key away from the store.

When two devices set a key, the last one wins. A device that was offline
gets the others' changes once it's back online, and its `Value`s follow
them then. iCloud syncs by itself; the package has no call to make it
sync sooner, nor a list of the changes iCloud makes and why (another
device's, the first download, an account's, the quota's).

The store is for small values. A key is at most 64 bytes and a value
1 MB; an app's values are at most 1 MB and 1024 keys in all: past that,
iCloud stops syncing them. For more, use documents.

Values are kept as property lists, as the app's Swift, a widget or a
watch app reads them: JSON's strings, numbers, booleans, arrays and
objects as theirs. A null inside an object is left out, which decodes
the same. A null inside an array can't be kept.

## Documents

An app's files in iCloud live in its **containers**: directories on the
device that iCloud syncs. `icloud.container(id)` is a container's
directory, and with `""` (the default), the app's first. By default the
app has one, `iCloud.<the app's ID>`; `asuka.toml`'s `icloud.containers`
names others, the first its default. The first call may take a while,
as iCloud sets the container up.

`icloud.documents()` is the first container's `Documents` directory.
What's in it shows in iCloud Drive (the Files app on iOS, Finder on the
Mac) in a folder with the app's name, which the user can open, rename
and move files in. The rest of a container is the app's alone.

### Listing files

`icloud.files(scope)` is tracked: the app's files, `.documents` those
in its containers' `Documents`, and `.data` the rest. The list includes
the files other devices made that aren't downloaded yet. Read in a
binding, it changes as they do: a file comes from another device, one
is added, removed, renamed, downloaded or uploaded, or a download goes
on. It's iCloud's metadata query (NSMetadataQuery), started for its
first reader and stopped with the last. `icloud.list(scope)` lists them
once, untracked.

A `File` has its `path`, `name`, `dir`, `size`, `created` and `modified`
(`time.Time`s), and where it is:

| Field | What it says |
| --- | --- |
| `status` | `.notDownloaded` (only in iCloud), `.downloaded` (an older version than iCloud's), `.current` |
| `downloading`, `downloadProgress` | It's coming, and how much of it has (0 to 1) |
| `uploading`, `uploadProgress`, `uploaded` | It's going to iCloud, how much of it has, and whether iCloud has it |
| `conflicts` | Other devices saved versions of it at the same time |
| `downloadError`, `uploadError` | Why the last one failed: no room on the device, iCloud full |

### Reading and writing

Read and write the app's files with `icloud.readFile(path)`,
`writeFile(path, data)`, `delete(path)` and `rename(from:, to:)`. They
coordinate with iCloud's own reads and writes (NSFileCoordinator), so a
sync never sees half a file, and the app never reads one iCloud is
replacing. `readFile` downloads a file that isn't on the device first,
and waits for it. `writeFile` writes the whole file at once and makes
its directories, and iCloud uploads it. `std/fs`'s functions work on a
downloaded file too, but may race with a sync.

`icloud.download(path)` starts downloading a file, and its `File` shows
how it goes. `icloud.evict(path)` takes a file off the device and keeps
it in iCloud, to free room: it's downloaded again when it's read.

`icloud.moveIn(from:, to:)` moves a file of the device's into iCloud (to
a path in a container): iCloud uploads it, and it's gone from where it
was. `icloud.moveOut(from:, to:)` moves one out onto the device, its
alone: downloaded first if it isn't here, and gone from iCloud on every
device.

### Conflicts

When two devices change a file at the same time (or one offline), iCloud
keeps one as the file and the others as versions in conflict with it,
and the file's `conflicts` is true. `icloud.conflicts(path)` gives those
versions: each `Version`'s `path` to read it from, its `modified`, and
the name of the `device` that saved it. `icloud.resolve(path, keeping:)`
settles them: the version `keeping` becomes the file, or without one the
file stays as it is, and the other versions are dropped on every device.
To merge, write the merge with `writeFile`, then `resolve(path)`.

```asuka
import platform/icloud

async fn keepTheOther(path: string) throws {
  let vs = try await icloud.conflicts(path)
  if let v = vs.first() {
    try await icloud.resolve(path, keeping: v)   // keep the other device's
  }
}
```

### Sharing a link

`icloud.link(path)` makes a public link to a file, as iCloud Drive's
Copy Link does: anyone with it downloads a copy. Its `url` is the link,
and `expires` when it stops working (none: as long as the file's
there). Send it with the share sheet (`device.share`).

## asuka.toml and entitlements

What an app is given follows what it calls. One that uses the key-value
store (`Value`, `remove`) gets its entitlement
(`com.apple.developer.ubiquity-kvstore-identifier`, the team's ID and
the app's). One that uses the documents (`container`, `documents`,
`files`, `list`, `readFile`, `writeFile`, `delete`, `rename`,
`download`, `evict`, `conflicts`, `resolve`) gets iCloud Documents
(`com.apple.developer.icloud-services`: `CloudDocuments`) and its
containers, and its Info.plist's `NSUbiquitousContainers` shows the
first one's `Documents` in iCloud Drive. One that only asks about the
account gets the store's entitlement, without which there's no account
to see.

```toml
[icloud]
containers = ["iCloud.com.example.journal"]
driveName = "Journal"
```

| Key | Type | Default | What it is |
| --- | --- | --- | --- |
| `containers` | strings | `["iCloud.<id>"]` | The app's iCloud containers, the first its default (`container()`, `documents()`). |
| `driveName` | string | the app's `name` | The first container's folder in iCloud Drive. |
| `driveHidden` | boolean | `false` | Leaves the app's documents out of iCloud Drive: they sync, but only the app sees them. |

For a device or a release build, the app's App ID needs the iCloud
capability at developer.apple.com (Identifiers): with iCloud Documents
and the app's containers for documents. Xcode makes it so when it makes
the development profile `asuka dev` uses on an iPhone. A profile with
iCloud has wildcards (its services' `*`, the store's `TEAMID.*`), which
the build signs as the values the app uses, as Xcode does. `asuka
build` says what a profile lacks: the capability, or a container.

iCloud Drive picks up a new folder, or a new name, with a new
`build` number: increase it after changing `icloud`.

## In asuka dev

iCloud keeps an app's values and documents under its own ID.
`asuka dev`'s player is built as the app, with its ID and the
entitlements of what it calls, so iCloud works in it as in a build, on
the simulator and on an iPhone. To try iCloud on the simulator, sign in
to an Apple Account in its Settings, with iCloud Drive on.

On a Mac, iCloud's entitlements need the app signed with a
provisioning profile (`asuka build --profile`, or `asuka.toml`'s
`mac.profile`), which `asuka dev` doesn't do: without one, the calls
throw and say why.

## On each platform

| Platform | iCloud |
| --- | --- |
| iOS | NSUbiquitousKeyValueStore, the app's ubiquity containers, NSMetadataQuery, NSFileCoordinator and NSFileVersion |
| macOS | The same, in the app's Swift library, which the build makes for an app that imports the package. Needs a provisioning profile with iCloud |
| Android, Windows, Linux, the web | None: "icloud: not available". iCloud for Windows syncs the user's iCloud Drive, but gives apps neither the store nor their containers, and CloudKit JS has neither on the web |

The package doesn't use CloudKit's databases (records, queries, sharing).

## Testing

`asuka test` has no iCloud: the account isn't available, the lists are
empty, a `Value` keeps its default and what the test sets it to, and the
calls throw "icloud: not available", as on Android. `icloud.fake()`
installs one a test drives, signed in: its container is a folder of the
app's (`path("Documents/a.txt")` is a file in it), and the test plays
the user's other devices: `arrive(rel, data)` is another device's new
file, listed and not yet downloaded; `change(key, value)` another
device's change of a `Value`; `conflict(rel, data)` another device's
version of a file; `signOut()` and `signIn(account)` the user's
account.

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
