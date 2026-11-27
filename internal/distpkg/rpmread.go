package distpkg

import (
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"fmt"
	"os"
	"strconv"
)

// Reading an RPM's headers, as a repository's metadata needs them (yum.go):
// what the package is, what it needs and has, and where its header is.

// rpmHeaderData is a header read: its entries' data by tag.
type rpmHeaderData struct {
	entries map[uint32]rpmEntry
	store   []byte
}

type rpmEntry struct {
	typ, offset, count uint32
}

var errNotRPM = errors.New("not an RPM")

// readRPMHeaderAt reads the header at b's start, and returns it with its
// length.
func readRPMHeaderAt(b []byte) (*rpmHeaderData, int, error) {
	if len(b) < 16 || !bytes.Equal(b[:4], []byte{0x8e, 0xad, 0xe8, 0x01}) {
		return nil, 0, errNotRPM
	}
	n := binary.BigEndian.Uint32(b[8:])
	size := binary.BigEndian.Uint32(b[12:])
	if n > 1<<16 || size > 1<<28 || 16+16*int(n)+int(size) > len(b) {
		return nil, 0, errNotRPM
	}
	h := &rpmHeaderData{entries: map[uint32]rpmEntry{}}
	for i := range int(n) {
		e := b[16+16*i:]
		h.entries[binary.BigEndian.Uint32(e)] = rpmEntry{binary.BigEndian.Uint32(e[4:]), binary.BigEndian.Uint32(e[8:]), binary.BigEndian.Uint32(e[12:])}
	}
	start := 16 + 16*int(n)
	h.store = b[start : start+int(size)]
	return h, start + int(size), nil
}

func (h *rpmHeaderData) strings(tag uint32) []string {
	e, ok := h.entries[tag]
	if !ok || int(e.offset) > len(h.store) || (e.typ != rpmString && e.typ != rpmStringArray && e.typ != rpmI18NString) {
		return nil
	}
	var out []string
	rest := h.store[e.offset:]
	for range e.count {
		i := bytes.IndexByte(rest, 0)
		if i < 0 {
			break
		}
		out = append(out, string(rest[:i]))
		rest = rest[i+1:]
	}
	return out
}

func (h *rpmHeaderData) string(tag uint32) string {
	if s := h.strings(tag); len(s) > 0 {
		return s[0]
	}
	return ""
}

func (h *rpmHeaderData) ints(tag uint32) []int64 {
	e, ok := h.entries[tag]
	if !ok {
		return nil
	}
	w := map[uint32]int{rpmInt16: 2, rpmInt32: 4, 5: 8}[e.typ]
	if w == 0 || int(e.offset)+w*int(e.count) > len(h.store) {
		return nil
	}
	out := make([]int64, e.count)
	for i := range out {
		p := h.store[int(e.offset)+w*i:]
		switch w {
		case 2:
			out[i] = int64(binary.BigEndian.Uint16(p))
		case 4:
			out[i] = int64(binary.BigEndian.Uint32(p))
		default:
			out[i] = int64(binary.BigEndian.Uint64(p))
		}
	}
	return out
}

func (h *rpmHeaderData) int(tag uint32) (int64, bool) {
	if v := h.ints(tag); len(v) > 0 {
		return v[0], true
	}
	return 0, false
}

func (h *rpmHeaderData) bin(tag uint32) []byte {
	e, ok := h.entries[tag]
	if !ok || e.typ != rpmBin || int(e.offset)+int(e.count) > len(h.store) {
		return nil
	}
	return h.store[e.offset : e.offset+e.count]
}

// An RPMDep is a dependency as a header lists it.
type RPMDep struct {
	Name, Version string
	Flags         int64
}

// An RPMFile is a file the package installs.
type RPMFile struct {
	Path string
	Dir  bool
}

// RPMInfo is what a repository's metadata says of an RPM.
type RPMInfo struct {
	Name, Epoch, Version, Release, Arch          string
	Summary, Description, Packager, URL          string
	License, Vendor, Group, BuildHost, SourceRPM string
	BuildTime                                    int64
	InstalledSize, ArchiveSize                   int64
	Provides, Requires, Recommends               []RPMDep
	Conflicts, Obsoletes, Suggests               []RPMDep
	Files                                        []RPMFile
	// HeaderStart and HeaderEnd are the header's range in the file.
	HeaderStart, HeaderEnd int64
	// Size is the file's, SHA256 its digest (the package's ID).
	Size   int64
	SHA256 string
	// Signed reports whether it has an OpenPGP signature.
	Signed bool
}

