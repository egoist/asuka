package distpkg

import (
	"archive/tar"
	"bytes"
	"compress/gzip"
	"fmt"
	"io"
	"os"
	"strings"
	"time"
)

// A Package is what a .deb or an .rpm says of the app, and its files.
type Package struct {
	// Name is the package's name (lower case: letters, digits, + - .).
	Name string
	// Version is the app's (1.2.0, 1.3.0-beta.1): each format writes it
	// as its own versions compare (a prerelease before its release).
	Version string
	// Arch is the GOARCH: amd64 or arm64.
	Arch string
	// Summary is a line; Description, paragraphs separated by blank lines.
	Summary, Description string
	// Maintainer is "Name <email>"; Vendor, the developer's name.
	Maintainer, Vendor string
	License, Homepage  string
	// Section is Debian's (utils, devel...); Group, RPM's.
	Section, Group string
	// Depends and Recommends are in each format's own syntax: Debian's
	// "libgtk-4-1 (>= 4.14)" or "a | b", RPM's "gtk4 >= 4.14" or "(a or b)".
	Depends, Recommends []string
	// Files are what it installs.
	Files []File
	// Time dates the files and the build.
	Time time.Time
	// Signer, when there's one, signs the .rpm (its OpenPGP signature,
	// as rpm --addsign makes it).
	Signer Signer
}

// A Signer makes OpenPGP signatures: a key (gpgsign.Key).
type Signer interface {
	// Sign is a detached binary signature of data.
	Sign(data []byte) ([]byte, error)
	// DSA reports whether the key is a DSA key.
	DSA() bool
}

// DebVersion is version as Debian compares it: a prerelease's "-" is a
// "~", which sorts before its release (1.3.0~beta.1 < 1.3.0); a "-" in
// the prerelease itself, a ".".
func DebVersion(version string) string {
	if i := strings.IndexByte(version, '-'); i >= 0 {
		version = version[:i] + "~" + strings.ReplaceAll(version[i+1:], "-", ".")
	}
	return version
}

// DebArch is Debian's name of a GOARCH.
func DebArch(goarch string) string {
	if goarch == "386" {
		return "i386"
	}
	return goarch // amd64, arm64, riscv64
}

// WriteDeb writes p as a Debian package (dpkg-deb's format, version 2.0)
// at file: an ar archive of debian-binary, control.tar.gz (control,
// md5sums) and data.tar.gz (the files, their directories first, owned by
// root). Desktops' caches (the desktop file database, the icon theme's,
// shared-mime-info's) are dpkg triggers of their own packages: the
// package needs no scripts.
func WriteDeb(p *Package, file string) error {
	files := withParents(p.Files)
	if err := check(files); err != nil {
		return err
	}
	mtime := p.Time
	if mtime.IsZero() {
		mtime = Epoch()
	}
	var data, sums bytes.Buffer
	var installed int64
	if err := writeTarGz(&data, func(tw *tar.Writer) error {
		if err := tarEntry(tw, &File{Path: "/", Mode: 0o755 | os.ModeDir}, mtime); err != nil {
			return err
		}
		for i := range files {
			f := &files[i]
			if err := tarEntry(tw, f, mtime); err != nil {
				return err
			}
			// Installed-Size counts each file's KiB, and each other entry
			// as one, as dpkg-gencontrol does.
			n, _ := f.Size()
			installed += (n + 1023) / 1024
			if f.IsDir() || f.IsLink() {
				installed++
				continue
			}
			m, _, err := f.digests()
			if err != nil {
				return err
			}
			fmt.Fprintf(&sums, "%s  %s\n", m, strings.TrimPrefix(f.Path, "/"))
		}
		return nil
	}); err != nil {
		return err
	}
	var control bytes.Buffer
	if err := writeTarGz(&control, func(tw *tar.Writer) error {
		for _, f := range []*File{
			{Path: "/", Mode: 0o755 | os.ModeDir},
			{Path: "/control", Mode: 0o644, Data: []byte(p.debControl(installed))},
			{Path: "/md5sums", Mode: 0o644, Data: sums.Bytes()},
		} {
			if err := tarEntry(tw, f, mtime); err != nil {
				return err
			}
		}
		return nil
	}); err != nil {
		return err
	}
	tmp := file + ".tmp"
	out, err := os.Create(tmp)
	if err != nil {
		return err
	}
	err = writeAr(out, mtime, []arMember{
		{"debian-binary", []byte("2.0\n")},
		{"control.tar.gz", control.Bytes()},
		{"data.tar.gz", data.Bytes()},
	})
	if cerr := out.Close(); err == nil {
		err = cerr
	}
	if err != nil {
		os.Remove(tmp)
		return err
	}
	return os.Rename(tmp, file)
}

