package distpkg

import (
	"bytes"
	"compress/gzip"
	"crypto/md5"
	"crypto/sha1"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path"
	"sort"
	"strings"
)

// An RPM (format version 4, as rpmbuild writes it and rpm 4.x reads it):
//
//	the lead          96 bytes, of which rpm reads only the magic
//	the signature     a header: the digests of what follows
//	the header        what the package is: its name, version, files...
//	the payload       its files, a gzipped cpio archive (newc)
//
// A header is an index of tags (tag, type, offset, count) and their data,
// after a region tag that seals them (rpm checks the immutable region).

// The tags' types.
const (
	rpmInt16       = 3
	rpmInt32       = 4
	rpmString      = 6
	rpmBin         = 7
	rpmStringArray = 8
	rpmI18NString  = 9
)

// The signature's tags.
const (
	sigHeaderSignatures = 62
	sigSHA1             = 269
	sigSHA256           = 273
	sigSize             = 1000
	sigMD5              = 1004
	sigPayloadSize      = 1007
)

// The header's tags.
const (
	tagHeaderImmutable   = 63
	tagI18NTable         = 100
	tagName              = 1000
	tagVersion           = 1001
	tagRelease           = 1002
	tagSummary           = 1004
	tagDescription       = 1005
	tagBuildTime         = 1006
	tagBuildHost         = 1007
	tagSize              = 1009
	tagVendor            = 1011
	tagLicense           = 1014
	tagPackager          = 1015
	tagGroup             = 1016
	tagURL               = 1020
	tagOS                = 1021
	tagArch              = 1022
	tagFileSizes         = 1028
	tagFileModes         = 1030
	tagFileRdevs         = 1033
	tagFileMtimes        = 1034
	tagFileDigests       = 1035
	tagFileLinkTos       = 1036
	tagFileFlags         = 1037
	tagFileUserName      = 1039
	tagFileGroupName     = 1040
	tagSourceRPM         = 1044
	tagFileVerifyFlags   = 1045
	tagProvideName       = 1047
	tagRequireFlags      = 1048
	tagRequireName       = 1049
	tagRequireVersion    = 1050
	tagRPMVersion        = 1064
	tagFileDevices       = 1095
	tagFileInodes        = 1096
	tagFileLangs         = 1097
	tagProvideFlags      = 1112
	tagProvideVersion    = 1113
	tagDirIndexes        = 1116
	tagBaseNames         = 1117
	tagDirNames          = 1118
	tagPayloadFormat     = 1124
	tagPayloadCompressor = 1125
	tagPayloadFlags      = 1126
	tagFileDigestAlgo    = 5011
	tagRecommendName     = 5046
	tagRecommendVersion  = 5047
	tagRecommendFlags    = 5048
	tagEncoding          = 5062
	tagPayloadDigest     = 5092
	tagPayloadDigestAlgo = 5093
)

// A dependency's sense.
const (
	senseLess    = 1 << 1
	senseGreater = 1 << 2
	senseEqual   = 1 << 3
	senseRPMLib  = 1 << 24
)

// sha256Algo is PGPHASHALGO_SHA256, the files' and payload's digests.
const sha256Algo = 8

// RPMVersion is version as RPM compares it: no "-" (the release's
// separator), a prerelease after a "~", which sorts before its release
// (1.3.0~beta.1 < 1.3.0).
func RPMVersion(version string) string { return DebVersion(version) }

// RPMArch is RPM's name of a GOARCH.
func RPMArch(goarch string) string {
	switch goarch {
	case "amd64":
		return "x86_64"
	case "arm64":
		return "aarch64"
	case "386":
		return "i686"
	}
	return goarch
}

// rpmRelease is the package's release: the first build of the version.
const rpmRelease = "1"

