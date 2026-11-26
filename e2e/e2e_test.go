//go:build e2e

// Package e2e drives the example apps on a simulator and an emulator,
// through the platform's widgets, not the core. The tests build each app
// with asuka dev (a debug build, which asuka dev serves its code to for
// the test's run; the player's tests, player_test.go, run asuka dev's
// player instead), then tap
// and read the screen through the debug hosts' automation
// (platform/ios/Sources/AsukaHost/Automation.swift, a notification;
// platform/android/asuka/src/main/java/dev/asuka/Automation.kt, a broadcast;
// platform/web/src/automation.js, through asuka dev --platform web's server;
// platform/linux/src/automation.c, a file).
//
//	go test -tags e2e ./e2e -run IOS      # a booted simulator, else the newest iPhone
//	go test -tags e2e ./e2e -run Android  # a running emulator or a device
//	go test -tags e2e ./e2e -run Web      # headless Chrome (ASUKA_CHROME: its path)
//	go test -tags e2e ./e2e -run StdWebViews  # std packages' web views ({std,sdk}/*/tests/*_view_test.js)
//	xvfb-run -a dbus-run-session go test -tags e2e ./e2e -run Linux  # GTK 4, on Linux
//	ASUKA_DEVICE=emulator-5556 go test -tags e2e ./e2e -run Android  # one of several
//
// They wait for what they expect (bounded, so a broken app fails rather
// than hangs), never for a fixed time.
//
// The examples and the apps in testdata are Asuka packages (asuka.toml,
// app.asuka and the files beside it): asuka dev builds each from its
// directory.
package e2e

import (
	"bufio"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"testing"
	"time"

	"github.com/egoist/asuka/internal/androidsdk"
)

// within is how long a test waits for the screen to show what it expects.
const within = 60 * time.Second

func root(t *testing.T) string {
	t.Helper()
	dir, err := filepath.Abs("..")
	if err != nil {
		t.Fatal(err)
	}
	return dir
}

// appDir is an app's directory from the repository's root: an example's
// name, or a path (conformance/app).
func appDir(app string) string {
	if strings.Contains(app, "/") {
		return app
	}
	return "examples/" + app
}

// launch builds an app (appDir) for the platform and launches it with
// asuka dev, which runs until the test ends: a relaunched app (the tests
// that check what's kept across launches) reconnects to it for its code,
// as an app does in development. It returns what asuka dev printed once
// the app was launched. ASUKA_DEVICE picks the simulator, emulator or
// device (asuka dev's --device), when there's more than one.
func launch(t *testing.T, platform, app string) string {
	t.Helper()
	cli := filepath.Join(t.TempDir(), "asuka")
	if runtime.GOOS == "windows" {
		cli += ".exe"
	}
	start := time.Now()
	run(t, "go", "build", "-C", root(t), "-o", cli, "./cmd/asuka")
	built := time.Since(start)
	args := []string{"dev", "--platform", platform}
	if d := os.Getenv("ASUKA_DEVICE"); d != "" {
		args = append(args, "--device", d)
	}
	c := exec.Command(cli, append(args, appDir(app))...)
	c.Dir = root(t)
	var logs syncBuffer
	c.Stdout, c.Stderr = &logs, &logs
	// What it starts may keep its output open: Wait doesn't wait for that.
	c.WaitDelay = 5 * time.Second
	interruptible(c)
	if err := c.Start(); err != nil {
		t.Fatal(err)
	}
	exited := make(chan struct{})
	go func() { c.Wait(); close(exited) }()
	// Stopped as asuka dev is (Ctrl-C's), which stops the app's run.
	t.Cleanup(func() {
		if err := interrupt(c.Process); err != nil {
			c.Process.Kill()
		}
		select {
		case <-exited:
		case <-time.After(30 * time.Second):
			c.Process.Kill()
			<-exited
		}
		if t.Failed() {
			t.Logf("asuka dev --platform %s:\n%s", platform, logs.String())
		}
	})
	// The first build of a platform's host takes a while.
	deadline := time.Now().Add(15 * time.Minute)
	// Launched is connected: a player relaunched before it connected (as a
	// test that relaunches at once does) would find the last run's asuka
	// dev, which it keeps, and not this one's.
	for !strings.Contains(logs.String(), "watching for changes") || !strings.Contains(logs.String(), " connected\n") {
		select {
		case <-exited:
			t.Fatalf("asuka dev --platform %s %s quit:\n%s", platform, appDir(app), logs.String())
		case <-time.After(200 * time.Millisecond):
		}
		if time.Now().After(deadline) {
			t.Fatalf("asuka dev --platform %s %s didn't launch the app:\n%s", platform, appDir(app), logs.String())
		}
	}
	// Where a slow launch's time went (go test -v shows it): the CLI's
	// build, then its own steps (the simulator's boot, the player's build).
	var steps []string
	for _, l := range strings.Split(logs.String(), "\n") {
		if strings.HasPrefix(l, "asuka: ") && !strings.Contains(l, "watching for changes") {
			steps = append(steps, "  "+l)
		}
	}
	t.Logf("launched in %s (the CLI built in %s):\n%s", time.Since(start).Round(100*time.Millisecond), built.Round(100*time.Millisecond), strings.Join(steps, "\n"))
	return logs.String()
}

func run(t *testing.T, name string, args ...string) string {
	t.Helper()
	out, err := exec.Command(name, args...).CombinedOutput()
	if err != nil {
		t.Fatalf("%s %s: %v\n%s", name, strings.Join(args, " "), err, out)
	}
	return string(out)
}

