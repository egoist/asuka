// Package devserver is asuka dev's side of a player's connection
// (core/src/abi/dev.h): players connect and say hello (their platform),
// get the app's latest bytecode for their platform and again on every
// build (or to restart with, keeping no state), and send back their logs
// and how each reload went. The error overlay a player shows hears from
// it too: a build that failed (and when it's fixed), and the source of an
// error the app didn't catch, which the player reports. The debugger's
// frames (debug.go) set breakpoints, step, and say where the app paused.
package devserver

import (
	"encoding/binary"
	"encoding/json"
	"errors"
	"io"
	"net"
	"sort"
	"sync"

	"github.com/egoist/asuka/lang/target"
)

// Frame kinds (core/src/abi/dev.h).
const (
	KindModule   = 1
	KindPing     = 2
	KindLog      = 3
	KindHello    = 4
	KindReloaded = 5
	KindError    = 6
	KindRestart  = 7
	KindUncaught = 8
	KindSource   = 9
	KindFailed   = 10
	KindFixed    = 11
	// KindInspect asks a player for its views; it answers with KindTree
	// (JSON: core/src/ui/inspect.c's).
	KindInspect = 12
	KindTree    = 13
	// KindHighlight has a player highlight the view at a path in its tree
	// (its indexes from the roots, joined by dots; empty: none).
	KindHighlight = 14
	// KindAutomate has a player run automation commands (u32 a number,
	// then a line each); it answers with KindAutomated (the number, then
	// each command's answer).
	KindAutomate  = 15
	KindAutomated = 16
)

// A reloaded frame's flags.
const flagRestarted = 1

// An uncaught frame's flags.
const flagDidntStart = 1

// Uncaught is an error nothing in the app caught, as a player reports it.
type Uncaught struct {
	Seq uint32 // its number, which its source names (Player.Source)
	// DidntStart: the module's init threw it, and the app didn't start.
	DidntStart bool
	Message    string
	// Trace is where it was thrown: a line per frame, innermost first,
	// "name (file:line)".
	Trace string
}

// Failure is a build that failed, as a player's error overlay shows it:
// its first error, the lines of the source at it, and the rest.
type Failure struct {
	Message, Source, Details string
}

func appendStr(b []byte, s string) []byte {
	b = binary.LittleEndian.AppendUint32(b, uint32(len(s)))
	return append(b, s...)
}

func (f Failure) encode() []byte {
	return appendStr(appendStr(appendStr(nil, f.Message), f.Source), f.Details)
}

// Encode is f as a failed frame's payload (the web's page gets it from
// the dev server's events, not a connection of its own).
func (f Failure) Encode() []byte { return f.encode() }

// DecodeUncaught reads an uncaught frame's payload.
func DecodeUncaught(b []byte) (Uncaught, bool) {
	var u Uncaught
	if len(b) < 5 {
		return u, false
	}
	u.Seq = binary.LittleEndian.Uint32(b)
	u.DidntStart = b[4]&flagDidntStart != 0
	b = b[5:]
	str := func() (string, bool) {
		if len(b) < 4 {
			return "", false
		}
		n := binary.LittleEndian.Uint32(b)
		if uint64(len(b)-4) < uint64(n) {
			return "", false
		}
		s := string(b[4 : 4+n])
		b = b[4+n:]
		return s, true
	}
	var ok1, ok2 bool
	u.Message, ok1 = str()
	u.Trace, ok2 = str()
	return u, ok1 && ok2
}

// EncodeSource is a source frame's payload: an uncaught error's number,
// the lines of the source where it went wrong, its stack as shown.
func EncodeSource(seq uint32, source, details string) []byte {
	b := binary.LittleEndian.AppendUint32(nil, seq)
	return appendStr(appendStr(b, source), details)
}

