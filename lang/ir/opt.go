package ir

import (
	"math/bits"
	"os"
	"strings"

	bc "github.com/egoist/asuka/lang/bytecode"
)

// The optimizer: passes over a lowered function's code that keep what
// it does (its results, its errors and their messages and lines, when
// references are let go of) and make it cheaper to run, on the VM and as
// the C lang/cgen writes from it. LOWERING.md's "Optimization" says what
// each pass does and why it's safe; opt_test.go checks each.
//
// The passes work on the code as the lowering left it: instructions
// over registers of one kind each, with labels. They don't make
// registers or functions, so a module's tables and what reloads keep
// (state's keys, functions' indexes) are the same either way. Deleted
// instructions become NOPs as a pass runs; compact drops them and moves
// the labels on.
//
// A reference register owns its value, and writing it lets the old one
// go: an instruction that writes one is never deleted or moved unless
// the register is known to hold none there, so no value is kept longer
// than the lowering keeps it (one kept would make a change in place copy
// what it changes, dropTemps' concern).

// Optimize runs the passes over every function of the program, inlines
// what it may (opt_inline.go), and compacts each function's registers.
func (p *Program) Optimize() {
	for _, f := range p.Funcs {
		f.Locals = nil // the debugger's: the passes move values between registers
		o := &optimizer{p: p, f: f, n: len(f.Kinds)}
		o.passes()
	}
	if !strings.Contains(os.Getenv("ASUKA_OPT_SKIP"), "inline") {
		p.inlineAll()
	}
	for _, f := range p.Funcs {
		o := &optimizer{p: p, f: f, n: len(f.Kinds)}
		o.finish()
	}
}

// An opInfo is what an opcode does with its operands, for the passes.
type opInfo struct {
	def   int8 // the operand it writes (a fresh value; the old one let go), -1: none
	uses  []int8
	inout []int8 // operands read and written in place (SETF's struct)
	// maydef is an operand an instruction writes only sometimes (MGETJ's
	// result: not when it jumps): a write that leaves the old value live.
	maydef int8
	// block says the instruction reads a block of registers: blockKind
	// says how many (blockN: an operand's count; the others, a shape's).
	block     int8
	blockKind uint8
	// blockWrites: a call, which may write its argument block (a mut fn's
	// receiver, an inout builtin's argument goes back there).
	blockWrites bool
	jump        int8 // the Off operand, -1: none
	term        bool // never goes on to the next instruction
	throws      bool // may throw an error (an in-function handler's edge)
	pure        bool // nothing but its result: deletable when that's dead
	// alias: its result may be one of its operands (it reads them all
	// before it writes), so a copy of its result may be folded into it.
	alias bool
	// unknown: an opcode the table doesn't describe (one added since):
	// it reads, and may write, every register, and may throw, so no pass
	// moves or deletes anything across it on its account.
	unknown bool
}

const (
	blockNone  = iota
	blockN     // Args[block+1] registers... see opBlock
	blockShape // a shape's fields (NEWS)
	blockCase  // a shape's case's fields (NEWE)
	blockPairs // 2n (NEWM)
	blockRest  // every register from it on: its count is in a path (CPATH)
)

