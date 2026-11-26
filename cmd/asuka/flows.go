package main

import (
	"encoding/binary"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"syscall"
	"time"

	"github.com/egoist/asuka/internal/appbuild"
	"github.com/egoist/asuka/internal/devserver"
)

// asuka test --gui: the app's flows (flows/*.flow), each a user's steps
// through the app, run on a platform's player as asuka dev runs it (the
// simulator, an emulator, the browser, this computer), as Maestro's flows
// are. A step is the core's automation's (core/src/ui/automate.c): tap,
// type, submit, back, tab, scroll, see, notsee; wait and scrollto (scroll
// until the text shows) are the runner's own. A step that
// fails is tried again until it passes or flowTimeout passes: the app may
// be loading, animating, waiting on the network. Each flow starts the
// app afresh, its state from its initial values.

// flowTimeout is how long a step is tried before its flow fails.
var flowTimeout = 10 * time.Second

// flow is a file's steps.
type flow struct {
	file  string
	steps []flowStep
}

type flowStep struct {
	line int
	verb string
	arg  string
}

var flowVerbs = map[string]bool{"tap": true, "type": true, "submit": true, "back": true, "tab": true, "scroll": true, "scrollto": true, "see": true, "notsee": true, "wait": true}

// parseFlow reads a flow: a step a line, "verb argument"; # comments.
func parseFlow(file string, src string) (flow, error) {
	f := flow{file: file}
	for i, l := range strings.Split(src, "\n") {
		l = strings.TrimSpace(l)
		if l == "" || strings.HasPrefix(l, "#") {
			continue
		}
		verb, arg, _ := strings.Cut(l, " ")
		arg = strings.TrimSpace(arg)
		if !flowVerbs[verb] {
			return f, fmt.Errorf("%s:%d: no step %q (tap, type, submit, back, tab, scroll, scrollto, see, notsee, wait)", file, i+1, verb)
		}
		needs := verb != "submit" && verb != "back" && verb != "scroll"
		if needs != (arg != "") {
			if needs {
				return f, fmt.Errorf("%s:%d: %s what?", file, i+1, verb)
			}
			return f, fmt.Errorf("%s:%d: %s takes nothing after it", file, i+1, verb)
		}
		if verb == "wait" {
			if _, err := time.ParseDuration(arg); err != nil {
				return f, fmt.Errorf("%s:%d: wait %s (want a duration: 500ms, 2s)", file, i+1, arg)
			}
		}
		f.steps = append(f.steps, flowStep{i + 1, verb, arg})
	}
	if len(f.steps) == 0 {
		return f, fmt.Errorf("%s: no steps", file)
	}
	return f, nil
}

// loadFlows reads the app's flows: dir/flows/*.flow, by name, those whose
// names contain pattern.
func loadFlows(dir, pattern string) ([]flow, error) {
	files, _ := filepath.Glob(filepath.Join(dir, "flows", "*.flow"))
	sort.Strings(files)
	var out []flow
	for _, file := range files {
		if pattern != "" && !strings.Contains(filepath.Base(file), pattern) {
			continue
		}
		b, err := os.ReadFile(file)
		if err != nil {
			return nil, err
		}
		rel, _ := filepath.Rel(dir, file)
		f, err := parseFlow(filepath.ToSlash(rel), string(b))
		if err != nil {
			return nil, err
		}
		out = append(out, f)
	}
	if len(out) == 0 {
		return nil, fmt.Errorf("no flows in %s%s: a GUI test is a flows/<name>.flow file, a step a line (tap Add, see Count: 1)", filepath.Join(dir, "flows"), matching(pattern))
	}
	return out, nil
}

// automation sends a player the core's automation commands and waits
// for their answers.
type automation struct {
	mu      sync.Mutex
	seq     uint32
	waiting map[uint32]chan string
	// send sends commands under seq to the player (a native player's
	// connection, the web's page by its events): false if none is there.
	send func(seq uint32, commands string) bool
}

func newAutomation() *automation { return &automation{waiting: map[uint32]chan string{}} }

