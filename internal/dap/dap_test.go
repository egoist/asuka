package dap

import (
	"bufio"
	"encoding/binary"
	"encoding/json"
	"errors"
	"io"
	"net"
	"reflect"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/egoist/asuka/internal/devserver"
	"github.com/egoist/asuka/lang/bytecode"
	"github.com/egoist/asuka/lang/target"
)

// hang guards a test that waits for a message: never a timing assertion.
const hang = 30 * time.Second

// fakePlayer is a player's side of the dev connection, as the core's
// dev.c speaks it.
type fakePlayer struct {
	t *testing.T
	c net.Conn
}

func dialPlayer(t *testing.T, ds *devserver.Server, hello string) *fakePlayer {
	t.Helper()
	c, err := net.Dial("tcp", "127.0.0.1:"+itoa(ds.Port()))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { c.Close() })
	p := &fakePlayer{t, c}
	p.send(devserver.KindHello, []byte(hello))
	return p
}

func itoa(n int) string {
	b, _ := json.Marshal(n)
	return string(b)
}

func (p *fakePlayer) send(kind byte, b []byte) {
	h := make([]byte, 5)
	binary.LittleEndian.PutUint32(h, uint32(len(b)+1))
	h[4] = kind
	if _, err := p.c.Write(append(h, b...)); err != nil {
		p.t.Fatal(err)
	}
}

// next reads the next frame the CLI sends, which must be kind's.
func (p *fakePlayer) next(kind byte) []byte {
	p.t.Helper()
	p.c.SetReadDeadline(time.Now().Add(hang))
	var h [5]byte
	if _, err := io.ReadFull(p.c, h[:]); err != nil {
		p.t.Fatal(err)
	}
	b := make([]byte, binary.LittleEndian.Uint32(h[:4])-1)
	if _, err := io.ReadFull(p.c, b); err != nil {
		p.t.Fatal(err)
	}
	if h[4] != kind {
		p.t.Fatalf("the player got frame %d (%q), want %d", h[4], b, kind)
	}
	return b
}

// breakpoints reads a breakpoints frame: the file, its lines.
func (p *fakePlayer) breakpoints() (string, []int) {
	p.t.Helper()
	file, bps := p.breakpointsFull()
	lines := []int{}
	for _, b := range bps {
		lines = append(lines, b.Line)
	}
	return file, lines
}

// breakpointsFull reads a breakpoints frame: the file, its breakpoints.
func (p *fakePlayer) breakpointsFull() (string, []devserver.Breakpoint) {
	p.t.Helper()
	b := p.next(devserver.KindDebugBreakpoints)
	u32 := func() uint32 {
		v := binary.LittleEndian.Uint32(b)
		b = b[4:]
		return v
	}
	n := u32()
	file := string(b[:n])
	b = b[n:]
	var bps []devserver.Breakpoint
	for count := u32(); count > 0; count-- {
		bp := devserver.Breakpoint{Line: int(u32()), Key: u32()}
		flags, op := b[0], b[1]
		b = b[2:]
		bp.Cond, bp.Log, bp.HitOp, bp.HitN = flags&1 != 0, flags&2 != 0, devserver.HitOp(op), int(u32())
		for m := u32(); m > 0; m-- {
			c := devserver.BreakpointCode{Base: binary.LittleEndian.Uint64(b)}
			b = b[8:]
			c.Func, c.Cond, c.Log = u32(), u32(), u32()
			for k := u32(); k > 0; k-- {
				c.Params = append(c.Params, u32())
			}
			c.Error = string(b[4 : 4+binary.LittleEndian.Uint32(b)])
			b = b[4+len(c.Error):]
			bp.Code = append(bp.Code, c)
		}
		bps = append(bps, bp)
	}
	return file, bps
}

// client is an editor's side of the adapter.
type client struct {
	t      *testing.T
	c      net.Conn
	seq    int
	mu     sync.Mutex
	waits  map[int]chan map[string]any
	events chan map[string]any
}