// screen is what an app's screen shows, and input to it.
type screen interface {
	texts() []string
	tap(text string)
	command(cmds string) // the automation's: type <text>, submit...
	relaunch()           // quits the app and starts it again
}

// waitFor waits until the screen shows every one of want and none of
// not; it fails the test if it doesn't within the bound.
func waitFor(t *testing.T, s screen, want []string, not ...string) {
	t.Helper()
	deadline := time.Now().Add(within)
	var got []string
	for time.Now().Before(deadline) {
		got = s.texts()
		if hasAll(got, want) && !hasAny(got, not) {
			return
		}
		time.Sleep(200 * time.Millisecond)
	}
	t.Fatalf("the screen doesn't show %q (without %q); it shows %q", want, not, got)
}

func hasAll(got, want []string) bool {
	for _, w := range want {
		if !hasAny(got, []string{w}) {
			return false
		}
	}
	return true
}

func hasAny(got, any []string) bool {
	for _, g := range got {
		for _, a := range any {
			if g == a {
				return true
			}
		}
	}
	return false
}

// counter is the same test on each platform: the counter counts, shows
// a note past ten, and resets.
func counter(t *testing.T, s screen) {
	waitFor(t, s, []string{"Count: 0", "Add", "Reset"}, "That's a lot")
	for i := 1; i <= 10; i++ {
		s.tap("Add")
		waitFor(t, s, []string{fmt.Sprintf("Count: %d", i)})
	}
	waitFor(t, s, []string{"That's a lot"})
	s.tap("Reset")
	waitFor(t, s, []string{"Count: 0"}, "That's a lot")
}

// notes is the same test on each platform: a note typed and submitted
// is written to SQLite and listed, and is still there when the app
// starts again.
func notes(t *testing.T, s screen) {
	waitFor(t, s, []string{"Add"})
	note := fmt.Sprintf("e2e note %d", time.Now().UnixNano())
	s.command("type " + note)
	s.command("submit")
	waitFor(t, s, []string{note})
	s.relaunch()
	waitFor(t, s, []string{"Add", note})
}

// navigation is the same test on each platform: a row pushes its
// screen; a button there presents a sheet, whose field and Save button
// write back to the screen; back pops it; a tab shows another stack.
func navigation(t *testing.T, s screen) {
	waitFor(t, s, []string{"Message 20", "Message 19"})
	s.tap("Message 20")
	waitFor(t, s, []string{"Add a note", "No note yet."})
	s.tap("Add a note")
	waitFor(t, s, []string{"Save", "Note"})
	s.command("type hello")
	s.tap("Save")
	waitFor(t, s, []string{"Note: hello"}, "Save")
	// A full-screen sheet: on a phone and in a browser it's told the
	// screen's own size and safe area, the root's; on a desktop it covers
	// its window (all of it on Linux, the header bar too: not the root's).
	s.tap("Write a Reply")
	if _, desktop := s.(*linuxScreen); desktop {
		waitFor(t, s, []string{"To: Linus", "Send"})
	} else {
		waitFor(t, s, []string{"To: Linus", "Send", "Covers the screen"})
	}
	s.tap("Cancel")
	waitFor(t, s, []string{"Add a note", "Note: hello"}, "To: Linus")
	s.command("back")
	waitFor(t, s, []string{"Message 19"}, "Add a note", "Note: hello")
	s.command("tab Settings")
	waitFor(t, s, []string{"Notifications", "About"}, "Message 19")
	s.tap("About")
	waitFor(t, s, []string{"Native widgets, driven by Asuka and fine-grained signals."})
	s.command("back")
	waitFor(t, s, []string{"About"}, "Native widgets, driven by Asuka and fine-grained signals.")
	s.command("tab Inbox")
	waitFor(t, s, []string{"Message 19"}, "Notifications")
}

// languages is the same test on each platform, from an app in English: its
// languages menu changes what the screen says at once, keeping the state the
// user made, and the choice is there when the app starts again.
func languages(t *testing.T, s screen) {
	waitFor(t, s, []string{"Hello, Ada!", "3 unread messages"})
	s.tap("More")
	waitFor(t, s, []string{"4 unread messages"})
	s.command("tab Settings")
	waitFor(t, s, []string{"English", "Deutsch", "العربية"}, "Hello, Ada!")
	s.tap("العربية")
	waitFor(t, s, []string{"مثل النظام"})
	s.command("tab الوارد")
	waitFor(t, s, []string{"مرحبًا، Ada!", "4 رسائل غير مقروءة"}, "Hello, Ada!")
	s.command("tab الإعدادات")
	s.tap("Deutsch")
	s.command("tab Eingang")
	waitFor(t, s, []string{"Hallo, Ada!", "4 ungelesene Nachrichten"}, "مرحبًا، Ada!")
	s.relaunch()
	waitFor(t, s, []string{"Hallo, Ada!", "3 ungelesene Nachrichten"})
	s.command("tab Einstellungen")
	s.tap("English")
	s.command("tab Inbox")
	waitFor(t, s, []string{"Hello, Ada!"}, "Hallo, Ada!")
}

// chat is the same test on each platform: the conversation opens at its
// newest message (a list anchored at its bottom); a message sent from
// the composer shows, and so does the answer that comes after it, the
// list staying at its end.
func chat(t *testing.T, s screen) {
	waitFor(t, s, []string{"The sticky headers look right on Android now.", "Send"})
	msg := fmt.Sprintf("e2e %d", time.Now().UnixNano())
	s.command("type " + msg)
	s.tap("Send")
	waitFor(t, s, []string{msg, "Got it: “" + msg + "”"})
}

