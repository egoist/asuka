// Package dap is asuka dev's debug adapter: an editor's debugger (VS
// Code's, Zed's, Neovim's nvim-dap) speaks the Debug Adapter Protocol to
// it over TCP, and it drives the players asuka dev runs the app on
// through their dev connections (internal/devserver's debugger frames,
// core/src/vm/debug.c on the player).
//
// It lasts as long as asuka dev does: the editor attaches to the dev
// loop that's running, whatever reloads, and its breakpoints are the
// adapter's, kept across saves and the players that come and go. Each
// player is a thread (an iPhone's and an emulator's at once, under
// asuka dev --platform ios,android), which stops, steps and goes on by
// itself.
//
// Breakpoints are by file and line, as the editor gives them; the
// adapter resolves each against the app's modules (Options.Resolve: a
// line without code moves to the next that has, in its function) and
// sends the players the lines resolved, again whenever the app compiles
// (Compiled), telling the editor of a breakpoint that moved or now has
// code. A breakpoint may have a condition, a hit condition and a log
// message (a logpoint), which the dev loop compiles for each player's
// module (Dev.SetBreakpoints) and the player evaluates as the line's
// reached. Errors stop the app where they're thrown (exception filters:
// uncaught, all), and an expression evaluates in a paused frame (the
// editor's hovers, watches and console: Options.Compile).
package dap

import (
	"bufio"
	"encoding/json"
	"errors"
	"fmt"
	"hash/fnv"
	"net"
	"os"
	"path/filepath"
	"runtime"
	"slices"
	"sort"
	"strconv"
	"strings"
	"sync"

	"github.com/egoist/asuka/internal/devserver"
)

// Dev is the dev loop's side the adapter drives: its devserver.Server's,
// and its modules', which compile the breakpoints' code.
type Dev interface {
	// SetBreakpoints sets file's breakpoints on every player (none:
	// nil), and on each that connects later: their conditions and log
	// messages compiled for each player's module. It returns why each
	// breakpoint's code doesn't compile ("": it does; nil: all do).
	SetBreakpoints(file string, bps []Breakpoint) []string
	// SetExceptions sets the errors that stop the players' apps
	// (devserver.ExcUncaught, ExcAll; 0: none).
	SetExceptions(filters byte)
	// DebugReset detaches the debugger from every player.
	DebugReset()
}

// Breakpoint is a breakpoint as the adapter sends it: at Line (resolved),
// stopping where Condition holds (an expression; "": always) at the
// hits HitOp and HitN say, or logging LogMessage there (text with {expr}s
// in it) and going on. Key is what its hit count goes with.
type Breakpoint struct {
	Line       int
	Condition  string
	LogMessage string
	HitOp      devserver.HitOp
	HitN       int
	Key        uint32
}

// Options are what the adapter knows of the app.
type Options struct {
	// Resolve is where each of lines of file stops in the app's modules
	// (devserver.ResolveBreakpoints): the line, the next with code, or 0
	// where none does (yet: a file the app doesn't use, a line outside
	// its functions). Nil: each line as it is.
	Resolve func(file string, lines []int) []int
	// Files are the source files the app's modules name (their absolute
	// paths): a path the editor spells otherwise (a symbolic link, a
	// letter's case) is matched to one of them.
	Files func() []string
	// Root is the app's directory: a frame's source in it is named by
	// its path from there, one outside (a dependency's) is shown as
	// such.
	Root string
	// Compile compiles an expression to evaluate in frame f of p's stop
	// (an editor's hover, watch, console). Nil: evaluating isn't
	// supported.
	Compile func(p *devserver.Player, f devserver.Frame, expr string) (*devserver.Fragment, error)
}

// Server is the adapter: it accepts an editor's connection (one at a
// time: a new one takes over) and hears the players (Hello, Gone,
// Stopped, Continued, Values, Output: the dev loop's devserver.Events
// call them).
type Server struct {
	// Dev is set before Listen.
	Dev  Dev
	opts Options
	ln   net.Listener

	// bmu orders what changes the breakpoints (an editor's, a compile's),
	// so the players get them in the order they were decided.
	bmu sync.Mutex

	mu         sync.Mutex
	sess       *session
	threads    map[*devserver.Player]*thread
	nextThread int
	files      map[string]*fileBPs     // by the editor's path
	sent       map[string][]Breakpoint // what Dev has, by the modules' file
	nextBP     int
	refs       map[int]*ref
	nextRef    int
	vseq       uint32
	waits      map[uint32]chan devserver.Variables
	rwaits     map[uint32]chan devserver.Result
	exceptions byte // the filters the editor set
	closed     bool
}

// thread is a player.
type thread struct {
	id   int
	p    *devserver.Player
	name string
	stop *devserver.Stop // nil: running
	gone chan struct{}
}

// fileBPs are an editor's breakpoints in a file.
type fileBPs struct {
	path     string // as the editor names it
	file     string // as the modules do (the last resolve's)
	lines    []int  // as asked (1-based)
	ids      []int
	resolved []int // where each stops (0: nowhere yet)
	// What each does there: its condition, hit condition and log
	// message, as asked; why its code or its hit condition doesn't work
	// ("": it does).
	specs []bpSpec
	errs  []string
}

