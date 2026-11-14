// Package icloudspec is the "icloud" module's wire (asuka/sdk/icloud):
// JSON, the same from every host (the iOS host's and the Mac's Swift,
// host/fake's; the others have no iCloud), so the Go hosts and the
// package share it. Times are milliseconds since 1970; paths, absolute.
//
// The methods (args → answer):
//
//	account                → Account
//	kvGet      key         → the value's JSON; nothing if the key has none
//	kvSet      KVSet       → nothing (null deletes)
//	kvDelete   key         → nothing
//	kvAll                  → {key: value}
//	kvSync                 → nothing
//	container  id          → its directory's path ("" id: the app's first container)
//	files      Query       → [File] (the query gathered once)
//	watch      Query       → nothing: the query runs, its lists come as "files" events
//	unwatch    Query       → nothing
//	download   path        → nothing (it starts)
//	evict      path        → nothing
//	read       path        → the file's bytes (coordinated: downloaded first)
//	write      u32 n, path (n bytes), the bytes → nothing (coordinated; the directories made)
//	delete     path        → nothing (coordinated)
//	move       Move        → nothing (coordinated)
//	moveIn     Move        → nothing (a local file moved into iCloud: From local, To in a container)
//	moveOut    Move        → nothing (out of iCloud: From in a container, To local)
//	versions   path        → [Version] (its conflicts not yet resolved)
//	resolve    Resolve     → nothing
//	link       path        → Link
//	next                   → Event (one; "stopped" after stop)
//	stop                   → nothing
//
// Errors start with what the package maps: "not available" (no iCloud
// here), "no account" (the user isn't signed in, or turned iCloud off for
// the app), "not exist" (no such file), "not a container" (a path outside
// the app's containers).
package icloudspec

import "errors"

// Error prefixes.
const (
	ErrNotAvailable = "not available"
	ErrNoAccount    = "no account"
	ErrNotExist     = "not exist"
	ErrNotContainer = "not a container"
)

// Account is the iCloud account the device is signed in to, as the app
// sees it.
type Account struct {
	// Available: signed in, with iCloud on for the app.
	Available bool `json:"available"`
	// ID stands for the account, for the app to tell one from another (a
	// hash of its identity token); "" when not Available.
	ID string `json:"id,omitempty"`
}

// KVSet sets a key's value: JSON, which the host keeps as a property
// list (null deletes the key; within objects a null is left out).
type KVSet struct {
	Key   string `json:"key"`
	Value any    `json:"value"`
}

// Scopes of a Query.
const (
	Documents = "documents" // in the containers' Documents: what iCloud Drive shows
	Data      = "data"      // the rest of the containers
)

// Query asks for the files of a scope.
type Query struct {
	Scope string `json:"scope"`
}

// Download statuses.
const (
	NotDownloaded = "notDownloaded"
	Downloaded    = "downloaded" // an older version than iCloud's
	Current       = "current"
)

// File is a file (or folder) in iCloud, downloaded or not.
type File struct {
	Path     string `json:"path"`
	Name     string `json:"name"`
	Dir      bool   `json:"dir,omitempty"`
	Size     int64  `json:"size"`
	Created  int64  `json:"created,omitempty"`
	Modified int64  `json:"modified,omitempty"`
	Status   string `json:"status"`
	// Downloading and Uploading: under way, Downloaded and Uploaded how
	// far (0 to 1). Uploaded: on iCloud's servers.
	Downloading     bool    `json:"downloading,omitempty"`
	DownloadPercent float64 `json:"downloadPercent,omitempty"`
	Uploading       bool    `json:"uploading,omitempty"`
	Uploaded        bool    `json:"uploaded,omitempty"`
	UploadPercent   float64 `json:"uploadPercent,omitempty"`
	Conflicts       bool    `json:"conflicts,omitempty"`
	DownloadError   string  `json:"downloadError,omitempty"`
	UploadError     string  `json:"uploadError,omitempty"`
}

// Move moves a file from one path to another.
type Move struct {
	From string `json:"from"`
	To   string `json:"to"`
}

// Version is a version of a file in conflict with the current one: what
// another device saved.
type Version struct {
	// ID is the version's, for Resolve.
	ID string `json:"id"`
	// Path is where its contents are, to read.
	Path     string `json:"path"`
	Name     string `json:"name,omitempty"`
	Modified int64  `json:"modified,omitempty"`
	// Device is the name of the device that saved it.
	Device string `json:"device,omitempty"`
}

// Resolve settles a file's conflicts: Keep is the version that becomes
// the file's ("" keeps the current one); the others are dropped.
type Resolve struct {
	Path string `json:"path"`
	Keep string `json:"keep,omitempty"`
}

// Link is a file's public link, which anyone can download it from.
type Link struct {
	URL     string `json:"url"`
	Expires int64  `json:"expires,omitempty"`
}

// KV change reasons.
const (
	ServerChange   = "server"      // another device changed values
	InitialSync    = "initialSync" // the first download, the store having been empty
	QuotaViolation = "quota"       // the app's values are over iCloud's limit
	AccountChange  = "account"     // the account changed: its values replace the last one's
)

// Event is what the host says (next).
type Event struct {
	// Type is "account", "kv" or "files".
	Type    string   `json:"type"`
	Account *Account `json:"account,omitempty"`
	// A kv event's reason and keys.
	Reason string   `json:"reason,omitempty"`
	Keys   []string `json:"keys,omitempty"`
	// A files event's scope and its files.
	Scope string `json:"scope,omitempty"`
	Files []File `json:"files,omitempty"`
}

// Methods are the module's.
var Methods = []string{"account", "kvGet", "kvSet", "kvDelete", "kvAll", "kvSync", "container", "files", "watch", "unwatch",
	"download", "evict", "read", "write", "delete", "move", "moveIn", "moveOut", "versions", "resolve", "link", "next", "stop"}

// Unavailable is the "icloud" module of a host whose platform has no
// iCloud (Android, Windows, Linux, the web, a Mac app run unbuilt): every
// method failing "not available: " and why; stop answering.
func Unavailable(why string) map[string]func([]byte, func([]byte, error)) {
	fail := func(_ []byte, reply func([]byte, error)) { reply(nil, errors.New(ErrNotAvailable+": "+why)) }
	m := map[string]func([]byte, func([]byte, error)){}
	for _, k := range Methods {
		m[k] = fail
	}
	m["stop"] = func(_ []byte, reply func([]byte, error)) { reply(nil, nil) }
	return m
}

// WriteArgs are write's arguments: the path's length, the path, the data.
func WriteArgs(path string, data []byte) []byte {
	b := make([]byte, 0, 4+len(path)+len(data))
	n := uint32(len(path))
	b = append(b, byte(n), byte(n>>8), byte(n>>16), byte(n>>24))
	b = append(b, path...)
	return append(b, data...)
}

// ReadWriteArgs splits write's arguments.
func ReadWriteArgs(b []byte) (path string, data []byte, ok bool) {
	if len(b) < 4 {
		return "", nil, false
	}
	n := int(uint32(b[0]) | uint32(b[1])<<8 | uint32(b[2])<<16 | uint32(b[3])<<24)
	if len(b) < 4+n {
		return "", nil, false
	}
	return string(b[4 : 4+n]), b[4+n:], true
}
