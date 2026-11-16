// Package devserver is asuka dev's side of a player's connection
// (core/src/abi/dev.h): players connect and say hello (their platform),
// get the app's latest bytecode for their platform and again on every
// build, and send back their logs and how each reload went.
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
)

// Events are what players report.
type Events struct {
	Hello func(p *Player, json []byte)
	// Need is told that a player said hello from a platform there's no
	// module for (Send and Keep's): one compiled for it, sent with Send,
	// goes to it.
	Need     func(p *Player, platform target.Platform)
	Log      func(p *Player, stderr bool, text []byte)
	Reloaded func(p *Player, kept uint32, micros uint32)
	Error    func(p *Player, text []byte)
	Gone     func(p *Player, err error)
}

// Server serves players. A module is a platform's: what players of
// that platform get (the app's packages' files for it, @platform's
// branches), each from when it says hello.
type Server struct {
	ln      net.Listener
	ev      Events
	mu      sync.Mutex
	modules map[target.Platform][]byte
	ps      map[*Player]bool
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
}

// Listen serves on addr (":0" picks a port).
func Listen(addr string, ev Events) (*Server, error) {
	ln, err := net.Listen("tcp", addr)
	if err != nil {
		return nil, err
	}
	s := &Server{ln: ln, ev: ev, ps: map[*Player]bool{}, modules: map[target.Platform][]byte{}}
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
	m := s.modules[platform]
	if m != nil {
		p.send(KindModule, m)
	}
	s.mu.Unlock()
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
				ev.Reloaded(p, binary.LittleEndian.Uint32(b), binary.LittleEndian.Uint32(b[4:]))
			}
		case KindError:
			if ev.Error != nil {
				ev.Error(p, b)
			}
		}
	}
}
