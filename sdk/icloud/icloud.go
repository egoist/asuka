// Package icloud is the user's iCloud, for an app on iOS and macOS: a
// key-value store that follows the user to each of their devices
// (NSUbiquitousKeyValueStore), and the app's documents in iCloud Drive
// (its ubiquity container), the files listed with their download and
// upload status, read and written in coordination with the system's sync,
// downloaded, evicted, their conflicts resolved, shared by a link.
//
//	theme := icloud.Signal("theme", "light") // the same on every device
//
//	dir, err := icloud.Documents() // what iCloud Drive shows of the app
//	err = icloud.WriteFile(filepath.Join(dir, "Note.txt"), []byte("Hello"))
//	files := icloud.UseFiles(icloud.DocumentsScope) // the list, kept up to date
//
// iCloud is Apple's: on Android, Windows, Linux and the web every call
// fails with ErrNotAvailable (sync there with your own server). Where
// there's iCloud, ErrNoAccount says the user isn't signed in, or turned
// iCloud off for the app: keep working on the device then, and sync once
// the account comes (WatchAccount).
//
// An app that uses the key-value store is given its entitlement, and one
// that uses documents the iCloud Documents service and its containers
// (asuka.json's icloud, by default iCloud.<the app's ID>). Its App ID
// needs the iCloud capability, with those containers, for a device or a
// release build (developer.apple.com). asuka dev builds such an app
// rather than run it in the player: its iCloud is its own.
package icloud

import (
	"encoding/json"
	"errors"
	"io/fs"
	"strings"
	"sync"

	spec "github.com/egoist/asuka/internal/icloudspec"
	"github.com/egoist/asuka/signal"
	"github.com/egoist/asuka/ui"
)

// Errors the package reports.
var (
	// ErrNotAvailable: no iCloud on this platform (or, on a Mac, an app
	// run with go run rather than built: its Swift has iCloud).
	ErrNotAvailable = errors.New("icloud: not available")
	// ErrNoAccount: the user isn't signed in to iCloud, or turned iCloud
	// off for the app (iCloud Drive, for documents).
	ErrNoAccount = errors.New("icloud: no account")
	// ErrKeyTooLong: a key over 64 bytes, iCloud's limit.
	ErrKeyTooLong = errors.New("icloud: a key is at most 64 bytes")
	// ErrTooLarge: a value over 1 MB, all the store holds.
	ErrTooLarge = errors.New("icloud: a value is at most 1 MB")
	// ErrNotContainer: a path outside the app's iCloud containers.
	ErrNotContainer = errors.New("icloud: not in the app's iCloud containers")
)

// The module's name.
const module = "icloud"

// call runs one of the module's methods: every call of the package's
// goes through it. Not on the main thread.
//
// Never inlined: asuka build looks for its symbol (cmd/asuka iosFeatures).
//
//go:noinline
func call(method string, args []byte) ([]byte, error) {
	b, err := ui.CallWait(module, method, args)
	if err != nil {
		return nil, errorOf(err)
	}
	return b, nil
}

// post runs one of the module's methods from the main thread, its answer
// to done (if not nil) on the main thread: calls posted so keep their
// order.
//
// Never inlined: asuka build looks for its symbol (cmd/asuka iosFeatures).
//
//go:noinline
func post(method string, args []byte, done func([]byte, error)) {
	ui.Call(module, method, args, func(b []byte, err error) {
		if done != nil {
			if err != nil {
				err = errorOf(err)
			}
			done(b, err)
		}
	})
}

// errorOf is a host's error as the package's.
func errorOf(err error) error {
	m := err.Error()
	switch {
	case strings.HasPrefix(m, spec.ErrNotAvailable), strings.HasPrefix(m, "no module"):
		return ErrNotAvailable
	case strings.HasPrefix(m, spec.ErrNoAccount):
		return ErrNoAccount
	case strings.HasPrefix(m, spec.ErrNotExist):
		return fs.ErrNotExist
	case strings.HasPrefix(m, spec.ErrNotContainer):
		return ErrNotContainer
	}
	return errors.New("icloud: " + m)
}

// Account is the iCloud account the device is signed in to, as the app
// sees it.
type Account struct {
	// Available: signed in, with iCloud on for the app.
	Available bool
	// ID stands for the account: another ID is another user (keep it,
	// and at launch drop what was the last user's); "" when not
	// Available.
	ID string
}

func accountOf(b []byte) (Account, error) {
	var a spec.Account
	if err := json.Unmarshal(b, &a); err != nil {
		return Account{}, err
	}
	return Account{Available: a.Available, ID: a.ID}, nil
}

