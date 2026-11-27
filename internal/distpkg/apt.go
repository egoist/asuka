package distpkg

import (
	"archive/tar"
	"bytes"
	"compress/gzip"
	"crypto/md5"
	"crypto/sha1"
	"crypto/sha256"
	"crypto/sha512"
	"encoding/hex"
	"errors"
	"fmt"
	"hash"
	"io"
	"os"
	"path"
	"slices"
	"sort"
	"strconv"
	"strings"
	"time"
)

// An apt repository, as reprepro and aptly write one:
//
//	dists/<suite>/Release       what the suite has, and each index's digests
//	dists/<suite>/InRelease     Release, clearsigned
//	dists/<suite>/Release.gpg   Release's detached signature
//	dists/<suite>/main/binary-<arch>/Packages(.gz)
//	                            each package's control fields, where it is
//	                            and its digests
//	pool/main/<n>/<name>/<name>_<version>_<arch>.deb
//
// apt checks InRelease's signature with the key sources.list names
// (signed-by), then each index's digest, then each package's.

// DebControl is the control file of the Debian package at path.
func DebControl(file string) (string, error) {
	f, err := os.Open(file)
	if err != nil {
		return "", err
	}
	defer f.Close()
	members, err := readAr(f)
	if err != nil {
		return "", fmt.Errorf("%s: %w", file, err)
	}
	for name, data := range members {
		if !strings.HasPrefix(name, "control.tar") {
			continue
		}
		var r io.Reader = bytes.NewReader(data)
		switch name {
		case "control.tar":
		case "control.tar.gz":
			zr, err := gzip.NewReader(r)
			if err != nil {
				return "", fmt.Errorf("%s: %w", file, err)
			}
			r = zr
		default:
			return "", fmt.Errorf("%s: its %s isn't one asuka reads (gzip)", file, name)
		}
		tr := tar.NewReader(r)
		for {
			h, err := tr.Next()
			if err != nil {
				return "", fmt.Errorf("%s: no control file: %w", file, err)
			}
			if path.Clean(strings.TrimPrefix(h.Name, "./")) == "control" {
				b, err := io.ReadAll(tr)
				return string(b), err
			}
		}
	}
	return "", fmt.Errorf("%s: not a Debian package", file)
}

// readAr reads an ar archive's members.
func readAr(r io.Reader) (map[string][]byte, error) {
	magic := make([]byte, 8)
	if _, err := io.ReadFull(r, magic); err != nil || string(magic) != "!<arch>\n" {
		return nil, errors.New("not an ar archive")
	}
	out := map[string][]byte{}
	for {
		h := make([]byte, 60)
		if _, err := io.ReadFull(r, h); err == io.EOF {
			return out, nil
		} else if err != nil {
			return nil, err
		}
		size, err := strconv.ParseInt(strings.TrimSpace(string(h[48:58])), 10, 64)
		if err != nil || size < 0 || size > 1<<31 {
			return nil, errors.New("a damaged ar archive")
		}
		data := make([]byte, size)
		if _, err := io.ReadFull(r, data); err != nil {
			return nil, err
		}
		if size%2 == 1 {
			io.ReadFull(r, make([]byte, 1))
		}
		out[strings.TrimSuffix(strings.TrimSpace(string(h[:16])), "/")] = data
	}
}

// A Stanza is a paragraph of control fields, in their order.
type Stanza struct {
	Keys   []string
	Values map[string]string // a multi-line value keeps its lines after the first
}

// Get is field k's value.
func (s *Stanza) Get(k string) string { return s.Values[k] }

// Set sets field k, after the others when it's new.
func (s *Stanza) Set(k, v string) {
	if s.Values == nil {
		s.Values = map[string]string{}
	}
	if _, ok := s.Values[k]; !ok {
		s.Keys = append(s.Keys, k)
	}
	s.Values[k] = v
}

// String is the stanza as a control file writes it.
func (s *Stanza) String() string {
	var b strings.Builder
	for _, k := range s.Keys {
		b.WriteString(k + ":")
		v := s.Values[k]
		if v != "" && !strings.HasPrefix(v, "\n") {
			b.WriteByte(' ')
		}
		b.WriteString(v + "\n")
	}
	return b.String()
}