func dialClient(t *testing.T, s *Server) *client {
	t.Helper()
	c, err := net.Dial("tcp", s.Addr())
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { c.Close() })
	cl := &client{t: t, c: c, waits: map[int]chan map[string]any{}, events: make(chan map[string]any, 256)}
	go func() {
		r := bufio.NewReader(c)
		for {
			b, err := readMessage(r)
			if err != nil {
				close(cl.events)
				return
			}
			var m map[string]any
			json.Unmarshal(b, &m)
			if m["type"] == "response" {
				cl.mu.Lock()
				ch := cl.waits[int(m["request_seq"].(float64))]
				cl.mu.Unlock()
				ch <- m
			} else {
				cl.events <- m
			}
		}
	}()
	return cl
}

// send sends a request, and returns where its response comes.
func (cl *client) send(command string, args any) chan map[string]any {
	cl.seq++
	ch := make(chan map[string]any, 1)
	cl.mu.Lock()
	cl.waits[cl.seq] = ch
	cl.mu.Unlock()
	if err := writeMessage(cl.c, map[string]any{"seq": cl.seq, "type": "request", "command": command, "arguments": args}); err != nil {
		cl.t.Fatal(err)
	}
	return ch
}

func (cl *client) wait(ch chan map[string]any) map[string]any {
	cl.t.Helper()
	select {
	case m := <-ch:
		return m
	case <-time.After(hang):
		cl.t.Fatal("no response")
	}
	return nil
}

// request sends a request and returns its response's body, which must
// have succeeded.
func (cl *client) request(command string, args any) map[string]any {
	cl.t.Helper()
	m := cl.wait(cl.send(command, args))
	if m["success"] != true {
		cl.t.Fatalf("%s: %v", command, m["message"])
	}
	body, _ := m["body"].(map[string]any)
	return body
}

// event waits for the next event named name (skipping others, output
// among them), and returns its body.
func (cl *client) event(name string) map[string]any {
	cl.t.Helper()
	for {
		select {
		case m, ok := <-cl.events:
			if !ok {
				cl.t.Fatalf("waiting for %s: the adapter hung up", name)
			}
			if m["event"] == name {
				body, _ := m["body"].(map[string]any)
				return body
			}
		case <-time.After(hang):
			cl.t.Fatalf("no %s event", name)
		}
	}
}

// js is v as JSON, for comparing what came as JSON.
func js(v any) string {
	b, _ := json.Marshal(v)
	return string(b)
}

// norm is JSON with its objects' keys sorted, as js writes what came.
func norm(s string) string {
	var v any
	if err := json.Unmarshal([]byte(s), &v); err != nil {
		panic(err)
	}
	return js(v)
}

// module is an app's module whose onPress has code on lines.
func module(lines ...uint32) *bytecode.Module {
	f := bytecode.FuncDef{Name: "Counter/onPress", File: "/app/app.asuka"}
	for i, l := range lines {
		f.Lines = append(f.Lines, bytecode.Line{PC: uint32(4 * i), Line: l})
	}
	return &bytecode.Module{Funcs: []bytecode.FuncDef{f}}
}

// testDev is the dev loop's side for the tests: its dev server's
// players get the breakpoints, their code "compiled" as the condition's
// and log message's lengths (a condition "bad(" doesn't compile).
type testDev struct {
	ds    *devserver.Server
	mu    sync.Mutex
	calls int // SetBreakpoints's
}

func (d *testDev) SetBreakpoints(file string, bps []Breakpoint) []string {
	d.mu.Lock()
	d.calls++
	d.mu.Unlock()
	errs := make([]string, len(bps))
	out := make([]devserver.Breakpoint, len(bps))
	for i, b := range bps {
		out[i] = devserver.Breakpoint{Line: b.Line, Key: b.Key, Cond: b.Condition != "", Log: b.LogMessage != "", HitOp: b.HitOp, HitN: b.HitN}
		if b.Condition == "" && b.LogMessage == "" {
			continue
		}
		c := devserver.BreakpointCode{Base: 1, Func: 0, Cond: devserver.NoFunc, Log: devserver.NoFunc, Params: []uint32{0}}
		if b.Condition != "" {
			c.Cond = uint32(len(b.Condition))
		}
		if b.LogMessage != "" {
			c.Log = uint32(len(b.LogMessage))
		}
		if b.Condition == "bad(" {
			c.Error, errs[i] = "The condition doesn't compile: bad(", "The condition doesn't compile: bad("
		}
		out[i].Code = []devserver.BreakpointCode{c}
	}
	d.ds.SetBreakpoints(target.MacOS, file, out)
	return errs
}