// bpSpec is what a breakpoint does at its line, as the editor asked.
type bpSpec struct {
	cond, hit, log string
}

type refKind int

const (
	refFrame  refKind = iota // a stack frame (scopes)
	refLocals                // a frame's locals
	refValue                 // a value's children
)

// ref is what a frame's or a variables reference names, while its
// thread is stopped.
type ref struct {
	kind    refKind
	t       *thread
	frame   int
	path    []int // refValue's: the local's index, then a child's at each level
	indexed bool  // refValue's: its children are an array's or a set's elements
}

// New is an adapter that isn't listening yet: the dev loop's events go
// to it from the start.
func New(opts Options) *Server {
	return &Server{opts: opts, threads: map[*devserver.Player]*thread{}, files: map[string]*fileBPs{}, sent: map[string][]Breakpoint{},
		refs: map[int]*ref{}, waits: map[uint32]chan devserver.Variables{}, rwaits: map[uint32]chan devserver.Result{}}
}

// Listen accepts editors' connections on addr.
func (s *Server) Listen(addr string) error {
	ln, err := net.Listen("tcp", addr)
	if err != nil {
		return err
	}
	s.ListenOn(ln)
	return nil
}

// ListenOn accepts editors' connections on ln.
func (s *Server) ListenOn(ln net.Listener) {
	s.ln = ln
	go func() {
		for {
			c, err := ln.Accept()
			if err != nil {
				return
			}
			s.serve(c)
		}
	}()
}

// Addr is where it listens.
func (s *Server) Addr() string {
	if s.ln == nil {
		return ""
	}
	return s.ln.Addr().String()
}

// Close stops listening and ends the editor's session (it hears that
// the app terminated).
func (s *Server) Close() {
	if s == nil {
		return
	}
	s.mu.Lock()
	s.closed = true
	sess := s.sess
	s.sess = nil
	s.mu.Unlock()
	if s.ln != nil {
		s.ln.Close()
	}
	if sess != nil {
		sess.event("terminated", nil)
		sess.close()
	}
}

// ---- The players ----

