package devserver

// The debugger's side of the connection (core/src/abi/dev.h's frames 17
// to 23, core/src/vm/debug.h): what an editor's debug adapter (DAP) in
// asuka dev builds on.
//
// The app's code runs on the player's VM, on its main thread. A
// breakpoint, a step or a pause stops that thread: the player sends a
// Stop (Events.Stopped: the stack, innermost first, each frame's
// function, file, line and locals) and waits for a command (Player.Debug:
// continue, or a step); meanwhile it answers Player.Variables (a value's
// children, by its path from a local: Events.Values) and pings, on the
// connection's own thread. Events.Continued says it runs again.
//
// Breakpoints are by file and line: the file as the module's functions
// name it (bytecode.FuncDef's File: the source's absolute path for the
// app's and its dependencies' files) and a line that has code
// (ResolveBreakpoints moves one that hasn't to one that has, as an
// editor shows it). Server.SetBreakpoints keeps them per platform and
// file and sends them to its players, and to each that says hello
// later, before its module; a player keeps them across reloads (its new
// module's functions resolve them again). A module that arrives while a
// player is paused lets it go on, stopping nowhere until the module is
// loaded.
//
// What the debugger runs of the CLI's (a breakpoint's condition and log
// message, an expression evaluated while paused: lang/compile's
// Evaluator) is functions of an extension of the player's module
// (bytecode.Extension), which Server.Extend and Player.Evaluate send
// first; a breakpoint's Code names them for each function its line is
// in. Server.SetExceptions has the players stop where an error is
// thrown: one nothing catches, or every one.

import (
	"encoding/binary"
	"encoding/json"
	"sort"

	"github.com/egoist/asuka/lang/bytecode"
	"github.com/egoist/asuka/lang/target"
)

// Debugger frame kinds (core/src/abi/dev.h).
const (
	// KindDebugBreakpoints sets a file's breakpoints: a u32-length
	// string, the file, then u32 n and n u32 lines (n 0: none).
	KindDebugBreakpoints = 17
	// KindDebugCommand is a DebugCommand, a byte.
	KindDebugCommand = 18
	// KindDebugVariables asks for a value's children: u32 its number,
	// u32 the frame, u32 start, u32 count, u32 n, then n u32s, the path.
	KindDebugVariables = 19
	// KindDebugStopped is a Stop, as JSON.
	KindDebugStopped = 20
	// KindDebugContinued: the app runs again (no payload).
	KindDebugContinued = 21
	// KindDebugValues answers KindDebugVariables: u32 its number, then
	// Variables, as JSON.
	KindDebugValues = 22
	// KindDebugReset: the debugger detached (no breakpoints, no step; a
	// paused app goes on).
	KindDebugReset = 23
	// KindDebugExtend is an extension of a module (bytecode.Extension's
	// encoding): the functions breakpoints' code and evaluations name.
	KindDebugExtend = 30
	// KindDebugExceptions is the errors that stop the app (ExcUncaught,
	// ExcAll), a byte.
	KindDebugExceptions = 31
	// KindDebugEvaluate evaluates in a frame of the stop's: u32 its
	// number, u32 the frame, u64 the module's hash, u32 the extension's
	// function, u32 n and n u32 locals, a u32-length descriptor.
	KindDebugEvaluate = 32
	// KindDebugResult answers KindDebugEvaluate: u32 its number, then a
	// Result, as JSON.
	KindDebugResult = 33
	// KindDebugLog is a Logpoint, as JSON.
	KindDebugLog = 34
)

// The errors that stop the app (SetExceptions).
const (
	ExcUncaught = 1 // an error nothing in the app catches
	ExcAll      = 2 // every error thrown
)

// NoFunc is a BreakpointCode's Cond or Log when it has none.
const NoFunc = 0xFFFFFFFF

// ResultFrame, or'ed with a Result's Ref, is the frame Player.Variables
// asks the value's children by (the path's first index 0).
const ResultFrame = 0x80000000

// A HitOp says at which of its hits (counted from 1, when its condition
// holds) a breakpoint stops: the count ==, >=, >, <=, < HitN, or a
// multiple of it.
type HitOp uint8

const (
	HitAny HitOp = iota
	HitEQ
	HitGE
	HitGT
	HitLE
	HitLT
	HitMod
)

