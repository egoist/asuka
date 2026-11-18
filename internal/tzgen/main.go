// Command tzgen writes the core's copy of the IANA time zone database
// (core/src/std/tzdata_gen.c, std/time's named zones) and the cases its
// C test checks it against (core/test/testdata/tz_cases.txt), from a
// pinned tzdb release:
//
//	go generate ./internal/tzgen               # downloads the release
//	go run ./internal/tzgen -cache dir         # tzdata<v>.tar.gz and tzcode<v>.tar.gz in dir
//
// It builds the release's zic with the C compiler cbuild finds, compiles
// the zones with it (-b slim: the transitions until a zone's rules last
// changed, then the POSIX TZ string that says the rest), and packs every
// distinct zone into tables (tz.h): a transition is its minute (an
// int32) and its type (a byte), its seconds when it isn't on a minute in
// a side table. The packing is checked against Go's reading of the same
// TZif files (time.LoadLocationFromTZData) at every transition, and the
// cases sample each zone (transitions, the TZ string's years, far past
// and future) as Go reads it, for the C to agree with.
package main

//go:generate go run .

import (
	"archive/tar"
	"bytes"
	"compress/gzip"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"flag"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
	"time"

	"github.com/egoist/asuka/internal/cbuild"
)

// The tzdb release, and its tarballs' SHA-256.
const (
	version    = "2026f"
	dataSHA256 = "01b4a91cec5167ed48c11e8ab78f2048cead78ce94b0cafcad0ae8c5a3044d3d"
	codeSHA256 = "5a18da01393679ef2559214dc687563f7f3d05691f6f58b7fbc0166f3e796cd6"
)

// The source files zic compiles: the zones, and the links of backward
// (older names: Asia/Calcutta, US/Eastern).
var sources = []string{"africa", "antarctica", "asia", "australasia", "europe", "northamerica", "southamerica", "etcetera", "backward"}

func main() {
	log.SetFlags(0)
	cache := flag.String("cache", "", "a directory with tzdata"+version+".tar.gz and tzcode"+version+".tar.gz (default: download them)")
	out := flag.String("out", "../../core/src/std/tzdata_gen.c", "the C tables")
	cases := flag.String("cases", "../../core/test/testdata/tz_cases.txt", "the C test's cases")
	flag.Parse()
	if err := run(*cache, *out, *cases); err != nil {
		log.Fatal("tzgen: ", err)
	}
}

func run(cache, out, casesPath string) error {
	tmp, err := os.MkdirTemp("", "tzgen")
	if err != nil {
		return err
	}
	defer os.RemoveAll(tmp)
	data, code := filepath.Join(tmp, "data"), filepath.Join(tmp, "code")
	if err := fetch(cache, "tzdata", dataSHA256, data); err != nil {
		return err
	}
	if err := fetch(cache, "tzcode", codeSHA256, code); err != nil {
		return err
	}
	zic, err := buildZic(code)
	if err != nil {
		return err
	}
	zoneinfo := filepath.Join(tmp, "zoneinfo")
	args := append([]string{"-b", "slim", "-d", zoneinfo}, sources...)
	cmd := exec.Command(zic, args...)
	cmd.Dir = data
	if b, err := cmd.CombinedOutput(); err != nil {
		return fmt.Errorf("zic: %v\n%s", err, b)
	}
	listed, err := readZoneTab(filepath.Join(data, "zone.tab"))
	if err != nil {
		return err
	}
	db, err := read(zoneinfo, listed)
	if err != nil {
		return err
	}
	t := pack(db)
	if err := t.verify(db); err != nil {
		return err
	}
	if err := os.WriteFile(out, append(t.c(), t.cNames(db)...), 0o644); err != nil {
		return err
	}
	return os.WriteFile(casesPath, cases(db), 0o644)
}

// ---- The release ----

