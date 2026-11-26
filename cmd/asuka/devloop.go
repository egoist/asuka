package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"strings"
	"sync"
	"time"

	"github.com/egoist/asuka/internal/devserver"
	"github.com/egoist/asuka/lang/target"
)

// devLoop is asuka dev's side of an app's players (a host built for
// development, running the C core without an app: the simulator's, an
// emulator's, this computer's, a page): it compiles the app, serves its
// module to the players that connect (internal/devserver), compiles it
// again on each save and sends the new one, which each player loads
// keeping its state and views. It prints the players' logs, and how each
// reload went.
//
// A module is a platform's (its packages' files for it, @platform's
// branches): the loop's player's (app.platform), and each other
// platform's a player connects from (one asuka dev serves an iPhone and
// an emulator at once), compiled when the first of them says hello and
// again on each save while one is connected.
type devLoop struct {
	app *asukaApp
	srv *devserver.Server
	out io.Writer
	// platform is the loop's player's.
	platform target.Platform
	// compiling is held while the loop compiles (a save's, a player's
	// hello's).
	compiling sync.Mutex

	mu       sync.Mutex
	module   []byte // the last module that compiled, for the loop's player's platform
	loaded   map[*devserver.Player]bool
	modules  []func(module []byte)       // more players (the web's page)
	restarts []func()                    // told to restart (the web's page)
	failed   []func(f devserver.Failure) // told what didn't compile
	fixed    []func()                    // told when it compiles again
	broken   bool
	// The C the players were built with (cKey), once launched: a module
	// that calls other C (its extern declarations changed) waits for
	// players built with it (stale).
	launched bool
	playerC  string
	stale    bool
	// reloads receives each reload a player reports (tests).
	reloads chan devReload
	// inspector serves the app's views to the inspector's page, at
	// inspectorURL.
	inspector    *inspector
	inspectorURL string
	// automation runs the core's automation on the player; flows, when
	// set, are run once the app has loaded (asuka test --gui), then the
	// loop ends.
	automation *automation
	flows      []flow
	// firstLoad is closed when a player first reports the app loaded;
	// playerErr gets what a player reports going wrong (--once's).
	firstLoad     chan struct{}
	firstLoadOnce sync.Once
	playerErr     chan string
}

// devReload is a player's report of a module it loaded.
type devReload struct {
	first, restarted bool
	kept             uint32
	micros           uint32
}

// newDevLoop listens for players on addr (":0": a free port) and
// compiles the app once. A module that doesn't compile is printed and
// waited on: the players get the first that does.
func newDevLoop(app *asukaApp, addr string) (*devLoop, error) {
	d := &devLoop{app: app, out: os.Stdout, platform: app.platform.Or(), loaded: map[*devserver.Player]bool{}, reloads: make(chan devReload, 16),
		firstLoad: make(chan struct{}), playerErr: make(chan string, 1), inspector: newInspector(), automation: newAutomation()}
	srv, err := devserver.Listen(addr, devserver.Events{
		Hello: func(p *devserver.Player, b []byte) {
			fmt.Fprintf(d.out, "asuka: %s connected\n", playerName(b))
		},
		Automated: func(p *devserver.Player, seq uint32, answers string) { d.automation.answered(seq, answers) },
		Tree: func(p *devserver.Player, tree []byte) {
			d.inspector.put(fmt.Sprintf("%p", p), playerName(p.Info), tree, false, func(path string) { p.Highlight(path) })
		},
		Need: func(p *devserver.Player, platform target.Platform) { d.compileFor(platform) },
		Log: func(p *devserver.Player, stderr bool, text []byte) {
			w := os.Stdout
			if stderr {
				w = os.Stderr
			}
			w.Write(text)
		},
		Reloaded: func(p *devserver.Player, kept, micros uint32, restarted bool) {
			d.report(p, false, restarted, kept, micros)
		},
		Error: func(p *devserver.Player, text []byte) {
			msg := strings.TrimSpace(string(text))
			fmt.Fprintf(os.Stderr, "asuka: the player: %s\n", msg)
			d.notePlayerErr(msg)
		},
		Uncaught: func(p *devserver.Player, u devserver.Uncaught) {
			source, details := d.uncaught(u)
			p.Source(u.Seq, source, details)
		},
		Gone: func(p *devserver.Player, err error) {
			d.mu.Lock()
			delete(d.loaded, p)
			d.mu.Unlock()
			d.inspector.forget(fmt.Sprintf("%p", p))
		},
	})
	if err != nil {
		return nil, err
	}
	d.srv = srv
	d.inspector.askers = append(d.inspector.askers, srv.Inspect)
	d.automation.send = d.nativeAutomation(srv)
	if url, err := d.inspector.listen(); err == nil {
		d.inspectorURL = url
		fmt.Fprintf(d.out, "asuka: the inspector (the app's views): %s\n", url)
	}
	d.compile()
	return d, nil
}