// iOS

type iosScreen struct {
	t      *testing.T
	udid   string
	bundle string
	tmp    string // the app's data container's tmp, where the automation reads and writes
}

func (s *iosScreen) command(cmds string) {
	file := filepath.Join(s.tmp, "asuka-commands.txt")
	// The app removes the file as it reads it. One it hasn't read yet (its main
	// thread is showing the keyboard, say) mustn't be written over, which would
	// lose that command: a loaded simulator's app can take seconds (CI's
	// lost a type before its send so). An app just launched may never read
	// one sent before it listened, so this is bounded, at half a minute.
	for range 3000 {
		if _, err := os.Stat(file); os.IsNotExist(err) {
			break
		}
		time.Sleep(10 * time.Millisecond)
	}
	if err := os.WriteFile(file, []byte(cmds+"\n"), 0o644); err != nil {
		s.t.Fatal(err)
	}
	run(s.t, "xcrun", "simctl", "spawn", s.udid, "notifyutil", "-p", "dev.asuka.automation")
}

func (s *iosScreen) texts() []string {
	dump := filepath.Join(s.tmp, "asuka-dump.txt")
	os.Remove(dump)
	s.command("dump")
	for range 50 {
		if b, err := os.ReadFile(dump); err == nil {
			return strings.Split(strings.TrimSpace(string(b)), "\n")
		}
		time.Sleep(100 * time.Millisecond)
	}
	return nil
}

func (s *iosScreen) tap(text string) { s.command("tap " + text) }

func (s *iosScreen) relaunch() {
	os.Remove(filepath.Join(s.tmp, "asuka-commands.txt")) // for the run that's ending
	run(s.t, "xcrun", "simctl", "launch", "--terminate-running-process", s.udid, s.bundle)
}

func iosApp(t *testing.T, example, bundle string) *iosScreen {
	if _, err := exec.LookPath("xcrun"); err != nil {
		t.Skip("no Xcode")
	}
	out := launch(t, "ios", example)
	m := regexp.MustCompile(`\(([0-9A-F-]{36})\)`).FindStringSubmatch(out)
	if m == nil {
		t.Fatalf("no simulator in:\n%s", out)
	}
	data := strings.TrimSpace(run(t, "xcrun", "simctl", "get_app_container", m[1], bundle, "data"))
	return &iosScreen{t: t, udid: m[1], bundle: bundle, tmp: filepath.Join(data, "tmp")}
}

func TestCounterIOS(t *testing.T) { counter(t, iosApp(t, "counter", "dev.asuka.counter")) }
func TestNotesIOS(t *testing.T)   { notes(t, iosApp(t, "notes", "dev.asuka.notes")) }
func TestChatIOS(t *testing.T)    { chat(t, iosApp(t, "chat", "dev.asuka.chat")) }
func TestNavigationIOS(t *testing.T) {
	navigation(t, iosApp(t, "navigation", "dev.asuka.navigation"))
}

// TestLanguagesIOS changes examples/i18n's language: at once, from its
// menu, and kept for the next launch (AppleLanguages) until it follows the
// system again. The simulator's language is English, as for the other tests.
func TestLanguagesIOS(t *testing.T) {
	s := iosApp(t, "i18n", "dev.asuka.i18n")
	reset := func() {
		exec.Command("xcrun", "simctl", "spawn", s.udid, "defaults", "delete", s.bundle, "AppleLanguages").Run() // none to delete is fine
	}
	reset()
	t.Cleanup(reset)
	s.relaunch()
	waitFor(t, s, []string{"Hello, Ada!", "More"})
	s.command("tab Settings")
	waitFor(t, s, []string{"As the system's", "Deutsch"}, "Hello, Ada!")
	s.tap("Deutsch")
	waitFor(t, s, []string{"Wie das System", "Deutsch"}, "As the system's")
	s.command("tab Eingang")
	waitFor(t, s, []string{"Hallo, Ada!", "Mehr"}, "Hello, Ada!")
	s.relaunch()
	waitFor(t, s, []string{"Hallo, Ada!", "Mehr"}, "Hello, Ada!")
	s.command("tab Einstellungen")
	waitFor(t, s, []string{"Wie das System"})
	s.tap("العربية")
	waitFor(t, s, []string{"مثل النظام"}, "Wie das System")
	s.command("tab الوارد")
	waitFor(t, s, []string{"المزيد"}, "Mehr")
	s.command("tab الإعدادات")
	waitFor(t, s, []string{"مثل النظام"}, "المزيد")
	s.tap("مثل النظام")
	waitFor(t, s, []string{"As the system's"}, "مثل النظام")
	s.relaunch()
	waitFor(t, s, []string{"Hello, Ada!", "More"})
}

// Android

type androidScreen struct {
	t      *testing.T
	serial string
	pkg    string
}

// androidTool is one of the Android SDK's tools: on the PATH, else in
// the SDK asuka dev finds (androidsdk; Windows' rarely has it on the
// PATH); "" if there's none.
func androidTool(name string) string {
	if p, err := exec.LookPath(name); err == nil {
		return p
	}
	sys := androidsdk.Local()
	sdk := sys.SDK()
	if sdk == "" {
		return ""
	}
	var p string
	switch name {
	case "adb":
		p = sys.ADB(sdk)
	case "keytool": // the JDK's, which Gradle runs on
		if java := sys.Java(); java != "" {
			p = filepath.Join(java, "bin", sys.Exe("keytool"))
		}
	}
	if _, err := os.Stat(p); p == "" || err != nil {
		return ""
	}
	return p
}