// fetch extracts <name><version>.tar.gz, from cache or downloaded, into
// dir, after checking its SHA-256.
func fetch(cache, name, sum, dir string) error {
	file := name + version + ".tar.gz"
	var b []byte
	var err error
	if cache != "" {
		b, err = os.ReadFile(filepath.Join(cache, file))
	}
	if cache == "" || os.IsNotExist(err) {
		b, err = download("https://data.iana.org/time-zones/releases/" + file)
	}
	if err != nil {
		return err
	}
	h := sha256.Sum256(b)
	if got := hex.EncodeToString(h[:]); got != sum {
		return fmt.Errorf("%s has SHA-256 %s, want %s", file, got, sum)
	}
	zr, err := gzip.NewReader(bytes.NewReader(b))
	if err != nil {
		return err
	}
	tr := tar.NewReader(zr)
	for {
		h, err := tr.Next()
		if err == io.EOF {
			return nil
		}
		if err != nil {
			return err
		}
		if h.Typeflag != tar.TypeReg || strings.Contains(h.Name, "..") || strings.HasPrefix(h.Name, "/") {
			continue
		}
		p := filepath.Join(dir, filepath.FromSlash(h.Name))
		if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
			return err
		}
		body, err := io.ReadAll(tr)
		if err != nil {
			return err
		}
		if err := os.WriteFile(p, body, 0o644); err != nil {
			return err
		}
	}
}

func download(url string) ([]byte, error) {
	resp, err := http.Get(url)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("%s: %s", url, resp.Status)
	}
	return io.ReadAll(resp.Body)
}

// buildZic compiles the release's zic, writing the two headers its
// Makefile would.
func buildZic(code string) (string, error) {
	headers := map[string]string{
		"version.h": fmt.Sprintf("static char const PKGVERSION[]=\"(tzcode) \";\nstatic char const TZVERSION[]=%q;\nstatic char const REPORT_BUGS_TO[]=\"tz@iana.org\";\n", version),
		"tzdir.h":   "#define TZDEFAULT \"/etc/localtime\"\n#define TZDIR \"/usr/share/zoneinfo\"\n",
	}
	for name, body := range headers {
		if err := os.WriteFile(filepath.Join(code, name), []byte(body), 0o644); err != nil {
			return "", err
		}
	}
	cc, err := cbuild.Find()
	if err != nil {
		return "", err
	}
	objs, err := cc.CompileWith([]string{filepath.Join(code, "zic.c")}, filepath.Join(code, "obj"), []string{"-O1", "-w"})
	if err != nil {
		return "", err
	}
	exe := filepath.Join(code, "zic")
	return exe, cc.Link(objs, exe, nil)
}

// readZoneTab is zone.tab's zones: one per place, what a picker lists.
func readZoneTab(path string) (map[string]bool, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	m := map[string]bool{"UTC": true}
	for _, line := range strings.Split(string(b), "\n") {
		if line == "" || line[0] == '#' {
			continue
		}
		f := strings.Split(line, "\t")
		if len(f) < 3 {
			return nil, fmt.Errorf("zone.tab: %q", line)
		}
		m[f[2]] = true
	}
	return m, nil
}

// ---- The zones, as zic wrote them ----

type ztype struct {
	off  int32
	abbr string
}

type zone struct {
	names  []string // its names, sorted: zic's links are the same file
	tzif   []byte
	trans  []int64
	idx    []uint8 // each transition's type
	types  []ztype // type 0 is before the first transition
	footer string
}

type name struct {
	name   string
	zone   int
	listed bool
}

type database struct {
	zones []*zone
	names []name // sorted as the C looks them up: by their lowercase
}

func read(dir string, listed map[string]bool) (*database, error) {
	byData := map[string]*zone{}
	db := &database{}
	err := filepath.WalkDir(dir, func(p string, d os.DirEntry, err error) error {
		if err != nil || d.IsDir() {
			return err
		}
		rel, _ := filepath.Rel(dir, p)
		rel = filepath.ToSlash(rel)
		b, err := os.ReadFile(p)
		if err != nil {
			return err
		}
		z := byData[string(b)]
		if z == nil {
			if z, err = parseTZif(b); err != nil {
				return fmt.Errorf("%s: %v", rel, err)
			}
			byData[string(b)] = z
			db.zones = append(db.zones, z)
		}
		z.names = append(z.names, rel)
		return nil
	})
	if err != nil {
		return nil, err
	}
	for _, z := range db.zones {
		sort.Strings(z.names)
	}
	sort.Slice(db.zones, func(i, j int) bool { return db.zones[i].names[0] < db.zones[j].names[0] })
	for i, z := range db.zones {
		for _, n := range z.names {
			db.names = append(db.names, name{n, i, listed[n]})
		}
	}
	sort.Slice(db.names, func(i, j int) bool { return lowerLess(db.names[i].name, db.names[j].name) })
	for i := 1; i < len(db.names); i++ {
		if strings.EqualFold(db.names[i-1].name, db.names[i].name) {
			return nil, fmt.Errorf("%s and %s differ only in case", db.names[i-1].name, db.names[i].name)
		}
	}
	for n := range listed {
		if !hasName(db, n) {
			return nil, fmt.Errorf("zone.tab's %s isn't a zone", n)
		}
	}
	return db, nil
}

