package main

import (
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"slices"
	"strings"
	"testing"

	"github.com/egoist/asuka/internal/cbuild"
)

// A Mac app's signature carries the hardened runtime's entitlements for
// what its purpose strings ask for, and none it doesn't need.
func TestMacEntitlements(t *testing.T) {
	cfg := &appConfig{}
	importStd(t, cfg, "macos", "camera", "contacts", "location")
	app := &asukaApp{cfg: cfg}
	f, err := writeMacEntitlements(app, t.TempDir())
	if err != nil || f == "" {
		t.Fatalf("entitlements: %q, %v", f, err)
	}
	b, _ := os.ReadFile(f)
	s := string(b)
	for _, want := range []string{"com.apple.security.device.camera", "com.apple.security.personal-information.addressbook",
		"com.apple.security.personal-information.location"} {
		if !strings.Contains(s, want) {
			t.Errorf("no %s in\n%s", want, s)
		}
	}
	for _, not := range []string{"calendars", "aps-environment"} {
		if strings.Contains(s, not) {
			t.Errorf("%s in\n%s", not, s)
		}
	}
	if f, err := writeMacEntitlements(&asukaApp{cfg: &appConfig{}}, t.TempDir()); f != "" || err != nil {
		t.Errorf("an app that asks for nothing: %q, %v", f, err)
	}
}

// The libraries in Frameworks are signed before the app, as it is (the
// hardened runtime loads only its own team's), without its entitlements.
func TestSignMacFrameworks(t *testing.T) {
	bundle := filepath.Join(t.TempDir(), "Notes.app")
	lib := filepath.Join(bundle, "Contents", "Frameworks", "libswift_Concurrency.dylib")
	os.MkdirAll(filepath.Dir(lib), 0o755)
	os.WriteFile(lib, []byte("lib"), 0o755)
	for _, id := range []string{"-", "Developer ID Application: X (T)"} {
		var calls []string
		run := func(dir string, env []string, cmd string, args ...string) error {
			calls = append(calls, cmd+" "+strings.Join(args, " "))
			return nil
		}
		if err := signMac(bundle, id, "ents.plist", run); err != nil {
			t.Fatal(err)
		}
		opts := ""
		if id != "-" {
			opts = " --options runtime --timestamp"
		}
		want := []string{
			"codesign --force --sign " + id + opts + " " + lib,
			"codesign --force --sign " + id + opts + " --entitlements ents.plist " + bundle,
			"codesign --verify --strict " + bundle,
		}
		if !slices.Equal(calls, want) {
			t.Errorf("%s:\n%s\nwant\n%s", id, strings.Join(calls, "\n"), strings.Join(want, "\n"))
		}
	}
}