// Hello notes a player that connected, a thread named name.
func (s *Server) Hello(p *devserver.Player, name string) {
	if s == nil {
		return
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.threads[p] != nil {
		return
	}
	s.nextThread++
	t := &thread{id: s.nextThread, p: p, name: name, gone: make(chan struct{})}
	s.threads[p] = t
	s.eventLocked("thread", map[string]any{"reason": "started", "threadId": t.id})
	// The console said it waits for a player when it attached.
	s.eventLocked("output", map[string]any{"category": "console", "output": name + " connected.\n"})
}

// Gone notes a player that disconnected.
func (s *Server) Gone(p *devserver.Player) {
	if s == nil {
		return
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	t := s.threads[p]
	if t == nil {
		return
	}
	delete(s.threads, p)
	close(t.gone)
	s.dropRefs(t)
	s.eventLocked("thread", map[string]any{"reason": "exited", "threadId": t.id})
}

// Stopped notes a player's app paused (a breakpoint, a step, a pause).
func (s *Server) Stopped(p *devserver.Player, st devserver.Stop) {
	if s == nil {
		return
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	t := s.threads[p]
	if t == nil {
		return
	}
	s.dropRefs(t)
	t.stop = &st
	if s.sess != nil && s.sess.ready {
		s.stoppedEvent(t)
	}
}

// stoppedEvent tells the editor t stopped (mu held).
func (s *Server) stoppedEvent(t *thread) {
	body := map[string]any{"reason": t.stop.Reason, "threadId": t.id, "allThreadsStopped": false}
	if x := t.stop.Exception; x != nil {
		body["description"] = "Paused on an error thrown"
		if x.Uncaught {
			body["description"] = "Paused on an error nothing catches"
		}
		body["text"] = x.Type + ": " + x.Message
	}
	if t.stop.Reason == "breakpoint" && len(t.stop.Frames) > 0 {
		f := t.stop.Frames[0]
		var hit []int
		for _, fb := range s.files {
			if fb.file != f.File {
				continue
			}
			for i, l := range fb.resolved {
				if l == f.Line {
					hit = append(hit, fb.ids[i])
				}
			}
		}
		if hit != nil {
			sort.Ints(hit)
			body["hitBreakpointIds"] = hit
		}
	}
	s.eventLocked("stopped", body)
}

// Continued notes a paused player's app runs again.
func (s *Server) Continued(p *devserver.Player) {
	if s == nil {
		return
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	t := s.threads[p]
	if t == nil || t.stop == nil {
		return
	}
	t.stop = nil
	s.dropRefs(t)
	s.eventLocked("continued", map[string]any{"threadId": t.id, "allThreadsContinued": false})
}

// Values is a player's answer to a variables request.
func (s *Server) Values(p *devserver.Player, seq uint32, v devserver.Variables) {
	if s == nil {
		return
	}
	s.mu.Lock()
	ch := s.waits[seq]
	delete(s.waits, seq)
	s.mu.Unlock()
	if ch != nil {
		ch <- v
	}
}

// Result is a player's answer to an evaluation.
func (s *Server) Result(p *devserver.Player, seq uint32, r devserver.Result) {
	if s == nil {
		return
	}
	s.mu.Lock()
	ch := s.rwaits[seq]
	delete(s.rwaits, seq)
	s.mu.Unlock()
	if ch != nil {
		ch <- r
	}
}

// Logpoint is what a player's logpoint said (or why a breakpoint's code
// failed), shown in the editor's debug console at its line.
func (s *Server) Logpoint(p *devserver.Player, l devserver.Logpoint) {
	if s == nil {
		return
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.sess == nil {
		return
	}
	body := map[string]any{"category": "console", "output": l.Text + "\n", "source": s.source(l.File), "line": s.sess.lineOut(l.Line)}
	if l.Error != "" {
		body["category"] = "stderr"
		body["output"] = l.Error + "\n"
	}
	s.eventLocked("output", body)
}

// Output is what a player's app printed (stderr: to its standard error),
// shown in the editor's debug console.
func (s *Server) Output(p *devserver.Player, stderr bool, text string) {
	if s == nil {
		return
	}
	category := "stdout"
	if stderr {
		category = "stderr"
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	s.eventLocked("output", map[string]any{"category": category, "output": text})
}

// dropRefs forgets t's references, which a stop's end ends (mu held).
func (s *Server) dropRefs(t *thread) {
	for id, r := range s.refs {
		if r.t == t {
			delete(s.refs, id)
		}
	}
}

func (s *Server) newRef(r *ref) int {
	s.nextRef++
	s.refs[s.nextRef] = r
	return s.nextRef
}

// eventLocked sends the editor an event, if one's attached (mu held, so
// that events go in the order they happened).
func (s *Server) eventLocked(name string, body any) {
	if s.sess != nil {
		s.sess.event(name, body)
	}
}

// ---- Breakpoints ----

// canonical is the file the modules name for an editor's path: the same
// path, or one that's the same file (a symbolic link's, a letter's case
// on a system that ignores it); else the path, cleaned.
func (s *Server) canonical(path string) string {
	path = filepath.Clean(path)
	if s.opts.Files == nil {
		return path
	}
	files := s.opts.Files()
	if slices.Contains(files, path) {
		return path
	}
	real := realPath(path)
	fold := runtime.GOOS == "darwin" || runtime.GOOS == "windows"
	for _, f := range files {
		if (fold && strings.EqualFold(f, path)) || realPath(f) == real {
			return f
		}
	}
	return path
}

func realPath(p string) string {
	if r, err := filepath.EvalSymlinks(p); err == nil {
		return r
	}
	return p
}

func (s *Server) resolve(file string, lines []int) []int {
	if s.opts.Resolve == nil {
		return append([]int(nil), lines...)
	}
	return s.opts.Resolve(file, lines)
}

// syncDev sends Dev the breakpoints of each of files that changed, or
// all (force: the app compiled: their code is compiled for each
// platform's module, a platform's first among them), and notes why
// each's code doesn't compile (bmu held, mu not). It returns the files
// whose breakpoints' errors changed.
func (s *Server) syncDev(files []string, force bool) []*fileBPs {
	type set struct {
		file string
		bps  []Breakpoint
		from []*fileBPs // each's
		idx  []int
	}
	var sets []set
	seen := map[string]bool{}
	s.mu.Lock()
	for _, f := range files {
		if seen[f] {
			continue
		}
		seen[f] = true
		st := set{file: f}
		for _, fb := range s.files {
			if fb.file != f {
				continue
			}
			for i, l := range fb.resolved {
				if l <= 0 || slices.ContainsFunc(st.bps, func(b Breakpoint) bool { return b.Line == l }) {
					continue
				}
				b := Breakpoint{Line: l, Condition: fb.specs[i].cond, LogMessage: fb.specs[i].log}
				op, n, err := parseHit(fb.specs[i].hit)
				if err == nil {
					b.HitOp, b.HitN = op, n
				}
				if b.Condition != "" || fb.specs[i].hit != "" {
					h := fnv.New32a()
					h.Write([]byte(b.Condition + "\x00" + fb.specs[i].hit))
					b.Key = h.Sum32()
				}
				st.bps = append(st.bps, b)
				st.from = append(st.from, fb)
				st.idx = append(st.idx, i)
			}
		}
		sort.Sort(byLine(st))
		prev, had := s.sent[f]
		if len(st.bps) == 0 {
			if !had {
				continue
			}
			delete(s.sent, f)
		} else if had && slices.Equal(st.bps, prev) && !force {
			continue
		} else {
			s.sent[f] = st.bps
		}
		sets = append(sets, st)
	}
	s.mu.Unlock()
	if s.Dev == nil {
		return nil
	}
	var changed []*fileBPs
	for _, st := range sets {
		errs := s.Dev.SetBreakpoints(st.file, st.bps)
		s.mu.Lock()
		for k, fb := range st.from {
			i := st.idx[k]
			if i >= len(fb.errs) {
				continue
			}
			e := ""
			if k < len(errs) {
				e = errs[k]
			}
			if _, _, err := parseHit(fb.specs[i].hit); err != nil {
				e = err.Error()
			}
			if fb.errs[i] != e {
				fb.errs[i] = e
				if !slices.Contains(changed, fb) {
					changed = append(changed, fb)
				}
			}
		}
		s.mu.Unlock()
	}
	return changed
}

// byLine sorts a set's breakpoints (and what each is of) by line.
type byLine struct {
	file string
	bps  []Breakpoint
	from []*fileBPs
	idx  []int
}

func (b byLine) Len() int           { return len(b.bps) }
func (b byLine) Less(i, j int) bool { return b.bps[i].Line < b.bps[j].Line }
func (b byLine) Swap(i, j int) {
	b.bps[i], b.bps[j] = b.bps[j], b.bps[i]
	b.from[i], b.from[j] = b.from[j], b.from[i]
	b.idx[i], b.idx[j] = b.idx[j], b.idx[i]
}

// parseHit reads a hit condition as editors write it: a count ("5": the
// fifth hit), or an operator and a count (">= 5", "> 2", "< 3", "<= 3",
// "== 5", "% 3": every third). "": none.
func parseHit(h string) (devserver.HitOp, int, error) {
	h = strings.TrimSpace(h)
	if h == "" {
		return devserver.HitAny, 0, nil
	}
	op := devserver.HitEQ
	for _, o := range []struct {
		s  string
		op devserver.HitOp
	}{{">=", devserver.HitGE}, {"<=", devserver.HitLE}, {"==", devserver.HitEQ}, {">", devserver.HitGT}, {"<", devserver.HitLT}, {"%", devserver.HitMod}, {"=", devserver.HitEQ}} {
		if rest, ok := strings.CutPrefix(h, o.s); ok {
			op, h = o.op, strings.TrimSpace(rest)
			break
		}
	}
	n, err := strconv.Atoi(h)
	if err != nil || n < 0 {
		return devserver.HitAny, 0, fmt.Errorf("The hit count isn't a count: write 5, >= 5, > 5, <= 5, < 5 or %% 5")
	}
	return op, n, nil
}

// setBreakpoints replaces the editor's breakpoints in path with lines,
// which do what specs say there, and says where each stops.
func (s *Server) setBreakpoints(path string, lines []int, specs []bpSpec) []breakpoint {
	s.bmu.Lock()
	defer s.bmu.Unlock()
	file := s.canonical(path)
	res := s.resolve(file, lines)
	s.mu.Lock()
	fb := s.files[path]
	touched := []string{file}
	if fb == nil {
		fb = &fileBPs{path: path}
		s.files[path] = fb
	} else if fb.file != file {
		touched = append(touched, fb.file)
	}
	// A line kept keeps its breakpoint's id.
	old := map[int]int{}
	for i, l := range fb.lines {
		old[l] = fb.ids[i]
	}
	fb.file, fb.lines, fb.resolved, fb.ids = file, append([]int(nil), lines...), res, make([]int, len(lines))
	fb.specs, fb.errs = specs, make([]string, len(lines))
	for i, l := range lines {
		if id, ok := old[l]; ok {
			fb.ids[i] = id
			delete(old, l)
		} else {
			s.nextBP++
			fb.ids[i] = s.nextBP
		}
	}
	if len(lines) == 0 {
		delete(s.files, path)
	}
	s.mu.Unlock()
	s.syncDev(touched, false)
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.breakpointsOf(fb)
}

// breakpointsOf is fb's breakpoints as the editor sees them, their
// lines 1-based (mu held).
func (s *Server) breakpointsOf(fb *fileBPs) []breakpoint {
	out := make([]breakpoint, len(fb.lines))
	for i, l := range fb.lines {
		out[i] = s.breakpointOf(fb, i, l)
	}
	return out
}

func (s *Server) breakpointOf(fb *fileBPs, i, asked int) breakpoint {
	b := breakpoint{ID: fb.ids[i], Line: asked}
	if r := fb.resolved[i]; r > 0 {
		b.Verified, b.Line = true, r
		if i < len(fb.errs) && fb.errs[i] != "" {
			b.Message = fb.errs[i]
		}
	} else {
		b.Message = "No code of the app's on this line (yet)"
		b.Reason = "pending"
	}
	return b
}

// Compiled resolves the breakpoints again in the app's new modules,
// before the players get them: the players' lines change where the
// code moved, and the editor hears of each breakpoint that moved, or
// has code now, or no longer has.
func (s *Server) Compiled() {
	if s == nil {
		return
	}
	s.bmu.Lock()
	defer s.bmu.Unlock()
	type job struct {
		fb    *fileBPs
		lines []int
		file  string
		res   []int
	}
	s.mu.Lock()
	jobs := make([]job, 0, len(s.files))
	for _, fb := range s.files {
		jobs = append(jobs, job{fb: fb, lines: append([]int(nil), fb.lines...)})
	}
	s.mu.Unlock()
	for i := range jobs {
		jobs[i].file = s.canonical(jobs[i].fb.path)
		jobs[i].res = s.resolve(jobs[i].file, jobs[i].lines)
	}
	var touched []string
	s.mu.Lock()
	for _, j := range jobs {
		fb := j.fb
		if s.files[fb.path] != fb || !slices.Equal(fb.lines, j.lines) {
			continue // the editor set them again meanwhile
		}
		if fb.file != j.file {
			touched = append(touched, fb.file)
		}
		touched = append(touched, j.file)
		was := fb.resolved
		fb.file, fb.resolved = j.file, j.res
		for i, l := range fb.lines {
			if was[i] != j.res[i] && s.sess != nil {
				b := s.breakpointOf(fb, i, l)
				b.Line = s.sess.lineOut(b.Line)
				s.eventLocked("breakpoint", map[string]any{"reason": "changed", "breakpoint": b})
			}
		}
	}
	s.mu.Unlock()
	// Their code is compiled for each platform's module: again for the
	// new one (a platform's first, which the players of the platform
	// get them for).
	changed := s.syncDev(touched, true)
	s.mu.Lock()
	for _, fb := range changed {
		if s.files[fb.path] != fb || s.sess == nil {
			continue
		}
		for i, l := range fb.lines {
			b := s.breakpointOf(fb, i, l)
			b.Line = s.sess.lineOut(b.Line)
			s.eventLocked("breakpoint", map[string]any{"reason": "changed", "breakpoint": b})
		}
	}
	s.mu.Unlock()
}

// resetLocked forgets the editor's breakpoints and references (mu held):
// it detached.
func (s *Server) resetLocked() {
	s.files = map[string]*fileBPs{}
	s.sent = map[string][]Breakpoint{}
	s.refs = map[int]*ref{}
	s.exceptions = 0
}

// ---- The editor's session ----

type session struct {
	s      *Server
	conn   net.Conn
	wmu    sync.Mutex
	seq    int
	lines1 bool // lines start at 1 (the default; mu)
	cols1  bool // mu
	ready  bool // configurationDone (mu)
	once   sync.Once
	done   chan struct{}
}

func (c *session) write(v any) {
	c.wmu.Lock()
	defer c.wmu.Unlock()
	c.seq++
	switch m := v.(type) {
	case *response:
		m.Seq = c.seq
	case *event:
		m.Seq = c.seq
	}
	if writeMessage(c.conn, v) != nil {
		c.conn.Close()
	}
}

func (c *session) event(name string, body any) {
	c.write(&event{Type: "event", Event: name, Body: body})
}

func (c *session) close() {
	c.once.Do(func() {
		close(c.done)
		c.conn.Close()
	})
}

// serve takes over from the session attached, if any, and serves c.
func (s *Server) serve(c net.Conn) {
	sess := &session{s: s, conn: c, lines1: true, cols1: true, done: make(chan struct{})}
	s.mu.Lock()
	if s.closed {
		s.mu.Unlock()
		c.Close()
		return
	}
	prev := s.sess
	s.sess = sess
	s.mu.Unlock()
	if prev != nil {
		prev.event("output", map[string]any{"category": "console", "output": "Another debugger attached to asuka dev.\n"})
		prev.event("terminated", nil)
		prev.close()
		s.detach()
	}
	go s.read(sess)
}

// detach drops the editor's breakpoints, here and on the players, which
// go on if they were paused.
func (s *Server) detach() {
	s.bmu.Lock()
	defer s.bmu.Unlock()
	s.mu.Lock()
	s.resetLocked()
	s.mu.Unlock()
	if s.Dev != nil {
		s.Dev.DebugReset()
	}
}

func (s *Server) read(sess *session) {
	r := bufio.NewReader(sess.conn)
	for {
		b, err := readMessage(r)
		if err != nil {
			break
		}
		var m message
		if json.Unmarshal(b, &m) != nil || m.Type != "request" {
			continue
		}
		if !s.request(sess, &m) {
			break
		}
	}
	sess.close()
	s.mu.Lock()
	current := s.sess == sess
	if current {
		s.sess = nil
	}
	s.mu.Unlock()
	if current {
		// Gone without disconnecting: as if it had.
		s.detach()
	}
}

func (sess *session) respond(m *message, body any) {
	sess.write(&response{Type: "response", RequestSeq: m.Seq, Success: true, Command: m.Command, Body: body})
}

func (sess *session) fail(m *message, msg string) {
	sess.write(&response{Type: "response", RequestSeq: m.Seq, Success: false, Command: m.Command, Message: msg,
		Body: map[string]any{"error": map[string]any{"id": 1, "format": msg, "showUser": false}}})
}

// request answers m; false: the session ends.
func (s *Server) request(sess *session, m *message) bool {
	switch m.Command {
	case "initialize":
		var a initializeArgs
		json.Unmarshal(m.Arguments, &a)
		s.mu.Lock() // read by events (Compiled's) under it
		sess.lines1 = a.LinesStartAt1 == nil || *a.LinesStartAt1
		sess.cols1 = a.ColumnsStartAt1 == nil || *a.ColumnsStartAt1
		s.mu.Unlock()
		sess.respond(m, capabilities{SupportsConfigurationDoneRequest: true, SupportsConditionalBreakpoints: true,
			SupportsHitConditionalBreakpoints: true, SupportsLogPoints: true, SupportsEvaluateForHovers: s.opts.Compile != nil,
			SupportsExceptionInfoRequest: true, ExceptionBreakpointFilters: exceptionFilters})
		sess.event("initialized", nil)
	case "attach", "launch":
		// Launch is attach: the app is asuka dev's, running already (an
		// editor's launch configuration starts asuka dev, then attaches).
		sess.respond(m, nil)
		s.mu.Lock()
		var names []string
		for _, t := range s.sortedThreads() {
			names = append(names, t.name)
		}
		s.mu.Unlock()
		msg := "Attached to asuka dev: waiting for the app's player to connect.\n"
		if len(names) > 0 {
			msg = "Attached to asuka dev: " + strings.Join(names, ", ") + ".\n"
		}
		sess.event("output", map[string]any{"category": "console", "output": msg})
	case "setBreakpoints":
		var a setBreakpointsArgs
		if err := json.Unmarshal(m.Arguments, &a); err != nil || a.Source.Path == "" {
			sess.fail(m, "setBreakpoints: no source path")
			break
		}
		var lines []int
		var specs []bpSpec
		if a.Breakpoints != nil {
			for _, b := range a.Breakpoints {
				lines = append(lines, sess.lineIn(b.Line))
				specs = append(specs, bpSpec{cond: strings.TrimSpace(b.Condition), hit: strings.TrimSpace(b.HitCondition), log: b.LogMessage})
			}
		} else {
			for _, l := range a.Lines {
				lines = append(lines, sess.lineIn(l))
				specs = append(specs, bpSpec{})
			}
		}
		bps := s.setBreakpoints(a.Source.Path, lines, specs)
		for i := range bps {
			bps[i].Line = sess.lineOut(bps[i].Line)
		}
		sess.respond(m, map[string]any{"breakpoints": bps})
	case "setExceptionBreakpoints":
		var a setExceptionBreakpointsArgs
		json.Unmarshal(m.Arguments, &a)
		var filters byte
		out := []breakpoint{}
		for _, f := range a.Filters {
			switch f {
			case "uncaught":
				filters |= devserver.ExcUncaught
			case "all":
				filters |= devserver.ExcAll
			default:
				out = append(out, breakpoint{Message: "No such filter: " + f})
				continue
			}
			out = append(out, breakpoint{Verified: true})
		}
		s.bmu.Lock()
		s.mu.Lock()
		s.exceptions = filters
		s.mu.Unlock()
		if s.Dev != nil {
			s.Dev.SetExceptions(filters)
		}
		s.bmu.Unlock()
		sess.respond(m, map[string]any{"breakpoints": out})
	case "exceptionInfo":
		var a threadArgs
		json.Unmarshal(m.Arguments, &a)
		s.mu.Lock()
		t := s.threadByID(a.ThreadID)
		var body map[string]any
		if t != nil && t.stop != nil && t.stop.Exception != nil {
			x := t.stop.Exception
			mode := "always"
			if x.Uncaught {
				mode = "unhandled"
			}
			var trace strings.Builder
			for _, f := range t.stop.Frames {
				fmt.Fprintf(&trace, "%s (%s:%d)\n", f.Name, f.File, f.Line)
			}
			body = map[string]any{"exceptionId": x.Type, "description": x.Message, "breakMode": mode,
				"details": map[string]any{"message": x.Message, "typeName": x.Type, "stackTrace": strings.TrimRight(trace.String(), "\n")}}
		}
		s.mu.Unlock()
		if body == nil {
			sess.fail(m, "The thread isn't stopped at an error")
			break
		}
		sess.respond(m, body)
	case "configurationDone":
		s.mu.Lock()
		sess.ready = true
		sess.respond(m, nil)
		if s.sess == sess {
			for _, t := range s.sortedThreads() {
				if t.stop != nil {
					s.stoppedEvent(t)
				}
			}
		}
		s.mu.Unlock()
	case "threads":
		s.mu.Lock()
		ts := []dapThread{}
		for _, t := range s.sortedThreads() {
			ts = append(ts, dapThread{ID: t.id, Name: t.name})
		}
		s.mu.Unlock()
		sess.respond(m, map[string]any{"threads": ts})
	case "stackTrace":
		var a stackTraceArgs
		json.Unmarshal(m.Arguments, &a)
		body, err := s.stackTrace(sess, a)
		if err != nil {
			sess.fail(m, err.Error())
			break
		}
		sess.respond(m, body)
	case "scopes":
		var a scopesArgs
		json.Unmarshal(m.Arguments, &a)
		body, err := s.scopes(a)
		if err != nil {
			sess.fail(m, err.Error())
			break
		}
		sess.respond(m, body)
	case "variables":
		var a variablesArgs
		json.Unmarshal(m.Arguments, &a)
		// Answered when the player is: meanwhile, the editor's other
		// requests (a continue) are.
		go func() {
			body, err := s.variables(sess, a)
			if err != nil {
				sess.fail(m, err.Error())
				return
			}
			sess.respond(m, body)
		}()
	case "continue", "next", "stepIn", "stepOut", "pause":
		var a threadArgs
		json.Unmarshal(m.Arguments, &a)
		cmd := map[string]devserver.DebugCommand{"continue": devserver.Continue, "next": devserver.StepOver, "stepIn": devserver.StepIn,
			"stepOut": devserver.StepOut, "pause": devserver.Pause}[m.Command]
		s.mu.Lock()
		t := s.threadByID(a.ThreadID)
		s.mu.Unlock()
		if t == nil {
			sess.fail(m, fmt.Sprintf("no thread %d", a.ThreadID))
			break
		}
		if err := t.p.Debug(cmd); err != nil {
			sess.fail(m, err.Error())
			break
		}
		if m.Command == "continue" {
			sess.respond(m, map[string]any{"allThreadsContinued": false})
		} else {
			sess.respond(m, nil)
		}
	case "evaluate":
		var a evaluateArgs
		json.Unmarshal(m.Arguments, &a)
		// Answered when the player is, as variables are.
		go func() {
			body, err := s.evaluate(sess, a)
			if err != nil {
				sess.fail(m, err.Error())
				return
			}
			sess.respond(m, body)
		}()
	case "disconnect", "terminate":
		s.mu.Lock()
		current := s.sess == sess
		if current {
			s.sess = nil
		}
		s.mu.Unlock()
		if current {
			s.detach()
		}
		sess.respond(m, nil)
		return false
	default:
		sess.fail(m, m.Command+" isn't supported")
	}
	return true
}

func (sess *session) lineIn(l int) int {
	if sess.lines1 {
		return l
	}
	return l + 1
}

func (sess *session) lineOut(l int) int {
	if sess.lines1 || l == 0 {
		return l
	}
	return l - 1
}

func (s *Server) sortedThreads() []*thread {
	ts := make([]*thread, 0, len(s.threads))
	for _, t := range s.threads {
		ts = append(ts, t)
	}
	sort.Slice(ts, func(i, j int) bool { return ts[i].id < ts[j].id })
	return ts
}

func (s *Server) threadByID(id int) *thread {
	for _, t := range s.threads {
		if t.id == id {
			return t
		}
	}
	return nil
}

var errNotPaused = errors.New("not paused")

// exceptionFilters are the errors an editor may have the app stop at.
var exceptionFilters = []exceptionFilter{
	{Filter: "uncaught", Label: "Uncaught Errors", Description: "Stop where an error is thrown that nothing in the app catches (no catch, no Boundary)", Default: true},
	{Filter: "all", Label: "All Errors", Description: "Stop where any error is thrown, caught or not"},
}

// evaluate evaluates an expression in a frame of a stopped thread, and
// waits for the player's answer, the player's going or the session's
// ending.
func (s *Server) evaluate(sess *session, a evaluateArgs) (any, error) {
	if s.opts.Compile == nil {
		return nil, errors.New("Evaluating expressions isn't supported here")
	}
	s.mu.Lock()
	r := s.refs[a.FrameID]
	if r == nil || r.kind != refFrame || r.t.stop == nil {
		s.mu.Unlock()
		return nil, errors.New("Pause the app (or stop it at a breakpoint) to evaluate: the expression runs in a paused frame")
	}
	t, frame := r.t, r.frame
	f := t.stop.Frames[frame]
	s.mu.Unlock()
	if f.Module == "" {
		return nil, errors.New("This frame's locals aren't known: evaluate in another")
	}
	fr, err := s.opts.Compile(t.p, f, a.Expression)
	if err != nil {
		return nil, err
	}
	s.mu.Lock()
	s.vseq++
	seq := s.vseq
	ch := make(chan devserver.Result, 1)
	s.rwaits[seq] = ch
	s.mu.Unlock()
	defer func() {
		s.mu.Lock()
		delete(s.rwaits, seq)
		s.mu.Unlock()
	}()
	if err := t.p.Evaluate(seq, frame, fr); err != nil {
		return nil, err
	}
	var res devserver.Result
	select {
	case res = <-ch:
	case <-t.gone:
		return nil, errors.New("the player disconnected")
	case <-sess.done:
		return nil, errors.New("detached")
	}
	if res.Error != "" {
		return nil, errors.New(res.Error)
	}
	if res.Value == nil {
		return nil, errors.New("no value")
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if t.stop == nil {
		return nil, errNotPaused
	}
	v := s.variable(t, devserver.ResultFrame|res.Ref, []int{0}, *res.Value)
	return map[string]any{"result": v.Value, "type": v.Type, "variablesReference": v.VariablesReference,
		"namedVariables": v.NamedVariables, "indexedVariables": v.IndexedVariables}, nil
}

func (s *Server) stackTrace(sess *session, a stackTraceArgs) (any, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	t := s.threadByID(a.ThreadID)
	if t == nil {
		return nil, fmt.Errorf("no thread %d", a.ThreadID)
	}
	if t.stop == nil {
		return nil, errNotPaused
	}
	all := t.stop.Frames
	start := min(max(a.StartFrame, 0), len(all))
	end := len(all)
	if a.Levels > 0 {
		end = min(start+a.Levels, end)
	}
	col := 1
	if !sess.cols1 {
		col = 0
	}
	frames := []stackFrame{}
	for i := start; i < end; i++ {
		f := all[i]
		sf := stackFrame{ID: s.newRef(&ref{kind: refFrame, t: t, frame: i}), Name: f.Name, Line: sess.lineOut(f.Line), Column: col}
		if f.File != "" && f.Line > 0 {
			sf.Source = s.source(f.File)
		} else {
			sf.PresentationHint = "subtle"
		}
		frames = append(frames, sf)
	}
	return map[string]any{"stackFrames": frames, "totalFrames": len(all)}, nil
}

// source is a frame's file as the editor knows it (mu held): the path it
// set breakpoints by, when it did, else the modules'; named from the
// app's directory, a dependency's file shown as one.
func (s *Server) source(file string) *source {
	path := file
	for _, fb := range s.files {
		if fb.file == file {
			path = fb.path
			break
		}
	}
	src := &source{Path: path, Name: filepath.Base(file)}
	if s.opts.Root != "" {
		if rel, err := filepath.Rel(s.opts.Root, file); err == nil && !strings.HasPrefix(rel, "..") {
			src.Name = filepath.ToSlash(rel)
			return src
		}
		src.PresentationHint = "deemphasize"
		if dir := filepath.Base(filepath.Dir(file)); dir != "" && dir != "." && dir != string(os.PathSeparator) {
			src.Origin = dir
		}
	}
	return src
}

func (s *Server) scopes(a scopesArgs) (any, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	r := s.refs[a.FrameID]
	if r == nil || r.kind != refFrame || r.t.stop == nil {
		return nil, errors.New("no such frame (the app went on)")
	}
	locals := r.t.stop.Frames[r.frame].Locals
	sc := scope{Name: "Locals", PresentationHint: "locals", NamedVariables: len(locals),
		VariablesReference: s.newRef(&ref{kind: refLocals, t: r.t, frame: r.frame})}
	return map[string]any{"scopes": []scope{sc}}, nil
}

// indexedType reports whether a value of type t (as the language writes
// it) has elements for children: an array, a set.
func indexedType(t string) bool {
	if t == "array" || t == "Set" || strings.HasPrefix(t, "Set<") || strings.HasPrefix(t, "SortedSet") {
		return true
	}
	if !strings.HasPrefix(t, "[") {
		return false
	}
	// [K: V] is a map: a colon at its own level.
	depth := 0
	for _, c := range t {
		switch c {
		case '[', '(', '<':
			depth++
		case ']', ')', '>':
			depth--
		case ':':
			if depth == 1 {
				return false
			}
		}
	}
	return true
}

// variable is v for the editor (mu held), its children a reference by
// their path.
func (s *Server) variable(t *thread, frame int, path []int, v devserver.Variable) variable {
	out := variable{Name: v.Name, Value: v.Value, Type: v.Type}
	if v.Children > 0 {
		r := &ref{kind: refValue, t: t, frame: frame, path: path, indexed: indexedType(v.Type)}
		out.VariablesReference = s.newRef(r)
		if r.indexed {
			out.IndexedVariables = v.Children
		} else {
			out.NamedVariables = v.Children
		}
	}
	return out
}

func (s *Server) variables(sess *session, a variablesArgs) (any, error) {
	s.mu.Lock()
	r := s.refs[a.VariablesReference]
	if r == nil || r.kind == refFrame || r.t.stop == nil {
		s.mu.Unlock()
		return nil, errors.New("no such variable (the app went on)")
	}
	vars := []variable{}
	if r.kind == refLocals {
		if a.Filter != "indexed" {
			for i, l := range r.t.stop.Frames[r.frame].Locals {
				vars = append(vars, s.variable(r.t, r.frame, []int{i}, l))
			}
		}
		s.mu.Unlock()
		return map[string]any{"variables": vars}, nil
	}
	if (a.Filter == "named" && r.indexed) || (a.Filter == "indexed" && !r.indexed) {
		s.mu.Unlock()
		return map[string]any{"variables": vars}, nil
	}
	start, count := max(a.Start, 0), max(a.Count, 0)
	if !r.indexed {
		start, count = 0, 0
	}
	s.vseq++
	seq := s.vseq
	ch := make(chan devserver.Variables, 1)
	s.waits[seq] = ch
	t, frame, path := r.t, r.frame, r.path
	s.mu.Unlock()
	defer func() {
		s.mu.Lock()
		delete(s.waits, seq)
		s.mu.Unlock()
	}()
	if err := t.p.Variables(seq, frame, path, start, count); err != nil {
		return nil, err
	}
	var v devserver.Variables
	select {
	case v = <-ch:
	case <-t.gone:
		return nil, errors.New("the player disconnected")
	case <-sess.done:
		return nil, errors.New("detached")
	}
	if v.Error != "" {
		return nil, errors.New(v.Error)
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if t.stop == nil {
		return nil, errNotPaused
	}
	for i, c := range v.Variables {
		vars = append(vars, s.variable(t, frame, append(slices.Clone(path), start+i), c))
	}
	return map[string]any{"variables": vars}, nil
}
