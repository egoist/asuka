package distpkg

import (
	"archive/tar"
	"bytes"
	"compress/gzip"
	"crypto/md5"
	"crypto/sha256"
	"encoding/hex"
	"io"
	"io/fs"
	"math/rand"
	"os"
	"path/filepath"
	"slices"
	"strconv"
	"strings"
	"testing"
	"time"
)

// testFiles are an app's files as a package installs them: an
// executable, assets (one of several blocks, incompressible), an empty
// file, an empty directory, a link, and enough files that the tables
// span more than one metadata block.
func testFiles() []File {
	rnd := rand.New(rand.NewSource(1))
	big := make([]byte, 300_000) // three blocks of 128 KiB, the last partial
	rnd.Read(big)
	files := []File{
		{Path: "/usr/bin/notes", Mode: 0o755, Data: []byte("#!/bin/sh\necho notes\n")},
		{Path: "/usr/share/notes/assets/a.txt", Mode: 0o644, Data: []byte("asset\n")},
		{Path: "/usr/share/notes/big.bin", Mode: 0o644, Data: big},
		{Path: "/usr/share/notes/empty", Mode: 0o644},
		{Path: "/usr/share/notes/dir", Mode: fs.ModeDir | 0o755},
		{Path: "/usr/share/notes/link", Mode: fs.ModeSymlink | 0o777, Link: "assets/a.txt"},
		{Path: "/usr/share/applications/com.example.notes.desktop", Mode: 0o644, Data: []byte("[Desktop Entry]\nName=Notes\n")},
	}
	for i := range 400 {
		files = append(files, File{Path: "/usr/share/notes/many/" + strings.Repeat("f", i%50) + strconv.Itoa(i), Mode: 0o644, Data: []byte(strconv.Itoa(i))})
	}
	return files
}

func testPackage() *Package {
	return &Package{
		Name: "notes", Version: "1.3.0-beta.1", Arch: "amd64",
		Summary: "Write things down", Description: "Notes, kept on your computer.\n\nA second paragraph.",
		Maintainer: "Example Inc. <dev@example.com>", Vendor: "Example Inc.", License: "MIT", Homepage: "https://example.com",
		Section: "utils",
		Files:   testFiles(),
		Time:    time.Unix(1_700_000_000, 0),
	}
}

func TestVersions(t *testing.T) {
	for v, want := range map[string]string{"1.2.0": "1.2.0", "1.3.0-beta.1": "1.3.0~beta.1", "1.0.0-alpha-2": "1.0.0~alpha.2", "2.0+build.5": "2.0+build.5"} {
		if got := DebVersion(v); got != want {
			t.Errorf("DebVersion(%q) = %q, want %q", v, got, want)
		}
		if got := RPMVersion(v); got != want || strings.Contains(got, "-") {
			t.Errorf("RPMVersion(%q) = %q", v, got)
		}
	}
	if RPMArch("amd64") != "x86_64" || RPMArch("arm64") != "aarch64" || DebArch("arm64") != "arm64" {
		t.Error("architectures")
	}
}

// arMembers reads an ar archive's members, checking its headers.
func arMembers(t *testing.T, b []byte) map[string][]byte {
	t.Helper()
	if !bytes.HasPrefix(b, []byte("!<arch>\n")) {
		t.Fatal("not an ar archive")
	}
	out := map[string][]byte{}
	var order []string
	for p := 8; p < len(b); {
		h := b[p : p+60]
		if string(h[58:60]) != "`\n" {
			t.Fatalf("a member's header ends %q", h[58:60])
		}
		name := strings.TrimSpace(string(h[:16]))
		if mode := strings.TrimSpace(string(h[40:48])); mode != "100644" {
			t.Errorf("%s: mode %s", name, mode)
		}
		size, err := strconv.Atoi(strings.TrimSpace(string(h[48:58])))
		if err != nil {
			t.Fatal(err)
		}
		out[name] = b[p+60 : p+60+size]
		order = append(order, name)
		p += 60 + size + size%2
	}
	// dpkg wants them in this order.
	if !slices.Equal(order, []string{"debian-binary", "control.tar.gz", "data.tar.gz"}) {
		t.Errorf("members %v", order)
	}
	return out
}

