package main

import (
	"bufio"
	"encoding/json"
	"fmt"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/egoist/asuka/internal/appbuild"
	"github.com/egoist/asuka/internal/cbuild"
	"github.com/egoist/asuka/lang/target"
)

// dapClient is an editor's debugger, speaking the Debug Adapter Protocol
// to asuka dev's adapter.
type dapClient struct {
	t      *testing.T
	c      net.Conn
	r      *bufio.Reader
	seq    int
	events []map[string]any // read while waiting for a response
}

func dialDAP(t *testing.T, addr string) *dapClient {
	t.Helper()
	c, err := net.Dial("tcp", addr)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { c.Close() })
	return &dapClient{t: t, c: c, r: bufio.NewReader(c)}
}

func (d *dapClient) read() map[string]any {
	d.t.Helper()
	// A hang guard: the player stops as soon as its script taps.
	d.c.SetReadDeadline(time.Now().Add(90 * time.Second))
	n := -1
	for {
		line, err := d.r.ReadString('\n')
		if err != nil {
			d.t.Fatal(err)
		}
		line = strings.TrimSpace(line)
		if line == "" && n >= 0 {
			break
		}
		if v, ok := strings.CutPrefix(line, "Content-Length:"); ok {
			n, _ = strconv.Atoi(strings.TrimSpace(v))
		}
	}
	b := make([]byte, n)
	if _, err := ioReadFull(d.r, b); err != nil {
		d.t.Fatal(err)
	}
	var m map[string]any
	json.Unmarshal(b, &m)
	return m
}

func ioReadFull(r *bufio.Reader, b []byte) (int, error) {
	n := 0
	for n < len(b) {
		k, err := r.Read(b[n:])
		n += k
		if err != nil {
			return n, err
		}
	}
	return n, nil
}

// request sends a request and returns its response's body; it must
// succeed.
func (d *dapClient) request(command string, args any) map[string]any {
	d.t.Helper()
	d.seq++
	b, _ := json.Marshal(map[string]any{"seq": d.seq, "type": "request", "command": command, "arguments": args})
	if _, err := fmt.Fprintf(d.c, "Content-Length: %d\r\n\r\n%s", len(b), b); err != nil {
		d.t.Fatal(err)
	}
	for {
		m := d.read()
		if m["type"] != "response" {
			d.events = append(d.events, m)
			continue
		}
		if m["request_seq"] != float64(d.seq) {
			continue
		}
		if m["success"] != true {
			d.t.Fatalf("%s: %v", command, m["message"])
		}
		body, _ := m["body"].(map[string]any)
		return body
	}
}

// event waits for an event named name, and returns its body.
func (d *dapClient) event(name string) map[string]any {
	d.t.Helper()
	for i, m := range d.events {
		if m["event"] == name {
			d.events = append(d.events[:i], d.events[i+1:]...)
			body, _ := m["body"].(map[string]any)
			return body
		}
	}
	for {
		m := d.read()
		if m["event"] == name {
			body, _ := m["body"].(map[string]any)
			return body
		}
	}
}

// locals are the top frame's locals of thread's stop, as "name: type =
// value" lines, with their children indented under them.
func (d *dapClient) locals(thread any) (frame map[string]any, lines []string) {
	d.t.Helper()
	st := d.request("stackTrace", map[string]any{"threadId": thread})
	frame = st["stackFrames"].([]any)[0].(map[string]any)
	scopes := d.request("scopes", map[string]any{"frameId": frame["id"]})["scopes"].([]any)
	var walk func(ref any, indent string)
	walk = func(ref any, indent string) {
		for _, v := range d.request("variables", map[string]any{"variablesReference": ref})["variables"].([]any) {
			v := v.(map[string]any)
			lines = append(lines, fmt.Sprintf("%s%s: %s = %s", indent, v["name"], v["type"], v["value"]))
			if v["variablesReference"] != 0.0 {
				walk(v["variablesReference"], indent+"  ")
			}
		}
	}
	walk(scopes[0].(map[string]any)["variablesReference"], "")
	return frame, lines
}

