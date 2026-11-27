package distpkg

import (
	"bytes"
	"compress/gzip"
	"crypto/sha256"
	"encoding/hex"
	"encoding/xml"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/ProtonMail/go-crypto/openpgp/armor"
	"github.com/ProtonMail/go-crypto/openpgp/packet"
	"github.com/egoist/asuka/internal/gpgsign"
)

var (
	repoKeyOnce sync.Once
	repoKey     *gpgsign.Key
)

// testKey is an OpenPGP key of the tests', made once (RSA 4096).
func testKey(t *testing.T) *gpgsign.Key {
	t.Helper()
	repoKeyOnce.Do(func() {
		repoKey, _ = gpgsign.Generate("Notes", "dev@example.com", time.Unix(1_700_000_000, 0))
	})
	if repoKey == nil {
		t.Fatal("no key")
	}
	return repoKey
}

// A signed RPM: its signature header has the header's OpenPGP signature
// (RSA: rpm --addsign's tag), which the key verifies over the header's
// bytes, and the digests as before.
func TestRPMSigned(t *testing.T) {
	key := testKey(t)
	p := testPackage()
	p.Depends = []string{"gtk4 >= 4.14"}
	p.Signer = key
	file := filepath.Join(t.TempDir(), "notes.rpm")
	if err := WriteRPM(p, func(d string) bool { return strings.HasPrefix(d, "/usr/share/notes") }, file); err != nil {
		t.Fatal(err)
	}
	b, _ := os.ReadFile(file)
	sig, n := readRPMHeader(t, b[96:], sigHeaderSignatures)
	at := 96 + n
	at += (8 - at%8) % 8
	_, n = readRPMHeader(t, b[at:], tagHeaderImmutable)
	header := b[at : at+n]
	s, ok := sig[sigRSA]
	if !ok || s.typ != rpmBin {
		t.Fatalf("no RSA header signature: %+v", sig)
	}
	pub, _ := key.Public()
	if err := gpgsign.Verify(pub, header, s.data); err != nil {
		t.Errorf("the header's signature: %v", err)
	}
	if _, ok := sig[sigDSA]; ok {
		t.Error("an RSA key's signature in the DSA tag")
	}
	s256 := sha256.Sum256(header)
	if sig[sigSHA256].strings()[0] != hex.EncodeToString(s256[:]) {
		t.Error("the header's SHA-256 differs")
	}
	// Read back as a repository reads it.
	info, err := ReadRPM(file)
	if err != nil {
		t.Fatal(err)
	}
	if !info.Signed || info.Name != "notes" || info.Version != "1.3.0~beta.1" || info.Release != "1" || info.Arch != "x86_64" ||
		info.HeaderStart != int64(at) || info.HeaderEnd != int64(at+n) || info.Size != int64(len(b)) || info.License != "MIT" {
		t.Errorf("read: %+v", info)
	}
	if len(info.Requires) == 0 || info.Requires[0].Name != "gtk4" || info.Requires[0].Version != "4.14" || rpmFlag(info.Requires[0].Flags) != "GE" {
		t.Errorf("requires: %+v", info.Requires)
	}
	if !hasFile(info.Files, "/usr/bin/notes", false) || !hasFile(info.Files, "/usr/share/notes/dir", true) {
		t.Errorf("files: %d", len(info.Files))
	}
	// Unsigned, it says so.
	p.Signer = nil
	if err := WriteRPM(p, func(d string) bool { return strings.HasPrefix(d, "/usr/share/notes") }, file); err != nil {
		t.Fatal(err)
	}
	if info, err := ReadRPM(file); err != nil || info.Signed {
		t.Errorf("unsigned: %+v %v", info, err)
	}
}

func hasFile(fs []RPMFile, path string, dir bool) bool {
	for _, f := range fs {
		if f.Path == path && f.Dir == dir {
			return true
		}
	}
	return false
}

func gunzip(t *testing.T, b []byte) []byte {
	t.Helper()
	zr, err := gzip.NewReader(bytes.NewReader(b))
	if err != nil {
		t.Fatal(err)
	}
	out, err := io.ReadAll(zr)
	if err != nil {
		t.Fatal(err)
	}
	return out
}

