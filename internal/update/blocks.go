package update

import (
	"bytes"
	"compress/zlib"
	"encoding/binary"
	"errors"
	"io"

	"github.com/egoist/asuka/internal/distpkg"
)

// A patch of two AppImages with their blocks decompressed (an entry's
// Blocks "zlib8"): its Data is
//
//	the size of the new file expanded (uvarint),
//	the old file's blocks: their number, then each block's distance from
//	the end of the one before (the first's from the file's start), its
//	length and its size decompressed (uvarints),
//	the new file's blocks, likewise,
//	the bsdiff patch (bsdiff.go) of the old file expanded into the new
//	file expanded,
//
// a file expanded being the file with each of its blocks decompressed in
// its place. The updater expands its file by the old blocks (zlib
// streams), patches that, and makes the new file of what the patch made:
// the bytes between the new blocks as they are, each block compressed
// with distpkg.Zlib8 (which must make its length).

// blockPatch is the Data of an entry that turns the AppImage prev into
// the AppImage b, patched with their blocks decompressed; nil when they
// aren't both AppImages (distpkg.AppImageBlocks), or their expanded
// files are larger than deltas patch. Only the blocks of b that Zlib8
// makes again are expanded: the others are patched as they are.
func blockPatch(prev, b []byte) []byte {
	oldBlocks, err := distpkg.AppImageBlocks(prev)
	if err != nil {
		return nil
	}
	newBlocks, err := distpkg.AppImageBlocks(b)
	if err != nil {
		return nil
	}
	oldX, err := expand(prev, oldBlocks)
	if err != nil {
		return nil
	}
	kept := newBlocks[:0:0]
	for _, k := range newBlocks {
		blk := b[k.Off : k.Off+k.Len]
		if d, err := inflateBlock(blk, k.Size); err == nil && bytes.Equal(distpkg.Zlib8(d), blk) {
			kept = append(kept, k)
		}
	}
	newX, err := expand(b, kept)
	if err != nil || len(oldX) > maxDiff || len(newX) > maxDiff {
		return nil
	}
	head := binary.AppendUvarint(nil, uint64(len(newX)))
	head = appendBlockMap(head, oldBlocks)
	head = appendBlockMap(head, kept)
	return append(head, Diff(oldX, newX)...)
}

func appendBlockMap(b []byte, blocks []distpkg.ImageBlock) []byte {
	b = binary.AppendUvarint(b, uint64(len(blocks)))
	end := int64(0)
	for _, k := range blocks {
		b = binary.AppendUvarint(b, uint64(k.Off-end))
		b = binary.AppendUvarint(b, uint64(k.Len))
		b = binary.AppendUvarint(b, uint64(k.Size))
		end = k.Off + k.Len
	}
	return b
}

// expand is f with each of blocks (in order, apart) decompressed in its
// place.
func expand(f []byte, blocks []distpkg.ImageBlock) ([]byte, error) {
	var out []byte
	pos := int64(0)
	for _, k := range blocks {
		if k.Off < pos || k.Off+k.Len > int64(len(f)) {
			return nil, errBlockMap
		}
		out = append(out, f[pos:k.Off]...)
		d, err := inflateBlock(f[k.Off:k.Off+k.Len], k.Size)
		if err != nil {
			return nil, err
		}
		out = append(out, d...)
		pos = k.Off + k.Len
	}
	return append(out, f[pos:]...), nil
}

// maxBlock is the largest block a block map may have, decompressed: a
// SquashFS block's (1 MB).
const maxBlock = 1 << 20

// inflateBlock is the zlib stream b decompressed, which must be size
// bytes.
func inflateBlock(b []byte, size int64) ([]byte, error) {
	if size < 0 || size > maxBlock {
		return nil, errBlockMap
	}
	r, err := zlib.NewReader(bytes.NewReader(b))
	if err != nil {
		return nil, err
	}
	d, err := io.ReadAll(io.LimitReader(r, size+1))
	if err != nil {
		return nil, err
	}
	if int64(len(d)) != size {
		return nil, errBlockMap
	}
	return d, nil
}

var errBlockMap = errors.New("the blocks' map isn't the file's")