// uncaught prints an error a player's app didn't catch, and returns what
// its overlay shows of it: the lines where it was thrown, and its stack.
func (d *devLoop) uncaught(u devserver.Uncaught) (source, details string) {
	source, details = uncaughtSource(d.app.dir, u.Trace)
	what := "error"
	if u.DidntStart {
		what = "the app didn't start"
		d.notePlayerErr(u.Message)
	}
	var b strings.Builder
	fmt.Fprintf(&b, "asuka: %s: %s\n", what, u.Message)
	for _, l := range strings.Split(details, "\n") {
		if l != "" {
			b.WriteString("    " + l + "\n")
		}
	}
	os.Stderr.WriteString(b.String())
	return source, details
}

func (d *devLoop) notePlayerErr(msg string) {
	select {
	case d.playerErr <- msg:
	default:
	}
}

// report prints a player's report: the first load, a reload with the
// state it kept, or a restart. A player of the dev server's says which
// by its connection; another (a page) says first itself.
func (d *devLoop) report(p *devserver.Player, first, restarted bool, kept, micros uint32) {
	d.mu.Lock()
	if p != nil {
		first = !d.loaded[p]
		d.loaded[p] = true
	}
	d.mu.Unlock()
	ms := float64(micros) / 1000
	switch {
	case first:
		fmt.Fprintf(d.out, "asuka: running (loaded in %s)\n", msText(ms))
	case restarted:
		fmt.Fprintf(d.out, "asuka: restarted in %s, every state from its initial value\n", msText(ms))
	default:
		fmt.Fprintf(d.out, "asuka: reloaded in %s, %d %s kept\n", msText(ms), kept, plural(int(kept), "state", "states"))
	}
	if first {
		d.firstLoadOnce.Do(func() { close(d.firstLoad) })
	}
	select {
	case d.reloads <- devReload{first: first, restarted: restarted, kept: kept, micros: micros}:
	default:
	}
}

func msText(ms float64) string {
	if ms < 10 {
		return fmt.Sprintf("%.1f ms", ms)
	}
	return fmt.Sprintf("%.0f ms", ms)
}

// port is where players connect.
func (d *devLoop) port() int { return d.srv.Port() }

// close disconnects every player.
func (d *devLoop) close() {
	d.srv.Close()
	d.inspector.close()
}

// playerName is who a player is, from its hello: "iOS (iPhone 17)".
func playerName(hello []byte) string {
	var h struct{ Platform, Device string }
	json.Unmarshal(hello, &h)
	who := h.Platform
	if n, ok := map[string]string{"ios": "iOS", "android": "Android", "darwin": "macOS", "macos": "macOS", "windows": "Windows", "linux": "Linux", "web": "Web"}[who]; ok {
		who = n
	}
	if h.Device != "" && h.Device != "desktop" {
		who += " (" + h.Device + ")"
	}
	return who
}

