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
	results := make(chan Result, 1)
	logs := make(chan Logpoint, 1)
	var player *Player
	var seq, resultSeq uint32
	s, err := Listen("127.0.0.1:0", Events{
		Stopped:   func(p *Player, st Stop) { player = p; stopped <- st },
		Continued: func(p *Player) { continued <- true },
		Values:    func(p *Player, n uint32, v Variables) { seq = n; values <- v },
		Result:    func(p *Player, n uint32, r Result) { resultSeq = n; results <- r },
		Logpoint:  func(p *Player, l Logpoint) { logs <- l },
		Pong:      func(p *Player) { pong <- true },
	})
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	s.Send(target.IOS, []byte("v1"))
	// Set before the player says hello: it gets them before its module,
	// its platform's extension first, and the errors that stop it.
	ext := append([]byte("AKBX\x0a\x00\x00\x00"), binary.LittleEndian.AppendUint64(nil, 0xabc)...)
	s.Extend(target.IOS, ext)
	s.SetExceptions(ExcUncaught)
	bps := []Breakpoint{{Line: 12}, {Line: 30, Key: 9, Cond: true, HitOp: HitGE, HitN: 2,
		Code: []BreakpointCode{{Base: 0xabc, Func: 4, Cond: 77, Log: NoFunc, Params: []uint32{1, 3}}, {Base: 0xabc, Func: 5, Cond: NoFunc, Log: NoFunc, Error: "no"}}}}
	s.SetBreakpoints(target.IOS, "/app/app.asuka", bps)
	s.SetBreakpoints(target.Android, "/app/other.asuka", Lines([]int{1}))
	c := dial(t, s)
	defer c.Close()
	c.Write(frame(KindHello, []byte(`{"platform":"ios"}`)))
	if k, b := read(c); k != KindDebugExceptions || b != "\x01" {
		t.Fatalf("%d %q", k, b)
	}
	if k, b := read(c); k != KindDebugExtend || b != string(ext) {
		t.Fatalf("%d %q", k, b)
	}
	want := encodeBreakpoints("/app/app.asuka", bps)
	if k, b := read(c); k != KindDebugBreakpoints || b != string(want) {
		t.Fatalf("%d %q", k, b)
	}
	if k, b := read(c); k != KindModule || b != "v1" {
		t.Fatalf("%d %q", k, b)
	}
	// The payload: a u32-length file, u32 n, then each breakpoint: u32
	// line, u32 key, u8 flags, u8 hit op, u32 hit n, u32 m codes: u64
	// module, u32 function, condition, log, u32 k locals, a u32-length
	// error.
	got := []byte(want)
	u32 := func(off int) uint32 { return binary.LittleEndian.Uint32(got[off:]) }
	if u32(0) != 14 || string(got[4:18]) != "/app/app.asuka" || u32(18) != 2 || u32(22) != 12 || u32(26) != 0 || got[30] != 0 || got[31] != 0 || u32(32) != 0 || u32(36) != 0 {
		t.Fatalf("% x", got)
	}
	if u32(40) != 30 || u32(44) != 9 || got[48] != 1 || got[49] != byte(HitGE) || u32(50) != 2 || u32(54) != 2 ||
		binary.LittleEndian.Uint64(got[58:]) != 0xabc || u32(66) != 4 || u32(70) != 77 || u32(74) != NoFunc || u32(78) != 2 || u32(82) != 1 || u32(86) != 3 || u32(90) != 0 {
		t.Fatalf("% x", got)
	}
	// Changed: sent to the platform's players; none left in a file: n 0.
	s.SetBreakpoints(target.IOS, "/app/app.asuka", nil)
	if k, b := read(c); k != KindDebugBreakpoints || b != string(encodeBreakpoints("/app/app.asuka", nil)) {
		t.Fatalf("%d %q", k, b)
	}
	s.SetBreakpoints(target.IOS, "/app/model.asuka", Lines([]int{3}))
	read(c)
	if bp := s.Breakpoints(target.IOS); !reflect.DeepEqual(bp, map[string][]Breakpoint{"/app/model.asuka": {{Line: 3}}}) {
		t.Fatal(bp)
	}
	// The same extension isn't sent again; a longer one is.
	s.Extend(target.IOS, ext)
	s.SetExceptions(ExcAll)
	if k, b := read(c); k != KindDebugExceptions || b != "\x02" {
		t.Fatalf("%d %q", k, b)
	}

	// It stops: the stack and its locals.
	c.Write(frame(KindDebugStopped, []byte(`{"reason":"exception","exception":{"type":"Oops","message":"bad","uncaught":true},"totalFrames":2,"frames":[{"name":"add","file":"/app/model.asuka","line":3,"locals":[{"name":"a","type":"int","value":"1"}],"module":"0000000000000abc","func":4,"pc":7},{"name":"run","file":"/app/app.asuka","line":9,"locals":[{"name":"p","type":"Point","value":"Point(x: 1, y: 2)","children":2}]}]}`)))
	st := <-stopped
	wantStop := Stop{Reason: "exception", Exception: &Exception{Type: "Oops", Message: "bad", Uncaught: true}, TotalFrames: 2, Frames: []Frame{
		{Name: "add", File: "/app/model.asuka", Line: 3, Locals: []Variable{{Name: "a", Type: "int", Value: "1"}}, Module: "0000000000000abc", Func: 4, PC: 7},
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
	w32 := func(i int) uint32 { return binary.LittleEndian.Uint32([]byte(b)[4*i:]) }
	if k != KindDebugVariables || len(b) != 28 || w32(0) != 7 || w32(1) != 1 || w32(2) != 5 || w32(3) != 10 || w32(4) != 2 || w32(5) != 0 || w32(6) != 2 {
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
	// An evaluation: its extension first (one longer than the player's),
	// then the request; the answer, and a logpoint's line.
	longer := append(append([]byte(nil), ext...), 1, 2)
	player.Evaluate(3, 0, &Fragment{Base: 0xabc, Ext: longer, Func: 99, Params: []uint32{2}, Desc: "i"})
	if k, b := read(c); k != KindDebugExtend || b != string(longer) {
		t.Fatalf("%d %q", k, b)
	}
	k, b = read(c)
	if wantEv := "\x03\x00\x00\x00\x00\x00\x00\x00\xbc\x0a\x00\x00\x00\x00\x00\x00\x63\x00\x00\x00\x01\x00\x00\x00\x02\x00\x00\x00\x01\x00\x00\x00i"; k != KindDebugEvaluate || b != wantEv {
		t.Fatalf("%d %q", k, b)
	}
	player.Evaluate(4, 0, &Fragment{Base: 0xabc, Ext: longer, Func: 99, Desc: "i"})
	if k, _ := read(c); k != KindDebugEvaluate {
		t.Fatalf("the extension again: %d", k)
	}
	c.Write(frame(KindDebugResult, append(binary.LittleEndian.AppendUint32(nil, 3), `{"result":{"name":"","type":"int","value":"7"},"ref":0}`...)))
	if r := <-results; resultSeq != 3 || !reflect.DeepEqual(r, Result{Value: &Variable{Type: "int", Value: "7"}}) {
		t.Fatalf("%d %+v", resultSeq, r)
	}
	c.Write(frame(KindDebugLog, []byte(`{"file":"/app/app.asuka","line":30,"text":"i is 2"}`)))
	if l := <-logs; l != (Logpoint{File: "/app/app.asuka", Line: 30, Text: "i is 2"}) {
		t.Fatalf("%+v", l)
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
	if bp := s.Breakpoints(target.IOS); len(bp) != 0 {
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