// debControl is the package's control file.
func (p *Package) debControl(installedKiB int64) string {
	var b strings.Builder
	field := func(k, v string) {
		if v != "" {
			fmt.Fprintf(&b, "%s: %s\n", k, oneLine(v))
		}
	}
	field("Package", p.Name)
	field("Version", DebVersion(p.Version))
	field("Architecture", DebArch(p.Arch))
	field("Maintainer", p.Maintainer)
	field("Installed-Size", fmt.Sprint(installedKiB))
	field("Depends", strings.Join(p.Depends, ", "))
	field("Recommends", strings.Join(p.Recommends, ", "))
	section := p.Section
	if section == "" {
		section = "misc"
	}
	field("Section", section)
	field("Priority", "optional")
	field("Homepage", p.Homepage)
	summary := p.Summary
	if summary == "" {
		summary = p.Name
	}
	field("Description", summary)
	// The long description: each line indented by a space, a blank line
	// a " .".
	for i, para := range paragraphs(p.Description) {
		if i > 0 {
			b.WriteString(" .\n")
		}
		for _, line := range wrapWords(para, 79) {
			fmt.Fprintf(&b, " %s\n", line)
		}
	}
	return b.String()
}

// oneLine is s on one line: control fields and spec tags are lines.
func oneLine(s string) string { return strings.Join(strings.Fields(s), " ") }

// paragraphs are s's paragraphs (separated by blank lines), each one's
// words joined by spaces.
func paragraphs(s string) []string {
	var out []string
	for _, p := range strings.Split(strings.ReplaceAll(s, "\r\n", "\n"), "\n\n") {
		if p = oneLine(p); p != "" {
			out = append(out, p)
		}
	}
	return out
}

// wrapWords breaks s into lines of at most width bytes where it can.
func wrapWords(s string, width int) []string {
	var lines []string
	line := ""
	for _, w := range strings.Fields(s) {
		if line != "" && len(line)+1+len(w) > width {
			lines = append(lines, line)
			line = ""
		}
		if line != "" {
			line += " "
		}
		line += w
	}
	if line != "" {
		lines = append(lines, line)
	}
	return lines
}

// writeTarGz writes a gzipped tar with entries.
func writeTarGz(w io.Writer, entries func(*tar.Writer) error) error {
	gz, _ := gzip.NewWriterLevel(w, gzip.BestCompression)
	tw := tar.NewWriter(gz)
	if err := entries(tw); err != nil {
		return err
	}
	if err := tw.Close(); err != nil {
		return err
	}
	return gz.Close()
}

// tarEntry writes f into a Debian package's tar: ./usr/bin/notes, owned
// by root, in GNU's format (which every dpkg reads, long names too).
func tarEntry(tw *tar.Writer, f *File, mtime time.Time) error {
	name := "." + f.Path
	h := &tar.Header{
		Name:    name,
		Mode:    int64(f.Mode.Perm()),
		ModTime: mtime,
		Uname:   "root",
		Gname:   "root",
		Format:  tar.FormatGNU,
	}
	switch {
	case f.IsDir():
		h.Typeflag = tar.TypeDir
		if !strings.HasSuffix(h.Name, "/") {
			h.Name += "/"
		}
	case f.IsLink():
		h.Typeflag, h.Linkname = tar.TypeSymlink, f.Link
	default:
		h.Typeflag = tar.TypeReg
		n, err := f.Size()
		if err != nil {
			return err
		}
		h.Size = n
	}
	if err := tw.WriteHeader(h); err != nil {
		return err
	}
	if h.Typeflag != tar.TypeReg {
		return nil
	}
	r, err := f.Open()
	if err != nil {
		return err
	}
	defer r.Close()
	_, err = io.Copy(tw, r)
	return err
}

type arMember struct {
	name string
	data []byte
}

// writeAr writes a Unix ar archive (the common format: names of 16
// bytes at most), as dpkg reads it.
func writeAr(w io.Writer, mtime time.Time, members []arMember) error {
	if _, err := io.WriteString(w, "!<arch>\n"); err != nil {
		return err
	}
	for _, m := range members {
		h := fmt.Sprintf("%-16s%-12d%-6d%-6d%-8s%-10d`\n", m.name, mtime.Unix(), 0, 0, "100644", len(m.data))
		if len(h) != 60 {
			return fmt.Errorf("distpkg: the ar member %q doesn't fit its header", m.name)
		}
		if _, err := io.WriteString(w, h); err != nil {
			return err
		}
		if _, err := w.Write(m.data); err != nil {
			return err
		}
		if len(m.data)%2 == 1 {
			if _, err := io.WriteString(w, "\n"); err != nil {
				return err
			}
		}
	}
	return nil
}
