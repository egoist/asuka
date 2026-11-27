package distpkg

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"hash/crc32"
	"sort"
	"strconv"
	"strings"
	"unicode"
	"unicode/utf16"
	"unicode/utf8"

	"golang.org/x/text/language"
)

// A PRI is a package resource index, the resources.pri of an MSIX: what
// Windows' resource manager (MRT) looks up the strings and files of a
// package in, by name and by the context's qualifiers, choosing among a
// resource's candidates the one that fits the user's languages, the
// display's scale, an icon's target size, the taskbar's form. The
// manifest's ms-resource:AppName is the map's Resources/AppName, and its
// Assets\Square44x44Logo.png is Files/Assets/Square44x44Logo.png, whose
// candidates are the files Assets\Square44x44Logo.scale-200.png and the
// others. WritePRI writes it byte for byte as makepri.exe (the Windows
// SDK's) does for the same resources, with its default configuration
// and no resource packs: version 2 (mrm_pri2), one resource map named
// after the package, deployment-mergeable, its values as data items
// grouped by qualifier set (as a resource pack would split them).
//
// The format isn't documented; this follows makepri's own output (the
// files of testdata/makepri-*.pri) and chausner's PriFormat
// (github.com/chausner/PriTools), which reads it.
type PRI struct {
	// Name is the resource map's: the package's identity name, which
	// ms-resource: URIs name (ms-resource://<Name>/Resources/AppName).
	Name string
	// DefaultLanguage is the language whose candidates are the default
	// (makepri's default qualifier), for a user who reads none of the
	// others: the manifest's first.
	DefaultLanguage string
	// Resources are the named resources, indexed in this order (makepri's
	// is a folder's files by name, then its strings).
	Resources []PRIResource
}

// A PRIResource is a named resource and its candidates.
type PRIResource struct {
	// Name is its path in the map, slash-separated: Resources/AppName,
	// Files/Assets/StoreLogo.png.
	Name string
	// Candidates are its values, each for some qualifiers; their
	// qualifiers are indexed in this order.
	Candidates []PRICandidate
}

// A PRICandidate is one of a resource's values.
type PRICandidate struct {
	Qualifiers []PRIQualifier
	// Value is the string, or the file's path in the package
	// (Assets\StoreLogo.scale-200.png) when Path is set.
	Value string
	Path  bool
}

// A PRIQualifier is a condition on the context: Language "de", Scale
// "200", TargetSize "24", AlternateForm "UNPLATED", Contrast "HIGH",
// Theme "LIGHT".
type PRIQualifier struct {
	Type  PRIQualifierType
	Value string
}

// PRIQualifierType is a qualifier's attribute, by MRT's numbering.
type PRIQualifierType uint16

const (
	PRILanguage      PRIQualifierType = 0
	PRIContrast      PRIQualifierType = 1
	PRIScale         PRIQualifierType = 2
	PRITargetSize    PRIQualifierType = 4
	PRITheme         PRIQualifierType = 6
	PRIAlternateForm PRIQualifierType = 7
)

// priPriority is each qualifier's priority (makepri's): a language
// decides before a contrast, which decides before a theme, a target size,
// a scale and then an alternate form.
var priPriority = map[PRIQualifierType]uint16{
	PRILanguage: 700, PRIContrast: 400, PRITheme: 350, PRITargetSize: 300, PRIScale: 200, PRIAlternateForm: 100,
}

// priScaleScore is how well a scale fits the default context (100%), in
// thousandths, as makepri scores the scales Windows has assets at.
var priScaleScore = map[int]uint16{100: 1000, 125: 937, 150: 875, 200: 750, 300: 468, 400: 437}