var opInfos = func() []opInfo {
	t := make([]opInfo, len(bc.Ops))
	for i := range t {
		t[i] = opInfo{def: -1, maydef: -1, block: -1, jump: -1}
		for j, o := range bc.Ops[i].Operands {
			if o == bc.Off {
				t[i].jump = int8(j)
			}
		}
	}
	described := make([]bool, len(t))
	set := func(names string, f func(*opInfo)) {
		for _, n := range splitNames(names) {
			f(&t[op(n)])
			described[op(n)] = true
		}
	}
	d0 := func(o *opInfo) { o.def = 0 }
	u := func(js ...int8) func(*opInfo) { return func(o *opInfo) { o.uses = append(o.uses, js...) } }
	// Results computed from operands, then stored.
	set("MOV MOVR CLR KINT KSMALL KFLOAT KSTR KNONE GGET GTAKE CATCH CAP CTAKE", d0)
	set("ADDI SUBI MULI DIVI MODI WADDI WSUBI WMULI EQI NEI LTI LEI BAND BOR BXOR SHL SHR ADDF SUBF MULF DIVF MODF EQF NEF LTF LEF EQ NE CONCAT LTS IDX MGET", func(o *opInfo) { o.def = 0; o.uses = []int8{1, 2} })
	set("NEGI BNOT NEGF I2F F2I NOT ISNONE TOSTR ADDIK ISSHAPE VBOX VGET VTAKE GETF ECASE GETP BOX UNBOX LEN CELL CELLK FOLLOWK CGET CPEEK MEMO MREAD ACCV ACCF ACCC AGET TASK AWAIT ADDIS SUBIS MULIS DIVIS MODIS DIVIK MODIK", func(o *opInfo) { o.def = 0; o.uses = []int8{1} })
	set("MOV MOVR", u(1))
	set("IDXF", func(o *opInfo) { o.def = 0; o.uses = []int8{1, 2} })
	set("GSET", u(1))
	set("JT JF JNONE JSOME RET EFFECT RENDER CLEANUP", u(0))
	set("JLTI JLEI JEQI JNEI", u(0, 1))
	set("JLTIS JLEIS JGTIS JGEIS JEQIS JNEIS", u(0))
	set("VSET CSET", u(0, 1))
	set("CPUT", func(o *opInfo) { o.uses = []int8{0}; o.inout = []int8{1} })
	set("THROW RETHROW", func(o *opInfo) { o.inout = []int8{0} })
	set("SETF", func(o *opInfo) { o.inout = []int8{0}; o.uses = []int8{2} })
	set("SETIDX MSET", func(o *opInfo) { o.inout = []int8{0}; o.uses = []int8{1, 2} })
	set("SETIF", func(o *opInfo) { o.inout = []int8{0}; o.uses = []int8{1, 3} })
	set("MGETJ", func(o *opInfo) { o.maydef = 0; o.uses = []int8{1, 2} })
	set("INTERP", func(o *opInfo) { o.def = 0; o.block = 1; o.blockKind = blockN })
	set("CALL CALLB", func(o *opInfo) { o.def = 0; o.block = 2; o.blockKind = blockN; o.blockWrites = true })
	set("CALLC CALLU", func(o *opInfo) {
		o.def = 0
		o.uses = []int8{1}
		o.block = 2
		o.blockKind = blockN
		o.blockWrites = true
	})
	set("CALLI", func(o *opInfo) { o.def = 0; o.block = 2; o.blockKind = blockN; o.blockWrites = true })
	set("CLOS", func(o *opInfo) { o.def = 0; o.block = 2; o.blockKind = blockN })
	set("NEWS", func(o *opInfo) { o.def = 0; o.block = 2; o.blockKind = blockShape })
	set("NEWE", func(o *opInfo) { o.def = 0; o.block = 3; o.blockKind = blockCase })
	set("NEWA", func(o *opInfo) { o.def = 0; o.block = 3; o.blockKind = blockN })
	set("NEWM", func(o *opInfo) { o.def = 0; o.block = 3; o.blockKind = blockPairs })
	set("CPATH CLEN AGETP", func(o *opInfo) { o.def = 0; o.uses = []int8{1}; o.block = 3; o.blockKind = blockRest })
	set("ACCP", func(o *opInfo) { o.def = 0; o.uses = []int8{1}; o.block = 3; o.blockKind = blockRest })
	set("CPUTP", func(o *opInfo) { o.uses = []int8{0}; o.inout = []int8{1}; o.block = 3; o.blockKind = blockRest })

	set("JMP RET RET0 THROW RETHROW", func(o *opInfo) { o.term = true })
	set("ADDI SUBI MULI DIVI MODI NEGI SHL SHR IDX SETIDX CALL CALLC CALLB CALLI CALLU THROW "+
		"CGET CPEEK CSET CPUT CPATH CLEN CPUTP MREAD AGET AGETP EFFECT RENDER BATCH TASK AWAIT FOLLOWK "+
		"ADDIS SUBIS MULIS IDXF SETIF", func(o *opInfo) { o.throws = true })
	set("MOV MOVR CLR KNONE KINT KSMALL KFLOAT KSTR WADDI WSUBI WMULI ADDIK EQI NEI LTI LEI BAND BOR BXOR BNOT "+
		"ADDF SUBF MULF DIVF MODF NEGF EQF NEF LTF LEF I2F F2I NOT ISNONE LTS EQ NE LEN ECASE ISSHAPE "+
		"DIVIS MODIS DIVIK MODIK GETF GETP UNBOX CAP GGET VGET TOSTR CONCAT INTERP BOX NEWS NEWA NEWE NEWM CLOS", func(o *opInfo) { o.pure = true })
	// The interpreter's and the C's code for these reads every operand
	// before it stores the result.
	set("MOV ADDI SUBI MULI DIVI MODI NEGI WADDI WSUBI WMULI ADDIK EQI NEI LTI LEI BAND BOR BXOR BNOT SHL SHR "+
		"ADDF SUBF MULF DIVF MODF NEGF EQF NEF LTF LEF I2F F2I NOT ISNONE LTS EQ NE LEN ECASE "+
		"ADDIS SUBIS MULIS DIVIS MODIS DIVIK MODIK GETF IDX IDXF CONCAT", func(o *opInfo) { o.alias = true })
	// The sized integers' (i8 … u64) and powers: as the int's, their type
	// an immediate.
	set("ADDN SUBN MULN DIVN MODN POWN POWI SHLN SHRN LTU LEU POWF", func(o *opInfo) { o.def = 0; o.uses = []int8{1, 2} })
	set("NEGN WRAP CONVN CLAMPN F2N U2F", func(o *opInfo) { o.def = 0; o.uses = []int8{1} })
	set("ADDN SUBN MULN DIVN MODN NEGN POWN POWI SHLN SHRN CONVN", func(o *opInfo) { o.throws = true })
	set("LTU LEU WRAP CLAMPN F2N U2F POWF", func(o *opInfo) { o.pure = true })
	set("ADDN SUBN MULN DIVN MODN NEGN POWN POWI SHLN SHRN LTU LEU WRAP CONVN CLAMPN F2N U2F POWF", func(o *opInfo) { o.alias = true })
	set("NOP RET0 UNTRACK RETRACK BATCH YIELD CATCH", func(*opInfo) {})
	for i := range t {
		if !described[i] {
			t[i].unknown, t[i].throws = true, true
		}
	}
	return t
}()