// compile compiles the app and sends the module to the players,
// reporting whether it compiled for the loop's player. What didn't is
// printed, and the players keep running what they have. Players of other
// platforms get their platform's (compileFor).
func (d *devLoop) compile() bool {
	d.compiling.Lock()
	defer d.compiling.Unlock()
	start := time.Now()
	if d.app.proj != nil {
		d.app.proj.Forget() // a save: compiled, whatever the files' times say
	}
	m, diags, err := d.app.compileShown(d.platform, nil)
	if m != nil {
		d.app.c = m.C
	}
	shown := map[string]bool{}
	for _, x := range diags {
		shown[x.String()] = true
	}
	defer func() {
		for _, p := range d.srv.Platforms() {
			if p != d.platform && p != "" {
				d.compileOther(p, shown)
			}
		}
	}()
	if err != nil {
		fmt.Fprintln(os.Stderr, "asuka:", err)
		failure := buildFailure(d.app.dir, diags, err)
		d.srv.Fail(d.platform, failure)
		d.srv.Fail("", failure)
		d.mu.Lock()
		d.broken = true
		fs := d.failed
		d.mu.Unlock()
		for _, f := range fs {
			f(failure)
		}
		return false
	}
	d.srv.Fixed(d.platform)
	d.srv.Fixed("")
	b := m.Encode()
	d.mu.Lock()
	d.module = b
	wasBroken := d.broken
	d.broken = false
	fs, fixed := d.modules, d.fixed
	stale := d.launched && cKey(m.C) != d.playerC
	d.stale = stale
	d.mu.Unlock()
	if stale {
		// Its ffi.* builtins aren't the players': new ones load it.
		d.srv.Keep(d.platform, b)
		d.srv.Keep("", b)
		fmt.Fprintf(d.out, "asuka: compiled in %s; the app's extern declarations changed: building the player again\n", msText(float64(time.Since(start).Microseconds())/1000))
		return true
	}
	if wasBroken {
		for _, f := range fixed {
			f()
		}
	}
	d.srv.Send(d.platform, b)
	d.srv.Send("", b) // players whose platform asuka doesn't know
	for _, f := range fs {
		f(b)
	}
	fmt.Fprintf(d.out, "asuka: compiled in %s\n", msText(float64(time.Since(start).Microseconds())/1000))
	return true
}

// restart has every player load the app's last module that compiled
// again, keeping none of its state: the app starts over, as it does when
// it launches, without its player launching again.
func (d *devLoop) restart() {
	d.mu.Lock()
	m, broken, rs := d.module, d.broken, d.restarts
	d.mu.Unlock()
	if m == nil {
		fmt.Fprintln(d.out, "asuka: nothing to restart: the app hasn't compiled")
		return
	}
	if broken {
		fmt.Fprintln(d.out, "asuka: restarting the last version that compiled")
	}
	d.srv.Restart()
	for _, f := range rs {
		f()
	}
}

// compileFor compiles the app for a player that said hello from
// platform, and sends it the module: the loop's player's platform's
// (and one asuka doesn't know: "") is compile's.
func (d *devLoop) compileFor(platform target.Platform) {
	if platform == d.platform || platform == "" {
		if m := d.current(); m != nil {
			d.srv.Send(platform, m)
		}
		return
	}
	d.compiling.Lock()
	defer d.compiling.Unlock()
	d.compileOther(platform, nil)
}

// compileOther compiles the app for platform, another than the loop's
// player's, and sends its players the module, printing the diagnostics
// shown doesn't have.
func (d *devLoop) compileOther(platform target.Platform, shown map[string]bool) {
	m, diags, err := d.app.compileShown(platform, shown)
	if err != nil {
		fmt.Fprintf(os.Stderr, "asuka: for %s: %v\n", platform.Title(), err)
		d.srv.Fail(platform, buildFailure(d.app.dir, diags, err))
		return
	}
	d.srv.Fixed(platform)
	d.srv.Send(platform, m.Encode())
}

// launch builds and launches the player with the app's C as of the last
// compile, and notes which.
func (d *devLoop) launch(p devPlayer) error {
	d.mu.Lock()
	c := cKey(d.app.c)
	d.mu.Unlock()
	if err := p.launch(); err != nil {
		return err
	}
	d.mu.Lock()
	d.launched, d.playerC, d.stale = true, c, false
	m := d.module
	d.mu.Unlock()
	if m != nil {
		d.srv.Keep(d.platform, m)
		d.srv.Keep("", m)
	}
	return nil
}

func (d *devLoop) isStale() bool {
	d.mu.Lock()
	defer d.mu.Unlock()
	return d.stale
}

