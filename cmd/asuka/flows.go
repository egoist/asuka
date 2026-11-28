package main

import (
	"bufio"
	"encoding/binary"
	"errors"
	"fmt"
	"image"
	"io"
	"net/url"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
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
// are. Most steps are the core's automation's (core/src/ui/automate.c):
// what a user does (tap, type, swipe, drag, pinch...) reaches the app's
// views as the platform's events would, and what they show is checked as
// the core has it, so a flow runs the same everywhere. The rest are the
// runner's: waiting, relaunching the app, its permissions, a link the
// system opens it with, and screenshots, taken from the device (the
// simulator's, adb's, Chrome's DevTools', the desktop window's) and
// compared with goldens (flows/__screenshots__/<platform>/<name>.png). A
// step that fails is tried again until it passes or flowTimeout passes:
// the app may be loading, animating, waiting on the network. Each flow
// starts the app afresh, its state from its initial values.

// flowTimeout is how long a step is tried before its flow fails.
var flowTimeout = 10 * time.Second

// guiRun is asuka test --gui's settings, beyond the flows.
type guiRun struct {
	update    bool   // record the goldens anew
	junit     string // JUnit XML's file ("": none)
	artifacts string // where screenshots and failures' captures go ("": the app's dist/flows)
}

var guiFlags guiRun

// flow is a file's steps.
type flow struct {
	file  string
	steps []flowStep
}

// name is the flow's name: its file's, without .flow.
func (f flow) name() string { return strings.TrimSuffix(filepath.Base(f.file), ".flow") }

type flowStep struct {
	line int
	verb string
	arg  string
	// cmds are the core's automation's commands the step runs, in order,
	// each tried until it passes.
	cmds []string
	// wait is wait's time; waitfor's timeout (0: flowTimeout).
	wait time.Duration
	// screenshot's and expect screenshot's: the name, the texts whose
	// views are hidden while it's taken, and the share of pixels that may
	// differ (0: the default).
	shot      string
	masks     []string
	tolerance float64
	// permission's: its name and allow, deny or ask.
	perm, setting string
}

// The steps, and what each takes after it (none: nothing).
var flowVerbs = map[string]bool{
	"tap": true, "doubletap": true, "longpress": true, "type": true, "submit": false, "hidekeyboard": false,
	"back": false, "tab": true, "scroll": false, "scrollto": true, "swipe": true, "drag": true, "pinch": true,
	"see": true, "notsee": true, "assert": true, "wait": true, "waitfor": true,
	"openlink": true, "relaunch": false, "background": false, "foreground": false, "permission": true,
	"screenshot": true, "expect": true,
}

func flowVerbList() string {
	return "tap, doubletap, longpress, type, submit, hidekeyboard, back, tab, scroll, scrollto, swipe, drag, pinch, see, notsee, assert, wait, waitfor, openlink, relaunch, background, foreground, permission, screenshot, expect screenshot"
}

// flowPermissions are the permissions a flow sets, by the names it gives
// them.
var flowPermissions = []string{"camera", "microphone", "location", "location-always", "photos", "contacts", "calendar", "reminders", "notifications", "motion", "bluetooth", "media-library"}

var shotName = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9._-]*$`)

// parseFlow reads a flow: a step a line, "verb argument"; # comments.
func parseFlow(file string, src string) (flow, error) {
	f := flow{file: file}
	others := false // a step other than permission came
	for i, l := range strings.Split(src, "\n") {
		l = strings.TrimSpace(l)
		if l == "" || strings.HasPrefix(l, "#") {
			continue
		}
		verb, arg, _ := strings.Cut(l, " ")
		arg = strings.TrimSpace(arg)
		at := fmt.Sprintf("%s:%d", file, i+1)
		needs, ok := flowVerbs[verb]
		if !ok {
			return f, fmt.Errorf("%s: no step %q (%s)", at, verb, flowVerbList())
		}
		if needs != (arg != "") {
			if needs {
				return f, fmt.Errorf("%s: %s what?", at, verb)
			}
			return f, fmt.Errorf("%s: %s takes nothing after it", at, verb)
		}
		s := flowStep{line: i + 1, verb: verb, arg: arg}
		if err := s.parse(); err != nil {
			return f, fmt.Errorf("%s: %v", at, err)
		}
		if verb == "permission" && others {
			return f, fmt.Errorf("%s: permission steps come first: they're set before the flow starts the app", at)
		}
		others = others || verb != "permission"
		f.steps = append(f.steps, s)
	}
	if len(f.steps) == 0 {
		return f, fmt.Errorf("%s: no steps", file)
	}
	return f, nil
}

// lastCut cuts s around the last sep.
func lastCut(s, sep string) (before, after string, found bool) {
	i := strings.LastIndex(s, sep)
	if i < 0 {
		return s, "", false
	}
	return strings.TrimSpace(s[:i]), strings.TrimSpace(s[i+len(sep):]), true
}

// parse reads a step's argument into what it runs.
func (s *flowStep) parse() error {
	a := s.arg
	switch s.verb {
	case "tap", "doubletap", "longpress", "see", "notsee", "tab":
		s.cmds = []string{s.verb + " " + a}
	case "submit", "back", "scroll", "hidekeyboard", "background", "foreground":
		s.cmds = []string{s.verb}
	case "scrollto":
	case "type":
		// type <text> into <field>: the field picked first.
		if text, field, ok := lastCut(a, " into "); ok && text != "" && field != "" {
			s.cmds = []string{"focus " + field, "type " + text}
		} else {
			s.cmds = []string{"type " + a}
		}
	case "swipe":
		dir, rest, _ := strings.Cut(a, " ")
		switch dir {
		case "left", "right", "up", "down":
		default:
			return fmt.Errorf("swipe %s: which way? (swipe left, swipe up on Photo)", dir)
		}
		rest = strings.TrimSpace(rest)
		if rest == "" {
			s.cmds = []string{"swipe " + dir}
			break
		}
		on, ok := strings.CutPrefix(rest, "on ")
		if !ok || strings.TrimSpace(on) == "" {
			return fmt.Errorf("swipe %s %s: swipe %s on <text>", dir, rest, dir)
		}
		s.cmds = []string{"swipe " + dir + " " + strings.TrimSpace(on)}
	case "drag":
		// drag <text> to <text>, to <x>,<y> (a point in the window), by <dx>,<dy>.
		if from, by, ok := lastCut(a, " by "); ok && isPoint(by) {
			s.cmds = []string{"drag " + from + "\t+" + strings.ReplaceAll(by, " ", "")}
			break
		}
		from, to, ok := lastCut(a, " to ")
		if !ok || from == "" || to == "" {
			return fmt.Errorf("drag %s: drag <text> to <text>, to <x>,<y> or by <dx>,<dy>", a)
		}
		if isPoint(to) {
			to = "@" + strings.ReplaceAll(to, " ", "")
		}
		s.cmds = []string{"drag " + from + "\t" + to}
	case "pinch":
		what, scale, _ := lastCut(a, " ")
		v, err := strconv.ParseFloat(scale, 64)
		if err != nil || v <= 0 || what == "" || what == a {
			return fmt.Errorf("pinch %s: pinch <text> <scale> (2: apart, twice as big; 0.5: together)", a)
		}
		s.cmds = []string{"pinch " + what + "\t" + scale}
	case "assert":
		kind, what, _ := strings.Cut(a, " ")
		what = strings.TrimSpace(what)
		if what == "" {
			return fmt.Errorf("assert %s what?", kind)
		}
		switch kind {
		case "visible":
			s.cmds = []string{"see " + what}
		case "hidden":
			s.cmds = []string{"notsee " + what}
		case "enabled", "disabled", "selected", "unselected":
			s.cmds = []string{"check " + kind + " " + what}
		case "value":
			field, want, ok := strings.Cut(what, " = ")
			if !ok {
				field, ok = strings.CutSuffix(what, " =") // empty
			}
			if !ok || strings.TrimSpace(field) == "" {
				return fmt.Errorf("assert value %s: assert value <field> = <text>", what)
			}
			s.cmds = []string{"value " + strings.TrimSpace(field) + "\t" + strings.TrimSpace(want)}
		default:
			return fmt.Errorf("assert %s: visible, hidden, enabled, disabled, selected, unselected or value", kind)
		}
	case "wait":
		d, err := time.ParseDuration(a)
		if err != nil || d < 0 {
			return fmt.Errorf("wait %s (want a duration: 500ms, 2s)", a)
		}
		s.wait = d
	case "waitfor":
		what := a
		if rest, last, ok := lastCut(a, " "); ok && rest != "" {
			if d, err := time.ParseDuration(last); err == nil && d > 0 {
				what, s.wait = rest, d
			}
		}
		s.cmds = []string{"see " + what}
	case "openlink":
		if strings.ContainsAny(a, " \t") || !strings.Contains(a, ":") {
			return fmt.Errorf("openlink %s: a URL (myapp://notes/1, https://example.com/a)", a)
		}
	case "permission":
		name, setting, _ := strings.Cut(a, " ")
		setting = strings.TrimSpace(setting)
		if !contains(flowPermissions, name) {
			return fmt.Errorf("permission %s: which? (%s)", name, strings.Join(flowPermissions, ", "))
		}
		if setting != "allow" && setting != "deny" && setting != "ask" {
			return fmt.Errorf("permission %s %s: allow, deny or ask", name, setting)
		}
		s.perm, s.setting = name, setting
	case "screenshot":
		if !shotName.MatchString(a) {
			return fmt.Errorf("screenshot %s: a name, of letters, digits, ., _ and - (screenshot home)", a)
		}
		s.shot = a
	case "expect":
		rest, ok := strings.CutPrefix(a, "screenshot ")
		if !ok {
			return fmt.Errorf("expect %s: expect screenshot <name>", a)
		}
		// expect screenshot <name> [mask <text>]... [tolerance <n>%]
		rest = strings.TrimSpace(rest)
		if r, tol, ok := lastCut(rest, " tolerance "); ok {
			v, err := strconv.ParseFloat(strings.TrimSuffix(tol, "%"), 64)
			if err != nil || !strings.HasSuffix(tol, "%") || v < 0 || v > 100 {
				return fmt.Errorf("tolerance %s: a share of the pixels (0.5%%)", tol)
			}
			rest, s.tolerance = r, v/100
			if v == 0 {
				s.tolerance = -1 // none may differ
			}
		}
		parts := strings.Split(" "+rest, " mask ")
		name := strings.TrimSpace(parts[0])
		if !shotName.MatchString(name) {
			return fmt.Errorf("expect screenshot %s: a name, of letters, digits, ., _ and - (expect screenshot home)", name)
		}
		s.shot = name
		for _, m := range parts[1:] {
			if m = strings.TrimSpace(m); m == "" {
				return errors.New("mask what?")
			}
			s.masks = append(s.masks, m)
		}
	}
	return nil
}

func isPoint(s string) bool {
	x, y, ok := strings.Cut(strings.ReplaceAll(s, " ", ""), ",")
	if !ok {
		return false
	}
	_, e1 := strconv.ParseFloat(x, 64)
	_, e2 := strconv.ParseFloat(y, 64)
	return e1 == nil && e2 == nil
}

func contains(xs []string, s string) bool {
	for _, x := range xs {
		if x == s {
			return true
		}
	}
	return false
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

// flowDriver is what a platform's player does for a flow outside the
// app: each nil where the platform hasn't it.
type flowDriver struct {
	// screenshot writes what the screen shows (the device's, the page's,
	// the window's) as a PNG.
	screenshot func(path string) error
	// relaunch ends the app's process and starts it again: its state
	// goes, what it stored stays.
	relaunch func() error
	// permission grants (allow), denies (deny) or resets (ask) one of
	// flowPermissions, ahead of the app's asking: a note when the
	// platform doesn't ask for it, else an error.
	permission func(name, setting string) (note string, err error)
	// relaunches: a permission's change ends the app (iOS, Android).
	relaunches bool
	// openURL opens a link the system gives the app: false when it
	// doesn't take that one (the core's automation gives it then).
	openURL func(url string) (bool, error)
	// prepare makes the captures the same each run (the status bar's
	// clock and battery, the system's animations), and returns what
	// puts it back.
	prepare func() (restore func())
	// capturing is told before a capture and after it (the web's caret
	// and animations, stilled).
	capturing func(on bool)
}

// flowRunner runs a loop's flows.
type flowRunner struct {
	d        *devLoop
	drv      *flowDriver
	platform string
	dir      string // the app's
	prepared bool
	restore  func()
	notes    map[string]bool
}

// flowResult is how a flow went.
type flowResult struct {
	f     flow
	err   error
	took  time.Duration
	shots int
}

// runFlows runs the flows on the loop's player, each from a restart but
// the first, and says how each went; it fails if one did.
func (d *devLoop) runFlows(flows []flow, drv *flowDriver) error {
	r := &flowRunner{d: d, drv: drv, platform: string(d.platform), dir: d.app.dir, notes: map[string]bool{}}
	if r.platform == "macos" {
		r.platform = platformMac
	}
	defer func() {
		if r.restore != nil {
			r.restore()
		}
	}()
	start := time.Now()
	var results []flowResult
	failed := 0
	for i, f := range flows {
		t := time.Now()
		err := r.begin(f, i == 0)
		if err == nil {
			err = r.run(f)
		}
		res := flowResult{f: f, err: err, took: time.Since(t)}
		results = append(results, res)
		if err != nil {
			failed++
			fmt.Fprintf(d.out, "FAIL %s (%s)\n    %v\n", f.file, res.took.Round(10*time.Millisecond), err)
			continue
		}
		fmt.Fprintf(d.out, "ok   %s (%d steps, %s)\n", f.file, len(f.steps), res.took.Round(10*time.Millisecond))
	}
	if guiFlags.junit != "" {
		if err := writeFlowsJUnit(guiFlags.junit, r.platform, start, results); err != nil {
			return err
		}
	}
	if failed > 0 {
		return fmt.Errorf("%d of %d flows failed", failed, len(flows))
	}
	fmt.Fprintf(d.out, "ok   %d flows passed\n", len(flows))
	return nil
}

func writeFlowsJUnit(path, platform string, start time.Time, results []flowResult) error {
	s := newJUnitSuite("flows ("+platform+")", start)
	for _, r := range results {
		c := junitCase{Name: r.f.name(), Classname: "flows." + platform, File: r.f.file, Time: r.took.Seconds()}
		if r.err != nil {
			c.Failure = &junitFailure{Message: firstLine(r.err.Error()), Text: r.err.Error()}
		}
		s.Time += c.Time
		s.add(c)
	}
	return writeJUnit(path, "asuka test --gui", []junitSuite{s})
}

// begin starts a flow afresh: its permissions set first (the app
// relaunched where that ends it), from a restart but the first.
func (r *flowRunner) begin(f flow, first bool) error {
	if !r.prepared {
		r.prepared = true
		if r.drv != nil && r.drv.prepare != nil {
			r.restore = r.drv.prepare()
		}
	}
	relaunch := false
	for _, s := range f.steps {
		if s.verb != "permission" {
			break
		}
		if r.drv == nil || r.drv.permission == nil {
			r.note(fmt.Sprintf("permission: %s doesn't ask apps for permissions: the steps do nothing here", r.platform))
			continue
		}
		note, err := r.drv.permission(s.perm, s.setting)
		if err != nil {
			return fmt.Errorf("%s:%d: permission %s %s: %v", f.file, s.line, s.perm, s.setting, err)
		}
		if note != "" {
			r.note(fmt.Sprintf("permission %s: %s", s.perm, note))
			continue
		}
		relaunch = relaunch || r.drv.relaunches
	}
	switch {
	case relaunch:
		return r.relaunch()
	case !first:
		return r.d.restartAndWait()
	}
	return nil
}

// note says something once a run.
func (r *flowRunner) note(s string) {
	if !r.notes[s] {
		r.notes[s] = true
		fmt.Fprintf(r.d.out, "asuka: %s\n", s)
	}
}

func (r *flowRunner) run(f flow) error {
	for _, s := range f.steps {
		at := fmt.Sprintf("%s:%d", f.file, s.line)
		switch s.verb {
		case "permission":
			// Set as the flow began.
		case "wait":
			time.Sleep(s.wait)
		case "scrollto":
			if err := r.d.scrollTo(f, s); err != nil {
				return err
			}
		case "relaunch":
			if err := r.relaunch(); err != nil {
				return fmt.Errorf("%s: relaunch: %v", at, err)
			}
		case "openlink":
			if err := r.openLink(f, s); err != nil {
				return err
			}
		case "screenshot":
			img, err := r.capture(f, s)
			if err != nil {
				return fmt.Errorf("%s: screenshot %s: %v", at, s.shot, err)
			}
			path := filepath.Join(r.artifacts(), s.shot+".png")
			if err := writePNG(path, img); err != nil {
				return err
			}
			fmt.Fprintf(r.d.out, "     %s: %s\n", at, relToCwd(path))
		case "expect":
			if err := r.expectScreenshot(f, s); err != nil {
				return err
			}
		default:
			for _, c := range s.cmds {
				if err := r.d.automate(f, s, c, s.wait); err != nil {
					return err
				}
			}
		}
	}
	return nil
}

// automate runs one of the core's commands until it passes, or timeout
// (0: flowTimeout) passes.
func (d *devLoop) automate(f flow, s flowStep, cmd string, timeout time.Duration) error {
	if timeout <= 0 {
		timeout = flowTimeout
	}
	shown := strings.ReplaceAll(s.verb+" "+s.arg, "\t", " ")
	deadline := time.Now().Add(timeout)
	for {
		ans, err := d.automation.run(cmd, flowTimeout)
		if err != nil {
			return fmt.Errorf("%s:%d: %s: %v", f.file, s.line, shown, err)
		}
		ans = strings.TrimSpace(ans)
		if !strings.HasPrefix(ans, "error: ") {
			return nil
		}
		if time.Now().After(deadline) {
			shows, _ := d.automation.run("dump", flowTimeout)
			return fmt.Errorf("%s:%d: %s: %s\n    the app shows:\n%s", f.file, s.line, shown, strings.TrimPrefix(ans, "error: "), indent(strings.TrimSpace(shows), "      "))
		}
		time.Sleep(100 * time.Millisecond)
	}
}

// openLink gives the app a link as the system opens it with one: by the
// system where the platform's runner can (a scheme the app declares, on
// the simulator and an emulator), else as the host reports it.
func (r *flowRunner) openLink(f flow, s flowStep) error {
	if r.drv != nil && r.drv.openURL != nil {
		ok, err := r.drv.openURL(s.arg)
		if err != nil {
			return fmt.Errorf("%s:%d: openlink %s: %v", f.file, s.line, s.arg, err)
		}
		if ok {
			return nil
		}
	}
	return r.d.automate(f, s, "openlink "+s.arg, 0)
}

// relaunch ends the app and starts it again, waiting until it has
// loaded; a platform that can't restarts it in its process.
func (r *flowRunner) relaunch() error {
	if r.drv == nil || r.drv.relaunch == nil {
		return r.d.restartAndWait()
	}
	d := r.d
	for len(d.reloads) > 0 {
		<-d.reloads
	}
	if err := r.drv.relaunch(); err != nil {
		return err
	}
	timeout := time.After(2 * time.Minute)
	for {
		select {
		case l := <-d.reloads:
			if l.first {
				// Its automation answers (the last process's connection gone).
				for range 100 {
					if _, err := d.automation.run("dump", 2*time.Second); err == nil {
						return nil
					}
					time.Sleep(100 * time.Millisecond)
				}
				return errors.New("the app started again, but doesn't answer")
			}
		case <-timeout:
			return errors.New("the app didn't start again")
		}
	}
}

// artifacts is where the run's screenshots and failures' captures go.
func (r *flowRunner) artifacts() string {
	if guiFlags.artifacts != "" {
		return filepath.Join(guiFlags.artifacts, r.platform)
	}
	return filepath.Join(r.dir, "dist", "flows", r.platform)
}

// goldens is where the flows' screenshots to compare with are kept.
func (r *flowRunner) goldens() string {
	return filepath.Join(r.dir, "flows", "__screenshots__", r.platform)
}

// capture takes a screenshot once the screen is still: its masks' views
// hidden, then two captures in a row the same (an animation over, a
// list's rows bound), or the last of several.
func (r *flowRunner) capture(f flow, s flowStep) (img *image.NRGBA, err error) {
	if r.drv == nil || r.drv.screenshot == nil {
		return nil, fmt.Errorf("%s can't take screenshots", r.platform)
	}
	for _, m := range s.masks {
		ms := s
		ms.verb, ms.arg = "mask", m
		if err := r.d.automate(f, ms, "mask "+m, 0); err != nil {
			return nil, err
		}
	}
	if len(s.masks) > 0 {
		defer r.d.automation.run("unmask", flowTimeout)
	}
	if r.drv.capturing != nil {
		r.drv.capturing(true)
		defer r.drv.capturing(false)
	}
	tmp, err := os.MkdirTemp("", "asuka-shot-")
	if err != nil {
		return nil, err
	}
	defer os.RemoveAll(tmp)
	time.Sleep(300 * time.Millisecond) // what the step started (a push, a scroll) animates
	var last *image.NRGBA
	for i := 0; i < 12; i++ {
		path := filepath.Join(tmp, fmt.Sprintf("%d.png", i))
		if err := r.drv.screenshot(path); err != nil {
			return nil, err
		}
		now, err := readPNG(path)
		if err != nil {
			return nil, fmt.Errorf("the screenshot: %v", err)
		}
		if last != nil && samePixels(last, now) {
			return now, nil
		}
		last = now
		time.Sleep(250 * time.Millisecond)
	}
	return last, nil
}

// expectScreenshot compares the screen with the flow's golden, or
// records it (--update, or none yet outside CI).
func (r *flowRunner) expectScreenshot(f flow, s flowStep) error {
	at := fmt.Sprintf("%s:%d: expect screenshot %s", f.file, s.line, s.shot)
	img, err := r.capture(f, s)
	if err != nil {
		return fmt.Errorf("%s: %v", at, err)
	}
	golden := filepath.Join(r.goldens(), s.shot+".png")
	want, err := readPNG(golden)
	if err != nil && !os.IsNotExist(err) {
		return fmt.Errorf("%s: %v", at, err)
	}
	if guiFlags.update || want == nil {
		if want == nil && !guiFlags.update && inCI() {
			return fmt.Errorf("%s: no golden (%s): record it with asuka test --gui --update, and commit it", at, relToCwd(golden))
		}
		if err := writePNG(golden, img); err != nil {
			return err
		}
		fmt.Fprintf(r.d.out, "     %s:%d: recorded %s\n", f.file, s.line, relToCwd(golden))
		return nil
	}
	o := defaultImageCompare
	switch {
	case s.tolerance < 0:
		o.maxRatio = 0
	case s.tolerance > 0:
		o.maxRatio = s.tolerance
	}
	actual := filepath.Join(r.artifacts(), s.shot+".actual.png")
	diff, err := compareImages(want, img, o)
	if err != nil {
		writePNG(actual, img)
		return fmt.Errorf("%s: %v (another device's?): the screen is in %s", at, err, relToCwd(actual))
	}
	if diff.ratio() <= o.maxRatio {
		return nil
	}
	diffPath := filepath.Join(r.artifacts(), s.shot+".diff.png")
	writePNG(actual, img)
	writePNG(diffPath, diff.diff)
	return fmt.Errorf("%s: %d pixels differ (%.2f%%, %.2f%% may): the screen is in %s, what differs in red in %s; asuka test --gui --update records it",
		at, diff.differ, diff.ratio()*100, o.maxRatio*100, relToCwd(actual), relToCwd(diffPath))
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

// headlessChrome opens page in a headless Chrome of its own, which stop
// quits; devtools is where its DevTools listen (host:port).
func headlessChrome(page string) (stop func(), devtools string, err error) {
	chrome := chromePath()
	if chrome == "" {
		return nil, "", errors.New("no Chrome to run the flows in: install Chrome or Chromium, or name one with ASUKA_CHROME")
	}
	profile, err := os.MkdirTemp("", "asuka-chrome-")
	if err != nil {
		return nil, "", err
	}
	// A phone's width, at one pixel a point, its scroll bars and caret
	// still: the same screenshots each run.
	cmd := exec.Command(chrome, "--headless=new", "--no-first-run", "--no-default-browser-check", "--disable-gpu",
		"--autoplay-policy=no-user-gesture-required", "--user-data-dir="+profile, "--window-size=420,900",
		"--force-device-scale-factor=1", "--hide-scrollbars", "--remote-debugging-port=0", page)
	stderr, err := cmd.StderrPipe()
	if err != nil {
		os.RemoveAll(profile)
		return nil, "", err
	}
	if err := cmd.Start(); err != nil {
		os.RemoveAll(profile)
		return nil, "", err
	}
	// Chrome says where its DevTools listen.
	found := make(chan string, 1)
	go func() {
		sc := bufio.NewScanner(stderr)
		re := regexp.MustCompile(`DevTools listening on (ws://[^\s]+)`)
		sent := false
		for sc.Scan() {
			if m := re.FindStringSubmatch(sc.Text()); m != nil && !sent {
				found <- m[1]
				sent = true
			}
		}
		io.Copy(io.Discard, stderr)
	}()
	browser := ""
	select {
	case browser = <-found:
		if u, err := url.Parse(browser); err == nil {
			devtools = u.Host
		}
	case <-time.After(20 * time.Second):
		// The page runs without them: no screenshots, no permissions.
	}
	exited := make(chan struct{})
	go func() {
		cmd.Wait()
		close(exited)
	}()
	stop = func() {
		// Closed as a user closes it, its renderers with it: one killed
		// leaves its page running a while, whose events' connection finds
		// the next run's server on the same port.
		if browser != "" {
			if c, err := dialCDP(browser); err == nil {
				c.call("Browser.close", nil, nil)
				c.close()
			}
		}
		select {
		case <-exited:
		case <-time.After(5 * time.Second):
			cmd.Process.Kill()
			<-exited
		}
		os.RemoveAll(profile)
	}
	return stop, devtools, nil
}

