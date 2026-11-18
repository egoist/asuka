package linux_test

import (
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/egoist/asuka/internal/appbuild"
	"github.com/egoist/asuka/internal/devserver"
	"github.com/egoist/asuka/lang/compile"
	platforms "github.com/egoist/asuka/lang/target"
)

// boomApp is an app whose Boom button throws an error nothing catches.
const boomApp = `component Boom {
  state n = 0
  state late = false

  effect(late) { if late { present(Note()) } }

  Column {
    gap: 8; padding: 16
    Text("n: {n}")
    Button("Add") { on press { n += 1 } }
    Button("Boom") { on press { try explode(n) } }
    Button("Sheet") { on press { present(Note()); try explode(n) } }
    Button("Late sheet") { on press { late = true; try explode(n) } }
  }
}

component Note {
  Screen {
    title: "Note"
    Text("A sheet") { padding: 16 }
  }
}

fn explode(n: int) throws {
  throw Error("boom at {n}")
}

app { Boom() }
`

// boomSource is what asuka dev answers an uncaught error of boomApp's
// with: the lines where it was thrown.
const boomSource = `  12 | fn explode(n: int) throws {
> 13 |   throw Error("boom at {n}")
     |   ^^^^^^^^^^^^^^^^^^^^^^^^^^
  14 | }`

// devPlayer is asuka dev's player for Linux running an app's module from
// a dev server of the test's, driven through ASUKA_COMMANDS.
type devPlayer struct {
	t       *testing.T
	srv     *devserver.Server
	dir     string
	reloads chan bool // a load each: whether a restart's
}

// startDevPlayer builds the player, serves it src's module and waits
// for its first load.
func startDevPlayer(t *testing.T, src string, env ...string) *devPlayer {
	t.Helper()
	res := compile.Files(map[string][]byte{"app.asuka": []byte(src)}, &compile.Options{Platform: "linux"})
	if err := res.Err(); err != nil {
		t.Fatalf("the app doesn't compile:\n%v", err)
	}
	dir := t.TempDir()
	exe := filepath.Join(dir, "player", "asuka-host")
	if err := os.MkdirAll(filepath.Dir(exe), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := appbuild.LinuxPlayer(target(), res.Module.C, nil, exe); err != nil {
		t.Fatal(err)
	}
	p := &devPlayer{t: t, dir: dir, reloads: make(chan bool, 16)}
	srv, err := devserver.Listen("127.0.0.1:0", devserver.Events{
		Reloaded: func(_ *devserver.Player, _, _ uint32, restarted bool) { p.reloads <- restarted },
		Uncaught: func(pl *devserver.Player, u devserver.Uncaught) { pl.Source(u.Seq, boomSource, u.Trace) },
	})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { srv.Close() })
	p.srv = srv
	srv.Keep(platforms.Linux, res.Module.Encode())
	cmd := exec.Command(exe)
	cmd.Dir = dir
	cmd.Env = append(runEnv(t, dir), "ASUKA_DEV=127.0.0.1:"+strconv.Itoa(srv.Port()), "ASUKA_COMMANDS="+dir, "ASUKA_DUMP=1")
	cmd.Env = append(cmd.Env, env...)
	var out strings.Builder
	cmd.Stdout, cmd.Stderr = &out, &out
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		cmd.Process.Kill()
		cmd.Wait()
		if t.Failed() {
			t.Logf("the player's output:\n%s", out.String())
		}
	})
	if restarted := p.nextLoad(); restarted {
		t.Fatal("the first load was a restart's")
	}
	return p
}

// nextLoad waits for the player's report of a load: a hang guard, not a
// timing assertion.
func (p *devPlayer) nextLoad() bool {
	p.t.Helper()
	select {
	case r := <-p.reloads:
		return r
	case <-time.After(90 * time.Second):
		p.t.Fatal("the player reported no load")
	}
	return false
}

// run has the player run commands (a line each), then dump what its
// window shows, and returns that once it has (a line each).
func (p *devPlayer) run(commands string) string {
	p.t.Helper()
	file, dump := filepath.Join(p.dir, "asuka-commands.txt"), filepath.Join(p.dir, "asuka-dump.txt")
	os.Remove(dump)
	tmp := file + ".tmp"
	if err := os.WriteFile(tmp, []byte(commands+"\ndump\n"), 0o644); err != nil {
		p.t.Fatal(err)
	}
	if err := os.Rename(tmp, file); err != nil {
		p.t.Fatal(err)
	}
	for deadline := time.Now().Add(90 * time.Second); time.Now().Before(deadline); time.Sleep(20 * time.Millisecond) {
		if b, err := os.ReadFile(dump); err == nil {
			return string(b)
		}
	}
	p.t.Fatalf("the player didn't run %q", commands)
	return ""
}

