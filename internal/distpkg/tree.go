// Package distpkg writes the packages a desktop app is handed out in, in
// Go, from any system: a Debian package (.deb), an RPM (.rpm), and an
// AppImage (a SquashFS image of the app after the AppImage runtime). No
// dpkg, rpmbuild or mksquashfs: each format is written here, as its
// tools write it.
package distpkg

import (
	"crypto/md5"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path"
	"path/filepath"
	"sort"
	"strings"
	"time"
)

// A File is one entry of a package: a regular file, a directory or a
// symbolic link, at the path it installs to.
type File struct {
	// Path is where it installs: absolute, slash-separated
	// (/usr/bin/notes).
	Path string
	// Mode is its type (fs.ModeDir, fs.ModeSymlink, or none for a regular
	// file) and its permissions.
	Mode fs.FileMode
	// Src is a regular file's contents on disk; Data, its contents when
	// Src is "".
	Src  string
	Data []byte
	// Link is a symbolic link's target.
	Link string
}

// IsDir reports whether f is a directory.
func (f *File) IsDir() bool { return f.Mode.IsDir() }

// IsLink reports whether f is a symbolic link.
func (f *File) IsLink() bool { return f.Mode&fs.ModeSymlink != 0 }

// Size is a regular file's size (0 for the others).
func (f *File) Size() (int64, error) {
	if f.IsDir() || f.IsLink() {
		return 0, nil
	}
	if f.Src == "" {
		return int64(len(f.Data)), nil
	}
	st, err := os.Stat(f.Src)
	if err != nil {
		return 0, err
	}
	return st.Size(), nil
}

// Open opens a regular file's contents.
func (f *File) Open() (io.ReadCloser, error) {
	if f.Src == "" {
		return io.NopCloser(strings.NewReader(string(f.Data))), nil
	}
	return os.Open(f.Src)
}

// digests are a regular file's MD5 and SHA-256, hex.
func (f *File) digests() (md5hex, sha256hex string, err error) {
	r, err := f.Open()
	if err != nil {
		return "", "", err
	}
	defer r.Close()
	m, s := md5.New(), sha256.New()
	if _, err := io.Copy(io.MultiWriter(m, s), r); err != nil {
		return "", "", err
	}
	return hex.EncodeToString(m.Sum(nil)), hex.EncodeToString(s.Sum(nil)), nil
}

// FromDir is the tree under dir as it installs under prefix (/usr): its
// directories, files and links, with the modes a package gives them
// (0755 for directories and executables, 0644 for the rest; a file in a
// bin directory is an executable, as a build made on Windows has no
// modes). skip leaves out the entries it's true of, by their path in dir
// (slash-separated).
func FromDir(dir, prefix string, skip func(rel string) bool) ([]File, error) {
	var out []File
	err := filepath.WalkDir(dir, func(p string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		rel, err := filepath.Rel(dir, p)
		if err != nil {
			return err
		}
		rel = filepath.ToSlash(rel)
		if rel == "." {
			return nil
		}
		if skip != nil && skip(rel) {
			if d.IsDir() {
				return filepath.SkipDir
			}
			return nil
		}
		info, err := d.Info()
		if err != nil {
			return err
		}
		f := File{Path: path.Join(prefix, rel)}
		switch {
		case info.IsDir():
			f.Mode = fs.ModeDir | 0o755
		case info.Mode()&fs.ModeSymlink != 0:
			t, err := os.Readlink(p)
			if err != nil {
				return err
			}
			f.Mode, f.Link = fs.ModeSymlink|0o777, filepath.ToSlash(t)
		case info.Mode().IsRegular():
			f.Src = p
			f.Mode = 0o644
			if info.Mode()&0o111 != 0 || path.Base(path.Dir(rel)) == "bin" {
				f.Mode = 0o755
			}
		default:
			return nil
		}
		out = append(out, f)
		return nil
	})
	return out, err
}

// withParents is files with a directory entry for each directory one is
// in that has none (up to /, which has none), sorted by path: a package's
// tree as its archive lists it, parents first.
func withParents(files []File) []File {
	have := map[string]bool{}
	for _, f := range files {
		have[f.Path] = true
	}
	out := append([]File(nil), files...)
	for _, f := range files {
		for d := path.Dir(f.Path); d != "/" && d != "." && !have[d]; d = path.Dir(d) {
			have[d] = true
			out = append(out, File{Path: d, Mode: fs.ModeDir | 0o755})
		}
	}
	sortFiles(out)
	return out
}

// sortFiles sorts files by path, bytewise.
func sortFiles(files []File) {
	sort.Slice(files, func(i, j int) bool { return files[i].Path < files[j].Path })
}

// check reports what's wrong with files: a path that isn't absolute or
// clean, or one given twice.
func check(files []File) error {
	seen := map[string]bool{}
	for _, f := range files {
		if !path.IsAbs(f.Path) || path.Clean(f.Path) != f.Path || f.Path == "/" {
			return fmt.Errorf("distpkg: %q isn't an absolute, clean path", f.Path)
		}
		if seen[f.Path] {
			return fmt.Errorf("distpkg: %s is in the package twice", f.Path)
		}
		seen[f.Path] = true
	}
	return nil
}

// Epoch is the time a package's files and its build are dated:
// SOURCE_DATE_EPOCH's (reproducible builds), else now.
func Epoch() time.Time {
	if s := os.Getenv("SOURCE_DATE_EPOCH"); s != "" {
		var n int64
		if _, err := fmt.Sscan(s, &n); err == nil && n >= 0 {
			return time.Unix(n, 0).UTC()
		}
	}
	return time.Now().UTC()
}
