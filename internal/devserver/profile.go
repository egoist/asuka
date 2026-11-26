package devserver

// A recording a player made (KindProfile, core/src/ui/perf.c's JSON), and
// what tools read of it: a trace in Chrome's trace event format, which
// Perfetto (ui.perfetto.dev) and chrome://tracing open, and a speedscope
// profile (speedscope.app). Times in the recording are nanoseconds from
// its start.

import (
	"encoding/json"
	"errors"
	"fmt"
	"sort"
	"strings"
)

// Profile is a player's recording.
type Profile struct {
	Version  int   `json:"version"`
	Duration int64 `json:"duration"` // ns
	// Truncated: it went on past what a player keeps (flushes, frames or
	// samples): the rest isn't in it.
	Truncated bool `json:"truncated"`
	// Phases name a flush's phases, in order: app (the app's code: its
	// handlers, effects and tasks), props (the props batch's crossing into
	// the host), layout (with the host's measures), frames (the frames
	// batch's crossing), after (layout handlers).
	Phases []string `json:"phases"`
	// Ops name the op codes a flush counts, from 1.
	Ops     []string `json:"ops"`
	Flushes []Flush  `json:"flushes"`
	// Frames are the display's frames: [t, dropped] each, dropped the
	// frames the display missed before it.
	Frames [][2]int64 `json:"frames"`
	// CPU is the app's functions sampled (nil: not asked for, or not on
	// this platform: the web's).
	CPU *CPU `json:"cpu"`
}

// Flush is a flush's record.
type Flush struct {
	T     int64  `json:"t"`               // its start: the call of the host's it ran in, or the last flush's end
	Cause string `json:"cause"`           // event, posted, list, load, other
	Event string `json:"event,omitempty"` // an event's kind ("Press")
	// D is each phase's time (Profile.Phases), ns.
	D []int64 `json:"d"`
	// Ops are the ops it sent, by code (Profile.Ops).
	Ops      []uint32 `json:"ops"`
	Effects  uint32   `json:"effects"`
	Measures uint32   `json:"measures"` // the host's
	Hits     uint32   `json:"hits"`     // measures the cache answered
	Allocs   uint64   `json:"allocs"`
	Bytes    uint64   `json:"bytes"`
}

// Total is the flush's time, ns.
func (f Flush) Total() int64 {
	var t int64
	for _, d := range f.D {
		t += d
	}
	return t
}

// OpCount is how many ops it sent.
func (f Flush) OpCount() uint32 {
	var n uint32
	for _, o := range f.Ops {
		n += o
	}
	return n
}

// CPU is the app's functions sampled on the main thread.
type CPU struct {
	Interval  int64 `json:"interval"` // ns
	Truncated bool  `json:"truncated"`
	Funcs     []Func
	// Nodes are the call tree: Nodes[i] is [parent, func], node 0 the
	// root ([-1, -1]).
	Nodes [][2]int32 `json:"nodes"`
	// Samples are [t, node, weight] each: when it was taken, its stack's
	// node, and the time it stands for, ns.
	Samples [][3]int64 `json:"samples"`
}

// Func is a function samples were in: the app's (its file and the line
// its code starts at), or what the main thread did outside the app's
// code, in parentheses ("(layout)", "(runtime)"), with no file.
type Func struct {
	Name string
	File string
	Line int
}

func (c *CPU) UnmarshalJSON(b []byte) error {
	var raw struct {
		Interval  int64      `json:"interval"`
		Truncated bool       `json:"truncated"`
		Funcs     [][3]any   `json:"funcs"`
		Nodes     [][2]int32 `json:"nodes"`
		Samples   [][3]int64 `json:"samples"`
	}
	if err := json.Unmarshal(b, &raw); err != nil {
		return err
	}
	c.Interval, c.Truncated, c.Nodes, c.Samples = raw.Interval, raw.Truncated, raw.Nodes, raw.Samples
	c.Funcs = make([]Func, len(raw.Funcs))
	for i, f := range raw.Funcs {
		name, _ := f[0].(string)
		file, _ := f[1].(string)
		line, _ := f[2].(float64)
		c.Funcs[i] = Func{name, file, int(line)}
	}
	for i, n := range c.Nodes {
		if i > 0 && (n[0] < 0 || int(n[0]) >= i || n[1] < 0 || int(n[1]) >= len(c.Funcs)) {
			return fmt.Errorf("devserver: profile: node %d is %v", i, n)
		}
	}
	for _, s := range c.Samples {
		if s[1] < 0 || s[1] >= int64(len(c.Nodes)) {
			return fmt.Errorf("devserver: profile: a sample's node %d", s[1])
		}
	}
	return nil
}