// value normalizes a qualifier's value as makepri writes it: a contrast,
// a theme and an alternate form upper case, a scale and a target size as
// their numbers.
func (q PRIQualifier) value() (string, error) {
	v := strings.TrimSpace(q.Value)
	switch q.Type {
	case PRILanguage:
		if _, err := language.Parse(v); err != nil || v == "" {
			return "", fmt.Errorf("distpkg: a PRI's language %q: %v", q.Value, err)
		}
		return v, nil
	case PRIScale, PRITargetSize:
		n, err := strconv.Atoi(v)
		if err != nil || n <= 0 {
			return "", fmt.Errorf("distpkg: a PRI's %s %q isn't a number", q.Type, q.Value)
		}
		if _, ok := priScaleScore[n]; q.Type == PRIScale && !ok {
			return "", fmt.Errorf("distpkg: a PRI's scale %d isn't one of Windows' (100, 125, 150, 200, 300, 400)", n)
		}
		return strconv.Itoa(n), nil
	case PRIContrast, PRITheme, PRIAlternateForm:
		if v == "" {
			return "", fmt.Errorf("distpkg: a PRI's %s is empty", q.Type)
		}
		return strings.ToUpper(v), nil
	}
	return "", fmt.Errorf("distpkg: a PRI's qualifier type %d", q.Type)
}

func (t PRIQualifierType) String() string {
	switch t {
	case PRILanguage:
		return "language"
	case PRIContrast:
		return "contrast"
	case PRIScale:
		return "scale"
	case PRITargetSize:
		return "target size"
	case PRITheme:
		return "theme"
	case PRIAlternateForm:
		return "alternate form"
	}
	return "qualifier " + strconv.Itoa(int(t))
}

// fallbackScore is how well a qualifier fits the default context, in
// thousandths: what MRT goes by when the context matches no candidate
// (a user who reads none of the app's languages gets the default one).
// The default context is makepri's: the default language, 100% scale,
// target size 256, standard contrast, the dark theme, no alternate form.
func (p *PRI) fallbackScore(t PRIQualifierType, v string) uint16 {
	switch t {
	case PRILanguage:
		return priLanguageScore(p.DefaultLanguage, v)
	case PRIScale:
		n, _ := strconv.Atoi(v)
		return priScaleScore[n]
	case PRITargetSize:
		if v == "256" {
			return 1000
		}
		return 500
	case PRIContrast:
		if v == "STANDARD" {
			return 1000
		}
	case PRITheme:
		if v == "DARK" {
			return 1000
		}
	}
	return 0
}

// priLanguageScore is how well a language fits the default one, as
// Windows matches language tags: 1 for the same language (iw is he), 0.99
// for a region within the other's (es-MX in es-419), 0.98 for one without
// a region or the world's (en and en-001, for en-GB) and for another of
// English's, 0.97 for another region (pt-PT, for pt-BR); nothing for
// another language or script (zh-Hans and zh-Hant). makepri scores so.
func priLanguageScore(def, v string) uint16 {
	a, err1 := language.Parse(def)
	b, err2 := language.Parse(v)
	if err1 != nil || err2 != nil {
		return 0
	}
	if strings.EqualFold(a.String(), b.String()) {
		return 1000
	}
	ab, _ := a.Base()
	bb, _ := b.Base()
	as, _ := a.Script()
	bs, _ := b.Script()
	if ab != bb || as != bs {
		return 0
	}
	ar, ac := a.Region()
	br, bc := b.Region()
	world := language.MustParseRegion("001")
	switch {
	case ac == language.Exact && bc == language.Exact && ar == br:
		return 1000
	case ac != language.Exact || bc != language.Exact || ar == world || br == world:
		return 980
	case ab == language.MustParseBase("en"):
		// English's regions are closer (makepri: en-US and en-GB as
		// en-GB and en-AU).
		return 980
	case ar.Contains(br) || br.Contains(ar):
		return 990
	}
	return 970
}