// Breakpoint is a breakpoint as a player gets it: its line, a condition
// and a log message (Code: compiled for each function of the player's
// module the line is in), and a hit condition. Key is what its hit count
// goes with: set again with the same, the count stays.
type Breakpoint struct {
	Line      int
	Key       uint32
	Cond, Log bool
	HitOp     HitOp
	HitN      int
	Code      []BreakpointCode
}

// BreakpointCode is a breakpoint's condition and log message compiled for
// a function its line is in: functions of the module's extension, which
// take the frame's locals Params (indexes in the function's Locals).
type BreakpointCode struct {
	Base      uint64 // the module's bytecode.Hash
	Func      uint32 // the module's function
	Cond, Log uint32 // the extension's functions, NoFunc: none
	Params    []uint32
	// Error is why it didn't compile: the breakpoint stops there, the
	// player logging why.
	Error string
}

// Fragment is an expression compiled to evaluate in a frame (lang/
// compile's Fragment): Func, a function of Ext (the module's extension,
// as Base names the module), taking the frame's locals Params, its value
// of descriptor Desc.
type Fragment struct {
	Base   uint64
	Ext    []byte
	Func   uint32
	Params []uint32
	Desc   string
}

// Exception is the error a Stop of reason "exception" is at.
type Exception struct {
	Type     string `json:"type"` // as the language writes it: "Error", "MailError"
	Message  string `json:"message"`
	Uncaught bool   `json:"uncaught"` // nothing in the app catches it
}

// Result answers Player.Evaluate: the value, and its Ref, which asks for
// its children (ResultFrame), or why not.
type Result struct {
	Value *Variable `json:"result,omitempty"`
	Ref   int       `json:"ref"`
	Error string    `json:"error,omitempty"`
}

// Logpoint is what a player's logpoint said (Text), or why a breakpoint's
// code failed (Error: it stopped).
type Logpoint struct {
	File  string `json:"file"`
	Line  int    `json:"line"`
	Text  string `json:"text,omitempty"`
	Error string `json:"error,omitempty"`
}

// A DebugCommand is what a paused player goes on with, or a pause.
type DebugCommand byte

const (
	Continue DebugCommand = 1
	StepOver DebugCommand = 2 // the frame's next line, or its caller's
	StepIn   DebugCommand = 3 // the next line that runs, in whichever function
	StepOut  DebugCommand = 4 // the caller's, once the frame returned
	// Pause stops the app at the next instruction it runs (an app that's
	// idle stops when it next runs code: an event, a timer).
	Pause DebugCommand = 5
)

// Stop is where a player's app paused (KindDebugStopped).
type Stop struct {
	// Reason is "breakpoint", "step", "pause" or "exception".
	Reason    string     `json:"reason"`
	Exception *Exception `json:"exception,omitempty"`
	// Frames are the stack, innermost first: at most 128 of TotalFrames.
	// A frame of a function's body the optimizer inlined has no locals.
	Frames      []Frame `json:"frames"`
	TotalFrames int     `json:"totalFrames"`
}

// Frame is a frame of a Stop's stack.
type Frame struct {
	Name   string     `json:"name"` // the function's ("add", "Counter/onPress")
	File   string     `json:"file"` // as bytecode.FuncDef's File
	Line   int        `json:"line"`
	Locals []Variable `json:"locals"` // its parameters and locals in scope, by name
	// Where it is, for what's compiled to evaluate there: its module's
	// bytecode.Hash (hex; "": a body inlined, whose locals aren't known),
	// its function and its pc.
	Module string `json:"module,omitempty"`
	Func   uint32 `json:"func,omitempty"`
	PC     uint32 `json:"pc,omitempty"`
}

// Variable is a local, or a value's child: a struct's field (by its
// name), an enum case's payload, an array's or a set's element ("[i]"),
// a map's entry (its key as written).
type Variable struct {
	Name string `json:"name"`
	Type string `json:"type"` // as the language writes it: "int", "[string]", "Todo"
	// Value is as the language writes it ("Todo(title: \"Milk\", done:
	// false)", .circle(radius: 2), none), cut short with … past about 200
	// bytes or three levels. A state's cell, a let's memo, a parameter's
	// accessor show what they hold.
	Value string `json:"value"`
	// Children is how many it has, which Player.Variables lists (a
	// struct's fields, an array's elements, ...); 0: none.
	Children int `json:"children,omitempty"`
}

