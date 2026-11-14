package icloud

import (
	"encoding/json"
	"io/fs"
	"os"
	"path/filepath"
	"sync"
	"time"

	spec "github.com/egoist/asuka/internal/icloudspec"
	"github.com/egoist/asuka/signal"
)

// Documents: the app's files in iCloud Drive. They live in the app's
// ubiquity containers (Container), directories on the device that iCloud
// syncs. What's in a container's Documents shows in iCloud Drive (the
// Files app, Finder) in a folder with the app's name; the rest is the
// app's alone.
//
// A file of another device's is listed (Files) before it's downloaded:
// read it with ReadFile, which downloads it first, or Download it. Read
// and write files with ReadFile and WriteFile (or Remove, Rename), which
// coordinate with iCloud's own reads and writes; the os package's
// functions work on a file downloaded, but may race with a sync.

// Container is the directory of the app's ubiquity container id (as
// asuka.json's icloud.containers names it: "iCloud.com.example.app"), or
// with "" its first (by default iCloud.<the app's ID>). The first call
// may take a while, as iCloud sets the container up. Not on the main
// thread.
func Container(id string) (string, error) {
	b, err := call("container", []byte(id))
	return string(b), err
}

// Documents is the Documents directory of the app's first container
// (Container("")), made if it isn't there: what iCloud Drive shows of
// the app, in a folder with its name. Not on the main thread.
func Documents() (string, error) {
	c, err := Container("")
	if err != nil {
		return "", err
	}
	d := filepath.Join(c, "Documents")
	return d, os.MkdirAll(d, 0o755)
}

// Scope is which of the app's files a list has.
type Scope uint8

const (
	// DocumentsScope: those in its containers' Documents, which iCloud
	// Drive shows.
	DocumentsScope Scope = iota
	// DataScope: the rest of its containers.
	DataScope
)

func (s Scope) wire() string {
	if s == DataScope {
		return spec.Data
	}
	return spec.Documents
}

func scopeOf(s string) Scope {
	if s == spec.Data {
		return DataScope
	}
	return DocumentsScope
}

// Status is whether a file is on the device.
type Status uint8

const (
	// NotDownloaded: only in iCloud (Download, or ReadFile, gets it).
	NotDownloaded Status = iota
	// Downloaded: on the device, an older version than iCloud's.
	Downloaded
	// Current: on the device, the version iCloud has.
	Current
)

func (s Status) String() string {
	return [...]string{"not downloaded", "downloaded", "current"}[min(int(s), 2)]
}

// File is one of the app's files in iCloud, on the device or not.
type File struct {
	// Path is its path: where it is, or will be once downloaded.
	Path string
	// Name is what the user sees it as.
	Name string
	// Dir: a folder.
	Dir               bool
	Size              int64
	Created, Modified time.Time
	Status            Status
	// Downloading: it's coming, DownloadProgress of it so far (0 to 1).
	Downloading      bool
	DownloadProgress float64
	// Uploading: it's going to iCloud, UploadProgress of it so far (0 to
	// 1). InCloud: iCloud has it.
	Uploading      bool
	UploadProgress float64
	InCloud        bool
	// Conflicts: other devices saved versions of it at once
	// (Conflicts, Resolve).
	Conflicts bool
	// DownloadError and UploadError say why the last one failed (no
	// room on the device, iCloud full).
	DownloadError, UploadError error
}

func msTime(ms int64) time.Time {
	if ms == 0 {
		return time.Time{}
	}
	return time.UnixMilli(ms)
}

func errOf(s string) error {
	if s == "" {
		return nil
	}
	return errorOf(errorString(s))
}

type errorString string

func (e errorString) Error() string { return string(e) }

func filesOf(w []spec.File) []File {
	out := make([]File, len(w))
	for i, f := range w {
		st := NotDownloaded
		switch f.Status {
		case spec.Downloaded:
			st = Downloaded
		case spec.Current:
			st = Current
		}
		out[i] = File{Path: f.Path, Name: f.Name, Dir: f.Dir, Size: f.Size, Created: msTime(f.Created), Modified: msTime(f.Modified),
			Status: st, Downloading: f.Downloading, DownloadProgress: f.DownloadPercent / 100, Uploading: f.Uploading,
			UploadProgress: f.UploadPercent / 100, InCloud: f.Uploaded, Conflicts: f.Conflicts,
			DownloadError: errOf(f.DownloadError), UploadError: errOf(f.UploadError)}
	}
	return out
}

func query(s Scope) []byte {
	b, _ := json.Marshal(spec.Query{Scope: s.wire()})
	return b
}

// Files lists the app's files of a scope, downloaded or not, sorted by
// path. Not on the main thread.
func Files(scope Scope) ([]File, error) {
	b, err := call("files", query(scope))
	if err != nil {
		return nil, err
	}
	var w []spec.File
	if err := json.Unmarshal(b, &w); err != nil {
		return nil, err
	}
	return filesOf(w), nil
}