// answered delivers a player's answers (Automated's).
func (a *automation) answered(seq uint32, answers string) {
	a.mu.Lock()
	c := a.waiting[seq]
	delete(a.waiting, seq)
	a.mu.Unlock()
	if c != nil {
		c <- answers
	}
}

// answeredFrame is a web page's answer: u32 seq, then the answers.
func (a *automation) answeredFrame(b []byte) {
	if len(b) >= 4 {
		a.answered(binary.LittleEndian.Uint32(b), string(b[4:]))
	}
}

// run runs one command and returns its answer.
func (a *automation) run(command string, wait time.Duration) (string, error) {
	a.mu.Lock()
	a.seq++
	seq := a.seq
	c := make(chan string, 1)
	a.waiting[seq] = c
	send := a.send
	a.mu.Unlock()
	if send == nil || !send(seq, command) {
		return "", errors.New("no app is connected")
	}
	select {
	case s := <-c:
		return s, nil
	case <-time.After(wait):
		a.mu.Lock()
		delete(a.waiting, seq)
		a.mu.Unlock()
		return "", errors.New("the app didn't answer")
	}
}

// runFlows runs the flows on the loop's player, each from a restart but
// the first, and says how each went; it fails if one did.
func (d *devLoop) runFlows(flows []flow) error {
	failed := 0
	for i, f := range flows {
		if i > 0 {
			if err := d.restartAndWait(); err != nil {
				return err
			}
		}
		start := time.Now()
		if err := d.runFlow(f); err != nil {
			failed++
			fmt.Fprintf(d.out, "FAIL %s (%s)\n    %v\n", f.file, time.Since(start).Round(10*time.Millisecond), err)
			continue
		}
		fmt.Fprintf(d.out, "ok   %s (%d steps, %s)\n", f.file, len(f.steps), time.Since(start).Round(10*time.Millisecond))
	}
	if failed > 0 {
		return fmt.Errorf("%d of %d flows failed", failed, len(flows))
	}
	fmt.Fprintf(d.out, "ok   %d flows passed\n", len(flows))
	return nil
}

func (d *devLoop) runFlow(f flow) error {
	for _, s := range f.steps {
		if s.verb == "wait" {
			w, _ := time.ParseDuration(s.arg)
			time.Sleep(w)
			continue
		}
		if s.verb == "scrollto" {
			if err := d.scrollTo(f, s); err != nil {
				return err
			}
			continue
		}
		cmd := s.verb
		if s.arg != "" {
			cmd += " " + s.arg
		}
		deadline := time.Now().Add(flowTimeout)
		for {
			ans, err := d.automation.run(cmd, flowTimeout)
			if err != nil {
				return fmt.Errorf("%s:%d: %s: %v", f.file, s.line, cmd, err)
			}
			ans = strings.TrimSpace(ans)
			if !strings.HasPrefix(ans, "error: ") {
				break
			}
			if time.Now().After(deadline) {
				shows, _ := d.automation.run("dump", flowTimeout)
				return fmt.Errorf("%s:%d: %s: %s\n    the app shows:\n%s", f.file, s.line, cmd, strings.TrimPrefix(ans, "error: "), indent(strings.TrimSpace(shows), "      "))
			}
			time.Sleep(100 * time.Millisecond)
		}
	}
	return nil
}

// scrollTo scrolls the list that shows a page at a time until it shows
// the step's text, or its end comes, or flowTimeout passes.
func (d *devLoop) scrollTo(f flow, s flowStep) error {
	deadline := time.Now().Add(flowTimeout)
	atEnd := 0 // tries since the list said it's at its end: its last rows bind in a moment
	for {
		ans, err := d.automation.run("see "+s.arg, flowTimeout)
		if err != nil {
			return fmt.Errorf("%s:%d: scrollto %s: %v", f.file, s.line, s.arg, err)
		}
		if !strings.HasPrefix(strings.TrimSpace(ans), "error: ") {
			return nil
		}
		ans, err = d.automation.run("scroll", flowTimeout)
		if err != nil {
			return fmt.Errorf("%s:%d: scrollto %s: %v", f.file, s.line, s.arg, err)
		}
		ended := strings.HasPrefix(strings.TrimSpace(ans), "error: ")
		if ended {
			atEnd++
		} else {
			atEnd = 0
		}
		if time.Now().After(deadline) || atEnd >= 10 {
			shows, _ := d.automation.run("dump", flowTimeout)
			why := "nothing shows it"
			if ended {
				why += " (" + strings.TrimPrefix(strings.TrimSpace(ans), "error: ") + ")"
			}
			return fmt.Errorf("%s:%d: scrollto %s: %s\n    the app shows:\n%s", f.file, s.line, s.arg, why, indent(strings.TrimSpace(shows), "      "))
		}
		time.Sleep(150 * time.Millisecond)
	}
}