// Variables answers Player.Variables: the children of the value it
// named, from its start (Total in all), or why not.
type Variables struct {
	Total     int        `json:"total"`
	Variables []Variable `json:"variables"`
	Error     string     `json:"error,omitempty"` // "not paused", "no such frame", "no such variable"
}

func encodeBreakpoints(file string, bps []Breakpoint) []byte {
	b := appendStr(nil, file)
	b = binary.LittleEndian.AppendUint32(b, uint32(len(bps)))
	for _, bp := range bps {
		var flags byte
		if bp.Cond {
			flags |= 1
		}
		if bp.Log {
			flags |= 2
		}
		b = binary.LittleEndian.AppendUint32(b, uint32(bp.Line))
		b = binary.LittleEndian.AppendUint32(b, bp.Key)
		b = append(b, flags, byte(bp.HitOp))
		b = binary.LittleEndian.AppendUint32(b, uint32(bp.HitN))
		b = binary.LittleEndian.AppendUint32(b, uint32(len(bp.Code)))
		for _, c := range bp.Code {
			b = binary.LittleEndian.AppendUint64(b, c.Base)
			for _, v := range []uint32{c.Func, c.Cond, c.Log, uint32(len(c.Params))} {
				b = binary.LittleEndian.AppendUint32(b, v)
			}
			for _, v := range c.Params {
				b = binary.LittleEndian.AppendUint32(b, v)
			}
			b = appendStr(b, c.Error)
		}
	}
	return b
}

// Lines are breakpoints at lines, plain.
func Lines(lines []int) []Breakpoint {
	bps := make([]Breakpoint, len(lines))
	for i, l := range lines {
		bps[i] = Breakpoint{Line: l}
	}
	return bps
}

// SetBreakpoints sets the player's breakpoints in file (none left: nil),
// replacing those it had there.
func (p *Player) SetBreakpoints(file string, bps []Breakpoint) error {
	return p.send(KindDebugBreakpoints, encodeBreakpoints(file, bps))
}

// Extend sends the player an extension of its module (bytecode.
// Extension's encoding), unless it has one of the module's as long.
func (p *Player) Extend(ext []byte) error {
	if len(ext) < 16 {
		return nil
	}
	base := binary.LittleEndian.Uint64(ext[8:])
	p.wmu.Lock()
	if p.exts == nil {
		p.exts = map[uint64]int{}
	}
	had := p.exts[base]
	if had < len(ext) {
		p.exts[base] = len(ext)
	}
	p.wmu.Unlock()
	if had >= len(ext) {
		return nil
	}
	return p.send(KindDebugExtend, ext)
}

// Evaluate asks the paused player to evaluate f in frame (an index in
// its stop's Frames): Events.Result hears the answer, under seq.
func (p *Player) Evaluate(seq uint32, frame int, f *Fragment) error {
	if err := p.Extend(f.Ext); err != nil {
		return err
	}
	b := binary.LittleEndian.AppendUint32(nil, seq)
	b = binary.LittleEndian.AppendUint32(b, uint32(frame))
	b = binary.LittleEndian.AppendUint64(b, f.Base)
	b = binary.LittleEndian.AppendUint32(b, f.Func)
	b = binary.LittleEndian.AppendUint32(b, uint32(len(f.Params)))
	for _, v := range f.Params {
		b = binary.LittleEndian.AppendUint32(b, v)
	}
	return p.send(KindDebugEvaluate, appendStr(b, f.Desc))
}

// Debug sends the player a command: Continue or a step goes to its app
// when it's paused (and is dropped when it isn't), Pause stops it.
func (p *Player) Debug(cmd DebugCommand) error { return p.send(KindDebugCommand, []byte{byte(cmd)}) }

// Variables asks a paused player for the children of a value of its
// stop: in frame (an index in Stop.Frames), at path (the index of a local
// in the frame's Locals, then of a child at each level), from start,
// count of them (0: all, up to 1000). Events.Values hears the answer,
// under seq.
func (p *Player) Variables(seq uint32, frame int, path []int, start, count int) error {
	b := binary.LittleEndian.AppendUint32(nil, seq)
	for _, v := range []int{frame, start, count, len(path)} {
		b = binary.LittleEndian.AppendUint32(b, uint32(v))
	}
	for _, v := range path {
		b = binary.LittleEndian.AppendUint32(b, uint32(v))
	}
	return p.send(KindDebugVariables, b)
}

