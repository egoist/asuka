package distpkg

import (
	"bytes"
	"compress/zlib"
	"debug/elf"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"sort"
)

// An ImageBlock is a compressed block of an AppImage's SquashFS (a
// file's data, or a metadata block of its tables): a zlib stream, Len
// bytes at Off in the AppImage, Size bytes decompressed.
type ImageBlock struct {
	Off, Len, Size int64
}

// maxImageBlock is the largest block SquashFS decompresses to (1 MB).
const maxImageBlock = 1 << 20

// AppImageBlocks are the compressed blocks of the AppImage b's SquashFS,
// in the order they're in the file: its files' blocks (the inodes say
// where) and its tables' metadata blocks. An AppImage update's delta is
// of the AppImages with their blocks decompressed (internal/update's
// WriteFileDelta): a change to a file then changes its bytes alone, not
// every compressed block after it. It reads what WriteSquashFS writes
// (gzip, no fragments, no export table, no extended attributes), and
// fails on anything else.
func AppImageBlocks(b []byte) ([]ImageBlock, error) {
	f, err := elf.NewFile(bytes.NewReader(b))
	if err != nil {
		return nil, fmt.Errorf("not an AppImage: %w", err)
	}
	base := elfEnd(b, f)
	f.Close()
	if len(b) < 11 || string(b[8:11]) != "AI\x02" || base <= 0 || base+96 > int64(len(b)) {
		return nil, errors.New("not a type 2 AppImage")
	}
	img := b[base:]
	le16 := func(p []byte) uint16 { return binary.LittleEndian.Uint16(p) }
	le32 := func(p []byte) uint32 { return binary.LittleEndian.Uint32(p) }
	le64 := func(p []byte) uint64 { return binary.LittleEndian.Uint64(p) }
	if le32(img) != sqfsMagic || le16(img[28:]) != 4 || le16(img[20:]) != sqfsGzip {
		return nil, errors.New("the AppImage's image isn't a SquashFS 4 with gzip")
	}
	blockSize := int64(le32(img[12:]))
	if le32(img[16:]) != 0 || le64(img[56:]) != sqfsNone || le64(img[88:]) != sqfsNone || blockSize <= 0 || blockSize > maxImageBlock {
		return nil, errors.New("the AppImage's SquashFS has fragments, extended attributes or an export table")
	}
	count := int(le32(img[4:]))
	idTable, inodeTable, dirTable := le64(img[48:]), le64(img[64:]), le64(img[72:])
	if idTable+8 > uint64(len(img)) || inodeTable > dirTable || dirTable > idTable {
		return nil, errors.New("the AppImage's SquashFS tables are out of place")
	}
	var blocks []ImageBlock
	// inflate adds the block at [off, off+n) of the image, and returns
	// it decompressed.
	inflate := func(off, n uint64) ([]byte, error) {
		if off+n > uint64(len(img)) || n == 0 {
			return nil, errors.New("a block past the image's end")
		}
		r, err := zlib.NewReader(bytes.NewReader(img[off : off+n]))
		if err != nil {
			return nil, err
		}
		d, err := io.ReadAll(io.LimitReader(r, maxImageBlock+1))
		if err == nil && len(d) > maxImageBlock {
			err = errors.New("a block too large")
		}
		if err != nil {
			return nil, err
		}
		blocks = append(blocks, ImageBlock{Off: base + int64(off), Len: int64(n), Size: int64(len(d))})
		return d, nil
	}
	// meta reads the metadata blocks of [start, end), and returns what
	// they hold.
	meta := func(start, end uint64) ([]byte, error) {
		var out []byte
		for start < end {
			if start+2 > uint64(len(img)) {
				return nil, errors.New("a metadata block past the image's end")
			}
			h := le16(img[start:])
			n := uint64(h &^ sqfsUncompressedMeta)
			if n == 0 || n > sqfsMetaSize || start+2+n > end {
				return nil, errors.New("a damaged metadata block")
			}
			d := img[start+2 : start+2+n]
			if h&sqfsUncompressedMeta == 0 {
				var err error
				if d, err = inflate(start+2, n); err != nil {
					return nil, err
				}
			}
			out = append(out, d...)
			start += 2 + n
		}
		return out, nil
	}
	inodes, err := meta(inodeTable, dirTable)
	if err != nil {
		return nil, err
	}
	// The ids' blocks, which the directories' end at.
	ids := (int(le16(img[26:]))*4 + sqfsMetaSize - 1) / sqfsMetaSize
	if ids == 0 || idTable+uint64(8*ids) > uint64(len(img)) {
		return nil, errors.New("the AppImage's SquashFS has no ids")
	}
	idBlocks := make([]uint64, ids)
	for i := range idBlocks {
		idBlocks[i] = le64(img[idTable+uint64(8*i):])
	}
	if _, err := meta(dirTable, idBlocks[0]); err != nil {
		return nil, err
	}
	for i, at := range idBlocks {
		end := idTable
		if i+1 < len(idBlocks) {
			end = idBlocks[i+1]
		}
		if _, err := meta(at, end); err != nil {
			return nil, err
		}
	}
	// The files' blocks, from their inodes.
	p := inodes
	need := func(n int) error {
		if n > len(p) {
			return errors.New("a damaged inode table")
		}
		return nil
	}
	for range count {
		if err := need(16); err != nil {
			return nil, err
		}
		typ := le16(p)
		var start, size uint64
		var list []byte
		switch typ {
		case sqfsDir:
			if err := need(32); err != nil {
				return nil, err
			}
			p = p[32:]
			continue
		case sqfsLDir:
			if err := need(40); err != nil {
				return nil, err
			}
			n := int(le16(p[32:]))
			p = p[40:]
			for range n {
				if err := need(12); err != nil {
					return nil, err
				}
				l := 12 + int(le32(p[8:])) + 1
				if err := need(l); err != nil {
					return nil, err
				}
				p = p[l:]
			}
			continue
		case sqfsSymlink:
			if err := need(24); err != nil {
				return nil, err
			}
			l := 24 + int(le32(p[20:]))
			if err := need(l); err != nil {
				return nil, err
			}
			p = p[l:]
			continue
		case sqfsFile:
			if err := need(32); err != nil {
				return nil, err
			}
			if le32(p[20:]) != sqfsNoIndex {
				return nil, errors.New("a file with a fragment")
			}
			start, size = uint64(le32(p[16:])), uint64(le32(p[28:]))
			p = p[32:]
		case sqfsLFile:
			if err := need(56); err != nil {
				return nil, err
			}
			if le32(p[44:]) != sqfsNoIndex {
				return nil, errors.New("a file with a fragment")
			}
			start, size = le64(p[16:]), le64(p[24:])
			p = p[56:]
		default:
			return nil, fmt.Errorf("an inode of type %d", typ)
		}
		n := int((size + uint64(blockSize) - 1) / uint64(blockSize))
		if err := need(4 * n); err != nil {
			return nil, err
		}
		list, p = p[:4*n], p[4*n:]
		for i := range n {
			s := le32(list[4*i:])
			l := uint64(s &^ sqfsUncompressedBlock)
			if l == 0 { // sparse: a block of zeros, not stored
				continue
			}
			if s&sqfsUncompressedBlock == 0 {
				if _, err := inflate(start, l); err != nil {
					return nil, err
				}
			}
			start += l
		}
	}
	sort.Slice(blocks, func(i, j int) bool { return blocks[i].Off < blocks[j].Off })
	for i := 1; i < len(blocks); i++ {
		if blocks[i].Off < blocks[i-1].Off+blocks[i-1].Len {
			return nil, errors.New("the AppImage's SquashFS has blocks that overlap")
		}
	}
	return blocks, nil
}
