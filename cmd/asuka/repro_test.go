package main

import (
	"archive/zip"
	"bytes"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"testing"
	"time"

	"github.com/egoist/asuka/internal/appbuild"
	"github.com/egoist/asuka/internal/cbuild"
	iupdate "github.com/egoist/asuka/internal/update"
)

// A build is dated by its sources (repro.go): SOURCE_DATE_EPOCH, else the
// app's last commit, else its newest file; never by when it ran.
func TestSourceDate(t *testing.T) {
	dir := t.TempDir()
	app := filepath.Join(dir, "app")
	writeTestFile(t, filepath.Join(app, "app.asuka"), "app { Text(\"a\") }\n")
	newest := time.Date(2025, 3, 4, 5, 6, 7, 0, time.UTC)
	os.Chtimes(filepath.Join(app, "app.asuka"), newest, newest)
	// Not its build's output, nor what's hidden.
	writeTestFile(t, filepath.Join(app, "dist", "x"), "x")
	writeTestFile(t, filepath.Join(app, ".cache", "x"), "x")
	if got, ok := sourceDate(app); !ok || !got.Equal(newest) {
		t.Errorf("without git: %v, %v; want %v", got, ok, newest)
	}
	if _, err := exec.LookPath("git"); err != nil {
		return
	}
	git := func(args ...string) {
		cmd := exec.Command("git", args...)
		cmd.Dir = dir
		cmd.Env = append(os.Environ(), "GIT_AUTHOR_DATE=2026-01-02T03:04:05Z", "GIT_COMMITTER_DATE=2026-01-02T03:04:05Z",
			"GIT_AUTHOR_NAME=a", "GIT_AUTHOR_EMAIL=a@example.com", "GIT_COMMITTER_NAME=a", "GIT_COMMITTER_EMAIL=a@example.com")
		if out, err := cmd.CombinedOutput(); err != nil {
			t.Fatalf("git %v: %v\n%s", args, err, out)
		}
	}
	git("init", "-q")
	git("add", ".")
	git("commit", "-q", "-m", "x")
	if got, ok := sourceDate(app); !ok || !got.Equal(time.Date(2026, 1, 2, 3, 4, 5, 0, time.UTC)) {
		t.Errorf("in a repository: %v, %v; want its commit's time", got, ok)
	}
	t.Setenv("SOURCE_DATE_EPOCH", "1700000000")
	setSourceDateEpoch(app)
	if v := os.Getenv("SOURCE_DATE_EPOCH"); v != "1700000000" {
		t.Errorf("SOURCE_DATE_EPOCH set: replaced with %s", v)
	}
}

// What a build archives (a Linux build's .tar.gz, a Windows build's
// .zip) is the same from one build to the next: its entries dated by
// SOURCE_DATE_EPOCH, not by when their files were written.
func TestArchivesReproducible(t *testing.T) {
	t.Setenv("SOURCE_DATE_EPOCH", "1700000000")
	build := func(at time.Time) (tgz, zipped []byte) {
		out := filepath.Join(t.TempDir(), "notes")
		writeTestFile(t, filepath.Join(out, "bin", "notes"), "ELF")
		writeTestFile(t, filepath.Join(out, "share", "notes", "a.txt"), "a")
		filepath.Walk(out, func(p string, _ os.FileInfo, _ error) error { return os.Chtimes(p, at, at) })
		tf := filepath.Join(t.TempDir(), "notes.tar.gz")
		zf := filepath.Join(t.TempDir(), "notes.zip")
		if err := tarGz(out, tf); err != nil {
			t.Fatal(err)
		}
		if err := zipDir(out, zf); err != nil {
			t.Fatal(err)
		}
		a, _ := os.ReadFile(tf)
		b, _ := os.ReadFile(zf)
		return a, b
	}
	t1, z1 := build(time.Date(2026, 1, 1, 0, 0, 0, 0, time.UTC))
	t2, z2 := build(time.Date(2026, 6, 1, 12, 0, 0, 0, time.UTC))
	if !bytes.Equal(t1, t2) {
		t.Error("two .tar.gz of the same files differ")
	}
	if !bytes.Equal(z1, z2) {
		t.Error("two .zip of the same files differ")
	}
	r, err := zip.NewReader(bytes.NewReader(z1), int64(len(z1)))
	if err != nil {
		t.Fatal(err)
	}
	for _, f := range r.File {
		if !f.Modified.Equal(time.Unix(1700000000, 0)) {
			t.Errorf("%s dated %v", f.Name, f.Modified)
		}
	}
}

