package main

import (
	"archive/tar"
	"compress/gzip"
	"debug/elf"
	"encoding/json"
	"encoding/xml"
	"fmt"
	"image"
	"image/color"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"slices"
	"strings"
	"testing"
	"time"

	"github.com/egoist/asuka/internal/cbuild"
	"github.com/egoist/asuka/internal/prebuilt"
)

func linuxTestApp(t *testing.T, config string) *asukaApp {
	t.Helper()
	dir := filepath.Join(t.TempDir(), "My Notes")
	os.MkdirAll(filepath.Join(dir, "assets", "img"), 0o755)
	os.WriteFile(filepath.Join(dir, "assets", "img", "a.txt"), []byte("asset"), 0o644)
	if config != "" {
		os.WriteFile(filepath.Join(dir, "asuka.toml"), []byte(config), 0o644)
	}
	cfg, err := loadConfig(dir)
	if err != nil {
		t.Fatal(err)
	}
	return &asukaApp{dir: dir, name: filepath.Base(dir), bundleID: cfg.ID, cfg: cfg}
}

// The host's settings: the app's name, ID, version and build (and the
// accent, as every platform's).
func TestLinuxHostConfig(t *testing.T) {
	app := linuxTestApp(t, "name = \"Notes & Co\"\nid = \"com.example.notes\"\nversion = \"2.1\"\nbuild = 7\naccent = \"#ff8800\"")
	c := app.hostConfig(platformLinux, "linux-amd64", true)
	if c.Name != "Notes & Co" || c.ID != "com.example.notes" || c.Version != "2.1" || c.Build != "7" || c.Accent != "#ff8800" || c.MultipleInstances {
		t.Errorf("%+v", c)
	}
	multi := linuxTestApp(t, "name = \"Notes\"\nid = \"com.example.notes\"\nsingleInstance = false")
	if c := multi.hostConfig(platformLinux, "", false); !c.MultipleInstances {
		t.Error(`"singleInstance": false isn't in the settings`)
	}
}

func TestLinuxExe(t *testing.T) {
	for name, want := range map[string]string{
		"Counter": "counter", "My Notes!": "my-notes", "Notes 2 Go": "notes-2-go", "笔记": "notes", "a/b": "a-b",
	} {
		app := &asukaApp{bundleID: "com.example.Notes", cfg: &appConfig{Name: name}}
		if got := linuxExe(app); got != want {
			t.Errorf("%q: %q, want %q", name, got, want)
		}
	}
}

func TestGAppIDValid(t *testing.T) {
	for id, want := range map[string]bool{
		"com.example.notes": true, "dev.asuka.my-app": true, "dev.asuka.my_app": true,
		"notes": false, "com.example.2notes": false, "com..notes": false, "com.example.notes!": false,
	} {
		if gAppIDValid(id) != want {
			t.Errorf("%q: %v", id, !want)
		}
	}
}

func TestLinuxArch(t *testing.T) {
	if a, err := linuxArch("arm64"); a != "arm64" || err != nil {
		t.Error(a, err)
	}
	if a, err := linuxArch(""); a != "arm64" && a != "amd64" || err != nil {
		t.Error(a, err)
	}
	if _, err := linuxArch("386"); err == nil {
		t.Error("386: no error")
	}
}

