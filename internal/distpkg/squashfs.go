package distpkg

import (
	"bytes"
	"compress/zlib"
	"encoding/binary"
	"fmt"
	"io"
	"io/fs"
	"path"
	"sort"
	"strings"
	"time"
)

// SquashFS 4.0, as mksquashfs writes it with -comp gzip -no-fragments
// -no-xattrs -all-root: the AppImage runtime (squashfuse) mounts it, as
// the kernel and unsquashfs read it.
//
//	the superblock     96 bytes
//	the data           each file's blocks (128 KiB, zlib), one after another
//	the inode table    metadata blocks (8 KiB, zlib) of the inodes
//	the directories    metadata blocks of the directories' entries
//	the fragments      none: a file's last block is a block of its own
//	the ids            uid and gid 0, and the table locating them
//
// Directories are written after what they hold, so that each knows where
// its children's inodes are; the root's inode is the last.

const (
	sqfsMagic       = 0x73717368
	sqfsBlockSize   = 128 << 10
	sqfsBlockLog    = 17
	sqfsMetaSize    = 8192
	sqfsGzip        = 1
	sqfsNoFragments = 0x0010
	sqfsNoXattrs    = 0x0200
	sqfsNone        = 0xFFFFFFFFFFFFFFFF
	sqfsNoIndex     = 0xFFFFFFFF

	sqfsDir     = 1
	sqfsFile    = 2
	sqfsSymlink = 3
	sqfsLDir    = 8
	sqfsLFile   = 9

	sqfsUncompressedBlock = 1 << 24
	sqfsUncompressedMeta  = 0x8000
)

// A node is an entry of the tree being written.
type sqNode struct {
	name     string
	file     *File
	children []*sqNode // a directory's, sorted by name
	ino      uint32
	ref      uint64 // its inode's reference: block << 16 | offset
	typ      uint16 // its inode's basic type, as directories list it
	parent   *sqNode
}

// metaWriter writes a metadata table: 8 KiB blocks, each compressed (or
// not, when that's no smaller) after a two-byte header of its size.
type metaWriter struct {
	out bytes.Buffer // the blocks written
	cur bytes.Buffer // the block being filled
}

// pos is where the next byte goes: the block's start in the table and
// the offset in it.
func (m *metaWriter) pos() (block uint32, offset uint16) {
	return uint32(m.out.Len()), uint16(m.cur.Len())
}

func (m *metaWriter) Write(p []byte) (int, error) {
	n := len(p)
	for len(p) > 0 {
		k := min(sqfsMetaSize-m.cur.Len(), len(p))
		m.cur.Write(p[:k])
		p = p[k:]
		if m.cur.Len() == sqfsMetaSize {
			m.flush()
		}
	}
	return n, nil
}

func (m *metaWriter) flush() {
	if m.cur.Len() == 0 {
		return
	}
	raw := m.cur.Bytes()
	c := compress(raw)
	var h [2]byte
	if len(c) < len(raw) {
		binary.LittleEndian.PutUint16(h[:], uint16(len(c)))
		m.out.Write(h[:])
		m.out.Write(c)
	} else {
		binary.LittleEndian.PutUint16(h[:], uint16(len(raw))|sqfsUncompressedMeta)
		m.out.Write(h[:])
		m.out.Write(raw)
	}
	m.cur.Reset()
}

// compress is data, zlib-compressed (SquashFS's "gzip").
func compress(data []byte) []byte {
	var b bytes.Buffer
	zw, _ := zlib.NewWriterLevel(&b, zlib.BestCompression)
	zw.Write(data)
	zw.Close()
	return b.Bytes()
}

