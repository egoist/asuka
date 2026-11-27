package distpkg

import (
	"bytes"
	"compress/gzip"
	"crypto/sha256"
	"encoding/hex"
	"encoding/xml"
	"fmt"
	"io"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"time"
)

// A dnf (and yum, zypper) repository's metadata, as createrepo_c writes
// it: repodata/repomd.xml, which names primary.xml.gz (what each package
// is, needs and provides, and its executables), filelists.xml.gz (every
// file) and other.xml.gz (changelogs: none here), each by its digest;
// repomd.xml.asc, its OpenPGP signature (repo_gpgcheck). A repository
// updated keeps the packages it had, as their entries were written.

// The metadata's namespaces.
const (
	nsCommon    = "http://linux.duke.edu/metadata/common"
	nsRPM       = "http://linux.duke.edu/metadata/rpm"
	nsFilelists = "http://linux.duke.edu/metadata/filelists"
	nsOther     = "http://linux.duke.edu/metadata/other"
	nsRepo      = "http://linux.duke.edu/metadata/repo"
)

// A YumPackage is a package's entries in the three files.
type YumPackage struct {
	// PkgID is the package file's SHA-256; Href, its path in the
	// repository.
	PkgID, Href string
	// NEVRA identifies it: name, epoch, version, release, architecture.
	Name, Epoch, Version, Release, Arch string
	// Primary, Filelists and Other are its <package> elements.
	Primary, Filelists, Other string
}

// YumMetadata is a repository's packages.
type YumMetadata struct {
	Packages []*YumPackage
}

func attr(name, v string) string { return " " + name + "=\"" + xmlEscape(v) + "\"" }

// rpmFlag is a dependency's sense as the metadata says it.
func rpmFlag(f int64) string {
	switch f & (senseLess | senseGreater | senseEqual) {
	case senseEqual:
		return "EQ"
	case senseLess:
		return "LT"
	case senseLess | senseEqual:
		return "LE"
	case senseGreater:
		return "GT"
	case senseGreater | senseEqual:
		return "GE"
	}
	return ""
}

// The senses of a dependency needed before the package's scripts run
// (rpmsense's PREREQ, SCRIPT_PRE, SCRIPT_POST...): pre="1".
const sensePre = 1<<6 | 1<<9 | 1<<10 | 1<<11 | 1<<12

// depEntry is a dependency's rpm:entry.
func depEntry(d RPMDep) string {
	s := "<rpm:entry" + attr("name", d.Name)
	if f := rpmFlag(d.Flags); f != "" {
		s += attr("flags", f)
		epoch, ver, rel := "0", d.Version, ""
		if e, rest, ok := strings.Cut(ver, ":"); ok && strings.Trim(e, "0123456789") == "" {
			epoch, ver = e, rest
		}
		if i := strings.LastIndexByte(ver, '-'); i >= 0 {
			ver, rel = ver[:i], ver[i+1:]
		}
		s += attr("epoch", epoch) + attr("ver", ver)
		if rel != "" {
			s += attr("rel", rel)
		}
	}
	if d.Flags&sensePre != 0 {
		s += attr("pre", "1")
	}
	return s + "/>"
}

// primaryFile is createrepo_c's choice of the files primary.xml lists:
// executables, configuration and sendmail, which dependencies name.
var primaryFile = regexp.MustCompile(`^(.*bin/.*|/etc/.*|/usr/lib/sendmail)$`)

