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
	type report struct {
		kept      uint32
		restarted bool
	}
	reloaded := make(chan report, 1)
	s, err := Listen("127.0.0.1:0", Events{
		Hello:    func(p *Player, b []byte) { hello <- string(b) },
		Log:      func(p *Player, stderr bool, b []byte) { logs <- strconv.FormatBool(stderr) + ":" + string(b) },
		Reloaded: func(p *Player, kept, us uint32, restarted bool) { reloaded <- report{kept, restarted} },
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
	if r := <-reloaded; r != (report{3, false}) {
		t.Fatal(r)
	}
	// A restart: the latest build again, to load keeping no state.
	s.Restart()
	if k, b := read(c); k != KindRestart || b != "v2" {
		t.Fatalf("%d %q", k, b)
	}
	c.Write(frame(KindReloaded, []byte{0, 0, 0, 0, 9, 0, 0, 0, 1}))
	if r := <-reloaded; r != (report{0, true}) {
		t.Fatal(r)
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

// A build that failed goes to its platform's players, and to each that
// says hello until it's fixed, after the module; an uncaught error a
// player reports is Uncaught's, and its source goes back to it.
func TestOverlay(t *testing.T) {
	uncaught := make(chan Uncaught, 1)
	var player *Player
	s, err := Listen("127.0.0.1:0", Events{
		Uncaught: func(p *Player, u Uncaught) { player = p; uncaught <- u },
	})
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	s.Send(target.IOS, []byte("v1"))
	f := Failure{Message: "app.asuka:3:5: count isn't declared", Source: "> 3 | count += 1", Details: "1 more error"}
	s.Fail(target.IOS, f)
	c := dial(t, s)
	defer c.Close()
	c.Write(frame(KindHello, []byte(`{"platform":"ios"}`)))
	if k, b := read(c); k != KindModule || b != "v1" {
		t.Fatalf("%d %q", k, b)
	}
	if k, b := read(c); k != KindFailed || b != string(f.Encode()) {
		t.Fatalf("%d %q", k, b)
	}
	s.Fixed(target.Android) // another platform's: nothing
	s.Fixed(target.IOS)
	if k, b := read(c); k != KindFixed || b != "" {
		t.Fatalf("%d %q", k, b)
	}
	s.Fixed(target.IOS) // fixed already: nothing
	s.Fail(target.IOS, f)
	if k, _ := read(c); k != KindFailed {
		t.Fatal(k)
	}
	// An uncaught error, and its source back.
	report := []byte{7, 0, 0, 0, flagDidntStart}
	report = appendStr(appendStr(report, "boom"), "init (/app/app.asuka:2)")
	c.Write(frame(KindUncaught, report))
	u := <-uncaught
	if u != (Uncaught{Seq: 7, DidntStart: true, Message: "boom", Trace: "init (/app/app.asuka:2)"}) {
		t.Fatalf("%+v", u)
	}
	player.Source(u.Seq, "> 2 | throw", "init (app.asuka:2)")
	if k, b := read(c); k != KindSource || b != string(EncodeSource(7, "> 2 | throw", "init (app.asuka:2)")) {
		t.Fatalf("%d %q", k, b)
	}
	if _, ok := DecodeUncaught(report[:len(report)-1]); ok {
		t.Fatal("a cut report decoded")
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
