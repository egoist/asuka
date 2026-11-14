package icloud_test

import (
	"errors"
	"io/fs"
	"os"
	"path/filepath"
	"runtime"
	"slices"
	"testing"
	"time"

	"github.com/egoist/asuka/host/fake"
	"github.com/egoist/asuka/sdk/icloud"
	"github.com/egoist/asuka/signal"
	"github.com/egoist/asuka/ui"
)

// mount mounts root on a fake host with iCloud, and returns it and what
// runs the main thread's posted work until cond holds.
func mount(t *testing.T, root func() ui.Node) (*fake.ICloud, *ui.Root, func(what string, cond func() bool)) {
	t.Helper()
	h := fake.New()
	ic := h.ICloud()
	ic.Root = t.TempDir()
	r := ui.Mount(h, root)
	t.Cleanup(r.Dispose)
	until := func(what string, cond func() bool) {
		t.Helper()
		deadline := time.Now().Add(10 * time.Second)
		for {
			h.RunPosted()
			r.Flush()
			if cond() {
				return
			}
			if time.Now().After(deadline) {
				t.Fatalf("timed out waiting for %s", what)
			}
			time.Sleep(time.Millisecond)
		}
	}
	return ic, r, until
}

// background runs f on a goroutine (the package's calls wait for the
// main thread) and reports when it's done.
func background(f func()) func() bool {
	done := make(chan struct{})
	go func() {
		defer close(done)
		f()
	}()
	return func() bool {
		select {
		case <-done:
			return true
		default:
			return false
		}
	}
}

type prefs struct {
	Theme string   `json:"theme"`
	Tags  []string `json:"tags"`
}

// Values are kept as JSON, read back, listed and deleted; keys and values
// over iCloud's limits are refused before they reach it.
func TestKeyValue(t *testing.T) {
	ic, _, until := mount(t, func() ui.Node { return ui.StaticText("app") })
	var (
		found, missing bool
		got            prefs
		all            map[string][]byte
		errs           []error
	)
	until("the calls", background(func() {
		errs = append(errs, icloud.Set("prefs", prefs{Theme: "dark"}))
		var err error
		found, err = icloud.Get("prefs", &got)
		errs = append(errs, err)
		var n int
		missing, err = icloud.Get("none", &n)
		errs = append(errs, err)
		errs = append(errs, icloud.Set("count", 3), icloud.Delete("count"), icloud.Sync())
		m, err := icloud.All()
		errs = append(errs, err)
		all = map[string][]byte{}
		for k, v := range m {
			all[k] = v
		}
	}))
	for _, err := range errs {
		if err != nil {
			t.Fatal(err)
		}
	}
	if !found || missing || got.Theme != "dark" {
		t.Fatalf("found %v %+v, missing %v", found, got, missing)
	}
	if v, _ := ic.Value("prefs"); string(v) != `{"theme":"dark","tags":null}` {
		t.Errorf("kept %s", v)
	}
	if _, ok := all["count"]; ok || len(all) != 1 {
		t.Errorf("all: %s", all)
	}
	var long, large error
	until("the limits", background(func() {
		long = icloud.Set("a key of more than sixty-four bytes, which iCloud's store refuses", 1)
		large = icloud.Set("big", make([]byte, 1<<20))
	}))
	if long != icloud.ErrKeyTooLong || large != icloud.ErrTooLarge {
		t.Errorf("limits: %v, %v", long, large)
	}
}

// Signal starts with its default, takes the kept value, writes what the
// app sets, follows another device's changes without writing them back,
// and stops following with its component.
func TestSignal(t *testing.T) {
	shown := signal.New(true)
	var theme *signal.Signal[string]
	var ic *fake.ICloud
	var r *ui.Root
	var until func(string, func() bool)
	ic, r, until = mount(t, func() ui.Node {
		return ui.Show(shown, func() ui.Node {
			theme = icloud.Signal("theme", "light")
			return ui.Text(signal.Func(theme.Get))
		})
	})
	if theme.Peek() != "light" {
		t.Fatalf("starts %q", theme.Peek())
	}
	theme.Set("dark")
	r.Flush()
	until("the write", func() bool { v, _ := ic.Value("theme"); return string(v) == `"dark"` })
	ic.Change("theme", "sepia")
	until("the change", func() bool { return theme.Peek() == "sepia" })
	ic.Change("theme", nil)
	until("the deletion", func() bool { return theme.Peek() == "light" })
	if _, ok := ic.Value("theme"); ok {
		t.Error("the default was written back")
	}
	shown.Set(false)
	r.Flush()
	ic.Change("theme", "dark")
	until("the change to pass", func() bool { return true })
	if theme.Peek() != "light" {
		t.Errorf("followed after disposal: %q", theme.Peek())
	}

	// A value kept before: the signal takes it as it's read.
	shown.Set(true)
	r.Flush()
	until("the kept value", func() bool { return theme.Peek() == "dark" })
}