func splitNames(s string) []string {
	var out []string
	start := -1
	for i := 0; i <= len(s); i++ {
		if i == len(s) || s[i] == ' ' {
			if start >= 0 {
				out = append(out, s[start:i])
			}
			start = -1
		} else if start < 0 {
			start = i
		}
	}
	return out
}

// ---------------------------------------------------------------------
// Registers an instruction reads and writes

// optimizer is the passes' state for one function.
type optimizer struct {
	p *Program
	f *Func
	n int // registers

	changed bool
	// blocks: the code's basic blocks, from cfg.
	blocks  []block
	blockOf []int // per instruction
	leader  []bool
	idx     []int // compact's
	// handlerOf: per instruction, the instruction its innermost handler
	// starts at (-1: none): where an error it throws goes.
	handlerOf []int
	// konsts: per instruction, its known operands (constants, for
	// selectOps).
	konsts [][2]*konst
}

type block struct {
	start, end int // [start, end) instructions
	// succ are the blocks it goes on to (at most two: a jump's and the
	// next), in succs.
	succs [2]int
	nsucc int
	// exc: blocks a throw inside it goes to (handlers).
	exc []int
}

func (b *block) succ() []int { return b.succs[:b.nsucc] }

// blockRange is the block of registers in reads: [start, start+n).
func (o *optimizer) blockRange(in *Instr) (start, n int) {
	info := &opInfos[in.Op]
	if info.block < 0 {
		return 0, 0
	}
	start = int(in.Args[info.block])
	switch info.blockKind {
	case blockN:
		n = int(in.Args[info.block+1])
	case blockShape:
		if s := int(in.Args[1]); s >= 0 && s < len(o.p.Mod.Shapes) {
			n = len(o.p.Mod.Shapes[s].Fields)
		}
	case blockCase:
		if s, c := int(in.Args[1]), int(in.Args[2]); s >= 0 && s < len(o.p.Mod.Shapes) && c >= 0 && c < len(o.p.Mod.Shapes[s].Cases) {
			n = len(o.p.Mod.Shapes[s].Cases[c].Fields)
		}
	case blockPairs:
		n = 2 * int(in.Args[info.block+1])
	case blockRest:
		n = o.n - start
	}
	if start < 0 || start >= o.n || n <= 0 {
		return 0, 0
	}
	if start+n > o.n {
		n = o.n - start
	}
	return start, n
}