func hasName(db *database, n string) bool {
	for _, x := range db.names {
		if x.name == n {
			return true
		}
	}
	return false
}

// lowerLess orders by ASCII lowercase, then bytes: the C's strcasecmp
// order, so a name is found whatever its case.
func lowerLess(a, b string) bool {
	la, lb := strings.ToLower(a), strings.ToLower(b)
	if la != lb {
		return la < lb
	}
	return a < b
}

// parseTZif reads a TZif file's version 2+ data (RFC 8536): its
// transitions, its types and its footer.
func parseTZif(b []byte) (*zone, error) {
	counts := func(b []byte) ([6]int, error) {
		var c [6]int
		if len(b) < 44 || string(b[:4]) != "TZif" {
			return c, errors.New("not TZif")
		}
		for i := range c {
			c[i] = int(binary.BigEndian.Uint32(b[20+4*i:]))
		}
		return c, nil
	}
	// isutcnt, isstdcnt, leapcnt, timecnt, typecnt, charcnt
	file := b
	c, err := counts(b)
	if err != nil {
		return nil, err
	}
	if b[4] < '2' {
		return nil, errors.New("TZif version 1")
	}
	b = b[44+c[3]*5+c[4]*6+c[5]+c[2]*8+c[1]+c[0]:]
	if c, err = counts(b); err != nil {
		return nil, err
	}
	if c[2] != 0 {
		return nil, errors.New("leap seconds")
	}
	r := b[44:]
	z := &zone{tzif: file}
	for i := 0; i < c[3]; i++ {
		z.trans = append(z.trans, int64(binary.BigEndian.Uint64(r[8*i:])))
	}
	r = r[8*c[3]:]
	z.idx = append(z.idx, r[:c[3]]...)
	r = r[c[3]:]
	chars := r[6*c[4]:][:c[5]]
	for i := 0; i < c[4]; i++ {
		e := r[6*i:]
		a := chars[e[5]:]
		a = a[:bytes.IndexByte(a, 0)]
		z.types = append(z.types, ztype{int32(binary.BigEndian.Uint32(e)), string(a)})
	}
	r = r[6*c[4]+c[5]+c[1]+c[0]:]
	if len(r) < 2 || r[0] != '\n' || r[len(r)-1] != '\n' {
		return nil, errors.New("no footer")
	}
	z.footer = string(r[1 : len(r)-1])
	return z, nil
}

// ---- The tables ----

type tables struct {
	strs    strpool
	types   []ztype // distinct
	typeIdx map[ztype]int
	ztypes  []int // a zone's types: indexes into types
	mins    []int32
	tidx    []uint8
	secIdx  []int // transitions off a minute, by index, and their seconds
	secs    []int
	zones   []czone
}

type czone struct{ trans, ntrans, types, ntypes, footer int }

type strpool struct {
	b   strings.Builder
	off map[string]int
}

func (p *strpool) add(s string) int {
	if p.off == nil {
		p.off = map[string]int{}
	}
	if o, ok := p.off[s]; ok {
		return o
	}
	o := p.b.Len()
	p.b.WriteString(s)
	p.b.WriteByte(0)
	p.off[s] = o
	return o
}

func pack(db *database) *tables {
	t := &tables{typeIdx: map[ztype]int{}}
	for _, n := range db.names {
		t.strs.add(n.name)
	}
	for _, z := range db.zones {
		cz := czone{trans: len(t.mins), ntrans: len(z.trans), types: len(t.ztypes), ntypes: len(z.types), footer: t.strs.add(z.footer)}
		for _, ty := range z.types {
			i, ok := t.typeIdx[ty]
			if !ok {
				i = len(t.types)
				t.typeIdx[ty] = i
				t.types = append(t.types, ty)
				t.strs.add(ty.abbr)
			}
			t.ztypes = append(t.ztypes, i)
		}
		for i, tr := range z.trans {
			m, s := floorDiv(tr, 60), floorMod(tr, 60)
			t.mins = append(t.mins, int32(m))
			if s != 0 {
				t.secIdx = append(t.secIdx, len(t.tidx))
				t.secs = append(t.secs, int(s))
			}
			t.tidx = append(t.tidx, z.idx[i])
		}
		t.zones = append(t.zones, cz)
	}
	return t
}