// NewYumPackage is the entries of the RPM info read (ParseRPM), at href in
// the repository, made at mtime.
func NewYumPackage(info *RPMInfo, href string, mtime time.Time) *YumPackage {
	epoch := info.Epoch
	if epoch == "" {
		epoch = "0"
	}
	version := "<version" + attr("epoch", epoch) + attr("ver", info.Version) + attr("rel", info.Release) + "/>"
	var p strings.Builder
	p.WriteString("<package type=\"rpm\">\n")
	fmt.Fprintf(&p, "  <name>%s</name>\n  <arch>%s</arch>\n  %s\n", xmlEscape(info.Name), xmlEscape(info.Arch), version)
	fmt.Fprintf(&p, "  <checksum type=\"sha256\" pkgid=\"YES\">%s</checksum>\n", info.SHA256)
	fmt.Fprintf(&p, "  <summary>%s</summary>\n  <description>%s</description>\n", xmlEscape(info.Summary), xmlEscape(info.Description))
	fmt.Fprintf(&p, "  <packager>%s</packager>\n  <url>%s</url>\n", xmlEscape(info.Packager), xmlEscape(info.URL))
	fmt.Fprintf(&p, "  <time file=\"%d\" build=\"%d\"/>\n", mtime.Unix(), info.BuildTime)
	fmt.Fprintf(&p, "  <size package=\"%d\" installed=\"%d\" archive=\"%d\"/>\n", info.Size, info.InstalledSize, info.ArchiveSize)
	fmt.Fprintf(&p, "  <location%s/>\n", attr("href", href))
	p.WriteString("  <format>\n")
	for _, f := range [][2]string{{"license", info.License}, {"vendor", info.Vendor}, {"group", info.Group}, {"buildhost", info.BuildHost}, {"sourcerpm", info.SourceRPM}} {
		fmt.Fprintf(&p, "    <rpm:%s>%s</rpm:%s>\n", f[0], xmlEscape(f[1]), f[0])
	}
	fmt.Fprintf(&p, "    <rpm:header-range start=\"%d\" end=\"%d\"/>\n", info.HeaderStart, info.HeaderEnd)
	for _, list := range []struct {
		name string
		deps []RPMDep
	}{{"provides", info.Provides}, {"requires", info.Requires}, {"conflicts", info.Conflicts}, {"obsoletes", info.Obsoletes}, {"suggests", info.Suggests}, {"recommends", info.Recommends}} {
		var entries []string
		for _, d := range list.deps {
			// rpmlib()'s are rpm's own, which createrepo leaves out.
			if strings.HasPrefix(d.Name, "rpmlib(") || d.Flags&senseRPMLib != 0 {
				continue
			}
			entries = append(entries, "      "+depEntry(d)+"\n")
		}
		if len(entries) > 0 {
			fmt.Fprintf(&p, "    <rpm:%s>\n%s    </rpm:%s>\n", list.name, strings.Join(entries, ""), list.name)
		}
	}
	var files strings.Builder
	for _, f := range info.Files {
		typ := ""
		if f.Dir {
			typ = " type=\"dir\""
		}
		line := fmt.Sprintf("<file%s>%s</file>\n", typ, xmlEscape(f.Path))
		files.WriteString("  " + line)
		if primaryFile.MatchString(f.Path) {
			p.WriteString("    " + line)
		}
	}
	p.WriteString("  </format>\n</package>\n")
	id := attr("pkgid", info.SHA256) + attr("name", info.Name) + attr("arch", info.Arch)
	return &YumPackage{
		PkgID: info.SHA256, Href: href,
		Name: info.Name, Epoch: epoch, Version: info.Version, Release: info.Release, Arch: info.Arch,
		Primary:   p.String(),
		Filelists: "<package" + id + ">\n  " + version + "\n" + files.String() + "</package>\n",
		Other:     "<package" + id + ">\n  " + version + "\n</package>\n",
	}
}

// Add puts p in the repository, in place of the package at its href and
// of the same name, version and architecture (one built again).
func (m *YumMetadata) Add(p *YumPackage) {
	kept := m.Packages[:0]
	for _, q := range m.Packages {
		if q.Href == p.Href || q.Name == p.Name && q.Arch == p.Arch && q.Epoch == p.Epoch && q.Version == p.Version && q.Release == p.Release {
			continue
		}
		kept = append(kept, q)
	}
	m.Packages = append(kept, p)
}

// RepomdData is one file repomd.xml names.
type RepomdData struct {
	Type         string `xml:"type,attr"`
	Checksum     string `xml:"checksum"`
	OpenChecksum string `xml:"open-checksum"`
	Location     struct {
		Href string `xml:"href,attr"`
	} `xml:"location"`
	Timestamp int64 `xml:"timestamp"`
	Size      int64 `xml:"size"`
	OpenSize  int64 `xml:"open-size"`
}

