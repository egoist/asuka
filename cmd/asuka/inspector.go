package main

import (
	_ "embed"
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/egoist/asuka/internal/devserver"
)

// asuka dev's inspector: a page on this computer that shows the running
// app's views (each one's element, text, frame and children), as React
// Native's and Flutter's DevTools do. The page asks for the tree; the
// inspector asks every player (AK_DEV_INSPECT, the web's page by its
// events) and answers with what they said (AK_DEV_TREE: the core's
// ak_ui_inspect). Its Performance tab turns a player's performance
// overlay on and off, and records a profile (devperf.go): the page
// draws it, and exports it as a trace or a speedscope profile.

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
	// profiles are the players' recordings.
	profiles *devProfiles
	// previews lists the app's previews, and what each player shows;
	// showPreview shows one (file "": the app) on the player of key (""
	// every player). Set by the loop (devpreview.go).
	previews    func() previewsAnswer
	showPreview func(file, name string, line int, key string) error
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
	// perf drives its performance tools (nil: none).
	perf *perfCtl
}

func newInspector() *inspector {
	return &inspector{trees: map[string]*inspected{}, answered: make(chan struct{}, 64), profiles: newDevProfiles()}
}

// put keeps a player's tree (key: the player), which answers an ask;
// highlight shows a view of it on the player, perf drives its
// performance tools.
func (in *inspector) put(key, name string, tree []byte, web bool, highlight func(path string), perf *perfCtl) {
	if !json.Valid(tree) {
		return
	}
	in.mu.Lock()
	t := in.trees[key]
	if t == nil {
		t = &inspected{order: len(in.trees)}
		in.trees[key] = t
	}
	t.name, t.tree, t.at, t.web, t.highlight, t.perf = name, append(json.RawMessage(nil), tree...), time.Now(), web, highlight, perf
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
	Tree json.RawMessage `json:"tree,omitempty"`
	// The performance tools': the overlay shows, a recording runs, it
	// samples the app's functions, and a recording of it is kept.
	Overlay   bool `json:"overlay"`
	Recording bool `json:"recording"`
	CPU       bool `json:"cpu"`
	Profile   bool `json:"profile"`
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
	return in.players(start, true)
}

// players are the players that answered, in the order they first did,
// with their trees or not; a page that didn't answer an ask that started
// at since is forgotten (zero: none is).
func (in *inspector) players(since time.Time, trees bool) []inspectorPlayer {
	in.mu.Lock()
	defer in.mu.Unlock()
	out := []inspectorPlayer{}
	order := map[string]int{}
	for k, t := range in.trees {
		if t.web && !since.IsZero() && t.at.Before(since) {
			delete(in.trees, k)
			continue
		}
		ip := inspectorPlayer{Key: k, Name: t.name, Profile: in.profiles.last(k) != nil}
		if trees {
			ip.Tree = t.tree
		}
		if t.perf != nil {
			ip.Overlay, ip.Recording = t.perf.state()
			ip.CPU = t.perf.cpu
		}
		out = append(out, ip)
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
	case "/players":
		// The players as they last answered, without asking them again (a
		// recording's: the tree's walk isn't the app's work).
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(struct {
			Players []inspectorPlayer `json:"players"`
		}{in.players(time.Time{}, false)})
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
	case "/previews":
		// The app's previews (its Previews tab, an editor's language
		// server), and what each player shows.
		if in.previews == nil {
			http.NotFound(w, r)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(in.previews())
	case "/preview":
		// Show a preview (file, its name or a line in it) on a player
		// (key; "": every one), or the app (no file).
		if r.Method != http.MethodPost {
			http.Error(w, "POST", http.StatusMethodNotAllowed)
			return
		}
		if in.showPreview == nil {
			http.NotFound(w, r)
			return
		}
		q := r.URL.Query()
		line, _ := strconv.Atoi(q.Get("line"))
		if err := in.showPreview(q.Get("file"), q.Get("name"), line, q.Get("key")); err != nil {
			http.Error(w, err.Error(), http.StatusNotFound)
			return
		}
		w.WriteHeader(http.StatusNoContent)
	case "/perf/overlay", "/perf/record", "/perf/stop":
		if r.Method != http.MethodPost {
			http.Error(w, "POST", http.StatusMethodNotAllowed)
			return
		}
		in.perf(w, r)
	case "/perf/profile", "/perf/export":
		in.profile(w, r)
	default:
		http.NotFound(w, r)
	}
}

func (in *inspector) ctl(key string) *perfCtl {
	in.mu.Lock()
	defer in.mu.Unlock()
	if t := in.trees[key]; t != nil {
		return t.perf
	}
	return nil
}

// perf turns a player's overlay on or off (on=1), starts a recording
// (cpu=1: sampling the app's functions too), or stops it, answering with
// the recording once the player sends it ({"name", "profile"}).
func (in *inspector) perf(w http.ResponseWriter, r *http.Request) {
	q := r.URL.Query()
	key := q.Get("key")
	c := in.ctl(key)
	if c == nil {
		http.NotFound(w, r)
		return
	}
	switch r.URL.Path {
	case "/perf/overlay":
		c.overlay(q.Get("on") == "1")
	case "/perf/record":
		c.start(q.Get("cpu") == "1" && c.cpu)
	case "/perf/stop":
		since := time.Now()
		c.stop()
		g := in.profiles.wait(key, since, profileWait)
		if g == nil {
			http.Error(w, "the app didn't send its recording", http.StatusGatewayTimeout)
			return
		}
		writeProfile(w, g)
	}
}

func writeProfile(w http.ResponseWriter, g *gotProfile) {
	w.Header().Set("Content-Type", "application/json")
	name, _ := json.Marshal(g.name)
	fmt.Fprintf(w, `{"name":%s,"profile":%s}`, name, g.data)
}

// profile answers with a player's last recording: as the page reads it
// (/perf/profile), or exported (/perf/export, format trace: Chrome's
// trace event format, for Perfetto and chrome://tracing; speedscope:
// speedscope's), to download.
func (in *inspector) profile(w http.ResponseWriter, r *http.Request) {
	q := r.URL.Query()
	g := in.profiles.last(q.Get("key"))
	if g == nil {
		http.NotFound(w, r)
		return
	}
	if r.URL.Path == "/perf/profile" {
		writeProfile(w, g)
		return
	}
	p, err := devserver.DecodeProfile(g.data)
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	var b []byte
	name := strings.TrimSuffix(profileFileName(g.name, g.at), ".json")
	switch q.Get("format") {
	case "speedscope":
		b, err = p.Speedscope(g.name)
		name += ".speedscope.json"
	default:
		b, err = p.ChromeTrace(g.name)
		name += ".json"
	}
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Content-Disposition", `attachment; filename="`+name+`"`)
	w.Write(b)
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
