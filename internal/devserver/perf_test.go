package devserver

import (
	"encoding/json"
	"strings"
	"testing"
	"time"
)

// The overlay's frame goes to each player, and to one that says hello
// while it's on; a recording starts and stops by its frames, and the
// player's answer is Profile's.
func TestPerfFrames(t *testing.T) {
	profiles := make(chan string, 1)
	hello := make(chan *Player, 2)
	s, err := Listen("127.0.0.1:0", Events{
		Hello:   func(p *Player, b []byte) { hello <- p },
		Profile: func(p *Player, b []byte) { profiles <- string(b) },
	})
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	c := dial(t, s)
	defer c.Close()
	c.Write(frame(KindHello, []byte(`{"platform":"ios"}`)))
	p := <-hello
	for len(s.Ready()) == 0 { // its hello noted
		time.Sleep(time.Millisecond)
	}
	s.SetPerfOverlay(true)
	if k, b := read(c); k != KindPerf || b != "\x01" {
		t.Fatalf("%d %q", k, b)
	}
	if !p.PerfOverlay() || !s.PerfOverlay() {
		t.Fatal("not on")
	}
	// Another, while it's on: on as it says hello.
	c2 := dial(t, s)
	defer c2.Close()
	c2.Write(frame(KindHello, []byte(`{"platform":"android"}`)))
	if k, b := read(c2); k != KindPerf || b != "\x01" {
		t.Fatalf("%d %q", k, b)
	}
	if err := p.SetPerfOverlay(false); err != nil {
		t.Fatal(err)
	}
	if k, b := read(c); k != KindPerf || b != "\x00" {
		t.Fatalf("%d %q", k, b)
	}
	if err := p.StartProfile(true, 250*time.Microsecond); err != nil {
		t.Fatal(err)
	}
	if k, b := read(c); k != KindProfileStart || b != "\x01\xfa\x00\x00\x00" {
		t.Fatalf("%d %q", k, b)
	}
	if !p.Recording() {
		t.Fatal("not recording")
	}
	p.StopProfile()
	if k, b := read(c); k != KindProfileStop || b != "" {
		t.Fatalf("%d %q", k, b)
	}
	c.Write(frame(KindProfile, []byte(`{"version":1}`)))
	if got := <-profiles; got != `{"version":1}` {
		t.Fatal(got)
	}
	if ps := s.Ready(); len(ps) != 2 || ps[0] != p {
		t.Fatalf("%v", ps)
	}
}

// A recording as the core writes it (core/src/ui/perf.c): two flushes,
// a frame dropped, and samples in fib called from spin, and a gap.
const testProfile = `{"version":1,"duration":30000000,"truncated":false,
"phases":["app","props","layout","frames","after"],
"ops":["Create","Destroy","Insert","Remove","SetProp","SetFrame","Listen","Unlisten","Command","SetRoot"],
"flushes":[{"t":1000000,"cause":"event","event":"Press","d":[5000000,1000000,2000000,500000,0],"ops":[0,0,0,0,3,1,0,0,0,0],"effects":2,"measures":1,"hits":0,"allocs":7,"bytes":300},
{"t":20000000,"cause":"posted","d":[1000000,0,0,0,0],"ops":[0,0,0,0,1,0,0,0,0,0],"effects":1,"measures":0,"hits":0,"allocs":0,"bytes":0}],
"frames":[[16000000,0],[50000000,1]],
"cpu":{"interval":1000000,"truncated":false,
"funcs":[["spin","/app/app.asuka",11],["fib","/app/app.asuka",4],["(layout)","",0]],
"nodes":[[-1,-1],[0,0],[1,1],[2,1],[0,2]],
"samples":[[2000000,2,1000000],[3000000,3,1000000],[4000000,2,1000000],[8000000,4,1000000],[20000000,1,1000000]]}}`

