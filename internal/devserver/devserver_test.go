package devserver

import (
	"encoding/binary"
	"io"
	"net"
	"strconv"
	"testing"

	"github.com/egoist/asuka/lang/target"
)

func frame(kind byte, b []byte) []byte {
	h := make([]byte, 5)
	binary.LittleEndian.PutUint32(h, uint32(len(b)+1))
	h[4] = kind
	return append(h, b...)
}

func TestPlayer(t *testing.T) {
	hello := make(chan string, 1)
	logs := make(chan string, 4)
	reloaded := make(chan uint32, 1)
	s, err := Listen("127.0.0.1:0", Events{
		Hello:    func(p *Player, b []byte) { hello <- string(b) },
		Log:      func(p *Player, stderr bool, b []byte) { logs <- strconv.FormatBool(stderr) + ":" + string(b) },
		Reloaded: func(p *Player, kept, us uint32) { reloaded <- kept },
	})
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	s.Send(target.IOS, []byte("v1"))
	c := dial(t, s)
	defer c.Close()
	// Its platform's latest build, once it says hello.
	c.Write(frame(KindHello, []byte(`{"platform":"ios"}`)))
	if h := <-hello; h != `{"platform":"ios"}` {
		t.Fatal(h)
	}
	if k, b := read(c); k != KindModule || b != "v1" {
		t.Fatalf("%d %q", k, b)
	}
	s.Send(target.IOS, []byte("v2"))
	if k, b := read(c); k != KindModule || b != "v2" {
		t.Fatalf("%d %q", k, b)
	}
	c.Write(frame(KindLog, append([]byte{2}, "boom\n"...)))
	if l := <-logs; l != "true:boom\n" {
		t.Fatal(l)
	}
	c.Write(frame(KindReloaded, []byte{3, 0, 0, 0, 9, 0, 0, 0}))
	if k := <-reloaded; k != 3 {
		t.Fatal(k)
	}
}

// Players on different platforms each get their platform's module; one
// whose platform has none yet is Need's.
func TestPlatforms(t *testing.T) {
	need := make(chan target.Platform, 1)
	var s *Server
	s, err := Listen("127.0.0.1:0", Events{Need: func(p *Player, platform target.Platform) { need <- platform }})
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	s.Keep(target.MacOS, []byte("mac 1"))
	mac, android := dial(t, s), dial(t, s)
	defer mac.Close()
	defer android.Close()
	mac.Write(frame(KindHello, []byte(`{"platform":"darwin","device":"desktop"}`)))
	if k, b := read(mac); k != KindModule || b != "mac 1" {
		t.Fatalf("%d %q", k, b)
	}
	android.Write(frame(KindHello, []byte(`{"platform":"android"}`)))
	if p := <-need; p != target.Android {
		t.Fatal(p)
	}
	s.Send(target.Android, []byte("android 1"))
	if k, b := read(android); k != KindModule || b != "android 1" {
		t.Fatalf("%d %q", k, b)
	}
	if ps := s.Platforms(); len(ps) != 2 || ps[0] != target.Android || ps[1] != target.MacOS {
		t.Fatal(ps)
	}
	s.Send(target.MacOS, []byte("mac 2"))
	s.Send(target.Android, []byte("android 2"))
	if _, b := read(mac); b != "mac 2" {
		t.Fatal(b)
	}
	if _, b := read(android); b != "android 2" {
		t.Fatal(b)
	}
}

func dial(t *testing.T, s *Server) net.Conn {
	t.Helper()
	c, err := net.Dial("tcp", "127.0.0.1:"+strconv.Itoa(s.Port()))
	if err != nil {
		t.Fatal(err)
	}
	return c
}

func read(c net.Conn) (byte, string) {
	var h [5]byte
	io.ReadFull(c, h[:])
	n := binary.LittleEndian.Uint32(h[:4])
	if n == 0 {
		return 0, ""
	}
	b := make([]byte, n-1)
	io.ReadFull(c, b)
	return h[4], string(b)
}
