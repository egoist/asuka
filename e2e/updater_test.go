//go:build e2e

package e2e

import (
	"bytes"
	"crypto/ed25519"
	"encoding/base64"
	"net/http"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/egoist/asuka/internal/distpkg"
	"github.com/egoist/asuka/internal/gpgsign"
)

// updateFeed is a feed of the test's own (httptest), the keys its updates
// are signed with, and the requests it served.
type updateFeed struct {
	dir  string // its files
	env  []string
	mu   sync.Mutex
	gets []string
	sent int64 // the bytes of the files it served
}

// countingWriter counts what a response's body sends.
type countingWriter struct {
	http.ResponseWriter
	n  *int64
	mu *sync.Mutex
}

func (w countingWriter) Write(b []byte) (int, error) {
	n, err := w.ResponseWriter.Write(b)
	w.mu.Lock()
	*w.n += int64(n)
	w.mu.Unlock()
	return n, err
}

func newUpdateFeed(t *testing.T) *updateFeed {
	pub, priv, _ := ed25519.GenerateKey(nil)
	f := &updateFeed{dir: t.TempDir()}
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		f.mu.Lock()
		f.gets = append(f.gets, r.URL.Path)
		f.mu.Unlock()
		http.FileServer(http.Dir(f.dir)).ServeHTTP(countingWriter{w, &f.sent, &f.mu}, r)
	}))
	t.Cleanup(srv.Close)
	f.env = append(os.Environ(),
		"ASUKA_UPDATE_PUBLIC_KEY="+base64.StdEncoding.EncodeToString(pub),
		"ASUKA_UPDATE_PRIVATE_KEY="+base64.StdEncoding.EncodeToString(priv),
		"ASUKA_UPDATE_URL="+srv.URL,
	)
	return f
}

// build builds e2e/testdata/updater at version for platform with asuka
// build, and publishes its update files on the feed, the manifest last.
// It returns the build's directory.
func (f *updateFeed) build(t *testing.T, platform, version string) string {
	return f.buildApp(t, platform, version, "e2e/testdata/updater")
}

// buildApp is build of the app in dir (a copy of e2e/testdata/updater,
// changed).
func (f *updateFeed) buildApp(t *testing.T, platform, version, dir string) string {
	out := filepath.Join(t.TempDir(), "dist")
	c := exec.Command("go", "run", "./cmd/asuka", "build", "--platform", platform, "--version", version, "-o", out, dir)
	c.Dir, c.Env = root(t), f.env
	b, err := c.CombinedOutput()
	if err != nil {
		t.Fatalf("asuka build %s: %v\n%s", version, err, b)
	}
	t.Logf("asuka build %s:\n%s", version, b)
	files, _ := filepath.Glob(filepath.Join(out, "updates", "*"))
	var manifests []string
	for _, p := range files {
		if strings.HasSuffix(p, ".json") {
			manifests = append(manifests, p)
			continue
		}
		run(t, "cp", p, f.dir)
	}
	for _, p := range manifests {
		run(t, "cp", p, f.dir)
	}
	return out
}

// downloaded returns the requests the feed served.
func (f *updateFeed) downloaded() string {
	f.mu.Lock()
	defer f.mu.Unlock()
	return strings.Join(f.gets, " ")
}

// served is how many bytes of files the feed has served.
func (f *updateFeed) served() int64 {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.sent
}

// runUpdater runs the installed app's executable in its self-update mode
// (a "selfupdate" file in its directory, ASUKA_DATA), waits for it to
// quit (it relaunches), then for the relaunched one's report, and
// returns the reports. Every desktop host has the update module
// (DESIGN.md, "Updates"). args are its arguments, env more of its
// environment.
func runUpdater(t *testing.T, exe string, args []string, env ...string) string {
	data := t.TempDir()
	if err := os.WriteFile(filepath.Join(data, "selfupdate"), nil, 0o644); err != nil {
		t.Fatal(err)
	}
	report := filepath.Join(data, "report")
	// Its output to a file: the update's helper, which outlives it, would
	// keep a pipe open.
	outFile := filepath.Join(t.TempDir(), "out")
	logf, err := os.Create(outFile)
	if err != nil {
		t.Fatal(err)
	}
	defer logf.Close()
	app := exec.Command(exe, args...)
	app.Env = append(append(os.Environ(), "ASUKA_DATA="+data), env...)
	app.Stdout, app.Stderr = logf, logf
	if err := app.Start(); err != nil {
		t.Fatal(err)
	}
	exited := make(chan error, 1)
	go func() { exited <- app.Wait() }()
	select { // it quits as it relaunches
	case err := <-exited:
		if err != nil {
			out, _ := os.ReadFile(outFile)
			t.Fatalf("1.0.0: %v\n%s", err, out)
		}
	case <-time.After(within):
		app.Process.Kill()
		out, _ := os.ReadFile(outFile)
		b, _ := os.ReadFile(report)
		t.Fatalf("1.0.0 didn't quit; it reported:\n%s\nits output:\n%s", b, out)
	}
	deadline := time.Now().Add(within)
	var got string
	for time.Now().Before(deadline) {
		b, _ := os.ReadFile(report)
		got = string(b)
		if strings.Contains(got, "launched 1.0.1") && strings.Contains(got, "up to date") {
			break
		}
		time.Sleep(100 * time.Millisecond)
	}
	t.Logf("the app reported:\n%s", got)
	for _, want := range []string{"launched 1.0.0", "found 1.0.1", "installed 1.0.1", "launched 1.0.1", "up to date"} {
		if !strings.Contains(got, want) {
			out, _ := os.ReadFile(outFile)
			t.Fatalf("the app didn't report %q:\n%s\n1.0.0's output:\n%s", want, got, out)
		}
	}
	return got
}

