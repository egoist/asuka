package fake

import (
	"encoding/json"
	"errors"
	"io/fs"
	"maps"
	"os"
	"path/filepath"
	"slices"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	spec "github.com/egoist/asuka/internal/icloudspec"
)

// ICloud is iCloud, as the hosts' "icloud" module reaches it (package
// icloud), for tests: an account, its key-value store, and the app's
// containers as directories under Root, whose files the app reads and
// writes. What another device does, a test does: Change sets a value,
// Arrive brings a file not yet downloaded, Conflict saves a version in
// conflict, SignIn and SignOut change the account. Each comes to the app
// as on a device: an event its watchers get. Host.ICloud installs it;
// without one, there's no iCloud (icloud.ErrNotAvailable).
//
//	ic := h.ICloud()
//	ic.Root = t.TempDir()
//	ic.Change("theme", "dark")                 // another device's
//	ic.Arrive("Documents/Note.txt", []byte("Hi")) // listed, not downloaded
type ICloud struct {
	// Root is where the containers' directories are, one per container
	// (default: a temporary directory, made on first use).
	Root string
	// Containers are the app's container IDs, the first its default
	// (default: iCloud.dev.asuka.test).
	Containers []string
	// Unavailable: no iCloud (icloud.ErrNotAvailable).
	Unavailable bool

	mu       sync.Mutex
	account  spec.Account
	values   map[string]map[string]json.RawMessage // by account
	remote   map[string][]byte                     // files in iCloud, not on the device, by path
	versions map[string][]spec.Version             // conflicts, by path
	watched  map[string]bool                       // scopes
	waiting  func([]byte, error)
	queue    []spec.Event
	follow   bool
	nextID   int
}

// ICloud gives the host iCloud (the "icloud" module), signed in to an
// account "test".
func (h *Host) ICloud() *ICloud {
	c := &ICloud{account: spec.Account{Available: true, ID: "test"},
		values: map[string]map[string]json.RawMessage{}, remote: map[string][]byte{},
		versions: map[string][]spec.Version{}, watched: map[string]bool{}}
	if h.AsyncModules == nil {
		h.AsyncModules = map[string]func(string, []byte, func([]byte, error)){}
	}
	h.AsyncModules["icloud"] = c.call
	return c
}

// root is Root, made. c.mu held.
func (c *ICloud) root() string {
	if c.Root == "" {
		c.Root, _ = os.MkdirTemp("", "asuka-icloud")
	}
	return c.Root
}

func (c *ICloud) containers() []string {
	if len(c.Containers) == 0 {
		return []string{"iCloud.dev.asuka.test"}
	}
	return c.Containers
}

// Path is the path of rel (slashes) in the default container: a test's
// way to name the app's files.
func (c *ICloud) Path(rel string) string {
	c.mu.Lock()
	defer c.mu.Unlock()
	return filepath.Join(c.root(), c.containers()[0], filepath.FromSlash(rel))
}

// inside reports whether p is in a container. c.mu held.
func (c *ICloud) inside(p string) bool {
	for _, id := range c.containers() {
		d := filepath.Join(c.root(), id)
		if p == d || strings.HasPrefix(p, d+string(filepath.Separator)) {
			return true
		}
	}
	return false
}

func (c *ICloud) kv() map[string]json.RawMessage {
	m := c.values[c.account.ID]
	if m == nil {
		m = map[string]json.RawMessage{}
		c.values[c.account.ID] = m
	}
	return m
}

// Value is key's value as the app's store has it, in JSON.
func (c *ICloud) Value(key string) (json.RawMessage, bool) {
	c.mu.Lock()
	defer c.mu.Unlock()
	v, ok := c.kv()[key]
	return v, ok
}

// Change sets key's value as another device does (nil deletes it): the
// app's OnChange gets a server change.
func (c *ICloud) Change(key string, v any) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if v == nil {
		delete(c.kv(), key)
	} else {
		b, _ := json.Marshal(v)
		c.kv()[key] = b
	}
	c.send(spec.Event{Type: "kv", Reason: spec.ServerChange, Keys: []string{key}})
}

// ExceedQuota says the app's values are over iCloud's limit.
func (c *ICloud) ExceedQuota() {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.send(spec.Event{Type: "kv", Reason: spec.QuotaViolation})
}

// SignIn signs another account in (its values replace the last one's:
// an account change), SignOut signs out.
func (c *ICloud) SignIn(id string) {
	c.mu.Lock()
	defer c.mu.Unlock()
	keys := slices.Collect(maps.Keys(c.kv()))
	c.account = spec.Account{Available: true, ID: id}
	for k := range c.kv() {
		if !slices.Contains(keys, k) {
			keys = append(keys, k)
		}
	}
	sort.Strings(keys)
	a := c.account
	c.send(spec.Event{Type: "account", Account: &a})
	c.send(spec.Event{Type: "kv", Reason: spec.AccountChange, Keys: keys})
	c.changed() // the lists are the new account's, as on a device
}