// Events are what players report.
type Events struct {
	Hello func(p *Player, json []byte)
	// Need is told that a player said hello from a platform there's no
	// module for (Send and Keep's): one compiled for it, sent with Send,
	// goes to it.
	Need func(p *Player, platform target.Platform)
	Log  func(p *Player, stderr bool, text []byte)
	// Reloaded is told of a module a player loaded: the states it kept,
	// how long it took, and whether it was a restart's (Restart).
	Reloaded func(p *Player, kept uint32, micros uint32, restarted bool)
	Error    func(p *Player, text []byte)
	// Uncaught is told of an error nothing in the app caught; its source
	// goes back with Player.Source.
	Uncaught func(p *Player, u Uncaught)
	// Tree is told of a player's views, as JSON, answering Inspect.
	Tree func(p *Player, tree []byte)
	// Automated is told of the answers to Automate's commands.
	Automated func(p *Player, seq uint32, answers string)
	// Stopped is told that a player's app paused in the debugger (a
	// breakpoint, a step, a pause): it waits for Player.Debug.
	Stopped func(p *Player, stop Stop)
	// Continued is told that a paused player's app runs again.
	Continued func(p *Player)
	// Values is told of the answer to Player.Variables, under its seq.
	Values func(p *Player, seq uint32, v Variables)
	// Result is told of the answer to Player.Evaluate, under its seq.
	Result func(p *Player, seq uint32, r Result)
	// Logpoint is told of what a logpoint said, or why a breakpoint's
	// code failed.
	Logpoint func(p *Player, l Logpoint)
	// Pong is told of the answer to Player.Ping.
	Pong func(p *Player)
	// Profile is told of a recording a player made (perf.go), as JSON:
	// DecodeProfile reads it.
	Profile func(p *Player, profile []byte)
	Gone    func(p *Player, err error)
}

// Server serves players. A module is a platform's: what players of
// that platform get (the app's packages' files for it, @platform's
// branches), each from when it says hello.
type Server struct {
	ln      net.Listener
	ev      Events
	mu      sync.Mutex
	modules map[target.Platform][]byte
	failed  map[target.Platform]Failure // a platform's build that failed, until Fixed
	ps      map[*Player]bool
	conns   uint64 // players that connected
	// debug is what the debugger set, by platform (SetBreakpoints,
	// Extend), and exceptions the errors that stop the apps
	// (SetExceptions): every player gets them as it says hello.
	debug      map[target.Platform]*debugSetup
	exceptions byte
	// perf: the performance overlay is on (SetPerfOverlay), for every
	// player that says hello.
	perf bool
}

// Player is a connected player.
type Player struct {
	conn net.Conn
	wmu  sync.Mutex
	Info []byte // its hello
	// Platform is the one its hello names ("": none it knows), once it
	// said hello.
	Platform target.Platform
	hello    bool
	seq      uint64 // the order it connected in
	// exts: the extensions it was sent, by module, how long (Extend:
	// under wmu).
	exts map[uint64]int
	// perf and recording are its overlay's and its recording's, as asuka
	// dev last turned them (under wmu).
	perf, recording bool
}

// Listen serves on addr (":0" picks a port).
func Listen(addr string, ev Events) (*Server, error) {
	ln, err := net.Listen("tcp", addr)
	if err != nil {
		return nil, err
	}
	s := &Server{ln: ln, ev: ev, ps: map[*Player]bool{}, modules: map[target.Platform][]byte{}, failed: map[target.Platform]Failure{}}
	go s.accept()
	return s, nil
}

// Port is the port players connect to.
func (s *Server) Port() int { return s.ln.Addr().(*net.TCPAddr).Port }

// Close stops serving and disconnects every player.
func (s *Server) Close() error {
	err := s.ln.Close()
	s.mu.Lock()
	for p := range s.ps {
		p.conn.Close()
	}
	s.mu.Unlock()
	return err
}

// Players is how many are connected.
func (s *Server) Players() int {
	s.mu.Lock()
	defer s.mu.Unlock()
	return len(s.ps)
}