// WritePRI returns the PRI's bytes.
func WritePRI(p *PRI) ([]byte, error) {
	if p.Name == "" {
		return nil, errors.New("distpkg: a PRI needs its map's name")
	}
	w := priWriter{p: p}
	if err := w.index(); err != nil {
		return nil, err
	}
	sections := [][]byte{
		w.decisionInfo(),
		nil, // the descriptor, which numbers the others
		w.schema(),
		w.resourceMap(),
	}
	for _, d := range w.data {
		sections = append(sections, d.bytes())
	}
	sections[1] = w.descriptor(len(sections))
	for i, s := range sections {
		if s == nil {
			return nil, fmt.Errorf("distpkg: a PRI's section %d overflows", i)
		}
	}
	ids := []string{"[mrm_decn_info]\x00", "[mrm_pridescex]\x00", "[mrm_hschemaex] ", "[mrm_res_map2_]\x00"}
	for len(ids) < len(sections) {
		ids = append(ids, "[mrm_dataitem] \x00")
	}
	le := binary.LittleEndian
	start := 32 + 32*len(sections)
	var body bytes.Buffer
	toc := make([]byte, 0, 32*len(sections))
	for i, s := range sections {
		length := 32 + align8len(len(s)) + 8
		off := body.Len()
		toc = append(toc, ids[i]...)
		toc = le.AppendUint16(toc, 0) // flags
		toc = le.AppendUint16(toc, 0) // section flags
		toc = le.AppendUint32(toc, 0) // qualifier
		toc = le.AppendUint32(toc, uint32(off))
		toc = le.AppendUint32(toc, uint32(length))
		h := []byte(ids[i])
		h = le.AppendUint32(h, 0)
		h = le.AppendUint16(h, 0)
		h = le.AppendUint16(h, 0)
		h = le.AppendUint32(h, uint32(length))
		h = le.AppendUint32(h, 0)
		body.Write(h)
		body.Write(s)
		body.Write(make([]byte, align8len(len(s))-len(s)))
		body.Write(le.AppendUint32(le.AppendUint32(nil, 0xDEF5FADE), uint32(length)))
	}
	total := start + body.Len() + 16
	out := make([]byte, 0, total)
	out = append(out, "mrm_pri2"...)
	out = le.AppendUint16(out, 0)
	out = le.AppendUint16(out, 1)
	out = le.AppendUint32(out, uint32(total))
	out = le.AppendUint32(out, 32) // the table of contents
	out = le.AppendUint32(out, uint32(start))
	out = le.AppendUint16(out, uint16(len(sections)))
	out = le.AppendUint16(out, 0xFFFF)
	out = le.AppendUint32(out, 0)
	out = append(out, toc...)
	out = append(out, body.Bytes()...)
	out = le.AppendUint32(out, 0xDEFFFADE)
	out = le.AppendUint32(out, uint32(total))
	out = append(out, "mrm_pri2"...)
	return out, nil
}

func align8len(n int) int { return (n + 7) &^ 7 }

// priWriter indexes a PRI's resources into makepri's tables: its
// qualifiers, qualifier sets and decisions (a resource's sets, in MRT's
// order), the schema's scopes and items, and the data items of each set.
type priWriter struct {
	p          *PRI
	qualifiers []priQualifier // 0 is makepri's empty one
	qualIndex  map[priQualifier]int
	sets       [][]int // qualifier indices, by priority; 0 is the empty set
	setIndex   map[string]int
	decisions  [][]int // set indices; 0 is empty, 1 the empty set's
	decIndex   map[string]int
	items      []priItem
	root       *priScope
	scopes     []*priScope // by index
	data       []*priData  // the data item sections, in their order
}

type priQualifier struct {
	typ   PRIQualifierType
	value string
}

type priItem struct {
	name       string
	decision   int
	candidates []priCandidate // in the decision's order
}

type priCandidate struct {
	set       int
	valueType int
	data      []byte
	section   int // the data items' section, by the sections' index
	index     int
}

type priScope struct {
	name     string
	index    int
	children map[string]*priScope
	items    map[string]int // item indices
	order    []string       // children's and items' names, upper-cased
}

// The candidates' value types (MRT's), each value written in the one of
// them that's shortest.
const (
	priString      = 0
	priPath        = 1
	priASCIIString = 3
	priUTF8String  = 4
	priASCIIPath   = 5
	priUTF8Path    = 6
)