// webFlowDriver drives the page a headless Chrome shows at url through
// its DevTools: its screenshots, its permissions, its reload.
func webFlowDriver(devtools, pageURL string, reload func()) *flowDriver {
	if devtools == "" {
		return &flowDriver{relaunch: func() error { reload(); return nil }}
	}
	var page *cdp
	var mu sync.Mutex
	conn := func() (*cdp, error) {
		mu.Lock()
		defer mu.Unlock()
		if page != nil {
			return page, nil
		}
		ws, err := cdpTarget(devtools, "page", pageURL)
		if err != nil {
			return nil, err
		}
		page, err = dialCDP(ws)
		return page, err
	}
	origin := pageURL
	if u, err := url.Parse(pageURL); err == nil {
		origin = u.Scheme + "://" + u.Host
	}
	webPerms := map[string][]string{
		"camera": {"camera"}, "microphone": {"microphone"}, "location": {"geolocation"}, "location-always": {"geolocation"},
		"notifications": {"notifications"}, "motion": {"accelerometer", "gyroscope", "magnetometer"},
	}
	return &flowDriver{
		screenshot: func(path string) error {
			c, err := conn()
			if err != nil {
				return err
			}
			b, err := c.screenshot()
			if err != nil {
				return err
			}
			return os.WriteFile(path, b, 0o644)
		},
		relaunch: func() error { reload(); return nil },
		permission: func(name, setting string) (string, error) {
			names, ok := webPerms[name]
			if !ok {
				return "the web doesn't ask for it", nil
			}
			c, err := conn()
			if err != nil {
				return "", err
			}
			state := map[string]string{"allow": "granted", "deny": "denied", "ask": "prompt"}[setting]
			for _, n := range names {
				if err := c.call("Browser.setPermission", map[string]any{"permission": map[string]any{"name": n}, "setting": state, "origin": origin}, nil); err != nil {
					return "", err
				}
			}
			return "", nil
		},
		prepare: func() func() {
			if c, err := conn(); err == nil {
				c.call("Emulation.setEmulatedMedia", map[string]any{"features": []map[string]string{{"name": "prefers-reduced-motion", "value": "reduce"}}}, nil)
			}
			return func() {
				mu.Lock()
				if page != nil {
					page.close()
				}
				mu.Unlock()
			}
		},
		capturing: func(on bool) {
			c, err := conn()
			if err != nil {
				return
			}
			// The caret's blink and what animates, stilled while it's taken.
			if on {
				c.evaluate(`(() => { const s = document.createElement("style"); s.id = "__asuka_still"; s.textContent = "*, *::before, *::after { caret-color: transparent !important; animation-duration: 0s !important; animation-delay: 0s !important; transition-duration: 0s !important; transition-delay: 0s !important; }"; document.head.appendChild(s); })()`)
			} else {
				c.evaluate(`document.getElementById("__asuka_still")?.remove()`)
			}
		},
	}
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
