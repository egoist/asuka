package distpkg

import (
	"encoding/binary"
	"fmt"
	"hash/crc32"
	"sort"
	"strings"
	"unicode"
	"unicode/utf16"
)

// A PRI reader, of what makepri writes and Windows' resource manager
// reads (chausner's PriFormat's reading of it, checked more strictly): the
// header, its table of sections and its footer; each section's header and
// footer; the descriptor's section references; the schema's scopes and
// items and its checksum; the decision info's qualifiers, qualifier sets
// and decisions, by index; the resource map's items and their candidates;
// the data items they point into. readPRI returns the file as a tree of
// named resources, each candidate's qualifiers and value, and dump prints
// it as makepri's detailed dump would.

type priRead struct {
	magic    string
	sections []priReadSection
	desc     struct {
		flags                           uint16
		schemas, decisions, maps, files []int
		data                            []int
		primary                         int
	}
	schema struct {
		uniqueName, name   string
		major, minor       uint16
		checksum           uint32
		scopes             []priReadNode // by scope index
		items              []priReadNode // by item index
		nodes              []priReadNode // in the file's order
		scopeOrder         []int         // scope indices in the file's order
		itemOrder          []int         // item indices in the file's order
		maxPath            int
		extended, inASCII  bool
		unicodeLen, asciiN int
	}
	qualifiers []priReadQualifier
	sets       [][]int // qualifier indices
	decisions  [][]int // qualifier set indices
	indexTable []uint16
	distinct   []priReadDistinct
	items      map[int]priReadItem // by item index
	dataItems  map[int][][]byte    // by section index
	valueTypes []uint32
}

type priReadSection struct {
	id         string
	qualifier  uint32
	flags      uint16
	secFlags   uint16
	content    []byte
	tocOffset  uint32
	tocLength  uint32
	fileOffset int
}

type priReadNode struct {
	parent     int // node index
	name       string
	isScope    bool
	index      int
	fullPath   int
	upperFirst uint16
	nameLen    int
	children   []int // node indices, for scopes
}

type priReadQualifier struct {
	distinct      int
	typ           int
	value         string
	priority      int
	fallbackScore int
}

type priReadDistinct struct {
	f1, typ, f3, f4 uint16
	offset          uint32
}

type priReadItem struct {
	decision   int
	candidates []priReadCandidate
}

type priReadCandidate struct {
	set       int
	valueType uint32
	section   int
	index     int
	value     []byte
}

var priTypeNames = []string{"Language", "Contrast", "Scale", "HomeRegion", "TargetSize", "LayoutDirection", "Theme", "AlternateForm", "DXFeatureLevel", "Configuration", "DeviceFamily", "Custom"}

type priBuf struct {
	b   []byte
	off int
	err error
}

func (r *priBuf) fail(f string, a ...any) {
	if r.err == nil {
		r.err = fmt.Errorf("at %d: "+f, append([]any{r.off}, a...)...)
	}
}

func (r *priBuf) bytes(n int) []byte {
	if r.err != nil || n < 0 || r.off+n > len(r.b) {
		r.fail("%d bytes past the end (%d)", n, len(r.b))
		return make([]byte, n)
	}
	b := r.b[r.off : r.off+n]
	r.off += n
	return b
}

func (r *priBuf) u8() uint8   { return r.bytes(1)[0] }
func (r *priBuf) u16() uint16 { return binary.LittleEndian.Uint16(r.bytes(2)) }
func (r *priBuf) u32() uint32 { return binary.LittleEndian.Uint32(r.bytes(4)) }

func (r *priBuf) expect16(v uint16, what string) {
	if g := r.u16(); g != v {
		r.fail("%s: %d, want %d", what, g, v)
	}
}

func (r *priBuf) expect32(v uint32, what string) {
	if g := r.u32(); g != v {
		r.fail("%s: %d, want %d", what, g, v)
	}
}

// utf16z reads a NUL-terminated UTF-16 string at byte offset at.
func utf16z(b []byte, at int) (string, error) {
	var u []uint16
	for i := at; ; i += 2 {
		if i+2 > len(b) {
			return "", fmt.Errorf("an unterminated string at %d", at)
		}
		c := binary.LittleEndian.Uint16(b[i:])
		if c == 0 {
			break
		}
		u = append(u, c)
	}
	return string(utf16.Decode(u)), nil
}

