// The Windows host's tests. They build the host for Windows (Zig's MinGW
// target from any system: ASUKA_CC=zig, or this PC's compiler on
// Windows) and run what they built on Windows: here on a PC, else in the
// Windows VM swiftvm runs (ASUKA_WINDOWS_VM names it, "Windows Fresh" by
// default), which sees ~/Library/Caches/asuka/windows-vm as
// Z:\windows-vm. With neither, they skip.
//
//   - TestC builds each test/*_test.c with the host (its main left out)
//     and the core, the apps under test/apps compiled to bytecode beside
//     it, and runs it: the host's own tests, in C (t.h's TEST).
//   - TestApps build apps (an Asuka source each) into the executable an
//     app ships, and drive them as a user does (ASUKA_SCRIPT), reading
//     back what they showed (ASUKA_DUMP, ASUKA_SNAPSHOT).
//   - TestStdViews runs the std and platform packages' view tests
//     ({std,sdk}/*/desktop/*_windows_test.c).
package windows_test

import (
	"bytes"
	"crypto/rand"
	"encoding/hex"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"sort"
	"strings"
	"testing"
	"time"

	"github.com/egoist/asuka/internal/appbuild"
	"github.com/egoist/asuka/internal/cbuild"
	"github.com/egoist/asuka/lang/compile"
)

// windows is where the tests run their programs.
type windows struct {
	vm    string // the VM's name; "" on Windows itself
	share string // a directory both see: the jobs' files go here
	guest string // share, as the program sees it
	arch  string
}

var win *windows

func onWindows(t *testing.T) *windows {
	t.Helper()
	if win != nil {
		return win
	}
	if runtime.GOOS == "windows" {
		dir, err := os.MkdirTemp("", "asuka-windows-")
		if err != nil {
			t.Fatal(err)
		}
		win = &windows{share: dir, guest: dir, arch: runtime.GOARCH}
		return win
	}
	if _, err := exec.LookPath("swiftvm"); err != nil {
		t.Skip("the Windows host's tests run on Windows, or in a Windows VM (swiftvm)")
	}
	name := os.Getenv("ASUKA_WINDOWS_VM")
	if name == "" {
		name = "Windows Fresh"
	}
	out, err := exec.Command("swiftvm", "list").Output()
	if err != nil || !bytes.Contains(out, []byte(name)) {
		t.Skipf("no Windows VM %q (swiftvm list)", name)
	}
	if !vmRunning(out, name) {
		t.Skipf("the Windows VM %q isn't running: swiftvm start %q", name, name)
	}
	cache, _ := os.UserCacheDir()
	share := filepath.Join(cache, "asuka", "windows-vm")
	if err := os.MkdirAll(share, 0o755); err != nil {
		t.Fatal(err)
	}
	win = &windows{vm: name, share: share, guest: `Z:\windows-vm`, arch: "arm64"}
	if a := os.Getenv("ASUKA_WINDOWS_ARCH"); a != "" {
		win.arch = a // amd64 runs there too, emulated
	}
	return win
}

func vmRunning(list []byte, name string) bool {
	for _, l := range strings.Split(string(list), "\n") {
		if strings.HasPrefix(l, name) && strings.Contains(l, "running") {
			return true
		}
	}
	return false
}