func (d *testDev) SetExceptions(filters byte) { d.ds.SetExceptions(filters) }
func (d *testDev) DebugReset()                { d.ds.DebugReset() }

// setup is asuka dev's side: a dev server whose events go to the
// adapter, which resolves breakpoints in *mod.
func setup(t *testing.T, mod **bytecode.Module, opts ...Options) (*devserver.Server, *Server) {
	var s *Server
	ds, err := devserver.Listen("127.0.0.1:0", devserver.Events{
		Hello:     func(p *devserver.Player, b []byte) { s.Hello(p, "macOS") },
		Gone:      func(p *devserver.Player, err error) { s.Gone(p) },
		Stopped:   func(p *devserver.Player, st devserver.Stop) { s.Stopped(p, st) },
		Continued: func(p *devserver.Player) { s.Continued(p) },
		Values:    func(p *devserver.Player, seq uint32, v devserver.Variables) { s.Values(p, seq, v) },
		Result:    func(p *devserver.Player, seq uint32, r devserver.Result) { s.Result(p, seq, r) },
		Logpoint:  func(p *devserver.Player, l devserver.Logpoint) { s.Logpoint(p, l) },
		Log:       func(p *devserver.Player, stderr bool, b []byte) { s.Output(p, stderr, string(b)) },
	})
	if err != nil {
		t.Fatal(err)
	}
	o := Options{}
	if len(opts) > 0 {
		o = opts[0]
	}
	o.Resolve = func(file string, lines []int) []int { return devserver.ResolveBreakpoints(*mod, file, lines) }
	o.Files = func() []string { return []string{"/app/app.asuka"} }
	o.Root = "/app"
	s = New(o)
	s.Dev = &testDev{ds: ds}
	if err := s.Listen("127.0.0.1:0"); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { s.Close(); ds.Close() })
	ds.Send(target.MacOS, []byte("v1"))
	return ds, s
}