// WriteRPM writes p as a binary RPM at file. It owns its files and the
// directories that are its own (owns says which: /usr/share/<name>, say),
// not the system's (/usr/bin, /usr/share/icons/hicolor/...), which other
// packages own; rpm makes the directories its files need. The desktop's
// caches are file triggers of their own packages: it has no scripts.
func WriteRPM(p *Package, owns func(dir string) bool, file string) error {
	var files []File
	for _, f := range withParents(p.Files) {
		if f.IsDir() && (owns == nil || !owns(f.Path)) {
			continue
		}
		files = append(files, f)
	}
	if err := check(files); err != nil {
		return err
	}
	mtime := p.Time
	if mtime.IsZero() {
		mtime = Epoch()
	}

	// The payload: the files as a cpio archive, gzipped.
	var payload bytes.Buffer
	gz, _ := gzip.NewWriterLevel(&payload, gzip.BestCompression)
	cw := &countWriter{w: gz}
	var (
		sizes, mtimes, flags, verify, devices, inodes, dirIndexes []int32
		modes, rdevs                                              []int16
		digests, links, users, groups, langs, bases               []string
		dirs                                                      []string
		total                                                     int64
	)
	dirIndex := map[string]int32{}
	for i := range files {
		f := &files[i]
		size, err := f.Size()
		if err != nil {
			return err
		}
		mode := uint32(f.Mode.Perm())
		digest, link := "", ""
		switch {
		case f.IsDir():
			mode |= 0o040000
		case f.IsLink():
			mode |= 0o120000
			link = f.Link
			size = int64(len(link))
		default:
			mode |= 0o100000
			if _, digest, err = f.digests(); err != nil {
				return err
			}
		}
		if size > 0xFFFFFFFF-1 {
			return fmt.Errorf("distpkg: %s is too large for an RPM's sizes", f.Path)
		}
		ino := int32(i + 1)
		if err := cpioEntry(cw, f, ino, mode, size, mtime.Unix()); err != nil {
			return err
		}
		total += size
		dir, base := path.Split(f.Path)
		idx, ok := dirIndex[dir]
		if !ok {
			idx = int32(len(dirs))
			dirIndex[dir] = idx
			dirs = append(dirs, dir)
		}
		sizes = append(sizes, int32(size))
		modes = append(modes, int16(uint16(mode)))
		rdevs = append(rdevs, 0)
		mtimes = append(mtimes, int32(mtime.Unix()))
		digests = append(digests, digest)
		links = append(links, link)
		flags = append(flags, 0)
		users = append(users, "root")
		groups = append(groups, "root")
		verify = append(verify, -1)
		devices = append(devices, 1)
		inodes = append(inodes, ino)
		langs = append(langs, "")
		dirIndexes = append(dirIndexes, idx)
		bases = append(bases, base)
	}
	if err := cpioTrailer(cw); err != nil {
		return err
	}
	if err := gz.Close(); err != nil {
		return err
	}
	payloadSum := sha256.Sum256(payload.Bytes())

	// What it needs and provides.
	name, version := p.Name, RPMVersion(p.Version)
	evr := version + "-" + rpmRelease
	arch := RPMArch(p.Arch)
	var reqs deps
	rich := false
	for _, d := range p.Depends {
		reqs.add(d, 0)
		rich = rich || strings.HasPrefix(strings.TrimSpace(d), "(")
	}
	reqs.add("rpmlib(CompressedFileNames) <= 3.0.4-1", senseRPMLib)
	reqs.add("rpmlib(FileDigests) <= 4.6.0-1", senseRPMLib)
	reqs.add("rpmlib(PayloadFilesHavePrefix) <= 4.0-1", senseRPMLib)
	if rich {
		reqs.add("rpmlib(RichDependencies) <= 4.12.0-1", senseRPMLib)
	}
	var recs deps
	for _, d := range p.Recommends {
		recs.add(d, 0)
	}
	var provs deps
	provs.add(name+" = "+evr, 0)
	provs.add(name+"("+rpmISA(p.Arch)+") = "+evr, 0)

	summary := oneLine(p.Summary)
	if summary == "" {
		summary = name
	}
	desc := strings.Join(paragraphs(p.Description), "\n\n")
	if desc == "" {
		desc = summary
	}
	group := p.Group
	if group == "" {
		group = "Unspecified"
	}
	license := p.License
	if license == "" {
		license = "LicenseRef-not-specified"
	}
	var h header
	h.strings(tagI18NTable, "C")
	h.string(tagName, name)
	h.string(tagVersion, version)
	h.string(tagRelease, rpmRelease)
	h.i18n(tagSummary, summary)
	h.i18n(tagDescription, desc)
	h.int32s(tagBuildTime, int32(mtime.Unix()))
	h.string(tagBuildHost, "localhost")
	h.int32s(tagSize, int32(min(total, 0x7FFFFFFF)))
	if p.Vendor != "" {
		h.string(tagVendor, oneLine(p.Vendor))
	}
	h.string(tagLicense, oneLine(license))
	if p.Maintainer != "" {
		h.string(tagPackager, oneLine(p.Maintainer))
	}
	h.i18n(tagGroup, group)
	if p.Homepage != "" {
		h.string(tagURL, oneLine(p.Homepage))
	}
	h.string(tagOS, "linux")
	h.string(tagArch, arch)
	if len(files) > 0 {
		h.int32s(tagFileSizes, sizes...)
		h.int16s(tagFileModes, modes...)
		h.int16s(tagFileRdevs, rdevs...)
		h.int32s(tagFileMtimes, mtimes...)
		h.strings(tagFileDigests, digests...)
		h.strings(tagFileLinkTos, links...)
		h.int32s(tagFileFlags, flags...)
		h.strings(tagFileUserName, users...)
		h.strings(tagFileGroupName, groups...)
	}
	h.string(tagSourceRPM, name+"-"+evr+".src.rpm") // a binary package's: rpm takes one without it for a source package
	if len(files) > 0 {
		h.int32s(tagFileVerifyFlags, verify...)
	}
	h.strings(tagProvideName, provs.names...)
	h.int32s(tagRequireFlags, reqs.flags...)
	h.strings(tagRequireName, reqs.names...)
	h.strings(tagRequireVersion, reqs.versions...)
	h.string(tagRPMVersion, "4.18.0")
	if len(files) > 0 {
		h.int32s(tagFileDevices, devices...)
		h.int32s(tagFileInodes, inodes...)
		h.strings(tagFileLangs, langs...)
	}
	h.int32s(tagProvideFlags, provs.flags...)
	h.strings(tagProvideVersion, provs.versions...)
	if len(files) > 0 {
		h.int32s(tagDirIndexes, dirIndexes...)
		h.strings(tagBaseNames, bases...)
		h.strings(tagDirNames, dirs...)
	}
	h.string(tagPayloadFormat, "cpio")
	h.string(tagPayloadCompressor, "gzip")
	h.string(tagPayloadFlags, "9")
	h.int32s(tagFileDigestAlgo, sha256Algo)
	if len(recs.names) > 0 {
		h.strings(tagRecommendName, recs.names...)
		h.strings(tagRecommendVersion, recs.versions...)
		h.int32s(tagRecommendFlags, recs.flags...)
	}
	h.string(tagEncoding, "utf-8")
	h.strings(tagPayloadDigest, hex.EncodeToString(payloadSum[:]))
	h.int32s(tagPayloadDigestAlgo, sha256Algo)
	main := h.encode(tagHeaderImmutable)

	// The signature: the header's digests, and the header's and
	// payload's.
	s1, s256 := sha1.Sum(main), sha256.Sum256(main)
	both := md5.New()
	both.Write(main)
	both.Write(payload.Bytes())
	var sig header
	sig.string(sigSHA1, hex.EncodeToString(s1[:]))
	sig.string(sigSHA256, hex.EncodeToString(s256[:]))
	sig.int32s(sigSize, int32(len(main)+payload.Len()))
	sig.bin(sigMD5, both.Sum(nil))
	sig.int32s(sigPayloadSize, int32(cw.n))
	signature := sig.encode(sigHeaderSignatures)
	if pad := len(signature) % 8; pad != 0 {
		signature = append(signature, make([]byte, 8-pad)...)
	}

	tmp := file + ".tmp"
	out, err := os.Create(tmp)
	if err != nil {
		return err
	}
	_, err = out.Write(rpmLead(name + "-" + evr))
	for _, b := range [][]byte{signature, main, payload.Bytes()} {
		if err == nil {
			_, err = out.Write(b)
		}
	}
	if cerr := out.Close(); err == nil {
		err = cerr
	}
	if err != nil {
		os.Remove(tmp)
		return err
	}
	return os.Rename(tmp, file)
}

