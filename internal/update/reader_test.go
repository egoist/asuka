package update

import (
	"bufio"
	"bytes"
	"compress/flate"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path"
	"path/filepath"
	"strings"

	"github.com/egoist/asuka/internal/distpkg"
)

// The reader of what this package writes, as the Go updater had it (the
// apps' is the C core's update.c now): the tests check the writers by
// reading back what they wrote.

func (e *deltaEntry) isFile() bool { return !e.Dir && e.Link == "" }

var errDamagedPatch = errors.New("damaged patch")

// Patch writes to w the file of size bytes that patch makes of old, a file
// of oldSize bytes. Patches come from signed updates, but a damaged one
// fails cleanly: it cannot make more than size bytes, bytes read out of
// old count as zeros, as in bspatch, and the caller checks the file made.
func Patch(w io.Writer, size int64, old io.ReaderAt, oldSize int64, patch io.ReaderAt, patchSize int64) error {
	var head [3 * binary.MaxVarintLen64]byte
	n, _ := patch.ReadAt(head[:min(patchSize, int64(len(head)))], 0)
	var blocks, ctrlLen, diffLen uint64
	start, ok := uvarints(head[:n], &blocks, &ctrlLen, &diffLen)
	if !ok || size < 0 || blocks > uint64(size)+1 ||
		ctrlLen > uint64(patchSize-start) || diffLen > uint64(patchSize-start)-ctrlLen {
		return errDamagedPatch
	}
	extraStart := start + int64(ctrlLen) + int64(diffLen)
	ctrl := bufio.NewReader(flate.NewReader(io.NewSectionReader(patch, start, int64(ctrlLen))))
	diff := flate.NewReader(io.NewSectionReader(patch, start+int64(ctrlLen), int64(diffLen)))
	extra := flate.NewReader(io.NewSectionReader(patch, extraStart, patchSize-extraStart))

	buf := make([]byte, 32<<10)
	oldBuf := make([]byte, len(buf))
	var oldPos, left int64 = 0, size
	for range blocks {
		d, err1 := binary.ReadUvarint(ctrl)
		e, err2 := binary.ReadUvarint(ctrl)
		seek, err3 := binary.ReadVarint(ctrl)
		if err1 != nil || err2 != nil || err3 != nil || d > uint64(left) || e > uint64(left)-d {
			return errDamagedPatch
		}
		left -= int64(d + e)
		for d > 0 {
			n := int(min(d, uint64(len(buf))))
			if _, err := io.ReadFull(diff, buf[:n]); err != nil {
				return errDamagedPatch
			}
			clear(oldBuf[:n])
			// Only the part of the block inside old is read.
			if from, to := max(oldPos, 0), min(oldPos+int64(n), oldSize); from < to {
				if _, err := old.ReadAt(oldBuf[from-oldPos:to-oldPos], from); err != nil {
					return err
				}
			}
			for i := range n {
				buf[i] += oldBuf[i]
			}
			if _, err := w.Write(buf[:n]); err != nil {
				return err
			}
			oldPos += int64(n)
			d -= uint64(n)
		}
		if n, err := io.CopyN(w, extra, int64(e)); n != int64(e) {
			if err == io.EOF || err == io.ErrUnexpectedEOF {
				return errDamagedPatch
			}
			return err
		}
		oldPos += seek
		if oldPos < -(1<<40) || oldPos > 1<<40 {
			return errDamagedPatch
		}
	}
	if left != 0 {
		return errDamagedPatch
	}
	return nil
}

// uvarints decodes uvarints from the start of b into vs and returns how
// many bytes they take.
func uvarints(b []byte, vs ...*uint64) (int64, bool) {
	n := 0
	for _, v := range vs {
		x, k := binary.Uvarint(b[n:])
		if k <= 0 {
			return 0, false
		}
		*v, n = x, n+k
	}
	return int64(n), true
}

// maxDeltaIndex limits the index of a delta that an app reads.
const maxDeltaIndex = 16 << 20

var errDamagedDelta = errors.New("damaged delta update")

