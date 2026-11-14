package icloud

import (
	"bytes"
	"encoding/json"
	"slices"
	"sync"

	spec "github.com/egoist/asuka/internal/icloudspec"
	"github.com/egoist/asuka/signal"
)

// The key-value store: small values (settings, the page last read, a
// streak) that follow the user to each of their devices, kept on the
// device and synced when it can. At most 1024 keys and 1 MB in all; a key
// is at most 64 bytes. Values are JSON (anything encoding/json encodes),
// kept as property lists, so the app's Swift, a widget or a watch app
// reads them as their own (a []byte is a string in base64, as in JSON; a
// null in an object is left out, which decodes the same).
//
// When two devices set a key, the last one wins. A device that was
// offline gets the changes once online, as Changes (OnChange).

func checkKey(key string) error {
	if key == "" || len(key) > 64 {
		return ErrKeyTooLong
	}
	return nil
}

// Get decodes key's value into v (a pointer), reporting whether it has
// one. Not on the main thread.
func Get(key string, v any) (bool, error) {
	if err := checkKey(key); err != nil {
		return false, err
	}
	b, err := call("kvGet", []byte(key))
	if err != nil || len(b) == 0 {
		return false, err
	}
	return true, json.Unmarshal(b, v)
}

// setArgs is kvSet's arguments: key and v's JSON.
func setArgs(key string, v any) ([]byte, error) {
	if err := checkKey(key); err != nil {
		return nil, err
	}
	b, ok := v.(json.RawMessage)
	if !ok {
		var err error
		if b, err = json.Marshal(v); err != nil {
			return nil, err
		}
	}
	if len(b) > 1<<20 {
		return nil, ErrTooLarge
	}
	return json.Marshal(spec.KVSet{Key: key, Value: json.RawMessage(b)})
}

// Set keeps v (anything encoding/json encodes) as key's value, on this
// device at once and on the user's others as iCloud syncs; a nil v
// deletes the key. Not on the main thread.
func Set(key string, v any) error {
	args, err := setArgs(key, v)
	if err != nil {
		return err
	}
	_, err = call("kvSet", args)
	return err
}

// Delete deletes key, on every device. Not on the main thread.
func Delete(key string) error {
	if err := checkKey(key); err != nil {
		return err
	}
	_, err := call("kvDelete", []byte(key))
	return err
}

// All is every key's value, as JSON. Not on the main thread.
func All() (map[string]json.RawMessage, error) {
	b, err := call("kvAll", nil)
	if err != nil {
		return nil, err
	}
	m := map[string]json.RawMessage{}
	return m, json.Unmarshal(b, &m)
}

// Sync writes the changes made on the device to its disk, and has iCloud
// look for the others' sooner. The system syncs by itself: call it when
// the user asks to (pulling to refresh). Not on the main thread.
func Sync() error {
	_, err := call("kvSync", nil)
	return err
}

// Reason is why values changed.
type Reason uint8

const (
	// ServerChange: another device changed them.
	ServerChange Reason = iota
	// InitialSync: the first download, as the app's store was empty (a
	// new device, the app installed again).
	InitialSync
	// QuotaViolation: the app's values are over iCloud's limit (1 MB,
	// 1024 keys): make room, deleting some.
	QuotaViolation
	// AccountChange: another account signed in; its values replace the
	// last one's.
	AccountChange
)

func (r Reason) String() string {
	return [...]string{"server change", "initial sync", "quota violation", "account change"}[min(int(r), 3)]
}

func reasonOf(s string) Reason {
	switch s {
	case spec.InitialSync:
		return InitialSync
	case spec.QuotaViolation:
		return QuotaViolation
	case spec.AccountChange:
		return AccountChange
	}
	return ServerChange
}

// Change is values changed from outside the app: by iCloud.
type Change struct {
	Reason Reason
	// Keys are the keys whose values changed.
	Keys []string
}

// OnChange calls f with each change iCloud makes to the values (not the
// app's own Sets), until stop. Main thread.
func OnChange(f func(Change)) (stop func()) {
	fp := &f
	mu.Lock()
	changeWatches[fp] = true
	mu.Unlock()
	listen()
	var once sync.Once
	return func() {
		once.Do(func() {
			mu.Lock()
			delete(changeWatches, fp)
			mu.Unlock()
			unlisten()
		})
	}
}

// Signal returns a signal whose value is key's in iCloud, for a
// component: it starts with def, then the value kept (once read, if the
// app hasn't set it meanwhile); each value it's set to is kept, and it
// follows the changes other devices make (def again if they delete the
// key), while the component lives. Main thread.
//
// Where there's no iCloud (or no account), it's a signal of the app's
// alone, kept in memory: keep a local copy (asuka/sdk/store) for that.
func Signal[T any](key string, def T) *signal.Signal[T] {
	s := signal.NewFunc(def, nil)
	// kept is the JSON iCloud has (as last read or set): a value the
	// signal takes from iCloud isn't written back. set: the app set it,
	// so the first read doesn't overwrite it.
	var kept []byte
	alive, set, first := true, false, true
	take := func(b []byte) { // iCloud's JSON, none if it has no value
		v := def
		if len(b) == 0 {
			b, _ = json.Marshal(def)
		} else {
			var x T
			if json.Unmarshal(b, &x) != nil {
				return
			}
			v = x
		}
		if !bytes.Equal(b, kept) {
			kept = b
			s.Set(v)
		}
	}
	read := func(initial bool) {
		post("kvGet", []byte(key), func(b []byte, err error) {
			if !alive || err != nil || initial && (set || len(b) == 0) {
				return
			}
			take(b)
		})
	}
	if checkKey(key) == nil {
		read(true)
	}
	signal.Effect(func() {
		v := s.Get()
		if first {
			first = false
			return
		}
		b, err := json.Marshal(v)
		if err != nil || bytes.Equal(b, kept) {
			return
		}
		set, kept = true, b
		if args, err := setArgs(key, json.RawMessage(b)); err == nil {
			post("kvSet", args, nil)
		}
	})
	stop := OnChange(func(c Change) {
		if c.Reason == AccountChange || slices.Contains(c.Keys, key) {
			read(false)
		}
	})
	signal.OnCleanup(func() {
		alive = false
		stop()
	})
	return s
}