// SignOut signs the account out.
func (c *ICloud) SignOut() {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.account = spec.Account{}
	a := c.account
	c.send(spec.Event{Type: "account", Account: &a})
}

// Arrive brings a file another device made, at rel in the default
// container: listed, not downloaded until the app downloads or reads it.
func (c *ICloud) Arrive(rel string, data []byte) {
	p := c.Path(rel)
	c.mu.Lock()
	defer c.mu.Unlock()
	os.Remove(p)
	c.remote[p] = data
	c.changed()
}

// Conflict saves a version of the file at rel (in the default container)
// in conflict with it, as device did.
func (c *ICloud) Conflict(rel string, data []byte, device string) {
	p := c.Path(rel)
	c.mu.Lock()
	defer c.mu.Unlock()
	c.nextID++
	id := "v" + strconv.Itoa(c.nextID)
	vp := filepath.Join(c.root(), ".versions", id, filepath.Base(p))
	os.MkdirAll(filepath.Dir(vp), 0o755)
	os.WriteFile(vp, data, 0o644)
	c.versions[p] = append(c.versions[p], spec.Version{ID: id, Path: vp, Name: filepath.Base(p), Modified: time.Now().UnixMilli(), Device: device})
	c.changed()
}

// Downloaded reports whether the file at rel is on the device.
func (c *ICloud) Downloaded(rel string) bool {
	p := c.Path(rel)
	c.mu.Lock()
	defer c.mu.Unlock()
	_, remote := c.remote[p]
	_, err := os.Stat(p)
	return !remote && err == nil
}

// Watching reports whether the app watches a scope's files ("documents",
// "data").
func (c *ICloud) Watching(scope string) bool {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.watched[scope]
}

// send gives the app an event, if it listens. c.mu held.
func (c *ICloud) send(e spec.Event) {
	if !c.follow {
		return
	}
	if w := c.waiting; w != nil {
		c.waiting = nil
		jsonReply(w, e)
		return
	}
	c.queue = append(c.queue, e)
}

// changed sends the files of each scope watched. c.mu held.
func (c *ICloud) changed() {
	for _, s := range []string{spec.Documents, spec.Data} {
		if c.watched[s] {
			c.send(spec.Event{Type: "files", Scope: s, Files: c.files(s)})
		}
	}
}

// files lists a scope's files. c.mu held.
func (c *ICloud) files(scope string) []spec.File {
	out := []spec.File{}
	in := func(p string) bool {
		for _, id := range c.containers() {
			rel, err := filepath.Rel(filepath.Join(c.root(), id), p)
			if err != nil || strings.HasPrefix(rel, "..") {
				continue
			}
			docs := rel == "Documents" || strings.HasPrefix(rel, "Documents"+string(filepath.Separator))
			return docs == (scope == spec.Documents) && rel != "Documents" && rel != "."
		}
		return false
	}
	for _, id := range c.containers() {
		filepath.WalkDir(filepath.Join(c.root(), id), func(p string, d fs.DirEntry, err error) error {
			if err != nil || !in(p) {
				return nil
			}
			info, err := d.Info()
			if err != nil {
				return nil
			}
			f := spec.File{Path: p, Name: d.Name(), Dir: d.IsDir(), Modified: info.ModTime().UnixMilli(), Status: spec.Current, Uploaded: true,
				Conflicts: len(c.versions[p]) > 0}
			if !d.IsDir() {
				f.Size = info.Size()
			}
			out = append(out, f)
			return nil
		})
	}
	for p, data := range c.remote {
		if in(p) {
			out = append(out, spec.File{Path: p, Name: filepath.Base(p), Size: int64(len(data)), Status: spec.NotDownloaded, Uploaded: true})
		}
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Path < out[j].Path })
	return out
}

// fetch puts a remote file on the device. c.mu held.
func (c *ICloud) fetch(p string) {
	if data, ok := c.remote[p]; ok {
		os.MkdirAll(filepath.Dir(p), 0o755)
		os.WriteFile(p, data, 0o644)
		delete(c.remote, p)
	}
}

func (c *ICloud) exists(p string) bool {
	if _, ok := c.remote[p]; ok {
		return true
	}
	_, err := os.Stat(p)
	return err == nil
}

