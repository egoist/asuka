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
// editor shows it). Server.SetBreakpoints keeps them per file and sends
// them to every player, and to each that says hello later, before its
// module; a player keeps them across reloads (its new module's functions
// resolve them again). A module that arrives while a player is paused
// lets it go on, stopping nowhere until the module is loaded.

import (
	"encoding/binary"
	"encoding/json"
	"sort"

	"github.com/egoist/asuka/lang/bytecode"
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
)

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
	// Reason is "breakpoint", "step" or "pause".
	Reason string `json:"reason"`
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

func encodeBreakpoints(file string, lines []int) []byte {
	b := appendStr(nil, file)
	b = binary.LittleEndian.AppendUint32(b, uint32(len(lines)))
	for _, l := range lines {
		b = binary.LittleEndian.AppendUint32(b, uint32(l))
	}
	return b
}

// SetBreakpoints sets the player's breakpoints in file (lines: none
// left), replacing those it had there.
func (p *Player) SetBreakpoints(file string, lines []int) error {
	return p.send(KindDebugBreakpoints, encodeBreakpoints(file, lines))
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

// SetBreakpoints sets file's breakpoints (lines: none left) on every
// player, and on each that says hello from now on, before its module.
func (s *Server) SetBreakpoints(file string, lines []int) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.breakpoints == nil {
		s.breakpoints = map[string][]int{}
	}
	if len(lines) == 0 {
		delete(s.breakpoints, file)
	} else {
		s.breakpoints[file] = append([]int(nil), lines...)
	}
	b := encodeBreakpoints(file, lines)
	for p := range s.ps {
		if p.hello {
			p.send(KindDebugBreakpoints, b)
		}
	}
}

// Breakpoints are the breakpoints SetBreakpoints keeps, by file.
func (s *Server) Breakpoints() map[string][]int {
	s.mu.Lock()
	defer s.mu.Unlock()
	out := make(map[string][]int, len(s.breakpoints))
	for f, ls := range s.breakpoints {
		out[f] = append([]int(nil), ls...)
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
	s.breakpoints = nil
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

// sendBreakpoints sends p the breakpoints kept (mu held), as it says
// hello.
func (s *Server) sendBreakpoints(p *Player) {
	files := make([]string, 0, len(s.breakpoints))
	for f := range s.breakpoints {
		files = append(files, f)
	}
	sort.Strings(files)
	for _, f := range files {
		p.send(KindDebugBreakpoints, encodeBreakpoints(f, s.breakpoints[f]))
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