// WriteSquashFS writes files (absolute paths, their directories made
// where they have no entry) as a SquashFS image to w, every entry owned
// by root and dated mtime.
func WriteSquashFS(w io.Writer, files []File, mtime time.Time) error {
	files = withParents(files)
	if err := check(files); err != nil {
		return err
	}
	root := &sqNode{file: &File{Path: "/", Mode: fs.ModeDir | 0o755}}
	byPath := map[string]*sqNode{"/": root}
	for i := range files {
		f := &files[i]
		parent := byPath[path.Dir(f.Path)]
		if parent == nil || !parent.file.IsDir() {
			return fmt.Errorf("distpkg: %s is in something that isn't a directory", f.Path)
		}
		n := &sqNode{name: path.Base(f.Path), file: f, parent: parent}
		parent.children = append(parent.children, n)
		byPath[f.Path] = n
	}
	// Inode numbers in the order the inodes are written: a directory's
	// after its children's.
	var order []*sqNode
	var number func(n *sqNode)
	number = func(n *sqNode) {
		sort.Slice(n.children, func(i, j int) bool { return n.children[i].name < n.children[j].name })
		for _, c := range n.children {
			if c.file.IsDir() {
				number(c)
			}
		}
		for _, c := range n.children {
			if !c.file.IsDir() {
				order = append(order, c)
			}
		}
		order = append(order, n)
	}
	number(root)
	for i, n := range order {
		n.ino = uint32(i + 1)
	}
	count := uint32(len(order))

	// The superblock goes first; the data after it.
	var img countWriter
	var buf bytes.Buffer
	img.w = &buf
	img.Write(make([]byte, 96))

	var inodes, dirs metaWriter
	t := uint32(mtime.Unix())
	head := func(typ uint16, n *sqNode) []byte {
		b := make([]byte, 16)
		binary.LittleEndian.PutUint16(b[0:], typ)
		binary.LittleEndian.PutUint16(b[2:], uint16(n.file.Mode.Perm()))
		binary.LittleEndian.PutUint16(b[4:], 0) // uid: the id table's first
		binary.LittleEndian.PutUint16(b[6:], 0) // gid
		binary.LittleEndian.PutUint32(b[8:], t)
		binary.LittleEndian.PutUint32(b[12:], n.ino)
		return b
	}
	writeInode := func(n *sqNode, b []byte) {
		blk, off := inodes.pos()
		n.ref = uint64(blk)<<16 | uint64(off)
		inodes.Write(b)
	}
	for _, n := range order {
		f := n.file
		switch {
		case f.IsLink():
			n.typ = sqfsSymlink
			b := head(sqfsSymlink, n)
			b = binary.LittleEndian.AppendUint32(b, 1)
			b = binary.LittleEndian.AppendUint32(b, uint32(len(f.Link)))
			writeInode(n, append(b, f.Link...))
		case f.IsDir():
			if err := writeDir(n, &dirs, head, writeInode, count); err != nil {
				return err
			}
		default:
			n.typ = sqfsFile
			start := uint64(img.n)
			sizes, size, err := writeBlocks(&img, f)
			if err != nil {
				return err
			}
			var b []byte
			if start > 0xFFFFFFFF || size > 0xFFFFFFFF {
				b = head(sqfsLFile, n)
				b = binary.LittleEndian.AppendUint64(b, start)
				b = binary.LittleEndian.AppendUint64(b, uint64(size))
				b = binary.LittleEndian.AppendUint64(b, 0) // sparse
				b = binary.LittleEndian.AppendUint32(b, 1) // links
				b = binary.LittleEndian.AppendUint32(b, sqfsNoIndex)
				b = binary.LittleEndian.AppendUint32(b, 0)
				b = binary.LittleEndian.AppendUint32(b, sqfsNoIndex) // xattrs
			} else {
				b = head(sqfsFile, n)
				b = binary.LittleEndian.AppendUint32(b, uint32(start))
				b = binary.LittleEndian.AppendUint32(b, sqfsNoIndex) // no fragment
				b = binary.LittleEndian.AppendUint32(b, 0)
				b = binary.LittleEndian.AppendUint32(b, uint32(size))
			}
			for _, s := range sizes {
				b = binary.LittleEndian.AppendUint32(b, s)
			}
			writeInode(n, b)
		}
	}
	inodes.flush()
	dirs.flush()

	inodeStart := uint64(img.n)
	img.Write(inodes.out.Bytes())
	dirStart := uint64(img.n)
	img.Write(dirs.out.Bytes())
	fragStart := uint64(img.n) // no fragments: an empty table here
	// The ids: one metadata block of uid 0, and the table of where it is.
	var ids metaWriter
	ids.Write([]byte{0, 0, 0, 0})
	ids.flush()
	idBlock := uint64(img.n)
	img.Write(ids.out.Bytes())
	idStart := uint64(img.n)
	img.Write(binary.LittleEndian.AppendUint64(nil, idBlock))
	used := uint64(img.n)

	sb := make([]byte, 0, 96)
	le32 := func(v uint32) { sb = binary.LittleEndian.AppendUint32(sb, v) }
	le16 := func(v uint16) { sb = binary.LittleEndian.AppendUint16(sb, v) }
	le64 := func(v uint64) { sb = binary.LittleEndian.AppendUint64(sb, v) }
	le32(sqfsMagic)
	le32(count)
	le32(t)
	le32(sqfsBlockSize)
	le32(0) // fragments
	le16(sqfsGzip)
	le16(sqfsBlockLog)
	le16(sqfsNoFragments | sqfsNoXattrs)
	le16(1) // ids
	le16(4)
	le16(0)
	le64(root.ref)
	le64(used)
	le64(idStart)
	le64(sqfsNone) // xattrs
	le64(inodeStart)
	le64(dirStart)
	le64(fragStart)
	le64(sqfsNone) // no export table
	data := buf.Bytes()
	copy(data, sb)
	// Padded to 4 KiB, as mksquashfs pads it (a loop device's blocks).
	if r := len(data) % 4096; r != 0 {
		data = append(data, make([]byte, 4096-r)...)
	}
	_, err := w.Write(data)
	return err
}