func (c *ICloud) call(method string, args []byte, reply func([]byte, error)) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.Unavailable {
		spec.Unavailable("no iCloud in this test")[method](args, reply)
		return
	}
	fail := func(prefix, what string) { reply(nil, errors.New(prefix+": "+what)) }
	signedIn := func() bool {
		if !c.account.Available {
			fail(spec.ErrNoAccount, "not signed in to iCloud")
		}
		return c.account.Available
	}
	path := func(p string) (string, bool) {
		if !c.inside(p) {
			fail(spec.ErrNotContainer, p)
			return "", false
		}
		return p, true
	}
	switch method {
	case "account":
		jsonReply(reply, c.account)
	case "kvGet":
		reply(c.kv()[string(args)], nil)
	case "kvSet":
		var in struct {
			Key   string          `json:"key"`
			Value json.RawMessage `json:"value"`
		}
		json.Unmarshal(args, &in)
		if string(in.Value) == "null" {
			delete(c.kv(), in.Key)
		} else {
			c.kv()[in.Key] = in.Value
		}
		reply(nil, nil)
	case "kvDelete":
		delete(c.kv(), string(args))
		reply(nil, nil)
	case "kvAll":
		jsonReply(reply, c.kv())
	case "kvSync":
		reply(nil, nil)
	case "container":
		if !signedIn() {
			return
		}
		id := string(args)
		if id == "" {
			id = c.containers()[0]
		}
		if !slices.Contains(c.containers(), id) {
			fail(spec.ErrNotContainer, id)
			return
		}
		d := filepath.Join(c.root(), id)
		os.MkdirAll(d, 0o755)
		reply([]byte(d), nil)
	case "files":
		if !signedIn() {
			return
		}
		var q spec.Query
		json.Unmarshal(args, &q)
		jsonReply(reply, c.files(q.Scope))
	case "watch":
		var q spec.Query
		json.Unmarshal(args, &q)
		c.watched[q.Scope] = true
		c.follow = true // its lists wait for the next "next"
		reply(nil, nil)
		if c.account.Available {
			c.send(spec.Event{Type: "files", Scope: q.Scope, Files: c.files(q.Scope)})
		}
	case "unwatch":
		var q spec.Query
		json.Unmarshal(args, &q)
		delete(c.watched, q.Scope)
		reply(nil, nil)
	case "download", "evict", "read", "delete", "versions", "link":
		if !signedIn() {
			return
		}
		p, ok := path(string(args))
		if !ok {
			return
		}
		if !c.exists(p) {
			fail(spec.ErrNotExist, p)
			return
		}
		switch method {
		case "download":
			c.fetch(p)
			reply(nil, nil)
		case "evict":
			if data, err := os.ReadFile(p); err == nil {
				os.Remove(p)
				c.remote[p] = data
			}
			reply(nil, nil)
		case "read":
			c.fetch(p)
			b, err := os.ReadFile(p)
			reply(b, err)
		case "delete":
			delete(c.remote, p)
			delete(c.versions, p)
			reply(nil, os.RemoveAll(p))
		case "versions":
			v := c.versions[p]
			if v == nil {
				v = []spec.Version{}
			}
			jsonReply(reply, v)
			return
		case "link":
			jsonReply(reply, spec.Link{URL: "https://www.icloud.com/iclouddrive/0test#" + filepath.Base(p)})
			return
		}
		c.changed()
	case "write":
		if !signedIn() {
			return
		}
		p, data, _ := spec.ReadWriteArgs(args)
		if p, ok := path(p); ok {
			delete(c.remote, p)
			os.MkdirAll(filepath.Dir(p), 0o755)
			reply(nil, os.WriteFile(p, data, 0o644))
			c.changed()
		}
	case "move", "moveIn", "moveOut":
		if !signedIn() {
			return
		}
		var m spec.Move
		json.Unmarshal(args, &m)
		from, to := m.From, m.To
		if method != "moveIn" {
			if _, ok := path(from); !ok {
				return
			}
			c.fetch(from)
		}
		if method != "moveOut" {
			if _, ok := path(to); !ok {
				return
			}
		}
		if _, err := os.Stat(from); err != nil {
			fail(spec.ErrNotExist, from)
			return
		}
		os.MkdirAll(filepath.Dir(to), 0o755)
		os.RemoveAll(to)
		reply(nil, os.Rename(from, to))
		c.changed()
	case "resolve":
		var r spec.Resolve
		json.Unmarshal(args, &r)
		p, ok := path(r.Path)
		if !ok {
			return
		}
		if r.Keep != "" {
			i := slices.IndexFunc(c.versions[p], func(v spec.Version) bool { return v.ID == r.Keep })
			if i < 0 {
				reply(nil, errors.New("no version "+r.Keep))
				return
			}
			b, _ := os.ReadFile(c.versions[p][i].Path)
			os.WriteFile(p, b, 0o644)
		}
		for _, v := range c.versions[p] {
			os.RemoveAll(filepath.Dir(v.Path))
		}
		delete(c.versions, p)
		reply(nil, nil)
		c.changed()
	case "next":
		c.follow = true
		if len(c.queue) > 0 {
			e := c.queue[0]
			c.queue = c.queue[1:]
			jsonReply(reply, e)
			return
		}
		c.waiting = reply
	case "stop":
		if w := c.waiting; w != nil {
			c.waiting = nil
			w(nil, errors.New("stopped"))
		}
		c.follow = false
		c.queue = nil
		reply(nil, nil)
	default:
		reply(nil, errors.New("no module method icloud."+method))
	}
}