// DecodeProfile reads a KindProfile's payload.
func DecodeProfile(b []byte) (*Profile, error) {
	var p Profile
	if err := json.Unmarshal(b, &p); err != nil {
		return nil, fmt.Errorf("devserver: profile: %v", err)
	}
	if p.Version != 1 {
		return nil, errors.New("devserver: profile: not version 1")
	}
	return &p, nil
}

// Stack is node's stack, outermost first: its functions' indexes.
func (c *CPU) Stack(node int32) []int32 {
	var s []int32
	for n := node; n > 0; n = c.Nodes[n][0] {
		s = append(s, c.Nodes[n][1])
	}
	for i, j := 0, len(s)-1; i < j; i, j = i+1, j-1 {
		s[i], s[j] = s[j], s[i]
	}
	return s
}

// FuncTime is a function's time in the samples: in it (Self), and in it
// or what it called (Total), ns.
type FuncTime struct {
	Func
	Self, Total int64
}

// Top are the functions by their own time, most first (n: at most that
// many; 0, all).
func (c *CPU) Top(n int) []FuncTime {
	self := make([]int64, len(c.Funcs))
	total := make([]int64, len(c.Funcs))
	seen := make([]int, len(c.Funcs))
	for i := range seen {
		seen[i] = -1
	}
	for si, s := range c.Samples {
		st := c.Stack(int32(s[1]))
		if len(st) == 0 {
			continue
		}
		self[st[len(st)-1]] += s[2]
		for _, f := range st {
			if seen[f] != si { // recursion counts once
				seen[f] = si
				total[f] += s[2]
			}
		}
	}
	var out []FuncTime
	for i, f := range c.Funcs {
		if total[i] > 0 {
			out = append(out, FuncTime{f, self[i], total[i]})
		}
	}
	sort.SliceStable(out, func(i, j int) bool {
		if out[i].Self != out[j].Self {
			return out[i].Self > out[j].Self
		}
		return out[i].Total > out[j].Total
	})
	if n > 0 && len(out) > n {
		out = out[:n]
	}
	return out
}

// The phases' names in a trace.
var phaseTitles = map[string]string{
	"app":    "App code",
	"props":  "Props batch",
	"layout": "Layout",
	"frames": "Frames batch",
	"after":  "Layout handlers",
}

func phaseTitle(p string) string {
	if t, ok := phaseTitles[p]; ok {
		return t
	}
	return p
}

// FlushName is how a trace names a flush: what started it.
func FlushName(f Flush) string {
	switch f.Cause {
	case "event":
		if f.Event != "" {
			return "Flush (" + f.Event + ")"
		}
	case "posted":
		return "Flush (a task, a timer)"
	case "list":
		return "Flush (a list's rows)"
	case "load":
		return "Flush (the app loaded)"
	}
	return "Flush"
}

type traceEvent struct {
	Name string         `json:"name"`
	Cat  string         `json:"cat,omitempty"`
	Ph   string         `json:"ph"`
	Ts   float64        `json:"ts"`
	Dur  *float64       `json:"dur,omitempty"`
	Pid  int            `json:"pid"`
	Tid  int            `json:"tid"`
	S    string         `json:"s,omitempty"`
	Args map[string]any `json:"args,omitempty"`
}

func us(ns int64) float64 { return float64(ns) / 1000 }

// The trace's threads.
const (
	tidFlushes = 1
	tidCPU     = 2
	tidDisplay = 3
)