func TestProfile(t *testing.T) {
	p, err := DecodeProfile([]byte(testProfile))
	if err != nil {
		t.Fatal(err)
	}
	if len(p.Flushes) != 2 || p.Flushes[0].Total() != 8500000 || p.Flushes[0].OpCount() != 4 || p.CPU == nil {
		t.Fatalf("%+v", p)
	}
	if st := p.CPU.Stack(3); len(st) != 3 || st[0] != 0 || st[1] != 1 || st[2] != 1 {
		t.Fatal(st)
	}
	// fib's own time: three samples; its total counts its recursion once.
	top := p.CPU.Top(0)
	if top[0].Name != "fib" || top[0].Self != 3000000 || top[0].Total != 3000000 {
		t.Fatalf("%+v", top)
	}
	if top[1].Name != "spin" || top[1].Self != 1000000 || top[1].Total != 4000000 {
		t.Fatalf("%+v", top)
	}

	b, err := p.ChromeTrace("iOS (iPhone 17)")
	if err != nil {
		t.Fatal(err)
	}
	var tr struct {
		TraceEvents []struct {
			Name string         `json:"name"`
			Ph   string         `json:"ph"`
			Ts   float64        `json:"ts"`
			Dur  float64        `json:"dur"`
			Tid  int            `json:"tid"`
			Args map[string]any `json:"args"`
		} `json:"traceEvents"`
	}
	if err := json.Unmarshal(b, &tr); err != nil {
		t.Fatal(err)
	}
	type slice struct {
		name    string
		tid     int
		ts, dur float64
	}
	var got []slice
	for _, e := range tr.TraceEvents {
		if e.Ph == "X" {
			got = append(got, slice{e.Name, e.Tid, e.Ts, e.Dur})
		}
	}
	want := []slice{
		{"Flush (Press)", tidFlushes, 1000, 8500},
		{"App code", tidFlushes, 1000, 5000},
		// spin, from the first sample's start to the last of the run's
		// end; fib in it, recursing; the gap after ends them.
		{"spin", tidCPU, 1000, 3000},
		{"fib", tidCPU, 1000, 3000},
		{"fib", tidCPU, 2000, 1000},
		{"Props batch", tidFlushes, 6000, 1000},
		{"(layout)", tidCPU, 7000, 1000},
		{"Layout", tidFlushes, 7000, 2000},
		{"Frames batch", tidFlushes, 9000, 500},
		{"Flush (a task, a timer)", tidFlushes, 20000, 1000},
		{"App code", tidFlushes, 20000, 1000},
		{"spin", tidCPU, 19000, 1000},
		{"1 frame dropped", tidDisplay, 16000, 34000},
	}
	if len(got) != len(want) {
		t.Fatalf("slices:\n%v\nwant\n%v", got, want)
	}
	for _, w := range want {
		found := false
		for _, g := range got {
			found = found || g == w
		}
		if !found {
			t.Errorf("no slice %v in\n%v", w, got)
		}
	}
	if !strings.Contains(string(b), `"name":"process_name","ph":"M","ts":0,"pid":1,"tid":0,"args":{"name":"iOS (iPhone 17)"}`) {
		t.Errorf("no process name: %s", b)
	}

	b, err = p.Speedscope("iOS")
	if err != nil {
		t.Fatal(err)
	}
	var ss struct {
		Shared struct {
			Frames []struct {
				Name string `json:"name"`
				File string `json:"file"`
				Line int    `json:"line"`
			} `json:"frames"`
		} `json:"shared"`
		Profiles []struct {
			Type    string  `json:"type"`
			Unit    string  `json:"unit"`
			Samples [][]int `json:"samples"`
			Weights []int64 `json:"weights"`
			Events  []struct {
				Type  string `json:"type"`
				Frame int    `json:"frame"`
				At    int64  `json:"at"`
			} `json:"events"`
		} `json:"profiles"`
	}
	if err := json.Unmarshal(b, &ss); err != nil {
		t.Fatal(err)
	}
	if len(ss.Profiles) != 2 || ss.Profiles[0].Type != "sampled" || ss.Profiles[1].Type != "evented" {
		t.Fatalf("%s", b)
	}
	sp := ss.Profiles[0]
	if len(sp.Samples) != 5 || len(sp.Samples[1]) != 3 || sp.Weights[0] != 1000000 || sp.Unit != "nanoseconds" {
		t.Fatalf("%s", b)
	}
	if f := ss.Shared.Frames[sp.Samples[1][2]]; f.Name != "fib" || f.File != "/app/app.asuka" || f.Line != 4 {
		t.Fatalf("%+v", f)
	}
	// The events open and close in order: the flush, its phases in it.
	ev := ss.Profiles[1].Events
	if len(ev) != 2+4*2+2+2 || ev[0].Type != "O" || ev[0].At != 1000000 || ev[len(ev)-1].Type != "C" || ev[len(ev)-1].At != 21000000 {
		t.Fatalf("%+v", ev)
	}
	if s := p.Summary(2); !strings.Contains(s, "2 flushes (9.5 ms in all, the slowest 8.5 ms: Flush (Press)), 1 frame dropped") ||
		!strings.Contains(s, "3.0 ms self,     3.0 ms total  fib (/app/app.asuka:4)") {
		t.Fatal(s)
	}
}

// A recording whose call tree names what isn't there is refused.
func TestProfileBad(t *testing.T) {
	for _, b := range []string{
		`{"version":2}`,
		`{"version":1,"cpu":{"funcs":[["a","",0]],"nodes":[[-1,-1],[5,0]],"samples":[]}}`,
		`{"version":1,"cpu":{"funcs":[["a","",0]],"nodes":[[-1,-1],[0,0]],"samples":[[0,9,1]]}}`,
	} {
		if _, err := DecodeProfile([]byte(b)); err == nil {
			t.Errorf("%s: no error", b)
		}
	}
}