func adb() string { return androidTool("adb") }

func (s *androidScreen) adb(args ...string) (string, error) {
	out, err := exec.Command(adb(), append([]string{"-s", s.serial}, args...)...).CombinedOutput()
	return string(out), err
}

func (s *androidScreen) command(cmds string) {
	// adb shell joins its arguments into a command line: quoted once more.
	q := "'" + strings.ReplaceAll(cmds, "'", `'\''`) + "'"
	// adb fails now and then just after an install: tried again.
	var out string
	var err error
	for range 5 {
		if out, err = s.adb("shell", "am", "broadcast", "-a", "dev.asuka.automation", "-p", s.pkg, "--es", "commands", q); err == nil {
			return
		}
		time.Sleep(500 * time.Millisecond)
	}
	s.t.Fatalf("broadcast %q: %v\n%s", cmds, err, out)
}

func (s *androidScreen) texts() []string { return s.read("dump", "asuka-dump.txt") }

// read runs the automation's command, which writes its lines to the file.
func (s *androidScreen) read(cmd, file string) []string {
	s.adb("shell", "run-as", s.pkg, "rm", "-f", "files/"+file)
	s.command(cmd)
	for range 50 {
		if out, err := s.adb("exec-out", "run-as", s.pkg, "cat", "files/"+file); err == nil && strings.HasSuffix(out, "\n") {
			return strings.Split(strings.TrimSpace(out), "\n")
		}
		time.Sleep(100 * time.Millisecond)
	}
	return nil
}

func (s *androidScreen) tap(text string) { s.command("tap " + text) }

func (s *androidScreen) relaunch() {
	s.adb("shell", "am", "force-stop", s.pkg)
	// Not monkey: it refuses to run on recent emulator images, which have no system keys.
	if out, err := s.adb("shell", "am", "start", "-n", s.pkg+"/dev.asuka.AsukaActivity"); err != nil {
		s.t.Fatalf("launch: %v\n%s", err, out)
	}
	s.foreground()
}

func androidApp(t *testing.T, example, pkg string) *androidScreen {
	if adb() == "" {
		t.Skip("no adb")
	}
	out := launch(t, "android", example)
	m := regexp.MustCompile(`(?m)^asuka: \S+ on (\S+)$`).FindStringSubmatch(out)
	if m == nil {
		t.Fatalf("no device in:\n%s", out)
	}
	s := &androidScreen{t: t, serial: m[1], pkg: pkg}
	s.foreground()
	return s
}

var (
	focusRE   = regexp.MustCompile(`mCurrentFocus=Window\{\S+ \S+ ([^}]*)\}`)
	resumedRE = regexp.MustCompile(`topResumedActivity=ActivityRecord\{\S+ \S+ (\S+)`)
	waitRE    = regexp.MustCompile(`text="Wait"[^>]*bounds="\[(\d+),(\d+)\]\[(\d+),(\d+)\]"`)
)

// foreground waits for the app to be in front, answering Wait to what
// the system shows over it: a slow emulator's "isn't responding" dialogs
// (the launcher's, System UI's), which would cover what the test reads
// and taps, with the focus or without it. In front is the app's window
// having the focus on any display (a dump can hold several, the first
// not the app's: the emulator's images differ), or its activity the top
// resumed one, and no such dialog on the screen.
func (s *androidScreen) foreground() {
	var focus []string
	var resumed string
	until(s.t, func() bool {
		// The whole dump: "windows" alone has no focus line on Android 17,
		// and a display with none says mCurrentFocus=null.
		out, _ := s.adb("shell", "dumpsys", "window")
		focus = focus[:0]
		for _, m := range focusRE.FindAllStringSubmatch(out, -1) {
			focus = append(focus, m[1])
		}
		acts, _ := s.adb("shell", "dumpsys", "activity", "activities")
		resumed = ""
		if m := resumedRE.FindStringSubmatch(acts); m != nil {
			resumed = m[1]
		}
		front := strings.HasPrefix(resumed, s.pkg+"/")
		dialog := false
		for _, f := range focus {
			front = front || strings.HasPrefix(f, s.pkg+"/")
			dialog = dialog || strings.Contains(f, "Not Responding")
		}
		if !front && !dialog {
			return false
		}
		// What the screen shows: an "isn't responding" dialog may sit over
		// the app without the focus (the launcher's, on CI's images).
		_, answered, _ := s.uiDumpOnce()
		return front && !answered
	}, func() string {
		return fmt.Sprintf("the focus is on %q and the top resumed activity is %q, not %s%s", focus, resumed, s.pkg, s.crashLog())
	})
}

// uiDump is UiAutomator's dump of the screen, with no "isn't responding"
// dialog over the app: a slow emulator's launcher or System UI may show
// one at any time, without taking the focus, and Wait answers it.
func (s *androidScreen) uiDump() (string, error) {
	var ui string
	var err error
	for range 10 {
		var answered bool
		if ui, answered, err = s.uiDumpOnce(); err != nil || !answered {
			return ui, err
		}
		time.Sleep(500 * time.Millisecond)
	}
	return ui, err
}

