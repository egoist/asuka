package main

import (
	"archive/zip"
	"bytes"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
	"time"
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

func writeTestFile(t *testing.T, p, s string) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(p, []byte(s), 0o644); err != nil {
		t.Fatal(err)
	}
}
