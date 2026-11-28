// Command ucdgen writes the core's Unicode tables (core/src/rt/
// unicode_gen.c: grapheme clusters, terminal widths, case mapping and
// folding) and the cases its C test checks them against (core/test/
// testdata/unicode), from a pinned release of the Unicode Character
// Database:
//
//	go generate ./internal/ucdgen          # downloads the release's files
//	go run ./internal/ucdgen -cache dir    # the files in dir, laid out as the UCD's (auxiliary/, emoji/)
//
// Each code point's properties are packed in a uint16 (unicode.h's
// AK_UP_*) and stored as ranges: the first code point of each run of
// equal properties, binary searched. The case mappings are runs of
// code points that map by the same delta, one apart or two (Latin's
// alternating capitals), and a table of the mappings to several code
// points (ß to SS, the full case folding's).
package main

//go:generate go run .

import (
	"bufio"
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"flag"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"slices"
	"sort"
	"strconv"
	"strings"
	"time"
)

// The release, and its files' SHA-256.
const version = "18.0.0"

var files = map[string]string{
	"UnicodeData.txt":                     "0736451de439ae7baf1425136617da495e09ee5afbe6e394374db7009ea08950",
	"EastAsianWidth.txt":                  "a0cf29eacd00cfcaec4381c6b7c281685f18dbb4e7ff82b4076ccb342ca839aa",
	"DerivedCoreProperties.txt":           "09c928886a178fcafd93c29e4bd59073a058e5a100b716d425cb563ab50f68c9",
	"PropList.txt":                        "f438f532e8737bb8a2702126cdf9c4af5e357c58c7acf9d9eb2fc7c1a1d955d6",
	"SpecialCasing.txt":                   "8538dea57c184f1ef3783885ea79677b10f6efa06423717157e63712f14d1ad2",
	"CaseFolding.txt":                     "a004797658a457bec4dc11683e39f69249ea3b595b752dbea6721c4c9f587b0d",
	"auxiliary/GraphemeBreakProperty.txt": "0839dcb79e4ac639ecd538b1abf7c9d22e3f9dd265b7e182d33627aa4d75b45a",
	"auxiliary/GraphemeBreakTest.txt":     "b0cf047ee94485bbdc846de2b902f5f8a815f6b674f9d04223cddadd91c9df31",
	"emoji/emoji-data.txt":                "80d00f8e616a0ef27fd6b8de3b758c06383b5d917e2977709578e68baf733bf1",
	"emoji/emoji-variation-sequences.txt": "ff1707564aa1f1b2fcf4ec92d609d4cb26940bc0d4dcf07f5328ad6879e84da3",
}

// The property bits: unicode.h's AK_UP_*, which must agree.
const (
	gcbMask      = 0xf // Grapheme_Cluster_Break, gcbs' index
	extPict      = 1 << 4
	incbShift    = 5       // Indic_Conjunct_Break: 0 None, 1 Linker, 2 Consonant, 3 Extend
	widthShift   = 7       // 0, 1 or 2 columns
	evs          = 1 << 9  // has emoji and text variation sequences
	cased        = 1 << 10 // Cased
	caseIgnor    = 1 << 11 // Case_Ignorable
	lowerChanges = 1 << 12 // lower, upper and fold don't map it to itself
	upperChanges = 1 << 13
	foldChanges  = 1 << 14
)

// gcNames are the general categories, in unicode.h's AK_GC_* order.
var gcNames = []string{"Cn", "Lu", "Ll", "Lt", "Lm", "Lo", "Mn", "Mc", "Me", "Nd", "Nl", "No", "Pc", "Pd", "Ps", "Pe", "Pi", "Pf", "Po", "Sm", "Sc", "Sk", "So", "Zs", "Zl", "Zp", "Cc", "Cf", "Cs", "Co"}

const whiteSpace = 0x80 // White_Space, beside the category

