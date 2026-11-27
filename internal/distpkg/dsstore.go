package distpkg

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"math"
	"math/bits"
	"sort"
	"strings"
	"unicode/utf16"
)

// A .DS_Store is how the Finder shows a folder: its window's bounds, its
// view and where each icon is. A disk image's says how its window opens:
// the app and the Applications folder side by side, large, to drag one
// onto the other.
//
// The file is a buddy allocator's blocks (its header, a root block of the
// blocks' addresses and a table of contents) holding a B-tree, "DSDB",
// of records: a file's name, a code (Iloc: an icon's place; bwsp: the
// window; icvp: the icon view's options) and a value.

// A DMGLayout is a disk image window's.
type DMGLayout struct {
	// Width and Height are the window's content's size, in points.
	Width, Height int
	// IconSize is the icons' size, in points (up to 512).
	IconSize int
	// Icons are where each item's icon's center goes, by its name.
	Icons map[string][2]int
}

// DSStore is layout as a .DS_Store.
func DSStore(layout DMGLayout) []byte {
	var recs []dsRecord
	bounds := fmt.Sprintf("{{200, 120}, {%d, %d}}", layout.Width, layout.Height)
	recs = append(recs,
		dsRecord{".", "bwsp", "blob", bplist(map[string]any{
			"ContainerShowSidebar": false, "ShowPathbar": false, "ShowSidebar": false,
			"ShowStatusBar": false, "ShowTabView": false, "ShowToolbar": false,
			"SidebarWidth": int64(0), "WindowBounds": bounds,
		})},
		dsRecord{".", "icvp", "blob", bplist(map[string]any{
			"viewOptionsVersion": int64(1), "backgroundType": int64(0),
			"backgroundColorRed": 1.0, "backgroundColorGreen": 1.0, "backgroundColorBlue": 1.0,
			"gridOffsetX": 0.0, "gridOffsetY": 0.0, "gridSpacing": 100.0,
			"arrangeBy": "none", "showIconPreview": true, "showItemInfo": false,
			"labelOnBottom": true, "textSize": 13.0, "iconSize": float64(layout.IconSize),
			"scrollPositionX": 0.0, "scrollPositionY": 0.0,
		})},
		dsRecord{".", "vSrn", "long", be32(1)},
		dsRecord{".", "vstl", "type", []byte("icnv")},
	)
	for name, at := range layout.Icons {
		loc := make([]byte, 16)
		binary.BigEndian.PutUint32(loc[0:], uint32(at[0]))
		binary.BigEndian.PutUint32(loc[4:], uint32(at[1]))
		copy(loc[8:], []byte{0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0, 0})
		recs = append(recs, dsRecord{name, "Iloc", "blob", loc})
	}
	// The B-tree's order: by name, without case (as the Finder compares
	// them), then by code.
	sort.Slice(recs, func(i, j int) bool {
		a, b := strings.ToLower(recs[i].name), strings.ToLower(recs[j].name)
		if a != b {
			return a < b
		}
		return recs[i].code < recs[j].code
	})

	// One leaf node holds them all.
	var node bytes.Buffer
	node.Write(be32(0)) // a leaf
	node.Write(be32(uint32(len(recs))))
	for _, r := range recs {
		r.encode(&node)
	}

	var a buddy
	a.init()
	header := a.alloc(32) // the allocator's header, at 0
	_ = header
	root := a.alloc(2048)
	dsdb := a.alloc(32)
	leaf := a.alloc(max(4096, node.Len()))
	blocks := []uint32{root, dsdb, leaf} // block 0: the root block

	var db bytes.Buffer
	db.Write(be32(2)) // the B-tree's root: block 2, the leaf
	db.Write(be32(0)) // levels above the leaves
	db.Write(be32(uint32(len(recs))))
	db.Write(be32(1))      // nodes
	db.Write(be32(0x1000)) // page size

	var rb bytes.Buffer
	rb.Write(be32(uint32(len(blocks))))
	rb.Write(be32(0))
	for i := range 256 * ((len(blocks) + 255) / 256) {
		if i < len(blocks) {
			rb.Write(be32(blocks[i]))
		} else {
			rb.Write(be32(0))
		}
	}
	rb.Write(be32(1)) // the table of contents: DSDB, block 1
	rb.WriteByte(4)
	rb.WriteString("DSDB")
	rb.Write(be32(1))
	for _, free := range a.free {
		rb.Write(be32(uint32(len(free))))
		for _, off := range free {
			rb.Write(be32(off))
		}
	}

	size := 0
	for _, b := range blocks {
		off, n := blockSpan(b)
		size = max(size, int(off)+n)
	}
	file := make([]byte, 4+size)
	binary.BigEndian.PutUint32(file[0:], 1)
	copy(file[4:], "Bud1")
	rootOff, rootSize := blockSpan(root)
	binary.BigEndian.PutUint32(file[8:], rootOff)
	binary.BigEndian.PutUint32(file[12:], uint32(rootSize))
	binary.BigEndian.PutUint32(file[16:], rootOff)
	copy(file[20:], []byte{0x00, 0x00, 0x10, 0x0c, 0x00, 0x00, 0x00, 0x87, 0x00, 0x00, 0x20, 0x0b, 0x00, 0x00, 0x00, 0x00})
	put := func(block uint32, data []byte) {
		off, _ := blockSpan(block)
		copy(file[4+off:], data)
	}
	put(root, rb.Bytes())
	put(dsdb, db.Bytes())
	put(leaf, node.Bytes())
	return file
}