// Platforms are the platforms of the players connected that said hello,
// sorted.
func (s *Server) Platforms() []target.Platform {
	s.mu.Lock()
	defer s.mu.Unlock()
	seen := map[target.Platform]bool{}
	var out []target.Platform
	for p := range s.ps {
		if p.hello && !seen[p.Platform] {
			seen[p.Platform] = true
			out = append(out, p.Platform)
		}
	}
	sort.Slice(out, func(i, j int) bool { return out[i] < out[j] })
	return out
}

// Keep makes module the one platform's players that say hello from now
// on get, sending it to none connected (players built for another: asuka
// dev builds new ones, which load it).
func (s *Server) Keep(platform target.Platform, module []byte) {
	s.mu.Lock()
	s.modules[platform] = module
	s.mu.Unlock()
}

// Send sends a build's bytecode to platform's players, and to every one
// that says hello from now on.
func (s *Server) Send(platform target.Platform, module []byte) {
	// Sent holding mu, so that a player never gets an older module after
	// a newer one (hello's).
	s.mu.Lock()
	defer s.mu.Unlock()
	s.modules[platform] = module
	for p := range s.ps {
		if p.hello && p.Platform == platform {
			p.send(KindModule, module)
		}
	}
}

// Restart sends every player its platform's module to restart with: it
// loads it keeping none of the app's state, which all starts again.
func (s *Server) Restart() {
	s.mu.Lock()
	defer s.mu.Unlock()
	for p := range s.ps {
		if m := s.modules[p.Platform]; p.hello && m != nil {
			p.send(KindRestart, m)
		}
	}
}

// Fail tells platform's players, and each that says hello until Fixed,
// that the app's build failed: their overlay shows it.
func (s *Server) Fail(platform target.Platform, f Failure) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.failed[platform] = f
	for p := range s.ps {
		if p.hello && p.Platform == platform {
			p.send(KindFailed, f.encode())
		}
	}
}

// Fixed tells platform's players the app builds again, if Fail told them
// it didn't. Before the module that's sent next.
func (s *Server) Fixed(platform target.Platform) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if _, ok := s.failed[platform]; !ok {
		return
	}
	delete(s.failed, platform)
	for p := range s.ps {
		if p.hello && p.Platform == platform {
			p.send(KindFixed, nil)
		}
	}
}

// Source answers an uncaught error p reported (seq, its number): the
// lines of the app's source where it went wrong, and its stack as the
// overlay shows it.
// Automate has the player run automation commands, a line each; it
// answers with Events.Automated, under seq.
func (p *Player) Automate(seq uint32, commands string) error {
	return p.send(KindAutomate, append(binary.LittleEndian.AppendUint32(nil, seq), commands...))
}

// Highlight has the player highlight the view at path in the tree it
// last gave ("0.2.1"), or none ("").
func (p *Player) Highlight(path string) error { return p.send(KindHighlight, []byte(path)) }

func (p *Player) Source(seq uint32, source, details string) error {
	return p.send(KindSource, EncodeSource(seq, source, details))
}

// Inspect asks every player that said hello for its views (Events.Tree
// hears them); how many it asked.
// First is the first player that said hello (none: nil).
func (s *Server) First() *Player {
	s.mu.Lock()
	defer s.mu.Unlock()
	var first *Player
	for p := range s.ps {
		if p.hello && (first == nil || p.seq < first.seq) {
			first = p
		}
	}
	return first
}

func (s *Server) Inspect() int {
	s.mu.Lock()
	var ps []*Player
	for p := range s.ps {
		if p.hello {
			ps = append(ps, p)
		}
	}
	s.mu.Unlock()
	n := 0
	for _, p := range ps {
		if p.send(KindInspect, nil) == nil {
			n++
		}
	}
	return n
}

func (s *Server) accept() {
	for {
		c, err := s.ln.Accept()
		if err != nil {
			return
		}
		go s.Serve(c)
	}
}