// OnChange gets the changes iCloud makes, with their reasons; an account
// change swaps the values, and WatchAccount sees the accounts.
func TestChangesAndAccount(t *testing.T) {
	ic, _, until := mount(t, func() ui.Node { return ui.StaticText("app") })
	var changes []icloud.Change
	var accounts []icloud.Account
	stopChanges := icloud.OnChange(func(c icloud.Change) { changes = append(changes, c) })
	stopAccount := icloud.WatchAccount(func(a icloud.Account) { accounts = append(accounts, a) })
	until("the account", func() bool { return len(accounts) == 1 })
	if a := accounts[0]; !a.Available || a.ID != "test" {
		t.Fatalf("account %+v", a)
	}
	ic.Change("score", 10)
	ic.ExceedQuota()
	until("the changes", func() bool { return len(changes) == 2 })
	if c := changes[0]; c.Reason != icloud.ServerChange || !slices.Equal(c.Keys, []string{"score"}) {
		t.Errorf("change %+v", c)
	}
	if changes[1].Reason != icloud.QuotaViolation {
		t.Errorf("quota %+v", changes[1])
	}
	ic.SignIn("other")
	until("the account change", func() bool { return len(changes) == 3 && len(accounts) == 2 })
	if changes[2].Reason != icloud.AccountChange || accounts[1].ID != "other" {
		t.Errorf("after sign in: %+v, %+v", changes[2], accounts[1])
	}
	ic.SignOut()
	until("the sign out", func() bool { return len(accounts) == 3 })
	if accounts[2].Available {
		t.Errorf("signed out: %+v", accounts[2])
	}
	var err error
	until("a container", background(func() { _, err = icloud.Container("") }))
	if err != icloud.ErrNoAccount {
		t.Errorf("container signed out: %v", err)
	}
	stopChanges()
	stopAccount()
	ic.SignIn("test")
	until("nothing", func() bool { return true })
	if len(accounts) != 3 {
		t.Errorf("watched after stop: %+v", accounts)
	}
}

// Files are written, listed (with another device's, not downloaded),
// read (downloading it), evicted, renamed and removed; WatchFiles follows
// each change.
func TestDocuments(t *testing.T) {
	ic, _, until := mount(t, func() ui.Node { return ui.StaticText("app") })
	var dir string
	var err error
	until("the documents", background(func() { dir, err = icloud.Documents() }))
	if err != nil || dir != ic.Path("Documents") {
		t.Fatalf("documents %q, %v", dir, err)
	}
	var lists [][]icloud.File
	stop := icloud.WatchFiles(icloud.DocumentsScope, func(fs []icloud.File) { lists = append(lists, fs) })
	until("the first list", func() bool { return len(lists) == 1 })
	if len(lists[0]) != 0 {
		t.Fatalf("first list %+v", lists[0])
	}
	note := filepath.Join(dir, "Note.txt")
	until("the write", background(func() { err = icloud.WriteFile(note, []byte("hello")) }))
	if err != nil {
		t.Fatal(err)
	}
	until("its listing", func() bool {
		l := lists[len(lists)-1]
		return len(l) == 1 && l[0].Path == note && l[0].Status == icloud.Current
	})

	ic.Arrive("Documents/Far.txt", []byte("from the iPad"))
	far := ic.Path("Documents/Far.txt")
	until("the other device's file", func() bool {
		l := lists[len(lists)-1]
		return len(l) == 2 && l[0].Path == far && l[0].Status == icloud.NotDownloaded && l[0].Size == 13
	})
	var b []byte
	until("the read", background(func() { b, err = icloud.ReadFile(far) }))
	if err != nil || string(b) != "from the iPad" || !ic.Downloaded("Documents/Far.txt") {
		t.Fatalf("read %q, %v", b, err)
	}
	until("the eviction", background(func() { err = icloud.Evict(far) }))
	if err != nil || ic.Downloaded("Documents/Far.txt") {
		t.Fatalf("evict: %v", err)
	}
	until("the download", background(func() { err = icloud.Download(far) }))
	if err != nil || !ic.Downloaded("Documents/Far.txt") {
		t.Fatalf("download: %v", err)
	}
	renamed := filepath.Join(dir, "Notes", "Renamed.txt")
	until("the rename", background(func() { err = icloud.Rename(note, renamed) }))
	if err != nil {
		t.Fatal(err)
	}
	var files []icloud.File
	until("the list", background(func() { files, err = icloud.Files(icloud.DocumentsScope) }))
	var paths []string
	for _, f := range files {
		paths = append(paths, f.Path)
	}
	if !slices.Equal(paths, []string{far, filepath.Join(dir, "Notes"), renamed}) {
		t.Errorf("files %v", paths)
	}
	until("the removal", background(func() { err = icloud.Remove(far) }))
	if err != nil {
		t.Fatal(err)
	}
	until("a missing file", background(func() { _, err = icloud.ReadFile(far) }))
	var pe *fs.PathError
	if !errors.As(err, &pe) || !errors.Is(err, fs.ErrNotExist) || pe.Path != far {
		t.Errorf("missing: %v", err)
	}
	until("outside", background(func() { err = icloud.WriteFile(filepath.Join(t.TempDir(), "x"), nil) }))
	if err != icloud.ErrNotContainer {
		t.Errorf("outside: %v", err)
	}
	n := len(lists)
	stop()
	until("the unwatch", func() bool { return !ic.Watching("documents") })
	until("a write", background(func() { icloud.WriteFile(filepath.Join(dir, "After.txt"), nil) }))
	if len(lists) != n {
		t.Error("listed after stop")
	}
}