func asciiz(b []byte, at int) (string, error) {
	for i := at; i < len(b); i++ {
		if b[i] == 0 {
			return string(b[at:i]), nil
		}
	}
	return "", fmt.Errorf("an unterminated string at %d", at)
}

func align8(n int) int { return (n + 7) &^ 7 }

func readPRI(b []byte) (*priRead, error) {
	p := &priRead{items: map[int]priReadItem{}, dataItems: map[int][][]byte{}}
	r := &priBuf{b: b}
	p.magic = string(r.bytes(8))
	if p.magic != "mrm_pri2" {
		return nil, fmt.Errorf("magic %q", p.magic)
	}
	r.expect16(0, "header")
	r.expect16(1, "header")
	total := r.u32()
	toc := r.u32()
	start := r.u32()
	n := int(r.u16())
	r.expect16(0xFFFF, "header")
	r.expect32(0, "header")
	if r.err != nil {
		return nil, r.err
	}
	if int(total) != len(b) {
		return nil, fmt.Errorf("total size %d of %d bytes", total, len(b))
	}
	if toc != 32 || int(start) != 32+32*n {
		return nil, fmt.Errorf("the table of contents at %d, the sections at %d", toc, start)
	}
	f := &priBuf{b: b, off: len(b) - 16}
	f.expect32(0xDEFFFADE, "footer")
	f.expect32(total, "footer size")
	if m := string(f.bytes(8)); m != p.magic {
		return nil, fmt.Errorf("footer magic %q", m)
	}
	if f.err != nil {
		return nil, f.err
	}
	r.off = int(toc)
	next := uint32(0)
	for i := 0; i < n; i++ {
		var s priReadSection
		s.id = string(r.bytes(16))
		s.flags = r.u16()
		s.secFlags = r.u16()
		s.qualifier = r.u32()
		s.tocOffset = r.u32()
		s.tocLength = r.u32()
		if s.tocOffset != next {
			return nil, fmt.Errorf("section %d at %d, after the one before at %d", i, s.tocOffset, next)
		}
		next += s.tocLength
		sr := &priBuf{b: b, off: int(start + s.tocOffset)}
		s.fileOffset = sr.off
		if id := string(sr.bytes(16)); id != s.id {
			return nil, fmt.Errorf("section %d: %q, the table says %q", i, id, s.id)
		}
		if q := sr.u32(); q != s.qualifier {
			return nil, fmt.Errorf("section %d's qualifier %d, the table's %d", i, q, s.qualifier)
		}
		if fl := sr.u16(); fl != s.flags {
			return nil, fmt.Errorf("section %d's flags", i)
		}
		if fl := sr.u16(); fl != s.secFlags {
			return nil, fmt.Errorf("section %d's section flags", i)
		}
		length := sr.u32()
		sr.expect32(0, "section header")
		if length != s.tocLength || length%8 != 0 || length < 40 {
			return nil, fmt.Errorf("section %d's length %d, the table's %d", i, length, s.tocLength)
		}
		s.content = sr.bytes(int(length) - 40)
		sr.expect32(0xDEF5FADE, "section footer")
		sr.expect32(length, "section footer length")
		if sr.err != nil {
			return nil, fmt.Errorf("section %d: %v", i, sr.err)
		}
		p.sections = append(p.sections, s)
	}
	if int(start+next) != len(b)-16 {
		return nil, fmt.Errorf("the sections end at %d, the footer at %d", start+next, len(b)-16)
	}
	if r.err != nil {
		return nil, r.err
	}
	descs := 0
	for i, s := range p.sections {
		if s.id == "[mrm_pridescex]\x00" {
			descs++
			if err := p.readDescriptor(s.content); err != nil {
				return nil, fmt.Errorf("[mrm_pridescex]: %v", err)
			}
		}
		_ = i
	}
	if descs != 1 {
		return nil, fmt.Errorf("%d descriptors", descs)
	}
	if len(p.desc.schemas) != 1 || len(p.desc.decisions) != 1 || len(p.desc.maps) != 1 || p.desc.primary != p.desc.maps[0] {
		return nil, fmt.Errorf("descriptor: %d schemas, %d decision infos, %d maps (primary %d)", len(p.desc.schemas), len(p.desc.decisions), len(p.desc.maps), p.desc.primary)
	}
	want := func(i int, id string) (*priReadSection, error) {
		if i < 0 || i >= len(p.sections) || p.sections[i].id != id {
			return nil, fmt.Errorf("section %d isn't %q", i, id)
		}
		return &p.sections[i], nil
	}
	s, err := want(p.desc.schemas[0], "[mrm_hschemaex] ")
	if err != nil {
		return nil, err
	}
	if err := p.readSchema(s.content); err != nil {
		return nil, fmt.Errorf("[mrm_hschemaex]: %v", err)
	}
	if s, err = want(p.desc.decisions[0], "[mrm_decn_info]\x00"); err != nil {
		return nil, err
	}
	if err := p.readDecisions(s.content); err != nil {
		return nil, fmt.Errorf("[mrm_decn_info]: %v", err)
	}
	for _, i := range p.desc.data {
		if s, err = want(i, "[mrm_dataitem] \x00"); err != nil {
			return nil, err
		}
		items, err := readDataItems(s.content)
		if err != nil {
			return nil, fmt.Errorf("[mrm_dataitem] %d: %v", i, err)
		}
		p.dataItems[i] = items
	}
	if s, err = want(p.desc.maps[0], "[mrm_res_map2_]\x00"); err != nil {
		return nil, err
	}
	if err := p.readMap(s.content); err != nil {
		return nil, fmt.Errorf("[mrm_res_map2_]: %v", err)
	}
	return p, nil
}

