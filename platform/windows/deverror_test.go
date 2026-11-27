package windows_test

import (
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"testing"

	"github.com/egoist/asuka/internal/appbuild"
	"github.com/egoist/asuka/internal/devserver"
	"github.com/egoist/asuka/lang/compile"
	langtarget "github.com/egoist/asuka/lang/target"
)

// boomApp is an app whose Boom button throws an error nothing catches.
const boomApp = `component Boom {
  state n = 0

  Column {
    gap: 8; padding: 16
    Text("n: {n}")
    Button("Add") { on press { n += 1 } }
    Button("Boom") { on press { try explode(n) } }
  }
}

fn explode(n: int) throws {
  throw Error("boom at {n}")
}

app { Boom() }
`

// codeFrame is the lines of src around where trace's first frame ("name
// (file:line)") is, as asuka dev's overlay shows them.
func codeFrame(src, trace string) string {
	m := regexp.MustCompile(`:(\d+)\)`).FindStringSubmatch(trace)
	if m == nil {
		return ""
	}
	at, _ := strconv.Atoi(m[1])
	lines := strings.Split(src, "\n")
	var b strings.Builder
	for i := at - 1; i <= at+1; i++ {
		if i < 1 || i > len(lines) {
			continue
		}
		mark := " "
		if i == at {
			mark = ">"
		}
		fmt.Fprintf(&b, "%s %2d | %s\n", mark, i, lines[i-1])
		if i == at {
			l := lines[i-1]
			lead := len(l) - len(strings.TrimLeft(l, " "))
			fmt.Fprintf(&b, "     | %s%s\n", strings.Repeat(" ", lead), strings.Repeat("^", len(strings.TrimSpace(l))))
		}
	}
	return strings.TrimRight(b.String(), "\n")
}

// buildFailure is a build that failed: a line of its source wider than
// the overlay's box, and more errors than the overlay has room for.
var buildFailure = func() devserver.Failure {
	var more []string
	for i := 1; i <= 40; i++ {
		more = append(more, fmt.Sprintf("app.asuka:%d:3: error %d", 20+i, i))
	}
	return devserver.Failure{
		Message: `app.asuka:7:32: += doesn't take an int and a string`,
		Source: "   6 |     Text(\"n: {n}\")\n>  7 |     Button(\"Add\") { on press { n += \"x\" } }" + strings.Repeat(" // and on", 12) +
			"\n     |                                ^^^^^^^^",
		Details: strings.Join(more, "\n"),
	}
}()

// devPlayer runs asuka dev's player (the host without an app) built at
// exe (a guest path) connected to a dev server of the test's, which
// serves it src, with ASUKA_SCRIPT's steps: waitfor:loaded waits for its
// first load, waitfor:sourceN for the source of the Nth error it didn't
// catch to have gone back, waitfor:restarted for a restart's load, after
// which the server says the build failed (waitfor:failed). It returns the
// dumps by snap's name, and "end"'s.
func devPlayer(t *testing.T, w *windows, exe, src string, env map[string]string, steps ...string) map[string]string {
	t.Helper()
	return devPlayerWith(t, w, exe, src, env, nil, steps...)
}

// devHooks are what a test does as the player goes: Loaded, once it
// first loaded the app, with dir, the job's directory (where its
// snapshots go), and flag, which writes a file of the job's that
// waitfor:<name> waits for. Direction is the app's, as asuka dev's --dir
// builds it ("rtl"). Dir is the job's directory, after the run.
type devHooks struct {
	Loaded    func(p *devserver.Player, dir string, flag func(name string))
	Direction string
	// Preview is the preview to build in the app's place (asuka dev's
	// preview), none for the app.
	Preview *compile.PreviewRef
	Dir     string
}

