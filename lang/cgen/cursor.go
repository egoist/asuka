package cgen

import (
	"fmt"
	"sort"
	"strings"

	bc "github.com/egoist/asuka/lang/bytecode"
)

// Write cursors: a fused loop's appends (lang/ir's seqfuse.go: an
// array's map, filter and compactMap, arr.append_room and
// arr.append_own) store through a pointer the loop keeps in a C local,
// not through the array.
//
// Each append into the loop's own array read its buffer and its count
// from the array, stored the element and stored the count back: the
// element's store may alias both (as far as the C compiler knows) and
// the loop's overflow checks leave it between them, so neither stays in
// a register. Go's indexed store into a slice it holds has none of
// that. Here the loop keeps where the next element goes (W<n>_) and
// where the array's room ends (E<n>_) in locals, opened as the loop is
// entered; an append is a store and an increment (and, a filter's, a
// compare with the end: on none left the count is written back, the
// array grown by the runtime and the cursor opened again). The count is
// written back (closed) on every way out of the loop: falling out of
// its end, a jump out, an error (to its handler, or the function's
// unwinding).
//
// A loop has one when it's a range of the instructions (its first, the
// header, the target of the jumps back from its last), come into only
// by falling into its header (no jump from outside into it, no handler
// in it), returning nowhere from inside, and its array is a register
// that every instruction of the loop naming it is an append's (the
// lowering's appendReg: MOV b, r; MOV r, none; CALLB append b, 2; MOV r,
// b; MOV b, none): nothing else reads, copies, passes or replaces the
// array while the loop runs, so nothing sees its count stale. The
// array is the loop's own (made by its NEWA, unique: the appends'
// contract), its elements values (ints, floats, references).

type cursor struct {
	id   int
	h, l int     // the loop's first and last instructions, by index
	r    int     // the array's register
	kind bc.Kind // its elements'
}

// stub is the code on a way out of cursor loops: their counts written
// back, then on to where the way goes.
type stub struct {
	name  string
	flush []*cursor
	to    string // C: the jump on
}

// cursors finds the function's loops with a write cursor: by index, the
// appends' CALLBs (dance) and the loops' headers (open) and last
// instructions (close).
func (g *gen) cursors(ins []insn, targets map[int]bool) {
	g.curs = nil
	g.curDance = map[int]*cursor{}
	g.stubs = nil
	if g.async {
		return
	}
	n := len(ins)
	at := make(map[int]int, n)
	for k, in := range ins {
		at[in.pc] = k
	}
	g.at = at
	// Loops: a header and the last of the jumps back to it.
	latch := map[int]int{}
	for k, x := range ins {
		for o, kind := range bc.Ops[x.op].Operands {
			if kind != bc.Off {
				continue
			}
			if t, ok := at[x.args[o]]; ok && t <= k && k > latch[t] {
				latch[t] = k
			}
		}
	}
	var heads []int
	for h := range latch {
		heads = append(heads, h)
	}
	// Outer loops first: a loop inside one whose array it is leaves it to
	// that one.
	sort.Slice(heads, func(i, j int) bool {
		a, b := heads[i], heads[j]
		if la, lb := latch[a]-a, latch[b]-b; la != lb {
			return la > lb
		}
		return a < b
	})
	handlers := map[int]bool{}
	for _, hd := range g.f.Handlers {
		if t, ok := at[int(hd.Target)]; ok {
			handlers[t] = true
		}
	}
	// none: reference registers only ever none (what appendReg moves into
	// the array's register while the call has it).
	none := make([]bool, len(g.f.Kinds))
	for r, k := range g.f.Kinds {
		none[r] = k == bc.KRef && r >= int(g.f.NParams)
	}
	for _, x := range ins {
		nm := bc.Ops[x.op].Name
		for _, r := range writes(g.m, x) {
			if nm != "KNONE" && nm != "CLR" && r < len(none) {
				none[r] = false
			}
		}
	}
	claimed := map[int]bool{} // dances an outer loop's cursor has
	for _, h := range heads {
		l := latch[h]
		if !g.cursorLoop(ins, h, l, at, handlers) {
			continue
		}
		// The appends in it, by array register.
		dances := map[int][]int{}
		kinds := map[int]bc.Kind{}
		for k := h; k+4 <= l; k++ {
			if r, kind, ok := g.dance(ins, k, none, targets); ok && !claimed[k+2] {
				if prev, seen := kinds[r]; seen && prev != kind {
					kinds[r] = bc.KVoid
				} else {
					kinds[r] = kind
				}
				dances[r] = append(dances[r], k)
			}
		}
		var rs []int
		for r := range dances {
			rs = append(rs, r)
		}
		sort.Ints(rs)
		for _, r := range rs {
			if kinds[r] == bc.KVoid || !g.onlyDances(ins, h, l, r, dances[r]) {
				continue
			}
			c := &cursor{id: len(g.curs), h: h, l: l, r: r, kind: kinds[r]}
			g.curs = append(g.curs, c)
			for _, k := range dances[r] {
				claimed[k+2] = true
				g.curDance[k+2] = c
			}
		}
	}
}