func (p *priRead) readDescriptor(b []byte) error {
	r := &priBuf{b: b}
	p.desc.flags = r.u16()
	if r.u16() != 0xFFFF {
		return fmt.Errorf("an included file list")
	}
	r.expect16(0, "descriptor")
	ns, nd, nm := r.u16(), r.u16(), r.u16()
	p.desc.primary = int(r.u16())
	nf, ndata := r.u16(), r.u16()
	r.expect16(0, "descriptor")
	list := func(n uint16) []int {
		var l []int
		for i := 0; i < int(n); i++ {
			l = append(l, int(r.u16()))
		}
		return l
	}
	p.desc.schemas, p.desc.decisions, p.desc.maps, p.desc.files, p.desc.data = list(ns), list(nd), list(nm), list(nf), list(ndata)
	if r.err == nil && align8(r.off) != len(b) {
		return fmt.Errorf("%d bytes, of %d", r.off, len(b))
	}
	return r.err
}

func (p *priRead) readSchema(b []byte) error {
	sc := &p.schema
	r := &priBuf{b: b}
	r.expect16(1, "schema")
	uniqueLen, nameLen := int(r.u16()), int(r.u16())
	r.expect16(0, "schema")
	switch id := string(r.bytes(16)); id {
	case "[def_hnamesx]  \x00":
		sc.extended = true
	case "[def_hnames]   \x00":
	default:
		return fmt.Errorf("names %q", id)
	}
	sc.major, sc.minor = r.u16(), r.u16()
	r.expect32(0, "schema version")
	sc.checksum = r.u32()
	nScopes, nItems := int(r.u32()), int(r.u32())
	if r.err != nil {
		return r.err
	}
	uname, err := utf16z(r.bytes(uniqueLen*2), 0)
	if err != nil {
		return err
	}
	name, err := utf16z(r.bytes(nameLen*2), 0)
	if err != nil {
		return err
	}
	sc.uniqueName, sc.name = uname, name
	r.expect16(0, "after the names")
	hnames := r.off
	sc.maxPath = int(r.u16())
	r.expect16(0, "names")
	r.expect32(uint32(nScopes+nItems), "nodes")
	r.expect32(uint32(nScopes), "scopes")
	r.expect32(uint32(nItems), "items")
	sc.unicodeLen = int(r.u32())
	totalNames := int(r.u32())
	if sc.extended {
		sc.asciiN = int(r.u32())
	}
	type info struct {
		parent, fullPath, upper int
		nameLen, flags          int
		nameOffset, index       int
	}
	infos := make([]info, nScopes+nItems)
	for i := range infos {
		infos[i].parent = int(r.u16())
		infos[i].fullPath = int(r.u16())
		infos[i].upper = int(r.u16())
		infos[i].nameLen = int(r.u8())
		infos[i].flags = int(r.u8())
		infos[i].nameOffset = int(r.u16()) | (infos[i].flags&0xF)<<16
		infos[i].index = int(r.u16())
	}
	type ex struct{ node, count, first int }
	exs := make([]ex, nScopes)
	for i := range exs {
		exs[i].node = int(r.u16())
		exs[i].count = int(r.u16())
		exs[i].first = int(r.u16())
		r.expect16(0, "scope")
	}
	itemNodes := make([]int, nItems)
	for i := range itemNodes {
		itemNodes[i] = int(r.u16())
	}
	uni := r.bytes(sc.unicodeLen * 2)
	ascii := r.bytes(sc.asciiN)
	if r.err != nil {
		return r.err
	}
	if align8(r.off-hnames) != totalNames || align8(hnames+totalNames) != len(b) {
		return fmt.Errorf("the names' length %d (they end at %d of %d)", totalNames, r.off, len(b))
	}
	sc.scopes = make([]priReadNode, nScopes)
	sc.items = make([]priReadNode, nItems)
	seenScope, seenItem := map[int]bool{}, map[int]bool{}
	for i, in := range infos {
		var name string
		var err error
		if in.fullPath != 0 {
			if in.flags&0x20 != 0 {
				name, err = asciiz(ascii, in.nameOffset)
			} else {
				name, err = utf16z(uni, in.nameOffset*2)
			}
			if err != nil {
				return fmt.Errorf("node %d: %v", i, err)
			}
		}
		if in.parent >= len(infos) {
			return fmt.Errorf("node %d's parent %d", i, in.parent)
		}
		if len([]rune(name)) != in.nameLen && !(in.nameLen == 255 && len([]rune(name)) > 255) {
			return fmt.Errorf("node %d (%s): its name's length %d", i, name, in.nameLen)
		}
		var first uint16
		if name != "" {
			first = utf16.Encode([]rune{unicode.ToUpper([]rune(name)[0])})[0]
		}
		if uint16(in.upper) != first {
			return fmt.Errorf("node %d (%s): its upper-case first letter %q", i, name, rune(in.upper))
		}
		n := priReadNode{parent: in.parent, name: name, isScope: in.flags&0x10 != 0, index: in.index, fullPath: in.fullPath, upperFirst: uint16(in.upper), nameLen: in.nameLen}
		sc.nodes = append(sc.nodes, n)
		if n.isScope {
			if in.index >= nScopes || seenScope[in.index] {
				return fmt.Errorf("node %d: scope %d", i, in.index)
			}
			seenScope[in.index] = true
		} else {
			if in.index >= nItems || seenItem[in.index] {
				return fmt.Errorf("node %d: item %d", i, in.index)
			}
			seenItem[in.index] = true
		}
	}
	if len(sc.nodes) == 0 || !sc.nodes[0].isScope || sc.nodes[0].name != "" || sc.nodes[0].index != 0 {
		return fmt.Errorf("no root scope")
	}
	for i, e := range exs {
		if e.node >= len(sc.nodes) || !sc.nodes[e.node].isScope {
			return fmt.Errorf("scope %d's node %d", i, e.node)
		}
		if sc.nodes[e.node].index != i {
			return fmt.Errorf("scope %d is node %d's, scope %d", i, e.node, sc.nodes[e.node].index)
		}
		for j := e.first; j < e.first+e.count; j++ {
			if j >= len(sc.nodes) || sc.nodes[j].parent != e.node {
				return fmt.Errorf("scope %d's child %d", i, j)
			}
			sc.nodes[e.node].children = append(sc.nodes[e.node].children, j)
		}
		sc.scopeOrder = append(sc.scopeOrder, i)
		sc.scopes[i] = sc.nodes[e.node]
	}
	for i, nd := range itemNodes {
		if nd >= len(sc.nodes) || sc.nodes[nd].isScope || sc.nodes[nd].index != i {
			return fmt.Errorf("item %d's node %d", i, nd)
		}
		sc.items[i] = sc.nodes[nd]
		sc.itemOrder = append(sc.itemOrder, i)
	}
	// Every node but the root is its parent scope's child, the children
	// sorted by their names (upper-cased), and its full path's length is
	// its parent's and its own name's.
	maxPath := 0
	for i, n := range sc.nodes {
		if i == 0 {
			continue
		}
		par := sc.nodes[n.parent]
		found := false
		for _, c := range par.children {
			found = found || c == i
		}
		if !found {
			return fmt.Errorf("node %d (%s) isn't its parent's child", i, n.name)
		}
		want := len(utf16.Encode([]rune(n.name)))
		if n.parent != 0 {
			want += par.fullPath + 1
		}
		if n.fullPath != want {
			return fmt.Errorf("node %d (%s): its full path's length %d, want %d", i, n.name, n.fullPath, want)
		}
		maxPath = max(maxPath, n.fullPath)
	}
	if maxPath != sc.maxPath {
		return fmt.Errorf("the longest full path %d, the schema says %d", maxPath, sc.maxPath)
	}
	for _, n := range sc.nodes {
		for k := 1; k < len(n.children); k++ {
			if strings.ToUpper(sc.nodes[n.children[k-1]].name) >= strings.ToUpper(sc.nodes[n.children[k]].name) {
				return fmt.Errorf("%s's children out of order", n.name)
			}
		}
	}
	if c := p.schemaChecksum(); c != sc.checksum {
		return fmt.Errorf("checksum %#x, want %#x", sc.checksum, c)
	}
	return nil
}