// The prefix the build lays out: the assets where the host finds them
// from bin/<exe>, the icon at each size, the desktop file and the
// metainfo, named for the ID; and its .tar.gz.
func TestLinuxShare(t *testing.T) {
	app := linuxTestApp(t, `name = "My Notes"
id = "com.example.notes"
version = "1.2"
icon = "icon.png"

[linux]
summary = "Write things down"
description = "Notes, kept.\n\nOn <your> machine & synced."
categories = ["Office", "Utility"]
keywords = ["memo"]
license = "MIT"
developer = "Example Inc."
homepage = "https://example.com"`)
	img := image.NewNRGBA(image.Rect(0, 0, 64, 64))
	for i := range img.Pix {
		img.Pix[i] = 200
	}
	img.SetNRGBA(0, 0, color.NRGBA{255, 0, 0, 255})
	if err := writePNG(app.cfg.Icon, img); err != nil {
		t.Fatal(err)
	}
	out := filepath.Join(t.TempDir(), "my-notes")
	if err := writeLinuxShare(app, out, "my-notes"); err != nil {
		t.Fatal(err)
	}
	for _, p := range []string{
		"share/my-notes/assets/img/a.txt",
		"share/icons/hicolor/16x16/apps/com.example.notes.png",
		"share/icons/hicolor/512x512/apps/com.example.notes.png",
		"share/applications/com.example.notes.desktop",
		"share/metainfo/com.example.notes.metainfo.xml",
	} {
		if _, err := os.Stat(filepath.Join(out, p)); err != nil {
			t.Error(err)
		}
	}
	if icon, err := loadIcon(filepath.Join(out, "share/icons/hicolor/48x48/apps/com.example.notes.png")); err != nil || icon.Bounds().Dx() != 48 {
		t.Errorf("48x48: %v", err)
	}

	desktop, _ := os.ReadFile(filepath.Join(out, "share/applications/com.example.notes.desktop"))
	for _, want := range []string{
		"[Desktop Entry]\nType=Application\n", "\nName=My Notes\n", "\nComment=Write things down\n", "\nExec=my-notes\n",
		"\nIcon=com.example.notes\n", "\nCategories=Office;Utility;\n", "\nKeywords=memo;\n", "\nStartupWMClass=my-notes\n",
	} {
		if !strings.Contains(string(desktop), want) {
			t.Errorf("the desktop file has no %q:\n%s", want, desktop)
		}
	}

	meta, _ := os.ReadFile(filepath.Join(out, "share/metainfo/com.example.notes.metainfo.xml"))
	var c struct {
		Type       string   `xml:"type,attr"`
		ID         string   `xml:"id"`
		Name       string   `xml:"name"`
		Summary    string   `xml:"summary"`
		License    string   `xml:"project_license"`
		Developer  string   `xml:"developer>name"`
		Paragraphs []string `xml:"description>p"`
		Categories []string `xml:"categories>category"`
		Launchable string   `xml:"launchable"`
		Icon       string   `xml:"icon"`
		Release    struct {
			Version string `xml:"version,attr"`
		} `xml:"releases>release"`
	}
	if err := xml.Unmarshal(meta, &c); err != nil {
		t.Fatalf("%v:\n%s", err, meta)
	}
	if c.Type != "desktop-application" || c.ID != "com.example.notes" || c.Name != "My Notes" || c.Summary != "Write things down" ||
		c.License != "MIT" || c.Developer != "Example Inc." || c.Launchable != "com.example.notes.desktop" || c.Icon != "com.example.notes" ||
		c.Release.Version != "1.2" || len(c.Paragraphs) != 2 || c.Paragraphs[1] != "On <your> machine & synced." ||
		strings.Join(c.Categories, ",") != "Office,Utility" {
		t.Errorf("%+v\n%s", c, meta)
	}

	// The archive: under the folder's name, the executable executable.
	os.MkdirAll(filepath.Join(out, "bin"), 0o755)
	os.WriteFile(filepath.Join(out, "bin", "my-notes"), []byte("ELF"), 0o644)
	tgz := filepath.Join(t.TempDir(), "my-notes.tar.gz")
	if err := tarGz(out, tgz); err != nil {
		t.Fatal(err)
	}
	f, _ := os.Open(tgz)
	defer f.Close()
	gz, err := gzip.NewReader(f)
	if err != nil {
		t.Fatal(err)
	}
	tr := tar.NewReader(gz)
	modes := map[string]int64{}
	for {
		h, err := tr.Next()
		if err == io.EOF {
			break
		}
		if err != nil {
			t.Fatal(err)
		}
		modes[h.Name] = h.Mode
	}
	if modes["my-notes/bin/my-notes"] != 0o755 || modes["my-notes/share/my-notes/assets/img/a.txt"] != 0o644 || modes["my-notes/bin/"] != 0o755 {
		t.Errorf("%v", modes)
	}
}

