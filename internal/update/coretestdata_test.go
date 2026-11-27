package update

import (
	"archive/tar"
	"bytes"
	"compress/gzip"
	"crypto/ed25519"
	"crypto/sha256"
	"encoding/hex"
	"flag"
	"math/rand/v2"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

var updateCore = flag.Bool("update-core", false, "rewrite core/test/testdata/update, the C core's tests' archives, deltas and patches")

// TestCoreTestdata writes what core/test/update_test.c reads, made by the
// code `asuka build` makes updates with (WriteArchive, WriteDelta, Diff,
// Sign): the C core must read what the CLI writes. Run it with
// -update-core after changing a format.
func TestCoreTestdata(t *testing.T) {
	if !*updateCore {
		t.Skip("-update-core writes core/test/testdata/update")
	}
	out := filepath.Join("..", "..", "core", "test", "testdata", "update")
	if err := os.RemoveAll(out); err != nil {
		t.Fatal(err)
	}
	must := func(err error) {
		t.Helper()
		if err != nil {
			t.Fatal(err)
		}
	}
	must(os.MkdirAll(out, 0o755))
	r := rand.New(rand.NewPCG(1, 2))

	// A patch of a binary that changed as code does: some bytes moved,
	// some new, most the same.
	old := make([]byte, 60<<10)
	for i := range old {
		old[i] = byte(r.IntN(16) * 3)
	}
	var nw []byte
	nw = append(nw, old[:20000]...)
	nw = append(nw, []byte("new code here, new code here")...)
	for i := 20000; i < 40000; i++ {
		b := old[i]
		if i%97 == 0 {
			b++
		}
		nw = append(nw, b)
	}
	nw = append(nw, old[45000:]...)
	must(os.WriteFile(filepath.Join(out, "old.bin"), old, 0o644))
	must(os.WriteFile(filepath.Join(out, "new.bin"), nw, 0o644))
	must(os.WriteFile(filepath.Join(out, "new.patch"), Diff(old, nw), 0o644))

	// Two versions of a Windows-like folder: a file kept, one changed (a
	// patch), one moved, one new, one gone, a link, a long name (PAX), an
	// executable, an empty directory, and data that doesn't compress
	// (DEFLATE's stored blocks).
	tree := func(dir string, files map[string]string, links map[string]string) {
		for name, content := range files {
			p := filepath.Join(dir, filepath.FromSlash(name))
			must(os.MkdirAll(filepath.Dir(p), 0o755))
			mode := os.FileMode(0o644)
			if strings.HasSuffix(name, "tool") {
				mode = 0o755
			}
			must(os.WriteFile(p, []byte(content), mode))
		}
		for name, target := range links {
			must(os.Symlink(target, filepath.Join(dir, filepath.FromSlash(name))))
		}
		must(os.MkdirAll(filepath.Join(dir, "empty"), 0o755))
	}
	noise := make([]byte, 80<<10)
	for i := range noise {
		noise[i] = byte(r.Uint32())
	}
	long := "assets/" + strings.Repeat("a-rather-long-name-", 8) + ".txt"
	exe := string(old)
	work := t.TempDir()
	oldDir, newDir := filepath.Join(work, "old"), filepath.Join(work, "new")
	tree(oldDir, map[string]string{
		"Notes.exe":        exe,
		"assets/same.txt":  strings.Repeat("the same, line after line\n", 200),
		"assets/moved.txt": "moved from assets to data\n",
		"assets/gone.txt":  "only in the old version\n",
		"assets/noise.bin": string(noise),
		"bin/tool":         "#!/bin/sh\necho old\n",
		long:               "long",
	}, map[string]string{"assets/current": "same.txt"})
	tree(newDir, map[string]string{
		"Notes.exe":        string(nw),
		"assets/same.txt":  strings.Repeat("the same, line after line\n", 200),
		"data/moved.txt":   "moved from assets to data\n",
		"assets/new.txt":   "only in the new version\n",
		"assets/noise.bin": string(noise),
		"bin/tool":         "#!/bin/sh\necho new\n",
		long:               "longer",
	}, map[string]string{"assets/current": "new.txt", "bin/up": "../assets"})
	stamp := time.Date(2026, 11, 16, 0, 0, 0, 0, time.UTC)
	for _, d := range []string{oldDir, newDir} {
		filepath.Walk(d, func(p string, info os.FileInfo, err error) error {
			if err == nil && info.Mode()&os.ModeSymlink == 0 {
				os.Chtimes(p, stamp, stamp)
			}
			return nil
		})
	}
	entries := func(dir string) []string {
		es, err := os.ReadDir(dir)
		must(err)
		var names []string
		for _, e := range es {
			names = append(names, e.Name())
		}
		return names
	}
	var archive bytes.Buffer
	must(WriteArchive(&archive, oldDir, entries(oldDir)))
	must(os.WriteFile(filepath.Join(out, "old.tar.gz"), archive.Bytes(), 0o644))
	archive.Reset()
	must(WriteArchive(&archive, newDir, entries(newDir)))
	must(os.WriteFile(filepath.Join(out, "new.tar.gz"), archive.Bytes(), 0o644))
	var delta bytes.Buffer
	must(WriteDelta(&delta, "1.0.0", "1.1.0", oldDir, newDir, nil))
	must(os.WriteFile(filepath.Join(out, "old-to-new.delta"), delta.Bytes(), 0o644))

	// Two versions of an AppImage, and the delta that patches them with
	// their blocks decompressed (blocks.go): the core compresses the
	// blocks it makes again, to the new AppImage's bytes.
	oldImage, newImage := testAppImages(t, out)
	delta.Reset()
	must(WriteFileDelta(&delta, "1.0.0", "1.1.0", oldImage, newImage))
	if readIndex(t, delta.Bytes()).Entries[0].Blocks != blocksZlib8 {
		t.Fatal("the AppImage's delta isn't of its blocks")
	}
	must(os.WriteFile(filepath.Join(out, "appimage.delta"), delta.Bytes(), 0o644))

	// Archives an updater must refuse, written by hand: a path out of the
	// app, a link out of it, and a link that leads a later entry out
	// through another link (p -> ., then p/q -> .., then p/q/x).
	evil := func(name string, hdrs ...*tar.Header) {
		var b bytes.Buffer
		gz := gzip.NewWriter(&b)
		tw := tar.NewWriter(gz)
		for _, h := range hdrs {
			if h.Typeflag == tar.TypeReg {
				h.Size = 4
			}
			must(tw.WriteHeader(h))
			if h.Typeflag == tar.TypeReg {
				_, err := tw.Write([]byte("evil"))
				must(err)
			}
		}
		must(tw.Close())
		must(gz.Close())
		must(os.WriteFile(filepath.Join(out, name), b.Bytes(), 0o644))
	}
	evil("evil-path.tar.gz", &tar.Header{Name: "../evil", Typeflag: tar.TypeReg, Mode: 0o644})
	evil("evil-link.tar.gz", &tar.Header{Name: "a/b", Typeflag: tar.TypeSymlink, Linkname: "../../x", Mode: 0o777})
	evil("evil-through.tar.gz",
		&tar.Header{Name: "p", Typeflag: tar.TypeSymlink, Linkname: ".", Mode: 0o777},
		&tar.Header{Name: "p/q", Typeflag: tar.TypeSymlink, Linkname: "..", Mode: 0o777},
		&tar.Header{Name: "p/q/x", Typeflag: tar.TypeReg, Mode: 0o644})
	evil("evil-fifo.tar.gz", &tar.Header{Name: "fifo", Typeflag: tar.TypeFifo, Mode: 0o644})

	// The key `asuka update keygen` would have made, and the archive's
	// signature as `asuka build` signs it (of its SHA-256).
	seed := sha256.Sum256([]byte("asuka update test key"))
	key := ed25519.NewKeyFromSeed(seed[:])
	sum := sha256.Sum256(archive.Bytes())
	sig, err := hex.DecodeString(hex.EncodeToString(ed25519.Sign(key, sum[:])))
	must(err)
	if Sign(key, sum[:]) == "" || Verify(key.Public().(ed25519.PublicKey), sum[:], Sign(key, sum[:])) != nil {
		t.Fatal("Sign doesn't verify")
	}
	must(os.WriteFile(filepath.Join(out, "key.txt"), []byte(hex.EncodeToString(seed[:])+"\n"+hex.EncodeToString(key[32:])+"\n"+hex.EncodeToString(sig)+"\n"), 0o644))
}