var gcbs = []string{"Other", "CR", "LF", "Control", "Extend", "ZWJ", "Regional_Indicator", "Prepend", "SpacingMark", "L", "V", "T", "LV", "LVT"}

const maxCP = 0x10ffff

func main() {
	log.SetFlags(0)
	cache := flag.String("cache", "", "a directory with the release's files (default: download them)")
	out := flag.String("out", "../../core/src/rt/unicode_gen.c", "the C tables")
	testdata := flag.String("testdata", "../../core/test/testdata/unicode", "the C test's cases")
	flag.Parse()
	if err := run(*cache, *out, *testdata); err != nil {
		log.Fatal("ucdgen: ", err)
	}
}

func run(cache, out, testdata string) error {
	data := map[string][]byte{}
	for name, sum := range files {
		b, err := fetch(cache, name, sum)
		if err != nil {
			return err
		}
		data[name] = b
	}

	gc := make([]string, maxCP+1)
	for i := range gc {
		gc[i] = "Cn"
	}
	simple := [3]map[rune]rune{{}, {}, {}} // lower, upper (UnicodeData's), fold (CaseFolding's C)
	if err := unicodeData(data["UnicodeData.txt"], gc, simple[0], simple[1]); err != nil {
		return err
	}

	props := make([]uint16, maxCP+1)
	set := func(name, file string, f func(lo, hi rune, fields []string)) error {
		return ranges(data[file], func(lo, hi rune, fields []string) {
			if name == "" || fields[0] == name {
				f(lo, hi, fields)
			}
		})
	}
	each := func(lo, hi rune, f func(r rune)) {
		for r := lo; r <= hi; r++ {
			f(r)
		}
	}
	gcbIndex := map[string]uint16{}
	for i, g := range gcbs {
		gcbIndex[g] = uint16(i)
	}
	if err := set("", "auxiliary/GraphemeBreakProperty.txt", func(lo, hi rune, f []string) {
		v, ok := gcbIndex[f[0]]
		if !ok {
			log.Fatalf("ucdgen: Grapheme_Cluster_Break %q", f[0])
		}
		each(lo, hi, func(r rune) { props[r] = props[r]&^gcbMask | v })
	}); err != nil {
		return err
	}
	if err := set("Extended_Pictographic", "emoji/emoji-data.txt", func(lo, hi rune, _ []string) {
		each(lo, hi, func(r rune) { props[r] |= extPict })
	}); err != nil {
		return err
	}
	incb := map[string]uint16{"Linker": 1, "Consonant": 2, "Extend": 3}
	derived := map[string][][2]rune{}
	if err := set("", "DerivedCoreProperties.txt", func(lo, hi rune, f []string) {
		if f[0] == "InCB" {
			v, ok := incb[f[1]]
			if !ok {
				log.Fatalf("ucdgen: InCB %q", f[1])
			}
			each(lo, hi, func(r rune) { props[r] |= v << incbShift })
			return
		}
		derived[f[0]] = append(derived[f[0]], [2]rune{lo, hi})
	}); err != nil {
		return err
	}
	for _, rg := range derived["Cased"] {
		each(rg[0], rg[1], func(r rune) { props[r] |= cased })
	}
	for _, rg := range derived["Case_Ignorable"] {
		each(rg[0], rg[1], func(r rune) { props[r] |= caseIgnor })
	}
	if err := set("", "emoji/emoji-variation-sequences.txt", func(lo, hi rune, f []string) {
		each(lo, hi, func(r rune) { props[r] |= evs })
	}); err != nil {
		return err
	}

	// East_Asian_Width: N unless listed, the CJK blocks' and planes 2
	// and 3's unassigned code points W (the file's header says so).
	eaw := make([]string, maxCP+1)
	for i := range eaw {
		eaw[i] = "N"
	}
	for _, rg := range [][2]rune{{0x3400, 0x4dbf}, {0x4e00, 0x9fff}, {0xf900, 0xfaff}, {0x20000, 0x2fffd}, {0x30000, 0x3fffd}} {
		each(rg[0], rg[1], func(r rune) { eaw[r] = "W" })
	}
	if err := set("", "EastAsianWidth.txt", func(lo, hi rune, f []string) {
		each(lo, hi, func(r rune) { eaw[r] = f[0] })
	}); err != nil {
		return err
	}
	pcm := map[rune]bool{}
	if err := set("Prepended_Concatenation_Mark", "PropList.txt", func(lo, hi rune, _ []string) {
		each(lo, hi, func(r rune) { pcm[r] = true })
	}); err != nil {
		return err
	}
	ignorable := map[rune]bool{}
	for _, rg := range derived["Default_Ignorable_Code_Point"] {
		each(rg[0], rg[1], func(r rune) { ignorable[r] = true })
	}
	for r := rune(0); r <= maxCP; r++ {
		props[r] |= uint16(width(r, gc[r], eaw[r], pcm[r], ignorable[r])) << widthShift
	}

	// Case mappings: lower and upper are the full ones without
	// conditions (SpecialCasing's, else UnicodeData's simple ones); fold
	// is CaseFolding's full folding (F, else C).
	full := [3]map[rune][]rune{{}, {}, {}}
	if err := special(data["SpecialCasing.txt"], full[0], full[1]); err != nil {
		return err
	}
	if err := folding(data["CaseFolding.txt"], simple[2], full[2]); err != nil {
		return err
	}
	maps := [3]map[rune][]rune{{}, {}, {}}
	for k := 0; k < 3; k++ {
		for r, to := range simple[k] {
			if to != r {
				maps[k][r] = []rune{to}
			}
		}
		for r, to := range full[k] {
			if len(to) == 1 && to[0] == r {
				delete(maps[k], r)
			} else {
				maps[k][r] = to
			}
		}
		bit := []uint16{lowerChanges, upperChanges, foldChanges}[k]
		for r := range maps[k] {
			props[r] |= bit
		}
	}

	// A code point's general category, and White_Space: a char's
	// isLetter, isDigit, isWhitespace and the rest.
	cats := make([]uint8, maxCP+1)
	for r := rune(0); r <= maxCP; r++ {
		cats[r] = uint8(slices.Index(gcNames, gc[r]))
	}
	if err := set("White_Space", "PropList.txt", func(lo, hi rune, _ []string) {
		each(lo, hi, func(r rune) { cats[r] |= whiteSpace })
	}); err != nil {
		return err
	}

	var c bytes.Buffer
	stats, err := writeC(&c, props, maps, cats)
	if err != nil {
		return err
	}
	if err := os.WriteFile(out, c.Bytes(), 0o644); err != nil {
		return err
	}
	if err := os.MkdirAll(testdata, 0o755); err != nil {
		return err
	}
	if err := os.WriteFile(filepath.Join(testdata, "GraphemeBreakTest.txt"), data["auxiliary/GraphemeBreakTest.txt"], 0o644); err != nil {
		return err
	}
	if err := os.WriteFile(filepath.Join(testdata, "case_cases.txt"), caseCases(maps), 0o644); err != nil {
		return err
	}
	log.Printf("ucdgen: Unicode %s: %s", version, stats)
	return nil
}