// reads calls fn with each register in reads (its block's included).
func (o *optimizer) reads(in *Instr, fn func(r int)) {
	info := &opInfos[in.Op]
	if info.unknown {
		for r := 0; r < o.n; r++ {
			fn(r)
		}
		return
	}
	for _, j := range info.uses {
		if r := int(in.Args[j]); r != bc.None {
			fn(r)
		}
	}
	for _, j := range info.inout {
		fn(int(in.Args[j]))
	}
	if s, n := o.blockRange(in); n > 0 {
		for r := s; r < s+n; r++ {
			fn(r)
		}
	}
}

// reads1 reports whether in reads r.
func (o *optimizer) readsReg(in *Instr, r int) bool {
	found := false
	o.reads(in, func(x int) {
		if x == r {
			found = true
		}
	})
	return found
}

// kill is the register in writes, a fresh value in it (-1: none).
func kill(in *Instr) int {
	info := &opInfos[in.Op]
	if info.def < 0 {
		return -1
	}
	r := int(in.Args[info.def])
	if r == bc.None {
		return -1
	}
	return r
}

// writes calls fn with each register in may change: its result, what it
// changes in place, and a call's argument block.
func (o *optimizer) writes(in *Instr, fn func(r int)) {
	info := &opInfos[in.Op]
	if info.unknown {
		for r := 0; r < o.n; r++ {
			fn(r)
		}
		return
	}
	if r := kill(in); r >= 0 {
		fn(r)
	}
	if info.maydef >= 0 {
		fn(int(in.Args[info.maydef]))
	}
	for _, j := range info.inout {
		fn(int(in.Args[j]))
	}
	if info.blockWrites && !o.p.notMut(in) {
		// (A call of a function that isn't a mut fn writes none of its
		// arguments.)
		if s, n := o.blockRange(in); n > 0 {
			for r := s; r < s+n; r++ {
				fn(r)
			}
		}
	}
}

func (o *optimizer) writesReg(in *Instr, r int) bool {
	found := false
	o.writes(in, func(x int) {
		if x == r {
			found = true
		}
	})
	return found
}

// notMut reports whether in is a call that's known not to call a mut fn
// (whose receiver goes back into its argument's register): a CALL of
// another, a CALLI of a method no type's mut fn is named.
func (p *Program) notMut(in *Instr) bool {
	switch in.Op {
	case opCALL:
		return p.Funcs[in.Args[1]].Flags&bc.FMut == 0
	case opCALLI:
		if p.mutNames == nil {
			p.mutNames = map[string]bool{}
			for _, s := range p.Mod.Shapes {
				for name, fi := range s.Methods {
					if int(fi) < len(p.Funcs) && p.Funcs[fi].Flags&bc.FMut != 0 {
						p.mutNames[name] = true
					}
				}
			}
		}
		k := int(in.Args[1])
		return k < len(p.Mod.Strings) && !p.mutNames[p.Mod.Strings[k]]
	}
	return false
}

// ---------------------------------------------------------------------
// The control flow graph

func (o *optimizer) nop(i int) {
	o.f.Code[i] = Instr{Op: opNOP, Line: o.f.Code[i].Line}
	o.changed = true
}

var (
	opNOP     = op("NOP")
	opMOV     = op("MOV")
	opMOVR    = op("MOVR")
	opJMP     = op("JMP")
	opCLR     = op("CLR")
	opKNONE   = op("KNONE")
	opKSMALL  = op("KSMALL")
	opKINT    = op("KINT")
	opKFLOAT  = op("KFLOAT")
	opCATCH   = op("CATCH")
	opRET     = op("RET")
	opRET0    = op("RET0")
	opCALL    = op("CALL")
	opCALLI   = op("CALLI")
	opRETHROW = op("RETHROW")
)

// target is the instruction a jump goes to.
func (o *optimizer) target(in *Instr) int {
	return o.f.labels[in.Args[opInfos[in.Op].jump]]
}

func (o *optimizer) handlerRange(h Handler) (int, int, int) {
	return o.f.labels[h.Start], o.f.labels[h.End], o.f.labels[h.Target]
}

