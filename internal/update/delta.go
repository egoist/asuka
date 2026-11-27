package update

import (
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path"
	"path/filepath"
)

// Delta updates, as Sparkle's: a delta turns the app of an earlier version
// into the new one, and holds only what changed. Its index lists the tree
// of the new app: directories, links, and files with their size and
// SHA-256, each a copy of a file of the old app, a patch of one
// (bsdiff.go), or new. A delta is
//
//	deltaMagic, the length of the index (uvarint), the index (JSON, DEFLATE),
//	the data of the files, in the order of the index: a patch, or the
//	file compressed with DEFLATE,
//
// and is signed like an archive. The data is read from the delta file in
// place, and applying it checks every file it makes, so that a delta
// applied to another app than the one it was made from fails: the updater
// then downloads the archive.

const deltaMagic = "asuka delta 1\n"

type deltaIndex struct {
	// From and Version are the versions of the app the delta updates and
	// makes.
	From    string       `json:"from"`
	Version string       `json:"version"`
	Entries []deltaEntry `json:"entries"`
}

// deltaEntry is a directory, a link or a file of the new app. Paths are
// relative to the app: its bundle on macOS, its directory elsewhere.
type deltaEntry struct {
	Path string `json:"path"`
	Dir  bool   `json:"dir,omitzero"`
	// Link is the target of a symbolic link.
	Link string `json:"link,omitzero"`
	// Mode holds the permissions of a file.
	Mode   uint32 `json:"mode,omitzero"`
	Size   int64  `json:"size,omitzero"`
	SHA256 string `json:"sha256,omitzero"`
	// From is the file of the old app that the file copies, or patches
	// with its Data. Without From, Data is the file, compressed.
	From string `json:"from,omitzero"`
	Data int64  `json:"data,omitzero"`
	// Blocks, on a patch of a file whose compressed blocks it patches
	// decompressed (an AppImage's: WriteFileDelta), is how the new file's
	// blocks are compressed again: "zlib8", zlib at level 8 as the core
	// compresses (distpkg.Zlib8). Its Data is then the blocks' map
	// (blockMap) and the patch.
	Blocks string `json:"blocks,omitzero"`
}

// blocksZlib8 is deltaEntry.Blocks for blocks that are distpkg.Zlib8's.
const blocksZlib8 = "zlib8"

// maxDiff is the size of the largest files that deltas patch: making the
// patch takes about ten times as much memory. Larger files that changed
// are included whole.
const maxDiff = 256 << 20

// stapledTicket is where stapling writes the notarization ticket of an app
// bundle. The archive's app has it, but the app installed from the disk
// image does not (dmg.go staples the app after copying it into the image),
// so deltas include it whole, never as a copy or patch of the old app's.
const stapledTicket = "Contents/CodeResources"

// WriteDelta writes the delta that turns the app in oldDir, of version from,
// into the app in newDir, of version to: the entries of newDir, or all of
// it when entries is nil.
func WriteDelta(w io.Writer, from, to, oldDir, newDir string, entries []string) error {
	// The files of the old app, and the first of each content.
	type oldFile struct {
		sum  string // SHA-256
		size int64
	}
	old := map[string]oldFile{}
	bySum := map[string]string{}
	err := filepath.WalkDir(oldDir, func(p string, d fs.DirEntry, err error) error {
		if err != nil || !d.Type().IsRegular() {
			return err
		}
		// The FileList too is included whole: the updater rewrites the
		// installed app's, which may then differ from the archive's.
		if rel := relPath(oldDir, p); rel == stapledTicket || path.Base(rel) == FileList {
			return nil
		}
		f, err := os.Open(p)
		if err != nil {
			return err
		}
		defer f.Close()
		h := sha256.New()
		size, err := io.Copy(h, f)
		if err != nil {
			return err
		}
		rel, sum := relPath(oldDir, p), hex.EncodeToString(h.Sum(nil))
		old[rel] = oldFile{sum, size}
		if _, ok := bySum[sum]; !ok {
			bySum[sum] = rel
		}
		return nil
	})
	if err != nil {
		return err
	}
	if entries == nil {
		all, err := os.ReadDir(newDir)
		if err != nil {
			return err
		}
		for _, e := range all {
			entries = append(entries, e.Name())
		}
	}

	index := deltaIndex{From: from, Version: to}
	var data [][]byte
	for _, top := range entries {
		err := filepath.WalkDir(filepath.Join(newDir, top), func(p string, d fs.DirEntry, err error) error {
			if err != nil {
				return err
			}
			e := deltaEntry{Path: relPath(newDir, p)}
			switch {
			case d.IsDir():
				e.Dir = true
			case d.Type()&fs.ModeSymlink != 0:
				if e.Link, err = os.Readlink(p); err != nil {
					return err
				}
				e.Link = filepath.ToSlash(e.Link)
			case d.Type().IsRegular():
				info, err := d.Info()
				if err != nil {
					return err
				}
				b, err := os.ReadFile(p)
				if err != nil {
					return err
				}
				sum := sha256.Sum256(b)
				e.Mode, e.Size, e.SHA256 = uint32(info.Mode().Perm()), int64(len(b)), hex.EncodeToString(sum[:])
				o := old[e.Path]
				if o.sum == e.SHA256 {
					e.From = e.Path
				} else if src, ok := bySum[e.SHA256]; ok {
					e.From = src // moved, or copied
				} else {
					blob := deflate(b)
					if o.size > 0 && o.size <= maxDiff && len(b) <= maxDiff {
						prev, err := os.ReadFile(filepath.Join(oldDir, filepath.FromSlash(e.Path)))
						if err != nil {
							return err
						}
						if patch := Diff(prev, b); len(patch) < len(blob) {
							e.From, blob = e.Path, patch
						}
					}
					e.Data = int64(len(blob))
					data = append(data, blob)
				}
			default:
				return fmt.Errorf("%s is not a file, directory or link", p)
			}
			index.Entries = append(index.Entries, e)
			return nil
		})
		if err != nil {
			return err
		}
	}

	return writeDeltaData(w, index, data)
}