type tarEntryInfo struct {
	h    *tar.Header
	data []byte
}

func readTarGz(t *testing.T, b []byte) ([]string, map[string]tarEntryInfo) {
	t.Helper()
	gz, err := gzip.NewReader(bytes.NewReader(b))
	if err != nil {
		t.Fatal(err)
	}
	tr := tar.NewReader(gz)
	var names []string
	m := map[string]tarEntryInfo{}
	for {
		h, err := tr.Next()
		if err == io.EOF {
			break
		}
		if err != nil {
			t.Fatal(err)
		}
		data, _ := io.ReadAll(tr)
		names = append(names, h.Name)
		m[h.Name] = tarEntryInfo{h, data}
	}
	return names, m
}

func TestDeb(t *testing.T) {
	p := testPackage()
	p.Depends = []string{"libc6 (>= 2.28)", "libgtk-4-1 (>= 4.14)", "libpoppler-glib8t64 | libpoppler-glib8"}
	p.Recommends = []string{"gstreamer1.0-plugins-good"}
	file := filepath.Join(t.TempDir(), "notes.deb")
	if err := WriteDeb(p, file); err != nil {
		t.Fatal(err)
	}
	b, _ := os.ReadFile(file)
	m := arMembers(t, b)
	if string(m["debian-binary"]) != "2.0\n" {
		t.Errorf("debian-binary: %q", m["debian-binary"])
	}
	_, control := readTarGz(t, m["control.tar.gz"])
	c := string(control["./control"].data)
	for _, want := range []string{
		"Package: notes\n", "Version: 1.3.0~beta.1\n", "Architecture: amd64\n",
		"Maintainer: Example Inc. <dev@example.com>\n",
		"Depends: libc6 (>= 2.28), libgtk-4-1 (>= 4.14), libpoppler-glib8t64 | libpoppler-glib8\n",
		"Recommends: gstreamer1.0-plugins-good\n", "Section: utils\n", "Priority: optional\n",
		"Homepage: https://example.com\n",
		"Description: Write things down\n Notes, kept on your computer.\n .\n A second paragraph.\n",
	} {
		if !strings.Contains(c, want) {
			t.Errorf("the control file lacks %q:\n%s", want, c)
		}
	}
	if !strings.Contains(c, "Installed-Size: ") {
		t.Error("no Installed-Size")
	}
	for _, line := range strings.Split(strings.TrimSuffix(c, "\n"), "\n") {
		if line == "" || !strings.HasPrefix(line, " ") && !strings.Contains(line, ": ") {
			t.Errorf("the control file's line %q", line)
		}
	}
	if _, ok := control["./postinst"]; ok {
		t.Error("a postinst: the desktop's caches are its packages' triggers")
	}

	names, data := readTarGz(t, m["data.tar.gz"])
	// Each directory before what's in it, from ./ down.
	if names[0] != "./" || names[1] != "./usr/" || !slices.Contains(names, "./usr/share/notes/assets/") {
		t.Errorf("the entries start %v", names[:3])
	}
	seen := map[string]bool{}
	for _, n := range names {
		if d := n[:strings.LastIndex(strings.TrimSuffix(n, "/"), "/")+1]; n != "./" && !seen[d] {
			t.Errorf("%s comes before its directory %s", n, d)
		}
		seen[n] = true
	}
	sums := string(control["./md5sums"].data)
	for _, f := range p.Files {
		e, ok := data["."+f.Path]
		if f.IsDir() {
			e, ok = data["."+f.Path+"/"]
		}
		if !ok {
			t.Errorf("%s isn't in data.tar.gz", f.Path)
			continue
		}
		if e.h.Uname != "root" || e.h.Gname != "root" || e.h.Uid != 0 {
			t.Errorf("%s: owned by %s:%s", f.Path, e.h.Uname, e.h.Gname)
		}
		if fs.FileMode(e.h.Mode).Perm() != f.Mode.Perm() {
			t.Errorf("%s: mode %o, want %o", f.Path, e.h.Mode, f.Mode.Perm())
		}
		switch {
		case f.IsDir():
			if e.h.Typeflag != tar.TypeDir {
				t.Errorf("%s isn't a directory", f.Path)
			}
		case f.IsLink():
			if e.h.Typeflag != tar.TypeSymlink || e.h.Linkname != f.Link {
				t.Errorf("%s: a link to %q", f.Path, e.h.Linkname)
			}
		default:
			if !bytes.Equal(e.data, f.Data) {
				t.Errorf("%s: its contents differ", f.Path)
			}
			sum := md5.Sum(f.Data)
			if !strings.Contains(sums, hex.EncodeToString(sum[:])+"  "+strings.TrimPrefix(f.Path, "/")+"\n") {
				t.Errorf("md5sums lacks %s", f.Path)
			}
		}
	}
	if strings.Contains(sums, "usr/share/notes/link") || strings.Contains(sums, "usr/share/notes/dir\n") {
		t.Error("md5sums lists a link or a directory")
	}
}