// DebugReset detaches the debugger from the player: its breakpoints
// and step go, and a paused app goes on.
func (p *Player) DebugReset() error { return p.send(KindDebugReset, nil) }

// Ping pings the player, whose connection's thread answers
// (Events.Pong), paused or not.
func (p *Player) Ping() error { return p.send(KindPing, nil) }

// debugSetup is what the debugger set for a platform's players: the
// breakpoints by file, and its module's extension.
type debugSetup struct {
	files map[string][]Breakpoint
	ext   []byte
}

func (s *Server) setup(platform target.Platform) *debugSetup {
	if s.debug == nil {
		s.debug = map[target.Platform]*debugSetup{}
	}
	d := s.debug[platform]
	if d == nil {
		d = &debugSetup{files: map[string][]Breakpoint{}}
		s.debug[platform] = d
	}
	return d
}

// SetBreakpoints sets file's breakpoints (none left: nil) on the players
// of platform (one asuka doesn't know: ""), and on each that says hello
// from it from now on, before its module. Their code is the extension's
// Extend sent.
func (s *Server) SetBreakpoints(platform target.Platform, file string, bps []Breakpoint) {
	s.mu.Lock()
	defer s.mu.Unlock()
	d := s.setup(platform)
	if len(bps) == 0 {
		delete(d.files, file)
	} else {
		d.files[file] = append([]Breakpoint(nil), bps...)
	}
	b := encodeBreakpoints(file, bps)
	for p := range s.ps {
		if p.hello && p.Platform == platform {
			p.send(KindDebugBreakpoints, b)
		}
	}
}

// Extend sends the players of platform its module's extension (bytecode.
// Extension's encoding), and each that says hello from it later.
func (s *Server) Extend(platform target.Platform, ext []byte) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.setup(platform).ext = ext
	for p := range s.ps {
		if p.hello && p.Platform == platform {
			p.Extend(ext)
		}
	}
}

// SetExceptions sets the errors that stop the players' apps (ExcUncaught,
// ExcAll; 0: none), on each that says hello later too.
func (s *Server) SetExceptions(filters byte) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.exceptions = filters
	for p := range s.ps {
		if p.hello {
			p.send(KindDebugExceptions, []byte{filters})
		}
	}
}

// Breakpoints are the breakpoints SetBreakpoints keeps for platform, by
// file.
func (s *Server) Breakpoints(platform target.Platform) map[string][]Breakpoint {
	s.mu.Lock()
	defer s.mu.Unlock()
	out := map[string][]Breakpoint{}
	if d := s.debug[platform]; d != nil {
		for f, bps := range d.files {
			out[f] = append([]Breakpoint(nil), bps...)
		}
	}
	return out
}

// Debug sends every player that said hello a command (Pause: they all
// stop; Continue: they all go on).
func (s *Server) Debug(cmd DebugCommand) {
	for _, p := range s.hellos() {
		p.Debug(cmd)
	}
}

// DebugReset forgets the breakpoints and detaches the debugger from
// every player (DebugReset's).
func (s *Server) DebugReset() {
	s.mu.Lock()
	s.debug = nil
	s.exceptions = 0
	s.mu.Unlock()
	for _, p := range s.hellos() {
		p.DebugReset()
	}
}

func (s *Server) hellos() []*Player {
	s.mu.Lock()
	defer s.mu.Unlock()
	var ps []*Player
	for p := range s.ps {
		if p.hello {
			ps = append(ps, p)
		}
	}
	sort.Slice(ps, func(i, j int) bool { return ps[i].seq < ps[j].seq })
	return ps
}

// sendBreakpoints sends p what the debugger set for its platform (mu
// held), as it says hello: its module's extension, the breakpoints, the
// errors that stop it.
func (s *Server) sendBreakpoints(p *Player) {
	if s.exceptions != 0 {
		p.send(KindDebugExceptions, []byte{s.exceptions})
	}
	d := s.debug[p.Platform]
	if d == nil {
		return
	}
	if d.ext != nil {
		p.Extend(d.ext)
	}
	files := make([]string, 0, len(d.files))
	for f := range d.files {
		files = append(files, f)
	}
	sort.Strings(files)
	for _, f := range files {
		p.send(KindDebugBreakpoints, encodeBreakpoints(f, d.files[f]))
	}
}