// ChromeTrace is the recording in Chrome's trace event format (JSON
// object form), for Perfetto and chrome://tracing: a process named name,
// with a track of the flushes (each a slice, its phases nested in it,
// its work in its arguments), one of the app's functions as sampled
// (consecutive samples of the same calls merged into slices), and one of
// the display's dropped frames (a slice over each gap).
func (p *Profile) ChromeTrace(name string) ([]byte, error) {
	evs := []traceEvent{
		{Name: "process_name", Ph: "M", Pid: 1, Args: map[string]any{"name": name}},
		{Name: "thread_name", Ph: "M", Pid: 1, Tid: tidFlushes, Args: map[string]any{"name": "Main thread: flushes"}},
		{Name: "thread_sort_index", Ph: "M", Pid: 1, Tid: tidFlushes, Args: map[string]any{"sort_index": 0}},
		{Name: "thread_name", Ph: "M", Pid: 1, Tid: tidCPU, Args: map[string]any{"name": "Main thread: the app's functions (sampled)"}},
		{Name: "thread_sort_index", Ph: "M", Pid: 1, Tid: tidCPU, Args: map[string]any{"sort_index": 1}},
		{Name: "thread_name", Ph: "M", Pid: 1, Tid: tidDisplay, Args: map[string]any{"name": "Display: frames dropped"}},
		{Name: "thread_sort_index", Ph: "M", Pid: 1, Tid: tidDisplay, Args: map[string]any{"sort_index": 2}},
	}
	slice := func(tid int, name, cat string, start, dur int64, args map[string]any) {
		d := us(dur)
		evs = append(evs, traceEvent{Name: name, Cat: cat, Ph: "X", Ts: us(start), Dur: &d, Pid: 1, Tid: tid, Args: args})
	}
	for _, f := range p.Flushes {
		args := map[string]any{"effects": f.Effects, "ops": f.OpCount(), "measures": f.Measures, "measures cached": f.Hits, "allocations": f.Allocs, "bytes allocated": f.Bytes}
		for i, n := range f.Ops {
			if n > 0 && i < len(p.Ops) {
				args["ops: "+p.Ops[i]] = n
			}
		}
		slice(tidFlushes, FlushName(f), "flush", f.T, f.Total(), args)
		t := f.T
		for i, d := range f.D {
			if d > 0 && i < len(p.Phases) {
				slice(tidFlushes, phaseTitle(p.Phases[i]), "phase", t, d, nil)
			}
			t += d
		}
	}
	// The frames: a dropped frame's slice is the gap before it.
	var prev int64 = -1
	for _, fr := range p.Frames {
		if fr[1] > 0 && prev >= 0 {
			n := fr[1]
			slice(tidDisplay, fmt.Sprintf("%d %s dropped", n, plural(n, "frame", "frames")), "frames", prev, fr[0]-prev, map[string]any{"dropped": n})
		}
		prev = fr[0]
	}
	if p.CPU != nil {
		p.CPU.slices(func(fn int32, start, end int64) {
			f := p.CPU.Funcs[fn]
			var args map[string]any
			if f.File != "" {
				args = map[string]any{"file": f.File, "line": f.Line}
			}
			slice(tidCPU, f.Name, "cpu", start, end-start, args)
		})
	}
	// Slices that start together, the longest first: the outer one.
	sort.SliceStable(evs, func(i, j int) bool {
		a, b := evs[i], evs[j]
		if (a.Ph == "M") != (b.Ph == "M") {
			return a.Ph == "M"
		}
		if a.Ts != b.Ts {
			return a.Ts < b.Ts
		}
		if a.Dur != nil && b.Dur != nil {
			return *a.Dur > *b.Dur
		}
		return false
	})
	meta := map[string]any{"source": "asuka dev", "duration_ms": us(p.Duration) / 1000, "truncated": p.Truncated}
	return json.Marshal(map[string]any{"traceEvents": evs, "displayTimeUnit": "ms", "metadata": meta})
}

func plural(n int64, one, many string) string {
	if n == 1 {
		return one
	}
	return many
}

// slices calls emit with each function's run as the samples show it: a
// sample stands for the time before it (its weight); consecutive samples
// whose stacks share their outer calls share those calls' slices. A gap
// longer than two intervals (the main thread idle) ends them all.
func (c *CPU) slices(emit func(fn int32, start, end int64)) {
	type open struct {
		fn    int32
		start int64
	}
	var stack []open
	var last int64
	closeTo := func(k int, at int64) {
		for len(stack) > k {
			o := stack[len(stack)-1]
			stack = stack[:len(stack)-1]
			if at > o.start {
				emit(o.fn, o.start, at)
			}
		}
	}
	gap := 2 * c.Interval
	if gap <= 0 {
		gap = 2000000
	}
	samples := append([][3]int64(nil), c.Samples...)
	sort.SliceStable(samples, func(i, j int) bool { return samples[i][0] < samples[j][0] })
	for _, s := range samples {
		end, w := s[0], s[2]
		start := end - w
		if start < last {
			start = last
		}
		if len(stack) > 0 && start-last > gap {
			closeTo(0, last)
		}
		st := c.Stack(int32(s[1]))
		k := 0
		for k < len(stack) && k < len(st) && stack[k].fn == st[k] {
			k++
		}
		closeTo(k, start)
		for _, f := range st[k:] {
			stack = append(stack, open{f, start})
		}
		last = end
	}
	closeTo(0, last)
}