// runOnce runs the app at exe as runUpdater does, where it can't update
// itself: it reports why and quits. It returns its report.
func runOnce(t *testing.T, exe string) string {
	data := t.TempDir()
	if err := os.WriteFile(filepath.Join(data, "selfupdate"), nil, 0o644); err != nil {
		t.Fatal(err)
	}
	app := exec.Command(exe)
	app.Env = append(os.Environ(), "ASUKA_DATA="+data)
	done := make(chan []byte, 1)
	go func() {
		out, _ := app.CombinedOutput()
		done <- out
	}()
	select {
	case <-done:
	case <-time.After(within):
		app.Process.Kill()
		t.Fatalf("%s didn't quit", exe)
	}
	b, _ := os.ReadFile(filepath.Join(data, "report"))
	return string(b)
}

// TestUpdaterMac builds e2e/testdata/updater at 1.0.0 and at 1.0.1 with
// asuka build, signing the updates with a key of the test's and publishing
// them on a feed of its own (httptest); installs 1.0.0 as a download is,
// from its disk image to an Applications folder (run from the image, it
// can't update itself: a read-only copy), and runs it, which checks,
// downloads the delta from 1.0.0, installs and relaunches: the bundle is
// then 1.0.1's, its code signature valid, and the relaunched app says
// it's 1.0.1.
func TestUpdaterMac(t *testing.T) {
	if runtime.GOOS != "darwin" {
		t.Skip("macOS")
	}
	f := newUpdateFeed(t)
	dmg := filepath.Join(f.build(t, "mac", "1.0.0"), "darwin-arm64", "updater-1.0.0-darwin-arm64.dmg")
	f.build(t, "mac", "1.0.1") // its delta from the published 1.0.0

	vol := t.TempDir()
	run(t, "hdiutil", "attach", dmg, "-nobrowse", "-readonly", "-noautoopen", "-mountpoint", vol)
	defer exec.Command("hdiutil", "detach", "-force", vol).Run()
	if got := runOnce(t, filepath.Join(vol, "Updater.app", "Contents", "MacOS", "Updater")); !strings.Contains(got, "read-only copy") {
		t.Errorf("run from its disk image, the app reported:\n%s", got)
	}
	apps := t.TempDir()
	installed := filepath.Join(apps, "Updater.app")
	run(t, "ditto", filepath.Join(vol, "Updater.app"), installed)
	run(t, "hdiutil", "detach", vol)
	runUpdater(t, filepath.Join(installed, "Contents", "MacOS", "Updater"), nil)

	if v := strings.TrimSpace(run(t, "/usr/libexec/PlistBuddy", "-c", "Print :CFBundleShortVersionString", filepath.Join(installed, "Contents", "Info.plist"))); v != "1.0.1" {
		t.Errorf("the installed bundle is %s", v)
	}
	run(t, "codesign", "--verify", "--deep", "--strict", installed)
	if got := f.downloaded(); !strings.Contains(got, "-1.0.0-to-1.0.1-darwin-arm64.delta") || strings.Contains(got, "updater-1.0.1-darwin-arm64.tar.gz") {
		t.Errorf("downloaded %v, want the delta alone", got)
	}
	// Nothing left beside the app: the helper removed its work.
	if left, _ := filepath.Glob(filepath.Join(apps, ".*")); len(left) > 0 {
		t.Errorf("left %v", left)
	}
}