// width is the columns a terminal gives r alone, as wcwidth and Rust's
// unicode-width say: none for controls, combining and enclosing marks,
// format characters and what's ignorable (but the soft hyphen and the
// prepended concatenation marks, which show), and Hangul's medial vowels
// and final consonants (they join the syllable before them); two for
// East Asian Wide and Fullwidth; one for the rest, Ambiguous included.
func width(r rune, gc, eaw string, pcm, ignorable bool) int {
	switch {
	case r == 0x00ad || pcm:
		return 1
	case gc == "Cc" || gc == "Mn" || gc == "Me" || gc == "Cf" || gc == "Zl" || gc == "Zp":
		return 0
	case r >= 0x1160 && r <= 0x11ff || r >= 0xd7b0 && r <= 0xd7ff:
		return 0
	case ignorable && r != 0x115f:
		return 0
	case eaw == "W" || eaw == "F":
		return 2
	}
	return 1
}

func fetch(cache, name, sum string) ([]byte, error) {
	var b []byte
	var err error
	if cache != "" {
		b, err = os.ReadFile(filepath.Join(cache, filepath.FromSlash(name)))
	}
	if cache == "" || os.IsNotExist(err) {
		b, err = download("https://www.unicode.org/Public/" + version + "/ucd/" + name)
	}
	if err != nil {
		return nil, err
	}
	h := sha256.Sum256(b)
	if got := hex.EncodeToString(h[:]); got != sum {
		return nil, fmt.Errorf("%s has SHA-256 %s, want %s", name, got, sum)
	}
	return b, nil
}