// Without an icon, nothing names one; without linux's fields, the
// defaults.
func TestLinuxShareDefaults(t *testing.T) {
	app := linuxTestApp(t, "")
	if d := desktopEntry(app, "my-notes", false); strings.Contains(d, "Icon=") || !strings.Contains(d, "\nCategories=Utility;\n") || strings.Contains(d, "Comment=") {
		t.Error(d)
	}
	m := metainfo(app, false, time.Date(2026, 1, 2, 0, 0, 0, 0, time.UTC))
	if strings.Contains(m, "<icon") || !strings.Contains(m, "<summary>My Notes</summary>") || !strings.Contains(m, `date="2026-01-02"`) ||
		!strings.Contains(m, "<description>\n    <p>My Notes</p>\n  </description>") || !strings.Contains(m, "<category>Utility</category>") {
		t.Error(m)
	}
}

func TestFlatpakManifest(t *testing.T) {
	app := linuxTestApp(t, `id = "com.example.notes"`)
	m := flatpakManifest(app, "my-notes")
	for _, want := range []string{
		"\napp-id: com.example.notes\n", "\nruntime: org.gnome.Platform\n", "\nruntime-version: '" + flatpakRuntime + "'\n",
		"\nsdk: org.gnome.Sdk\n", "\ncommand: my-notes\n", "--socket=wayland", "cp -a bin share /app/", "\n        path: my-notes\n",
	} {
		if !strings.Contains(m, want) {
			t.Errorf("no %q in\n%s", want, m)
		}
	}
	app.cfg.Linux.FlatpakRuntime = "47"
	if !strings.Contains(flatpakManifest(app, "my-notes"), "runtime-version: '47'") {
		t.Error("linux.flatpakRuntime")
	}
}