// A dnf repository's metadata: repomd.xml names primary, filelists and
// other by their digests; each lists the packages; a repository updated
// keeps what it had and replaces a version built again.
func TestYumMetadata(t *testing.T) {
	dir := t.TempDir()
	write := func(version string) *RPMInfo {
		p := testPackage()
		p.Version = version
		p.Depends = []string{"gtk4 >= 4.14", "libadwaita >= 1:1.5-2"}
		f := filepath.Join(dir, version+".rpm")
		if err := WriteRPM(p, func(d string) bool { return strings.HasPrefix(d, "/usr/share/notes") }, f); err != nil {
			t.Fatal(err)
		}
		info, err := ReadRPM(f)
		if err != nil {
			t.Fatal(err)
		}
		return info
	}
	now := time.Unix(1_800_000_000, 0)
	v1, v2 := write("1.0.0"), write("1.1.0")
	m := &YumMetadata{}
	m.Add(NewYumPackage(v1, "Packages/n/notes-1.0.0-1.x86_64.rpm", now))
	files, err := m.Files(now)
	if err != nil {
		t.Fatal(err)
	}
	// As the repository holds them, read back, then the next version.
	served := map[string][]byte{"repodata/repomd.xml": files.Repomd}
	for k, v := range files.Files {
		served[k] = v
	}
	read := func(href string) ([]byte, error) {
		if b, ok := served[href]; ok {
			return b, nil
		}
		return nil, os.ErrNotExist
	}
	m, err = ReadYumMetadata(files.Repomd, read)
	if err != nil {
		t.Fatal(err)
	}
	if len(m.Packages) != 1 || m.Packages[0].Version != "1.0.0" || m.Packages[0].PkgID != v1.SHA256 {
		t.Fatalf("read back: %+v", m.Packages)
	}
	m.Add(NewYumPackage(v2, "Packages/n/notes-1.1.0-1.x86_64.rpm", now))
	m.Add(NewYumPackage(v2, "Packages/n/notes-1.1.0-1.x86_64.rpm", now)) // built again
	files, err = m.Files(now)
	if err != nil {
		t.Fatal(err)
	}
	data, err := ParseRepomd(files.Repomd)
	if err != nil || len(data) != 3 {
		t.Fatalf("repomd.xml: %v %v\n%s", data, err, files.Repomd)
	}
	docs := map[string][]byte{}
	for _, d := range data {
		gz := files.Files[d.Location.Href]
		x := gunzip(t, gz)
		closed, open := sha256.Sum256(gz), sha256.Sum256(x)
		if d.Checksum != hex.EncodeToString(closed[:]) || d.OpenChecksum != hex.EncodeToString(open[:]) || d.Size != int64(len(gz)) || d.OpenSize != int64(len(x)) ||
			d.Location.Href != "repodata/"+d.Checksum+"-"+d.Type+".xml.gz" || d.Timestamp != now.Unix() {
			t.Errorf("%s: %+v", d.Type, d)
		}
		docs[d.Type] = x
	}
	// primary.xml, as dnf reads it: two packages, their versions, digests,
	// header ranges, dependencies (not rpmlib's) and executables.
	var primary struct {
		XMLName  xml.Name `xml:"http://linux.duke.edu/metadata/common metadata"`
		Packages string   `xml:"packages,attr"`
		Package  []struct {
			Type    string `xml:"type,attr"`
			Name    string `xml:"name"`
			Arch    string `xml:"arch"`
			Version struct {
				Epoch string `xml:"epoch,attr"`
				Ver   string `xml:"ver,attr"`
				Rel   string `xml:"rel,attr"`
			} `xml:"version"`
			Checksum struct {
				Type  string `xml:"type,attr"`
				PkgID string `xml:"pkgid,attr"`
				Value string `xml:",chardata"`
			} `xml:"checksum"`
			Size struct {
				Package int64 `xml:"package,attr"`
			} `xml:"size"`
			Location struct {
				Href string `xml:"href,attr"`
			} `xml:"location"`
			Format struct {
				License string `xml:"http://linux.duke.edu/metadata/rpm license"`
				Range   struct {
					Start int64 `xml:"start,attr"`
					End   int64 `xml:"end,attr"`
				} `xml:"http://linux.duke.edu/metadata/rpm header-range"`
				Requires []struct {
					Name  string `xml:"name,attr"`
					Flags string `xml:"flags,attr"`
					Epoch string `xml:"epoch,attr"`
					Ver   string `xml:"ver,attr"`
					Rel   string `xml:"rel,attr"`
				} `xml:"http://linux.duke.edu/metadata/rpm requires>entry"`
				Files []string `xml:"file"`
			} `xml:"format"`
		} `xml:"package"`
	}
	if err := xml.Unmarshal(docs["primary"], &primary); err != nil {
		t.Fatalf("primary.xml: %v\n%s", err, docs["primary"])
	}
	if primary.Packages != "2" || len(primary.Package) != 2 {
		t.Fatalf("primary.xml has %s: %d", primary.Packages, len(primary.Package))
	}
	for i, info := range []*RPMInfo{v1, v2} {
		p := primary.Package[i]
		if p.Type != "rpm" || p.Name != "notes" || p.Arch != "x86_64" || p.Version.Epoch != "0" || p.Version.Ver != info.Version || p.Version.Rel != "1" ||
			p.Checksum.Type != "sha256" || p.Checksum.PkgID != "YES" || p.Checksum.Value != info.SHA256 || p.Size.Package != info.Size ||
			p.Format.License != "MIT" || p.Format.Range.Start != info.HeaderStart || p.Format.Range.End != info.HeaderEnd {
			t.Errorf("package %d: %+v", i, p)
		}
		if len(p.Format.Requires) != 2 || p.Format.Requires[0].Name != "gtk4" || p.Format.Requires[0].Flags != "GE" || p.Format.Requires[0].Ver != "4.14" ||
			p.Format.Requires[1].Epoch != "1" || p.Format.Requires[1].Ver != "1.5" || p.Format.Requires[1].Rel != "2" {
			t.Errorf("package %d requires %+v", i, p.Format.Requires)
		}
		if len(p.Format.Files) != 1 || p.Format.Files[0] != "/usr/bin/notes" {
			t.Errorf("package %d's primary files: %v", i, p.Format.Files)
		}
	}
	if primary.Package[1].Location.Href != "Packages/n/notes-1.1.0-1.x86_64.rpm" {
		t.Errorf("the second's location: %s", primary.Package[1].Location.Href)
	}
	// filelists.xml: every file of each, by its package's ID.
	var filelists struct {
		XMLName xml.Name `xml:"http://linux.duke.edu/metadata/filelists filelists"`
		Package []struct {
			PkgID string `xml:"pkgid,attr"`
			Files []struct {
				Type string `xml:"type,attr"`
				Path string `xml:",chardata"`
			} `xml:"file"`
		} `xml:"package"`
	}
	if err := xml.Unmarshal(docs["filelists"], &filelists); err != nil {
		t.Fatal(err)
	}
	if len(filelists.Package) != 2 || filelists.Package[0].PkgID != v1.SHA256 || len(filelists.Package[0].Files) != len(v1.Files) {
		t.Errorf("filelists.xml: %d packages", len(filelists.Package))
	}
	var other struct {
		XMLName xml.Name `xml:"http://linux.duke.edu/metadata/other otherdata"`
		Package []struct {
			PkgID string `xml:"pkgid,attr"`
		} `xml:"package"`
	}
	if err := xml.Unmarshal(docs["other"], &other); err != nil || len(other.Package) != 2 || other.Package[1].PkgID != v2.SHA256 {
		t.Errorf("other.xml: %v %+v", err, other)
	}
}