// uiDumpOnce dumps the screen; answered says it showed an "isn't
// responding" dialog, whose Wait it pressed.
func (s *androidScreen) uiDumpOnce() (ui string, answered bool, err error) {
	if out, err := s.adb("shell", "uiautomator", "dump", "/sdcard/asuka-ui.xml"); err != nil {
		return out, false, err
	}
	if ui, err = s.adb("exec-out", "cat", "/sdcard/asuka-ui.xml"); err != nil {
		return ui, false, err
	}
	if m := waitRE.FindStringSubmatch(ui); m != nil && strings.Contains(ui, "t responding") {
		n := func(i int) int { v, _ := strconv.Atoi(m[i]); return v }
		s.adb("shell", "input", "tap", strconv.Itoa((n(1)+n(3))/2), strconv.Itoa((n(2)+n(4))/2))
		return ui, true, nil
	}
	return ui, false, nil
}

// crashLog is what the device logged of a crash, for a test that failed:
// the crash buffer (a native crash's backtrace, an uncaught exception's
// trace) and the last warnings and errors, which say why an app that
// started went away (a library that didn't load, the system killing it).
func (s *androidScreen) crashLog() string {
	var b strings.Builder
	if out, _ := s.adb("logcat", "-d", "-b", "crash"); strings.TrimSpace(out) != "" {
		b.WriteString("\nthe crash buffer:\n" + lastLines(out, 150))
	}
	if out, _ := s.adb("logcat", "-d", "-t", "400", "*:W"); strings.TrimSpace(out) != "" {
		b.WriteString("\nthe last warnings and errors:\n" + lastLines(out, 120))
	}
	// What the system and the app said of the app at any level: its
	// process started and died, its activity finished, the host's own
	// lines (asuka), and whatever its process logged.
	if out, _ := s.adb("logcat", "-d", "-t", "4000"); strings.TrimSpace(out) != "" {
		pids := map[string]bool{}
		for _, m := range regexp.MustCompile(`Start proc (\d+):`+regexp.QuoteMeta(s.pkg)).FindAllStringSubmatch(out, -1) {
			pids[m[1]] = true
		}
		var kept []string
		for _, l := range strings.Split(out, "\n") {
			f := strings.Fields(l)
			own := len(f) > 2 && pids[f[2]]
			if own || strings.Contains(l, s.pkg) || strings.Contains(l, " asuka") || strings.Contains(l, " Asuka") ||
				strings.Contains(l, "ActivityTaskManager") || strings.Contains(l, "ActivityManager") {
				kept = append(kept, l)
			}
		}
		b.WriteString("\nthe app's lines and the system's of it:\n" + lastLines(strings.Join(kept, "\n"), 200))
	}
	// Where the window manager has the focus, and, while the app runs,
	// what its threads are doing (a main thread that never returns to its
	// loop takes no focus): native backtraces, then Java's.
	if out, _ := s.adb("shell", "dumpsys", "window"); out != "" {
		var focus []string
		for _, l := range strings.Split(out, "\n") {
			if strings.Contains(l, "mCurrentFocus") || strings.Contains(l, "mFocusedApp") || strings.Contains(l, "mFocusedWindow") || strings.Contains(l, "mInputMethodTarget") {
				focus = append(focus, strings.TrimSpace(l))
			}
		}
		b.WriteString("\nthe focus:\n" + strings.Join(focus, "\n"))
	}
	if pid, _ := s.adb("shell", "pidof", s.pkg); strings.TrimSpace(pid) != "" {
		p := strings.Fields(pid)[0]
		if out, _ := s.adb("shell", "debuggerd", "-b", p); strings.TrimSpace(out) != "" {
			b.WriteString("\nits threads (native):\n" + firstLines(out, 160))
		}
		if out, _ := s.adb("shell", "debuggerd", "-j", p); strings.TrimSpace(out) != "" {
			b.WriteString("\nits threads (Java):\n" + firstLines(out, 120))
		}
	}
	return b.String()
}

// firstLines is text's first n lines.
func firstLines(text string, n int) string {
	lines := strings.Split(strings.TrimRight(text, "\n"), "\n")
	if len(lines) > n {
		lines = lines[:n]
	}
	return strings.Join(lines, "\n")
}

// lastLines is text's last n lines.
func lastLines(text string, n int) string {
	lines := strings.Split(strings.TrimRight(text, "\n"), "\n")
	if len(lines) > n {
		lines = lines[len(lines)-n:]
	}
	return strings.Join(lines, "\n")
}

func TestCounterAndroid(t *testing.T) { counter(t, androidApp(t, "counter", "dev.asuka.counter")) }
func TestNotesAndroid(t *testing.T)   { notes(t, androidApp(t, "notes", "dev.asuka.notes")) }
func TestChatAndroid(t *testing.T)    { chat(t, androidApp(t, "chat", "dev.asuka.chat")) }
func TestNavigationAndroid(t *testing.T) {
	navigation(t, androidApp(t, "navigation", "dev.asuka.navigation"))
}
func TestLanguagesAndroid(t *testing.T) {
	s := androidApp(t, "i18n", "dev.asuka.i18n")
	// The language a run before this one kept (Android 13's; before it the
	// test leaves English): what the test starts from is the app's own.
	s.adb("shell", "cmd", "locale", "set-app-locales", s.pkg, "--locales", "en")
	s.relaunch()
	languages(t, s)
}

// Web

// webScreen is a page asuka dev --platform web serves, open in headless Chrome; the
// automation's commands go through the dev server to the page.
type webScreen struct {
	t   *testing.T
	url string
}