// rpmHeader is a header read back: its entries' data by tag.
type rpmHeader map[uint32]rpmValue

type rpmValue struct {
	typ  uint32
	data []byte
	n    uint32
}

func (v rpmValue) strings() []string {
	var out []string
	b := v.data
	for range v.n {
		i := bytes.IndexByte(b, 0)
		out = append(out, string(b[:i]))
		b = b[i+1:]
	}
	return out
}

func (v rpmValue) int32s() []int32 {
	var out []int32
	for i := range v.n {
		out = append(out, int32(be(v.data[4*i:])))
	}
	return out
}

func (v rpmValue) int16s() []uint16 {
	var out []uint16
	for i := range v.n {
		out = append(out, uint16(v.data[2*i])<<8|uint16(v.data[2*i+1]))
	}
	return out
}

func be(b []byte) uint32 { return uint32(b[0])<<24 | uint32(b[1])<<16 | uint32(b[2])<<8 | uint32(b[3]) }

// readRPMHeader reads a header at b, checking its region, and returns it
// and its length.
func readRPMHeader(t *testing.T, b []byte, region uint32) (rpmHeader, int) {
	t.Helper()
	if !bytes.HasPrefix(b, []byte{0x8e, 0xad, 0xe8, 0x01}) {
		t.Fatal("no header magic")
	}
	n, size := be(b[8:]), be(b[12:])
	index, store := b[16:16+16*n], b[16+16*n:16+16*n+size]
	h := rpmHeader{}
	sizes := map[uint32]int{rpmInt16: 2, rpmInt32: 4}
	for i := range n {
		e := index[16*i:]
		tag, typ, off, count := be(e), be(e[4:]), be(e[8:]), be(e[12:])
		if i == 0 {
			// The region: its trailer at the store's end seals every entry.
			if tag != region || typ != rpmBin || count != 16 || int(off) != len(store)-16 {
				t.Fatalf("the region's entry: %d %d %d %d", tag, typ, off, count)
			}
			tr := store[off:]
			if be(tr) != region || int32(be(tr[8:])) != -16*int32(n) {
				t.Fatalf("the region's trailer: %x", tr)
			}
			continue
		}
		var data []byte
		switch typ {
		case rpmString, rpmI18NString, rpmStringArray:
			end := int(off)
			for range count {
				end += bytes.IndexByte(store[end:], 0) + 1
			}
			data = store[off:end]
		case rpmBin:
			data = store[off : off+count]
		default:
			if int(off)%sizes[typ] != 0 {
				t.Errorf("tag %d's data isn't aligned", tag)
			}
			data = store[off : int(off)+sizes[typ]*int(count)]
		}
		h[tag] = rpmValue{typ, data, count}
	}
	return h, 16 + 16*int(n) + int(size)
}

