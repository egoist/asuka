package main

import (
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"sync"
	"time"

	"github.com/egoist/asuka/internal/devserver"
)

// asuka dev's performance tools: the overlay each player shows over the
// app (p in the terminal, or the inspector's Performance tab, a player at
// a time), and recordings (t in the terminal: every player's, saved as a
// trace when it stops; or the Performance tab's Record and Stop), which
// the players send back as a devserver.Profile.

// perfCtl drives one player's tools: a phone's or desktop's player
// (devserver.Player's frames), or a page (its events).
type perfCtl struct {
	overlay func(on bool)
	start   func(cpu bool)
	stop    func()
	// state is what asuka dev last asked of it.
	state func() (overlay, recording bool)
	// cpu: it samples the app's functions (not the web's page, whose
	// core has no thread to sample with).
	cpu bool
}

// perfAll is what reaches every player at once: the dev server's, and
// the web's pages.
type perfAll struct {
	overlay func(on bool)
	start   func()          // every player's recording, sampling where it can
	stop    func() []string // the keys of the players asked to stop
}

// profileWait is how long the page waits for a player's recording once
// it's asked to stop: one that's busy (a breakpoint) answers later, and
// the page can ask again.
const profileWait = 20 * time.Second

// devProfiles keeps each player's last recording, for the page, and says
// when one comes.
type devProfiles struct {
	mu   sync.Mutex
	got  map[string]*gotProfile
	cond *sync.Cond
	// save: the terminal's recording, whose players' profiles are saved
	// as traces when they come (by key).
	save map[string]bool
}

type gotProfile struct {
	name string
	data []byte
	at   time.Time
}

func newDevProfiles() *devProfiles {
	p := &devProfiles{got: map[string]*gotProfile{}, save: map[string]bool{}}
	p.cond = sync.NewCond(&p.mu)
	return p
}

// put keeps a player's recording; it reports whether the terminal's
// recording asked for it to be saved.
func (p *devProfiles) put(key, name string, data []byte) (save bool) {
	p.mu.Lock()
	defer p.mu.Unlock()
	p.got[key] = &gotProfile{name, append([]byte(nil), data...), time.Now()}
	save = p.save[key]
	delete(p.save, key)
	p.cond.Broadcast()
	return save
}

// wait waits for a recording of key's that came after since, at most d.
// The timer says it's over itself, under the lock: a deadline the waiter
// computed could be a little after the timer fired, and the waiter, woken
// before it, would wait again with nothing left to wake it.
func (p *devProfiles) wait(key string, since time.Time, d time.Duration) *gotProfile {
	over := false
	deadline := time.AfterFunc(d, func() {
		p.mu.Lock()
		over = true
		p.cond.Broadcast()
		p.mu.Unlock()
	})
	defer deadline.Stop()
	p.mu.Lock()
	defer p.mu.Unlock()
	for {
		if g := p.got[key]; g != nil && !g.at.Before(since) {
			return g
		}
		if over {
			return nil
		}
		p.cond.Wait()
	}
}

func (p *devProfiles) last(key string) *gotProfile {
	p.mu.Lock()
	defer p.mu.Unlock()
	return p.got[key]
}

// togglePerfOverlay turns every player's overlay on or off (the
// terminal's p).
func (d *devLoop) togglePerfOverlay() {
	d.mu.Lock()
	d.perfOverlay = !d.perfOverlay
	on, all := d.perfOverlay, d.perfAll
	d.mu.Unlock()
	for _, a := range all {
		a.overlay(on)
	}
	if on {
		fmt.Fprintln(d.out, "asuka: the performance overlay is on (p: off)")
	} else {
		fmt.Fprintln(d.out, "asuka: the performance overlay is off")
	}
}

// toggleRecording starts a recording on every player, or stops it (the
// terminal's t): each player's is saved as a trace as it comes.
func (d *devLoop) toggleRecording() {
	d.mu.Lock()
	d.perfRecording = !d.perfRecording
	on, all := d.perfRecording, d.perfAll
	d.mu.Unlock()
	if on {
		for _, a := range all {
			a.start()
		}
		fmt.Fprintln(d.out, "asuka: recording a profile (t: stop and save it)")
		return
	}
	var keys []string
	for _, a := range all {
		keys = append(keys, a.stop()...)
	}
	d.profiles.mu.Lock()
	for _, k := range keys {
		d.profiles.save[k] = true
	}
	d.profiles.mu.Unlock()
	if len(keys) == 0 {
		fmt.Fprintln(d.out, "asuka: no app to record")
	}
}

// gotProfile is a player's recording (key: the player, name: who it is):
// kept for the inspector's page, and saved if the terminal asked for it.
func (d *devLoop) gotProfile(key, name string, data []byte) {
	if !d.profiles.put(key, name, data) {
		return
	}
	p, err := devserver.DecodeProfile(data)
	if err != nil {
		fmt.Fprintln(os.Stderr, "asuka:", err)
		return
	}
	trace, err := p.ChromeTrace(name)
	if err != nil {
		fmt.Fprintln(os.Stderr, "asuka:", err)
		return
	}
	path, err := filepath.Abs(profileFileName(name, time.Now()))
	if err == nil {
		err = os.WriteFile(path, trace, 0o644)
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, "asuka: saving the profile:", err)
		return
	}
	fmt.Fprintf(d.out, "asuka: %s's profile: %s\n", name, path)
	fmt.Fprintf(d.out, "asuka: open it in https://ui.perfetto.dev or chrome://tracing; %s", indentAfter(p.Summary(5), "  "))
}

func indentAfter(s, pre string) string {
	lines := strings.Split(strings.TrimRight(s, "\n"), "\n")
	for i := 1; i < len(lines); i++ {
		lines[i] = pre + lines[i]
	}
	return strings.Join(lines, "\n") + "\n"
}

var unsafeName = regexp.MustCompile(`[^A-Za-z0-9]+`)

// profileFileName is where a player's recording is saved, in the current
// directory: asuka-profile-ios-iphone-17-20261127-153012.json.
func profileFileName(player string, at time.Time) string {
	slug := strings.Trim(strings.ToLower(unsafeName.ReplaceAllString(player, "-")), "-")
	if slug == "" {
		slug = "app"
	}
	return fmt.Sprintf("asuka-profile-%s-%s.json", slug, at.Format("20060102-150405"))
}

// nativePerf is what reaches the dev server's players: a player's
// controls, and every player's.
func nativePerf(p *devserver.Player) *perfCtl {
	return &perfCtl{
		overlay: func(on bool) { p.SetPerfOverlay(on) },
		start:   func(cpu bool) { p.StartProfile(cpu, 0) },
		stop:    func() { p.StopProfile() },
		state:   func() (bool, bool) { return p.PerfOverlay(), p.Recording() },
		cpu:     true,
	}
}

func nativePerfAll(srv *devserver.Server) perfAll {
	return perfAll{
		overlay: srv.SetPerfOverlay,
		start: func() {
			for _, p := range srv.Ready() {
				p.StartProfile(true, 0)
			}
		},
		stop: func() []string {
			var keys []string
			for _, p := range srv.Ready() {
				if p.Recording() {
					p.StopProfile()
					keys = append(keys, playerKey(p))
				}
			}
			return keys
		},
	}
}

// playerKey names a dev server's player for the inspector and the
// recordings it made.
func playerKey(p *devserver.Player) string { return fmt.Sprintf("%p", p) }