func (p *priRead) fullName(n priReadNode) string {
	var parts []string
	for n.parent != 0 || n.name != "" {
		parts = append([]string{n.name}, parts...)
		if n.parent == 0 {
			break
		}
		n = p.schema.nodes[n.parent]
	}
	return strings.Join(parts, "/")
}

// schemaChecksum is the schema's CRC-32, as PriFormat computes it.
func (p *priRead) schemaChecksum() uint32 {
	sc := &p.schema
	h := crc32.NewIEEE()
	le := func(v uint32) { h.Write(binary.LittleEndian.AppendUint32(nil, v)) }
	str := func(s string) {
		var lower []rune
		for _, r := range s {
			if r >= 'A' && r <= 'Z' {
				r += 'a' - 'A'
			}
			lower = append(lower, r)
		}
		u := append(utf16.Encode(lower), 0)
		b := make([]byte, 2*len(u))
		for i, c := range u {
			binary.LittleEndian.PutUint16(b[2*i:], c)
		}
		le(uint32(len(b)))
		h.Write(b)
	}
	str(sc.uniqueName)
	str(sc.name)
	h.Write(binary.LittleEndian.AppendUint16(nil, sc.major))
	h.Write(binary.LittleEndian.AppendUint16(nil, sc.minor))
	le(0)
	le(0)
	le(1)
	le(uint32(len(sc.scopes)))
	for _, i := range sc.scopeOrder {
		str(strings.ReplaceAll(p.fullName(sc.scopes[i]), "\\", "/"))
	}
	le(0)
	le(0)
	le(1)
	le(uint32(len(sc.items)))
	for _, i := range sc.itemOrder {
		str(p.fullName(sc.items[i]))
	}
	return h.Sum32()
}