// TestUpdaterLinux does the same with the Linux build: its tarball
// unpacked into a prefix that holds another program too, as ~/.local
// does; 1.0.1 replaces the app's files alone, from the delta, and the
// relaunched app (once 1.0.0 has quit: it runs once) says it's 1.0.1.
//
//	xvfb-run -a dbus-run-session go test -tags e2e ./e2e -run TestUpdaterLinux
func TestUpdaterLinux(t *testing.T) {
	if runtime.GOOS != "linux" {
		t.Skip("Linux")
	}
	f := newUpdateFeed(t)
	arch := runtime.GOARCH
	v100 := f.build(t, "linux", "1.0.0")
	f.build(t, "linux", "1.0.1")

	prefix := t.TempDir()
	os.MkdirAll(filepath.Join(prefix, "bin"), 0o755)
	os.WriteFile(filepath.Join(prefix, "bin", "other"), []byte("another program"), 0o755)
	run(t, "tar", "-xzf", filepath.Join(v100, "linux-"+arch, "updater-1.0.0-linux-"+arch+".tar.gz"), "--strip-components=1", "-C", prefix)
	runUpdater(t, filepath.Join(prefix, "bin", "updater"), nil)

	if b, _ := os.ReadFile(filepath.Join(prefix, "bin", "other")); string(b) != "another program" {
		t.Error("the update touched another program of the prefix")
	}
	list, _ := os.ReadFile(filepath.Join(prefix, "share", "updater", ".asuka-files"))
	if !strings.Contains(string(list), "bin/updater\n") || !strings.Contains(string(list), "share/applications/dev.asuka.updatertest.desktop\n") {
		t.Errorf("the prefix's file list:\n%s", list)
	}
	if got := f.downloaded(); !strings.Contains(got, "-1.0.0-to-1.0.1-linux-"+arch+".delta") || strings.Contains(got, "updater-1.0.1-linux-"+arch+".tar.gz") {
		t.Errorf("downloaded %v, want the delta alone", got)
	}
	if left, _ := filepath.Glob(filepath.Join(prefix, "share", "updater", ".asuka-update-*")); len(left) > 0 {
		t.Errorf("left %v", left)
	}
}

