package distpkg

import (
	"bytes"
	"compress/zlib"
	"io"
	"io/fs"
	"math/rand/v2"
	"os"
	"path/filepath"
	"testing"
)

// AppImageBlocks finds every compressed block of an AppImage's SquashFS:
// its files' (one of several blocks, one stored as it is), its inodes'
// and directories' and its ids': each a zlib stream that Zlib8 makes
// again, the blocks apart and in order, and between them nothing but
// what isn't compressed. What isn't an AppImage has none.
func TestAppImageBlocks(t *testing.T) {
	r := rand.New(rand.NewPCG(5, 6))
	noise := make([]byte, 50000) // doesn't compress: a block stored as it is
	for i := range noise {
		noise[i] = byte(r.Uint32())
	}
	code := make([]byte, 300<<10) // three blocks
	for i := range code {
		code[i] = byte(r.IntN(20) * 13)
	}
	files := []File{
		{Path: "/AppRun", Mode: 0o755, Data: []byte("#!/bin/sh\n")},
		{Path: "/usr/bin/notes", Mode: 0o755, Data: code},
		{Path: "/usr/share/notes/noise", Mode: 0o644, Data: noise},
		{Path: "/usr/share/notes/empty", Mode: 0o644},
		{Path: "/notes.png", Mode: fs.ModeSymlink | 0o777, Link: "usr/share/icons/notes.png"},
	}
	// Enough files for directories' metadata of more than one block.
	for i := range 400 {
		files = append(files, File{Path: "/usr/share/notes/many/a-file-of-a-rather-long-name-" + string(rune('a'+i%26)) + string(rune('a'+i/26)), Mode: 0o644, Data: []byte("x")})
	}
	file := filepath.Join(t.TempDir(), "notes.AppImage")
	if err := WriteAppImage(&AppImage{Runtime: fakeRuntime(1024), Files: files}, file); err != nil {
		t.Fatal(err)
	}
	b, err := os.ReadFile(file)
	if err != nil {
		t.Fatal(err)
	}
	blocks, err := AppImageBlocks(b)
	if err != nil {
		t.Fatal(err)
	}
	data, metas := 0, 0
	end := int64(len(fakeRuntime(1024)))
	for _, k := range blocks {
		if k.Off < end || k.Off+k.Len > int64(len(b)) {
			t.Fatalf("a block at %d, %d bytes: out of place", k.Off, k.Len)
		}
		zr, err := zlib.NewReader(bytes.NewReader(b[k.Off : k.Off+k.Len]))
		if err != nil {
			t.Fatal(err)
		}
		d, err := io.ReadAll(zr)
		if err != nil || int64(len(d)) != k.Size {
			t.Fatalf("a block of %d bytes: %v", len(d), err)
		}
		if !bytes.Equal(Zlib8(d), b[k.Off:k.Off+k.Len]) {
			t.Errorf("Zlib8 doesn't make the block at %d again", k.Off)
		}
		if k.Size == sqfsBlockSize || bytes.Equal(d, code[2*sqfsBlockSize:]) {
			data++
		} else {
			metas++
		}
		end = k.Off + k.Len
	}
	// The code's three blocks (the noise is stored as it is), and the
	// tables' metadata blocks: the inodes', the directories' (two, at
	// least), the ids'.
	if data != 3 || metas < 4 {
		t.Errorf("%d blocks of data and %d of metadata", data, metas)
	}
	for _, bad := range [][]byte{nil, []byte("not an AppImage"), fakeRuntime(1024), append(fakeRuntime(1024), make([]byte, 4096)...)} {
		if _, err := AppImageBlocks(bad); err == nil {
			t.Errorf("blocks in %d bytes that aren't an AppImage", len(bad))
		}
	}
}
