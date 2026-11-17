//go:build darwin

package mactest

import (
	"encoding/json"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"testing"
	"time"
)

// Launch Services' files and links, together: the files reach on files at
// once, as paths, the link on openURL.
func TestShellOpenURLs(t *testing.T) {
	a := Run(t, `
		state files = ""
		state urls = ""
		app {
		  on files(paths) { files = files + "[" + paths.joined(",") + "]" }
		  on openURL(u) { urls = urls + "[" + u + "]" }
		  Text("files {files} urls {urls}")
		}
	`)
	a.Do("openurls /tmp/a b.md|/tmp/c.md|myapp://note/1")
	a.UntilText("files [/tmp/a b.md,/tmp/c.md] urls [myapp://note/1]")
}

// A quit asks the app's on quit once, which can cancel it, but not as the
// session ends; a quit the app allowed only finishes.
func TestShellQuitCanceled(t *testing.T) {
	a := Run(t, `
		state asked = 0
		state ending = false
		app {
		  on quit(e) {
		    asked += 1
		    ending = e.sessionEnding
		    e.cancel()
		  }
		  Text("asked {asked} ending {ending}")
		}
	`)
	a.UntilText("asked 0 ending false")
	a.Do("key cmd+q")
	a.UntilText("asked 1 ending false")
	time.Sleep(500 * time.Millisecond)
	if a.Exited() {
		t.Fatal("a canceled quit quit")
	}
	// The session's end can't be canceled.
	a.Try("poweroff")
	if !a.Wait(10 * time.Second) {
		t.Fatal("the session's end didn't quit the app")
	}
}

// An allowed quit, from the Quit command: the app is asked once, and ends.
func TestShellQuitCommand(t *testing.T) {
	a := Run(t, `
		state asked = 0
		app {
		  on quit(e) { asked += 1 }
		  Column {
		    Text("asked {asked}")
		    Button("Quit") { on press { quit() } }
		  }
		}
	`)
	a.UntilText("asked 0")
	a.Try("tap Quit")
	if !a.Wait(10 * time.Second) {
		t.Fatalf("quit() didn't quit the app:\n%s", a.Logs())
	}
}

// SIGTERM (and SIGINT: Ctrl+C, asuka dev stopping the app) quits as ⌘Q
// does: the app's on quit is asked, and can cancel it; a second signal
// ends the app at once, its main thread stuck or not.
func TestShellQuitSignal(t *testing.T) {
	a := Run(t, `
		state asked = 0
		app {
		  on quit(e) {
		    asked += 1
		    e.cancel()
		  }
		  Text("asked {asked}")
		}
	`)
	a.UntilText("asked 0")
	a.cmd.Process.Signal(syscall.SIGTERM)
	a.UntilText("asked 1")
	time.Sleep(500 * time.Millisecond)
	if a.Exited() {
		t.Fatal("a canceled quit quit")
	}
	a.cmd.Process.Signal(syscall.SIGTERM)
	if !a.Wait(10 * time.Second) {
		t.Fatal("a second SIGTERM didn't end the app")
	}
	if st, ok := a.cmd.ProcessState.Sys().(syscall.WaitStatus); !ok || st.ExitStatus() != 128+int(syscall.SIGTERM) {
		t.Errorf("the app ended with %v, want 143", a.cmd.ProcessState)
	}
}

// relaunch() quits as quit() does, then opens the app again once it has
// ended: its bundle, through Launch Services.
func TestShellRelaunch(t *testing.T) {
	exe := Player(t)
	a := Run(t, `
		state asked = 0
		app {
		  on quit(e) { asked += 1 }
		  Column {
		    Text("asked {asked}")
		    Button("Relaunch") { on press { relaunch() } }
		  }
		}
	`)
	a.UntilText("asked 0")
	old := a.cmd.Process.Pid
	a.Try("tap Relaunch")
	if !a.Wait(10 * time.Second) {
		t.Fatalf("relaunch() didn't quit the app:\n%s", a.Logs())
	}
	// The new process runs the bundle's executable (without asuka dev's
	// app: the player alone).
	var pid string
	deadline := time.Now().Add(30 * time.Second)
	for time.Now().Before(deadline) && pid == "" {
		out, _ := exec.Command("pgrep", "-f", exe).Output()
		for _, p := range strings.Fields(string(out)) {
			if p != "" && p != strconv.Itoa(old) {
				pid = p
			}
		}
		time.Sleep(200 * time.Millisecond)
	}
	if pid == "" {
		t.Fatal("the app wasn't opened again")
	}
	exec.Command("kill", "-9", pid).Run()
}