// cursorLoop reports whether ins[h..l] is a loop a cursor may be kept
// in: come into only by falling into h, no handler in it, no return.
func (g *gen) cursorLoop(ins []insn, h, l int, at map[int]int, handlers map[int]bool) bool {
	if h == 0 {
		return false
	}
	switch bc.Ops[ins[h-1].op].Name {
	case "JMP", "RET", "RET0", "THROW", "RETHROW":
		return false // not fallen into
	}
	for k, x := range ins {
		nm := bc.Ops[x.op].Name
		if k >= h && k <= l {
			if handlers[k] || nm == "RET" || nm == "RET0" || nm == "YIELD" || nm == "AWAIT" {
				return false
			}
			continue
		}
		for o, kind := range bc.Ops[x.op].Operands {
			if kind != bc.Off {
				continue
			}
			if t, ok := at[x.args[o]]; ok && t >= h && t <= l {
				return false // a jump in from outside
			}
		}
	}
	return true
}

// dance reports whether ins[k..k+4] is an append into the loop's own
// array (expr.go's appendReg): MOV b, r; MOV r, z; CALLB _, append_room
// or append_own, b, 2; MOV r, b; MOV b, z (z always none), straight
// (no label inside it), of elements of a value's kind; r, and that kind.
func (g *gen) dance(ins []insn, k int, none []bool, targets map[int]bool) (int, bc.Kind, bool) {
	name := func(j int) string { return bc.Ops[ins[j].op].Name }
	if name(k) != "MOV" || name(k+1) != "MOV" || name(k+2) != "CALLB" || name(k+3) != "MOV" || name(k+4) != "MOV" {
		return 0, 0, false
	}
	for j := k + 1; j <= k+4; j++ {
		if targets[ins[j].pc] {
			return 0, 0, false
		}
	}
	m0, m1, call, m3, m4 := ins[k].args, ins[k+1].args, ins[k+2].args, ins[k+3].args, ins[k+4].args
	b, r, z := m0[0], m0[1], m1[1]
	if b == r || !g.isRef(r) || !g.isRef(b) || z >= len(none) || !none[z] {
		return 0, 0, false
	}
	if m1[0] != r || m3[0] != r || m3[1] != b || m4[0] != b || m4[1] != z {
		return 0, 0, false
	}
	bn := g.m.Builtins[call[1]]
	if bn != "arr.append_room" && bn != "arr.append_own" || call[0] != bc.None || call[2] != b || call[3] != 2 {
		return 0, 0, false
	}
	kind := g.f.Kinds[b+1]
	if kind != bc.KInt && kind != bc.KFloat && kind != bc.KRef || b+1 == r {
		return 0, 0, false
	}
	return r, kind, true
}

// onlyDances reports whether every instruction of ins[h..l] that names
// r is one of the appends' moves (at dances).
func (g *gen) onlyDances(ins []insn, h, l, r int, dances []int) bool {
	ok := map[int]bool{}
	for _, k := range dances {
		ok[k], ok[k+1], ok[k+3] = true, true, true
	}
	for k := h; k <= l; k++ {
		if ok[k] {
			continue
		}
		if has(regs(g.m, ins[k]), r) || has(writes(g.m, ins[k]), r) {
			return false
		}
	}
	return true
}