// An editor attached to asuka dev: breakpoints resolved and sent to the
// players, a stop with its stack, scopes and variables (a value's
// children asked of the player by their path), a step, a reload's lines,
// a second player as a second thread, and detaching.
func TestAdapter(t *testing.T) {
	mod := module(10, 12, 14)
	ds, s := setup(t, &mod)
	p := dialPlayer(t, ds, `{"platform":"macos","device":"desktop"}`)
	p.next(devserver.KindModule)

	cl := dialClient(t, s)
	caps := cl.request("initialize", map[string]any{"adapterID": "asuka", "linesStartAt1": true, "columnsStartAt1": true, "pathFormat": "path"})
	if caps["supportsConfigurationDoneRequest"] != true || caps["supportsConditionalBreakpoints"] != true || caps["supportsEvaluateForHovers"] != false ||
		caps["supportsLogPoints"] != true || caps["supportsHitConditionalBreakpoints"] != true || caps["supportsExceptionInfoRequest"] != true {
		t.Fatal(caps)
	}
	if f := js(caps["exceptionBreakpointFilters"]); !strings.Contains(f, `"filter":"uncaught"`) || !strings.Contains(f, `"filter":"all"`) {
		t.Fatal(f)
	}
	cl.event("initialized")
	cl.request("attach", map[string]any{})
	if out := cl.event("output"); !strings.Contains(out["output"].(string), "macOS") {
		t.Fatal(out)
	}
	// 11 has no code: it moves to 12; 30 is past every function.
	body := cl.request("setBreakpoints", map[string]any{"source": map[string]any{"path": "/app/app.asuka"},
		"breakpoints": []any{map[string]any{"line": 11}, map[string]any{"line": 14}, map[string]any{"line": 30}}})
	bps := body["breakpoints"].([]any)
	if got := js(bps); got != norm(`[{"id":1,"verified":true,"line":12},{"id":2,"verified":true,"line":14},{"id":3,"verified":false,"message":"No code of the app's on this line (yet)","line":30,"reason":"pending"}]`) {
		t.Fatal(got)
	}
	if file, lines := p.breakpoints(); file != "/app/app.asuka" || !reflect.DeepEqual(lines, []int{12, 14}) {
		t.Fatal(file, lines)
	}
	cl.request("configurationDone", nil)
	if th := js(cl.request("threads", nil)["threads"]); th != norm(`[{"id":1,"name":"macOS"}]`) {
		t.Fatal(th)
	}

	// It stops at line 12.
	p.send(devserver.KindDebugStopped, []byte(`{"reason":"breakpoint","totalFrames":2,"frames":[`+
		`{"name":"Counter/onPress","file":"/app/app.asuka","line":12,"locals":[{"name":"n","type":"int","value":"3"},`+
		`{"name":"todo","type":"Todo","value":"Todo(title: \"Milk\", done: false)","children":2},`+
		`{"name":"items","type":"[int]","value":"[0, 1, 2, …]","children":150}]},`+
		`{"name":"run","file":"/deps/ui/run.asuka","line":7,"locals":[]}]}`))
	if ev := js(cl.event("stopped")); ev != norm(`{"allThreadsStopped":false,"hitBreakpointIds":[1],"reason":"breakpoint","threadId":1}`) {
		t.Fatal(ev)
	}
	st := cl.request("stackTrace", map[string]any{"threadId": 1})
	frames := st["stackFrames"].([]any)
	if len(frames) != 2 || st["totalFrames"] != 2.0 {
		t.Fatal(js(st))
	}
	f0, f1 := frames[0].(map[string]any), frames[1].(map[string]any)
	if js(f0["source"]) != norm(`{"name":"app.asuka","path":"/app/app.asuka"}`) || f0["line"] != 12.0 || f0["name"] != "Counter/onPress" {
		t.Fatal(js(f0))
	}
	if js(f1["source"]) != norm(`{"name":"run.asuka","path":"/deps/ui/run.asuka","presentationHint":"deemphasize","origin":"ui"}`) {
		t.Fatal(js(f1))
	}
	scopes := cl.request("scopes", map[string]any{"frameId": f0["id"]})["scopes"].([]any)
	locals := scopes[0].(map[string]any)
	if locals["name"] != "Locals" || locals["namedVariables"] != 3.0 {
		t.Fatal(js(scopes))
	}
	vars := cl.request("variables", map[string]any{"variablesReference": locals["variablesReference"]})["variables"].([]any)
	if len(vars) != 3 {
		t.Fatal(js(vars))
	}
	n, todo, items := vars[0].(map[string]any), vars[1].(map[string]any), vars[2].(map[string]any)
	if n["value"] != "3" || n["type"] != "int" || n["variablesReference"] != 0.0 {
		t.Fatal(js(n))
	}
	if todo["namedVariables"] != 2.0 || todo["variablesReference"] == 0.0 || items["indexedVariables"] != 150.0 {
		t.Fatal(js(vars))
	}
	// A value's children: the player's, by the path from the local.
	resp := cl.send("variables", map[string]any{"variablesReference": todo["variablesReference"]})
	b := p.next(devserver.KindDebugVariables)
	u32 := func(i int) uint32 { return binary.LittleEndian.Uint32(b[4*i:]) }
	if len(b) != 24 || u32(1) != 0 || u32(2) != 0 || u32(3) != 0 || u32(4) != 1 || u32(5) != 1 {
		t.Fatalf("% x", b)
	}
	p.send(devserver.KindDebugValues, append(binary.LittleEndian.AppendUint32(nil, u32(0)),
		`{"total":2,"variables":[{"name":"title","type":"string","value":"\"Milk\""},{"name":"done","type":"bool","value":"false"}]}`...))
	if got := js(cl.wait(resp)["body"]); got != norm(`{"variables":[{"name":"title","value":"\"Milk\"","type":"string","variablesReference":0},{"name":"done","value":"false","type":"bool","variablesReference":0}]}`) {
		t.Fatal(got)
	}
	// An array's elements, a page at a time; the named ones are none.
	if got := js(cl.request("variables", map[string]any{"variablesReference": items["variablesReference"], "filter": "named"})); got != norm(`{"variables":[]}`) {
		t.Fatal(got)
	}
	resp = cl.send("variables", map[string]any{"variablesReference": items["variablesReference"], "filter": "indexed", "start": 100, "count": 2})
	b = p.next(devserver.KindDebugVariables)
	if len(b) != 24 || u32(2) != 100 || u32(3) != 2 || u32(5) != 2 {
		t.Fatalf("% x", b)
	}
	p.send(devserver.KindDebugValues, append(binary.LittleEndian.AppendUint32(nil, u32(0)),
		`{"total":150,"variables":[{"name":"[100]","type":"int","value":"100"},{"name":"[101]","type":"int","value":"101"}]}`...))
	if v := cl.wait(resp)["body"].(map[string]any)["variables"].([]any); len(v) != 2 || v[1].(map[string]any)["name"] != "[101]" {
		t.Fatal(js(v))
	}
	// What the player can't answer fails the request.
	resp = cl.send("variables", map[string]any{"variablesReference": todo["variablesReference"]})
	b = p.next(devserver.KindDebugVariables)
	p.send(devserver.KindDebugValues, append(binary.LittleEndian.AppendUint32(nil, u32(0)), `{"error":"no such variable"}`...))
	if m := cl.wait(resp); m["success"] != false || m["message"] != "no such variable" {
		t.Fatal(js(m))
	}
	if m := cl.wait(cl.send("evaluate", map[string]any{"expression": "n + 1", "frameId": f0["id"]})); m["success"] != false {
		t.Fatal(js(m))
	}

	// A step: the command to the player; it goes on, and the references
	// with it.
	cl.request("next", map[string]any{"threadId": 1})
	if b := p.next(devserver.KindDebugCommand); string(b) != "\x02" {
		t.Fatalf("%q", b)
	}
	p.send(devserver.KindDebugContinued, nil)
	if ev := js(cl.event("continued")); ev != norm(`{"allThreadsContinued":false,"threadId":1}`) {
		t.Fatal(ev)
	}
	if m := cl.wait(cl.send("scopes", map[string]any{"frameId": f0["id"]})); m["success"] != false {
		t.Fatal(js(m))
	}
	for cmd, want := range map[string]string{"continue": "\x01", "stepIn": "\x03", "stepOut": "\x04", "pause": "\x05"} {
		cl.request(cmd, map[string]any{"threadId": 1})
		if b := p.next(devserver.KindDebugCommand); string(b) != want {
			t.Fatalf("%s: %q", cmd, b)
		}
	}

	// A save: line 12's code is on 13 now; 30 has code too. The editor
	// hears of both, the player gets the new lines.
	mod = module(10, 13, 14, 30)
	s.Compiled()
	moved := map[float64]float64{}
	for range 2 {
		bp := cl.event("breakpoint")["breakpoint"].(map[string]any)
		moved[bp["id"].(float64)] = bp["line"].(float64)
		if bp["verified"] != true {
			t.Fatal(js(bp))
		}
	}
	if !reflect.DeepEqual(moved, map[float64]float64{1: 13, 3: 30}) {
		t.Fatal(moved)
	}
	if _, lines := p.breakpoints(); !reflect.DeepEqual(lines, []int{13, 14, 30}) {
		t.Fatal(lines)
	}

	// A second player: a thread of its own, which gets the breakpoints
	// before its module.
	p2 := dialPlayer(t, ds, `{"platform":"macos"}`)
	if ev := js(cl.event("thread")); ev != norm(`{"reason":"started","threadId":2}`) {
		t.Fatal(ev)
	}
	// The console says so, after the attach's waiting for one.
	if out := cl.event("output"); out["category"] != "console" || out["output"] != "macOS connected.\n" {
		t.Fatal(out)
	}
	if _, lines := p2.breakpoints(); !reflect.DeepEqual(lines, []int{13, 14, 30}) {
		t.Fatal(lines)
	}
	p2.next(devserver.KindModule)
	p2.send(devserver.KindLog, append([]byte{1}, "hi\n"...))
	if out := cl.event("output"); out["category"] != "stdout" || out["output"] != "hi\n" {
		t.Fatal(out)
	}
	p2.c.Close()
	if ev := js(cl.event("thread")); ev != norm(`{"reason":"exited","threadId":2}`) {
		t.Fatal(ev)
	}

	// Detached: the players' breakpoints go, here too.
	cl.request("disconnect", map[string]any{})
	p.next(devserver.KindDebugReset)
	if bp := ds.Breakpoints(target.MacOS); len(bp) != 0 {
		t.Fatal(bp)
	}
}

