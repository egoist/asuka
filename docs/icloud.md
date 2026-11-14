# iCloud

`asuka/sdk/icloud` is the user's iCloud, for an app on iOS and macOS. It
has two parts. The **key-value store** keeps small values (settings, the
page last read, a streak) that follow the user to each of their devices.
The **documents** are the app's files in iCloud Drive, which the Files
app and Finder show in a folder with the app's name. It's what
NSUbiquitousKeyValueStore and an app's ubiquity container are in Swift,
and what react-native-cloud-storage and expo-icloud-storage give an
Expo app.

```go
size := icloud.Signal("textSize", 17.0) // the same on every device

dir, err := icloud.Documents()          // the app's folder in iCloud Drive
err = icloud.WriteFile(filepath.Join(dir, "2026-11-15.txt"), []byte("Rain."))
files := icloud.UseFiles(icloud.DocumentsScope) // the list, kept up to date
```

iCloud is Apple's. On Android, Windows, Linux and the web, every call
fails with `icloud.ErrNotAvailable`: sync there through your own server.

[examples/journal](../examples/journal) keeps a journal in iCloud Drive,
one text file a day, with its text size in the key-value store.

## The account

`icloud.CurrentAccount` says whether the user is signed in to iCloud
with iCloud on for the app (`Available`), and which account it is
(`ID`); not on the main thread. `icloud.UseAccount` is the same as a
signal for a component, which follows the account while the component
lives. `icloud.WatchAccount` calls a function with each change: the user
signs in or out, turns iCloud off for the app, or another user signs in.

```go
account := icloud.UseAccount()
ui.Show(signal.Func(func() bool { return !account.Get().Available }), func() ui.Node {
	return ui.StaticText("Sign in to iCloud to keep your journal on all your devices.")
})
```