// ParseRepomd is the files repomd.xml names.
func ParseRepomd(b []byte) ([]RepomdData, error) {
	var r struct {
		Data []RepomdData `xml:"data"`
	}
	if err := xml.Unmarshal(b, &r); err != nil {
		return nil, fmt.Errorf("repomd.xml: %w", err)
	}
	return r.Data, nil
}

// ReadYumMetadata reads a repository's metadata: repomd.xml, and the files
// it names, which read reads by their href.
func ReadYumMetadata(repomd []byte, read func(href string) ([]byte, error)) (*YumMetadata, error) {
	data, err := ParseRepomd(repomd)
	if err != nil {
		return nil, err
	}
	elems := map[string]map[string]string{} // type → pkgid → element
	var primary []string
	for _, d := range data {
		if d.Type != "primary" && d.Type != "filelists" && d.Type != "other" {
			continue
		}
		b, err := read(d.Location.Href)
		if err != nil {
			return nil, err
		}
		if strings.HasSuffix(d.Location.Href, ".gz") {
			zr, err := gzip.NewReader(bytes.NewReader(b))
			if err != nil {
				return nil, fmt.Errorf("%s: %w", d.Location.Href, err)
			}
			if b, err = io.ReadAll(zr); err != nil {
				return nil, fmt.Errorf("%s: %w", d.Location.Href, err)
			}
		} else if !strings.HasSuffix(d.Location.Href, ".xml") {
			return nil, fmt.Errorf("%s: only gzipped metadata is read", d.Location.Href)
		}
		pkgs, err := packageElements(b)
		if err != nil {
			return nil, fmt.Errorf("%s: %w", d.Location.Href, err)
		}
		elems[d.Type] = map[string]string{}
		for _, e := range pkgs {
			var head struct {
				PkgID    string `xml:"pkgid,attr"`
				Checksum string `xml:"checksum"`
			}
			if err := xml.Unmarshal([]byte(e), &head); err != nil {
				return nil, fmt.Errorf("%s: %w", d.Location.Href, err)
			}
			id := head.PkgID
			if d.Type == "primary" {
				id = strings.TrimSpace(head.Checksum)
				primary = append(primary, e)
			}
			elems[d.Type][id] = e
		}
	}
	m := &YumMetadata{}
	for _, e := range primary {
		var p struct {
			Name    string `xml:"name"`
			Arch    string `xml:"arch"`
			Version struct {
				Epoch string `xml:"epoch,attr"`
				Ver   string `xml:"ver,attr"`
				Rel   string `xml:"rel,attr"`
			} `xml:"version"`
			Checksum string `xml:"checksum"`
			Location struct {
				Href string `xml:"href,attr"`
			} `xml:"location"`
		}
		// The rpm: prefix is the document's; alone, an element is read
		// without it.
		if err := xml.Unmarshal([]byte(e), &p); err != nil {
			return nil, fmt.Errorf("primary.xml: %w", err)
		}
		id := strings.TrimSpace(p.Checksum)
		m.Packages = append(m.Packages, &YumPackage{
			PkgID: id, Href: p.Location.Href,
			Name: p.Name, Epoch: p.Version.Epoch, Version: p.Version.Ver, Release: p.Version.Rel, Arch: p.Arch,
			Primary: e + "\n", Filelists: elems["filelists"][id] + "\n", Other: elems["other"][id] + "\n",
		})
	}
	return m, nil
}

// packageElements are the <package> elements of a metadata file, as they
// are written.
func packageElements(b []byte) ([]string, error) {
	d := xml.NewDecoder(bytes.NewReader(b))
	d.Strict = true
	var out []string
	depth := 0
	for {
		start := d.InputOffset()
		tok, err := d.RawToken()
		if err == io.EOF {
			return out, nil
		}
		if err != nil {
			return nil, err
		}
		switch t := tok.(type) {
		case xml.StartElement:
			depth++
			if depth == 2 && t.Name.Local == "package" {
				if err := skipRaw(d); err != nil {
					return nil, err
				}
				out = append(out, string(b[start:d.InputOffset()]))
				depth--
			}
		case xml.EndElement:
			depth--
		}
	}
}