func (w *priWriter) index() error {
	w.qualifiers = []priQualifier{{}}
	w.qualIndex = map[priQualifier]int{}
	w.sets = [][]int{nil}
	w.setIndex = map[string]int{"": 0}
	w.decisions = [][]int{nil, {0}}
	w.decIndex = map[string]int{"0": 1}
	w.root = &priScope{children: map[string]*priScope{}, items: map[string]int{}}
	w.scopes = []*priScope{w.root}
	seen := map[string]bool{}
	for _, r := range w.p.Resources {
		parts := strings.Split(strings.Trim(strings.ReplaceAll(r.Name, `\`, "/"), "/"), "/")
		for _, part := range parts {
			if part == "" || len([]rune(part)) > 255 {
				return fmt.Errorf("distpkg: a PRI's resource name %q", r.Name)
			}
		}
		key := strings.ToUpper(strings.Join(parts, "/"))
		if seen[key] {
			return fmt.Errorf("distpkg: the PRI's resource %s twice (its names ignore case)", r.Name)
		}
		seen[key] = true
		if len(r.Candidates) == 0 {
			return fmt.Errorf("distpkg: the PRI's resource %s has no candidates", r.Name)
		}
		// Its candidates' qualifier sets, each by priority.
		type cand struct {
			set int
			c   PRICandidate
		}
		var cands []cand
		bySet := map[int]bool{}
		for _, c := range r.Candidates {
			var qs []priQualifier
			types := map[PRIQualifierType]bool{}
			for _, q := range c.Qualifiers {
				v, err := q.value()
				if err != nil {
					return err
				}
				if types[q.Type] {
					return fmt.Errorf("distpkg: a candidate of the PRI's %s has its %s twice", r.Name, q.Type)
				}
				types[q.Type] = true
				qs = append(qs, priQualifier{q.Type, v})
			}
			sort.SliceStable(qs, func(i, j int) bool { return priPriority[qs[i].typ] > priPriority[qs[j].typ] })
			var set []int
			for _, q := range qs {
				i, ok := w.qualIndex[q]
				if !ok {
					i = len(w.qualifiers)
					w.qualifiers = append(w.qualifiers, q)
					w.qualIndex[q] = i
				}
				set = append(set, i)
			}
			sk := intsKey(set)
			si, ok := w.setIndex[sk]
			if !ok {
				si = len(w.sets)
				w.sets = append(w.sets, set)
				w.setIndex[sk] = si
			}
			if bySet[si] {
				return fmt.Errorf("distpkg: the PRI's %s has two candidates for the same qualifiers", r.Name)
			}
			bySet[si] = true
			cands = append(cands, cand{si, c})
		}
		// MRT's order: the sets compared qualifier by qualifier, a higher
		// priority's first, then a greater value (as text: "32" before
		// "256"), a set before those it extends, descending.
		sort.SliceStable(cands, func(i, j int) bool { return w.setLess(cands[j].set, cands[i].set) })
		var dec []int
		for _, c := range cands {
			dec = append(dec, c.set)
		}
		dk := intsKey(dec)
		di, ok := w.decIndex[dk]
		if !ok {
			di = len(w.decisions)
			w.decisions = append(w.decisions, dec)
			w.decIndex[dk] = di
		}
		item := priItem{name: strings.Join(parts, "/"), decision: di}
		for _, c := range cands {
			vt, data := priValue(c.c.Value, c.c.Path)
			item.candidates = append(item.candidates, priCandidate{set: c.set, valueType: vt, data: data})
		}
		idx := len(w.items)
		w.items = append(w.items, item)
		// Its scopes, made as they're first named.
		s := w.root
		for _, part := range parts[:len(parts)-1] {
			up := strings.ToUpper(part)
			c, ok := s.children[up]
			if !ok {
				if _, isItem := s.items[up]; isItem {
					return fmt.Errorf("distpkg: the PRI's %s is a resource and a scope", part)
				}
				c = &priScope{name: part, index: len(w.scopes), children: map[string]*priScope{}, items: map[string]int{}}
				w.scopes = append(w.scopes, c)
				s.children[up] = c
				s.order = append(s.order, up)
			}
			s = c
		}
		leaf := strings.ToUpper(parts[len(parts)-1])
		if _, isScope := s.children[leaf]; isScope {
			return fmt.Errorf("distpkg: the PRI's %s is a resource and a scope", r.Name)
		}
		s.items[leaf] = idx
		s.order = append(s.order, leaf)
	}
	if len(w.items) > 0xFFFF || len(w.scopes)+len(w.items) > 0xFFFF || len(w.qualifiers) > 0xFFFF || len(w.sets) > 0xFFFF || len(w.decisions) > 0xFFFF {
		return errors.New("distpkg: a PRI with more than 65535 resources or qualifiers")
	}
	// The data items, as makepri stores them: the qualifier sets in their
	// order (the empty one last), each one's values in the resources'
	// order, a value in the section of the first set that has it (other
	// sets', other resources' equal values the same item), a set's
	// section made with its first value.
	order := make([]int, 0, len(w.sets))
	for i := 1; i < len(w.sets); i++ {
		order = append(order, i)
	}
	order = append(order, 0)
	type at struct{ section, index int }
	stored := map[string]at{}
	for _, set := range order {
		var d *priData
		for i := range w.items {
			for j := range w.items[i].candidates {
				c := &w.items[i].candidates[j]
				if c.set != set {
					continue
				}
				if a, ok := stored[string(c.data)]; ok {
					c.section, c.index = a.section, a.index
					continue
				}
				if d == nil {
					d = &priData{section: 4 + len(w.data)}
					w.data = append(w.data, d)
				}
				d.items = append(d.items, c.data)
				c.section, c.index = d.section, len(d.items)-1
				stored[string(c.data)] = at{c.section, c.index}
			}
		}
	}
	return nil
}

// setLess orders two qualifier sets, as MRT orders a decision's.
func (w *priWriter) setLess(a, b int) bool {
	x, y := w.sets[a], w.sets[b]
	for i := 0; i < len(x) && i < len(y); i++ {
		qa, qb := w.qualifiers[x[i]], w.qualifiers[y[i]]
		if pa, pb := priPriority[qa.typ], priPriority[qb.typ]; pa != pb {
			return pa < pb
		}
		if va, vb := strings.ToUpper(qa.value), strings.ToUpper(qb.value); va != vb {
			return va < vb
		}
	}
	return len(x) < len(y)
}

func intsKey(l []int) string {
	var b strings.Builder
	for i, v := range l {
		if i > 0 {
			b.WriteByte(',')
		}
		b.WriteString(strconv.Itoa(v))
	}
	return b.String()
}

// priValue is a value's type and bytes, NUL-terminated: ASCII when it is,
// else UTF-8 or UTF-16, whichever is shorter (UTF-8 on a tie).
func priValue(s string, path bool) (int, []byte) {
	ascii := true
	for i := 0; i < len(s); i++ {
		ascii = ascii && s[i] < 0x80
	}
	u16 := utf16.Encode([]rune(s))
	switch {
	case ascii:
		if path {
			return priASCIIPath, append([]byte(s), 0)
		}
		return priASCIIString, append([]byte(s), 0)
	case utf8.ValidString(s) && len(s)+1 <= 2*(len(u16)+1):
		if path {
			return priUTF8Path, append([]byte(s), 0)
		}
		return priUTF8String, append([]byte(s), 0)
	}
	b := utf16Bytes(u16)
	if path {
		return priPath, b
	}
	return priString, b
}

func utf16Bytes(u []uint16) []byte {
	b := make([]byte, 0, 2*len(u)+2)
	for _, c := range u {
		b = binary.LittleEndian.AppendUint16(b, c)
	}
	return binary.LittleEndian.AppendUint16(b, 0)
}

// priData is a data item section's: its values, each 4-byte aligned.
type priData struct {
	section int
	items   [][]byte
}

func (d *priData) bytes() []byte {
	le := binary.LittleEndian
	var data []byte
	var entries []byte
	for _, it := range d.items {
		for len(data)%4 != 0 {
			data = append(data, 0)
		}
		if len(data) > 0xFFFF || len(it) > 0xFFFF {
			return nil
		}
		entries = le.AppendUint16(entries, uint16(len(data)))
		entries = le.AppendUint16(entries, uint16(len(it)))
		data = append(data, it...)
	}
	start := 12 + len(entries)
	b := le.AppendUint32(nil, 0)
	b = le.AppendUint16(b, uint16(len(d.items)))
	b = le.AppendUint16(b, 0) // blobs
	b = le.AppendUint32(b, uint32(align8len(start+len(data))-start))
	b = append(b, entries...)
	return append(b, data...)
}

// descriptor is the [mrm_pridescex] section: which sections are the
// schema, the decisions, the map and the data.
func (w *priWriter) descriptor(n int) []byte {
	le := binary.LittleEndian
	b := le.AppendUint16(nil, 2)   // deployment-mergeable
	b = le.AppendUint16(b, 0xFFFF) // no included file list
	b = le.AppendUint16(b, 0)
	b = le.AppendUint16(b, 1) // schemas
	b = le.AppendUint16(b, 1) // decision infos
	b = le.AppendUint16(b, 1) // maps
	b = le.AppendUint16(b, 3) // the primary map
	b = le.AppendUint16(b, 0) // referenced files
	b = le.AppendUint16(b, uint16(n-4))
	b = le.AppendUint16(b, 0)
	b = le.AppendUint16(b, 2) // the schema's section
	b = le.AppendUint16(b, 0) // the decisions'
	b = le.AppendUint16(b, 3) // the map's
	for i := 4; i < n; i++ {  // the data items'
		b = le.AppendUint16(b, uint16(i))
	}
	return b
}

// decisionInfo is the [mrm_decn_info] section: the qualifiers, the
// qualifier sets and the decisions, the last two lists of indices in one
// table, and the qualifiers' values, each once (a value that ends another
// is its end).
func (w *priWriter) decisionInfo() []byte {
	le := binary.LittleEndian
	var pool []uint16
	addValue := func(s string) int {
		u := append(utf16.Encode([]rune(s)), 0)
	next:
		for i := 0; i+len(u) <= len(pool); i++ {
			for j := range u {
				if pool[i+j] != u[j] {
					continue next
				}
			}
			return i
		}
		pool = append(pool, u...)
		return len(pool) - len(u)
	}
	// The index table: the empty set's decision's, then the sets', then
	// the other decisions'.
	table := []uint16{0}
	type span struct{ first, n int }
	sets := make([]span, len(w.sets))
	for i, s := range w.sets {
		if i == 0 {
			continue
		}
		sets[i] = span{len(table), len(s)}
		for _, q := range s {
			table = append(table, uint16(q))
		}
	}
	decs := make([]span, len(w.decisions))
	decs[1] = span{0, 1}
	for i, d := range w.decisions {
		if i < 2 {
			continue
		}
		decs[i] = span{len(table), len(d)}
		for _, s := range d {
			table = append(table, uint16(s))
		}
	}
	if len(table) > 0xFFFF {
		return nil
	}
	var distinct []byte
	for i, q := range w.qualifiers {
		off := addValue(q.value)
		if i == 0 {
			// makepri's first qualifier: none, its value empty.
			distinct = le.AppendUint16(distinct, 0)
			distinct = le.AppendUint16(distinct, 0)
			distinct = le.AppendUint16(distinct, 0)
			distinct = le.AppendUint16(distinct, 1)
		} else {
			distinct = le.AppendUint16(distinct, 2)
			distinct = le.AppendUint16(distinct, uint16(q.typ))
			distinct = le.AppendUint16(distinct, 0)
			distinct = le.AppendUint16(distinct, 10)
		}
		distinct = le.AppendUint32(distinct, uint32(off))
	}
	if len(pool) > 0xFFFF {
		return nil
	}
	b := le.AppendUint16(nil, uint16(len(w.qualifiers))) // distinct qualifiers
	b = le.AppendUint16(b, uint16(len(w.qualifiers)))
	b = le.AppendUint16(b, uint16(len(w.sets)))
	b = le.AppendUint16(b, uint16(len(w.decisions)))
	b = le.AppendUint16(b, uint16(len(table)))
	b = le.AppendUint16(b, uint16(len(pool)))
	for _, d := range decs {
		b = le.AppendUint16(b, uint16(d.first))
		b = le.AppendUint16(b, uint16(d.n))
	}
	for _, s := range sets {
		b = le.AppendUint16(b, uint16(s.first))
		b = le.AppendUint16(b, uint16(s.n))
	}
	for i, q := range w.qualifiers {
		b = le.AppendUint16(b, uint16(i))
		if i == 0 {
			b = le.AppendUint32(b, 0)
		} else {
			b = le.AppendUint16(b, priPriority[q.typ])
			b = le.AppendUint16(b, w.p.fallbackScore(q.typ, q.value))
		}
		b = le.AppendUint16(b, 0)
	}
	b = append(b, distinct...)
	for _, v := range table {
		b = le.AppendUint16(b, v)
	}
	for _, c := range pool {
		b = le.AppendUint16(b, c)
	}
	return b
}

// schema is the [mrm_hschemaex] section: the map's names, its scopes and
// items as a tree (each scope's children together, by name), and its
// checksum.
func (w *priWriter) schema() []byte {
	le := binary.LittleEndian
	unique := "ms-appx://" + w.p.Name + "/"
	name := w.p.Name
	// The nodes: the root, then each scope's children (sorted by name),
	// the scopes in their order.
	type node struct {
		scope    *priScope
		item     int
		name     string
		parent   int
		fullPath int
	}
	nodes := []node{{scope: w.root, item: -1}}
	type ex struct{ node, count, first int }
	exs := make([]ex, len(w.scopes))
	itemNode := make([]int, len(w.items))
	scopeNode := make([]int, len(w.scopes))
	for _, s := range w.scopes {
		i := scopeNode[s.index]
		names := append([]string(nil), s.order...)
		sort.Strings(names)
		exs[s.index] = ex{i, len(names), len(nodes)}
		for _, n := range names {
			nd := node{parent: i, item: -1}
			if c, ok := s.children[n]; ok {
				nd.scope, nd.name = c, c.name
				scopeNode[c.index] = len(nodes)
			} else {
				nd.item = s.items[n]
				full := w.items[nd.item].name
				nd.name = full[strings.LastIndexByte(full, '/')+1:]
				itemNode[nd.item] = len(nodes)
			}
			nd.fullPath = len(utf16.Encode([]rune(nd.name)))
			if i != 0 {
				nd.fullPath += nodes[i].fullPath + 1
			}
			nodes = append(nodes, nd)
		}
	}
	// The names: ASCII ones in their pool, the others in UTF-16's, each
	// pool from an empty name; the scopes' in their order, then the
	// items'.
	ascii := []byte{0}
	var uni []uint16
	offsets := make([]int, len(nodes))
	inASCII := make([]bool, len(nodes))
	order := append(append([]int(nil), scopeNode[1:]...), itemNode...)
	maxPath := 0
	for _, ni := range order {
		n := nodes[ni]
		maxPath = max(maxPath, n.fullPath)
		isASCII := true
		for i := 0; i < len(n.name); i++ {
			isASCII = isASCII && n.name[i] < 0x80
		}
		if isASCII {
			inASCII[ni], offsets[ni] = true, len(ascii)
			ascii = append(append(ascii, n.name...), 0)
		} else {
			if len(uni) == 0 {
				uni = []uint16{0}
			}
			offsets[ni] = len(uni)
			uni = append(append(uni, utf16.Encode([]rune(n.name))...), 0)
		}
	}
	if len(ascii) > 0xFFFFF || len(uni) > 0xFFFFF || maxPath > 0xFFFF {
		return nil
	}
	var names []byte
	names = le.AppendUint16(names, uint16(maxPath))
	names = le.AppendUint16(names, 0)
	names = le.AppendUint32(names, uint32(len(nodes)))
	names = le.AppendUint32(names, uint32(len(w.scopes)))
	names = le.AppendUint32(names, uint32(len(w.items)))
	names = le.AppendUint32(names, uint32(len(uni)))
	lengthAt := len(names)
	names = le.AppendUint32(names, 0) // the names' length, below
	names = le.AppendUint32(names, uint32(len(ascii)))
	for i, n := range nodes {
		names = le.AppendUint16(names, uint16(n.parent))
		names = le.AppendUint16(names, uint16(n.fullPath))
		var upper uint16
		if n.name != "" {
			upper = utf16.Encode([]rune{unicode.ToUpper([]rune(n.name)[0])})[0]
		}
		names = le.AppendUint16(names, upper)
		names = append(names, byte(len([]rune(n.name))))
		flags := byte(offsets[i]>>16) & 0xF
		index := n.item
		if n.scope != nil {
			flags |= 0x10
			index = n.scope.index
		}
		if inASCII[i] {
			flags |= 0x20
		}
		names = append(names, flags)
		names = le.AppendUint16(names, uint16(offsets[i]))
		names = le.AppendUint16(names, uint16(index))
	}
	for _, e := range exs {
		names = le.AppendUint16(names, uint16(e.node))
		names = le.AppendUint16(names, uint16(e.count))
		names = le.AppendUint16(names, uint16(e.first))
		names = le.AppendUint16(names, 0)
	}
	for _, n := range itemNode {
		names = le.AppendUint16(names, uint16(n))
	}
	for _, c := range uni {
		names = le.AppendUint16(names, c)
	}
	names = append(names, ascii...)
	names = append(names, make([]byte, align8len(len(names))-len(names))...)
	le.PutUint32(names[lengthAt:], uint32(len(names)))

	// The checksum: the names, the version, every scope's and item's full
	// name, lower-cased (ASCII), by index.
	full := func(ni int) string {
		var parts []string
		for ni != 0 {
			parts = append([]string{nodes[ni].name}, parts...)
			ni = nodes[ni].parent
		}
		return strings.Join(parts, "/")
	}
	h := crc32.NewIEEE()
	str := func(s string) {
		var r []rune
		for _, c := range s {
			if c >= 'A' && c <= 'Z' {
				c += 'a' - 'A'
			}
			r = append(r, c)
		}
		u := utf16Bytes(utf16.Encode(r))
		h.Write(le.AppendUint32(nil, uint32(len(u))))
		h.Write(u)
	}
	str(unique)
	str(name)
	h.Write(le.AppendUint16(le.AppendUint16(nil, 1), 0))
	h.Write(le.AppendUint32(le.AppendUint32(le.AppendUint32(nil, 0), 0), 1))
	h.Write(le.AppendUint32(nil, uint32(len(w.scopes))))
	for _, ni := range scopeNode {
		str(full(ni))
	}
	h.Write(le.AppendUint32(le.AppendUint32(le.AppendUint32(nil, 0), 0), 1))
	h.Write(le.AppendUint32(nil, uint32(len(w.items))))
	for _, ni := range itemNode {
		str(full(ni))
	}

	uniqueU := utf16Bytes(utf16.Encode([]rune(unique)))
	nameU := utf16Bytes(utf16.Encode([]rune(name)))
	b := le.AppendUint16(nil, 1)
	b = le.AppendUint16(b, uint16(len(uniqueU)/2))
	b = le.AppendUint16(b, uint16(len(nameU)/2))
	b = le.AppendUint16(b, 0)
	b = append(b, "[def_hnamesx]  \x00"...)
	b = le.AppendUint16(b, 1) // version 1.0
	b = le.AppendUint16(b, 0)
	b = le.AppendUint32(b, 0)
	b = le.AppendUint32(b, h.Sum32())
	b = le.AppendUint32(b, uint32(len(w.scopes)))
	b = le.AppendUint32(b, uint32(len(w.items)))
	b = append(b, uniqueU...)
	b = append(b, nameU...)
	b = le.AppendUint16(b, 0)
	return append(b, names...)
}

// resourceMap is the [mrm_res_map2_] section: each item's decision and
// candidates, each candidate's value a data item.
func (w *priWriter) resourceMap() []byte {
	le := binary.LittleEndian
	n := 0
	for _, it := range w.items {
		n += len(it.candidates)
	}
	b := le.AppendUint16(nil, 0) // environment references
	b = le.AppendUint16(b, 0)
	b = le.AppendUint16(b, 2) // the schema's section
	b = le.AppendUint16(b, 0) // no schema reference
	b = le.AppendUint16(b, 0) // the decisions' section
	b = le.AppendUint16(b, 7) // value types
	groups := 0
	if len(w.items) > 0 {
		groups = 1
	}
	b = le.AppendUint16(b, uint16(groups))
	b = le.AppendUint16(b, uint16(groups))
	b = le.AppendUint32(b, uint32(len(w.items)))
	b = le.AppendUint32(b, uint32(n))
	b = le.AppendUint32(b, 0) // no inline data
	b = le.AppendUint32(b, 0) // no large table
	for t := 0; t < 7; t++ {
		b = le.AppendUint32(b, 4)
		b = le.AppendUint32(b, uint32(t))
	}
	if groups == 1 {
		b = le.AppendUint16(b, 0) // the items from the first are group 0's
		b = le.AppendUint16(b, 0)
		b = le.AppendUint16(b, uint16(len(w.items))) // group 0: all of them, from the first
		b = le.AppendUint16(b, 0)
	}
	first := 0
	for _, it := range w.items {
		if first > 0xFFFF {
			return nil
		}
		b = le.AppendUint16(b, uint16(it.decision))
		b = le.AppendUint16(b, uint16(first))
		first += len(it.candidates)
	}
	for _, it := range w.items {
		for _, c := range it.candidates {
			b = append(b, 1, byte(c.valueType))
			b = le.AppendUint16(b, 0) // no source file
			b = le.AppendUint16(b, uint16(c.index))
			b = le.AppendUint16(b, uint16(c.section))
		}
	}
	return b
}