// dump is what the player's window shows, a line each.
func (p *devPlayer) dump() string { return p.run("") }

// until polls the dump until ok says it's what's wanted (a hang guard),
// and returns it.
func (p *devPlayer) until(what string, ok func(dump string) bool) string {
	p.t.Helper()
	var got string
	for i := 0; i < 200; i++ {
		if got = p.dump(); ok(got) {
			return got
		}
		time.Sleep(50 * time.Millisecond)
	}
	p.t.Fatalf("waiting for %s, the player shows:\n%s", what, got)
	return ""
}

// onTop checks that the window's view tree (ASUKA_DUMP's, beside a
// snapshot) has the overlay for an app's error, and no dialog of the
// app's over it ("covered").
func (p *devPlayer) onTop(where string) {
	p.t.Helper()
	snap := filepath.Join(p.dir, "overlay.png")
	p.run("snap " + snap)
	b, err := os.ReadFile(snap + ".txt")
	if err != nil || !strings.Contains(string(b), `DevError reload "Uncaught error"`) {
		p.t.Fatalf("%s: the overlay isn't on top (%v):\n%s", where, err, b)
	}
}

func hasAll(want ...string) func(string) bool {
	return func(d string) bool {
		for _, w := range want {
			if !strings.Contains(d, w) {
				return false
			}
		}
		return true
	}
}

// asuka dev's error overlay: an error the app didn't catch shows over
// the window, with the lines where it was thrown and its stack; Dismiss
// (and Escape) go back to the app as it was; a build that fails shows
// until it builds again, with no Reload; Reload starts the app again.
func TestDevErrorOverlay(t *testing.T) {
	onLinux(t)
	p := startDevPlayer(t, boomApp)
	p.run("tap Add\ntap Boom")
	p.until("the overlay", hasAll("Uncaught error", "boom at 1", "> 13 |   throw Error", "explode (app.asuka:", "Dismiss", "Reload"))
	p.onTop("the overlay")
	p.run("tap Dismiss")
	p.until("Dismiss", func(d string) bool { return !strings.Contains(d, "Uncaught error") && strings.Contains(d, "n: 1") })

	// Escape dismisses it too.
	p.run("tap Boom")
	p.until("the overlay again", hasAll("Uncaught error", "boom at 1"))
	p.run("escape")
	p.until("Escape", func(d string) bool { return !strings.Contains(d, "Uncaught error") })

	// A build that failed: over the app until it builds again, no Reload.
	p.srv.Fail(platforms.Linux, devserver.Failure{Message: "app.asuka:7:32: += doesn't take an int and a string", Source: "> 7 |     Button(\"Add\") { on press { n += \"x\" } }\n    |                                ^^"})
	d := p.until("the build's failure", hasAll("Build failed", "doesn't take an int and a string", "^^"))
	if strings.Contains(d, "Reload") {
		t.Errorf("a build's failure offers Reload:\n%s", d)
	}
	p.srv.Fixed(platforms.Linux)
	p.until("the fix", func(d string) bool { return !strings.Contains(d, "Build failed") && strings.Contains(d, "n: 1") })

	// Reload: the app starts again, from its initial state.
	p.run("tap Boom")
	p.until("the overlay again", hasAll("Uncaught error", "Reload"))
	p.run("tap Reload")
	if restarted := p.nextLoad(); !restarted {
		t.Fatal("Reload didn't restart the app")
	}
	p.until("the reload", func(d string) bool { return !strings.Contains(d, "Uncaught error") && strings.Contains(d, "n: 0") })

	// An error as the app presents a sheet: the overlay over the sheet.
	p.run("tap Sheet")
	p.until("the overlay over the sheet", hasAll("Uncaught error", "boom at 0", "A sheet"))
	p.onTop("over the sheet")
	p.run("tap Dismiss")
	p.until("Dismiss over the sheet", func(d string) bool { return !strings.Contains(d, "Uncaught error") && strings.Contains(d, "A sheet") })
	p.run("escape")
	p.until("the sheet's dismissal", func(d string) bool { return !strings.Contains(d, "A sheet") })

	// A sheet the app presents over the overlay: the overlay over it again.
	p.run("tap Late sheet")
	p.until("the overlay over the later sheet", hasAll("Uncaught error", "A sheet"))
	p.onTop("over the later sheet")
}
