package devserver

import (
	"encoding/binary"
	"reflect"
	"testing"

	"github.com/egoist/asuka/lang/bytecode"
	"github.com/egoist/asuka/lang/target"
)

// The debugger's frames: breakpoints a player gets before its module and
// as they change, commands and variables requests to it, and what it
// says back (stopped, continued, values, a ping's answer).
func TestDebug(t *testing.T) {
	stopped := make(chan Stop, 1)
	continued := make(chan bool, 1)
	values := make(chan Variables, 1)
	pong := make(chan bool, 1)
	var player *Player
	var seq uint32
	s, err := Listen("127.0.0.1:0", Events{
		Stopped:   func(p *Player, st Stop) { player = p; stopped <- st },
		Continued: func(p *Player) { continued <- true },
		Values:    func(p *Player, n uint32, v Variables) { seq = n; values <- v },
		Pong:      func(p *Player) { pong <- true },
	})
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	s.Send(target.IOS, []byte("v1"))
	// Set before the player says hello: it gets them before its module.
	s.SetBreakpoints("/app/app.asuka", []int{12, 30})
	c := dial(t, s)
	defer c.Close()
	c.Write(frame(KindHello, []byte(`{"platform":"ios"}`)))
	want := encodeBreakpoints("/app/app.asuka", []int{12, 30})
	if k, b := read(c); k != KindDebugBreakpoints || b != string(want) {
		t.Fatalf("%d %q", k, b)
	}
	if k, b := read(c); k != KindModule || b != "v1" {
		t.Fatalf("%d %q", k, b)
	}
	// The payload: a u32-length file, u32 n, n u32 lines.
	if got := []byte(want); binary.LittleEndian.Uint32(got) != 14 || string(got[4:18]) != "/app/app.asuka" || binary.LittleEndian.Uint32(got[18:]) != 2 ||
		binary.LittleEndian.Uint32(got[22:]) != 12 || binary.LittleEndian.Uint32(got[26:]) != 30 || len(got) != 30 {
		t.Fatalf("% x", got)
	}
	// Changed: sent to the players; none left in a file: n 0.
	s.SetBreakpoints("/app/app.asuka", nil)
	if k, b := read(c); k != KindDebugBreakpoints || b != string(encodeBreakpoints("/app/app.asuka", nil)) {
		t.Fatalf("%d %q", k, b)
	}
	s.SetBreakpoints("/app/model.asuka", []int{3})
	read(c)
	if bp := s.Breakpoints(); !reflect.DeepEqual(bp, map[string][]int{"/app/model.asuka": {3}}) {
		t.Fatal(bp)
	}

	// It stops: the stack and its locals.
	c.Write(frame(KindDebugStopped, []byte(`{"reason":"breakpoint","totalFrames":2,"frames":[{"name":"add","file":"/app/model.asuka","line":3,"locals":[{"name":"a","type":"int","value":"1"}]},{"name":"run","file":"/app/app.asuka","line":9,"locals":[{"name":"p","type":"Point","value":"Point(x: 1, y: 2)","children":2}]}]}`)))
	st := <-stopped
	wantStop := Stop{Reason: "breakpoint", TotalFrames: 2, Frames: []Frame{
		{Name: "add", File: "/app/model.asuka", Line: 3, Locals: []Variable{{Name: "a", Type: "int", Value: "1"}}},
		{Name: "run", File: "/app/app.asuka", Line: 9, Locals: []Variable{{Name: "p", Type: "Point", Value: "Point(x: 1, y: 2)", Children: 2}}},
	}}
	if !reflect.DeepEqual(st, wantStop) {
		t.Fatalf("%+v", st)
	}
	// Paused, it answers a ping, a variables request, then a step.
	player.Ping()
	if k, b := read(c); k != KindPing || b != "" {
		t.Fatalf("%d %q", k, b)
	}
	c.Write(frame(KindPing, nil))
	<-pong
	player.Variables(7, 1, []int{0, 2}, 5, 10)
	k, b := read(c)
	u32 := func(i int) uint32 { return binary.LittleEndian.Uint32([]byte(b)[4*i:]) }
	if k != KindDebugVariables || len(b) != 28 || u32(0) != 7 || u32(1) != 1 || u32(2) != 5 || u32(3) != 10 || u32(4) != 2 || u32(5) != 0 || u32(6) != 2 {
		t.Fatalf("%d % x", k, b)
	}
	c.Write(frame(KindDebugValues, append(binary.LittleEndian.AppendUint32(nil, 7), `{"total":2,"variables":[{"name":"x","type":"int","value":"1"},{"name":"y","type":"int","value":"2"}]}`...)))
	if v := <-values; seq != 7 || !reflect.DeepEqual(v, Variables{Total: 2, Variables: []Variable{{Name: "x", Type: "int", Value: "1"}, {Name: "y", Type: "int", Value: "2"}}}) {
		t.Fatalf("%d %+v", seq, v)
	}
	c.Write(frame(KindDebugValues, append(binary.LittleEndian.AppendUint32(nil, 8), `{"error":"not paused"}`...)))
	if v := <-values; seq != 8 || v.Error != "not paused" {
		t.Fatalf("%d %+v", seq, v)
	}
	player.Debug(StepOver)
	if k, b := read(c); k != KindDebugCommand || b != "\x02" {
		t.Fatalf("%d %q", k, b)
	}
	c.Write(frame(KindDebugContinued, nil))
	<-continued
	s.Debug(Pause)
	if k, b := read(c); k != KindDebugCommand || b != "\x05" {
		t.Fatalf("%d %q", k, b)
	}
	// Detached: the breakpoints go, here and on the players.
	s.DebugReset()
	if k, b := read(c); k != KindDebugReset || b != "" {
		t.Fatalf("%d %q", k, b)
	}
	if bp := s.Breakpoints(); len(bp) != 0 {
		t.Fatal(bp)
	}
}

// A breakpoint on a line without code moves to the next line that has,
// in the function it's in; one outside every function has none.
func TestResolveBreakpoints(t *testing.T) {
	m := &bytecode.Module{Funcs: []bytecode.FuncDef{
		// run: lines 10 to 20, a blank line 13, and a closure (lines 15
		// and 16) inside it.
		{Name: "run", File: "/app/app.asuka", Lines: []bytecode.Line{{PC: 0, Line: 10}, {PC: 4, Line: 12}, {PC: 9, Line: 14}, {PC: 20, Line: 20}}},
		{Name: "run/closure@14", File: "/app/app.asuka", Lines: []bytecode.Line{{PC: 0, Line: 15}, {PC: 3, Line: 17}}},
		{Name: "other", File: "/app/other.asuka", Lines: []bytecode.Line{{PC: 0, Line: 13}}},
		// A function another's body was inlined in: its lines there are
		// the inlined function's file's.
		{Name: "caller", File: "/app/caller.asuka", Lines: []bytecode.Line{{PC: 0, Line: 2}, {PC: 5, Line: 40}, {PC: 9, Line: 3}},
			Inlines: []bytecode.Inline{{Start: 5, End: 9, Func: 0, Line: 2}}},
	}}
	got := ResolveBreakpoints(m, "/app/app.asuka", []int{10, 13, 16, 18, 40, 9, 25})
	if want := []int{10, 14, 17, 20, 40, 0, 0}; !reflect.DeepEqual(got, want) {
		t.Fatalf("got %v, want %v", got, want)
	}
	if got := ResolveBreakpoints(m, "/app/caller.asuka", []int{2, 40}); !reflect.DeepEqual(got, []int{2, 0}) {
		t.Fatal(got)
	}
}