// rpmISA is the architecture's ISA, as packages provide name(x86-64).
func rpmISA(goarch string) string {
	switch goarch {
	case "amd64":
		return "x86-64"
	case "arm64":
		return "aarch-64"
	}
	return RPMArch(goarch)
}

// rpmLead is the lead: the magic, version 3.0, a binary package, its
// name, the signature's type (a header).
func rpmLead(name string) []byte {
	b := make([]byte, 96)
	copy(b, []byte{0xed, 0xab, 0xee, 0xdb, 3, 0})
	binary.BigEndian.PutUint16(b[6:], 0)  // binary
	binary.BigEndian.PutUint16(b[8:], 1)  // the architecture (rpm reads the header's)
	copy(b[10:75], name)                  // 66 bytes, NUL-terminated
	binary.BigEndian.PutUint16(b[76:], 1) // Linux
	binary.BigEndian.PutUint16(b[78:], 5) // RPMSIGTYPE_HEADERSIG
	return b
}

// deps are dependencies as the header lists them: names, versions and
// senses.
type deps struct {
	names, versions []string
	flags           []int32
}

// add parses "name", "name >= 1.2" or a rich dependency "(a or b)".
func (d *deps) add(s string, sense int32) {
	s = strings.TrimSpace(s)
	name, ver := s, ""
	if !strings.HasPrefix(s, "(") {
		if f := strings.Fields(s); len(f) == 3 {
			ops := map[string]int32{"<": senseLess, "<=": senseLess | senseEqual, "=": senseEqual, "==": senseEqual, ">=": senseGreater | senseEqual, ">": senseGreater}
			if op, ok := ops[f[1]]; ok {
				name, ver, sense = f[0], f[2], sense|op
			}
		}
	}
	d.names = append(d.names, name)
	d.versions = append(d.versions, ver)
	d.flags = append(d.flags, sense)
}