// A file moves into iCloud and out; a conflict is listed, its version
// read, and resolved by keeping it.
func TestMovesAndConflicts(t *testing.T) {
	ic, _, until := mount(t, func() ui.Node { return ui.StaticText("app") })
	local := filepath.Join(t.TempDir(), "Draft.txt")
	os.WriteFile(local, []byte("draft"), 0o644)
	draft := ic.Path("Documents/Draft.txt")
	var err error
	until("the move in", background(func() { err = icloud.MoveIn(local, draft) }))
	if _, e := os.Stat(local); err != nil || e == nil || !ic.Downloaded("Documents/Draft.txt") {
		t.Fatalf("move in: %v", err)
	}
	ic.Conflict("Documents/Draft.txt", []byte("theirs"), "Ann's iPad")
	var files []icloud.File
	var versions []icloud.Version
	until("the conflicts", background(func() {
		files, _ = icloud.Files(icloud.DocumentsScope)
		versions, err = icloud.Conflicts(draft)
	}))
	if len(files) != 1 || !files[0].Conflicts || err != nil || len(versions) != 1 || versions[0].Device != "Ann's iPad" {
		t.Fatalf("files %+v, versions %+v, %v", files, versions, err)
	}
	if b, _ := os.ReadFile(versions[0].Path); string(b) != "theirs" {
		t.Errorf("version's contents %q", b)
	}
	until("the resolution", background(func() { err = icloud.Resolve(draft, &versions[0]) }))
	if b, _ := os.ReadFile(draft); err != nil || string(b) != "theirs" {
		t.Fatalf("resolved %q, %v", b, err)
	}
	until("the check", background(func() { versions, err = icloud.Conflicts(draft) }))
	if len(versions) != 0 {
		t.Errorf("still in conflict: %+v", versions)
	}
	var url string
	until("the link", background(func() { url, _, err = icloud.Link(draft) }))
	if err != nil || url == "" {
		t.Errorf("link %q, %v", url, err)
	}
	back := filepath.Join(t.TempDir(), "Back.txt")
	until("the move out", background(func() { err = icloud.MoveOut(draft, back) }))
	if b, _ := os.ReadFile(back); err != nil || string(b) != "theirs" || ic.Downloaded("Documents/Draft.txt") {
		t.Fatalf("move out %q, %v", b, err)
	}
}

// UseFiles follows the files while its component lives, and its watch
// ends with it.
func TestUseFiles(t *testing.T) {
	shown := signal.New(true)
	var files signal.Accessor[[]icloud.File]
	ic, r, until := mount(t, func() ui.Node {
		return ui.Show(shown, func() ui.Node {
			files = icloud.UseFiles(icloud.DataScope)
			return ui.StaticText("files")
		})
	})
	until("the watch", func() bool { return ic.Watching("data") && files.Get() != nil })
	ic.Arrive("Cache/state.json", []byte("{}"))
	until("the file", func() bool { return len(files.Get()) == 1 && files.Get()[0].Name == "state.json" })
	shown.Set(false)
	r.Flush()
	until("the unwatch", func() bool { return !ic.Watching("data") })
}

// Without iCloud, every call says so, and watchers hear nothing.
func TestNotAvailable(t *testing.T) {
	h := fake.New()
	r := ui.Mount(h, func() ui.Node { return ui.StaticText("app") })
	defer r.Dispose()
	goroutines := runtime.NumGoroutine()
	var errs []error
	done := background(func() {
		_, err := icloud.CurrentAccount()
		errs = append(errs, err, icloud.Set("k", 1))
		_, err = icloud.Documents()
		errs = append(errs, err)
	})
	stop := icloud.WatchAccount(func(icloud.Account) { t.Error("an account without iCloud") })
	deadline := time.Now().Add(10 * time.Second)
	for !done() || runtime.NumGoroutine() > goroutines {
		h.RunPosted()
		r.Flush()
		if time.Now().After(deadline) {
			t.Fatal("timed out")
		}
		time.Sleep(time.Millisecond)
	}
	stop()
	for _, err := range errs {
		if err != icloud.ErrNotAvailable {
			t.Errorf("err %v", err)
		}
	}
}