// skipRaw reads to the end of the element just started.
func skipRaw(d *xml.Decoder) error {
	for depth := 1; depth > 0; {
		tok, err := d.RawToken()
		if err != nil {
			return err
		}
		switch tok.(type) {
		case xml.StartElement:
			depth++
		case xml.EndElement:
			depth--
		}
	}
	return nil
}

// YumFiles are a repository's metadata files: repodata/repomd.xml and the
// files it names (repodata/<digest>-primary.xml.gz...), by path.
type YumFiles struct {
	Repomd []byte
	Files  map[string][]byte
}

// Files writes the repository's metadata, dated now.
func (m *YumMetadata) Files(now time.Time) (*YumFiles, error) {
	pkgs := append([]*YumPackage(nil), m.Packages...)
	sort.SliceStable(pkgs, func(i, j int) bool {
		if pkgs[i].Name != pkgs[j].Name {
			return pkgs[i].Name < pkgs[j].Name
		}
		return pkgs[i].Href < pkgs[j].Href
	})
	n := strconv.Itoa(len(pkgs))
	docs := []struct {
		typ, head, tail string
		elem            func(*YumPackage) string
	}{
		{"primary", `<metadata xmlns="` + nsCommon + `" xmlns:rpm="` + nsRPM + `" packages="` + n + `">`, "</metadata>", func(p *YumPackage) string { return p.Primary }},
		{"filelists", `<filelists xmlns="` + nsFilelists + `" packages="` + n + `">`, "</filelists>", func(p *YumPackage) string { return p.Filelists }},
		{"other", `<otherdata xmlns="` + nsOther + `" packages="` + n + `">`, "</otherdata>", func(p *YumPackage) string { return p.Other }},
	}
	out := &YumFiles{Files: map[string][]byte{}}
	var repomd strings.Builder
	repomd.WriteString("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<repomd xmlns=\"" + nsRepo + "\" xmlns:rpm=\"" + nsRPM + "\">\n")
	fmt.Fprintf(&repomd, "  <revision>%d</revision>\n", now.Unix())
	for _, doc := range docs {
		var x bytes.Buffer
		x.WriteString("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n" + doc.head + "\n")
		for _, p := range pkgs {
			e := doc.elem(p)
			if strings.TrimSpace(e) == "" {
				return nil, fmt.Errorf("distpkg: %s has no %s entry", p.Href, doc.typ)
			}
			x.WriteString(e)
		}
		x.WriteString(doc.tail + "\n")
		var gz bytes.Buffer
		zw, _ := gzip.NewWriterLevel(&gz, gzip.BestCompression)
		zw.Write(x.Bytes())
		if err := zw.Close(); err != nil {
			return nil, err
		}
		open, closed := sha256.Sum256(x.Bytes()), sha256.Sum256(gz.Bytes())
		href := "repodata/" + hex.EncodeToString(closed[:]) + "-" + doc.typ + ".xml.gz"
		out.Files[href] = gz.Bytes()
		fmt.Fprintf(&repomd, "  <data type=\"%s\">\n", doc.typ)
		fmt.Fprintf(&repomd, "    <checksum type=\"sha256\">%x</checksum>\n", closed)
		fmt.Fprintf(&repomd, "    <open-checksum type=\"sha256\">%x</open-checksum>\n", open)
		fmt.Fprintf(&repomd, "    <location href=\"%s\"/>\n", href)
		fmt.Fprintf(&repomd, "    <timestamp>%d</timestamp>\n    <size>%d</size>\n    <open-size>%d</open-size>\n  </data>\n", now.Unix(), gz.Len(), x.Len())
	}
	repomd.WriteString("</repomd>\n")
	out.Repomd = []byte(repomd.String())
	return out, nil
}