type cpioFile struct {
	ino, mode uint32
	data      []byte
}

// readCpio reads a newc archive's entries by name.
func readCpio(t *testing.T, b []byte) ([]string, map[string]cpioFile) {
	t.Helper()
	m := map[string]cpioFile{}
	var names []string
	for p := 0; ; {
		h := string(b[p : p+110])
		if h[:6] != "070701" {
			t.Fatalf("at %d: %q", p, h[:6])
		}
		field := func(i int) uint32 {
			v, _ := strconv.ParseUint(h[6+8*i:14+8*i], 16, 32)
			return uint32(v)
		}
		namesize, size := int(field(11)), int(field(6))
		name := string(b[p+110 : p+110+namesize-1])
		p += 110 + namesize
		p += (4 - p%4) % 4
		if name == "TRAILER!!!" {
			return names, m
		}
		m[name] = cpioFile{field(0), field(1), b[p : p+size]}
		names = append(names, name)
		p += size
		p += (4 - p%4) % 4
	}
}

func TestRPM(t *testing.T) {
	p := testPackage()
	p.Depends = []string{"libc.so.6(GLIBC_2.28)(64bit)", "gtk4 >= 4.14", "(poppler-glib or poppler-glib-qt)"}
	p.Recommends = []string{"gstreamer1-plugins-good"}
	file := filepath.Join(t.TempDir(), "notes.rpm")
	owns := func(d string) bool { return d == "/usr/share/notes" || strings.HasPrefix(d, "/usr/share/notes/") }
	if err := WriteRPM(p, owns, file); err != nil {
		t.Fatal(err)
	}
	b, _ := os.ReadFile(file)
	if !bytes.HasPrefix(b, []byte{0xed, 0xab, 0xee, 0xdb, 3, 0}) || len(b) < 96 {
		t.Fatal("no lead")
	}
	if name := string(b[10 : bytes.IndexByte(b[10:76], 0)+10]); name != "notes-1.3.0~beta.1-1" {
		t.Errorf("the lead's name %q", name)
	}
	sig, n := readRPMHeader(t, b[96:], sigHeaderSignatures)
	at := 96 + n
	at += (8 - at%8) % 8
	h, n := readRPMHeader(t, b[at:], tagHeaderImmutable)
	header, payload := b[at:at+n], b[at+n:]

	// The signature's digests are of what follows it.
	s256 := sha256.Sum256(header)
	if got := sig[sigSHA256].strings()[0]; got != hex.EncodeToString(s256[:]) {
		t.Error("the header's SHA-256 differs")
	}
	both := md5.Sum(append(append([]byte(nil), header...), payload...))
	if !bytes.Equal(sig[sigMD5].data, both[:]) {
		t.Error("the MD5 differs")
	}
	if got := sig[sigSize].int32s()[0]; int(got) != len(header)+len(payload) {
		t.Errorf("the size %d, want %d", got, len(header)+len(payload))
	}
	ps := sha256.Sum256(payload)
	if got := h[tagPayloadDigest].strings()[0]; got != hex.EncodeToString(ps[:]) {
		t.Error("the payload's digest differs")
	}

	str := func(tag uint32) string { return h[tag].strings()[0] }
	for tag, want := range map[uint32]string{
		tagName: "notes", tagVersion: "1.3.0~beta.1", tagRelease: "1", tagArch: "x86_64", tagOS: "linux",
		tagSummary: "Write things down", tagDescription: "Notes, kept on your computer.\n\nA second paragraph.",
		tagLicense: "MIT", tagURL: "https://example.com", tagVendor: "Example Inc.",
		tagPayloadFormat: "cpio", tagPayloadCompressor: "gzip", tagSourceRPM: "notes-1.3.0~beta.1-1.src.rpm",
	} {
		if got := str(tag); got != want {
			t.Errorf("tag %d: %q, want %q", tag, got, want)
		}
	}
	reqs, vers, flags := h[tagRequireName].strings(), h[tagRequireVersion].strings(), h[tagRequireFlags].int32s()
	want := map[string]string{"gtk4": "4.14", "libc.so.6(GLIBC_2.28)(64bit)": "", "(poppler-glib or poppler-glib-qt)": "", "rpmlib(RichDependencies)": "4.12.0-1"}
	for i, r := range reqs {
		if v, ok := want[r]; ok && v != vers[i] {
			t.Errorf("requires %s %q", r, vers[i])
		}
		if r == "gtk4" && flags[i] != senseGreater|senseEqual {
			t.Errorf("gtk4's sense %x", flags[i])
		}
		if strings.HasPrefix(r, "rpmlib(") && flags[i]&senseRPMLib == 0 {
			t.Errorf("%s isn't rpmlib's", r)
		}
		delete(want, r)
	}
	if len(want) > 0 {
		t.Errorf("requires %v, not %v", reqs, want)
	}
	if got := h[tagRecommendName].strings(); !slices.Equal(got, []string{"gstreamer1-plugins-good"}) {
		t.Errorf("recommends %v", got)
	}
	if got := h[tagProvideName].strings(); !slices.Equal(got, []string{"notes", "notes(x86-64)"}) {
		t.Errorf("provides %v", got)
	}

	// The files: the app's directories owned, the system's not; each in
	// the payload, its mode, size and digest as the header says.
	gz, err := gzip.NewReader(bytes.NewReader(payload))
	if err != nil {
		t.Fatal(err)
	}
	raw, _ := io.ReadAll(gz)
	if got := sig[sigPayloadSize].int32s()[0]; int(got) != len(raw) {
		t.Errorf("the payload's size %d, want %d", got, len(raw))
	}
	cnames, cpio := readCpio(t, raw)
	dirs, bases, idx := h[tagDirNames].strings(), h[tagBaseNames].strings(), h[tagDirIndexes].int32s()
	modes, sizes, digests, links := h[tagFileModes].int16s(), h[tagFileSizes].int32s(), h[tagFileDigests].strings(), h[tagFileLinkTos].strings()
	inodes := h[tagFileInodes].int32s()
	var paths []string
	for i := range bases {
		paths = append(paths, dirs[idx[i]]+bases[i])
	}
	if !slices.IsSorted(paths) {
		t.Error("the files aren't sorted")
	}
	for _, d := range []string{"/usr", "/usr/bin", "/usr/share/applications"} {
		if slices.Contains(paths, d) {
			t.Errorf("it owns the system's %s", d)
		}
	}
	for _, d := range []string{"/usr/share/notes", "/usr/share/notes/assets", "/usr/share/notes/dir"} {
		if !slices.Contains(paths, d) {
			t.Errorf("it doesn't own its %s", d)
		}
	}
	if len(cnames) != len(paths) {
		t.Errorf("%d files in the payload, %d in the header", len(cnames), len(paths))
	}
	byPath := map[string]File{}
	for _, f := range p.Files {
		byPath[f.Path] = f
	}
	for i, path := range paths {
		c, ok := cpio["."+path]
		if !ok {
			t.Errorf("%s isn't in the payload", path)
			continue
		}
		if uint32(modes[i]) != c.mode || c.ino != uint32(inodes[i]) {
			t.Errorf("%s: the header says mode %o inode %d, the payload %o %d", path, modes[i], inodes[i], c.mode, c.ino)
		}
		f, ok := byPath[path]
		if !ok {
			continue // a parent directory it made
		}
		if fileMode(c.mode) != f.Mode {
			t.Errorf("%s: mode %v, want %v", path, fileMode(c.mode), f.Mode)
		}
		switch {
		case f.IsLink():
			if links[i] != f.Link || string(c.data) != f.Link {
				t.Errorf("%s: a link to %q", path, links[i])
			}
		case !f.IsDir():
			sum := sha256.Sum256(f.Data)
			if digests[i] != hex.EncodeToString(sum[:]) || int(sizes[i]) != len(f.Data) || !bytes.Equal(c.data, f.Data) {
				t.Errorf("%s: its size, digest or contents differ", path)
			}
		}
	}
}