// WatchFiles calls f with the app's files of a scope and each time they
// change (a file comes from another device, one is added, removed,
// renamed, downloaded or uploaded, a download goes on), until stop. Where
// there's no iCloud, or no account, f isn't called. Main thread.
func WatchFiles(scope Scope, f func([]File)) (stop func()) {
	fp := &f
	mu.Lock()
	first := fileWatches[scope] == nil
	if first {
		fileWatches[scope] = map[*func([]File)]bool{}
	}
	fileWatches[scope][fp] = true
	last, had := lastFiles[scope]
	mu.Unlock()
	listen()
	if first {
		post("watch", query(scope), nil)
	} else if had {
		f(last)
	}
	var once sync.Once
	return func() {
		once.Do(func() {
			mu.Lock()
			delete(fileWatches[scope], fp)
			gone := len(fileWatches[scope]) == 0
			if gone {
				delete(fileWatches, scope)
				delete(lastFiles, scope)
			}
			mu.Unlock()
			if gone {
				post("unwatch", query(scope), nil)
			}
			unlisten()
		})
	}
}

// UseFiles is the app's files of a scope as a signal, for a component: it
// follows them while the component lives (nil until the first list).
// Main thread.
func UseFiles(scope Scope) signal.Accessor[[]File] {
	s := signal.NewFunc[[]File](nil, nil)
	signal.OnCleanup(WatchFiles(scope, s.Set))
	return s
}

// pathErr is err as an *fs.PathError of op and path when the file isn't
// there.
func pathErr(op, path string, err error) error {
	if err == fs.ErrNotExist {
		return &fs.PathError{Op: op, Path: path, Err: fs.ErrNotExist}
	}
	return err
}

// Download starts downloading the file at path (a folder: what's in it),
// if it isn't on the device; its File says how it goes. Not on the main
// thread.
func Download(path string) error {
	_, err := call("download", []byte(path))
	return pathErr("download", path, err)
}

// Evict takes the file at path off the device, keeping it in iCloud (to
// free room): it's downloaded again when read. Not on the main thread.
func Evict(path string) error {
	_, err := call("evict", []byte(path))
	return pathErr("evict", path, err)
}

// ReadFile reads the file at path, downloading it first if it isn't on
// the device (waiting for it). Not on the main thread.
func ReadFile(path string) ([]byte, error) {
	b, err := call("read", []byte(path))
	if err != nil {
		return nil, pathErr("read", path, err)
	}
	if b == nil {
		b = []byte{}
	}
	return b, nil
}

// WriteFile writes data to the file at path, all at once (as it was, or
// as written: never half), making its directories; iCloud uploads it.
// Not on the main thread.
func WriteFile(path string, data []byte) error {
	_, err := call("write", spec.WriteArgs(path, data))
	return pathErr("write", path, err)
}

// Remove removes the file (or folder, with what's in it) at path, from
// iCloud and every device. Not on the main thread.
func Remove(path string) error {
	_, err := call("delete", []byte(path))
	return pathErr("remove", path, err)
}

func move(method, from, to string) error {
	b, _ := json.Marshal(spec.Move{From: from, To: to})
	_, err := call(method, b)
	return pathErr(method, from, err)
}

// Rename moves the file at from to to, both in the app's containers,
// replacing what's there. Not on the main thread.
func Rename(from, to string) error { return move("move", from, to) }

// MoveIn moves a file (or folder) of the device's, local, into iCloud, at
// path in one of the app's containers: it's gone from where it was, and
// on the user's devices. Not on the main thread.
func MoveIn(local, path string) error { return move("moveIn", local, path) }

// MoveOut moves the file (or folder) at path out of iCloud to local, a
// path of the device's: it's gone from iCloud and the user's other
// devices. Not on the main thread.
func MoveOut(path, local string) error { return move("moveOut", path, local) }

// Version is a version of a file that conflicts with the file: one
// another device saved while this one did (or offline).
type Version struct {
	// ID is the version's, for Resolve.
	ID string
	// Path is where its contents are: read it (with os) to show it, or to
	// merge it into the file.
	Path string
	// Name is the file's name it was saved with; Modified, when; Device,
	// the name of the device that saved it ("Ann's iPhone").
	Name     string
	Modified time.Time
	Device   string
}

// Conflicts are the versions of the file at path in conflict with it, not
// yet resolved (its File's Conflicts). Not on the main thread.
func Conflicts(path string) ([]Version, error) {
	b, err := call("versions", []byte(path))
	if err != nil {
		return nil, pathErr("conflicts", path, err)
	}
	var w []spec.Version
	if err := json.Unmarshal(b, &w); err != nil {
		return nil, err
	}
	out := make([]Version, len(w))
	for i, v := range w {
		out[i] = Version{ID: v.ID, Path: v.Path, Name: v.Name, Modified: msTime(v.Modified), Device: v.Device}
	}
	return out, nil
}

// Resolve settles the conflicts of the file at path: keep becomes the
// file (nil keeps the file as it is: write the merge first, to merge),
// and the other versions are dropped, on every device. Not on the main
// thread.
func Resolve(path string, keep *Version) error {
	r := spec.Resolve{Path: path}
	if keep != nil {
		r.Keep = keep.ID
	}
	b, _ := json.Marshal(r)
	_, err := call("resolve", b)
	return pathErr("resolve", path, err)
}

// Link is a public link to the file at path, uploaded first if it isn't:
// anyone with it can download a copy of the file (as it is now), until
// expires (zero: never). Not on the main thread.
func Link(path string) (url string, expires time.Time, err error) {
	b, err := call("link", []byte(path))
	if err != nil {
		return "", time.Time{}, pathErr("link", path, err)
	}
	var l spec.Link
	if err := json.Unmarshal(b, &l); err != nil {
		return "", time.Time{}, err
	}
	return l.URL, msTime(l.Expires), nil
}