// A Linux release is the same bytes from one build to the next, from
// another directory too: its executable, its .tar.gz, .deb, .rpm and
// AppImage. The next version's AppImage delta (what an app run from the
// AppImage downloads) is about as small as its executable's own patch,
// though its code moved what follows the change: it's of the AppImages'
// blocks decompressed. Two builds of e2e/testdata/updater once made two
// programs (a map's order), and e2e's TestUpdaterLinuxAppImage then
// downloaded most of the AppImage.
func TestLinuxReleaseReproducible(t *testing.T) {
	arch := runtime.GOARCH
	needDesktopBuild(t, cbuild.Target{OS: "linux", Arch: arch})
	// As a process of its own runs asuka build: commands other tests ran
	// in this one (asuka dev, asuka test --gui) leave a development
	// build's settings, whose program names the files of the directory it
	// was built in and isn't optimized.
	oldDebug, oldNoOpt := appbuild.Debug, appbuild.DevNoOptimize
	appbuild.DevNoOptimize = false
	t.Cleanup(func() { appbuild.Debug, appbuild.DevNoOptimize = oldDebug, oldNoOpt })
	t.Setenv("SOURCE_DATE_EPOCH", "1700000000")
	t.Setenv("ASUKA_UPDATE_URL", "")
	t.Setenv("ASUKA_UPDATE_PUBLIC_KEY", "")
	src := filepath.Join("..", "..", "e2e", "testdata", "updater")
	build := func(version, change string) string {
		t.Helper()
		dir := filepath.Join(t.TempDir(), "updater")
		if err := os.CopyFS(dir, os.DirFS(src)); err != nil {
			t.Fatal(err)
		}
		if change != "" {
			p := filepath.Join(dir, "updater.asuka")
			b, _ := os.ReadFile(p)
			if !bytes.Contains(b, []byte(`"up to date"`)) {
				t.Fatal("updater.asuka doesn't report \"up to date\"")
			}
			os.WriteFile(p, bytes.Replace(b, []byte(`"up to date"`), []byte(change), 1), 0o644)
		}
		out := t.TempDir()
		if err := cmdBuild([]string{"--platform", "linux", "--arch", arch, "--version", version, "-o", out, dir}); err != nil {
			t.Fatal(err)
		}
		return filepath.Join(out, "linux-"+arch)
	}
	a, b := build("1.0.0", ""), build("1.0.0", "")
	exe := filepath.Join("updater", "bin", "updater")
	for _, name := range []string{exe, "updater-1.0.0-linux-" + arch + ".tar.gz", "updater-1.0.0-linux-" + arch + ".deb", "updater-1.0.0-linux-" + arch + ".rpm", "updater-1.0.0-linux-" + arch + ".AppImage"} {
		x, err := os.ReadFile(filepath.Join(a, name))
		if err != nil {
			t.Fatal(err)
		}
		if y, _ := os.ReadFile(filepath.Join(b, name)); !bytes.Equal(x, y) {
			t.Errorf("two builds of the app made two %s", name)
		}
	}

	c := build("1.0.1", `"up to date, as of this version"`)
	oldImage := filepath.Join(a, "updater-1.0.0-linux-"+arch+".AppImage")
	newImage := filepath.Join(c, "updater-1.0.1-linux-"+arch+".AppImage")
	var delta bytes.Buffer
	if err := iupdate.WriteFileDelta(&delta, "1.0.0", "1.0.1", oldImage, newImage); err != nil {
		t.Fatal(err)
	}
	oldExe, _ := os.ReadFile(filepath.Join(a, exe))
	newExe, _ := os.ReadFile(filepath.Join(c, exe))
	if bytes.Equal(oldExe, newExe) {
		t.Fatal("the change didn't change the executable")
	}
	image, _ := os.ReadFile(newImage)
	patch := len(iupdate.Diff(oldExe, newExe))
	t.Logf("the AppImage's delta: %d bytes, of an AppImage of %d; the executable's patch %d", delta.Len(), len(image), patch)
	if delta.Len() > 2*patch+16<<10 || delta.Len()*10 > len(image) {
		t.Errorf("the AppImage's delta is %d bytes, of an AppImage of %d: the executable's own patch is %d", delta.Len(), len(image), patch)
	}
}

func writeTestFile(t *testing.T, p, s string) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(p, []byte(s), 0o644); err != nil {
		t.Fatal(err)
	}
}