func TestSquashFS(t *testing.T) {
	files := testFiles()
	var b bytes.Buffer
	if err := WriteSquashFS(&b, files, time.Unix(1_700_000_000, 0)); err != nil {
		t.Fatal(err)
	}
	if b.Len()%4096 != 0 {
		t.Errorf("the image is %d bytes, not padded to 4 KiB", b.Len())
	}
	got := readSquashFS(t, b.Bytes())
	for _, f := range files {
		e, ok := got[f.Path]
		if !ok {
			t.Errorf("%s isn't in the image", f.Path)
			continue
		}
		if e.mode != f.Mode {
			t.Errorf("%s: mode %v, want %v", f.Path, e.mode, f.Mode)
		}
		switch {
		case f.IsLink():
			if e.link != f.Link {
				t.Errorf("%s: a link to %q", f.Path, e.link)
			}
		case !f.IsDir():
			if !bytes.Equal(e.data, f.Data) {
				t.Errorf("%s: %d bytes, want %d", f.Path, len(e.data), len(f.Data))
			}
		}
	}
	for _, d := range []string{"/usr", "/usr/share", "/usr/share/notes/many"} {
		if e, ok := got[d]; !ok || !e.mode.IsDir() {
			t.Errorf("%s isn't a directory of it", d)
		}
	}
}