func floorDiv(a, b int64) int64 {
	q := a / b
	if a%b != 0 && (a < 0) != (b < 0) {
		q--
	}
	return q
}

func floorMod(a, b int64) int64 { return a - floorDiv(a, b)*b }

// at is the transition i's instant, as the C reads it back.
func (t *tables) at(i int) int64 {
	s := int64(0)
	if k := sort.SearchInts(t.secIdx, i); k < len(t.secIdx) && t.secIdx[k] == i {
		s = int64(t.secs[k])
	}
	return int64(t.mins[i])*60 + s
}

// verify reads every zone's transitions back from the tables and checks
// them, and the offsets and abbreviations on each side of them, against
// Go's reading of the TZif file; and that the tables fit their types.
func (t *tables) verify(db *database) error {
	if t.strs.b.Len() > 0xffff || len(t.mins) > 0xffff || len(t.types) > 0xffff || len(t.zones) > 0xffff || len(t.ztypes) > 0xffff {
		return fmt.Errorf("the tables outgrew their uint16 indexes")
	}
	for zi, z := range db.zones {
		if len(z.types) > 0xff {
			return fmt.Errorf("%s: %d types", z.names[0], len(z.types))
		}
		loc, err := time.LoadLocationFromTZData(z.names[0], z.tzif)
		if err != nil {
			return err
		}
		cz := t.zones[zi]
		for i := 0; i < cz.ntrans; i++ {
			tr := t.at(cz.trans + i)
			if tr != z.trans[i] {
				return fmt.Errorf("%s: transition %d is %d, packed as %d", z.names[0], i, z.trans[i], tr)
			}
			ty := t.types[t.ztypes[cz.types+int(t.tidx[cz.trans+i])]]
			abbr, off := time.Unix(tr, 0).In(loc).Zone()
			if int32(off) != ty.off || abbr != ty.abbr {
				return fmt.Errorf("%s at %d: %s %d, packed %s %d", z.names[0], tr, abbr, off, ty.abbr, ty.off)
			}
		}
	}
	return nil
}

func (t *tables) c() []byte {
	var b bytes.Buffer
	fmt.Fprintf(&b, "// Code generated by internal/tzgen from tzdata %s; DO NOT EDIT.\n\n", version)
	b.WriteString("#include \"std/tz.h\"\n\n")
	fmt.Fprintf(&b, "const char ak_tz_version[] = %q;\n\n", version)
	b.WriteString("const char ak_tz_strs[] =")
	s := t.strs.b.String()
	for len(s) > 0 {
		n := min(len(s), 96)
		fmt.Fprintf(&b, "\n\t\"%s\"", cEscape(s[:n]))
		s = s[n:]
	}
	b.WriteString(";\n\n")
	fmt.Fprintf(&b, "const ak_tz_type ak_tz_types[%d] = {", len(t.types))
	for i, ty := range t.types {
		if i%6 == 0 {
			b.WriteString("\n\t")
		} else {
			b.WriteByte(' ')
		}
		fmt.Fprintf(&b, "{%d, %d},", ty.off, t.strs.off[ty.abbr])
	}
	b.WriteString("\n};\n\n")
	ints(&b, "const uint16_t ak_tz_ztypes", t.ztypes, 16)
	m := make([]int, len(t.mins))
	for i, v := range t.mins {
		m[i] = int(v)
	}
	ints(&b, "const int32_t ak_tz_mins", m, 10)
	ti := make([]int, len(t.tidx))
	for i, v := range t.tidx {
		ti[i] = int(v)
	}
	ints(&b, "const uint8_t ak_tz_tidx", ti, 32)
	ints(&b, "const uint16_t ak_tz_sec_idx", t.secIdx, 16)
	ints(&b, "const uint8_t ak_tz_secs", t.secs, 24)
	fmt.Fprintf(&b, "const uint16_t ak_tz_nsecs = %d;\n\n", len(t.secs))
	fmt.Fprintf(&b, "const ak_tz_zone ak_tz_zones[%d] = {", len(t.zones))
	for i, z := range t.zones {
		if i%4 == 0 {
			b.WriteString("\n\t")
		} else {
			b.WriteByte(' ')
		}
		fmt.Fprintf(&b, "{%d, %d, %d, %d},", z.trans, z.ntrans, z.types, z.footer)
	}
	b.WriteString("\n};\n\n")
	return b.Bytes()
}

