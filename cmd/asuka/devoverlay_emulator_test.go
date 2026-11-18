package main

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// asuka dev's error overlay on an Android emulator or device, as
// TestDevWebErrorOverlay on the web: an error the app didn't catch shows
// over it with the lines where it was thrown, and Dismiss (or back) goes
// back to the app as it was; a build that fails shows until it builds
// again; Reload starts the app again.
func TestDevAndroidErrorOverlay(t *testing.T) {
	if testing.Short() {
		t.Skip("an emulator (not -short)")
	}
	tc, err := newAndroidToolchain(false)
	if err != nil {
		t.Skip(err)
	}
	b, _ := exec.Command(tc.adb, "devices").Output()
	serial := ""
	for _, l := range strings.Split(string(b), "\n")[1:] {
		if f := strings.Fields(l); len(f) == 2 && f[1] == "device" {
			serial = f[0]
			break
		}
	}
	if serial == "" {
		t.Skip("no device or emulator (adb devices)")
	}
	dir := counterDir(t)
	os.WriteFile(filepath.Join(dir, "app.asuka"), []byte(boomApp), 0o644)
	app := projectApp(t, dir, nil)
	loop, err := newDevLoop(app, "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer loop.close()
	p, err := androidPlayer(app, tc, t.TempDir(), serial, loop)
	if err != nil {
		t.Fatal(err)
	}
	if err := p.launch(); err != nil {
		t.Fatal(err)
	}
	defer p.stop()
	if r := nextReload(t, loop); !r.first {
		t.Fatalf("the first report: %+v", r)
	}
	automate := androidDevAutomation(t, tc.adb, serial, androidPackage(app.bundleID))
	dump := func() string { return automate("dump") }
	automate("tap Add\ntap Boom")
	shows(t, dump, "Uncaught error", "boom at 1", "> 12 |   throw Error(\"boom at {n}\")", "explode (app.asuka:12)", "Dismiss", "Reload")
	automate("tap Dismiss")
	if got := dump(); strings.Contains(got, "Uncaught error") || !strings.Contains(got, "n: 1") {
		t.Fatalf("after Dismiss, the player shows:\n%s", got)
	}
	// Back is Dismiss.
	automate("tap Boom")
	shows(t, dump, "Uncaught error")
	automate("back")
	if got := dump(); strings.Contains(got, "Uncaught error") || !strings.Contains(got, "n: 1") {
		t.Fatalf("after back, the player shows:\n%s", got)
	}
	// A save that doesn't build: the overlay says why, over the app.
	os.WriteFile(filepath.Join(dir, "app.asuka"), []byte(strings.Replace(boomApp, "n += 1", "n += \"x\"", 1)), 0o644)
	if loop.compile() {
		t.Fatal("it compiled")
	}
	shows(t, dump, "Build failed", "app.asuka:6:32: += doesn't take an int and a string", "> 6 |", "^^^")
	// Fixed: the overlay goes, the app reloads.
	os.WriteFile(filepath.Join(dir, "app.asuka"), []byte(boomApp), 0o644)
	if !loop.compile() {
		t.Fatal("didn't compile")
	}
	nextReload(t, loop)
	if got := dump(); strings.Contains(got, "Build failed") || !strings.Contains(got, "n: 1") {
		t.Fatalf("after the fix, the player shows:\n%s", got)
	}
	// Reload: the app starts again, from its initial state.
	automate("tap Boom")
	shows(t, dump, "Uncaught error", "boom at 1")
	automate("tap Reload")
	if r := nextReload(t, loop); !r.restarted {
		t.Fatalf("Reload: %+v", r)
	}
	if got := dump(); strings.Contains(got, "Uncaught error") || !strings.Contains(got, "n: 0") {
		t.Fatalf("after Reload, the player shows:\n%s", got)
	}
}

// androidDevAutomation drives the player through the Android host's
// automation (platform/android's Automation.kt): its commands, a line
// each, broadcast; with dump, what it wrote in the app's files.
func androidDevAutomation(t *testing.T, adb, serial, pkg string) func(commands string) string {
	t.Helper()
	run := func(args ...string) (string, error) {
		out, err := exec.Command(adb, append([]string{"-s", serial}, args...)...).CombinedOutput()
		return string(out), err
	}
	return func(commands string) string {
		t.Helper()
		dump := strings.HasSuffix(commands, "dump")
		if dump {
			run("shell", "run-as", pkg, "rm", "-f", "files/asuka-dump.txt")
		}
		// adb shell joins its arguments into a command line: quoted once more.
		q := "'" + strings.ReplaceAll(commands, "'", `'\''`) + "'"
		if out, err := run("shell", "am", "broadcast", "-a", "dev.asuka.automation", "-p", pkg, "--es", "commands", q); err != nil {
			t.Fatalf("broadcast %q: %v\n%s", commands, err, out)
		}
		if !dump {
			return ""
		}
		for range 100 {
			// Written whole, then moved, ending in a newline.
			if out, err := run("exec-out", "run-as", pkg, "cat", "files/asuka-dump.txt"); err == nil && strings.HasSuffix(out, "\n") {
				return out
			}
			time.Sleep(100 * time.Millisecond)
		}
		t.Fatal("the player wrote no dump")
		return ""
	}
}