func TestDSStore(t *testing.T) {
	b := DSStore(DMGLayout{Width: 600, Height: 380, IconSize: 128, Icons: map[string][2]int{"Notes.app": {160, 180}, "Applications": {440, 180}}})
	recs := readDSStore(t, b)
	var keys []string
	for _, r := range recs {
		keys = append(keys, r.name+" "+r.code)
	}
	want := []string{". bwsp", ". icvp", ". vSrn", ". vstl", "Applications Iloc", "Notes.app Iloc"}
	if !slices.Equal(keys, want) {
		t.Errorf("records %v, want %v", keys, want)
	}
	for _, r := range recs {
		switch r.code {
		case "Iloc":
			x, y := be(r.data), be(r.data[4:])
			if r.name == "Notes.app" && (x != 160 || y != 180) || r.name == "Applications" && (x != 440 || y != 180) {
				t.Errorf("%s at %d,%d", r.name, x, y)
			}
		case "bwsp", "icvp":
			if !bytes.HasPrefix(r.data, []byte("bplist00")) {
				t.Errorf("%s isn't a binary plist", r.code)
			}
		}
	}
	bp := string(recs[0].data)
	if !strings.Contains(bp, "{{200, 120}, {600, 380}}") || !strings.Contains(bp, "ShowToolbar") {
		t.Errorf("the window's plist: %q", bp)
	}
}

type dsRec struct {
	name, code, typ string
	data            []byte
}

// readDSStore reads a .DS_Store's records back, through its allocator's
// root block, table of contents and B-tree.
func readDSStore(t *testing.T, b []byte) []dsRec {
	t.Helper()
	if be(b) != 1 || string(b[4:8]) != "Bud1" || be(b[8:]) != be(b[16:]) {
		t.Fatal("not a .DS_Store")
	}
	block := func(addr uint32) []byte {
		off, n := blockSpan(addr)
		return b[4+off : 4+int(off)+n]
	}
	root := b[4+be(b[8:]):]
	count := be(root)
	addrs := make([]uint32, count)
	for i := range count {
		addrs[i] = be(root[8+4*i:])
	}
	toc := root[8+4*256*((count+255)/256):]
	if be(toc) != 1 || toc[4] != 4 || string(toc[5:9]) != "DSDB" {
		t.Fatalf("the table of contents: %x", toc[:16])
	}
	db := block(addrs[be(toc[9:])])
	node := block(addrs[be(db)])
	if be(node) != 0 {
		t.Fatal("not a leaf")
	}
	n := be(node[4:])
	if be(db[8:]) != n {
		t.Errorf("DSDB says %d records, its node has %d", be(db[8:]), n)
	}
	p := node[8:]
	var out []dsRec
	for range n {
		l := be(p)
		var name []rune
		for i := range l {
			name = append(name, rune(uint16(p[4+2*i])<<8|uint16(p[5+2*i])))
		}
		p = p[4+2*l:]
		r := dsRec{name: string(name), code: string(p[:4]), typ: string(p[4:8])}
		p = p[8:]
		switch r.typ {
		case "blob":
			l := be(p)
			r.data, p = p[4:4+l], p[4+l:]
		case "long", "shor", "type":
			r.data, p = p[:4], p[4:]
		default:
			t.Fatalf("type %q", r.typ)
		}
		out = append(out, r)
	}
	return out
}