// A breakpoint in a file the app doesn't use yet is pending, and set on
// the players once a compile has code there; an editor that goes without
// disconnecting detaches all the same, and the next one starts afresh.
func TestAdapterPendingAndHangUp(t *testing.T) {
	mod := &bytecode.Module{}
	ds, s := setup(t, &mod)
	p := dialPlayer(t, ds, `{"platform":"macos"}`)
	p.next(devserver.KindModule)
	cl := dialClient(t, s)
	cl.request("initialize", map[string]any{"linesStartAt1": false})
	// Lines from 0: the editor's 11 is line 12.
	bp := cl.request("setBreakpoints", map[string]any{"source": map[string]any{"path": "/app/app.asuka"}, "breakpoints": []any{map[string]any{"line": 11}}})
	if got := js(bp["breakpoints"]); got != norm(`[{"id":1,"verified":false,"message":"No code of the app's on this line (yet)","line":11,"reason":"pending"}]`) {
		t.Fatal(got)
	}
	mod = module(10, 12)
	s.Compiled()
	if got := js(cl.event("breakpoint")); got != norm(`{"breakpoint":{"id":1,"verified":true,"line":11},"reason":"changed"}`) {
		t.Fatal(got)
	}
	if _, lines := p.breakpoints(); !reflect.DeepEqual(lines, []int{12}) {
		t.Fatal(lines)
	}
	cl.c.Close()
	p.next(devserver.KindDebugReset)
	cl2 := dialClient(t, s)
	cl2.request("initialize", nil)
	if bp := ds.Breakpoints(target.MacOS); len(bp) != 0 {
		t.Fatal(bp)
	}
	// A stop before configurationDone is told once it's done.
	cl2.request("attach", nil)
	p.send(devserver.KindDebugStopped, []byte(`{"reason":"pause","totalFrames":1,"frames":[{"name":"main","file":"/app/app.asuka","line":12,"locals":[]}]}`))
	// A player's frames are read in order: once its log is heard, so is
	// its stop.
	p.send(devserver.KindLog, append([]byte{2}, "after\n"...))
	for cl2.event("output")["output"] != "after\n" {
	}
	cl2.request("configurationDone", nil)
	if ev := js(cl2.event("stopped")); ev != norm(`{"allThreadsStopped":false,"reason":"pause","threadId":1}`) {
		t.Fatal(ev)
	}
}