// A header being written: its entries, by tag.
type header struct {
	entries []hentry
}

type hentry struct {
	tag, typ uint32
	count    uint32
	data     []byte
}

func (h *header) add(tag, typ, count uint32, data []byte) {
	h.entries = append(h.entries, hentry{tag, typ, count, data})
}

func (h *header) string(tag uint32, s string) {
	h.add(tag, rpmString, 1, append([]byte(s), 0))
}

func (h *header) i18n(tag uint32, s string) {
	h.add(tag, rpmI18NString, 1, append([]byte(s), 0))
}

func (h *header) strings(tag uint32, ss ...string) {
	var b []byte
	for _, s := range ss {
		b = append(append(b, s...), 0)
	}
	h.add(tag, rpmStringArray, uint32(len(ss)), b)
}

func (h *header) int32s(tag uint32, vs ...int32) {
	b := make([]byte, 4*len(vs))
	for i, v := range vs {
		binary.BigEndian.PutUint32(b[4*i:], uint32(v))
	}
	h.add(tag, rpmInt32, uint32(len(vs)), b)
}

func (h *header) int16s(tag uint32, vs ...int16) {
	b := make([]byte, 2*len(vs))
	for i, v := range vs {
		binary.BigEndian.PutUint16(b[2*i:], uint16(v))
	}
	h.add(tag, rpmInt16, uint32(len(vs)), b)
}

func (h *header) bin(tag uint32, data []byte) {
	h.add(tag, rpmBin, uint32(len(data)), data)
}

// align is the alignment of a type's data in the store.
func align(typ uint32) int {
	switch typ {
	case rpmInt16:
		return 2
	case rpmInt32:
		return 4
	}
	return 1
}

