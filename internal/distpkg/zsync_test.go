package distpkg

import (
	"bytes"
	"os"
	"path/filepath"
	"strconv"
	"testing"
	"time"
)

// zsyncSample is a test file's bytes: an LCG's, seeded by its size.
func zsyncSample(n int) []byte {
	b := make([]byte, n)
	x := uint32(n)
	for i := range b {
		x = x*1103515245 + 12345
		b[i] = byte(x >> 16)
	}
	return b
}

// TestZsync compares WriteZsync's files with zsyncmake's (zsync 0.6.2,
// Debian's), made of the same samples dated 2026-01-02 03:04:05 UTC:
//
//	zsyncmake -u https://example.com/sample-N -o sample-N.zsync sample-N
//
// one block, a block exactly, and many, whose hash lengths differ.
func TestZsync(t *testing.T) {
	for _, n := range []int{1000, 2048, 300001} {
		name := "sample-" + strconv.Itoa(n)
		want, err := os.ReadFile(filepath.Join("testdata", "zsync-"+name+".zsync"))
		if err != nil {
			t.Fatal(err)
		}
		var got bytes.Buffer
		info := ZsyncInfo{Filename: name, URL: "https://example.com/" + name, MTime: time.Date(2026, 1, 2, 3, 4, 5, 0, time.UTC)}
		if err := WriteZsync(&got, bytes.NewReader(zsyncSample(n)), int64(n), info); err != nil {
			t.Fatal(err)
		}
		if !bytes.Equal(got.Bytes(), want) {
			head := func(b []byte) []byte { h, _, _ := bytes.Cut(b, []byte("\n\n")); return h }
			t.Errorf("%s: %d bytes, zsyncmake's %d:\n%s\nwant:\n%s", name, got.Len(), len(want), head(got.Bytes()), head(want))
		}
	}
}

func TestZsyncHashLengths(t *testing.T) {
	for _, c := range []struct {
		n                int64
		seq, rsum, check int
	}{
		{1000, 1, 2, 4},
		{2048, 1, 2, 4},
		{300001, 2, 2, 4},
		{60 << 20, 2, 2, 5},  // an AppImage
		{200 << 20, 2, 2, 5}, // in blocks of 4 KiB
		{4 << 30, 2, 3, 5},   // larger
	} {
		bs := zsyncBlocksize(c.n)
		seq, rsum, check := zsyncHashLengths(c.n, bs)
		if seq != c.seq || rsum != c.rsum || check != c.check {
			t.Errorf("%d bytes: %d,%d,%d, want %d,%d,%d", c.n, seq, rsum, check, c.seq, c.rsum, c.check)
		}
	}
	if err := WriteZsync(&bytes.Buffer{}, bytes.NewReader(nil), 0, ZsyncInfo{}); err == nil {
		t.Error("an empty file")
	}
}