// Speedscope is the recording as a speedscope profile (its file format,
// speedscope.app): the app's functions as sampled (a "sampled" profile
// named name), and the flushes and their phases (an "evented" one).
func (p *Profile) Speedscope(name string) ([]byte, error) {
	type frame struct {
		Name string `json:"name"`
		File string `json:"file,omitempty"`
		Line int    `json:"line,omitempty"`
	}
	var frames []frame
	index := map[frame]int{}
	frameOf := func(f frame) int {
		if i, ok := index[f]; ok {
			return i
		}
		index[f] = len(frames)
		frames = append(frames, f)
		return len(frames) - 1
	}
	var profiles []any
	if p.CPU != nil && len(p.CPU.Samples) > 0 {
		samples := [][]int{}
		weights := []int64{}
		for _, s := range p.CPU.Samples {
			var st []int
			for _, fn := range p.CPU.Stack(int32(s[1])) {
				f := p.CPU.Funcs[fn]
				st = append(st, frameOf(frame{f.Name, f.File, f.Line}))
			}
			samples = append(samples, st)
			weights = append(weights, s[2])
		}
		profiles = append(profiles, map[string]any{
			"type": "sampled", "name": name + ": the app's functions", "unit": "nanoseconds",
			"startValue": 0, "endValue": p.Duration, "samples": samples, "weights": weights,
		})
	}
	type event struct {
		Type  string `json:"type"`
		Frame int    `json:"frame"`
		At    int64  `json:"at"`
	}
	var evs []event
	var end int64
	for _, f := range p.Flushes {
		if f.T < end {
			continue // speedscope's events nest: a flush overlapping the last isn't one
		}
		fl := frameOf(frame{Name: FlushName(f)})
		evs = append(evs, event{"O", fl, f.T})
		t := f.T
		for i, d := range f.D {
			if d > 0 && i < len(p.Phases) {
				ph := frameOf(frame{Name: phaseTitle(p.Phases[i])})
				evs = append(evs, event{"O", ph, t}, event{"C", ph, t + d})
			}
			t += d
		}
		evs = append(evs, event{"C", fl, t})
		end = t
	}
	if len(evs) > 0 {
		profiles = append(profiles, map[string]any{
			"type": "evented", "name": name + ": flushes", "unit": "nanoseconds",
			"startValue": 0, "endValue": max(p.Duration, end), "events": evs,
		})
	}
	if frames == nil {
		frames = []frame{}
	}
	if profiles == nil {
		profiles = []any{}
	}
	return json.Marshal(map[string]any{
		"$schema":  "https://www.speedscope.app/file-format-schema.json",
		"shared":   map[string]any{"frames": frames},
		"profiles": profiles,
		"name":     name,
		"exporter": "asuka dev",
	})
}

// Summary is a few lines about the recording: its flushes (how many, the
// slowest), frames dropped, and the functions that took the most time.
func (p *Profile) Summary(top int) string {
	var b strings.Builder
	var slowest Flush
	var total int64
	for _, f := range p.Flushes {
		total += f.Total()
		if f.Total() > slowest.Total() {
			slowest = f
		}
	}
	var dropped int64
	for _, fr := range p.Frames {
		dropped += fr[1]
	}
	fmt.Fprintf(&b, "%.1f s, %d %s", float64(p.Duration)/1e9, len(p.Flushes), plural(int64(len(p.Flushes)), "flush", "flushes"))
	if len(p.Flushes) > 0 {
		fmt.Fprintf(&b, " (%.1f ms in all, the slowest %.1f ms: %s)", float64(total)/1e6, float64(slowest.Total())/1e6, FlushName(slowest))
	}
	fmt.Fprintf(&b, ", %d %s dropped", dropped, plural(dropped, "frame", "frames"))
	if p.Truncated {
		b.WriteString(" (cut short)")
	}
	b.WriteString("\n")
	if p.CPU != nil {
		for _, f := range p.CPU.Top(top) {
			where := ""
			if f.File != "" {
				where = fmt.Sprintf(" (%s:%d)", f.File, f.Line)
			}
			fmt.Fprintf(&b, "  %7.1f ms self, %7.1f ms total  %s%s\n", float64(f.Self)/1e6, float64(f.Total)/1e6, f.Name, where)
		}
	}
	return b.String()
}