// job is a directory of the share's for one run's files, removed when the
// test ends.
func (w *windows) job(t *testing.T) (local, guest string) {
	t.Helper()
	b := make([]byte, 6)
	rand.Read(b)
	id := "job-" + hex.EncodeToString(b)
	local = filepath.Join(w.share, "jobs", id)
	if err := os.MkdirAll(local, 0o755); err != nil {
		t.Fatal(err)
	}
	if os.Getenv("ASUKA_KEEP") == "" {
		t.Cleanup(func() { os.RemoveAll(local) })
	} else {
		t.Logf("kept %s", local)
	}
	if w.vm == "" {
		return local, local
	}
	return local, w.guest + `\jobs\` + id
}

// run runs exe (a guest path) with env and args in dir, and returns its
// output and whether it exited 0.
func (w *windows) run(t *testing.T, dir, exe string, env map[string]string, args ...string) (string, error) {
	t.Helper()
	return w.runApp(t, dir, exe, env, false, args...)
}

// runRelaunching is run for an app that relaunches itself: the run ends
// once no process of exe's name is left (90 seconds at most). In the VM
// the app runs as a scheduled task, whose job Task Scheduler ends, with
// every process in it, once the task's command has, and lets none break
// away: the relaunched app lives only as long as the task.
func (w *windows) runRelaunching(t *testing.T, dir, exe string, env map[string]string, args ...string) (string, error) {
	t.Helper()
	return w.runApp(t, dir, exe, env, true, args...)
}

func (w *windows) runApp(t *testing.T, dir, exe string, env map[string]string, relaunching bool, args ...string) (string, error) {
	t.Helper()
	if w.vm == "" {
		cmd := exec.Command(exe, args...)
		cmd.Dir = dir
		cmd.Env = os.Environ()
		for k, v := range env {
			cmd.Env = append(cmd.Env, k+"="+v)
		}
		out, err := cmd.CombinedOutput()
		return string(out), err
	}
	// Over SSH a program has no desktop to show its windows on: it runs
	// in the signed-in user's session, as a scheduled task does, and its
	// output and exit code come back in files of its job.
	local := w.local(dir)
	var cmd strings.Builder
	cmd.WriteString("@echo off\r\n")
	keys := make([]string, 0, len(env))
	for k := range env {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	for _, k := range keys {
		fmt.Fprintf(&cmd, "set \"%s=%s\"\r\n", k, env[k])
	}
	fmt.Fprintf(&cmd, "cd /d \"%s\"\r\n\"%s\"", dir, exe)
	for _, a := range args {
		fmt.Fprintf(&cmd, " \"%s\"", a)
	}
	cmd.WriteString(" > out.txt 2>&1\r\nset \"asuka_code=%ERRORLEVEL%\"\r\n")
	if relaunching {
		name := strings.TrimSuffix(exe[strings.LastIndex(exe, `\`)+1:], ".exe")
		fmt.Fprintf(&cmd, "powershell -NoProfile -Command \"$end = (Get-Date).AddSeconds(90); while ((Get-Process -Name '%s' -ErrorAction SilentlyContinue) -and (Get-Date) -lt $end) { Start-Sleep -Milliseconds 200 }\"\r\n", name)
	}
	cmd.WriteString("echo %asuka_code% > code.txt\r\n")
	// A run before in the job left its output.
	os.Remove(filepath.Join(local, "code.txt"))
	os.Remove(filepath.Join(local, "out.txt"))
	if err := os.WriteFile(filepath.Join(local, "run.cmd"), []byte(cmd.String()), 0o644); err != nil {
		t.Fatal(err)
	}
	task := "asuka-" + filepath.Base(local)
	ps := fmt.Sprintf(`$a = New-ScheduledTaskAction -Execute 'cmd.exe' -Argument '/c "%s\run.cmd"'; `+
		`Register-ScheduledTask -TaskName '%s' -Action $a -User $env:USERNAME -RunLevel Limited -Force | Out-Null; `+
		`Start-ScheduledTask -TaskName '%s'`, dir, task, task)
	if out, err := exec.Command("swiftvm", "exec", w.vm, "--", ps).CombinedOutput(); err != nil {
		t.Fatalf("starting %s in the VM: %v\n%s", exe, err, out)
	}
	defer exec.Command("swiftvm", "exec", w.vm, "--", fmt.Sprintf(`Unregister-ScheduledTask -TaskName '%s' -Confirm:$false`, task)).Run()
	deadline := time.Now().Add(runTimeout)
	for {
		if b, err := os.ReadFile(filepath.Join(local, "code.txt")); err == nil && len(bytes.TrimSpace(b)) > 0 {
			out, _ := os.ReadFile(filepath.Join(local, "out.txt"))
			if code := strings.TrimSpace(string(b)); code != "0" {
				return string(out), fmt.Errorf("exit status %s", code)
			}
			return string(out), nil
		}
		if time.Now().After(deadline) {
			out, _ := os.ReadFile(filepath.Join(local, "out.txt"))
			exec.Command("swiftvm", "exec", w.vm, "--", fmt.Sprintf(`Stop-ScheduledTask -TaskName '%s'; Stop-Process -Name '%s' -Force -ErrorAction SilentlyContinue`, task, strings.TrimSuffix(filepath.Base(exe), ".exe"))).Run()
			return string(out), fmt.Errorf("%s didn't end in %v", exe, runTimeout)
		}
		time.Sleep(200 * time.Millisecond)
	}
}

// runTimeout is how long a program the tests run may take.
const runTimeout = 2 * time.Minute

// local is a guest path of the share's, as this computer sees it.
func (w *windows) local(guest string) string {
	if w.vm == "" {
		return guest
	}
	rel := strings.TrimPrefix(guest, w.guest)
	return filepath.Join(w.share, filepath.FromSlash(strings.ReplaceAll(rel, `\`, "/")))
}

func psQuote(s string) string { return "'" + strings.ReplaceAll(s, "'", "''") + "'" }

func target(w *windows) cbuild.Target { return cbuild.Target{OS: "windows", Arch: w.arch} }

func zig(t *testing.T) {
	if runtime.GOOS != "windows" && os.Getenv("ASUKA_CC") == "" {
		t.Setenv("ASUKA_CC", "zig")
	}
}

// buildApp compiles src (an Asuka file with its app) to C and builds the
// app's executable, as asuka build does, into dir.
func buildApp(t *testing.T, w *windows, src, dir string) string {
	t.Helper()
	zig(t)
	res := compile.Files(map[string][]byte{"app.asuka": []byte(src)}, &compile.Options{Platform: "windows"})
	if err := res.Err(); err != nil {
		t.Fatalf("the app doesn't compile:\n%v", err)
	}
	c, err := appbuild.AppC(appbuild.StripTests(res.Module))
	if err != nil {
		t.Fatal(err)
	}
	exe := filepath.Join(dir, "app.exe")
	if err := appbuild.WindowsHost(target(w), appbuild.WindowsOptions{App: []string{c}, Console: true}, exe); err != nil {
		t.Fatal(err)
	}
	return exe
}

const counter = `
component Counter {
  state count = 0
  Column {
    gap: 8; padding: 16
    Text("Count: {count}")
    Button("Add") { on press { count += 1 } }
  }
}
app { Counter() }
`

// The Counter as an app ships: compiled to C into the host's executable,
// Add pressed (a real click: BM_CLICK, through BN_CLICKED to its parent),
// the text set again, laid out in the window, and the window written.
func TestCounter(t *testing.T) {
	w := onWindows(t)
	local, guest := w.job(t)
	buildApp(t, w, counter, local)
	out, err := w.run(t, guest, guest+`\app.exe`, map[string]string{
		"ASUKA_SCRIPT":   "tap:Add",
		"ASUKA_SNAPSHOT": guest + `\c.png`,
		"ASUKA_DUMP":     "1",
	})
	dump, rerr := os.ReadFile(filepath.Join(local, "c.png.txt"))
	if err != nil || rerr != nil {
		t.Fatalf("%v %v\n%s", err, rerr, out)
	}
	if !strings.Contains(string(dump), `"Count: 1"`) {
		t.Fatalf("after tapping Add, the window shows:\n%s\n%s", dump, out)
	}
	if _, err := os.Stat(filepath.Join(local, "c.png")); err != nil {
		t.Fatalf("no snapshot: %v\n%s", err, out)
	}
	t.Logf("%s", dump)
}

// buildExample builds examples/<name> as asuka build does, into dir.
func buildExample(t *testing.T, w *windows, name, dir string) string {
	t.Helper()
	zig(t)
	p, err := appbuild.Load(filepath.Join("..", "..", "examples", name))
	if err != nil {
		t.Fatal(err)
	}
	m, diags, err := p.Compile("windows")
	if err != nil || m == nil {
		t.Fatalf("%s doesn't compile: %v\n%s", name, err, strings.Join(diags, "\n"))
	}
	c, err := appbuild.AppC(appbuild.StripTests(m))
	if err != nil {
		t.Fatal(err)
	}
	exe := filepath.Join(dir, name+".exe")
	if err := appbuild.WindowsHost(target(w), appbuild.WindowsOptions{App: []string{c}, Console: true}, exe); err != nil {
		t.Fatal(err)
	}
	return exe
}

// script runs exe (built into the job local, guest) with ASUKA_SCRIPT's
// steps, a snap:<name> each dumped, and returns the dumps by name.
func script(t *testing.T, w *windows, local, guest, exe string, steps ...string) map[string]string {
	t.Helper()
	var s []string
	var snaps []string
	for _, st := range steps {
		if name, ok := strings.CutPrefix(st, "snap:"); ok {
			snaps = append(snaps, name)
			st = "snap:" + guest + `\` + name + ".png"
		}
		s = append(s, st)
	}
	out, err := w.run(t, guest, guest+`\`+exe, map[string]string{"ASUKA_SCRIPT": strings.Join(s, ";"), "ASUKA_DUMP": "1", "ASUKA_SNAPSHOT": guest + `\end.png`})
	if err != nil {
		t.Fatalf("%v\n%s", err, out)
	}
	dumps := map[string]string{}
	for _, name := range append(snaps, "end") {
		b, err := os.ReadFile(filepath.Join(local, name+".png.txt"))
		if err != nil {
			t.Fatalf("no dump %s: %v\n%s", name, err, out)
		}
		dumps[name] = string(b)
	}
	if strings.Contains(out, "automation: nothing") || strings.Contains(out, "automation: no ") {
		t.Errorf("the script missed a step:\n%s", out)
	}
	return dumps
}

func shows(t *testing.T, dump, where string, want []string, not ...string) {
	t.Helper()
	for _, s := range want {
		if !strings.Contains(dump, fmt.Sprintf("%q", s)) {
			t.Errorf("%s: %q isn't shown:\n%s", where, s, dump)
		}
	}
	for _, s := range not {
		if strings.Contains(dump, fmt.Sprintf("%q", s)) && !strings.Contains(dump, fmt.Sprintf("%q hidden", s)) {
			for _, l := range strings.Split(dump, "\n") {
				if strings.Contains(l, fmt.Sprintf("%q", s)) && !strings.Contains(l, " hidden") && !strings.Contains(l, " parked") {
					t.Errorf("%s: %q is shown:\n%s", where, s, dump)
					break
				}
			}
		}
	}
}

// Navigation, as e2e's navigation scenario drives the other platforms
// (testdata/nav.asuka): a list's row pushes a screen, a sheet's form,
// back, the tabs.
func TestNavigation(t *testing.T) {
	w := onWindows(t)
	local, guest := w.job(t)
	src, err := os.ReadFile(filepath.Join("testdata", "nav.asuka"))
	if err != nil {
		t.Fatal(err)
	}
	buildApp(t, w, string(src), local)
	d := script(t, w, local, guest, "app.exe",
		"snap:inbox",
		"tap:Add a note", "snap:sheet",
		"tap:Note", "type:hello", "tap:Save", "snap:saved",
		"tap:Settings", "snap:settings",
		"tap:Inbox", "snap:back")
	shows(t, d["inbox"], "the inbox", []string{"Message 30", "Message 29", "No note yet."})
	shows(t, d["sheet"], "the sheet", []string{"Save"})
	t.Logf("sheet:\n%s", d["sheet"])
	if !strings.Contains(d["sheet"], "sheet:") {
		t.Errorf("no sheet shows:\n%s", d["sheet"])
	}
	shows(t, d["saved"], "saved", []string{"Note: hello"})
	if strings.Contains(d["saved"], "sheet:") {
		t.Errorf("the sheet stayed:\n%s", d["saved"])
	}
	shows(t, d["settings"], "settings", []string{"Notifications", "About"}, "Note: hello")
	shows(t, d["back"], "the inbox again", []string{"Note: hello"}, "Notifications")
}

// Rich text (testdata/rich.asuka): a RichText's spans and paragraphs, a
// Rich TextField's styled text in and the user's typing out, and the
// core's text replacing it.
func TestRichText(t *testing.T) {
	w := onWindows(t)
	local, guest := w.job(t)
	src, err := os.ReadFile(filepath.Join("testdata", "rich.asuka"))
	if err != nil {
		t.Fatal(err)
	}
	buildApp(t, w, string(src), local)
	d := script(t, w, local, guest, "app.exe",
		"snap:start",
		"tap:Notes", "type:X", "snap:typed",
		"tap:Reset", "snap:reset")
	shows(t, d["start"], "the start", []string{"Read the docs first.\nSecond line.", "Hello world", "Spans: 2"})
	if !strings.Contains(d["typed"], `XHello world"`) && !strings.Contains(d["typed"], `"Hello worldX"`) {
		t.Errorf("the typing isn't in the field:\n%s", d["typed"])
	}
	shows(t, d["reset"], "reset", []string{"Plain", "Spans: 1"})
}