// encode is the header, sealed by region: the magic, the index (the
// region's entry first, then the tags in order) and the data, the
// region's trailer last.
func (h *header) encode(region uint32) []byte {
	es := append([]hentry(nil), h.entries...)
	sort.SliceStable(es, func(i, j int) bool { return es[i].tag < es[j].tag })
	var store []byte
	index := make([]byte, 0, 16*(len(es)+1))
	put := func(tag, typ uint32, off int32, count uint32) {
		var e [16]byte
		binary.BigEndian.PutUint32(e[0:], tag)
		binary.BigEndian.PutUint32(e[4:], typ)
		binary.BigEndian.PutUint32(e[8:], uint32(off))
		binary.BigEndian.PutUint32(e[12:], count)
		index = append(index, e[:]...)
	}
	var entries [][4]uint32
	for _, e := range es {
		if a := align(e.typ); len(store)%a != 0 {
			store = append(store, make([]byte, a-len(store)%a)...)
		}
		entries = append(entries, [4]uint32{e.tag, e.typ, uint32(len(store)), e.count})
		store = append(store, e.data...)
	}
	n := len(es) + 1
	// The region's entry points at its trailer, at the store's end, which
	// says how many entries it seals (as a negative offset).
	put(region, rpmBin, int32(len(store)), 16)
	for _, e := range entries {
		put(e[0], e[1], int32(e[2]), e[3])
	}
	var trailer [16]byte
	binary.BigEndian.PutUint32(trailer[0:], region)
	binary.BigEndian.PutUint32(trailer[4:], rpmBin)
	binary.BigEndian.PutUint32(trailer[8:], uint32(int32(-16*n)))
	binary.BigEndian.PutUint32(trailer[12:], 16)
	store = append(store, trailer[:]...)

	out := []byte{0x8e, 0xad, 0xe8, 0x01, 0, 0, 0, 0}
	out = binary.BigEndian.AppendUint32(out, uint32(n))
	out = binary.BigEndian.AppendUint32(out, uint32(len(store)))
	out = append(out, index...)
	return append(out, store...)
}

// countWriter counts what's written through it.
type countWriter struct {
	w io.Writer
	n int64
}

func (c *countWriter) Write(p []byte) (int, error) {
	n, err := c.w.Write(p)
	c.n += int64(n)
	return n, err
}

// cpioEntry writes f into a newc cpio archive, as ./usr/bin/notes.
func cpioEntry(w io.Writer, f *File, ino int32, mode uint32, size, mtime int64) error {
	name := "." + f.Path
	if err := cpioHeader(w, uint32(ino), mode, 1, uint32(mtime), uint32(size), name); err != nil {
		return err
	}
	switch {
	case f.IsDir():
		return nil
	case f.IsLink():
		if _, err := io.WriteString(w, f.Link); err != nil {
			return err
		}
	default:
		r, err := f.Open()
		if err != nil {
			return err
		}
		n, err := io.Copy(w, r)
		r.Close()
		if err != nil {
			return err
		}
		if n != size {
			return fmt.Errorf("distpkg: %s changed as it was packaged", f.Path)
		}
	}
	return cpioPad(w, size)
}

func cpioTrailer(w io.Writer) error {
	return cpioHeader(w, 0, 0, 1, 0, 0, "TRAILER!!!")
}

// cpioHeader writes a newc header (070701), its name and the padding to
// four bytes.
func cpioHeader(w io.Writer, ino, mode, nlink, mtime, size uint32, name string) error {
	h := fmt.Sprintf("070701%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x",
		ino, mode, 0, 0, nlink, mtime, size, 0, 0, 0, 0, len(name)+1, 0)
	if _, err := io.WriteString(w, h+name+"\x00"); err != nil {
		return err
	}
	return cpioPad(w, int64(len(h)+len(name)+1))
}

func cpioPad(w io.Writer, n int64) error {
	if r := n % 4; r != 0 {
		_, err := w.Write(make([]byte, 4-r))
		return err
	}
	return nil
}

// fileMode is a cpio or header mode's fs.FileMode (for tests).
func fileMode(m uint32) fs.FileMode {
	fm := fs.FileMode(m & 0o777)
	switch m & 0o170000 {
	case 0o040000:
		fm |= fs.ModeDir
	case 0o120000:
		fm |= fs.ModeSymlink
	}
	return fm
}