// ParseStanzas reads control paragraphs (a Packages file's).
func ParseStanzas(text string) []*Stanza {
	var out []*Stanza
	for _, para := range strings.Split(strings.ReplaceAll(text, "\r\n", "\n"), "\n\n") {
		if strings.TrimSpace(para) == "" {
			continue
		}
		s := &Stanza{}
		last := ""
		for _, line := range strings.Split(strings.TrimRight(para, "\n"), "\n") {
			if strings.HasPrefix(line, " ") || strings.HasPrefix(line, "\t") {
				if last != "" {
					s.Values[last] += "\n" + line
				}
				continue
			}
			k, v, ok := strings.Cut(line, ":")
			if !ok {
				continue
			}
			last = k
			s.Set(k, strings.TrimSpace(v))
		}
		if len(s.Keys) > 0 {
			out = append(out, s)
		}
	}
	return out
}

// DebPoolPath is where the package of a control file goes in the
// repository: pool/main/<initial>/<name>/<name>_<version>_<arch>.deb.
func DebPoolPath(control *Stanza) string {
	name, version := control.Get("Package"), control.Get("Version")
	if _, v, ok := strings.Cut(version, ":"); ok { // no epoch in file names
		version = v
	}
	initial := name[:1]
	if strings.HasPrefix(name, "lib") && len(name) > 3 {
		initial = name[:4]
	}
	return "pool/main/" + initial + "/" + name + "/" + name + "_" + version + "_" + control.Get("Architecture") + ".deb"
}

// DebStanza is the Packages entry of the Debian package at file: its
// control file's fields, where it goes in the pool (DebPoolPath), its size
// and digests.
func DebStanza(file string) (*Stanza, error) {
	control, err := DebControl(file)
	if err != nil {
		return nil, err
	}
	ss := ParseStanzas(control)
	if len(ss) != 1 || ss[0].Get("Package") == "" || ss[0].Get("Version") == "" || ss[0].Get("Architecture") == "" {
		return nil, fmt.Errorf("%s: its control file has no Package, Version or Architecture", file)
	}
	s := ss[0]
	b, err := os.ReadFile(file)
	if err != nil {
		return nil, err
	}
	m, s1, s256 := md5.Sum(b), sha1.Sum(b), sha256.Sum256(b)
	s.Set("Filename", DebPoolPath(s))
	s.Set("Size", strconv.Itoa(len(b)))
	s.Set("MD5sum", hex.EncodeToString(m[:]))
	s.Set("SHA1", hex.EncodeToString(s1[:]))
	s.Set("SHA256", hex.EncodeToString(s256[:]))
	return s, nil
}

// AddPackage is a Packages file's text with p in it, in place of the
// package of its name, version and architecture (one built again).
func AddPackage(packages string, p *Stanza) string {
	var b strings.Builder
	for _, s := range ParseStanzas(packages) {
		if s.Get("Package") == p.Get("Package") && s.Get("Version") == p.Get("Version") && s.Get("Architecture") == p.Get("Architecture") {
			continue
		}
		b.WriteString(s.String() + "\n")
	}
	b.WriteString(p.String())
	return b.String()
}

// Gzip is b gzipped, as indexes are served (no name, no time: the same
// bytes for the same text).
func Gzip(b []byte) []byte {
	var out bytes.Buffer
	zw, _ := gzip.NewWriterLevel(&out, gzip.BestCompression)
	zw.Write(b)
	zw.Close()
	return out.Bytes()
}

// An AptRelease is a suite's Release file.
type AptRelease struct {
	Origin, Label, Suite, Codename, Description string
	Date                                        time.Time
	Components                                  []string
	// Files are the suite's indexes, by their path in dists/<suite>
	// (main/binary-amd64/Packages...).
	Files map[string][]byte
	// AcquireByHash says the indexes are also at their digests
	// (ByHashPaths), where apt then fetches them: a Release and the
	// indexes it names can't be of different versions of the suite.
	AcquireByHash bool
}