func (p *priRead) readDecisions(b []byte) error {
	r := &priBuf{b: b}
	nDistinct, nQual, nSets, nDec, nIndex, dataLen := int(r.u16()), int(r.u16()), int(r.u16()), int(r.u16()), int(r.u16()), int(r.u16())
	type span struct{ first, n int }
	decs := make([]span, nDec)
	for i := range decs {
		decs[i] = span{int(r.u16()), int(r.u16())}
	}
	sets := make([]span, nSets)
	for i := range sets {
		sets[i] = span{int(r.u16()), int(r.u16())}
	}
	type qinfo struct{ distinct, priority, fallback int }
	quals := make([]qinfo, nQual)
	for i := range quals {
		quals[i] = qinfo{int(r.u16()), int(r.u16()), int(r.u16())}
		r.expect16(0, "qualifier")
	}
	p.distinct = make([]priReadDistinct, nDistinct)
	for i := range p.distinct {
		d := &p.distinct[i]
		d.f1, d.typ, d.f3, d.f4 = r.u16(), r.u16(), r.u16(), r.u16()
		d.offset = r.u32()
	}
	p.indexTable = make([]uint16, nIndex)
	for i := range p.indexTable {
		p.indexTable[i] = r.u16()
	}
	data := r.bytes(dataLen * 2)
	if r.err != nil {
		return r.err
	}
	if align8(r.off) != len(b) {
		return fmt.Errorf("%d bytes, of %d", r.off, len(b))
	}
	for i, q := range quals {
		if q.distinct >= nDistinct {
			return fmt.Errorf("qualifier %d's %d", i, q.distinct)
		}
		d := p.distinct[q.distinct]
		v, err := utf16z(data, int(d.offset)*2)
		if err != nil {
			return err
		}
		p.qualifiers = append(p.qualifiers, priReadQualifier{distinct: q.distinct, typ: int(d.typ), value: v, priority: q.priority, fallbackScore: q.fallback})
	}
	for i, s := range sets {
		if s.first+s.n > nIndex {
			return fmt.Errorf("qualifier set %d", i)
		}
		var l []int
		for j := s.first; j < s.first+s.n; j++ {
			if int(p.indexTable[j]) >= nQual {
				return fmt.Errorf("qualifier set %d's %d", i, p.indexTable[j])
			}
			l = append(l, int(p.indexTable[j]))
		}
		p.sets = append(p.sets, l)
	}
	for i, d := range decs {
		if d.first+d.n > nIndex {
			return fmt.Errorf("decision %d", i)
		}
		var l []int
		for j := d.first; j < d.first+d.n; j++ {
			if int(p.indexTable[j]) >= nSets {
				return fmt.Errorf("decision %d's %d", i, p.indexTable[j])
			}
			l = append(l, int(p.indexTable[j]))
		}
		p.decisions = append(p.decisions, l)
	}
	return nil
}

