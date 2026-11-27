package update

import (
	"bytes"
	"encoding/binary"
	"io/fs"
	"math/rand/v2"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/egoist/asuka/internal/distpkg"
)

// testRuntime is an ELF as the AppImage runtime is to WriteAppImage: AI
// and 2 in its identification, and nothing after its section headers
// (the null section and .shstrtab).
func testRuntime() []byte {
	le := binary.LittleEndian
	shstr := []byte("\x00.shstrtab\x00")
	hdr := make([]byte, 64)
	copy(hdr, []byte{0x7f, 'E', 'L', 'F', 2, 1, 1, 0, 'A', 'I', 2})
	le.PutUint16(hdr[16:], 2)  // an executable
	le.PutUint16(hdr[18:], 62) // x86-64
	le.PutUint32(hdr[20:], 1)
	le.PutUint64(hdr[40:], uint64(64+len(shstr)))
	le.PutUint16(hdr[52:], 64)
	le.PutUint16(hdr[58:], 64)
	le.PutUint16(hdr[60:], 2)
	le.PutUint16(hdr[62:], 1)
	b := append(hdr, shstr...)
	b = append(b, make([]byte, 64)...) // the null section
	s := make([]byte, 64)
	le.PutUint32(s[0:], 1)
	le.PutUint32(s[4:], 3) // STRTAB
	le.PutUint64(s[24:], 64)
	le.PutUint64(s[32:], uint64(len(shstr)))
	le.PutUint64(s[48:], 1)
	return append(b, s...)
}

// testAppImage is an AppImage of files (an AppDir's, by path) at file.
func testAppImage(t testing.TB, file string, files map[string]string) []byte {
	t.Helper()
	var fl []distpkg.File
	for p, content := range files {
		mode := fs.FileMode(0o644)
		if strings.HasPrefix(p, "/usr/bin/") || p == "/AppRun" {
			mode = 0o755
		}
		fl = append(fl, distpkg.File{Path: p, Mode: mode, Data: []byte(content)})
	}
	if err := distpkg.WriteAppImage(&distpkg.AppImage{Runtime: testRuntime(), Files: fl, Time: time.Date(2026, 11, 27, 0, 0, 0, 0, time.UTC)}, file); err != nil {
		t.Fatal(err)
	}
	b, err := os.ReadFile(file)
	if err != nil {
		t.Fatal(err)
	}
	return b
}

// testCode is an executable's code, as a version of it: the next one has
// a function more near its start (what follows moves, as a linker moves
// it) and a few instructions changed further on.
func testCode(version int) string {
	r := rand.New(rand.NewPCG(3, 4))
	code := make([]byte, 700<<10)
	for i := range code {
		code[i] = byte(r.IntN(24) * 11) // compresses about as code does
	}
	if version > 1 {
		fn := bytes.Repeat([]byte("a new function's code "), 20)
		code = append(code[:30000:30000], append(fn, code[30000:]...)...)
		for i := 200000; i < len(code); i += 50000 {
			code[i]++
		}
	}
	return string(code)
}

// testAppImages are two versions of an app's AppImage, in dir: its code
// changed (moved, as a change to an executable moves what follows), a
// file changed, one new, the rest the same.
func testAppImages(t testing.TB, dir string) (oldFile, newFile string) {
	t.Helper()
	same := strings.Repeat("the same in both versions\n", 4000)
	oldFile, newFile = filepath.Join(dir, "old.AppImage"), filepath.Join(dir, "new.AppImage")
	testAppImage(t, oldFile, map[string]string{
		"/AppRun":                 "#!/bin/sh\nexec \"$APPDIR/usr/bin/notes\" \"$@\"\n",
		"/usr/bin/notes":          testCode(1),
		"/usr/share/notes/a.json": `{"version": "1.0.0"}`,
		"/usr/share/notes/same":   same,
	})
	testAppImage(t, newFile, map[string]string{
		"/AppRun":                 "#!/bin/sh\nexec \"$APPDIR/usr/bin/notes\" \"$@\"\n",
		"/usr/bin/notes":          testCode(2),
		"/usr/share/notes/a.json": `{"version": "1.1.0"}`,
		"/usr/share/notes/new":    "only in 1.1.0",
		"/usr/share/notes/same":   same,
	})
	return oldFile, newFile
}

// An AppImage's delta patches the AppImages with their blocks
// decompressed: a change to the app's code that moves what follows it
// makes a delta about as small as the code's own patch, not one of every
// compressed block after the change. Applied as the updater applies it
// (its blocks compressed again), it makes the new AppImage byte for byte;
// applied to another AppImage, it fails.
func TestAppImageDelta(t *testing.T) {
	dir := t.TempDir()
	oldFile, newFile := testAppImages(t, dir)
	prev, _ := os.ReadFile(oldFile)
	b, _ := os.ReadFile(newFile)
	var delta bytes.Buffer
	if err := WriteFileDelta(&delta, "1.0.0", "1.1.0", oldFile, newFile); err != nil {
		t.Fatal(err)
	}
	index := readIndex(t, delta.Bytes())
	if len(index.Entries) != 1 || index.Entries[0].Blocks != blocksZlib8 || index.Entries[0].From != "old.AppImage" {
		t.Fatalf("the index: %+v", index)
	}
	// The code's own patch, and the AppImages' patched as they are.
	code := Diff([]byte(testCode(1)), []byte(testCode(2)))
	raw := Diff(prev, b)
	t.Logf("the delta: %d bytes; the code's patch %d, the AppImages' as they are %d, the AppImage %d", delta.Len(), len(code), len(raw), len(b))
	if delta.Len() > 2*len(code)+4096 || delta.Len()*4 > len(raw) {
		t.Errorf("the delta is %d bytes: the code's patch is %d, the AppImages' as they are %d", delta.Len(), len(code), len(raw))
	}

	installed := t.TempDir()
	os.WriteFile(filepath.Join(installed, "old.AppImage"), prev, 0o755)
	out := filepath.Join(t.TempDir(), "new")
	if err := ApplyDelta(bytes.NewReader(delta.Bytes()), int64(delta.Len()), "1.0.0", "1.1.0", installed, out); err != nil {
		t.Fatal(err)
	}
	if got, _ := os.ReadFile(filepath.Join(out, "new.AppImage")); !bytes.Equal(got, b) {
		t.Errorf("applying the delta made %d bytes, not the new AppImage", len(got))
	}

	// An AppImage changed since: a byte of its code, in a block.
	blocks, err := distpkg.AppImageBlocks(prev)
	if err != nil || len(blocks) < 5 {
		t.Fatalf("the blocks: %v, %v", blocks, err)
	}
	changed := append([]byte{}, prev...)
	changed[blocks[2].Off+blocks[2].Len/2] ^= 0x40
	os.WriteFile(filepath.Join(installed, "old.AppImage"), changed, 0o755)
	out2 := filepath.Join(t.TempDir(), "new")
	if err := ApplyDelta(bytes.NewReader(delta.Bytes()), int64(delta.Len()), "1.0.0", "1.1.0", installed, out2); err == nil {
		t.Error("the delta applied to another AppImage")
	}

	// Files that aren't AppImages are patched as they are.
	other := filepath.Join(dir, "other")
	os.WriteFile(other, []byte(testCode(2)), 0o755)
	delta.Reset()
	if err := WriteFileDelta(&delta, "1.0.0", "1.1.0", oldFile, other); err != nil {
		t.Fatal(err)
	}
	if e := readIndex(t, delta.Bytes()).Entries[0]; e.Blocks != "" {
		t.Errorf("a file that isn't an AppImage patched by its blocks: %+v", e)
	}
}
