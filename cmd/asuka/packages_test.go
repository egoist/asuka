package main

import (
	"archive/tar"
	"bytes"
	"compress/gzip"
	"io"
	"os"
	"path/filepath"
	"slices"
	"strconv"
	"strings"
	"testing"
)

func TestPackagesFor(t *testing.T) {
	all := func(m map[string]bool) []string {
		var out []string
		for _, p := range append(append([]string{pkgZip, pkgDMG}, linuxPackages...), pkgInstaller, pkgMSIX) {
			if m[p] {
				out = append(out, p)
			}
		}
		return out
	}
	// None asked for: all of them.
	got, err := packagesFor(&buildOptions{}, nil, linuxPackages, "linux")
	if err != nil || !slices.Equal(all(got), linuxPackages) {
		t.Errorf("default: %v %v", got, err)
	}
	// asuka.toml's, then --package's over it; tgz is tar.gz.
	got, _ = packagesFor(&buildOptions{}, []string{"deb", "AppImage"}, linuxPackages, "linux")
	if !slices.Equal(all(got), []string{pkgDeb, pkgAppImage}) {
		t.Errorf("configured: %v", got)
	}
	got, _ = packagesFor(&buildOptions{packages: []string{"tgz", "rpm"}}, []string{"deb"}, linuxPackages, "linux")
	if !slices.Equal(all(got), []string{pkgTarGz, pkgRPM}) {
		t.Errorf("--package: %v", got)
	}
	if _, err := packagesFor(&buildOptions{}, []string{"snap"}, linuxPackages, "linux"); err == nil || !strings.Contains(err.Error(), `asuka.toml's linux.packages: "snap" isn't a linux package`) {
		t.Errorf("a package there's none of: %v", err)
	}
	if _, err := packagesFor(&buildOptions{packages: []string{"deb"}}, nil, macPackages, "mac"); err == nil || !strings.Contains(err.Error(), "--package") {
		t.Errorf("a Linux package for the Mac: %v", err)
	}
	// Windows: the zip and the installer, unless the MSIX is asked for
	// (setup and appx are their other names).
	got, _ = packagesFor(&buildOptions{}, nil, windowsPackages, "windows")
	if !slices.Equal(all(got), []string{pkgZip, pkgInstaller}) {
		t.Errorf("Windows' default: %v", got)
	}
	got, _ = packagesFor(&buildOptions{packages: []string{"setup", "appx"}}, nil, windowsPackages, "windows")
	if !slices.Equal(all(got), []string{pkgInstaller, pkgMSIX}) {
		t.Errorf("Windows' --package: %v", got)
	}

	// --package takes a list, or itself again.
	var o buildOptions
	fs := buildFlags(&o)
	if err := fs.parse([]string{"--package", "deb, rpm", "--package=appimage"}); err != nil {
		t.Fatal(err)
	}
	if !slices.Equal(o.packages, []string{"deb", "rpm", "appimage"}) {
		t.Errorf("--package: %v", o.packages)
	}
}

func TestLinuxDeps(t *testing.T) {
	app := linuxTestApp(t, "name = \"Notes\"\nid = \"com.example.notes\"\n[linux]\ndebDepends = [\"libfoo1 (>= 2)\"]\nrpmRequires = [\"foo >= 2\"]")
	// What the app's packages' manifests add.
	app.cfg.debDepends = []string{"libpoppler-glib8t64 | libpoppler-glib8", "libfoo1 (>= 2)"}
	app.cfg.rpmRecommends = []string{"pocketsphinx-models"}
	// What one package recommends and another needs, it depends on.
	app.cfg.debRecommends = []string{"libpoppler-glib8t64 | libpoppler-glib8", "geoclue-2.0"}
	deb, debRec := linuxDeps(app, "2.34", true)
	want := []string{"libc6 (>= 2.34)", "libgtk-4-1 (>= 4.14)", "libadwaita-1-0 (>= 1.5)", "libsoup-3.0-0", "libpoppler-glib8t64 | libpoppler-glib8", "libfoo1 (>= 2)"}
	if !slices.Equal(deb, want) || !slices.Equal(debRec, []string{"geoclue-2.0"}) {
		t.Errorf("the .deb's: %v %v", deb, debRec)
	}
	rpm, rpmRec := linuxDeps(app, "2.34", false)
	want = []string{"libc.so.6(GLIBC_2.34)(64bit)", "gtk4 >= 4.14", "libadwaita >= 1.5", "libsoup3", "foo >= 2"}
	if !slices.Equal(rpm, want) || !slices.Equal(rpmRec, []string{"pocketsphinx-models"}) {
		t.Errorf("the .rpm's: %v %v", rpm, rpmRec)
	}
}

func TestDebName(t *testing.T) {
	for exe, want := range map[string]string{"my-notes": "my-notes", "my_notes": "my-notes", "a": "a-app"} {
		if got := debName(exe); got != want {
			t.Errorf("%q: %q", exe, got)
		}
	}
}