// ByHashPath is where an index at path (main/binary-amd64/Packages) is
// kept by its SHA-256 (hex), as a Release with Acquire-By-Hash has it:
// main/binary-amd64/by-hash/SHA256/<sum>.
func ByHashPath(index, sum string) string { return byHash(index, "SHA256", sum) }

func byHash(index, hash, sum string) string {
	return path.Join(path.Dir(index), "by-hash", hash, sum)
}

// ByHashPaths are where the index at path with data is kept by its
// digests: by its SHA-256, and by its SHA-512, the strongest a Release
// names, which apt fetches.
func ByHashPaths(index string, data []byte) []string {
	a, b := sha256.Sum256(data), sha512.Sum512(data)
	return []string{byHash(index, "SHA256", hex.EncodeToString(a[:])), byHash(index, "SHA512", hex.EncodeToString(b[:]))}
}

// ReleaseFiles are the indexes a Release names, by their path: their
// size and digests (its SHA256 and SHA512 fields).
func ReleaseFiles(release *Stanza) map[string]ReleaseFile {
	out := map[string]ReleaseFile{}
	for _, field := range []string{"SHA256", "SHA512"} {
		for _, line := range strings.Split(release.Get(field), "\n") {
			f := strings.Fields(line)
			if len(f) != 3 {
				continue
			}
			n, err := strconv.ParseInt(f[1], 10, 64)
			if err != nil {
				continue
			}
			r := out[f[2]]
			r.Size = n
			if field == "SHA256" {
				r.SHA256 = f[0]
			} else {
				r.SHA512 = f[0]
			}
			out[f[2]] = r
		}
	}
	return out
}

// A ReleaseFile is an index a Release names.
type ReleaseFile struct {
	SHA256, SHA512 string
	Size           int64
}

// ByHash are where the index at path is kept by its digests, as
// ByHashPaths has them.
func (f ReleaseFile) ByHash(index string) []string {
	var out []string
	if f.SHA256 != "" {
		out = append(out, byHash(index, "SHA256", f.SHA256))
	}
	if f.SHA512 != "" {
		out = append(out, byHash(index, "SHA512", f.SHA512))
	}
	return out
}

// Architectures are those the release's indexes are of.
func (r *AptRelease) Architectures() []string {
	var out []string
	for p := range r.Files {
		for _, part := range strings.Split(p, "/") {
			if a, ok := strings.CutPrefix(part, "binary-"); ok && !slices.Contains(out, a) {
				out = append(out, a)
			}
		}
	}
	sort.Strings(out)
	return out
}

// Text is the Release file.
func (r *AptRelease) Text() []byte {
	var b strings.Builder
	field := func(k, v string) {
		if v != "" {
			fmt.Fprintf(&b, "%s: %s\n", k, v)
		}
	}
	field("Origin", oneLine(r.Origin))
	field("Label", oneLine(r.Label))
	field("Suite", r.Suite)
	field("Codename", r.Codename)
	field("Date", r.Date.UTC().Format("Mon, 02 Jan 2006 15:04:05 UTC"))
	field("Architectures", strings.Join(r.Architectures(), " "))
	field("Components", strings.Join(r.Components, " "))
	field("Description", oneLine(r.Description))
	if r.AcquireByHash {
		field("Acquire-By-Hash", "yes")
	}
	var paths []string
	for p := range r.Files {
		paths = append(paths, p)
	}
	sort.Strings(paths)
	for _, h := range []struct {
		name string
		new  func() hash.Hash
	}{{"MD5Sum", md5.New}, {"SHA1", sha1.New}, {"SHA256", sha256.New}, {"SHA512", sha512.New}} {
		b.WriteString(h.name + ":\n")
		for _, p := range paths {
			d := h.new()
			d.Write(r.Files[p])
			fmt.Fprintf(&b, " %s %8d %s\n", hex.EncodeToString(d.Sum(nil)), len(r.Files[p]), p)
		}
	}
	return []byte(b.String())
}