// Overlays and pickers (testdata/overlays.asuka): a SplitView's sidebar,
// a Picker's choice, a Popover opened and closed from inside it.
func TestOverlays(t *testing.T) {
	w := onWindows(t)
	local, guest := w.job(t)
	src, err := os.ReadFile(filepath.Join("testdata", "overlays.asuka"))
	if err != nil {
		t.Fatal(err)
	}
	buildApp(t, w, string(src), local)
	d := script(t, w, local, guest, "app.exe",
		"snap:start",
		"tap:Banana", "snap:picked",
		"tap:Info", "snap:popover",
		"tap:Done", "snap:closed",
		"tap:Drafts", "snap:drafts")
	shows(t, d["start"], "the inbox", []string{"No fruit", "Closed", "Info"})
	if !strings.Contains(d["start"], " Split [") || !strings.Contains(d["start"], " Picker [") {
		t.Errorf("no split view or picker:\n%s", d["start"])
	}
	shows(t, d["picked"], "picked", []string{"Fruit 1"}, "No fruit")
	shows(t, d["popover"], "the popover", []string{"Open"}, "Closed")
	shows(t, d["closed"], "closed", []string{"Closed"}, "Open")
	shows(t, d["drafts"], "drafts", []string{"Draft list"}, "Fruit 1")
}

func TestWebView(t *testing.T) {
	w := onWindows(t)
	local, guest := w.job(t)
	src, err := os.ReadFile(filepath.Join("testdata", "web.asuka"))
	if err != nil {
		t.Fatal(err)
	}
	buildApp(t, w, string(src), local)
	d := script(t, w, local, guest, "app.exe", "wait:5s", "snap:web")
	shows(t, d["web"], "the page's", []string{"Got: hi from the page", "Loaded.", "The video failed."}, "Nothing yet.")
	t.Logf("%s", d["web"])
}