// blockSpan is a block address's offset (in the allocator, after the
// file's first four bytes) and size: its low five bits are its size's
// log2.
func blockSpan(addr uint32) (uint32, int) {
	return addr &^ 0x1f, 1 << (addr & 0x1f)
}

// buddy is the allocator's free lists: free[k] are the free blocks of
// 2^k bytes.
type buddy struct {
	free [32][]uint32
}

func (b *buddy) init() { b.free[31] = []uint32{0} }

// alloc takes a block of n bytes (rounded up to a power of two, 32 at
// least), splitting a larger one, and returns its address.
func (b *buddy) alloc(n int) uint32 {
	k := max(5, bits.Len(uint(n-1)))
	j := k
	for j < 32 && len(b.free[j]) == 0 {
		j++
	}
	off := b.free[j][0]
	b.free[j] = b.free[j][1:]
	for j > k {
		j--
		b.free[j] = append(b.free[j], off+1<<j)
		sort.Slice(b.free[j], func(x, y int) bool { return b.free[j][x] < b.free[j][y] })
	}
	return off | uint32(k)
}

type dsRecord struct {
	name, code, typ string
	data            []byte
}

func (r dsRecord) encode(w *bytes.Buffer) {
	u := utf16.Encode([]rune(r.name))
	w.Write(be32(uint32(len(u))))
	for _, c := range u {
		binary.Write(w, binary.BigEndian, c)
	}
	w.WriteString(r.code)
	w.WriteString(r.typ)
	switch r.typ {
	case "blob":
		w.Write(be32(uint32(len(r.data))))
	}
	w.Write(r.data)
}

func be32(v uint32) []byte { return binary.BigEndian.AppendUint32(nil, v) }

// bplist is a binary property list (bplist00) of a dictionary of
// strings, booleans, integers and reals.
func bplist(m map[string]any) []byte {
	keys := make([]string, 0, len(m))
	for k := range m {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	// Objects: the dictionary, its keys, its values.
	var objs [][]byte
	objs = append(objs, nil) // the dictionary, made last
	ref := func(b []byte) int {
		objs = append(objs, b)
		return len(objs) - 1
	}
	var keyRefs, valRefs []int
	for _, k := range keys {
		keyRefs = append(keyRefs, ref(bpString(k)))
	}
	for _, k := range keys {
		var o []byte
		switch v := m[k].(type) {
		case string:
			o = bpString(v)
		case bool:
			o = []byte{0x08}
			if v {
				o = []byte{0x09}
			}
		case int64:
			o = append([]byte{0x13}, binary.BigEndian.AppendUint64(nil, uint64(v))...)
		case float64:
			o = append([]byte{0x23}, binary.BigEndian.AppendUint64(nil, math.Float64bits(v))...)
		}
		valRefs = append(valRefs, ref(o))
	}
	dict := bpLength(0xd0, len(keys))
	for _, r := range append(keyRefs, valRefs...) {
		dict = append(dict, byte(r)) // a byte per reference: under 256 objects
	}
	objs[0] = dict

	out := []byte("bplist00")
	offsets := make([]uint16, len(objs))
	for i, o := range objs {
		offsets[i] = uint16(len(out))
		out = append(out, o...)
	}
	table := len(out)
	for _, off := range offsets {
		out = binary.BigEndian.AppendUint16(out, off)
	}
	trailer := make([]byte, 32)
	trailer[6] = 2 // offsets' size
	trailer[7] = 1 // references' size
	binary.BigEndian.PutUint64(trailer[8:], uint64(len(objs)))
	binary.BigEndian.PutUint64(trailer[16:], 0) // the top object: the dictionary
	binary.BigEndian.PutUint64(trailer[24:], uint64(table))
	return append(out, trailer...)
}

// bpString is an ASCII string's object, or UTF-16's for another.
func bpString(s string) []byte {
	ascii := true
	for i := 0; i < len(s); i++ {
		ascii = ascii && s[i] < 0x80
	}
	if ascii {
		return append(bpLength(0x50, len(s)), s...)
	}
	u := utf16.Encode([]rune(s))
	b := bpLength(0x60, len(u))
	for _, c := range u {
		b = binary.BigEndian.AppendUint16(b, c)
	}
	return b
}

// bpLength is an object's marker with its length: in its low four bits,
// or after it as an integer from 15 on.
func bpLength(marker byte, n int) []byte {
	if n < 15 {
		return []byte{marker | byte(n)}
	}
	if n < 256 {
		return []byte{marker | 0x0f, 0x10, byte(n)}
	}
	return []byte{marker | 0x0f, 0x11, byte(n >> 8), byte(n)}
}