// ApplyDelta makes in newDir, which must not exist, the app that the delta
// of size bytes read from r makes of the app in oldDir. The delta must
// update version from to version to, and every file it makes must have
// the size and SHA-256 its index gives.
func ApplyDelta(r io.ReaderAt, size int64, from, to, oldDir, newDir string) error {
	var head [len(deltaMagic) + binary.MaxVarintLen64]byte
	n, _ := r.ReadAt(head[:min(size, int64(len(head)))], 0)
	if !bytes.HasPrefix(head[:n], []byte(deltaMagic)) {
		return errors.New("not a delta update")
	}
	var indexLen uint64
	k, ok := uvarints(head[len(deltaMagic):n], &indexLen)
	start := int64(len(deltaMagic)) + k
	if !ok || indexLen > uint64(size-start) {
		return errDamagedDelta
	}
	js, err := io.ReadAll(io.LimitReader(flate.NewReader(io.NewSectionReader(r, start, int64(indexLen))), maxDeltaIndex+1))
	if err != nil || len(js) > maxDeltaIndex {
		return errDamagedDelta
	}
	var index deltaIndex
	if err := json.Unmarshal(js, &index); err != nil {
		return errDamagedDelta
	}
	if index.From != from || index.Version != to {
		return fmt.Errorf("the delta updates %s to %s, not %s to %s", index.From, index.Version, from, to)
	}

	// Check the index before making anything.
	offset := start + int64(indexLen)
	seen := map[string]bool{}
	for _, e := range index.Entries {
		if !fs.ValidPath(e.Path) || e.Path == "." || seen[e.Path] || e.Size < 0 || e.Data < 0 || e.Data > size-offset ||
			e.Dir && e.Link != "" || !e.isFile() && (e.Size != 0 || e.Data != 0 || e.From != "") ||
			e.From != "" && !fs.ValidPath(e.From) || e.isFile() && e.From == "" && e.Data == 0 {
			return errDamagedDelta
		}
		if e.Link != "" {
			if target := path.Join(path.Dir(e.Path), e.Link); path.IsAbs(e.Link) || !fs.ValidPath(target) {
				return fmt.Errorf("delta update link %q points outside the app", e.Path)
			}
		}
		seen[e.Path] = true
		offset += e.Data
	}
	if offset != size {
		return errDamagedDelta
	}

	if err := os.Mkdir(newDir, 0o755); err != nil {
		return err
	}
	oldRoot, err := os.OpenRoot(oldDir)
	if err != nil {
		return err
	}
	defer oldRoot.Close()
	newRoot, err := os.OpenRoot(newDir)
	if err != nil {
		return err
	}
	defer newRoot.Close()
	offset = start + int64(indexLen)
	for _, e := range index.Entries {
		name := filepath.FromSlash(e.Path)
		if dir := filepath.Dir(name); dir != "." {
			if err := newRoot.MkdirAll(dir, 0o755); err != nil {
				return err
			}
		}
		switch {
		case e.Dir:
			err = newRoot.MkdirAll(name, 0o755)
		case e.Link != "":
			err = newRoot.Symlink(filepath.FromSlash(e.Link), name)
		default:
			err = applyFile(e, io.NewSectionReader(r, offset, e.Data), oldRoot, newRoot)
			offset += e.Data
		}
		if err != nil {
			return err
		}
	}
	return nil
}

// applyFile makes the file of e from data, its data in the delta.
func applyFile(e deltaEntry, data *io.SectionReader, oldRoot, newRoot *os.Root) error {
	f, err := newRoot.OpenFile(filepath.FromSlash(e.Path), os.O_CREATE|os.O_EXCL|os.O_WRONLY, fs.FileMode(e.Mode).Perm()|0o200)
	if err != nil {
		return err
	}
	defer f.Close()
	h := sha256.New()
	out := &limitWriter{w: io.MultiWriter(f, h), n: e.Size}
	if e.From == "" {
		_, err = io.Copy(out, flate.NewReader(data))
	} else {
		var src *os.File
		if src, err = oldRoot.Open(filepath.FromSlash(e.From)); err != nil {
			return err
		}
		defer src.Close()
		var info fs.FileInfo
		switch info, err = src.Stat(); {
		case err != nil:
		case !info.Mode().IsRegular():
			err = fmt.Errorf("%s is not a file", e.From)
		case e.Data == 0:
			_, err = io.Copy(out, src)
		case e.Blocks == blocksZlib8:
			err = patchBlocks(out, e.Size, src, info.Size(), data, e.Data)
		case e.Blocks != "":
			err = errDamagedPatch
		default:
			err = Patch(out, e.Size, src, info.Size(), data, e.Data)
		}
	}
	if err == nil {
		err = f.Close()
	}
	if err == errDamagedPatch || err == errTooLong || err == nil && (out.n != 0 || hex.EncodeToString(h.Sum(nil)) != e.SHA256) {
		// Most likely, the app is not the one the delta was made from.
		return fmt.Errorf("the delta update made %s wrong", e.Path)
	}
	if err == nil {
		err = newRoot.Chmod(filepath.FromSlash(e.Path), fs.FileMode(e.Mode).Perm())
	}
	return err
}