// A Mac build is an .app for one architecture (Apple silicon's, unless
// --arch amd64: Intel's): its executable the Swift host linked with the
// core with the app compiled in and, as it imports std/sqlite, SQLite,
// signed, the app's settings in its Resources. It runs: the window shows
// the Counter, which counts a press.
func TestBuildMac(t *testing.T) {
	needMacBuild(t)
	useSQLiteCounter(t, "Count: ")
	app := sqliteTestApp(t)
	out := t.TempDir()
	if err := buildMac(app, &buildOptions{out: out}); err != nil {
		t.Fatal(err)
	}
	bundle := filepath.Join(out, "darwin-arm64", app.displayName()+".app")
	bin := filepath.Join(bundle, "Contents", "MacOS", sanitizeExe(app.displayName()))
	// What to hand out: the app zipped, and a disk image of it, their
	// names each build's own.
	if _, err := os.Stat(filepath.Join(out, "darwin-arm64", "sqliteapp-"+app.cfg.Version+"-darwin-arm64.zip")); err != nil {
		t.Error(err)
	}
	checkDMG(t, filepath.Join(out, "darwin-arm64", "sqliteapp-"+app.cfg.Version+"-darwin-arm64.dmg"), app.displayName()+".app")
	if archs, err := exec.Command("lipo", "-archs", bin).Output(); err != nil || strings.TrimSpace(string(archs)) != "arm64" {
		t.Errorf("the executable: %q %v, want arm64's alone", archs, err)
	}
	// Intel's: another build, its own updates.
	intel := t.TempDir()
	if err := buildMac(app, &buildOptions{out: intel, arch: "amd64"}); err != nil {
		t.Fatal(err)
	}
	ibin := filepath.Join(intel, "darwin-amd64", app.displayName()+".app", "Contents", "MacOS", sanitizeExe(app.displayName()))
	if _, err := os.Stat(filepath.Join(intel, "darwin-amd64", "sqliteapp-"+app.cfg.Version+"-darwin-amd64.zip")); err != nil {
		t.Error(err)
	}
	if archs, err := exec.Command("lipo", "-archs", ibin).Output(); err != nil || strings.TrimSpace(string(archs)) != "x86_64" {
		t.Errorf("--arch amd64's executable: %q %v", archs, err)
	}
	// No Go, no core beside it: the core is linked in.
	if _, err := os.Stat(filepath.Join(bundle, "Contents", "Frameworks", "libasuka.dylib")); err == nil {
		t.Error("a libasuka.dylib in Frameworks")
	}
	if out, _ := exec.Command("nm", "-arch", "arm64", bin).Output(); !containsField(string(out), "_asuka_run") || strings.Contains(string(out), "runtime.gopanic") {
		t.Error("the executable isn't the Swift host with the core linked in")
	}
	checkSQLiteLinked(t, bin)
	for _, f := range []string{"Resources/asuka-app.json", "Info.plist"} {
		if _, err := os.Stat(filepath.Join(bundle, "Contents", f)); err != nil {
			t.Error(err)
		}
	}
	if b, err := exec.Command("codesign", "--verify", "--deep", "--strict", bundle).CombinedOutput(); err != nil {
		t.Errorf("codesign --verify --deep: %v\n%s", err, b)
	}
	snap := filepath.Join(t.TempDir(), "c.png")
	c := exec.Command(bin)
	c.Env = append(os.Environ(), "ASUKA_SCRIPT=tap:Add", "ASUKA_SNAPSHOT="+snap, "ASUKA_DUMP=1")
	if b, err := c.CombinedOutput(); err != nil {
		t.Fatalf("%v\n%s", err, b)
	}
	dump, err := os.ReadFile(snap + ".txt")
	if err != nil || !strings.Contains(string(dump), `"Count: 1"`) || !strings.Contains(string(dump), `window "sqliteapp"`) {
		t.Fatalf("after tapping Add, the window shows (%v):\n%s", err, dump)
	}
}

// checkDMG checks a build's disk image: it verifies, and opens to the
// app (its signature intact), the Applications folder to drag it to and
// the window's layout.
func checkDMG(t *testing.T, dmg, app string) {
	t.Helper()
	if b, err := exec.Command("hdiutil", "verify", dmg).CombinedOutput(); err != nil {
		t.Fatalf("hdiutil verify: %v\n%s", err, b)
	}
	if b, _ := exec.Command("hdiutil", "imageinfo", dmg).CombinedOutput(); !strings.Contains(string(b), "ULFO") {
		t.Errorf("the image isn't LZFSE's:\n%s", b)
	}
	vol := t.TempDir()
	if b, err := exec.Command("hdiutil", "attach", dmg, "-nobrowse", "-readonly", "-noautoopen", "-mountpoint", vol).CombinedOutput(); err != nil {
		t.Fatalf("hdiutil attach: %v\n%s", err, b)
	}
	defer exec.Command("hdiutil", "detach", "-force", vol).Run()
	if l, err := os.Readlink(filepath.Join(vol, "Applications")); err != nil || l != "/Applications" {
		t.Errorf("Applications: %q %v", l, err)
	}
	if b, err := exec.Command("codesign", "--verify", "--deep", "--strict", filepath.Join(vol, app)).CombinedOutput(); err != nil {
		t.Errorf("the app in the image: %v\n%s", err, b)
	}
	if _, err := os.Stat(filepath.Join(vol, ".DS_Store")); err != nil {
		t.Error(err)
	}
	// The volume's icon is the app's, where it has one.
	_, icon := os.Stat(filepath.Join(vol, app, "Contents", "Resources", "AppIcon.icns"))
	if _, err := os.Stat(filepath.Join(vol, ".VolumeIcon.icns")); (err == nil) != (icon == nil) {
		t.Errorf("the volume's icon: %v, the app's: %v", err, icon)
	}
	if _, err := os.Stat(filepath.Join(vol, ".fseventsd")); err == nil {
		t.Error("the image has .fseventsd")
	}
}