// cfg finds the basic blocks, their edges, and each instruction's
// handler.
func (o *optimizer) cfg() {
	code := o.f.Code
	n := len(code)
	if cap(o.handlerOf) < n {
		o.handlerOf = make([]int, n)
	}
	o.handlerOf = o.handlerOf[:n]
	for i := range o.handlerOf {
		o.handlerOf[i] = -1
	}
	// Innermost first: the first handler covering an instruction is its.
	for _, h := range o.f.Handlers {
		s, e, t := o.handlerRange(h)
		for i := s; i < e && i < n; i++ {
			if o.handlerOf[i] < 0 {
				o.handlerOf[i] = t
			}
		}
	}
	if cap(o.leader) < n+1 {
		o.leader = make([]bool, n+1)
	}
	leader := o.leader[:n+1]
	for i := range leader {
		leader[i] = false
	}
	leader[0] = true
	// Blocks start where a jump or a handler goes, and after a jump.
	for _, h := range o.f.Handlers {
		leader[o.f.labels[h.Target]] = true
	}
	for i := range code {
		info := &opInfos[code[i].Op]
		if info.jump >= 0 {
			leader[o.target(&code[i])] = true
		}
		if info.jump >= 0 || info.term {
			leader[i+1] = true
		}
	}
	o.blocks = o.blocks[:0]
	if cap(o.blockOf) < n+1 {
		o.blockOf = make([]int, n+1)
	}
	o.blockOf = o.blockOf[:n+1]
	for i := 0; i < n; {
		j := i + 1
		for j < n && !leader[j] {
			j++
		}
		for k := i; k < j; k++ {
			o.blockOf[k] = len(o.blocks)
		}
		o.blocks = append(o.blocks, block{start: i, end: j})
		i = j
	}
	o.blockOf[n] = len(o.blocks) // the end: no block
	for b := range o.blocks {
		bl := &o.blocks[b]
		last := &code[bl.end-1]
		info := &opInfos[last.Op]
		if info.jump >= 0 {
			if t := o.target(last); t < n {
				bl.succs[bl.nsucc] = o.blockOf[t]
				bl.nsucc++
			}
		}
		if !info.term && bl.end < n {
			bl.succs[bl.nsucc] = o.blockOf[bl.end]
			bl.nsucc++
		}
		for k := bl.start; k < bl.end; k++ {
			if h := o.handlerOf[k]; h >= 0 && h < n && o.throwsHere(&code[k]) {
				hb := o.blockOf[h]
				dup := false
				for _, x := range bl.exc {
					dup = dup || x == hb
				}
				if !dup {
					bl.exc = append(bl.exc, hb)
				}
			}
		}
	}
}

// throwsHere reports whether an error in may throw goes to a handler of
// this function (RETHROW's never does).
func (o *optimizer) throwsHere(in *Instr) bool {
	return opInfos[in.Op].throws
}

// ---------------------------------------------------------------------
// Register sets

type regset []uint64

func newRegset(n int) regset { return make(regset, (n+63)/64) }

func (s regset) has(r int) bool { return s[r>>6]&(1<<(r&63)) != 0 }
func (s regset) add(r int)      { s[r>>6] |= 1 << (r & 63) }
func (s regset) del(r int)      { s[r>>6] &^= 1 << (r & 63) }
func (s regset) copyFrom(t regset) {
	copy(s, t)
}

// union adds t to s, reporting whether s grew.
func (s regset) union(t regset) bool {
	grew := false
	for i := range s {
		if v := s[i] | t[i]; v != s[i] {
			s[i] = v
			grew = true
		}
	}
	return grew
}

// intersect keeps in s what t has too, reporting whether s shrank.
func (s regset) intersect(t regset) bool {
	shrank := false
	for i := range s {
		if v := s[i] & t[i]; v != s[i] {
			s[i] = v
			shrank = true
		}
	}
	return shrank
}

func (s regset) count() int {
	c := 0
	for _, w := range s {
		c += bits.OnesCount64(w)
	}
	return c
}

// ---------------------------------------------------------------------
// Liveness

// liveness is each block's live-in set: the registers read before
// they're written on some path from its start. An error thrown in a
// handler's range goes to the handler with the registers as they were
// before the instruction: what's live there is live before it.
func (o *optimizer) liveness() []regset {
	in := make([]regset, len(o.blocks))
	w := (o.n + 63) / 64
	slab := make([]uint64, len(o.blocks)*w)
	for b := range in {
		in[b] = slab[b*w : (b+1)*w : (b+1)*w]
	}
	live := newRegset(o.n)
	for changed := true; changed; {
		changed = false
		for b := len(o.blocks) - 1; b >= 0; b-- {
			o.liveOut(in, b, live)
			o.walkBack(in, b, live, nil)
			if in[b].union(live) {
				changed = true
			}
		}
	}
	return in
}