// The tags read here beyond those WriteRPM writes.
const (
	tagEpoch          = 1003
	tagConflictFlags  = 1053
	tagConflictName   = 1054
	tagConflictVer    = 1055
	tagObsoleteName   = 1090
	tagObsoleteFlags  = 1114
	tagObsoleteVer    = 1115
	tagSuggestName    = 5049
	tagSuggestVersion = 5050
	tagSuggestFlags   = 5051
)

// ReadRPM reads what a repository says of the RPM at path.
func ReadRPM(path string) (*RPMInfo, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	info, err := ParseRPM(b)
	if err != nil {
		return nil, fmt.Errorf("%s: %w", path, err)
	}
	return info, nil
}

// ParseRPM reads what a repository says of the RPM b.
func ParseRPM(b []byte) (*RPMInfo, error) {
	if len(b) < 96 || !bytes.Equal(b[:4], []byte{0xed, 0xab, 0xee, 0xdb}) {
		return nil, errNotRPM
	}
	sig, n, err := readRPMHeaderAt(b[96:])
	if err != nil {
		return nil, err
	}
	start := 96 + n
	if r := start % 8; r != 0 {
		start += 8 - r
	}
	if start > len(b) {
		return nil, errNotRPM
	}
	h, m, err := readRPMHeaderAt(b[start:])
	if err != nil {
		return nil, err
	}
	sum := sha256.Sum256(b)
	info := &RPMInfo{
		Name: h.string(tagName), Version: h.string(tagVersion), Release: h.string(tagRelease), Arch: h.string(tagArch),
		Summary: h.string(tagSummary), Description: h.string(tagDescription), Packager: h.string(tagPackager), URL: h.string(tagURL),
		License: h.string(tagLicense), Vendor: h.string(tagVendor), Group: h.string(tagGroup), BuildHost: h.string(tagBuildHost),
		SourceRPM:   h.string(tagSourceRPM),
		HeaderStart: int64(start), HeaderEnd: int64(start + m),
		Size: int64(len(b)), SHA256: hex.EncodeToString(sum[:]),
		Signed: sig.bin(sigRSA) != nil || sig.bin(sigDSA) != nil,
	}
	if e, ok := h.int(tagEpoch); ok {
		info.Epoch = strconv.FormatInt(e, 10)
	}
	info.BuildTime, _ = h.int(tagBuildTime)
	info.InstalledSize, _ = h.int(tagSize)
	info.ArchiveSize, _ = sig.int(sigPayloadSize)
	deps := func(names, flags, versions uint32) []RPMDep {
		ns, fs, vs := h.strings(names), h.ints(flags), h.strings(versions)
		var out []RPMDep
		for i, name := range ns {
			d := RPMDep{Name: name}
			if i < len(fs) {
				d.Flags = fs[i]
			}
			if i < len(vs) {
				d.Version = vs[i]
			}
			out = append(out, d)
		}
		return out
	}
	info.Provides = deps(tagProvideName, tagProvideFlags, tagProvideVersion)
	info.Requires = deps(tagRequireName, tagRequireFlags, tagRequireVersion)
	info.Recommends = deps(tagRecommendName, tagRecommendFlags, tagRecommendVersion)
	info.Conflicts = deps(tagConflictName, tagConflictFlags, tagConflictVer)
	info.Obsoletes = deps(tagObsoleteName, tagObsoleteFlags, tagObsoleteVer)
	info.Suggests = deps(tagSuggestName, tagSuggestFlags, tagSuggestVersion)
	dirs, bases, idx, modes := h.strings(tagDirNames), h.strings(tagBaseNames), h.ints(tagDirIndexes), h.ints(tagFileModes)
	for i, base := range bases {
		if i >= len(idx) || int(idx[i]) >= len(dirs) {
			return nil, errNotRPM
		}
		f := RPMFile{Path: dirs[idx[i]] + base}
		if i < len(modes) {
			f.Dir = modes[i]&0o170000 == 0o040000
		}
		info.Files = append(info.Files, f)
	}
	return info, nil
}