// patchBlocks writes to w the file of size bytes that the patch of
// patchSize bytes in patch makes of old, its blocks decompressed (an
// entry's Blocks "zlib8", blocks.go), as the core does: old expanded by
// the old blocks' map, patched, and the new blocks compressed again.
func patchBlocks(w io.Writer, size int64, old io.ReaderAt, oldSize int64, patch io.ReaderAt, patchSize int64) error {
	head := make([]byte, min(patchSize, 1<<20))
	if _, err := patch.ReadAt(head, 0); err != nil {
		return errDamagedPatch
	}
	r := bytes.NewReader(head)
	newSize, err := binary.ReadUvarint(r)
	if err != nil || newSize > 2*maxDiff {
		return errDamagedPatch
	}
	readMap := func(limit int64) ([]distpkg.ImageBlock, error) {
		n, err := binary.ReadUvarint(r)
		if err != nil || n > uint64(limit) {
			return nil, errDamagedPatch
		}
		var blocks []distpkg.ImageBlock
		end := int64(0)
		for range n {
			var v [3]uint64
			for i := range v {
				if v[i], err = binary.ReadUvarint(r); err != nil || v[i] > uint64(limit) {
					return nil, errDamagedPatch
				}
			}
			k := distpkg.ImageBlock{Off: end + int64(v[0]), Len: int64(v[1]), Size: int64(v[2])}
			if k.Off+k.Len > limit || k.Len == 0 || k.Size > maxBlock {
				return nil, errDamagedPatch
			}
			blocks = append(blocks, k)
			end = k.Off + k.Len
		}
		return blocks, nil
	}
	oldBlocks, err := readMap(oldSize)
	if err != nil {
		return err
	}
	newBlocks, err := readMap(size)
	if err != nil {
		return err
	}
	start := int64(len(head) - r.Len())
	f := make([]byte, oldSize)
	if _, err := old.ReadAt(f, 0); err != nil {
		return err
	}
	oldX, err := expand(f, oldBlocks)
	if err != nil {
		return errDamagedPatch
	}
	var newX bytes.Buffer
	if err := Patch(&newX, int64(newSize), bytes.NewReader(oldX), int64(len(oldX)), io.NewSectionReader(patch, start, patchSize-start), patchSize-start); err != nil {
		return err
	}
	// The new file: x's bytes between the blocks as they are, each block
	// compressed again.
	x := newX.Bytes()
	pos, fileEnd := int64(0), int64(0) // in x, in the file

	for _, k := range newBlocks {
		gap := k.Off - fileEnd
		if pos+gap+k.Size > int64(len(x)) {
			return errDamagedPatch
		}
		if _, err := w.Write(x[pos : pos+gap]); err != nil {
			return err
		}
		z := distpkg.Zlib8(x[pos+gap : pos+gap+k.Size])
		if int64(len(z)) != k.Len {
			return errDamagedPatch
		}
		if _, err := w.Write(z); err != nil {
			return err
		}
		pos += gap + k.Size
		fileEnd = k.Off + k.Len
	}
	_, err = w.Write(x[pos:])
	return err
}

var errTooLong = errors.New("too long")

// limitWriter fails writes past n bytes.
type limitWriter struct {
	w io.Writer
	n int64
}

func (l *limitWriter) Write(b []byte) (int, error) {
	if int64(len(b)) > l.n {
		return 0, errTooLong
	}
	l.n -= int64(len(b))
	return l.w.Write(b)
}

// ParseFileList returns the paths that the content of a FileList names,
// leaving out the lines ValidListPath refuses, which an update must never
// remove.
func ParseFileList(b []byte) []string {
	var names []string
	for line := range strings.Lines(string(b)) {
		if n := strings.TrimRight(line, "\r\n"); ValidListPath(n) {
			names = append(names, n)
		}
	}
	return names
}