func download(url string) ([]byte, error) {
	c := &http.Client{Timeout: 2 * time.Minute}
	resp, err := c.Get(url)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		return nil, fmt.Errorf("%s: %s", url, resp.Status)
	}
	return io.ReadAll(resp.Body)
}

// ranges calls f for each line of a UCD property file: its code points
// and its fields after them, without the comment.
func ranges(b []byte, f func(lo, hi rune, fields []string)) error {
	sc := bufio.NewScanner(bytes.NewReader(b))
	sc.Buffer(make([]byte, 1<<16), 1<<20)
	for sc.Scan() {
		line := sc.Text()
		if i := strings.IndexByte(line, '#'); i >= 0 {
			line = line[:i]
		}
		if strings.TrimSpace(line) == "" {
			continue
		}
		parts := strings.Split(line, ";")
		cps := strings.TrimSpace(parts[0])
		var fields []string
		for _, p := range parts[1:] {
			fields = append(fields, strings.TrimSpace(p))
		}
		lo, hi, err := cpRange(cps)
		if err != nil {
			return err
		}
		f(lo, hi, fields)
	}
	return sc.Err()
}

// cpRange reads "0041", "0041..005A", or (a sequence) its first code
// point.
func cpRange(s string) (rune, rune, error) {
	if a, b, ok := strings.Cut(s, ".."); ok {
		lo, err := hexRune(a)
		if err != nil {
			return 0, 0, err
		}
		hi, err := hexRune(b)
		return lo, hi, err
	}
	r, err := hexRune(strings.Fields(s)[0])
	return r, r, err
}

func hexRune(s string) (rune, error) {
	v, err := strconv.ParseUint(strings.TrimSpace(s), 16, 32)
	if err != nil || v > maxCP {
		return 0, fmt.Errorf("bad code point %q", s)
	}
	return rune(v), nil
}

func hexRunes(s string) ([]rune, error) {
	var out []rune
	for _, f := range strings.Fields(s) {
		r, err := hexRune(f)
		if err != nil {
			return nil, err
		}
		out = append(out, r)
	}
	return out, nil
}

func unicodeData(b []byte, gc []string, lower, upper map[rune]rune) error {
	sc := bufio.NewScanner(bytes.NewReader(b))
	var first rune = -1
	for sc.Scan() {
		f := strings.Split(sc.Text(), ";")
		if len(f) < 15 {
			continue
		}
		r, err := hexRune(f[0])
		if err != nil {
			return err
		}
		if strings.HasSuffix(f[1], ", First>") {
			first = r
			continue
		}
		lo := r
		if strings.HasSuffix(f[1], ", Last>") {
			lo = first
		}
		for c := lo; c <= r; c++ {
			gc[c] = f[2]
		}
		if f[12] != "" {
			if upper[r], err = hexRune(f[12]); err != nil {
				return err
			}
		}
		if f[13] != "" {
			if lower[r], err = hexRune(f[13]); err != nil {
				return err
			}
		}
	}
	return sc.Err()
}