// A Linux build is an install prefix: one executable in bin (the host,
// the core, the app compiled to C and, as it imports std/sqlite, SQLite,
// whose names it doesn't export: a system SQLite GTK loads never takes
// them), the app's settings in share/<exe>; the tarball and the Flatpak
// have them all.
func TestBuildLinux(t *testing.T) {
	needDesktopBuild(t, cbuild.Target{OS: "linux", Arch: "amd64"})
	useSQLiteCounter(t, "Count: ")
	app := sqliteTestApp(t)
	machines := map[string]elf.Machine{"amd64": elf.EM_X86_64, "arm64": elf.EM_AARCH64}
	for _, arch := range []string{"amd64", "arm64"} {
		if _, err := cbuild.ForTarget(cbuild.Target{OS: "linux", Arch: arch}); err != nil {
			t.Logf("not %s: %v", arch, err) // another architecture's needs Zig
			continue
		}
		out := t.TempDir()
		if err := buildLinux(app, &buildOptions{out: out, arch: arch}); err != nil {
			t.Fatal(err)
		}
		prefix := filepath.Join(out, "linux-"+arch, "sqliteapp")
		bin := filepath.Join(prefix, "bin", "sqliteapp")
		e, err := elf.Open(bin)
		if err != nil {
			t.Fatal(err)
		}
		if e.Machine != machines[arch] {
			t.Errorf("%s: the executable is for %v", arch, e.Machine)
		}
		// The core and SQLite are in the executable: no library of
		// Asuka's or SQLite beside it, GTK and the rest opened as it runs.
		libs, _ := e.ImportedLibraries()
		for _, l := range libs {
			if strings.Contains(l, "asuka") || strings.Contains(l, "sqlite") {
				t.Errorf("%s: the executable needs %s", arch, l)
			}
		}
		syms, _ := e.DynamicSymbols()
		for _, s := range syms {
			if s.Section != elf.SHN_UNDEF && (strings.HasPrefix(s.Name, "sqlite3_") || strings.HasPrefix(s.Name, "asq_")) {
				t.Errorf("%s: the executable exports %s", arch, s.Name)
			}
		}
		e.Close()
		checkSQLiteLinked(t, bin)
		if _, err := os.Stat(filepath.Join(prefix, "lib")); err == nil {
			t.Errorf("%s: a lib/ beside the executable, which has the core and SQLite", arch)
		}
		if b, err := os.ReadFile(filepath.Join(prefix, "share", "sqliteapp", "asuka-app.json")); err != nil || !strings.Contains(string(b), `"name": "sqliteapp"`) {
			t.Errorf("%s: the settings: %v\n%s", arch, err, b)
		}
		m, _ := os.ReadFile(filepath.Join(out, "linux-"+arch, app.bundleID+".yml"))
		if !strings.Contains(string(m), "cp -a bin share /app/") {
			t.Errorf("%s: the Flatpak leaves a directory out:\n%s", arch, m)
		}
		tgz, _ := filepath.Glob(filepath.Join(out, "linux-"+arch, "*.tar.gz"))
		if len(tgz) != 1 {
			t.Fatalf("%s: %v", arch, tgz)
		}
		f, _ := os.Open(tgz[0])
		gz, err := gzip.NewReader(f)
		if err != nil {
			t.Fatal(err)
		}
		var names []string
		tr := tar.NewReader(gz)
		for h, err := tr.Next(); err == nil; h, err = tr.Next() {
			names = append(names, h.Name)
		}
		f.Close()
		for _, want := range []string{"sqliteapp/bin/sqliteapp", "sqliteapp/share/sqliteapp/asuka-app.json"} {
			if !slices.Contains(names, want) {
				t.Errorf("%s: the tarball has %v, not %s", arch, names, want)
			}
		}
		// The packages to hand out beside it, each named as the tarball is.
		release := filepath.Join(out, "linux-"+arch, "sqliteapp-"+app.cfg.Version+"-linux-"+arch)
		c := debControl(t, release+".deb")
		for _, want := range []string{"Package: sqliteapp\n", "Architecture: " + arch + "\n", "Depends: libc6 (>= 2.", "libgtk-4-1 (>= 4.14), libadwaita-1-0 (>= 1.5)"} {
			if !strings.Contains(c, want) {
				t.Errorf("%s: the .deb's control file lacks %q:\n%s", arch, want, c)
			}
		}
		rpm, err := os.ReadFile(release + ".rpm")
		if err != nil || !strings.HasPrefix(string(rpm), "\xed\xab\xee\xdb") {
			t.Errorf("%s: the .rpm: %v", arch, err)
		}
		ai, err := os.Stat(release + ".AppImage")
		if err != nil || ai.Mode().Perm()&0o111 == 0 {
			t.Errorf("%s: the AppImage: %v", arch, err)
		}
		// --package makes some: the others a build before made go.
		if err := buildLinux(app, &buildOptions{out: out, arch: arch, packages: []string{"deb"}}); err != nil {
			t.Fatal(err)
		}
		for ext, want := range map[string]bool{".deb": true, ".rpm": false, ".AppImage": false, ".tar.gz": false} {
			if fileExists(release+ext) != want {
				t.Errorf("%s: with --package deb, %s there: %v", arch, ext, !want)
			}
		}
		if fileExists(filepath.Join(out, "linux-"+arch, app.bundleID+".yml")) {
			t.Errorf("%s: with --package deb, the Flatpak manifest", arch)
		}
	}
}