func devPlayerWith(t *testing.T, w *windows, exe, src string, env map[string]string, hooks *devHooks, steps ...string) map[string]string {
	t.Helper()
	if hooks == nil {
		hooks = &devHooks{}
	}
	res := compile.Files(map[string][]byte{"app.asuka": []byte(src)}, &compile.Options{Platform: "windows", Direction: hooks.Direction, Preview: hooks.Preview, Debug: hooks.Preview != nil, NoTests: hooks.Preview != nil})
	if err := res.Err(); err != nil {
		t.Fatalf("the app doesn't compile:\n%v", err)
	}
	local, guest := w.job(t)
	flag := func(name string) { os.WriteFile(filepath.Join(local, name), []byte("1"), 0o644) }
	hooks.Dir = local
	var mu sync.Mutex
	var log strings.Builder
	var reloads, uncaught int
	var srv *devserver.Server
	srv, err := devserver.Listen(":0", devserver.Events{
		Log: func(p *devserver.Player, stderr bool, text []byte) {
			mu.Lock()
			log.Write(text)
			mu.Unlock()
		},
		Reloaded: func(p *devserver.Player, kept, micros uint32, restarted bool) {
			mu.Lock()
			reloads++
			first := reloads == 1
			mu.Unlock()
			if first {
				if hooks.Loaded != nil {
					hooks.Loaded(p, local, flag)
				}
				flag("loaded")
			}
			if restarted {
				flag("restarted")
				go func() {
					srv.Fail(langtarget.Windows, buildFailure)
					flag("failed")
				}()
			}
		},
		Uncaught: func(p *devserver.Player, u devserver.Uncaught) {
			p.Source(u.Seq, codeFrame(src, u.Trace), u.Trace)
			mu.Lock()
			uncaught++
			n := uncaught
			mu.Unlock()
			flag("source" + strconv.Itoa(n))
		},
	})
	if err != nil {
		t.Fatal(err)
	}
	defer srv.Close()
	srv.Send(langtarget.Windows, appbuild.StripTests(res.Module).Encode())

	var s, snaps []string
	for _, st := range steps {
		if name, ok := strings.CutPrefix(st, "snap:"); ok {
			snaps = append(snaps, name)
			st = "snap:" + guest + `\` + name + ".png"
		} else if name, ok := strings.CutPrefix(st, "waitfor:"); ok {
			st = "waitfor:" + guest + `\` + name
		}
		s = append(s, st)
	}
	e := map[string]string{
		"ASUKA_DEV":      hostAddress(t, w) + ":" + strconv.Itoa(srv.Port()),
		"ASUKA_SCRIPT":   strings.Join(s, ";"),
		"ASUKA_DUMP":     "1",
		"ASUKA_SNAPSHOT": guest + `\end.png`,
		"ASUKA_DATA":     guest + `\data`,
	}
	for k, v := range env {
		e[k] = v
	}
	out, err := w.run(t, guest, exe, e)
	mu.Lock()
	played := log.String()
	mu.Unlock()
	if err != nil {
		t.Fatalf("%v\n%s\n%s", err, out, played)
	}
	if played != "" {
		t.Logf("the player's log:\n%s", played)
	}
	if strings.Contains(out, "automation: nothing") || strings.Contains(out, "automation: no ") {
		t.Errorf("the script missed a step:\n%s", out)
	}
	dumps := map[string]string{}
	for _, name := range append(snaps, "end") {
		b, err := os.ReadFile(filepath.Join(local, name+".png.txt"))
		if err != nil {
			t.Fatalf("no dump %s: %v\n%s\n%s", name, err, out, played)
		}
		dumps[name] = string(b)
	}
	return dumps
}

// asuka dev's error overlay (DevError): an error the app didn't catch
// shows over the main window, with the lines where it was thrown and its
// stack; Dismiss takes it away, the app as it was; Reload starts the app
// again; a build that failed shows too, and Escape dismisses it. Then the
// overlay in dark mode.
func TestDevErrorOverlay(t *testing.T) {
	w := onWindows(t)
	zig(t)
	local, guest := w.job(t)
	if err := appbuild.WindowsHost(target(w), appbuild.WindowsOptions{Console: true}, filepath.Join(local, "player.exe")); err != nil {
		t.Fatal(err)
	}
	player := guest + `\player.exe`

	d := devPlayer(t, w, player, boomApp, nil,
		"waitfor:loaded", "tap:Add", "tap:Boom", "waitfor:source1", "wait:700ms", "snap:boom",
		"tap:Dismiss", "wait:300ms", "snap:dismissed",
		"tap:Boom", "waitfor:source2", "wait:700ms", "tap:Reload", "waitfor:restarted", "waitfor:failed", "wait:700ms", "snap:failed",
		"resize:600,420", "wait:300ms", "snap:resized",
		"key:escape", "wait:300ms", "snap:escaped")
	shows(t, d["boom"], "an uncaught error", []string{"Uncaught error", "boom at 1", `> 13 |   throw Error("boom at {n}")`, "explode (app.asuka:13)", "Reload", "Dismiss"})
	shows(t, d["dismissed"], "after Dismiss", []string{"n: 1"}, "Uncaught error", "boom at 1")
	shows(t, d["failed"], "a build that failed", []string{"Build failed", "app.asuka:7:32: += doesn't take an int and a string", "     |                                ^^^^^^^^",
		"app.asuka:60:3: error 40", "n: 0", "Dismiss"}, "Reload", "Uncaught error")
	// Over the main window's client area, following it.
	for name, want := range map[string]string{"boom": "dev error [0,0 800x600]:", "failed": "dev error [0,0 800x600]:", "resized": "dev error [0,0 600x420]:"} {
		if !strings.Contains(d[name], want) {
			t.Errorf("%s: no %q:\n%s", name, want, d[name])
		}
	}
	shows(t, d["escaped"], "after Escape", []string{"n: 0"}, "Build failed")

	// In dark mode.
	d = devPlayer(t, w, player, boomApp, map[string]string{"ASUKA_COLOR_SCHEME": "dark"},
		"waitfor:loaded", "tap:Boom", "waitfor:source1", "wait:700ms")
	shows(t, d["end"], "dark", []string{"Uncaught error", "boom at 0", "Reload", "Dismiss"})
}