func readDataItems(b []byte) ([][]byte, error) {
	r := &priBuf{b: b}
	r.expect32(0, "data items")
	nStrings, nBlobs := int(r.u16()), int(r.u16())
	total := int(r.u32())
	type ent struct{ off, n int }
	var ents []ent
	for i := 0; i < nStrings; i++ {
		ents = append(ents, ent{int(r.u16()), int(r.u16())})
	}
	for i := 0; i < nBlobs; i++ {
		ents = append(ents, ent{int(r.u32()), int(r.u32())})
	}
	if r.err != nil {
		return nil, r.err
	}
	start := r.off
	if align8(start+total) != len(b) {
		return nil, fmt.Errorf("%d bytes of data at %d, of %d", total, start, len(b))
	}
	var items [][]byte
	for _, e := range ents {
		if e.off+e.n > total {
			return nil, fmt.Errorf("an item past the data")
		}
		items = append(items, b[start+e.off:start+e.off+e.n])
	}
	return items, nil
}

func (p *priRead) readMap(b []byte) error {
	r := &priBuf{b: b}
	r.expect16(0, "environment references")
	r.expect16(0, "environment references")
	if s := int(r.u16()); s != p.desc.schemas[0] {
		return fmt.Errorf("its schema %d", s)
	}
	schemaRef := int(r.u16())
	if s := int(r.u16()); s != p.desc.decisions[0] {
		return fmt.Errorf("its decisions %d", s)
	}
	nTypes, nGroupMap, nGroups := int(r.u16()), int(r.u16()), int(r.u16())
	nInfos, nCands, dataLen, large := int(r.u32()), int(r.u32()), int(r.u32()), int(r.u32())
	if schemaRef != 0 || large != 0 {
		return fmt.Errorf("a schema reference (%d) or a large table (%d)", schemaRef, large)
	}
	for i := 0; i < nTypes; i++ {
		r.expect32(4, "value type")
		p.valueTypes = append(p.valueTypes, r.u32())
	}
	type gm struct{ first, group int }
	gms := make([]gm, nGroupMap)
	for i := range gms {
		gms[i] = gm{int(r.u16()), int(r.u16())}
	}
	type grp struct{ size, first int }
	grps := make([]grp, nGroups)
	for i := range grps {
		grps[i] = grp{int(r.u16()), int(r.u16())}
	}
	type info struct{ decision, first int }
	infos := make([]info, nInfos)
	for i := range infos {
		infos[i] = info{int(r.u16()), int(r.u16())}
	}
	type cand struct {
		typ, vt          int
		src, idx, sec    int
		length, dataOffs int
	}
	cands := make([]cand, nCands)
	for i := range cands {
		c := &cands[i]
		c.typ = int(r.u8())
		c.vt = int(r.u8())
		switch c.typ {
		case 1:
			c.src, c.idx, c.sec = int(r.u16()), int(r.u16()), int(r.u16())
		case 0:
			c.length, c.dataOffs = int(r.u16()), int(r.u32())
		default:
			return fmt.Errorf("candidate %d's type %d", i, c.typ)
		}
	}
	data := r.bytes(dataLen)
	if r.err != nil {
		return r.err
	}
	if align8(r.off) != len(b) {
		return fmt.Errorf("%d bytes, of %d", r.off, len(b))
	}
	used := 0
	for _, g := range gms {
		var gr grp
		if g.group < len(grps) {
			gr = grps[g.group]
		} else {
			gr = grp{1, g.group - len(grps)}
		}
		for k := 0; k < gr.size; k++ {
			in := infos[gr.first+k]
			item := g.first + k
			if item >= len(p.schema.items) {
				return fmt.Errorf("item %d of %d", item, len(p.schema.items))
			}
			if in.decision >= len(p.decisions) {
				return fmt.Errorf("item %d's decision %d", item, in.decision)
			}
			if _, dup := p.items[item]; dup {
				return fmt.Errorf("item %d twice", item)
			}
			it := priReadItem{decision: in.decision}
			for j, set := range p.decisions[in.decision] {
				if in.first+j >= nCands {
					return fmt.Errorf("item %d's candidates", item)
				}
				c := cands[in.first+j]
				if c.vt >= len(p.valueTypes) {
					return fmt.Errorf("item %d's value type %d", item, c.vt)
				}
				pc := priReadCandidate{set: set, valueType: p.valueTypes[c.vt]}
				if c.typ == 1 {
					items, ok := p.dataItems[c.sec]
					if !ok || c.idx >= len(items) || c.src != 0 {
						return fmt.Errorf("item %d's candidate %d: data item %d of section %d", item, j, c.idx, c.sec)
					}
					pc.section, pc.index, pc.value = c.sec, c.idx, items[c.idx]
				} else {
					if c.dataOffs+c.length > len(data) {
						return fmt.Errorf("item %d's candidate %d: past the data", item, j)
					}
					pc.section, pc.value = -1, data[c.dataOffs:c.dataOffs+c.length]
				}
				it.candidates = append(it.candidates, pc)
				used++
			}
			p.items[item] = it
		}
	}
	if used != nCands {
		return fmt.Errorf("%d candidates, %d of them an item's", nCands, used)
	}
	return nil
}