`ID` is a hash of iCloud's identity token. On a device, the same account
always has the same ID (it isn't meant to be compared between devices).
Keep it, and when the app starts with another, set aside what the last
user's iCloud gave, as Apple advises.

Without an account, the documents' calls fail with `icloud.ErrNoAccount`.
The key-value store keeps working on the device, and syncs once the
user signs in.

## The key-value store

`Get`, `Set`, `Delete` and `All` read and write values by key, not on
the main thread. A value is anything `encoding/json` encodes. `Set`
keeps it on the device at once and on the user's other devices as iCloud
syncs. A `nil` deletes the key.

```go
icloud.Set("lastPage", 42)
var page int
found, err := icloud.Get("lastPage", &page)
```

`icloud.Signal(key, def)` is a key's value as a signal, for a component,
as `asuka/sdk/store`'s `Signal` is for the device's values. It starts
with `def`, and then has the value iCloud keeps, once read, unless the
app set it meanwhile. Each value the app sets is kept. It follows the
changes other devices make (and goes back to `def` if one deletes the
key), while the component lives. A value it takes from iCloud isn't
written back.

`icloud.OnChange` calls a function, on the main thread, with each change
iCloud makes. A change has a `Reason` and the keys it changed:

| Reason | What happened |
| --- | --- |
| `ServerChange` | Another device changed the values |
| `InitialSync` | The first download, the app's store having been empty: a new device, the app installed again |
| `QuotaViolation` | The app's values are over iCloud's limit: delete some |
| `AccountChange` | Another account signed in, and its values replaced the last one's |

When two devices set a key, the last one wins. A device that was offline
gets the others' changes once it's back online, and `OnChange` hears of
them then. `Sync` writes the device's changes to its disk and has iCloud
look for the others' sooner. iCloud syncs by itself, so call it only
when the user asks (pulling to refresh).

The store is for small values. A key is at most 64 bytes and a value
1 MB: `Set` fails with `ErrKeyTooLong` and `ErrTooLarge` beyond them. An
app's values are at most 1 MB and 1024 keys in all: past that, iCloud
stops syncing them, and `OnChange` hears `QuotaViolation`. For more, use
documents.

Values are kept as property lists, as the app's Swift, a widget or a
watch app reads them: JSON's strings, numbers, booleans, arrays and
objects as theirs. A null inside an object is left out, which decodes
the same in Go. A null inside an array can't be kept. A value Swift code
(the app's own, its widget's) wrote as data or a date comes as base64 and
as RFC 3339 text, which Go decodes into a `[]byte` and a `time.Time`.

## Documents

An app's files in iCloud live in its **containers**: directories on the
device that iCloud syncs. `icloud.Container(id)` is a container's
directory, and with `""`, the app's first. By default the app has one,
`iCloud.<the app's ID>`; `asuka.json`'s `icloud.containers` names others,
the first its default. The first call may take a while, as iCloud sets
the container up.

`icloud.Documents()` is the first container's `Documents` directory,
made if it isn't there. What's in it shows in iCloud Drive (the Files
app on iOS, Finder on the Mac) in a folder with the app's name, which
the user can open, rename and move files in. The rest of a container is
the app's alone.

### Listing files

`icloud.Files(scope)` lists the app's files: `DocumentsScope` lists those
in its containers' `Documents`, and `DataScope` the rest. The list
includes the files other devices made that aren't downloaded yet.
`WatchFiles` calls a function with the list and each time it changes:
a file comes from another device, one is added, removed, renamed,
downloaded or uploaded, or a download goes on. `UseFiles` is the list as a
signal for a component. Both are iCloud's metadata query
(NSMetadataQuery), started for the first watcher of a scope and stopped
with the last.

A `File` has its `Path`, `Name`, `Size`, `Created` and `Modified`, and
where it is:

| Field | What it says |
| --- | --- |
| `Status` | `NotDownloaded` (only in iCloud), `Downloaded` (an older version than iCloud's), `Current` |
| `Downloading`, `DownloadProgress` | It's coming, and how much of it has (0 to 1) |
| `Uploading`, `UploadProgress`, `InCloud` | It's going to iCloud, how much of it has, and whether iCloud has it |
| `Conflicts` | Other devices saved versions of it at the same time |
| `DownloadError`, `UploadError` | Why the last one failed: no room on the device, iCloud full |

### Reading and writing

Read and write the app's files with `ReadFile`, `WriteFile`, `Remove`
and `Rename`, not on the main thread. They coordinate with iCloud's own
reads and writes (NSFileCoordinator), so a sync never sees half a file,
and the app never reads one iCloud is replacing. `ReadFile` downloads a
file that isn't on the device first, and waits for it. `WriteFile` writes
the whole file at once and makes its directories, and iCloud uploads it.
The `os` package's functions work on a downloaded file too, but may race
with a sync.

`Download` starts downloading a file (or what's in a folder), and its
`File` shows how it goes. `Evict` takes a file off the device and keeps
it in iCloud, to free room: it's downloaded again when it's read.

`MoveIn(local, path)` moves a file of the device's into iCloud, at
`path` in one of the app's containers. It leaves where it was, and shows
up on the user's other devices. `MoveOut(path, local)` moves one out of iCloud
onto the device, and it's gone from the user's other devices. A path
outside the app's containers fails with `ErrNotContainer`, and a file
that isn't there with an error that `errors.Is(err, fs.ErrNotExist)`.

### Conflicts

When two devices change a file at the same time (or one offline), iCloud
keeps one as the file and the others as versions in conflict with it,
and the file's `Conflicts` is true. `icloud.Conflicts(path)` gives those
versions: each one's `Path` to read it from, its `Modified`, and the name
of the `Device` that saved it. `icloud.Resolve(path, keep)` settles them:
`keep` becomes the file, or with `nil` the file stays as it is, and the
other versions are dropped on every device. To merge, write the merge
with `WriteFile`, then `Resolve(path, nil)`.

```go
vs, _ := icloud.Conflicts(path)
if len(vs) > 0 {
	icloud.Resolve(path, &vs[0]) // keep the other device's
}
```

### Sharing a link

`icloud.Link(path)` is a public link to a file, uploaded first if it
isn't yet, and when it expires. Anyone with the link can download a copy
of the file as it is now.

## asuka.json and entitlements

What an app is given follows what it calls, found in its Go code. One
that calls the key-value store's functions gets its entitlement
(`com.apple.developer.ubiquity-kvstore-identifier`, the team's ID and
the app's). One that calls the documents' functions gets iCloud
Documents (`com.apple.developer.icloud-services`: `CloudDocuments`) and
its containers, and its Info.plist's `NSUbiquitousContainers` shows the
first one's `Documents` in iCloud Drive. One that only asks about the
account gets the store's entitlement, without which there's no account
to see.

```json
"icloud": {
  "containers": ["iCloud.com.example.journal"],
  "driveName": "Journal"
}
```

| Key | Type | Default | What it is |
| --- | --- | --- | --- |
| `containers` | strings | `["iCloud.<id>"]` | The app's iCloud containers, the first its default (`Container("")`, `Documents`). |
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

iCloud keeps an app's values and documents under its own ID, which
the player's isn't. So `asuka dev` builds an app that imports the
package rather than run it in the player, on the simulator and on an
iPhone, and says so. To try iCloud on the simulator, sign in to an
Apple Account in its Settings, with iCloud Drive on.

On a Mac, iCloud's entitlements need the app signed with a
provisioning profile (`asuka build --profile`, or `asuka.json`'s
`mac.profile`), which `asuka dev` doesn't do: without one, the calls fail
with `ErrNotAvailable` and say why.

## On each platform

| Platform | iCloud |
| --- | --- |
| iOS | NSUbiquitousKeyValueStore, the app's ubiquity containers, NSMetadataQuery, NSFileCoordinator and NSFileVersion |
| macOS | The same, in the app's Swift library, which the build makes for an app that imports the package. Needs a provisioning profile with iCloud |
| Android, Windows, Linux, the web | None: `ErrNotAvailable`. iCloud for Windows syncs the user's iCloud Drive, but gives apps neither the store nor their containers, and CloudKit JS has neither on the web |

The package doesn't use CloudKit's databases (records, queries, sharing).

## Testing

`h.ICloud()` gives the fake host an iCloud (`fake.ICloud`) that the test
plays the other devices for. Its containers are directories under
`Root`, and the app reads and writes them as on a device. `Change` sets a
value as another device does. `Arrive` brings a file that's listed but
not downloaded. `Conflict` saves a version in conflict. `SignIn` and
`SignOut` change the account, and `ExceedQuota` goes over the store's
limit. Each reaches the app as an event, as on a device. `Value`,
`Downloaded`, `Watching` and `Path` check what the app did.

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

Without `h.ICloud()`, the fake host has no iCloud, and the calls fail
with `ErrNotAvailable`, as on Android.