// special reads SpecialCasing's mappings without a condition (Final_Sigma
// is the C's to do; the languages' aren't done).
func special(b []byte, lower, upper map[rune][]rune) error {
	return ranges(b, func(lo, _ rune, f []string) {
		if len(f) < 4 || f[3] != "" {
			return
		}
		l, err1 := hexRunes(f[0])
		u, err2 := hexRunes(f[2])
		if err1 != nil || err2 != nil {
			log.Fatalf("ucdgen: SpecialCasing %04X", lo)
		}
		lower[lo] = l
		upper[lo] = u
	})
}

// folding reads CaseFolding's common (C) and full (F) foldings; the
// simple (S) and Turkic (T) ones aren't used.
func folding(b []byte, simple map[rune]rune, full map[rune][]rune) error {
	return ranges(b, func(lo, _ rune, f []string) {
		to, err := hexRunes(f[1])
		if err != nil {
			log.Fatalf("ucdgen: CaseFolding %04X", lo)
		}
		switch f[0] {
		case "C":
			simple[lo] = to[0]
		case "F":
			full[lo] = to
		}
	})
}

type caseRun struct {
	start  rune
	count  int
	stride rune
	delta  rune
}

func writeC(w *bytes.Buffer, props []uint16, maps [3]map[rune][]rune, cats []uint8) (string, error) {
	fmt.Fprintf(w, "// Code generated by internal/ucdgen from Unicode %s's character database; DO NOT EDIT.\n", version)
	fmt.Fprintf(w, "// The tables unicode.h's lookups read (unicode.c).\n\n#include \"rt/unicode.h\"\n\n")
	fmt.Fprintf(w, "const char ak_unicode_version[] = %q;\n\n", version)

	// The property ranges.
	var starts []rune
	var vals []uint16
	for r := rune(0); r <= maxCP; r++ {
		if r == 0 || props[r] != props[r-1] {
			starts = append(starts, r)
			vals = append(vals, props[r])
		}
	}
	fmt.Fprintf(w, "const uint32_t ak_uprop_count = %d;\n\n", len(starts))
	fmt.Fprintf(w, "const uint32_t ak_uprop_starts[%d] = {", len(starts))
	for i, s := range starts {
		if i%10 == 0 {
			w.WriteString("\n\t")
		} else {
			w.WriteString(" ")
		}
		fmt.Fprintf(w, "0x%x,", s)
	}
	fmt.Fprintf(w, "\n};\n\nconst uint16_t ak_uprop_values[%d] = {", len(vals))
	for i, v := range vals {
		if i%12 == 0 {
			w.WriteString("\n\t")
		} else {
			w.WriteString(" ")
		}
		fmt.Fprintf(w, "0x%x,", v)
	}
	w.WriteString("\n};\n")
	size := len(starts)*4 + len(vals)*2

	// The categories' ranges.
	var cstarts []rune
	var cvals []uint8
	for r := rune(0); r <= maxCP; r++ {
		if r == 0 || cats[r] != cats[r-1] {
			cstarts = append(cstarts, r)
			cvals = append(cvals, cats[r])
		}
	}
	fmt.Fprintf(w, "\nconst uint32_t ak_ucat_count = %d;\n\n", len(cstarts))
	fmt.Fprintf(w, "const uint32_t ak_ucat_starts[%d] = {", len(cstarts))
	for i, s := range cstarts {
		if i%10 == 0 {
			w.WriteString("\n\t")
		} else {
			w.WriteString(" ")
		}
		fmt.Fprintf(w, "0x%x,", s)
	}
	fmt.Fprintf(w, "\n};\n\nconst uint8_t ak_ucat_values[%d] = {", len(cvals))
	for i, v := range cvals {
		if i%16 == 0 {
			w.WriteString("\n\t")
		} else {
			w.WriteString(" ")
		}
		fmt.Fprintf(w, "0x%x,", v)
	}
	w.WriteString("\n};\n")
	size += len(cstarts)*4 + len(cvals)

	names := []string{"lower", "upper", "fold"}
	var counts []string
	for k, m := range maps {
		// The single code point mappings, as runs; the rest listed.
		var singles []rune
		var multis []rune
		for r, to := range m {
			if len(to) == 1 {
				singles = append(singles, r)
			} else {
				multis = append(multis, r)
			}
		}
		sort.Slice(singles, func(i, j int) bool { return singles[i] < singles[j] })
		sort.Slice(multis, func(i, j int) bool { return multis[i] < multis[j] })
		var runs []caseRun
		for _, r := range singles {
			d := m[r][0] - r
			if n := len(runs); n > 0 && runs[n-1].delta == d {
				last := &runs[n-1]
				if last.count == 1 && (r-last.start == 1 || r-last.start == 2) {
					last.stride = r - last.start
					last.count = 2
					continue
				}
				if last.count > 1 && r == last.start+rune(last.count)*last.stride {
					last.count++
					continue
				}
			}
			runs = append(runs, caseRun{start: r, count: 1, stride: 1, delta: d})
		}
		fmt.Fprintf(w, "\nconst uint32_t ak_case_%s_runs_count = %d;\n", names[k], len(runs))
		fmt.Fprintf(w, "const ak_case_run ak_case_%s_runs[%d] = {\n", names[k], len(runs))
		for _, rn := range runs {
			if rn.count > 0xffff || rn.stride > 255 {
				return "", fmt.Errorf("a run too long at %04X", rn.start)
			}
			fmt.Fprintf(w, "\t{0x%x, %d, %d, %d},\n", rn.start, rn.count, rn.stride, rn.delta)
		}
		w.WriteString("};\n")
		fmt.Fprintf(w, "const uint32_t ak_case_%s_full_count = %d;\n", names[k], len(multis))
		fmt.Fprintf(w, "const ak_case_full ak_case_%s_full[%d] = {\n", names[k], max(len(multis), 1))
		for _, r := range multis {
			to := m[r]
			if len(to) > 3 {
				return "", fmt.Errorf("%04X maps to %d code points", r, len(to))
			}
			fmt.Fprintf(w, "\t{0x%x, {", r)
			for i := 0; i < 3; i++ {
				if i > 0 {
					w.WriteString(", ")
				}
				if i < len(to) {
					fmt.Fprintf(w, "0x%x", to[i])
				} else {
					w.WriteString("0")
				}
			}
			w.WriteString("}},\n")
		}
		if len(multis) == 0 {
			w.WriteString("\t{0, {0, 0, 0}},\n")
		}
		w.WriteString("};\n")
		size += len(runs)*12 + len(multis)*16
		counts = append(counts, fmt.Sprintf("%s %d runs and %d full", names[k], len(runs), len(multis)))
	}
	return fmt.Sprintf("%d property ranges; %s; %d bytes", len(starts), strings.Join(counts, ", "), size), nil
}

// caseCases lists every code point that a mapping changes, and what
// lower, upper and fold give it: "cp;lower;upper;fold", in hex, the
// code points of each separated by spaces.
func caseCases(maps [3]map[rune][]rune) []byte {
	all := map[rune]bool{}
	for _, m := range maps {
		for r := range m {
			all[r] = true
		}
	}
	var rs []rune
	for r := range all {
		rs = append(rs, r)
	}
	sort.Slice(rs, func(i, j int) bool { return rs[i] < rs[j] })
	var b bytes.Buffer
	fmt.Fprintf(&b, "# Generated by internal/ucdgen from Unicode %s: each code point a case mapping changes,\n# then its lower, upper and fold (full, without conditions or a language's rules).\n", version)
	for _, r := range rs {
		fmt.Fprintf(&b, "%X", r)
		for _, m := range maps {
			to, ok := m[r]
			if !ok {
				to = []rune{r}
			}
			b.WriteString(";")
			for i, c := range to {
				if i > 0 {
					b.WriteString(" ")
				}
				fmt.Fprintf(&b, "%X", c)
			}
		}
		b.WriteString("\n")
	}
	return b.Bytes()
}