func (s *webScreen) post(cmds string) (string, error) {
	resp, err := http.Post(s.url+"__asuka/automation", "text/plain", strings.NewReader(cmds))
	if err != nil {
		return "", err
	}
	defer resp.Body.Close()
	b, _ := io.ReadAll(resp.Body)
	if resp.StatusCode != 200 {
		return "", fmt.Errorf("%s: %s", resp.Status, b)
	}
	return string(b), nil
}

func (s *webScreen) command(cmds string) {
	// The page may not be listening yet (just loaded): tried again.
	var err error
	for range 50 {
		if _, err = s.post(cmds); err == nil {
			return
		}
		time.Sleep(200 * time.Millisecond)
	}
	s.t.Fatalf("%q: %v", cmds, err)
}

func (s *webScreen) texts() []string {
	out, err := s.post("dump")
	if err != nil {
		return nil
	}
	return strings.Split(strings.TrimSpace(out), "\n")
}

func (s *webScreen) tap(text string) { s.command("tap " + text) }

func (s *webScreen) relaunch() {
	s.command("reload")
	time.Sleep(time.Second) // the page goes; the next command waits for the new one
}

// chrome is Chrome's (or Chromium's) executable.
func chrome() string {
	if p := os.Getenv("ASUKA_CHROME"); p != "" {
		return p
	}
	for _, p := range []string{"/Applications/Google Chrome.app/Contents/MacOS/Google Chrome", "/Applications/Chromium.app/Contents/MacOS/Chromium"} {
		if _, err := os.Stat(p); err == nil {
			return p
		}
	}
	for _, n := range []string{"google-chrome", "google-chrome-stable", "chromium", "chromium-browser", "chrome"} {
		if p, err := exec.LookPath(n); err == nil {
			return p
		}
	}
	return ""
}

func webApp(t *testing.T, example string) *webScreen {
	browser := chrome()
	if browser == "" {
		t.Skip("no Chrome (ASUKA_CHROME)")
	}
	url := webServer(t, example)
	page := exec.Command(browser, "--headless=new", "--no-first-run", "--no-default-browser-check", "--disable-gpu",
		"--user-data-dir="+t.TempDir(), "--window-size=430,932", url)
	if err := page.Start(); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { page.Process.Kill(); page.Wait() })
	return &webScreen{t: t, url: url}
}

// webServer serves an app (appDir) with asuka dev --platform web, and
// returns its URL.
func webServer(t *testing.T, example string) string {
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	port := l.Addr().(*net.TCPAddr).Port
	l.Close()
	// The CLI built, not go run: killing it then stops the server it is.
	cli := filepath.Join(t.TempDir(), "asuka")
	build := exec.Command("go", "build", "-o", cli, "./cmd/asuka")
	build.Dir = root(t)
	if b, err := build.CombinedOutput(); err != nil {
		t.Fatalf("building the CLI: %v\n%s", err, b)
	}
	dev := exec.Command(cli, "dev", "--platform", "web", "--no-open", "--port", fmt.Sprint(port), appDir(example))
	dev.Dir = root(t)
	out, _ := dev.StdoutPipe()
	var logs strings.Builder
	dev.Stderr = &logs
	t.Cleanup(func() {
		if t.Failed() {
			t.Logf("asuka dev --platform web:\n%s", logs.String())
		}
	})
	if err := dev.Start(); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { dev.Process.Kill(); dev.Wait() })
	url := ""
	sc := bufio.NewScanner(out)
	for sc.Scan() {
		if m := regexp.MustCompile(`at (http://\S+/)`).FindStringSubmatch(sc.Text()); m != nil {
			url = m[1]
			break
		}
	}
	if url == "" {
		t.Fatal("asuka dev --platform web printed no URL")
	}
	go io.Copy(io.Discard, out)
	return url
}

func TestCounterWeb(t *testing.T) { counter(t, webApp(t, "counter")) }
func TestChatWeb(t *testing.T)    { chat(t, webApp(t, "chat")) }
func TestNotesWeb(t *testing.T)   { notes(t, webApp(t, "notes")) }

// TestBookmarksWeb: a bookmark added in the editor, in a collection made
// there, is written to SQLite and listed (a live query), and is still
// there after a reload.
func TestBookmarksWeb(t *testing.T) {
	s := webApp(t, "bookmarks")
	waitFor(t, s, []string{"Bookmarks"})
	title := fmt.Sprintf("e2e bookmark %d", time.Now().UnixNano())
	s.tap("Add")
	waitFor(t, s, []string{"Save", "Title"})
	s.command("type " + title)
	s.tap("https://")
	s.command("type https://example.com/e2e")
	s.tap("Unsorted")
	waitFor(t, s, []string{"Collection", "New collection"})
	s.tap("New collection")
	s.command("type E2E")
	s.command("submit")
	waitFor(t, s, []string{"E2E", "Save"}, "New collection")
	s.tap("Save")
	waitFor(t, s, []string{title}, "Save")
	s.relaunch()
	waitFor(t, s, []string{"Bookmarks", title})
}

// TestNavigationWeb also checks the Inbox tab's badge: its unread count
// goes down as a message is read.
func TestNavigationWeb(t *testing.T) {
	s := webApp(t, "navigation")
	waitFor(t, s, []string{"Inbox", "3"})
	navigation(t, s)
	waitFor(t, s, []string{"Inbox", "2"}, "3")
}