// An apt repository's indexes: a package's stanza is its control file's
// fields, where it is in the pool and its digests; a Packages file
// updated keeps the others; Release lists each index's digests.
func TestAptIndexes(t *testing.T) {
	p := testPackage()
	p.Depends = []string{"libgtk-4-1 (>= 4.14)"}
	file := filepath.Join(t.TempDir(), "notes.deb")
	if err := WriteDeb(p, file); err != nil {
		t.Fatal(err)
	}
	s, err := DebStanza(file)
	if err != nil {
		t.Fatal(err)
	}
	b, _ := os.ReadFile(file)
	sum := sha256.Sum256(b)
	want := map[string]string{
		"Package": "notes", "Version": "1.3.0~beta.1", "Architecture": "amd64", "Depends": "libgtk-4-1 (>= 4.14)",
		"Filename": "pool/main/n/notes/notes_1.3.0~beta.1_amd64.deb", "SHA256": hex.EncodeToString(sum[:]),
		"Size": strconv.Itoa(len(b)),
	}
	for k, v := range want {
		if s.Get(k) != v {
			t.Errorf("%s: %q, want %q", k, s.Get(k), v)
		}
	}
	if !strings.Contains(s.String(), "\nDescription: Write things down\n Notes, kept on your computer.\n .\n A second paragraph.\n") {
		t.Errorf("the description:\n%s", s)
	}
	// Another version, and this one again: two stanzas, each once.
	older := &Stanza{}
	older.Set("Package", "notes")
	older.Set("Version", "1.2.0")
	older.Set("Architecture", "amd64")
	text := AddPackage(AddPackage(AddPackage("", older), s), s)
	ss := ParseStanzas(text)
	if len(ss) != 2 || ss[0].Get("Version") != "1.2.0" || ss[1].Get("Version") != "1.3.0~beta.1" || ss[1].Get("Description") != s.Get("Description") {
		t.Errorf("Packages:\n%s", text)
	}
	if DebPoolPath(&Stanza{Keys: []string{"Package"}, Values: map[string]string{"Package": "libfoo", "Version": "2:1.0", "Architecture": "arm64"}}) != "pool/main/libf/libfoo/libfoo_1.0_arm64.deb" {
		t.Error("a library's pool path")
	}
	rel := &AptRelease{Origin: "Notes", Label: "Notes", Suite: "stable", Codename: "stable", Date: time.Date(2026, 11, 27, 5, 0, 0, 0, time.UTC), Components: []string{"main"},
		Files: map[string][]byte{"main/binary-amd64/Packages": []byte(text), "main/binary-amd64/Packages.gz": Gzip([]byte(text)), "main/binary-arm64/Packages": nil}}
	r := ParseStanzas(string(rel.Text()))
	if len(r) != 1 || r[0].Get("Suite") != "stable" || r[0].Get("Architectures") != "amd64 arm64" || r[0].Get("Date") != "Fri, 27 Nov 2026 05:00:00 UTC" || r[0].Get("Components") != "main" {
		t.Fatalf("Release:\n%s", rel.Text())
	}
	packagesSum := sha256.Sum256([]byte(text))
	if !strings.Contains(r[0].Get("SHA256"), "\n "+hex.EncodeToString(packagesSum[:])+" "+fmt.Sprintf("%8d", len(text))+" main/binary-amd64/Packages") {
		t.Errorf("Release's SHA256:\n%s", r[0].Get("SHA256"))
	}
	if !bytes.Equal(gunzip(t, Gzip([]byte(text))), []byte(text)) || !bytes.Equal(Gzip([]byte(text)), Gzip([]byte(text))) {
		t.Error("Gzip isn't the same bytes each time")
	}
}