// The Linux libraries a release carries, built with Zig as it builds
// them (on a Mac, for glibc 2.28), are what an installed asuka (its
// sources its own, not a checkout's) links with the system's gcc or
// clang: it compiles none of the core or SQLite, and the app runs (with
// a display: xvfb-run -a dbus-run-session).
func TestLinuxPrebuiltFromZig(t *testing.T) {
	if runtime.GOOS != "linux" {
		t.Skip("builds with this Linux's compilers")
	}
	if _, err := cbuild.Zig(); err != nil {
		t.Skip(err)
	}
	needDesktopBuild(t, cbuild.Host())
	name := map[string]string{"amd64": "linux-x64", "arm64": "linux-arm64"}[runtime.GOARCH]
	var target *prebuiltTarget
	for i := range prebuiltTargets {
		if prebuiltTargets[i].name == name {
			target = &prebuiltTargets[i]
		}
	}
	if target == nil {
		t.Fatalf("no prebuilt target %q", name)
	}

	// The package, as the release builds it, put where a fetch puts it.
	version := fmt.Sprintf("0.0.0-test.%d", time.Now().UnixNano())
	pkg := prebuilt.Package{Asset: prebuilt.AssetName(name, version), Version: version, SHA256: strings.Repeat("0", 64)}
	dir := prebuilt.Dir(pkg)
	t.Cleanup(func() { os.RemoveAll(dir) })
	t.Setenv("ASUKA_CC", "zig")
	c := prebuiltContents{Name: name, Files: map[string]prebuilt.File{}}
	if err := target.buildC(dir, &c); err != nil {
		t.Fatal(err)
	}
	t.Setenv("ASUKA_CC", "")
	if c.Compiler != "zig" {
		t.Fatalf("built with %q", c.Compiler)
	}
	os.WriteFile(filepath.Join(dir, ".complete"), nil, 0o644)
	pkg.Compiler = c.Compiler
	m := prebuilt.Manifest{Packages: map[string]prebuilt.Package{name: pkg}, Archives: map[string]prebuilt.File{}}
	for k, f := range c.Files {
		f.Package = name
		m.Archives[k] = f
	}
	b, _ := json.Marshal(m)
	manifest := filepath.Join(t.TempDir(), "manifest.json")
	os.WriteFile(manifest, b, 0o644)

	// asuka as installed: outside the checkout, its sources its own.
	asuka := filepath.Join(t.TempDir(), "asuka")
	gobuild := exec.Command("go", "build", "-o", asuka, ".")
	gobuild.Env = append(os.Environ(), "CGO_ENABLED=0")
	if out, err := gobuild.CombinedOutput(); err != nil {
		t.Fatalf("go build: %v\n%s", err, out)
	}
	src, _ := os.ReadFile(filepath.Join("testdata", "sqliteapp", "app.asuka"))

	for _, cc := range []string{"gcc", "clang"} {
		t.Run(cc, func(t *testing.T) {
			real, err := exec.LookPath(cc)
			if err != nil {
				t.Skip(err)
			}
			// The compiler, logging what it's asked to do.
			log := filepath.Join(t.TempDir(), "cc.log")
			wrapper := filepath.Join(t.TempDir(), "cc")
			os.WriteFile(wrapper, []byte("#!/bin/sh\necho \"$@\" >> '"+log+"'\nexec '"+real+"' \"$@\"\n"), 0o755)
			app := filepath.Join(t.TempDir(), "sqliteapp")
			os.MkdirAll(app, 0o755)
			os.WriteFile(filepath.Join(app, "app.asuka"), src, 0o644)
			build := exec.Command(asuka, "build", "--platform", "linux", ".")
			build.Dir = app
			build.Env = append(os.Environ(), "CC="+wrapper, "ASUKA_CC=", "ASUKA_PREBUILT_MANIFEST="+manifest)
			if out, err := build.CombinedOutput(); err != nil {
				t.Fatalf("asuka build: %v\n%s", err, out)
			}
			calls, _ := os.ReadFile(log)
			linked := false
			for _, line := range strings.Split(string(calls), "\n") {
				if strings.Contains(line, "/core/src/") || strings.Contains(line, "sqlite3.c") {
					t.Errorf("compiled what the package has: %s", line)
				}
				if strings.Contains(line, dir) {
					linked = true
				}
			}
			if !linked {
				t.Fatalf("no link took the package's libraries:\n%s", calls)
			}
			bin := filepath.Join(app, "dist", "linux-"+runtime.GOARCH, "sqliteapp", "bin", "sqliteapp")
			checkSQLiteLinked(t, bin)
			if os.Getenv("DISPLAY") == "" {
				return
			}
			snap := filepath.Join(t.TempDir(), "end.png")
			run := exec.Command(bin)
			run.Env = append(os.Environ(), "ASUKA_DUMP=1", "ASUKA_SNAPSHOT="+snap, "ASUKA_DATA="+t.TempDir())
			if b, err := run.CombinedOutput(); err != nil {
				t.Fatalf("%v\n%s", err, b)
			}
			if b, _ := os.ReadFile(snap + ".txt"); !strings.Contains(string(b), `"runs"`) {
				t.Errorf("the app shows:\n%s", b)
			}
		})
	}
}