// TestRoutesWeb: the address bar shows the screen (a router's); a reload
// comes back to it, the inbox under it; the browser's back and forward
// go through the stack and the tabs; a link pasted opens its screens.
func TestRoutesWeb(t *testing.T) {
	s := webApp(t, "navigation")
	waitFor(t, s, []string{"Message 20"})
	waitLocation(t, s, "/inbox")
	s.tap("Message 20")
	waitFor(t, s, []string{"No note yet."}, "Message 19")
	waitLocation(t, s, "/inbox/20")
	s.relaunch()
	waitFor(t, s, []string{"Message 20", "No note yet."})
	s.command("history -1")
	waitFor(t, s, []string{"Message 19"}, "No note yet.")
	waitLocation(t, s, "/inbox")
	s.command("history 1")
	waitFor(t, s, []string{"No note yet."}, "Message 19")
	s.command("tab Settings")
	waitLocation(t, s, "/settings")
	s.command("history -1")
	waitFor(t, s, []string{"No note yet."}, "Notifications")
	s.command("visit /inbox/18/reply")
	time.Sleep(time.Second) // the page goes; the next command waits for the new one
	waitFor(t, s, []string{"To: Ada", "Covers the screen"})
	s.tap("Cancel")
	waitFor(t, s, []string{"Message 18", "No note yet."}, "To: Ada")
	waitLocation(t, s, "/inbox/18")
	s.command("back")
	waitFor(t, s, []string{"Message 19"}, "No note yet.")
	waitLocation(t, s, "/inbox")
}

// waitLocation waits for the page's address to be path.
func waitLocation(t *testing.T, s *webScreen, path string) {
	t.Helper()
	var got string
	for range 100 {
		if out, err := s.post("location"); err == nil {
			if got = strings.TrimSpace(out); got == path {
				return
			}
		}
		time.Sleep(100 * time.Millisecond)
	}
	t.Fatalf("the page's address is %q, not %q", got, path)
}

// TestToolbarWeb: the inbox's primary action, a floating button, composes a
// message; a message's bottom bar has its toolbar's buttons, which follow
// the app's state, one opening a menu, and its primary action.
func TestToolbarWeb(t *testing.T) {
	s := webApp(t, "navigation")
	waitFor(t, s, []string{"Message 20", "Compose"})
	s.tap("Compose")
	waitFor(t, s, []string{"Message 21"})
	s.tap("Message 21")
	waitFor(t, s, []string{"Delete", "Star", "Reply", "Mark as unread", "Compose"}, "Message 20")
	s.tap("Reply")
	waitFor(t, s, []string{"Forward"})
	s.tap("Forward")
	waitFor(t, s, []string{"Reply"}, "Forward")
	s.tap("Star") // the top bar's: the toolbar's follows it
	waitFor(t, s, []string{"Unstar"})
	s.command("back")
	waitFor(t, s, []string{"Message 20", "Compose"}, "Mark as unread")
}

// TestMenusWeb: the gallery's menu shows its section under its title,
// its submenu and its disabled item; choosing in the section moves its
// check, and the submenu opens beside its item.
func TestMenusWeb(t *testing.T) {
	s := webApp(t, "gallery")
	waitFor(t, s, []string{"Gallery"})
	s.tap("Gallery") // the drawer's button, on a narrow page
	waitFor(t, s, []string{"Symbols"})
	s.tap("Symbols")
	waitFor(t, s, []string{"Sorted by Name"})
	s.tap("More")
	waitFor(t, s, []string{"Sort by", "Name", "Date", "Move to", "Delete"})
	s.tap("Date")
	waitFor(t, s, []string{"Sorted by Date"}, "Sort by")
	s.tap("More")
	waitFor(t, s, []string{"Move to"})
	s.tap("Move to")
	waitFor(t, s, []string{"Inbox", "Archive"})
}

// The gallery's Picker: a <select> showing its label while none is
// chosen; choosing an item writes the app's signal.
func TestPickerWeb(t *testing.T) {
	s := webApp(t, "gallery")
	waitFor(t, s, []string{"Gallery"})
	s.tap("Gallery") // the drawer's button, on a narrow page
	waitFor(t, s, []string{"Controls"})
	s.tap("Controls")
	waitFor(t, s, []string{"Milk", "No milk chosen"})
	s.command("choose Oat")
	waitFor(t, s, []string{"Oat", "Oat milk"}, "No milk chosen")
}

// TestSnackbarWeb: the gallery's snackbar says what was done, at the
// bottom of the window; its Undo undoes it, and its close button and a
// new one take it away.
func TestSnackbarWeb(t *testing.T) {
	s := webApp(t, "gallery")
	waitFor(t, s, []string{"Gallery"})
	s.tap("Gallery") // the drawer's button, on a narrow page
	waitFor(t, s, []string{"Controls"})
	s.tap("Controls")
	waitFor(t, s, []string{"3 drafts", "Delete a draft"})
	s.tap("Delete a draft")
	waitFor(t, s, []string{"2 drafts", "Draft deleted", "Undo", "Close"})
	s.tap("Undo")
	waitFor(t, s, []string{"3 drafts"}, "Draft deleted")
	s.tap("Upload")
	waitFor(t, s, []string{"Uploading 3 photos…"})
	s.tap("Copy the link") // replaces it
	waitFor(t, s, []string{"Link copied"}, "Uploading 3 photos…")
	s.tap("Close")
	waitFor(t, s, []string{"Copy the link"}, "Link copied")
}
func TestLanguagesWeb(t *testing.T) { languages(t, webApp(t, "i18n")) }

