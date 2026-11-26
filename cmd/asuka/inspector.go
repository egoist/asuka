package main

import (
	_ "embed"
	"encoding/json"
	"net"
	"net/http"
	"sort"
	"sync"
	"time"
)

// asuka dev's inspector: a page on this computer that shows the running
// app's views (each one's element, text, frame and children), as React
// Native's and Flutter's DevTools do. The page asks for the tree; the
// inspector asks every player (AK_DEV_INSPECT, the web's page by its
// events) and answers with what they said (AK_DEV_TREE: the core's
// ak_ui_inspect).

//go:embed inspector.html
var inspectorPage []byte

// inspectWait is how long a tree's request waits for the players to
// answer: a player that's busy (a breakpoint, a long flush) shows the
// tree it last gave.
const inspectWait = 2 * time.Second

type inspector struct {
	mu    sync.Mutex
	trees map[string]*inspected
	// askers each ask their players for their views, and say how many
	// they asked.
	askers []func() int
	// answered is signalled at each tree that comes.
	answered chan struct{}
	ln       net.Listener
	srv      *http.Server
}

// inspected is a player's views, as its core said them.
type inspected struct {
	name string
	tree json.RawMessage
	at   time.Time
	// web: a page, which can go without a word (a closed tab): it's
	// forgotten once it doesn't answer.
	web bool
	// order keeps the players in the order they first answered.
	order int
	// highlight has the player highlight the view at a path ("": none).
	highlight func(path string)
}

func newInspector() *inspector {
	return &inspector{trees: map[string]*inspected{}, answered: make(chan struct{}, 64)}
}

// put keeps a player's tree (key: the player), which answers an ask;
// highlight shows a view of it on the player.
func (in *inspector) put(key, name string, tree []byte, web bool, highlight func(path string)) {
	if !json.Valid(tree) {
		return
	}
	in.mu.Lock()
	t := in.trees[key]
	if t == nil {
		t = &inspected{order: len(in.trees)}
		in.trees[key] = t
	}
	t.name, t.tree, t.at, t.web, t.highlight = name, append(json.RawMessage(nil), tree...), time.Now(), web, highlight
	in.mu.Unlock()
	select {
	case in.answered <- struct{}{}:
	default:
	}
}

// forget drops a player that went.
func (in *inspector) forget(key string) {
	in.mu.Lock()
	delete(in.trees, key)
	in.mu.Unlock()
}

type inspectorPlayer struct {
	Key  string          `json:"key"`
	Name string          `json:"name"`
	Tree json.RawMessage `json:"tree"`
}

// ask asks every player for its views and returns them once each has
// answered, or at wait: the players that didn't answer by then show what
// they last said (a page that didn't is forgotten).
func (in *inspector) ask(wait time.Duration) []inspectorPlayer {
	start := time.Now()
	in.mu.Lock()
	askers := append([]func() int(nil), in.askers...)
	in.mu.Unlock()
	// What a previous ask's late answers signalled isn't this one's.
	for len(in.answered) > 0 {
		<-in.answered
	}
	n := 0
	for _, a := range askers {
		n += a()
	}
	timer := time.NewTimer(wait)
	defer timer.Stop()
	for n > 0 && in.answeredSince(start) < n {
		select {
		case <-in.answered:
		case <-timer.C:
			n = 0
		}
	}
	in.mu.Lock()
	defer in.mu.Unlock()
	out := []inspectorPlayer{}
	order := map[string]int{}
	for k, t := range in.trees {
		if t.web && t.at.Before(start) {
			delete(in.trees, k)
			continue
		}
		out = append(out, inspectorPlayer{k, t.name, t.tree})
		order[k] = t.order
	}
	sort.Slice(out, func(i, j int) bool { return order[out[i].Key] < order[out[j].Key] })
	return out
}

func (in *inspector) answeredSince(t time.Time) int {
	in.mu.Lock()
	defer in.mu.Unlock()
	n := 0
	for _, x := range in.trees {
		if !x.at.Before(t) {
			n++
		}
	}
	return n
}

func (in *inspector) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Cache-Control", "no-store")
	switch r.URL.Path {
	case "/":
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		w.Write(inspectorPage)
	case "/tree":
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(struct {
			Players []inspectorPlayer `json:"players"`
		}{in.ask(inspectWait)})
	case "/highlight":
		// The page selected a view (path "": none) of a player (key).
		if r.Method != http.MethodPost {
			http.Error(w, "POST", http.StatusMethodNotAllowed)
			return
		}
		q := r.URL.Query()
		in.mu.Lock()
		t := in.trees[q.Get("key")]
		var h func(string)
		if t != nil {
			h = t.highlight
		}
		in.mu.Unlock()
		if h == nil {
			http.NotFound(w, r)
			return
		}
		h(q.Get("path"))
	default:
		http.NotFound(w, r)
	}
}

// listen serves the inspector's page on this computer alone (it shows
// what the app shows) and returns its address.
func (in *inspector) listen() (string, error) {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		return "", err
	}
	in.ln = ln
	in.srv = &http.Server{Handler: in}
	go in.srv.Serve(ln)
	return "http://" + ln.Addr().String() + "/", nil
}

func (in *inspector) close() {
	if in.srv != nil {
		in.srv.Close()
	}
}