// WriteFileDelta writes the delta that turns the file oldFile, of version
// from, into newFile, of version to: a delta of one file (an AppImage's),
// named as newFile is, a bsdiff patch of oldFile (its From, named as
// oldFile is), else newFile compressed when that's smaller. The updater
// applies it to the file it runs from, whatever its name, and checks the
// file it makes against the index's size and SHA-256. Files larger than
// deltas patch (256 MB) have none.
//
// Two AppImages are patched with their SquashFS's blocks decompressed
// (blockPatch): a change to the app's executable moves the bytes after
// it, which changes every compressed block from there on, but not the
// decompressed bytes. The updater decompresses its AppImage's blocks,
// patches that, and compresses the new AppImage's blocks again as the
// AppImage was built (distpkg.Zlib8): byte for byte the AppImage that
// was signed.
func WriteFileDelta(w io.Writer, from, to, oldFile, newFile string) error {
	info, err := os.Stat(newFile)
	if err != nil {
		return err
	}
	oldInfo, err := os.Stat(oldFile)
	if err != nil {
		return err
	}
	if !info.Mode().IsRegular() || !oldInfo.Mode().IsRegular() {
		return fmt.Errorf("a delta of %s from %s: not files", newFile, oldFile)
	}
	if info.Size() > maxDiff || oldInfo.Size() > maxDiff {
		return fmt.Errorf("%s is larger than deltas patch (%d MB)", filepath.Base(newFile), maxDiff>>20)
	}
	b, err := os.ReadFile(newFile)
	if err != nil {
		return err
	}
	prev, err := os.ReadFile(oldFile)
	if err != nil {
		return err
	}
	sum := sha256.Sum256(b)
	e := deltaEntry{Path: filepath.Base(newFile), Mode: uint32(info.Mode().Perm()), Size: int64(len(b)), SHA256: hex.EncodeToString(sum[:]), From: filepath.Base(oldFile)}
	var blob []byte
	if sha256.Sum256(prev) != sum {
		if blob = blockPatch(prev, b); blob != nil {
			e.Blocks = blocksZlib8
		} else {
			blob = Diff(prev, b)
		}
		if whole := deflate(b); len(whole) <= len(blob) {
			e.Blocks = ""
			e.From, blob = "", whole
		}
		e.Data = int64(len(blob))
	}
	index := deltaIndex{From: from, Version: to, Entries: []deltaEntry{e}}
	var data [][]byte
	if blob != nil {
		data = append(data, blob)
	}
	return writeDeltaData(w, index, data)
}

// writeDeltaData writes a delta: its magic, its index, its files' data.
func writeDeltaData(w io.Writer, index deltaIndex, data [][]byte) error {
	js, err := json.Marshal(index)
	if err != nil {
		return err
	}
	js = deflate(js)
	head := binary.AppendUvarint([]byte(deltaMagic), uint64(len(js)))
	if _, err := w.Write(append(head, js...)); err != nil {
		return err
	}
	for _, b := range data {
		if _, err := w.Write(b); err != nil {
			return err
		}
	}
	return nil
}

func relPath(dir, p string) string {
	rel, _ := filepath.Rel(dir, p)
	return filepath.ToSlash(rel)
}