// The .deb's and .rpm's files are the prefix's under /usr, without the
// update's file list; what they say of the app is asuka.toml's.
func TestLinuxPackage(t *testing.T) {
	app := linuxTestApp(t, "name = \"My Notes\"\nid = \"com.example.notes\"\nversion = \"1.2.0\"\n[linux]\nsummary = \"Write things down\"\ndeveloper = \"Example Inc.\"\ncategories = [\"Office\"]\nlicense = \"MIT\"")
	prefix := filepath.Join(t.TempDir(), "my-notes")
	os.MkdirAll(filepath.Join(prefix, "bin"), 0o755)
	os.WriteFile(filepath.Join(prefix, "bin", "my-notes"), []byte("exe"), 0o755)
	if err := writeLinuxShare(app, prefix, "my-notes"); err != nil {
		t.Fatal(err)
	}
	os.WriteFile(filepath.Join(prefix, "share", "my-notes", ".asuka-files"), []byte("bin/my-notes\n"), 0o644)
	p, err := linuxPackage(app, prefix, "my-notes", "arm64")
	if err != nil {
		t.Fatal(err)
	}
	if p.Name != "my-notes" || p.Version != "1.2.0" || p.Arch != "arm64" || p.Summary != "Write things down" ||
		p.Maintainer != "Example Inc." || p.Section != "editors" || p.License != "MIT" {
		t.Errorf("%+v", p)
	}
	var paths []string
	for _, f := range p.Files {
		paths = append(paths, f.Path)
	}
	for _, want := range []string{"/usr/bin/my-notes", "/usr/share/applications/com.example.notes.desktop", "/usr/share/metainfo/com.example.notes.metainfo.xml", "/usr/share/my-notes/assets/img/a.txt"} {
		if !slices.Contains(paths, want) {
			t.Errorf("no %s in %v", want, paths)
		}
	}
	if slices.Contains(paths, "/usr/share/my-notes/.asuka-files") {
		t.Error("the update's file list is in the package")
	}
	owns := rpmOwns("my-notes")
	if !owns("/usr/share/my-notes") || !owns("/usr/share/my-notes/assets") || owns("/usr/share") || owns("/usr/share/my-notes-other") {
		t.Error("the .rpm's directories")
	}
}

func TestReleaseAssets(t *testing.T) {
	app := linuxTestApp(t, "name = \"Notes\"\nversion = \"1.2.0\"")
	o := &buildOptions{out: t.TempDir()}
	dir := desktopDir(o, "linux", "amd64")
	os.MkdirAll(dir, 0o755)
	for _, ext := range []string{".tar.gz", ".deb", ".rpm", ".AppImage"} {
		os.WriteFile(releaseFile(app, o, "linux", "amd64", ext), nil, 0o644)
	}
	// A build of the .deb alone: the others a build before made go.
	removeUnmade(app, o, "linux", "amd64", map[string]string{pkgTarGz: ".tar.gz", pkgDeb: ".deb", pkgRPM: ".rpm", pkgAppImage: ".AppImage"}, map[string]bool{pkgDeb: true})
	got := releaseAssets(app, o, "linux", "amd64")
	if len(got) != 1 || filepath.Base(got[0]) != "notes-1.2.0-linux-amd64.deb" {
		t.Errorf("%v", got)
	}
	if fileExists(releaseFile(app, o, "linux", "amd64", ".tar.gz")) {
		t.Error("the tarball of a build before is left")
	}
	for name, want := range map[string]string{"a.deb": "application/vnd.debian.binary-package", "a.rpm": "application/x-rpm", "a.AppImage": "application/vnd.appimage", "a.dmg": "application/x-apple-diskimage", "a.tar.gz": "application/octet-stream"} {
		if got := assetType(name); got != want {
			t.Errorf("%s: %s", name, got)
		}
	}
}

// debControl reads a .deb's control file.
func debControl(t *testing.T, file string) string {
	t.Helper()
	b, err := os.ReadFile(file)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.HasPrefix(b, []byte("!<arch>\n")) {
		t.Fatalf("%s isn't an ar archive", file)
	}
	for p := 8; p+60 <= len(b); {
		name := strings.TrimSpace(string(b[p : p+16]))
		size, _ := strconv.Atoi(strings.TrimSpace(string(b[p+48 : p+58])))
		data := b[p+60 : p+60+size]
		p += 60 + size + size%2
		if name != "control.tar.gz" {
			continue
		}
		gz, err := gzip.NewReader(bytes.NewReader(data))
		if err != nil {
			t.Fatal(err)
		}
		tr := tar.NewReader(gz)
		for h, err := tr.Next(); err == nil; h, err = tr.Next() {
			if h.Name == "./control" {
				c, _ := io.ReadAll(tr)
				return string(c)
			}
		}
	}
	t.Fatalf("%s has no control file", file)
	return ""
}