// current is the last module that compiled (nil if none has).
func (d *devLoop) current() []byte {
	d.mu.Lock()
	defer d.mu.Unlock()
	return d.module
}

// devPlayer is a platform's player as the loop drives it: launch builds
// it if need be and starts it (again), connected to the loop; stop ends
// it; done is closed when it quits by itself (nil: never).
type devPlayer struct {
	launch func() error
	stop   func()
	done   func() <-chan struct{}
}

// run launches the player and runs the loop until stop (or the player
// quits): a change to the sources is compiled and sent; one to the rest
// of the app (asuka.toml, assets, native code) launches the player
// again, built again. once launches it without watching: it returns as
// the player has the app loaded (the app keeps running), or with what
// the player reported going wrong first.
func (d *devLoop) run(p devPlayer, stop <-chan os.Signal, once bool) error {
	defer d.close()
	if len(d.flows) > 0 {
		// asuka test --gui runs the app as it is: one that doesn't compile
		// fails now, rather than waiting for a save.
		d.mu.Lock()
		broken := d.module == nil || d.broken
		d.mu.Unlock()
		if broken {
			return errors.New("the app doesn't compile (its errors are above)")
		}
	}
	if err := d.launch(p); err != nil {
		return err
	}
	var done <-chan struct{}
	if p.done != nil {
		done = p.done()
	}
	if len(d.flows) > 0 {
		// asuka test --gui: the flows once it's loaded, then the player stops.
		defer func() {
			if p.stop != nil {
				p.stop()
			}
		}()
		select {
		case <-d.firstLoad:
			return d.runFlows(d.flows)
		case msg := <-d.playerErr:
			return fmt.Errorf("the app didn't start: %s", msg)
		case <-done:
			return errors.New("the app quit before it loaded")
		case <-stop:
			return nil
		}
	}
	if once {
		select {
		case <-d.firstLoad:
			return nil
		case msg := <-d.playerErr:
			return fmt.Errorf("the app didn't start: %s", msg)
		case <-done:
			return errors.New("the app quit before it loaded")
		case <-stop:
			if p.stop != nil {
				p.stop()
			}
			return nil
		}
	}
	keys, restore := devKeys()
	defer restore()
	if keys != nil {
		fmt.Fprintln(d.out, "asuka: watching for changes (r: restart, i: the inspector, Ctrl-C: stop)")
	} else {
		fmt.Fprintln(d.out, "asuka: watching for changes (Ctrl-C to stop)")
	}
	w := newWatcher(d.app.proj.WatchRoots()...)
	watchEmbeds(w, d.app.used())
	for {
		select {
		case k := <-keys:
			switch k {
			case 'r', 'R':
				d.restart()
			case 'i', 'I':
				if d.inspectorURL != "" {
					openBrowser(d.inspectorURL)
				}
			}
		case <-stop:
			if p.stop != nil {
				p.stop()
			}
			return nil
		case <-done:
			fmt.Fprintln(d.out, "asuka: the app quit")
			return nil
		case <-w.wake:
			src, proj := w.changed()
			if proj {
				fmt.Fprintln(d.out, "asuka: the app's settings, assets or native code changed: building the player again")
				if cfg, err := loadConfig(d.app.dir); err != nil {
					fmt.Fprintln(os.Stderr, "asuka:", err)
					continue
				} else {
					d.app.cfg, d.app.bundleID = cfg, cfg.ID
				}
				// The compile's uses are what the app needs (refresh takes
				// them, not a compile of its own).
				if src {
					d.compile()
				}
				d.app.refresh()
				watchEmbeds(w, d.app.used())
				if err := d.launch(p); err != nil {
					fmt.Fprintln(os.Stderr, "asuka:", err)
				}
				if p.done != nil {
					done = p.done()
				}
				continue
			}
			if src {
				d.compile()
				d.app.refresh()
				watchEmbeds(w, d.app.used())
				if d.isStale() {
					if err := d.launch(p); err != nil {
						fmt.Fprintln(os.Stderr, "asuka:", err)
					}
					if p.done != nil {
						done = p.done()
					}
				}
			}
		}
	}
}