// curType is the C type a cursor of elements of kind k points to: each
// a value's size (8 bytes), as the array's slots are, a reference's too
// (ak_ref_slot, where a pointer is 4 bytes), so the cursor steps a slot
// at a time and AK_CUR_CLOSE counts slots.
func curType(k bc.Kind) string {
	switch k {
	case bc.KFloat:
		return "double"
	case bc.KRef:
		return "ak_ref_slot"
	}
	return "int64_t"
}

// curNext is the C lvalue of the slot cursor c points to, stepping it
// past: a reference's pointer in its slot.
func curNext(c *cursor) string {
	if c.kind == bc.KRef {
		return fmt.Sprintf("(W%d_++)->p", c.id)
	}
	return fmt.Sprintf("*W%d_++", c.id)
}

// curDecls declares the cursors' locals.
func (g *gen) curDecls() {
	for _, c := range g.curs {
		t := curType(c.kind)
		g.p("\t%s*W%d_ = NULL, *E%d_ = NULL; (void)E%d_;\n", strings.TrimSuffix(t, " ")+" ", c.id, c.id, c.id)
	}
}

// curOpen is C opening the cursors of the loop whose first instruction
// is k (as it's fallen into).
func (g *gen) curOpen(k int) {
	for _, c := range g.curs {
		if c.h == k {
			g.w("AK_CUR_OPEN(%s, W%d_, E%d_);", g.rp(c.r), c.id, c.id)
		}
	}
}

// curClose is C writing back the counts of the cursors cs.
func (g *gen) curClose(cs []*cursor) string {
	var s strings.Builder
	for _, c := range cs {
		fmt.Fprintf(&s, "AK_CUR_CLOSE(%s, W%d_); ", g.rp(c.r), c.id)
	}
	return s.String()
}

// curLast is C closing the cursors of the loops whose last instruction
// is k, as the code falls out of it.
func (g *gen) curLast(k int, in insn) {
	if bc.Ops[in.op].Name == "JMP" {
		return
	}
	var cs []*cursor
	for _, c := range g.curs {
		if c.l == k {
			cs = append(cs, c)
		}
	}
	if len(cs) > 0 {
		g.w("%s", strings.TrimSpace(g.curClose(cs)))
	}
}

// leaving are the cursors open at the instruction being written that a
// way to index t (-1: out of the function) leaves.
func (g *gen) leaving(t int) []*cursor {
	var out []*cursor
	for _, c := range g.curs {
		if g.k >= c.h && g.k <= c.l && (t < c.h || t > c.l) {
			out = append(out, c)
		}
	}
	return out
}

// stubTo is a label whose code closes cs and then does jump: one per
// pair, written after the function's code.
func (g *gen) stubTo(cs []*cursor, jump string) string {
	key := g.curClose(cs) + jump
	for _, s := range g.stubs {
		if g.curClose(s.flush)+s.to == key {
			return s.name
		}
	}
	s := stub{name: fmt.Sprintf("X%d_", len(g.stubs)), flush: cs, to: jump}
	g.stubs = append(g.stubs, s)
	return s.name
}

// curAppend writes an append into a cursor's array (a dance's CALLB):
// the element stored where the cursor is, a filter's when there's room,
// else the count written back, the array grown and the cursor opened
// again.
func (g *gen) curAppend(c *cursor, name string, a []int) {
	arr := g.rp(a[2])
	v := g.r(a[2]+1) + "." + member(c.kind)
	retain := ""
	if c.kind == bc.KRef {
		retain = fmt.Sprintf("ak_retain(%s); ", v)
	}
	if name == "arr.append_room" {
		g.w("{ %sAK_CUR_ROOM(W%d_, E%d_); %s = %s; }", retain, c.id, c.id, curNext(c), v)
		return
	}
	g.w("{ %sif (AK_LIKELY(W%d_ != E%d_)) %s = %s; else { AK_CUR_CLOSE(%s, W%d_); %s = ak_op_append_slow(%s, (ak_val){.%s = %s}); AK_CUR_OPEN(%s, W%d_, E%d_); } }",
		retain, c.id, c.id, curNext(c), v, arr, c.id, arr, arr, member(c.kind), v, arr, c.id, c.id)
}

// stubsC writes the stubs' code.
func (g *gen) stubsC() {
	for _, s := range g.stubs {
		g.out.WriteString(fmt.Sprintf("%s:;\n", s.name))
		g.w("%s%s", g.curClose(s.flush), s.to)
	}
}