func TestAppImage(t *testing.T) {
	rt := fakeRuntime(1024)
	dir := t.TempDir()
	file := filepath.Join(dir, "Notes.AppImage")
	a := &AppImage{
		Runtime:    rt,
		Files:      append(testFiles(), File{Path: "/AppRun", Mode: 0o755, Data: []byte("#!/bin/sh\n")}),
		UpdateInfo: "gh-releases-zsync|example|notes|latest|notes-*-x86_64.AppImage.zsync",
		Time:       time.Unix(1_700_000_000, 0),
	}
	if err := WriteAppImage(a, file); err != nil {
		t.Fatal(err)
	}
	b, _ := os.ReadFile(file)
	if st, _ := os.Stat(file); st.Mode().Perm()&0o111 == 0 {
		t.Error("the AppImage isn't executable")
	}
	// The runtime, its update information in its section, then the image.
	if !bytes.Equal(b[:fakeUpdOffset], rt[:fakeUpdOffset]) {
		t.Error("the runtime changed before its .upd_info")
	}
	if !bytes.HasPrefix(b[fakeUpdOffset:], []byte(a.UpdateInfo+"\x00")) {
		t.Error("the update information isn't in .upd_info")
	}
	got := readSquashFS(t, b[len(rt):])
	if e, ok := got["/AppRun"]; !ok || e.mode != 0o755 {
		t.Errorf("AppRun: %+v", e)
	}
	// A runtime that isn't one: refused.
	if err := WriteAppImage(&AppImage{Runtime: []byte("nope")}, file); err == nil {
		t.Error("a runtime that isn't an ELF")
	}
	if err := WriteAppImage(&AppImage{Runtime: append(fakeRuntime(16), 0)}, file); err == nil {
		t.Error("a runtime with something after its ELF")
	}
}

func TestPackagesFromDir(t *testing.T) {
	dir := t.TempDir()
	os.MkdirAll(filepath.Join(dir, "bin"), 0o755)
	os.MkdirAll(filepath.Join(dir, "share", "notes"), 0o755)
	os.WriteFile(filepath.Join(dir, "bin", "notes"), []byte("exe"), 0o644) // as a build on Windows has it
	os.WriteFile(filepath.Join(dir, "share", "notes", "a.json"), []byte("{}"), 0o600)
	os.WriteFile(filepath.Join(dir, "share", "notes", ".asuka-files"), []byte("x"), 0o644)
	files, err := FromDir(dir, "/usr", func(rel string) bool { return rel == "share/notes/.asuka-files" })
	if err != nil {
		t.Fatal(err)
	}
	modes := map[string]fs.FileMode{}
	for _, f := range files {
		modes[f.Path] = f.Mode
	}
	want := map[string]fs.FileMode{
		"/usr/bin": fs.ModeDir | 0o755, "/usr/bin/notes": 0o755, "/usr/share": fs.ModeDir | 0o755,
		"/usr/share/notes": fs.ModeDir | 0o755, "/usr/share/notes/a.json": 0o644,
	}
	if len(modes) != len(want) {
		t.Errorf("%v", modes)
	}
	for p, m := range want {
		if modes[p] != m {
			t.Errorf("%s: %v, want %v", p, modes[p], m)
		}
	}
}