// liveOut sets live to what's live at the end of block b.
func (o *optimizer) liveOut(in []regset, b int, live regset) {
	for i := range live {
		live[i] = 0
	}
	for _, s := range o.blocks[b].succ() {
		live.union(in[s])
	}
	if bl := &o.blocks[b]; bl.nsucc == 0 || opInfos[o.f.Code[bl.end-1].Op].term && opInfos[o.f.Code[bl.end-1].Op].jump < 0 {
		o.exitLive(live)
	}
}

// exitLive adds what's read as the function ends (returned or thrown):
// a mut fn's receiver, which goes back to the caller's register.
func (o *optimizer) exitLive(live regset) {
	if o.f.Flags&bc.FMut != 0 && o.f.NParams > 0 {
		live.add(0)
	}
}

// throwLive adds to live, before instruction i (which may throw), what's
// live where its error goes: its handler, or out of the function.
func (o *optimizer) throwLive(i int, in []regset, live regset) {
	c := &o.f.Code[i]
	if !o.throwsHere(c) && c.Op != opRETHROW {
		return
	}
	if h := o.handlerOf[i]; h >= 0 && h < len(o.f.Code) && c.Op != opRETHROW {
		live.union(in[o.blockOf[h]])
	} else {
		o.exitLive(live)
	}
}

// walkBack moves live from the end of block b to its start, calling
// visit (when not nil) with each instruction and what's live after it
// first.
func (o *optimizer) walkBack(in []regset, b int, live regset, visit func(i int, after regset)) {
	bl := &o.blocks[b]
	code := o.f.Code
	for i := bl.end - 1; i >= bl.start; i-- {
		c := &code[i]
		if visit != nil {
			visit(i, live)
		}
		if r := kill(c); r >= 0 {
			live.del(r)
		}
		o.reads(c, live.add)
		o.throwLive(i, in, live)
	}
}

// ---------------------------------------------------------------------
// The driver

// passes runs the passes until they change nothing (or a few rounds).
func (o *optimizer) passes() {
	f := o.f
	o.n = len(f.Kinds)
	if len(f.Code) == 0 {
		return
	}
	skip := os.Getenv("ASUKA_OPT_SKIP")
	pass := func(name string, fn func()) {
		if !strings.Contains(skip, name) {
			o.cfg()
			fn()
		}
	}
	if !strings.Contains(skip, "release") {
		o.releaseTemps()
	}
	f.tempSpans = nil
	for round := 0; round < 3; round++ {
		o.changed = false
		o.dropDeadHandlers()
		o.cfg()
		o.unreachable()
		pass("jumps", o.threadJumps)
		o.compact()
		pass("copies", o.copyProp)
		pass("constants", o.constants)
		pass("select", o.selectOps)
		pass("cse", o.cse)
		pass("coalesce", o.coalesce)
		o.compact()
		pass("elem", o.elemFields)
		o.compact()
		pass("sroa", o.scalarStructs)
		pass("dead", o.deadCode)
		o.compact()
		pass("pairs", o.copyPairs)
		o.compact()
		if !o.changed {
			break
		}
	}
}

// finish runs what's done once, last: constants out of loops, and the
// registers compacted.
func (o *optimizer) finish() {
	if len(o.f.Code) == 0 {
		return
	}
	o.cfg()
	o.hoistConstants()
	o.compact()
	o.compactRegs()
}

// compact deletes the NOPs, moving each label to the instruction that
// followed it, and drops handlers whose ranges came to nothing.
func (o *optimizer) compact() {
	f := o.f
	if cap(o.idx) < len(f.Code)+1 {
		o.idx = make([]int, len(f.Code)+1)
	}
	newIdx := o.idx[:len(f.Code)+1]
	out := f.Code[:0]
	for i := range f.Code {
		newIdx[i] = len(out)
		if f.Code[i].Op != opNOP {
			out = append(out, f.Code[i])
		}
	}
	newIdx[len(f.Code)] = len(out)
	f.Code = out
	for l, i := range f.labels {
		if i >= 0 {
			f.labels[l] = newIdx[i]
		}
	}
	hs := f.Handlers[:0]
	for _, h := range f.Handlers {
		if f.labels[h.Start] < f.labels[h.End] {
			hs = append(hs, h)
		}
	}
	f.Handlers = hs
}