// value is a candidate's value as text.
func (c priReadCandidate) text() (string, error) {
	v := c.value
	switch c.valueType {
	case 0, 1: // String, Path: UTF-16
		if len(v)%2 != 0 {
			return "", fmt.Errorf("odd UTF-16")
		}
		s, err := utf16z(v, 0)
		if err == nil && len(utf16.Encode([]rune(s)))*2+2 != len(v) {
			err = fmt.Errorf("a value's length %d isn't its string's", len(v))
		}
		return s, err
	case 3, 4, 5, 6: // ASCII and UTF-8 strings and paths
		if len(v) == 0 || v[len(v)-1] != 0 || strings.IndexByte(string(v[:len(v)-1]), 0) >= 0 {
			return "", fmt.Errorf("a value not terminated once: %q", v)
		}
		return string(v[:len(v)-1]), nil
	}
	return "", fmt.Errorf("value type %d", c.valueType)
}

var priValueTypeNames = []string{"String", "Path", "EmbeddedData", "AsciiString", "Utf8String", "AsciiPath", "Utf8Path"}

func (p *priRead) qualifierText(q int) string {
	x := p.qualifiers[q]
	return fmt.Sprintf("%s=%s (priority %d, default %d)", priTypeNames[x.typ], x.value, x.priority, x.fallbackScore)
}