// Serve serves a player on c, a connection made either way: one it
// made to the server, or one made to it (an iPhone's, through the cable:
// usbmuxd reaches a port on the phone, the phone can't reach the Mac).
// It returns when the player goes.
func (s *Server) Serve(c net.Conn) {
	if tc, ok := c.(*net.TCPConn); ok {
		tc.SetNoDelay(true)
	}
	p := &Player{conn: c}
	s.mu.Lock()
	s.conns++
	p.seq = s.conns
	s.ps[p] = true
	s.mu.Unlock()
	s.serve(p)
}

func (s *Server) serve(p *Player) {
	err := p.read(s)
	s.mu.Lock()
	delete(s.ps, p)
	s.mu.Unlock()
	p.conn.Close()
	if s.ev.Gone != nil {
		s.ev.Gone(p, err)
	}
}

func (p *Player) send(kind byte, b []byte) error {
	h := make([]byte, 5, 5+len(b))
	binary.LittleEndian.PutUint32(h, uint32(len(b)+1))
	h[4] = kind
	p.wmu.Lock()
	defer p.wmu.Unlock()
	_, err := p.conn.Write(append(h, b...))
	return err
}

// hello notes a player's hello, and sends it its platform's module.
func (s *Server) hello(p *Player, b []byte) {
	var h struct{ Platform string }
	json.Unmarshal(b, &h)
	platform, _ := target.Parse(h.Platform)
	if s.ev.Hello != nil {
		s.ev.Hello(p, b)
	}
	s.mu.Lock()
	p.Info, p.Platform, p.hello = b, platform, true
	// The breakpoints first: its module's code may stop at them as it
	// starts.
	s.sendBreakpoints(p)
	m := s.modules[platform]
	if m != nil {
		p.send(KindModule, m)
	}
	if f, ok := s.failed[platform]; ok {
		p.send(KindFailed, f.encode())
	}
	perf := s.perf
	s.mu.Unlock()
	if perf {
		p.SetPerfOverlay(true)
	}
	if m == nil && s.ev.Need != nil {
		s.ev.Need(p, platform)
	}
}

func (p *Player) read(s *Server) error {
	ev := s.ev
	var h [5]byte
	for {
		if _, err := io.ReadFull(p.conn, h[:]); err != nil {
			if errors.Is(err, io.EOF) {
				return nil
			}
			return err
		}
		n := binary.LittleEndian.Uint32(h[:4])
		if n == 0 || n > 64<<20 {
			return errors.New("devserver: a bad frame")
		}
		b := make([]byte, n-1)
		if _, err := io.ReadFull(p.conn, b); err != nil {
			return err
		}
		switch h[4] {
		case KindHello:
			s.hello(p, b)
		case KindLog:
			if ev.Log != nil && len(b) > 0 {
				ev.Log(p, b[0] == 2, b[1:])
			}
		case KindReloaded:
			if ev.Reloaded != nil && len(b) >= 8 {
				ev.Reloaded(p, binary.LittleEndian.Uint32(b), binary.LittleEndian.Uint32(b[4:]), len(b) > 8 && b[8]&flagRestarted != 0)
			}
		case KindError:
			if ev.Error != nil {
				ev.Error(p, b)
			}
		case KindUncaught:
			if u, ok := DecodeUncaught(b); ok && ev.Uncaught != nil {
				ev.Uncaught(p, u)
			}
		case KindAutomated:
			if ev.Automated != nil && len(b) >= 4 {
				ev.Automated(p, binary.LittleEndian.Uint32(b), string(b[4:]))
			}
		case KindTree:
			if ev.Tree != nil {
				ev.Tree(p, b)
			}
		case KindProfile:
			if ev.Profile != nil {
				ev.Profile(p, b)
			}
		default:
			s.debugFrame(p, h[4], b)
		}
	}
}