// TestProgressRingWeb: the gallery's rings follow the volume (40%), read
// as progress bars with their percentage; the indeterminate one has none.
func TestProgressRingWeb(t *testing.T) {
	s := webApp(t, "gallery")
	waitFor(t, s, []string{"Gallery"})
	s.tap("Gallery") // the drawer's button, on a narrow page
	waitFor(t, s, []string{"Controls"})
	s.tap("Controls")
	waitFor(t, s, []string{"Wi-Fi", "progress 40%"})
	n := 0
	for _, x := range s.texts() {
		if x == "progress 40%" {
			n++
		}
	}
	if n != 2 {
		t.Fatalf("%d rings at 40%%, want 2: %q", n, s.texts())
	}
}

// Linux

// linuxScreen is an app asuka dev --platform linux runs on this
// machine's display (xvfb-run's, in CI), until it's stopped (which stops
// the app); the automation's commands go through a directory
// (ASUKA_COMMANDS), as on iOS.
type linuxScreen struct {
	t    *testing.T
	cli  string
	app  string
	dir  string
	dev  *exec.Cmd
	logs syncBuffer
}

// syncBuffer is asuka dev's output, written by its process's copiers.
type syncBuffer struct {
	mu sync.Mutex
	b  strings.Builder
}

func (b *syncBuffer) Write(p []byte) (int, error) {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.b.Write(p)
}

func (b *syncBuffer) String() string {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.b.String()
}

func (s *linuxScreen) start() {
	c := exec.Command(s.cli, "dev", "--platform", "linux", appDir(s.app))
	c.Dir = root(s.t)
	c.Env = append(os.Environ(), "ASUKA_COMMANDS="+s.dir)
	c.Stdout, c.Stderr = &s.logs, &s.logs
	// The app it runs has its output too: Wait doesn't wait for that.
	c.WaitDelay = 5 * time.Second
	if err := c.Start(); err != nil {
		s.t.Fatal(err)
	}
	s.dev = c
}

// stop stops asuka dev, which stops the app (SIGINT: it quits as it
// would).
func (s *linuxScreen) stop() {
	if s.dev == nil {
		return
	}
	s.dev.Process.Signal(syscall.SIGINT)
	done := make(chan struct{})
	go func() { s.dev.Wait(); close(done) }()
	select {
	case <-done:
	case <-time.After(30 * time.Second):
		s.dev.Process.Kill()
		<-done
	}
	s.dev = nil
}

func (s *linuxScreen) command(cmds string) {
	file := filepath.Join(s.dir, "asuka-commands.txt")
	// As on iOS: one the app hasn't read yet isn't written over (bounded:
	// an app just started may not read it yet).
	for range 500 {
		if _, err := os.Stat(file); os.IsNotExist(err) {
			break
		}
		time.Sleep(10 * time.Millisecond)
	}
	// Written whole, then moved: the app never reads half of it.
	if err := os.WriteFile(file+".part", []byte(cmds+"\n"), 0o644); err != nil {
		s.t.Fatal(err)
	}
	if err := os.Rename(file+".part", file); err != nil {
		s.t.Fatal(err)
	}
}

func (s *linuxScreen) texts() []string {
	dump := filepath.Join(s.dir, "asuka-dump.txt")
	os.Remove(dump)
	s.command("dump")
	for range 50 {
		if b, err := os.ReadFile(dump); err == nil {
			return strings.Split(strings.TrimSpace(string(b)), "\n")
		}
		time.Sleep(100 * time.Millisecond)
	}
	return nil
}

func (s *linuxScreen) tap(text string) { s.command("tap " + text) }

func (s *linuxScreen) relaunch() {
	s.stop()
	os.Remove(filepath.Join(s.dir, "asuka-commands.txt")) // for the run that ended
	s.start()
}

var linuxCLI struct {
	once sync.Once
	path string
	err  error
}

func linuxApp(t *testing.T, example string) *linuxScreen {
	if runtime.GOOS != "linux" {
		t.Skip("not Linux")
	}
	if os.Getenv("DISPLAY") == "" && os.Getenv("WAYLAND_DISPLAY") == "" {
		t.Skip("no display (xvfb-run -a dbus-run-session go test ...)")
	}
	// The CLI built once, not go run: stopping it then stops what it runs.
	linuxCLI.once.Do(func() {
		dir, err := os.MkdirTemp("", "asuka-e2e")
		if err != nil {
			linuxCLI.err = err
			return
		}
		linuxCLI.path = filepath.Join(dir, "asuka")
		build := exec.Command("go", "build", "-o", linuxCLI.path, "./cmd/asuka")
		build.Dir = root(t)
		if b, err := build.CombinedOutput(); err != nil {
			linuxCLI.err = fmt.Errorf("building the CLI: %v\n%s", err, b)
		}
	})
	if linuxCLI.err != nil {
		t.Fatal(linuxCLI.err)
	}
	s := &linuxScreen{t: t, cli: linuxCLI.path, app: example, dir: t.TempDir()}
	s.start()
	t.Cleanup(func() {
		s.stop()
		if t.Failed() {
			t.Logf("asuka dev --platform linux:\n%s", s.logs.String())
		}
	})
	return s
}

func TestCounterLinux(t *testing.T)    { counter(t, linuxApp(t, "counter")) }
func TestNotesLinux(t *testing.T)      { notes(t, linuxApp(t, "notes")) }
func TestChatLinux(t *testing.T)       { chat(t, linuxApp(t, "chat")) }
func TestNavigationLinux(t *testing.T) { navigation(t, linuxApp(t, "navigation")) }
func TestLanguagesLinux(t *testing.T)  { languages(t, linuxApp(t, "i18n")) }