// CurrentAccount is the account now. Not on the main thread.
func CurrentAccount() (Account, error) {
	b, err := call("account", nil)
	if err != nil {
		return Account{}, err
	}
	return accountOf(b)
}

// The watchers of the host's events (follow), and the files each scope's
// watch last listed. The watchers come and go on the main thread, where
// the events are asked for and handed out.
var (
	mu             sync.Mutex
	accountWatches = map[*func(Account)]bool{}
	changeWatches  = map[*func(Change)]bool{}
	fileWatches    = map[Scope]map[*func([]File)]bool{}
	lastFiles      = map[Scope][]File{}
	polling        bool
)

// watching reports whether anyone watches. mu is held.
func watching() bool {
	return len(accountWatches) > 0 || len(changeWatches) > 0 || len(fileWatches) > 0
}

// listen has follow running, as a watcher was added. mu isn't held.
func listen() {
	mu.Lock()
	start := !polling
	polling = true
	mu.Unlock()
	if start {
		follow()
	}
}

// unlisten stops the host's events once no one watches. mu isn't held.
func unlisten() {
	mu.Lock()
	again := watching()
	mu.Unlock()
	if !again {
		post("stop", nil, nil)
	}
}

// WatchAccount calls f with the account now and each time it changes (the
// user signs in or out, turns iCloud off for the app, another user signs
// in), until stop. Where there's no iCloud, f isn't called. Main thread.
func WatchAccount(f func(Account)) (stop func()) {
	fp := &f
	mu.Lock()
	accountWatches[fp] = true
	mu.Unlock()
	listen()
	post("account", nil, func(b []byte, err error) {
		a, err2 := accountOf(b)
		if err == nil && err2 == nil && live(accountWatches, fp) {
			f(a)
		}
	})
	var once sync.Once
	return func() {
		once.Do(func() {
			mu.Lock()
			delete(accountWatches, fp)
			mu.Unlock()
			unlisten()
		})
	}
}

// UseAccount is the account as a signal, for a component: it follows
// changes while the component lives (not Available until the first
// answer, and where there's no iCloud). Main thread.
func UseAccount() signal.Accessor[Account] {
	s := signal.New(Account{})
	signal.OnCleanup(WatchAccount(s.Set))
	return s
}

// follow asks the host for the next event ("next" answers with one),
// hands it to the watchers, and asks again, while someone watches. It
// runs on the main thread, call after call: a root disposed answers the
// one waiting, which ends it.
func follow() {
	post("next", nil, func(b []byte, err error) {
		mu.Lock()
		// "stopped": the last watcher went as another came: ask again.
		if !watching() || err != nil && err.Error() != "icloud: stopped" {
			polling = false
			mu.Unlock()
			return
		}
		mu.Unlock()
		var e spec.Event
		if err == nil && json.Unmarshal(b, &e) == nil {
			dispatch(e)
		}
		follow()
	})
}

// dispatch hands an event to its watchers, on the main thread: each one
// still watching as it comes.
func dispatch(e spec.Event) {
	switch e.Type {
	case "account":
		if e.Account == nil {
			return
		}
		a := Account{Available: e.Account.Available, ID: e.Account.ID}
		for _, f := range current(accountWatches) {
			if live(accountWatches, f) {
				(*f)(a)
			}
		}
	case "kv":
		c := Change{Reason: reasonOf(e.Reason), Keys: e.Keys}
		for _, f := range current(changeWatches) {
			if live(changeWatches, f) {
				(*f)(c)
			}
		}
	case "files":
		scope := scopeOf(e.Scope)
		files := filesOf(e.Files)
		mu.Lock()
		if fileWatches[scope] == nil {
			mu.Unlock()
			return
		}
		lastFiles[scope] = files
		fs := make([]*func([]File), 0, len(fileWatches[scope]))
		for f := range fileWatches[scope] {
			fs = append(fs, f)
		}
		mu.Unlock()
		for _, f := range fs {
			mu.Lock()
			ok := fileWatches[scope][f]
			mu.Unlock()
			if ok {
				(*f)(files)
			}
		}
	}
}

// current is the watchers of m now.
func current[F any](m map[*F]bool) []*F {
	mu.Lock()
	defer mu.Unlock()
	out := make([]*F, 0, len(m))
	for f := range m {
		out = append(out, f)
	}
	return out
}

func live[F any](m map[*F]bool, f *F) bool {
	mu.Lock()
	defer mu.Unlock()
	return m[f]
}