// A Mac app made of Asuka (the compiler's) that imports a standard
// library package with native code: the package's Swift is built into
// the app, and the host makes the package's objects there (not in Go):
// the network it shows says what carries it, which only the Swift
// (NWPathMonitor) knows; without it, the package says "other".
func TestBuildMacStd(t *testing.T) {
	needMacBuild(t)
	if _, err := cbuild.ForTarget(cbuild.Target{OS: "darwin", Arch: "amd64"}); err != nil {
		t.Skip(err)
	}
	dir := filepath.Join(t.TempDir(), "netapp")
	os.MkdirAll(dir, 0o755)
	os.WriteFile(filepath.Join(dir, "asuka.toml"), []byte("name = \"netapp\"\nid = \"dev.asuka.netapp\"\n"), 0o644)
	os.WriteFile(filepath.Join(dir, "app.asuka"), []byte(`import platform/apps
import platform/network
import platform/recent

component Net {
  let net = network.status()
  state asked = "recent: asking"

  state first = "first: asking"

  task {
    let n = (await recent.list()).count()
    asked = "recent: {n >= 0}"
  }

  task {
    do {
      let found = try await apps.search(try await apps.list(), "finder")
      first = "first: {found.first()?.name ?? "none"}"
    } catch e {
      first = "first: {e.message}"
    }
  }

  Column {
    Text(if net.connected { "online over {net.type}" } else { "offline" })
    Text(asked)
    Text(first)
  }
}

app { Net() }
`), 0o644)
	app := projectApp(t, dir, nil)
	out := t.TempDir()
	if err := buildMac(app, &buildOptions{out: out}); err != nil {
		t.Fatal(err)
	}
	bundle := filepath.Join(out, "darwin-"+runtime.GOARCH, app.displayName()+".app")
	snap := filepath.Join(t.TempDir(), "n.png")
	c := exec.Command(filepath.Join(bundle, "Contents", "MacOS", sanitizeExe(app.displayName())))
	c.Env = append(os.Environ(), "ASUKA_SCRIPT=until:first: F;until:recent: t", "ASUKA_SNAPSHOT="+snap, "ASUKA_DUMP=1")
	if b, err := c.CombinedOutput(); err != nil {
		t.Fatalf("%v\n%s", err, b)
	}
	dump, err := os.ReadFile(snap + ".txt")
	s := string(dump)
	if !strings.Contains(s, `"first: Finder"`) {
		t.Errorf("platform/apps didn't find Finder through the host:\n%s", s)
	}
	if !strings.Contains(s, `"recent: true"`) {
		t.Errorf("the host's recent module didn't answer (platform/host's call):\n%s", s)
	}
	if err != nil || !(strings.Contains(s, `"online over wifi"`) || strings.Contains(s, `"online over ethernet"`) || strings.Contains(s, `"online over cellular"`) || strings.Contains(s, `"offline"`)) {
		t.Fatalf("the window shows (%v):\n%s", err, dump)
	}
}