// debugFrame reads a debugger frame a player sent; false: not one.
func (s *Server) debugFrame(p *Player, kind byte, b []byte) bool {
	ev := s.ev
	switch kind {
	case KindDebugStopped:
		var st Stop
		if json.Unmarshal(b, &st) == nil && ev.Stopped != nil {
			ev.Stopped(p, st)
		}
	case KindDebugContinued:
		if ev.Continued != nil {
			ev.Continued(p)
		}
	case KindDebugValues:
		if len(b) < 4 {
			return true
		}
		var v Variables
		if json.Unmarshal(b[4:], &v) == nil && ev.Values != nil {
			ev.Values(p, binary.LittleEndian.Uint32(b), v)
		}
	case KindDebugResult:
		if len(b) < 4 {
			return true
		}
		var r Result
		if json.Unmarshal(b[4:], &r) == nil && ev.Result != nil {
			ev.Result(p, binary.LittleEndian.Uint32(b), r)
		}
	case KindDebugLog:
		var l Logpoint
		if json.Unmarshal(b, &l) == nil && ev.Logpoint != nil {
			ev.Logpoint(p, l)
		}
	case KindPing:
		if ev.Pong != nil {
			ev.Pong(p)
		}
	default:
		return false
	}
	return true
}

// ResolveBreakpoints is where in m each of lines of file stops: the line
// itself when it has code, else the first line after it that has, in the
// function whose lines it's among (an editor moves the breakpoint
// there); 0 when none does (a line outside any function, a comment
// after a function's last line).
func ResolveBreakpoints(m *bytecode.Module, file string, lines []int) []int {
	type span struct {
		lo, hi uint32
		lines  []uint32
	}
	var spans []span
	code := map[uint32]bool{}
	for i := range m.Funcs {
		f := &m.Funcs[i]
		var sp span
		for _, l := range f.Lines {
			if codeFile(m, f, l.PC) != file || l.Line == 0 {
				continue
			}
			code[l.Line] = true
			sp.lines = append(sp.lines, l.Line)
			if sp.lo == 0 || l.Line < sp.lo {
				sp.lo = l.Line
			}
			if l.Line > sp.hi {
				sp.hi = l.Line
			}
		}
		if len(sp.lines) > 0 {
			spans = append(spans, sp)
		}
	}
	out := make([]int, len(lines))
	for i, l := range lines {
		want := uint32(l)
		if code[want] {
			out[i] = l
			continue
		}
		// The innermost function around it: the one of the smallest
		// span (a closure's, not the function it's in).
		best, size := uint32(0), ^uint32(0)
		for _, sp := range spans {
			if want < sp.lo || want > sp.hi || sp.hi-sp.lo >= size {
				continue
			}
			next := uint32(0)
			for _, x := range sp.lines {
				if x > want && (next == 0 || x < next) {
					next = x
				}
			}
			if next != 0 {
				best, size = next, sp.hi-sp.lo
			}
		}
		out[i] = int(best)
	}
	return out
}

// codeFile is the file of f's code at pc: a function's inlined there, or
// f's (core/src/vm/debug.c's).
func codeFile(m *bytecode.Module, f *bytecode.FuncDef, pc uint32) string {
	for _, in := range f.Inlines {
		if pc >= in.Start && pc < in.End && int(in.Func) < len(m.Funcs) {
			return m.Funcs[in.Func].File
		}
	}
	return f.File
}

// LineCode is where line of file starts in a function of a module's.
type LineCode struct {
	Func, PC uint32
}

// LineCodes is each function of m's that has code on line of file, and
// the first pc of it there: where a breakpoint's condition is compiled
// to be evaluated.
func LineCodes(m *bytecode.Module, file string, line int) []LineCode {
	var out []LineCode
	for i := range m.Funcs {
		f := &m.Funcs[i]
		for _, l := range f.Lines {
			if int(l.Line) == line && codeFile(m, f, l.PC) == file {
				out = append(out, LineCode{uint32(i), l.PC})
				break
			}
		}
	}
	return out
}
