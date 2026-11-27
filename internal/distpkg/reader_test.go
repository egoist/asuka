package distpkg

import (
	"bytes"
	"compress/zlib"
	"encoding/binary"
	"io"
	"io/fs"
	"testing"
)

// A SquashFS reader, of what WriteSquashFS writes (gzip, no fragments),
// as squashfuse reads it: the superblock, the inodes by reference, the
// directories' entries, the files' blocks.

type sqEntry struct {
	mode fs.FileMode
	data []byte
	link string
}

func readSquashFS(t *testing.T, img []byte) map[string]sqEntry {
	t.Helper()
	le16 := func(b []byte) uint16 { return binary.LittleEndian.Uint16(b) }
	le32 := func(b []byte) uint32 { return binary.LittleEndian.Uint32(b) }
	le64 := func(b []byte) uint64 { return binary.LittleEndian.Uint64(b) }
	if le32(img) != sqfsMagic || le16(img[28:]) != 4 || le16(img[20:]) != sqfsGzip || le32(img[12:]) != sqfsBlockSize {
		t.Fatalf("the superblock: %x", img[:32])
	}
	if le16(img[24:])&sqfsNoFragments == 0 || le32(img[16:]) != 0 {
		t.Error("fragments")
	}
	inodes := le32(img[4:])
	root, used := le64(img[32:]), le64(img[40:])
	idTable, inodeTable, dirTable := le64(img[48:]), le64(img[64:]), le64(img[72:])
	if used > uint64(len(img)) {
		t.Fatalf("bytes used %d, of %d", used, len(img))
	}
	unzip := func(b []byte) []byte {
		r, err := zlib.NewReader(bytes.NewReader(b))
		if err != nil {
			t.Fatal(err)
		}
		out, err := io.ReadAll(r)
		if err != nil {
			t.Fatal(err)
		}
		return out
	}
	// meta reads n bytes of a metadata table from the block at start
	// (absolute), offset in.
	meta := func(start uint64, offset, n int) []byte {
		var out []byte
		for len(out) < offset+n {
			h := le16(img[start:])
			size := uint64(h &^ sqfsUncompressedMeta)
			b := img[start+2 : start+2+size]
			if h&sqfsUncompressedMeta == 0 {
				b = unzip(b)
			}
			out = append(out, b...)
			start += 2 + size
		}
		return out[offset : offset+n]
	}
	// The ids: uid 0 alone.
	if got := le32(meta(le64(img[idTable:]), 0, 4)); got != 0 {
		t.Errorf("the id %d", got)
	}
	out := map[string]sqEntry{}
	seen := map[uint32]bool{}
	var walk func(ref uint64, path string)
	walk = func(ref uint64, path string) {
		blk, off := inodeTable+ref>>16, int(ref&0xffff)
		h := meta(blk, off, 16)
		typ, perm, ino := le16(h), fs.FileMode(le16(h[2:])), le32(h[12:])
		if seen[ino] || ino == 0 || ino > inodes {
			t.Errorf("%s: inode %d", path, ino)
		}
		seen[ino] = true
		switch typ {
		case sqfsDir, sqfsLDir:
			var start uint32
			var size, doff int
			if typ == sqfsDir {
				d := meta(blk, off+16, 16)
				start, size, doff = le32(d), int(le16(d[8:])), int(le16(d[10:]))
			} else {
				d := meta(blk, off+16, 24)
				size, start, doff = int(le32(d[4:])), le32(d[8:]), int(le16(d[18:]))
			}
			out[path] = sqEntry{mode: fs.ModeDir | perm}
			listing := meta(dirTable+uint64(start), doff, size-3)
			for len(listing) > 0 {
				count, iblock, base := le32(listing)+1, le32(listing[4:]), int32(le32(listing[8:]))
				listing = listing[12:]
				var prev string
				for range count {
					ioff, delta, nlen := le16(listing), int16(le16(listing[2:])), int(le16(listing[6:]))+1
					name := string(listing[8 : 8+nlen])
					listing = listing[8+nlen:]
					if name <= prev {
						t.Errorf("%s: %q after %q", path, name, prev)
					}
					prev = name
					child := path + "/" + name
					if path == "/" {
						child = "/" + name
					}
					_ = base + int32(delta)
					walk(uint64(iblock)<<16|uint64(ioff), child)
				}
			}
		case sqfsFile:
			d := meta(blk, off+16, 16)
			start, frag, size := le32(d), le32(d[4:]), le32(d[12:])
			if frag != sqfsNoIndex {
				t.Errorf("%s: a fragment", path)
			}
			n := (int(size) + sqfsBlockSize - 1) / sqfsBlockSize
			sizes := meta(blk, off+32, 4*n)
			var data []byte
			pos := uint64(start)
			for i := range n {
				s := le32(sizes[4*i:])
				l := uint64(s &^ sqfsUncompressedBlock)
				b := img[pos : pos+l]
				if s&sqfsUncompressedBlock == 0 {
					b = unzip(b)
				}
				data = append(data, b...)
				pos += l
			}
			if len(data) != int(size) {
				t.Errorf("%s: %d bytes, its inode says %d", path, len(data), size)
			}
			out[path] = sqEntry{mode: perm, data: data}
		case sqfsSymlink:
			d := meta(blk, off+16, 8)
			l := int(le32(d[4:]))
			out[path] = sqEntry{mode: fs.ModeSymlink | perm, link: string(meta(blk, off+24, l))}
		default:
			t.Errorf("%s: inode type %d", path, typ)
		}
	}
	walk(root, "/")
	if len(seen) != int(inodes) {
		t.Errorf("%d inodes reached, the superblock says %d", len(seen), inodes)
	}
	return out
}

// fakeUpdOffset is where fakeRuntime's .upd_info section is.
const fakeUpdOffset = 64

// fakeRuntime is an ELF as the AppImage runtime is to WriteAppImage: AI
// and 2 in its identification, a .upd_info section of updSize bytes, and
// nothing after its section headers.
func fakeRuntime(updSize int) []byte {
	shstr := []byte("\x00.upd_info\x00.shstrtab\x00")
	var b bytes.Buffer
	hdr := make([]byte, 64)
	copy(hdr, []byte{0x7f, 'E', 'L', 'F', 2, 1, 1, 0, 'A', 'I', 2})
	le := binary.LittleEndian
	le.PutUint16(hdr[16:], 2)  // an executable
	le.PutUint16(hdr[18:], 62) // x86-64
	le.PutUint32(hdr[20:], 1)
	shoff := 64 + updSize + len(shstr)
	le.PutUint64(hdr[40:], uint64(shoff))
	le.PutUint16(hdr[52:], 64) // the header's size
	le.PutUint16(hdr[58:], 64) // a section header's
	le.PutUint16(hdr[60:], 3)  // sections
	le.PutUint16(hdr[62:], 2)  // .shstrtab's index
	b.Write(hdr)
	b.Write(make([]byte, updSize))
	b.Write(shstr)
	sh := func(name, typ uint32, off, size int) {
		s := make([]byte, 64)
		le.PutUint32(s[0:], name)
		le.PutUint32(s[4:], typ)
		le.PutUint64(s[24:], uint64(off))
		le.PutUint64(s[32:], uint64(size))
		le.PutUint64(s[48:], 1)
		b.Write(s)
	}
	sh(0, 0, 0, 0)
	sh(1, 1, 64, updSize)             // .upd_info: PROGBITS
	sh(11, 3, 64+updSize, len(shstr)) // .shstrtab
	return b.Bytes()
}