// writeBlocks writes a file's data as blocks, each compressed unless
// that's no smaller, and returns their sizes as its inode lists them.
func writeBlocks(w io.Writer, f *File) ([]uint32, int64, error) {
	r, err := f.Open()
	if err != nil {
		return nil, 0, err
	}
	defer r.Close()
	var sizes []uint32
	var total int64
	block := make([]byte, sqfsBlockSize)
	for {
		n, err := io.ReadFull(r, block)
		if n > 0 {
			total += int64(n)
			raw := block[:n]
			c := compress(raw)
			if len(c) < n {
				w.Write(c)
				sizes = append(sizes, uint32(len(c)))
			} else {
				w.Write(raw)
				sizes = append(sizes, uint32(n)|sqfsUncompressedBlock)
			}
		}
		if err == io.EOF || err == io.ErrUnexpectedEOF {
			return sizes, total, nil
		}
		if err != nil {
			return nil, 0, err
		}
	}
}

// writeDir writes a directory's entries to the directory table and its
// inode: its children's inodes are written already. Entries go in runs
// of those whose inodes are in one metadata block, numbered near one
// another, 256 at most, each run after a header.
func writeDir(n *sqNode, dirs *metaWriter, head func(uint16, *sqNode) []byte, writeInode func(*sqNode, []byte), count uint32) error {
	var listing bytes.Buffer
	links := uint32(2)
	for i := 0; i < len(n.children); {
		first := n.children[i]
		block := uint32(first.ref >> 16)
		j := i
		for j < len(n.children) && j-i < 256 {
			c := n.children[j]
			d := int64(c.ino) - int64(first.ino)
			if uint32(c.ref>>16) != block || d < -32768 || d > 32767 {
				break
			}
			j++
		}
		binary.Write(&listing, binary.LittleEndian, uint32(j-i-1))
		binary.Write(&listing, binary.LittleEndian, block)
		binary.Write(&listing, binary.LittleEndian, first.ino)
		for _, c := range n.children[i:j] {
			if len(c.name) > 256 || c.name == "" || strings.Contains(c.name, "/") {
				return fmt.Errorf("distpkg: %q isn't a file name SquashFS takes", c.name)
			}
			binary.Write(&listing, binary.LittleEndian, uint16(c.ref&0xFFFF))
			binary.Write(&listing, binary.LittleEndian, int16(int64(c.ino)-int64(first.ino)))
			binary.Write(&listing, binary.LittleEndian, c.typ)
			binary.Write(&listing, binary.LittleEndian, uint16(len(c.name)-1))
			listing.WriteString(c.name)
			if c.file.IsDir() {
				links++
			}
		}
		i = j
	}
	blk, off := dirs.pos()
	dirs.Write(listing.Bytes())
	size := uint32(listing.Len()) + 3 // "." and "..", which it doesn't list
	parent := count + 1               // the root's, as mksquashfs numbers it
	if n.parent != nil {
		parent = n.parent.ino
	}
	n.typ = sqfsDir
	var b []byte
	if size > 0xFFFF {
		b = head(sqfsLDir, n)
		b = binary.LittleEndian.AppendUint32(b, links)
		b = binary.LittleEndian.AppendUint32(b, size)
		b = binary.LittleEndian.AppendUint32(b, blk)
		b = binary.LittleEndian.AppendUint32(b, parent)
		b = binary.LittleEndian.AppendUint16(b, 0) // no index
		b = binary.LittleEndian.AppendUint16(b, off)
		b = binary.LittleEndian.AppendUint32(b, sqfsNoIndex) // xattrs
	} else {
		b = head(sqfsDir, n)
		b = binary.LittleEndian.AppendUint32(b, blk)
		b = binary.LittleEndian.AppendUint32(b, links)
		b = binary.LittleEndian.AppendUint16(b, uint16(size))
		b = binary.LittleEndian.AppendUint16(b, off)
		b = binary.LittleEndian.AppendUint32(b, parent)
	}
	writeInode(n, b)
	return nil
}