// A signed AppImage, as appimagetool --sign signs one: .sha256_sig holds
// an armored text signature of the file's SHA-256 (hex) with both
// sections zeroed, which the public key in .sig_key verifies; the rest
// of the runtime, its update information and the image are as unsigned;
// a byte changed anywhere outside the two sections fails the check.
func TestAppImageSigned(t *testing.T) {
	key := testKey(t)
	dir := t.TempDir()
	a := &AppImage{
		Runtime:    fakeRuntime(1024),
		Files:      append(testFiles(), File{Path: "/AppRun", Mode: 0o755, Data: []byte("#!/bin/sh\n")}),
		UpdateInfo: "zsync|https://example.com/notes-x86_64.AppImage.zsync",
		Time:       time.Unix(1_700_000_000, 0),
	}
	plain := filepath.Join(dir, "plain.AppImage")
	if err := WriteAppImage(a, plain); err != nil {
		t.Fatal(err)
	}
	unsigned, _ := os.ReadFile(plain)
	if _, sig, k, err := AppImageSignature(unsigned); err != nil || len(sig) != 0 || len(k) != 0 {
		t.Fatalf("unsigned: %q %q %v", sig, k, err)
	}
	a.Signer = key
	file := filepath.Join(dir, "Notes.AppImage")
	if err := WriteAppImage(a, file); err != nil {
		t.Fatal(err)
	}
	b, _ := os.ReadFile(file)
	if !bytes.Equal(a.Runtime, fakeRuntime(1024)) {
		t.Error("signing changed the runtime it was given")
	}
	digest, sig, pub, err := AppImageSignature(b)
	if err != nil {
		t.Fatal(err)
	}
	if want, _ := AppImageDigest(unsigned); digest != want || len(digest) != 64 || strings.ToLower(digest) != digest {
		t.Errorf("the digest %s, want the unsigned file's %s", digest, want)
	}
	sum := sha256.Sum256(unsigned) // the sections are zeros there
	if digest != hex.EncodeToString(sum[:]) {
		t.Error("the digest isn't the SHA-256 of the file with its sections zeroed")
	}
	if !bytes.HasPrefix(sig, []byte("-----BEGIN PGP SIGNATURE-----")) || !bytes.HasPrefix(pub, []byte("-----BEGIN PGP PUBLIC KEY BLOCK-----")) {
		t.Fatalf("the sections: %q, %q", sig, pub)
	}
	if fp, err := gpgsign.ParsePublic(pub); err != nil || fp != key.Fingerprint() {
		t.Errorf(".sig_key: %s %v", fp, err)
	}
	if err := gpgsign.Verify(pub, []byte(digest), sig); err != nil {
		t.Errorf("the signature: %v", err)
	}
	if typ := signatureType(t, sig); typ != 0x01 {
		t.Errorf("signature type %#x, want a text document's (0x01), as gpgme's text mode makes", typ)
	}
	// Only the two sections differ from the unsigned AppImage.
	if len(b) != len(unsigned) {
		t.Fatalf("%d bytes, unsigned %d", len(b), len(unsigned))
	}
	sigSec, _ := appImageSection(b, ".sha256_sig")
	keySec, _ := appImageSection(b, ".sig_key")
	for i := range b {
		in := func(s span) bool { return int64(i) >= s.off && int64(i) < s.off+s.size }
		if b[i] != unsigned[i] && !in(sigSec) && !in(keySec) {
			t.Fatalf("byte %d changed outside the signature's sections", i)
		}
	}
	// Tampered: a byte of the image, or of the update information.
	for _, at := range []int{len(b) - 10, fakeUpdOffset} {
		bad := bytes.Clone(b)
		bad[at] ^= 1
		d, s, k, _ := AppImageSignature(bad)
		if gpgsign.Verify(k, []byte(d), s) == nil {
			t.Errorf("a byte changed at %d: the signature still verifies", at)
		}
	}
	// A runtime without the sections can't be signed.
	a.Runtime = fakeRuntimeSections(1024, 0, 0)
	if err := WriteAppImage(a, file); err == nil || !strings.Contains(err.Error(), ".sha256_sig") {
		t.Errorf("no .sha256_sig: %v", err)
	}
	a.Runtime = fakeRuntimeSections(1024, 1024, 512)
	if err := WriteAppImage(a, file); err == nil || !strings.Contains(err.Error(), "room") {
		t.Errorf("a .sig_key too small: %v", err)
	}
}

// signatureType is an armored signature's type.
func signatureType(t *testing.T, armored []byte) packet.SignatureType {
	t.Helper()
	blk, err := armor.Decode(bytes.NewReader(armored))
	if err != nil {
		t.Fatal(err)
	}
	p, err := packet.Read(blk.Body)
	if err != nil {
		t.Fatal(err)
	}
	sig, ok := p.(*packet.Signature)
	if !ok {
		t.Fatalf("a %T", p)
	}
	return sig.SigType
}