// asuka dev's debugger on this Mac, end to end: an editor attaches to
// the dev loop's adapter, sets a breakpoint in a handler of the app's
// source, the player (the macOS host) stops there as its script taps
// Add, with the handler's locals (the component's state); it steps,
// goes on, stops again on the next tap with the count one more, and
// detaching lets it run on.
func TestDevDebugger(t *testing.T) {
	needMacBuild(t)
	dir := filepath.Join(t.TempDir(), "counter")
	os.MkdirAll(dir, 0o755)
	os.WriteFile(filepath.Join(dir, "asuka.toml"), []byte("name = \"Counter\"\nid = \"dev.asuka.debugtest\"\n"), 0o644)
	src := filepath.Join(dir, "app.asuka")
	os.WriteFile(src, []byte(`component Counter {
  state count = 0

  Column {
    Text("Count: {count}")
    Button("Add") {
      on press {
        let next = count + 1
        count = next
      }
    }
  }
}

app { Counter() }
`), 0o644)
	// As asuka dev compiles: unoptimized, the locals named.
	oldDebug, oldNoOpt, oldPort := appbuild.Debug, appbuild.DevNoOptimize, devDebugPort
	appbuild.Debug, appbuild.DevNoOptimize = true, true
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	devDebugPort = ln.Addr().(*net.TCPAddr).Port
	ln.Close()
	t.Cleanup(func() { appbuild.Debug, appbuild.DevNoOptimize, devDebugPort = oldDebug, oldNoOpt, oldPort })

	app := projectApp(t, dir, nil)
	app.platform = target.MacOS
	loop, err := newDevLoop(app, "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer loop.close()
	if loop.debugger == nil {
		t.Fatal("no debugger")
	}
	exe, err := buildDesktopPlayer(app, platformMac, cbuild.Host(), t.TempDir(), false)
	if err != nil {
		t.Fatal(err)
	}
	marks := t.TempDir()
	tap1, tap2, end := filepath.Join(marks, "tap1"), filepath.Join(marks, "tap2"), filepath.Join(marks, "end")
	cmd := exec.Command(exe)
	cmd.Env = append(os.Environ(),
		"ASUKA_DEV=127.0.0.1:"+strconv.Itoa(loop.port()),
		"ASUKA_DATA="+filepath.Join(t.TempDir(), "data"),
		"ASUKA_SCRIPT=waitfor:"+tap1+";tap:Add;waitfor:"+tap2+";tap:Add;waitfor:"+end)
	var out strings.Builder
	cmd.Stdout, cmd.Stderr = &out, &out
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	defer cmd.Process.Kill()
	if r := nextReload(t, loop); !r.first {
		t.Fatalf("the first report: %+v", r)
	}

	d := dialDAP(t, loop.debugger.Addr())
	d.request("initialize", map[string]any{"adapterID": "asuka", "linesStartAt1": true, "pathFormat": "path"})
	d.request("attach", map[string]any{})
	// Line 7 (on press) has no code of its own: the breakpoint moves to
	// the handler's first line.
	bps := d.request("setBreakpoints", map[string]any{"source": map[string]any{"path": src}, "breakpoints": []any{map[string]any{"line": 7}}})
	if bp := bps["breakpoints"].([]any)[0].(map[string]any); bp["verified"] != true || bp["line"] != 8.0 {
		t.Fatalf("the breakpoint: %v", bp)
	}
	d.request("configurationDone", nil)
	threads := d.request("threads", nil)["threads"].([]any)
	if len(threads) != 1 || threads[0].(map[string]any)["name"] != "macOS" {
		t.Fatalf("threads: %v", threads)
	}

	os.WriteFile(tap1, nil, 0o644)
	st := d.event("stopped")
	if st["reason"] != "breakpoint" {
		t.Fatalf("stopped: %v", st)
	}
	frame, locals := d.locals(st["threadId"])
	if frame["line"] != 8.0 || frame["source"].(map[string]any)["path"] != src {
		t.Fatalf("the frame: %v", frame)
	}
	if got := strings.Join(locals, "\n"); !strings.Contains(got, "count: int = 0") {
		t.Fatalf("locals at the breakpoint:\n%s", got)
	}
	// Over the let: next has its value.
	d.request("next", map[string]any{"threadId": st["threadId"]})
	st = d.event("stopped")
	frame, locals = d.locals(st["threadId"])
	if st["reason"] != "step" || frame["line"] != 9.0 || !strings.Contains(strings.Join(locals, "\n"), "next: int = 1") {
		t.Fatalf("after a step, at line %v: %v\n%s", frame["line"], st, strings.Join(locals, "\n"))
	}
	d.request("continue", map[string]any{"threadId": st["threadId"]})
	d.event("continued")

	os.WriteFile(tap2, nil, 0o644)
	st = d.event("stopped")
	if _, locals = d.locals(st["threadId"]); !strings.Contains(strings.Join(locals, "\n"), "count: int = 1") {
		t.Fatalf("the second stop:\n%s", strings.Join(locals, "\n"))
	}
	// Detached: the app goes on, and its count is 2.
	d.request("disconnect", map[string]any{})
	inspects(t, loop, "macOS", `"text":"Count: 2"`)
	os.WriteFile(end, nil, 0o644)
}