// Conditions, hit counts and log messages go to the players as their
// code, compiled again as the app compiles; errors stop the app where
// thrown, their details asked for; an expression evaluates in a paused
// frame, its value's children asked of the player as a local's are.
func TestAdapterCodeErrorsEvaluate(t *testing.T) {
	mod := module(10, 12, 14)
	var compiled []string
	var cmu sync.Mutex
	ds, s := setup(t, &mod, Options{Compile: func(p *devserver.Player, f devserver.Frame, expr string) (*devserver.Fragment, error) {
		cmu.Lock()
		compiled = append(compiled, f.Module+" "+itoa(int(f.Func))+" "+itoa(int(f.PC))+" "+expr)
		cmu.Unlock()
		if expr == "nope" {
			return nil, errors.New("unknown name nope")
		}
		return &devserver.Fragment{Base: 0xab, Ext: append([]byte("AKBX\x0a\x00\x00\x00"), binary.LittleEndian.AppendUint64(nil, 0xab)...), Func: 9, Params: []uint32{0}, Desc: "S./app.Todo;"}, nil
	}})
	dev := s.Dev.(*testDev)
	p := dialPlayer(t, ds, `{"platform":"macos"}`)
	p.next(devserver.KindModule)
	cl := dialClient(t, s)
	if caps := cl.request("initialize", map[string]any{"linesStartAt1": true}); caps["supportsEvaluateForHovers"] != true {
		t.Fatal(caps)
	}
	cl.request("attach", nil)

	body := cl.request("setBreakpoints", map[string]any{"source": map[string]any{"path": "/app/app.asuka"}, "breakpoints": []any{
		map[string]any{"line": 10, "condition": "n > 2", "hitCondition": ">= 3"},
		map[string]any{"line": 12, "logMessage": "n is {n}"},
		map[string]any{"line": 14, "condition": "bad("},
	}})
	if got := js(body["breakpoints"]); got != norm(`[{"id":1,"verified":true,"line":10},{"id":2,"verified":true,"line":12},{"id":3,"verified":true,"line":14,"message":"The condition doesn't compile: bad("}]`) {
		t.Fatal(got)
	}
	_, bps := p.breakpointsFull()
	if len(bps) != 3 || !bps[0].Cond || bps[0].HitOp != devserver.HitGE || bps[0].HitN != 3 || bps[0].Key == 0 || bps[0].Code[0].Cond != 5 ||
		!bps[1].Log || bps[1].Code[0].Log != 8 || bps[1].Code[0].Cond != devserver.NoFunc || bps[2].Code[0].Error == "" {
		t.Fatalf("%+v", bps)
	}
	// A hit condition that isn't one: said, and none sent.
	body = cl.request("setBreakpoints", map[string]any{"source": map[string]any{"path": "/app/app.asuka"}, "breakpoints": []any{
		map[string]any{"line": 10, "hitCondition": "often"}}})
	if bp := body["breakpoints"].([]any)[0].(map[string]any); !strings.HasPrefix(bp["message"].(string), "The hit count isn't a count") {
		t.Fatal(js(bp))
	}
	if _, bps := p.breakpointsFull(); len(bps) != 1 || bps[0].HitOp != devserver.HitAny {
		t.Fatalf("%+v", bps)
	}
	for hit, want := range map[string][2]int{"5": {int(devserver.HitEQ), 5}, "% 3": {int(devserver.HitMod), 3}, "> 2": {int(devserver.HitGT), 2}, "<=4": {int(devserver.HitLE), 4}} {
		if op, n, err := parseHit(hit); err != nil || int(op) != want[0] || n != want[1] {
			t.Fatalf("%q: %v %d %v", hit, op, n, err)
		}
	}
	// A save compiles a file's code again, its lines the same.
	cl.request("setBreakpoints", map[string]any{"source": map[string]any{"path": "/app/app.asuka"}, "breakpoints": []any{
		map[string]any{"line": 12, "condition": "n == 1"}}})
	p.breakpointsFull()
	dev.mu.Lock()
	before := dev.calls
	dev.mu.Unlock()
	s.Compiled()
	if _, bps := p.breakpointsFull(); len(bps) != 1 || !bps[0].Cond {
		t.Fatalf("%+v", bps)
	}
	if dev.calls != before+1 {
		t.Fatal(dev.calls, before)
	}

	// Errors: the filters to the player.
	ex := cl.request("setExceptionBreakpoints", map[string]any{"filters": []string{"uncaught"}})
	if js(ex["breakpoints"]) != norm(`[{"id":0,"verified":true}]`) {
		t.Fatal(js(ex))
	}
	if b := p.next(devserver.KindDebugExceptions); string(b) != "\x01" {
		t.Fatalf("%q", b)
	}
	cl.request("configurationDone", nil)
	p.send(devserver.KindDebugStopped, []byte(`{"reason":"exception","exception":{"type":"MailError","message":"offline","uncaught":true},"totalFrames":1,"frames":[`+
		`{"name":"send","file":"/app/app.asuka","line":12,"locals":[{"name":"n","type":"int","value":"3"}],"module":"00000000000000ab","func":4,"pc":20}]}`))
	if ev := js(cl.event("stopped")); ev != norm(`{"allThreadsStopped":false,"reason":"exception","threadId":1,"description":"Paused on an error nothing catches","text":"MailError: offline"}`) {
		t.Fatal(ev)
	}
	info := cl.request("exceptionInfo", map[string]any{"threadId": 1})
	if js(info) != norm(`{"breakMode":"unhandled","description":"offline","exceptionId":"MailError","details":{"message":"offline","typeName":"MailError","stackTrace":"send (/app/app.asuka:12)"}}`) {
		t.Fatal(js(info))
	}

	// Evaluating: compiled for the frame, sent with its extension; the
	// answer, then its children.
	f0 := cl.request("stackTrace", map[string]any{"threadId": 1})["stackFrames"].([]any)[0].(map[string]any)
	resp := cl.send("evaluate", map[string]any{"expression": "todo", "frameId": f0["id"], "context": "hover"})
	p.next(devserver.KindDebugExtend)
	b := p.next(devserver.KindDebugEvaluate)
	seq := binary.LittleEndian.Uint32(b)
	if binary.LittleEndian.Uint32(b[4:]) != 0 || binary.LittleEndian.Uint64(b[8:]) != 0xab || binary.LittleEndian.Uint32(b[16:]) != 9 {
		t.Fatalf("% x", b)
	}
	p.send(devserver.KindDebugResult, append(binary.LittleEndian.AppendUint32(nil, seq), `{"result":{"name":"","type":"Todo","value":"Todo(title: \"Milk\")","children":1},"ref":2}`...))
	ev := cl.wait(resp)["body"].(map[string]any)
	if ev["result"] != `Todo(title: "Milk")` || ev["type"] != "Todo" || ev["namedVariables"] != 1.0 || ev["variablesReference"] == 0.0 {
		t.Fatal(js(ev))
	}
	resp = cl.send("variables", map[string]any{"variablesReference": ev["variablesReference"]})
	b = p.next(devserver.KindDebugVariables)
	u32 := func(i int) uint32 { return binary.LittleEndian.Uint32(b[4*i:]) }
	if u32(1) != devserver.ResultFrame|2 || u32(4) != 1 || u32(5) != 0 {
		t.Fatalf("% x", b)
	}
	p.send(devserver.KindDebugValues, append(binary.LittleEndian.AppendUint32(nil, u32(0)), `{"total":1,"variables":[{"name":"title","type":"string","value":"\"Milk\""}]}`...))
	if got := js(cl.wait(resp)["body"]); got != norm(`{"variables":[{"name":"title","value":"\"Milk\"","type":"string","variablesReference":0}]}`) {
		t.Fatal(got)
	}
	// What doesn't compile, and what the player can't do, fail it.
	if m := cl.wait(cl.send("evaluate", map[string]any{"expression": "nope", "frameId": f0["id"]})); m["success"] != false || m["message"] != "unknown name nope" {
		t.Fatal(js(m))
	}
	resp = cl.send("evaluate", map[string]any{"expression": "1 / 0", "frameId": f0["id"]})
	b = p.next(devserver.KindDebugEvaluate) // the extension was sent already
	p.send(devserver.KindDebugResult, append(binary.LittleEndian.AppendUint32(nil, binary.LittleEndian.Uint32(b)), `{"error":"division by zero"}`...))
	if m := cl.wait(resp); m["success"] != false || m["message"] != "division by zero" {
		t.Fatal(js(m))
	}
	if m := cl.wait(cl.send("evaluate", map[string]any{"expression": "n", "context": "repl"})); m["success"] != false || !strings.HasPrefix(m["message"].(string), "Pause the app") {
		t.Fatal(js(m))
	}
	cmu.Lock()
	if compiled[0] != "00000000000000ab 4 20 todo" {
		t.Fatal(compiled)
	}
	cmu.Unlock()

	// A logpoint's line, at its source.
	p.send(devserver.KindDebugLog, []byte(`{"file":"/app/app.asuka","line":12,"text":"n is 3"}`))
	out := cl.event("output")
	if out["output"] != "n is 3\n" || out["line"] != 12.0 || out["source"].(map[string]any)["path"] != "/app/app.asuka" {
		t.Fatal(js(out))
	}
	p.send(devserver.KindDebugLog, []byte(`{"file":"/app/app.asuka","line":12,"error":"the breakpoint's condition: division by zero"}`))
	if out := cl.event("output"); out["category"] != "stderr" || !strings.Contains(out["output"].(string), "division by zero") {
		t.Fatal(js(out))
	}
}