// restartAndWait starts the app again with none of its state, and waits
// for the player to say it has.
func (d *devLoop) restartAndWait() error {
	for len(d.reloads) > 0 {
		<-d.reloads
	}
	d.restart()
	timeout := time.After(time.Minute)
	for {
		select {
		case r := <-d.reloads:
			if r.restarted {
				return nil
			}
		case <-timeout:
			return errors.New("the app didn't start again")
		}
	}
}

func indent(s, by string) string {
	if s == "" {
		return by + "(nothing)"
	}
	return by + strings.ReplaceAll(s, "\n", "\n"+by)
}

// The native players' automation: the first that said hello's.
func (d *devLoop) nativeAutomation(srv *devserver.Server) func(seq uint32, commands string) bool {
	return func(seq uint32, commands string) bool {
		p := srv.First()
		return p != nil && p.Automate(seq, commands) == nil
	}
}

// chromePath is a Chrome or Chromium to run a page in, headless
// (ASUKA_CHROME names one); "" if there's none.
func chromePath() string {
	if p := os.Getenv("ASUKA_CHROME"); p != "" {
		return p
	}
	for _, p := range []string{"/Applications/Google Chrome.app/Contents/MacOS/Google Chrome", "/Applications/Chromium.app/Contents/MacOS/Chromium",
		`C:\Program Files\Google\Chrome\Application\chrome.exe`, `C:\Program Files (x86)\Google\Chrome\Application\chrome.exe`} {
		if _, err := os.Stat(p); err == nil {
			return p
		}
	}
	for _, n := range []string{"google-chrome", "google-chrome-stable", "chromium", "chromium-browser"} {
		if p, err := exec.LookPath(n); err == nil {
			return p
		}
	}
	return ""
}

// headlessChrome opens url in a headless Chrome of its own, which stop
// quits.
func headlessChrome(url string) (stop func(), err error) {
	chrome := chromePath()
	if chrome == "" {
		return nil, errors.New("no Chrome to run the flows in: install Chrome or Chromium, or name one with ASUKA_CHROME")
	}
	profile, err := os.MkdirTemp("", "asuka-chrome-")
	if err != nil {
		return nil, err
	}
	cmd := exec.Command(chrome, "--headless=new", "--no-first-run", "--no-default-browser-check", "--disable-gpu",
		"--autoplay-policy=no-user-gesture-required", "--user-data-dir="+profile, "--window-size=420,900", url)
	if err := cmd.Start(); err != nil {
		os.RemoveAll(profile)
		return nil, err
	}
	return func() {
		cmd.Process.Kill()
		cmd.Wait()
		os.RemoveAll(profile)
	}, nil
}

// testGUI is asuka test --gui: the app's flows on a platform's player.
func testGUI(dir, platform, device, pattern, timeout string) error {
	if dir == "" {
		dir = "."
	}
	t, err := time.ParseDuration(timeout)
	if err != nil || t <= 0 {
		return fmt.Errorf("--timeout %s: a duration, as 10s", timeout)
	}
	flowTimeout = t
	flows, err := loadFlows(dir, pattern)
	if err != nil {
		return err
	}
	o := devOptions{platform: platform, device: device, flows: flows, noOpen: true}
	if o.platform == "" {
		if o.platform = thisComputer(); o.platform == "" {
			return errors.New("pick a platform: asuka test --gui --platform ios|android|web <dir>")
		}
	}
	appbuild.Debug = true
	appbuild.DevNoOptimize = true
	stop := make(chan os.Signal, 1)
	signal.Notify(stop, os.Interrupt, syscall.SIGTERM)
	return devOn(dir, &o, stop)
}
