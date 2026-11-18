package main

import (
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
	"time"
)

// asuka dev's error overlay on the simulator, as TestDevWebErrorOverlay
// on the web: an error the app didn't catch shows over it with the lines
// where it was thrown, and Dismiss goes back to the app as it was; a
// build that fails shows until it builds again; Reload starts the app
// again.
func TestDevIOSErrorOverlay(t *testing.T) {
	if runtime.GOOS != "darwin" || testing.Short() {
		t.Skip("the simulator, on a Mac (not -short)")
	}
	sim := bootedSimulator()
	if sim == "" {
		t.Skip("no booted simulator")
	}
	tc, err := newIOSToolchain(false)
	if err != nil {
		t.Skip(err)
	}
	dir := counterDir(t)
	os.WriteFile(filepath.Join(dir, "app.asuka"), []byte(boomApp), 0o644)
	app := projectApp(t, dir, nil)
	loop, err := newDevLoop(app, "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer loop.close()
	p := simulatorPlayer(app, tc, t.TempDir(), &simDevice{UDID: sim}, loop)
	if err := p.launch(); err != nil {
		t.Fatal(err)
	}
	defer p.stop()
	if r := nextReload(t, loop); !r.first {
		t.Fatalf("the first report: %+v", r)
	}
	automate := iosDevAutomation(t, sim, app.bundleID)
	dump := func() string { return automate("dump") }
	automate("tap Add\ntap Boom")
	shows(t, dump, "Uncaught error", "boom at 1", "> 12 |   throw Error(\"boom at {n}\")", "explode (app.asuka:12)", "Dismiss", "Reload")
	automate("tap Dismiss")
	if got := dump(); strings.Contains(got, "Uncaught error") || !strings.Contains(got, "n: 1") {
		t.Fatalf("after Dismiss, the player shows:\n%s", got)
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

// iosDevAutomation drives the player on the simulator through the iOS
// host's automation (platform/ios's Automation.swift): its commands, a
// line each, written to its data container's tmp and posted; with dump,
// what it wrote.
func iosDevAutomation(t *testing.T, sim, bundle string) func(commands string) string {
	t.Helper()
	out, err := exec.Command("xcrun", "simctl", "get_app_container", sim, bundle, "data").Output()
	if err != nil {
		t.Fatalf("the player's data container: %v", err)
	}
	tmp := filepath.Join(strings.TrimSpace(string(out)), "tmp")
	file := filepath.Join(tmp, "asuka-commands.txt")
	dumped := filepath.Join(tmp, "asuka-dump.txt")
	return func(commands string) string {
		t.Helper()
		// The player removes the file as it reads it: one not read yet
		// isn't written over (a hang guard, bounded).
		for range 500 {
			if _, err := os.Stat(file); os.IsNotExist(err) {
				break
			}
			time.Sleep(10 * time.Millisecond)
		}
		dump := strings.HasSuffix(commands, "dump")
		if dump {
			os.Remove(dumped)
		}
		if err := os.WriteFile(file, []byte(commands+"\n"), 0o644); err != nil {
			t.Fatal(err)
		}
		if b, err := exec.Command("xcrun", "simctl", "spawn", sim, "notifyutil", "-p", "dev.asuka.automation").CombinedOutput(); err != nil {
			t.Fatalf("notifyutil: %v\n%s", err, b)
		}
		if !dump {
			return ""
		}
		for range 200 {
			if b, err := os.ReadFile(dumped); err == nil {
				return string(b)
			}
			time.Sleep(50 * time.Millisecond)
		}
		t.Fatal("the player wrote no dump")
		return ""
	}
}