// TestUpdaterLinuxAppImage does it with the Linux build's AppImage (1.0.1's
// code changed), run from a folder of the user's as ~/Applications is:
// 1.0.0 downloads
// the delta of 1.0.1's AppImage from its own (the manifest's appimage,
// signed), makes 1.0.1's AppImage of its file with it, which takes the
// file's place, and the relaunched AppImage says it's 1.0.1; far fewer
// bytes come down than the AppImage has. Each way the runtime runs it:
// extracted (--appimage-extract-and-run, where there's no FUSE, as in a
// container), and mounted, where FUSE is. Both AppImages are signed with
// the build's OpenPGP key, as appimagetool --sign signs one.
//
//	xvfb-run -a dbus-run-session go test -tags e2e ./e2e -run TestUpdaterLinuxAppImage
func TestUpdaterLinuxAppImage(t *testing.T) {
	if runtime.GOOS != "linux" {
		t.Skip("Linux")
	}
	f := newUpdateFeed(t)
	// The AppImages signed with a key of the test's.
	keys := t.TempDir()
	c := exec.Command("go", "run", "./cmd/asuka", "linux", "keygen", "--uid", "Updater <dev@example.com>", "--out", keys)
	c.Dir = root(t)
	if b, err := c.CombinedOutput(); err != nil {
		t.Fatalf("asuka linux keygen: %v\n%s", err, b)
	}
	f.env = append(f.env, "ASUKA_LINUX_GPG_KEY="+filepath.Join(keys, "asuka-linux.key"))
	pub, err := os.ReadFile(filepath.Join(keys, "asuka-linux.asc"))
	if err != nil {
		t.Fatal(err)
	}
	arch := runtime.GOARCH
	v100 := f.build(t, "linux", "1.0.0")
	// 1.0.1's code changed, as a new version's does: what follows the
	// change in its executable moves.
	changed := filepath.Join(t.TempDir(), "updater")
	if err := os.CopyFS(changed, os.DirFS(filepath.Join(root(t), "e2e", "testdata", "updater"))); err != nil {
		t.Fatal(err)
	}
	src, _ := os.ReadFile(filepath.Join(changed, "updater.asuka"))
	if !bytes.Contains(src, []byte(`"up to date"`)) {
		t.Fatal(`updater.asuka doesn't report "up to date"`)
	}
	os.WriteFile(filepath.Join(changed, "updater.asuka"), bytes.Replace(src, []byte(`"up to date"`), []byte(`"up to date, as of this version"`), 1), 0o644)
	v101 := f.buildApp(t, "linux", "1.0.1", changed)
	name := "updater-1.0.0-linux-" + arch + ".AppImage"
	want, err := os.ReadFile(filepath.Join(v101, "linux-"+arch, "updater-1.0.1-linux-"+arch+".AppImage"))
	if err != nil {
		t.Fatal(err)
	}
	for _, v := range []string{"1.0.0", "1.0.1"} {
		dir := map[string]string{"1.0.0": v100, "1.0.1": v101}[v]
		b, err := os.ReadFile(filepath.Join(dir, "linux-"+arch, "updater-"+v+"-linux-"+arch+".AppImage"))
		if err != nil {
			t.Fatal(err)
		}
		digest, sig, key, err := distpkg.AppImageSignature(b)
		if err != nil || len(sig) == 0 || !bytes.Equal(bytes.TrimSpace(key), bytes.TrimSpace(pub)) {
			t.Fatalf("%s's AppImage isn't signed with the key: %v", v, err)
		}
		if err := gpgsign.Verify(pub, []byte(digest), sig); err != nil {
			t.Errorf("%s's AppImage's signature: %v", v, err)
		}
	}
	// AppImageUpdate's information, in the runtime's .upd_info: the feed's
	// .zsync, beside the manifest.
	out, _ := exec.Command(filepath.Join(v100, "linux-"+arch, name), "--appimage-updateinformation").Output()
	if got := strings.TrimSpace(string(out)); !strings.HasPrefix(got, "zsync|http://") || !strings.HasSuffix(got, "/updater-linux-"+arch+".AppImage.zsync") {
		t.Errorf("the update information: %q", got)
	}
	if _, err := os.Stat(filepath.Join(f.dir, "updater-linux-"+arch+".AppImage.zsync")); err != nil {
		t.Error(err)
	}
	// Extracted, by the flag (which the runtime keeps from the app): the
	// relaunch must ask for it again. The runtime extracts into TMPDIR,
	// and with NO_CLEANUP leaves each launch's there.
	type mode struct {
		name      string
		args      []string
		extracted int
	}
	modes := []mode{{"extracted", []string{"--appimage-extract-and-run"}, 2}}
	if _, err := os.Stat("/dev/fuse"); err == nil && (lookPath("fusermount3") || lookPath("fusermount")) {
		modes = append(modes, mode{"mounted", nil, 0})
	} else {
		t.Log("no FUSE here (/dev/fuse, fusermount3): the AppImage runs extracted only")
	}
	for _, m := range modes {
		t.Run(m.name, func(t *testing.T) {
			apps, tmp := t.TempDir(), t.TempDir()
			os.WriteFile(filepath.Join(apps, "another.AppImage"), []byte("another app"), 0o755)
			file := filepath.Join(apps, "Updater.AppImage") // named as the user likes
			run(t, "cp", filepath.Join(v100, "linux-"+arch, name), file)
			before, sent := f.downloaded(), f.served()
			runUpdater(t, file, m.args, "TMPDIR="+tmp, "NO_CLEANUP=1")
			if got, _ := filepath.Glob(filepath.Join(tmp, "appimage_extracted_*")); len(got) != m.extracted {
				t.Errorf("extracted %d times, want %d: %v", len(got), m.extracted, got)
			}
			got, _ := os.ReadFile(file)
			if !bytes.Equal(got, want) {
				t.Errorf("the AppImage isn't 1.0.1's (%d bytes, want %d)", len(got), len(want))
			}
			if st, _ := os.Stat(file); st == nil || st.Mode().Perm()&0o111 == 0 {
				t.Error("the new AppImage isn't executable")
			}
			if got := strings.TrimPrefix(f.downloaded(), before); !strings.Contains(got, "updater-1.0.0-to-1.0.1-linux-"+arch+".AppImage.delta") || strings.Contains(got, "updater-1.0.1-linux-"+arch+".AppImage ") || strings.HasSuffix(got, "updater-1.0.1-linux-"+arch+".AppImage") || strings.Contains(got, ".tar.gz") {
				t.Errorf("downloaded %v, want the AppImage's delta alone", got)
			}
			// The delta is of the AppImages' blocks decompressed: about
			// as large as the executable's own patch, though its code
			// moved what follows the change.
			if n := f.served() - sent; n*20 > int64(len(want)) {
				t.Errorf("downloaded %d bytes, of an AppImage of %d: the delta should be far smaller", n, len(want))
			} else {
				t.Logf("downloaded %d bytes for an AppImage of %d", n, len(want))
			}
			// Nothing left beside it, and the other AppImage untouched.
			if left, _ := filepath.Glob(filepath.Join(apps, ".*")); len(left) > 0 {
				t.Errorf("left %v", left)
			}
			if b, _ := os.ReadFile(filepath.Join(apps, "another.AppImage")); string(b) != "another app" {
				t.Error("the update touched another AppImage")
			}
		})
	}
}

func lookPath(name string) bool {
	_, err := exec.LookPath(name)
	return err == nil
}