func (t *tables) cNames(db *database) []byte {
	var b bytes.Buffer
	fmt.Fprintf(&b, "const ak_tz_name ak_tz_names[%d] = {", len(db.names))
	for i, n := range db.names {
		if i%4 == 0 {
			b.WriteString("\n\t")
		} else {
			b.WriteByte(' ')
		}
		l := 0
		if n.listed {
			l = 1
		}
		fmt.Fprintf(&b, "{%d, %d, %d},", t.strs.off[n.name], n.zone, l)
	}
	b.WriteString("\n};\n")
	fmt.Fprintf(&b, "const uint16_t ak_tz_nnames = %d;\n", len(db.names))
	return b.Bytes()
}

func ints(b *bytes.Buffer, decl string, v []int, per int) {
	fmt.Fprintf(b, "%s[%d] = {", decl, max(len(v), 1))
	if len(v) == 0 {
		b.WriteString("0")
	}
	for i, x := range v {
		if i%per == 0 {
			b.WriteString("\n\t")
		} else {
			b.WriteByte(' ')
		}
		fmt.Fprintf(b, "%d,", x)
	}
	b.WriteString("\n};\n\n")
}

// cEscape writes s as a C string's body, the pool's NULs as \000 (three
// digits: an octal escape takes no more, so a digit after it is itself).
func cEscape(s string) string {
	var b strings.Builder
	for i := 0; i < len(s); i++ {
		switch c := s[i]; {
		case c == 0:
			b.WriteString(`\000`)
		case c == '"' || c == '\\':
			b.WriteByte('\\')
			b.WriteByte(c)
		case c < ' ' || c > '~':
			fmt.Fprintf(&b, `\%03o`, c)
		default:
			b.WriteByte(c)
		}
	}
	return b.String()
}

// ---- The C test's cases ----

// cases samples every zone as Go reads its TZif file: a few transitions
// (the second before and the transition), the transitions of 2050 by
// its TZ string, and 1800 and 2200: "name unix offset abbreviation".
func cases(db *database) []byte {
	var b bytes.Buffer
	fmt.Fprintf(&b, "# tzdata %s, as Go's time package reads zic's files: a zone, a Unix time, its offset and abbreviation\n", version)
	for _, z := range db.zones {
		loc, err := time.LoadLocationFromTZData(z.names[0], z.tzif)
		if err != nil {
			panic(err)
		}
		var at []int64
		n := len(z.trans)
		for _, i := range []int{0, n / 3, 2 * n / 3, n - 1} {
			if i >= 0 && i < n {
				at = append(at, z.trans[i]-1, z.trans[i])
			}
		}
		for _, d := range []time.Time{time.Date(2050, 1, 15, 0, 0, 0, 0, time.UTC), time.Date(2050, 7, 15, 0, 0, 0, 0, time.UTC)} {
			start, end := d.In(loc).ZoneBounds()
			for _, e := range []time.Time{start, end} {
				if !e.IsZero() && e.Year() == 2050 {
					at = append(at, e.Unix()-1, e.Unix())
				}
			}
		}
		at = append(at, time.Date(1800, 1, 1, 0, 0, 0, 0, time.UTC).Unix(), time.Date(2200, 7, 1, 0, 0, 0, 0, time.UTC).Unix())
		sort.Slice(at, func(i, j int) bool { return at[i] < at[j] })
		last := int64(-1 << 62)
		for _, u := range at {
			if u == last {
				continue
			}
			last = u
			abbr, off := time.Unix(u, 0).In(loc).Zone()
			fmt.Fprintf(&b, "%s %d %d %s\n", z.names[0], u, off, abbr)
		}
	}
	return b.Bytes()
}