// Recent documents: newest first, without those gone, in the File menu's
// Open Recent, where choosing one opens it; Clear Menu empties them.
func TestShellRecentDocuments(t *testing.T) {
	dir, _ := filepath.EvalSymlinks(t.TempDir()) // /private/var: what AppKit answers
	a, b, gone := filepath.Join(dir, "a.md"), filepath.Join(dir, "b.md"), filepath.Join(dir, "gone.md")
	for _, p := range []string{a, b, gone} {
		os.WriteFile(p, []byte("x"), 0o644)
	}
	app := Run(t, sysCallSrc+`
		state step = 0
		state opened = ""
		app {
		  on files(paths) { opened = opened + "[" + paths.joined(",") + "]" }
		  Column {
		    Text("opened {opened}")
		    Button("Next") { on press { step += 1 } }
		    Sys("recent", "clear", bytes()) { label: "clear" }
		    if step >= 1 {
		      Sys("recent", "add", `+shellString(gone)+`.bytes()) { label: "gone" }
		    }
		    if step >= 2 {
		      Sys("recent", "add", `+shellString(a)+`.bytes()) { label: "a" }
		      Sys("recent", "add", "not/absolute".bytes()) { label: "relative" }
		    }
		    if step >= 3 { Sys("recent", "add", `+shellString(b)+`.bytes()) { label: "b" } }
		    if step >= 4 { Sys("recent", "add", `+shellString(a)+`.bytes()) { label: "a2" } }
		    if step >= 5 { Sys("recent", "list", bytes()) { label: "list"; text: true } }
		    if step >= 6 { Sys("recent", "list", bytes()) { label: "after"; text: true } }
		  }
		}
	`)
	sysPrefix(t, app, "clear", "ok")
	for _, l := range []string{"gone", "a", "b", "a2"} {
		app.Do("tap Next")
		sysPrefix(t, app, l, "ok")
	}
	sysWant(t, app, "relative", "err platform/recent: not an absolute path")
	os.Remove(gone)
	app.Do("tap Next")
	s := strings.TrimSuffix(strings.TrimPrefix(sysText(t, app, "list"), "ok ["), "]")
	var paths []string
	if err := json.Unmarshal([]byte(s), &paths); err != nil || strings.Join(paths, "|") != a+"|"+b {
		t.Fatalf("list = %q, want [a b]", s)
	}
	// The File menu's Open Recent.
	tr := app.Until(func(tr *Tree) bool { return shellMenuItem(tr, "File", "Open Recent") != nil })
	var titles []string
	for _, it := range shellMenuItem(tr, "File", "Open Recent").Items {
		titles = append(titles, it.Title)
	}
	if strings.Join(titles, "|") != "a.md|b.md||Clear Menu" {
		t.Fatalf("Open Recent: %q", titles)
	}
	app.Do("menu b.md")
	app.UntilText("opened [" + b + "]")
	app.Do("menu Clear Menu", "tap Next")
	if s := sysText(t, app, "after"); s != "ok [[]]" {
		t.Fatalf("after Clear Menu: %s", s)
	}
}

// The login item: whether the app is one, and whether it opened at login
// (once it has launched): not, in a test. Turning it off where it isn't on
// is nothing to do.
func TestShellLoginItem(t *testing.T) {
	a := Run(t, sysCallSrc+`
		app {
		  Column {
		    Sys("login", "enabled", bytes()) { label: "enabled" }
		    Sys("login", "openedAtLogin", bytes()) { label: "atLogin" }
		    Sys("login", "set", host.u8(0)) { label: "off" }
		  }
		}
	`)
	sysWant(t, a, "enabled", "ok 00")
	sysWant(t, a, "atLogin", "ok 00")
	if s := sysText(t, a, "off"); s != "ok " && !strings.Contains(s, "bundle") && !strings.Contains(s, "macOS 13") {
		t.Fatalf("set off: %s", s)
	}
}