// dump prints the file's resources as makepri's detailed dump would: its
// map, its resources by name, each candidate's qualifiers and value.
func (p *priRead) dump() string {
	var b strings.Builder
	fmt.Fprintf(&b, "map %s (%s) flags %d\n", p.schema.name, p.schema.uniqueName, p.desc.flags)
	type res struct {
		name string
		item int
	}
	var all []res
	for i, n := range p.schema.items {
		all = append(all, res{p.fullName(n), i})
	}
	sort.Slice(all, func(i, j int) bool { return all[i].name < all[j].name })
	for _, r := range all {
		it := p.items[r.item]
		fmt.Fprintf(&b, "%s\n", r.name)
		for _, c := range it.candidates {
			var qs []string
			for _, q := range p.sets[c.set] {
				qs = append(qs, p.qualifierText(q))
			}
			v, err := c.text()
			if err != nil {
				v = "error: " + err.Error()
			}
			fmt.Fprintf(&b, "  [%s] %s %q\n", strings.Join(qs, ", "), priValueTypeNames[c.valueType], v)
		}
	}
	return b.String()
}

// raw prints every table of the file by index: what dump leaves out.
func (p *priRead) raw() string {
	var b strings.Builder
	for i, s := range p.sections {
		fmt.Fprintf(&b, "section %d %q flags %d/%d qualifier %d: %d bytes\n", i, strings.TrimRight(s.id, "\x00 "), s.flags, s.secFlags, s.qualifier, len(s.content)+40)
	}
	fmt.Fprintf(&b, "descriptor: flags %d schemas %v decisions %v maps %v (primary %d) files %v data %v\n", p.desc.flags, p.desc.schemas, p.desc.decisions, p.desc.maps, p.desc.primary, p.desc.files, p.desc.data)
	sc := &p.schema
	fmt.Fprintf(&b, "schema %q %q %d.%d checksum %#x extended %v unicode %d ascii %d maxPath %d\n", sc.uniqueName, sc.name, sc.major, sc.minor, sc.checksum, sc.extended, sc.unicodeLen, sc.asciiN, sc.maxPath)
	for i, n := range sc.nodes {
		kind := "item"
		if n.isScope {
			kind = "scope"
		}
		fmt.Fprintf(&b, "  node %d: %s %d %q parent %d children %v\n", i, kind, n.index, n.name, n.parent, n.children)
	}
	fmt.Fprintf(&b, "  scopes in order %v, items in order %v\n", sc.scopeOrder, sc.itemOrder)
	for i, d := range p.distinct {
		fmt.Fprintf(&b, "distinct %d: %d %s %d %d @%d\n", i, d.f1, priTypeNames[d.typ], d.f3, d.f4, d.offset)
	}
	for i := range p.qualifiers {
		fmt.Fprintf(&b, "qualifier %d: %s (distinct %d)\n", i, p.qualifierText(i), p.qualifiers[i].distinct)
	}
	for i, s := range p.sets {
		fmt.Fprintf(&b, "set %d: %v\n", i, s)
	}
	for i, d := range p.decisions {
		fmt.Fprintf(&b, "decision %d: %v\n", i, d)
	}
	fmt.Fprintf(&b, "index table %v\n", p.indexTable)
	fmt.Fprintf(&b, "value types %v\n", p.valueTypes)
	var keys []int
	for k := range p.items {
		keys = append(keys, k)
	}
	sort.Ints(keys)
	for _, k := range keys {
		it := p.items[k]
		fmt.Fprintf(&b, "item %d (%s): decision %d\n", k, p.fullName(sc.items[k]), it.decision)
		for _, c := range it.candidates {
			fmt.Fprintf(&b, "  set %d %s section %d #%d %q\n", c.set, priValueTypeNames[c.valueType], c.section, c.index, c.value)
		}
	}
	for _, i := range p.desc.data {
		fmt.Fprintf(&b, "data %d: %d items\n", i, len(p.dataItems[i]))
	}
	return b.String()
}
