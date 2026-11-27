package ir

import (
	"slices"
	"strings"

	bc "github.com/egoist/asuka/lang/bytecode"
	"github.com/egoist/asuka/lang/check"
	"github.com/egoist/asuka/lang/diag"
	"github.com/egoist/asuka/lang/syntax"
)

// fb builds a function: its registers, its code, and where each name it
// uses lives.
type fb struct {
	// pathVars: a collection method's closure's parameter, inlined in a
	// loop over a path (pathloops.go): the element's path.
	pathVars map[*check.Var]*pathVar

	l     *Lowerer
	f     *Func
	idx   int
	ctx   *check.FuncCtx
	subst check.Subst
	line  int32
	node  syntax.Node // what's being lowered (at's), for an error found as it's emitted
	// pinLine: while above 0, at keeps the line: a default's code,
	// lowered into the call that leaves its argument out, is at the
	// call's line (its own is its declaration's, maybe another file's).
	pinLine int
	// tooBig: the function was said to need more than the bytecode holds
	// (registers, an operand), once.
	tooBig bool

	// bench: a bench's function, whose register benchN holds how many
	// times to run what it measures (testing.go).
	bench  bool
	benchN int

	// inout: the place an inout builtin's call changes, when it's x?.f's
	// (places.go), and what's done with its result there.
	inout      *lvalue
	inoutAfter func(r int)
	// holdCaps: where the closure being made puts its captures' block
	// rather than let it go at once: a fused loop's (seqfuse.go), which
	// lets it go after the loop, so that the closure's calls in it can be
	// inlined.
	holdCaps *[]heldBlock
	// loadKey: the function a load's thunk being made calls, whose key
	// and arguments taskOf records (load.key) for reload (rfcs/0017).
	loadKey *check.Function

	vars map[*check.Var]int // a local's register (its box, accessor or memo)
	// openLocals: the debugger's names (Func.Locals) still in their
	// registers, by register; localOf: a name's, by its var.
	openLocals map[int][]int
	localOf    map[*check.Var]int
	// unboxed: boxed vars this function holds the values of (a spawn's
	// copies), in their registers.
	unboxed map[*check.Var]bool
	// locals: the local fns in scope, their functions' indexes (shared
	// with the closures and local fns made inside).
	locals map[*check.LocalFn]int

	// Registers: per kind, those free to reuse; temps die at the end of
	// the statement that made them; locals at the end of their block.
	free   map[bc.Kind][]int
	temps  []int
	tempAt map[int]int // a reference temp's register: where in the code it was made a temp
	// localAt: a local's register whose value is data alone (plainData):
	// where its name was declared, for the span closeScope records.
	localAt   map[int]int
	scopes    [][]int
	blockFree map[string][]int // argument blocks, by their kinds
	nullReg   int              // a reference register that's always none

	hoisting bool // making a hoisted literal: don\'t hoist again
	// peeks: the peeks open where code is being emitted; untracks: the
	// UNTRACKs emitted so far, which tell a handler's target whether its
	// range began any (it then says how many are still open, RETRACK).
	peeks, untracks int
	loops           []loop
	defers          []*syntax.Block // the defers in effect, innermost last
	inDefer         int

	// The component's or store's env: envReg holds the struct (in
	// methods, closures, and the component's function once it's made);
	// envBox the box its closures made before that capture.
	comp   *check.TypeName
	envReg int
	envBox int
	// ownerReg: in the function of a component that holds handlers
	// (check's Component.HoldsHandlers), the owner it was made under
	// (ui.owner), which its env keeps after its members.
	ownerReg   int
	memberRegs map[*check.Member]int // in the component's function

	mainWindow *check.ElementCall // the app's: the app block's first Window
}

type loop struct {
	brk, cont Label
	defers    int         // len(defers) where the loop starts
	node      syntax.Node // the loop's statement, which a break name names
}

func (b *fb) emit(o bc.Op, args ...int)     { b.f.Emit(b.line, o, b.fits(o, args)...) }
func (b *fb) emitKind(o bc.Op, args ...int) { b.f.Emit(b.line, o, b.fits(o, args)...) }

// immWhat names what an instruction's 16-bit immediate counts or
// indexes, for the error when there are more.
var immWhat = map[string]string{
	"CALL": "arguments", "CALLC": "arguments", "CALLB": "arguments", "CALLI": "arguments", "CALLU": "arguments",
	"CLOS": "captured values", "CAP": "captured values", "INTERP": "parts of a string",
	"GETF": "fields", "SETF": "fields", "GETP": "payload values", "NEWE": "enum cases",
	"NEWA": "elements", "NEWM": "entries",
}

// fits checks args' immediates: one past 16 bits (a call of 70000
// arguments, the 70000th case of an enum with payloads) is an error
// where it's lowered, not a module the assembler can't write.
func (b *fb) fits(o bc.Op, args []int) []int {
	def := bc.Ops[o]
	for i, kind := range def.Operands {
		if kind != bc.Imm || i >= len(args) || args[i] >= -0x8000 && args[i] <= 0xFFFF {
			continue
		}
		what := immWhat[def.Name]
		if what == "" {
			what = "of what " + def.Name + " counts"
		}
		b.overflow("this needs more than 65535 %s in one instruction, the most the bytecode holds", what)
		args[i] = 0
	}
	return args
}

// overflow reports, once per function, that it needs more than the
// bytecode holds, where it's being lowered.
func (b *fb) overflow(format string, args ...any) {
	if b.tooBig {
		return
	}
	b.tooBig = true
	where := b.node
	if where == nil {
		where = b.l.lastNode
	}
	b.l.errorf(where, diag.TooLarge, format, args...)
}

// emitJump emits a jump to l: JMP, or a conditional one on reg.
func (b *fb) emitJump(o bc.Op, reg int, l Label) {
	if o == op("JMP") {
		b.f.Emit(b.line, o, int(l))
		return
	}
	b.f.Emit(b.line, o, reg, int(l))
}

func (b *fb) jmp(l Label) { b.f.Emit(b.line, op("JMP"), int(l)) }

func (b *fb) at(n syntax.Node) {
	if n != nil {
		b.node, b.l.lastNode = n, n
		if b.pinLine > 0 {
			return
		}
		// A line is the function's file's: code from another file in
		// it (a default's) keeps the line it's at, or a breakpoint on
		// that line of this file would stop there.
		if f := b.l.fset.File(n.Range().Start); f != nil && (b.f.File == "" || f.Name() == b.f.File) {
			if line := f.Position(n.Range().Start).Line; line > 0 {
				b.line = int32(line)
			}
		}
	}
}

// defaultValue lowers a default (a parameter's, a field's, a prop's) for
// a call that leaves its argument out: at the call's line.
func (b *fb) defaultValue(e syntax.Expr, dst int) int {
	b.pinLine++
	defer func() { b.pinLine-- }()
	return b.value(e, dst)
}

// ---------------------------------------------------------------------
// Registers

func (b *fb) newReg(k bc.Kind) int {
	if len(b.f.Kinds) >= bc.None-1 {
		// An instruction names a register in 16 bits (bc.None is none).
		b.overflow("this needs more than %d registers (values held at once) in one function, the most the bytecode holds: split it up", bc.None-1)
	}
	return b.f.Reg(k)
}

// temp is a register that lives until the statement's end.
func (b *fb) temp(k bc.Kind) int {
	if k == bc.KVoid {
		return bc.None
	}
	var r int
	if fr := b.free[k]; len(fr) > 0 {
		r = fr[len(fr)-1]
		b.free[k] = fr[:len(fr)-1]
	} else {
		r = b.newReg(k)
	}
	b.temps = append(b.temps, r)
	if k == bc.KRef {
		if b.tempAt == nil {
			b.tempAt = map[int]int{}
		}
		b.tempAt[r] = len(b.f.Code)
	}
	return r
}

func (b *fb) mark() int { return len(b.temps) }

// release frees the temps made since mark: a reference temp's span of
// the code (where it was a temp, nothing named) is recorded for the
// optimizer, which lets its value go after its last read (opt_release.go).
func (b *fb) release(mark int) {
	for _, r := range b.temps[mark:] {
		b.endLocal(r)
		k := b.f.Kinds[r]
		b.free[k] = append(b.free[k], r)
		if k == bc.KRef {
			if at, ok := b.tempAt[r]; ok && at < len(b.f.Code) {
				b.f.tempSpans = append(b.f.tempSpans, tempSpan{reg: r, start: at, end: len(b.f.Code)})
			}
			delete(b.tempAt, r)
		}
	}
	b.temps = b.temps[:mark]
}

// dropTemps clears the reference temps made since mark, but keep's:
// what computing a change's value or arguments read on its way (a
// struct's field, for xs[i] = xs[j] in a mut fn). Held to the
// statement's end, each would be another reference to the place about
// to change, which would then be copied, not changed in place.
func (b *fb) dropTemps(mark int, keep ...int) {
	for _, r := range b.temps[mark:] {
		if b.f.Kinds[r] == bc.KRef && !slices.Contains(keep, r) {
			b.emit(op("CLR"), r)
		}
	}
}

// local is a register that lives until its block ends.
func (b *fb) local(k bc.Kind) int {
	var r int
	if fr := b.free[k]; len(fr) > 0 {
		r = fr[len(fr)-1]
		b.free[k] = fr[:len(fr)-1]
	} else {
		r = b.newReg(k)
	}
	if n := len(b.scopes); n > 0 {
		b.scopes[n-1] = append(b.scopes[n-1], r)
	}
	return r
}

func (b *fb) openScope() { b.scopes = append(b.scopes, nil) }

func (b *fb) closeScope() {
	n := len(b.scopes)
	for _, r := range b.scopes[n-1] {
		if at, ok := b.localAt[r]; ok {
			if at < len(b.f.Code) {
				b.f.tempSpans = append(b.f.tempSpans, tempSpan{reg: r, start: at, end: len(b.f.Code)})
			}
			delete(b.localAt, r)
		}
	}
	for _, r := range b.scopes[n-1] {
		b.endLocal(r)
		k := b.f.Kinds[r]
		if k == bc.KRef {
			// Let go of what it held: a value someone changes next
			// stays unique, and so in place.
			b.emit(op("CLR"), r)
		}
		b.free[k] = append(b.free[k], r)
	}
	b.scopes = b.scopes[:n-1]
}

func kindsKey(ks []bc.Kind) string {
	var s strings.Builder
	for _, k := range ks {
		s.WriteByte(byte('0' + k))
	}
	return s.String()
}

// regs is n consecutive registers of the kinds: a call's arguments, a
// struct's fields, a closure's captures.
func (b *fb) regs(ks []bc.Kind) int {
	if len(ks) == 0 {
		return 0
	}
	key := kindsKey(ks)
	if fr := b.blockFree[key]; len(fr) > 0 {
		r := fr[len(fr)-1]
		b.blockFree[key] = fr[:len(fr)-1]
		return r
	}
	base := len(b.f.Kinds)
	for _, k := range ks {
		b.newReg(k)
	}
	return base
}

// releaseRegs lets a block's references go and frees it.
func (b *fb) releaseRegs(base int, ks []bc.Kind) {
	if len(ks) == 0 {
		return
	}
	for i := range ks {
		b.endLocal(base + i)
	}
	for i, k := range ks {
		if k == bc.KRef {
			b.emit(op("CLR"), base+i)
		}
	}
	key := kindsKey(ks)
	b.blockFree[key] = append(b.blockFree[key], base)
}

// null is a reference register that's always none (for moves).
func (b *fb) null() int {
	if b.nullReg < 0 {
		b.nullReg = b.newReg(bc.KRef)
	}
	return b.nullReg
}

// move copies src into dst: retained when a reference.
func (b *fb) move(dst, src int) {
	if dst == src || src < 0 || src == bc.None || dst == bc.None {
		return
	}
	if b.f.Kinds[dst] == bc.KRef {
		b.emit(op("MOVR"), dst, src)
	} else {
		b.emit(op("MOV"), dst, src)
	}
}

// valueReg checks that v holds a value of kind k.
func (b *fb) valueReg(v int, k bc.Kind) int {
	if v < 0 || v == bc.None {
		r := b.temp(k)
		if k == bc.KRef {
			b.emit(op("KNONE"), r)
		} else if k != bc.KVoid {
			b.emit(op("KSMALL"), r, 0)
		}
		return r
	}
	return v
}

// konst is an int constant in a temp.
func (b *fb) konst(v int64) int {
	r := b.temp(bc.KInt)
	b.loadInt(r, v)
	return r
}

func (b *fb) loadInt(r int, v int64) {
	if v >= -32768 && v <= 32767 {
		b.emit(op("KSMALL"), r, int(uint16(int16(v))))
		return
	}
	b.emit(op("KINT"), r, b.l.intConst(v))
}

func (l *Lowerer) intConst(v int64) int {
	for i, x := range l.mod.Ints {
		if x == v {
			return i
		}
	}
	l.mod.Ints = append(l.mod.Ints, v)
	return len(l.mod.Ints) - 1
}

func (l *Lowerer) floatConst(v float64) int {
	for i, x := range l.mod.Floats {
		if x == v {
			return i
		}
	}
	l.mod.Floats = append(l.mod.Floats, v)
	return len(l.mod.Floats) - 1
}

func (b *fb) kstr(s string) int {
	r := b.temp(bc.KRef)
	b.emit(op("KSTR"), r, b.l.str(s))
	return r
}

// callB calls a builtin with args (registers, of their kinds) and
// returns its result in a temp of kind res (None for void).
func (b *fb) callB(name string, res bc.Kind, args ...int) int {
	dst := bc.None
	if res != bc.KVoid {
		dst = b.temp(res)
	}
	b.callBTo(name, dst, args...)
	return dst
}

func (b *fb) callBTo(name string, dst int, args ...int) {
	ks := make([]bc.Kind, len(args))
	for i, a := range args {
		ks[i] = b.f.Kinds[a]
	}
	blk := b.regs(ks)
	for i, a := range args {
		b.move(blk+i, a)
	}
	b.emit(op("CALLB"), dst, b.l.builtin(name), blk, len(args))
	b.releaseRegs(blk, ks)
}

// freeze makes a constant immortal (rt.freeze), for sharing.
func (b *fb) freeze(v int, k bc.Kind) int {
	return b.callB("rt.freeze", k, v, b.konst(int64(k)))
}

// typ is t with the function's type arguments.
func (b *fb) typ(t check.Type) check.Type {
	t = b.subst.Apply(t)
	if b.f != nil && b.f.Flags&bc.FSized == 0 && mentionsSized(t) {
		b.f.Flags |= bc.FSized
	}
	return t
}

// mentionsSized reports whether t is a sized integer type, or holds
// them where an array may pack them: an array's (or a Seq's) elements,
// through optionals and arrays (bc.FSized).
func mentionsSized(t check.Type) bool {
	for {
		switch x := t.(type) {
		case *check.Array:
			t = x.Elem
		case *check.Optional:
			t = x.Elem
		case *check.Seq:
			t = x.Elem
		default:
			return check.IsSized(t)
		}
	}
}

func (b *fb) kind(t check.Type) bc.Kind { return b.l.kind(b.typ(t)) }

// typeOf is e's type, as it's used (after its implicit conversion).
func (b *fb) typeOf(e syntax.Expr) check.Type {
	if t, ok := b.l.info.Convs[e]; ok {
		return b.typ(t)
	}
	return b.typ(b.l.info.Types[e])
}

// rawType is e's own type.
func (b *fb) rawType(e syntax.Expr) check.Type { return b.typ(b.l.info.Types[e]) }

// ---------------------------------------------------------------------
// Locals

// declare makes v's register, and initializes it with init (a register,
// or -1 for none).
func (b *fb) declare(v *check.Var, init int) int {
	if v == nil || v.Name() == "_" && init < 0 {
		return -1
	}
	k := b.kind(v.Type)
	if v.Kind == check.RowItem || v.Kind == check.ViewMemo {
		k = bc.KRef
	}
	if v.Boxed() {
		r := b.local(bc.KRef)
		val := b.valueReg(init, k)
		b.emit(op("VBOX"), r, val, int(k))
		b.setVar(v, r)
		return r
	}
	r := b.local(k)
	if k == bc.KRef && v.Kind == check.LocalVar && len(b.scopes) > 0 && plainData(b.typ(v.Type), nil) {
		// Data alone: its value may be let go of once nothing reads it
		// (opt_release.go), as no drop of a native object's is put off.
		if b.localAt == nil {
			b.localAt = map[int]int{}
		}
		b.localAt[r] = len(b.f.Code)
	}
	if init >= 0 {
		b.move(r, init)
	}
	b.setVar(v, r)
	return r
}

// setVar makes r v's register from here on.
func (b *fb) setVar(v *check.Var, r int) {
	b.vars[v] = r
	if b.l.opts.Locals {
		b.noteLocal(v, r)
	}
}

// noteLocal records v's name for the debugger (Func.Locals): in r from
// the next instruction on, until r is let go of (endLocal) or v is
// given another register.
func (b *fb) noteLocal(v *check.Var, r int) {
	name := v.Name()
	if r < 0 || r >= len(b.f.Kinds) || !debugName(name) {
		return
	}
	if i, ok := b.localOf[v]; ok && b.f.Locals[i].End < 0 {
		b.closeLocal(i)
	}
	via := uint8(bc.LocalValue)
	switch {
	case v.Boxed() && !b.unboxed[v]:
		via = bc.LocalBox
	case v.Kind == check.RowItem, v.Kind == check.ViewMemo, b.l.prog.AccessorVar(v), v.Kind == check.StyleLocal && b.l.styleMemo[v]:
		via = bc.LocalSignal
	}
	t := b.subst.Apply(v.Type)
	b.localOf[v] = b.addLocal(name, r, via, b.l.debugDesc(t))
	b.f.Locals[b.localOf[v]].Type = t
}

// addLocal records a name the debugger shows, in r from the next
// instruction on, and returns its index in Func.Locals.
func (b *fb) addLocal(name string, r int, via uint8, desc string) int {
	if b.openLocals == nil {
		b.openLocals, b.localOf = map[int][]int{}, map[*check.Var]int{}
	}
	b.f.Locals = append(b.f.Locals, Local{Name: name, Reg: r, Via: via, Desc: desc, Start: len(b.f.Code), End: -1})
	i := len(b.f.Locals) - 1
	b.openLocals[r] = append(b.openLocals[r], i)
	return i
}

// noteEnv records the component's or store's env as "self" for the
// debugger: a struct of its members, each shown as what it holds now.
func (b *fb) noteEnv() {
	if b.l.opts.Locals && b.envReg >= 0 {
		i := b.addLocal("self", b.envReg, bc.LocalValue, "-")
		b.f.Locals[i].Env = b.comp
	}
}

// noteMembers records the registers of a component's members in its own
// function, by their names, for the debugger.
func (b *fb) noteMembers(order []*check.Member) {
	if !b.l.opts.Locals {
		return
	}
	for _, m := range order {
		r, ok := b.memberRegs[m]
		if !ok || !debugName(m.Name()) {
			continue
		}
		// A state's cell, a let's memo, a parameter's accessor: what it
		// holds (the debugger shows any other value as it is).
		t := b.subst.Apply(m.Type)
		i := b.addLocal(m.Name(), r, bc.LocalSignal, b.l.debugDesc(t))
		b.f.Locals[i].Type = t
	}
}

// debugName reports whether a var's name is the source's (not one the
// checker or the lowering made up).
func debugName(n string) bool {
	if n == "" || n == "_" {
		return false
	}
	c := n[0]
	return c == '_' || c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' || c >= 0x80
}

// endLocal ends the names in register r: it's let go of here.
func (b *fb) endLocal(r int) {
	if len(b.openLocals[r]) == 0 {
		return
	}
	for _, i := range b.openLocals[r] {
		b.f.Locals[i].End = len(b.f.Code)
	}
	delete(b.openLocals, r)
}

// closeLocal ends the name Locals[i] here.
func (b *fb) closeLocal(i int) {
	b.f.Locals[i].End = len(b.f.Code)
	r := b.f.Locals[i].Reg
	is := b.openLocals[r]
	for j, k := range is {
		if k == i {
			b.openLocals[r] = append(is[:j:j], is[j+1:]...)
			break
		}
	}
}

// debugDesc is t's descriptor as the debugger writes its values
// (bytecode.Local's Desc): its JSON descriptor (desc), but a duration's
// "d" and a color's "c", and nothing made for it but a payloadless
// enum's shape, which names its cases.
func (l *Lowerer) debugDesc(t check.Type) string {
	switch x := t.(type) {
	case *check.Basic:
		switch x {
		case check.Duration:
			return "d"
		case check.Color:
			return "c"
		case check.Int, check.Length, check.Pointer:
			return "i"
		case check.Char:
			return "r"
		case check.I8, check.I16, check.I32, check.U8, check.U16, check.U32, check.U64:
			return "n" + string(rune('0'+check.IntCode(x)))
		case check.Float:
			return "f"
		case check.Bool:
			return "b"
		case check.String:
			return "s"
		case check.Bytes:
			return "y"
		}
		return "-"
	case *check.Optional:
		return "?" + l.debugDesc(x.Elem)
	case *check.Array:
		return "[" + l.debugDesc(x.Elem)
	case *check.Map:
		if x.Key != check.String {
			return "m" + l.debugDesc(x.Key) + l.debugDesc(x.Elem)
		}
		return "{" + l.debugDesc(x.Elem)
	case *check.Set:
		return "<" + l.debugDesc(x.Elem)
	case *check.Tuple:
		return "("
	case *check.Named:
		switch x.Obj.Kind {
		case check.StructType:
			return "S" + l.typeKey(t) + ";"
		case check.EnumType:
			if x.Obj.RefEnum() {
				return "E" + l.typeKey(t) + ";"
			}
			l.intEnumShape(x)
			return "e" + l.typeKey(t) + ";"
		}
	}
	return "-"
}

// plainData reports whether t's values are data alone, made of numbers,
// strings and bytes, in arrays, maps, sets, sorted maps, optionals,
// tuples, structs and enums: letting go of one does nothing but free
// memory. Not a closure, a task, a stream, a view, an interface's value
// (any type's), a native interface's or an extern type's (their drops
// close, stop, release what the platform holds), nor what holds one.
func plainData(t check.Type, seen map[*check.TypeName]bool) bool {
	switch t := t.(type) {
	case *check.Basic:
		switch t.Kind {
		case check.KInt, check.KFloat, check.KBool, check.KString, check.KBytes, check.KDuration, check.KColor,
			check.KLength, check.KChar, check.KI8, check.KI16, check.KI32, check.KU8, check.KU16, check.KU32, check.KU64:
			return true
		}
		return false
	case *check.Array:
		return plainData(t.Elem, seen)
	case *check.Set:
		return plainData(t.Elem, seen)
	case *check.Map:
		return plainData(t.Key, seen) && plainData(t.Elem, seen)
	case *check.Tree:
		return plainData(t.Key, seen) && plainData(t.Elem, seen)
	case *check.Optional:
		return plainData(t.Elem, seen)
	case *check.Tuple:
		for _, e := range t.Elems {
			if !plainData(e, seen) {
				return false
			}
		}
		return true
	case *check.Named:
		o := t.Obj
		if o.Native || o.Extern || o.Release != nil || o.Kind != check.StructType && o.Kind != check.EnumType {
			return false
		}
		if seen[o] {
			return true // a recursive type: what it holds is checked where it's first met
		}
		if seen == nil {
			seen = map[*check.TypeName]bool{}
		}
		seen[o] = true
		defer delete(seen, o)
		sub := t.Subst()
		for _, f := range o.Fields {
			if !plainData(sub.Apply(f.Type), seen) {
				return false
			}
		}
		for _, c := range o.Cases {
			for _, p := range c.Params {
				if !plainData(sub.Apply(p), seen) {
					return false
				}
			}
		}
		return true
	}
	return false
}

// readVar is v's value: its register (borrowed), or a temp.
func (b *fb) readVar(v *check.Var, n syntax.Node) int {
	if pv := b.pathVars[v]; pv != nil {
		// A loop's element (pathloops.go): read whole, by its path.
		return b.pathVarRead(pv, b.kind(v.Type), -1, true)
	}
	r, ok := b.vars[v]
	if !ok {
		b.l.errorf(n, diag.InternalError, "internal: %s has no register in %s", v.Name(), b.f.Name)
		return b.valueReg(-1, b.kind(v.Type))
	}
	switch {
	case v.Boxed() && !b.unboxed[v]:
		t := b.temp(b.kind(v.Type))
		b.emit(op("VGET"), t, r)
		return t
	case v.Kind == check.RowItem, b.l.prog.AccessorVar(v):
		t := b.temp(b.kind(v.Type))
		b.emit(op("AGET"), t, r)
		return t
	case v.Kind == check.ViewMemo, v.Kind == check.StyleLocal && b.l.styleMemo[v]:
		t := b.temp(b.kind(v.Type))
		b.emit(op("MREAD"), t, r)
		return t
	}
	return r
}

// writeVar sets v to val.
func (b *fb) writeVar(v *check.Var, val int, n syntax.Node) {
	r, ok := b.vars[v]
	if !ok {
		b.l.errorf(n, diag.InternalError, "internal: %s has no register", v.Name())
		return
	}
	if v.Boxed() {
		b.emit(op("VSET"), r, val)
		return
	}
	b.move(r, val)
}

// ---------------------------------------------------------------------
// Components' and stores' members

// env is the env struct's register.
func (b *fb) env(n syntax.Node) int {
	if b.envReg >= 0 {
		return b.envReg
	}
	b.l.errorf(n, diag.NotSupported, "%s's members can't be used here (while it's being made)", b.compName())
	return b.null()
}

func (b *fb) compName() string {
	if b.comp != nil {
		return b.comp.Name()
	}
	return "the component"
}

// memberIndex is m's field in its owner's env.
func (l *Lowerer) memberIndex(m *check.Member) int {
	if i, ok := l.memberIdx[m]; ok {
		return i
	}
	i := 0
	for _, o := range m.Owner.Comp.Order {
		if o.Kind == check.FnMember {
			continue
		}
		l.memberIdx[o] = i
		i++
	}
	return l.memberIdx[m]
}

// ownerIndex is the field of t's env that holds the owner it was made
// under, after its members (t.Comp.HoldsHandlers).
func (l *Lowerer) ownerIndex(t *check.TypeName) int {
	n := 0
	for _, m := range t.Comp.Order {
		if m.Kind != check.FnMember {
			n++
		}
	}
	return n
}

// heldHandler wraps clos, an async handler of a component's event, as
// what the component that declares it holds (ui.handler): its tasks are
// that component's, held by the owner its env keeps; outside a
// component, by the owner where it's declared.
func (b *fb) heldHandler(clos int) int {
	holder := -1
	if b.comp != nil && b.comp.Comp != nil && b.comp.Comp.HoldsHandlers {
		switch {
		case b.ownerReg >= 0:
			holder = b.ownerReg
		case b.envReg >= 0:
			holder = b.temp(bc.KRef)
			b.emit(op("GETF"), holder, b.envReg, b.l.ownerIndex(b.comp))
		}
	}
	if holder < 0 {
		holder = b.temp(bc.KRef)
		b.emit(op("KNONE"), holder)
	}
	return b.callB(uiHandler, bc.KRef, clos, holder)
}

// suspends reports whether the function being lowered is async: it may
// await (bc.FAsync).
func (b *fb) suspends() bool {
	return b.f.Flags&bc.FAsync != 0 || b.ctx != nil && b.ctx.Awaits
}

// memberKind is how m is stored in the env.
func (l *Lowerer) memberKind(m *check.Member) bc.Kind {
	if m.Owner.Comp.Worker && m.Kind != check.VarMember {
		return l.kind(m.Type) // a worker's parameters and lets: values
	}
	if m.Kind == check.LetMember && !m.Memo && !isLoad(m.Default) {
		return l.kind(m.Type)
	}
	return bc.KRef
}

func isLoad(e syntax.Expr) bool {
	u, ok := e.(*syntax.UnaryExpr)
	return ok && u.Op == syntax.LOAD
}

// envShapeOf is a component's or store's env, typ its type (a generic
// store's instantiation): a struct of its members' cells, memos,
// accessors and handlers.
func (l *Lowerer) envShapeOf(t *check.TypeName, typ check.Type) int {
	if typ == nil || check.HasTypeParams(typ) && len(t.TypeParams) == 0 {
		typ = t.Type
	}
	key := l.typeKey(typ)
	if i, ok := l.envShape[key]; ok {
		return i
	}
	i := l.addShape(key, typ)
	l.envShape[key] = i
	var fs []bc.Field
	for _, m := range t.Comp.Order {
		if m.Kind == check.FnMember {
			continue
		}
		l.memberIndex(m)
		fs = append(fs, bc.Field{Name: m.Name(), Kind: l.memberKind(m), Shape: -1})
	}
	if t.Comp.HoldsHandlers {
		// After the members: the owner it was made under (ownerIndex).
		fs = append(fs, bc.Field{Name: "$owner", Kind: bc.KRef, Shape: -1})
	}
	l.mod.Shapes[i].Fields = fs
	return i
}

// memberRef is the register holding m's cell, memo, accessor or value.
func (b *fb) memberRef(m *check.Member, n syntax.Node) int {
	if r, ok := b.memberRegs[m]; ok {
		return r
	}
	env := b.env(n)
	r := b.temp(b.l.memberKind(m))
	b.emit(op("GETF"), r, env, b.l.memberIndex(m))
	return r
}

// readMember is m's value.
func (b *fb) readMember(m *check.Member, n syntax.Node) int {
	if m.Owner.Comp.Worker {
		return b.workerMember(m, n)
	}
	ref := b.memberRef(m, n)
	k := b.kind(m.Type)
	switch m.Kind {
	case check.StateMember:
		r := b.temp(k)
		b.emit(op("CGET"), r, ref)
		return r
	case check.LetMember:
		if isLoad(m.Default) {
			return ref
		}
		if m.Path {
			// A path: its accessor's (rfcs/0013).
			r := b.temp(k)
			b.emit(op("AGET"), r, ref)
			return r
		}
		if m.Memo {
			r := b.temp(k)
			b.emit(op("MREAD"), r, ref)
			return r
		}
		return ref
	case check.EventMember:
		// Whether the caller handles it: its handler isn't none.
		r := b.temp(bc.KBool)
		b.emit(op("ISNONE"), r, ref)
		b.emit(op("NOT"), r, r)
		return r
	case check.ParamMember, check.PropMember:
		if m.IsSlot() {
			return ref // a builder
		}
		r := b.temp(k)
		b.emit(op("AGET"), r, ref)
		return r
	}
	return ref
}

// storeMemberRef is a store member's cell or memo, from the store value s.
func (b *fb) storeMemberRef(s int, m *check.Member) int {
	r := b.temp(b.l.memberKind(m))
	b.emit(op("GETF"), r, s, b.l.memberIndex(m))
	return r
}

// ---------------------------------------------------------------------
// Closures

// closure describes a closure to make: its function's body, and what it
// captures (what its contexts capture, and the env when one uses it).
type closure struct {
	name   string
	ctxs   []*check.FuncCtx
	params []*check.Var // nil entries: parameters it ignores
	pkinds []bc.Kind    // the kinds of the nil ones (default ref)
	ret    bc.Kind
	async  bool
	throws bool
	env    bool       // capture the env even if no context says so
	extra  []extraCap // more captures: registers of the maker's
	// byValue: the vars it captures are copied in, their values at the
	// time it's made, not shared (spawn's: values only).
	byValue bool
	body    func(c *fb)
}

// extraCap captures the maker's register reg as v.
type extraCap struct {
	v   *check.Var
	reg int
}

// makeClosure makes the closure's function and, here, the closure: its
// register (a temp).
func (b *fb) makeClosure(s closure) int {
	l := b.l
	c := l.newFunc(b.f.Name+"/"+s.name, b.f.File, len(s.params), s.ret)
	c.subst = b.subst
	c.comp = b.comp
	c.locals = b.locals
	c.line = b.line
	if len(s.ctxs) > 0 {
		c.ctx = s.ctxs[0]
	}
	// Parameters.
	for i, v := range s.params {
		if v == nil {
			k := bc.KRef
			if i < len(s.pkinds) && s.pkinds[i] != bc.KVoid {
				k = s.pkinds[i]
			}
			c.newReg(k)
			continue
		}
		k := c.kind(v.Type)
		if v.Kind == check.RowItem || v.Kind == check.ViewMemo || l.prog.AccessorVar(v) {
			// An accessor (a for's key's item over a path, of any type).
			k = bc.KRef
		}
		r := c.newReg(k)
		c.setVar(v, r)
	}
	// Captures.
	var caps []int // the maker's registers
	var capVars []*check.Var
	seen := map[*check.Var]bool{}
	for _, p := range s.params {
		if p != nil {
			seen[p] = true
		}
	}
	useEnv := s.env
	for _, x := range s.ctxs {
		if x == nil {
			continue
		}
		useEnv = useEnv || x.UsesEnv
		for _, v := range x.Captures {
			if seen[v] {
				continue
			}
			seen[v] = true
			r, ok := b.vars[v]
			if !ok {
				// Not the maker's: a closure further in declares it.
				continue
			}
			if s.byValue && v.Boxed() && !b.unboxed[v] {
				// The value, not the box.
				t := b.temp(b.kind(v.Type))
				b.emit(op("VGET"), t, r)
				r = t
				c.unboxed[v] = true
			} else if b.unboxed[v] {
				c.unboxed[v] = true // a copy already: a spawn's closure's
			}
			caps = append(caps, r)
			capVars = append(capVars, v)
		}
	}
	for _, x := range s.extra {
		if !seen[x.v] {
			seen[x.v] = true
			caps = append(caps, x.reg)
			capVars = append(capVars, x.v)
		}
	}
	envCap := -1
	envIsBox := false
	if useEnv {
		switch {
		case b.envReg >= 0:
			envCap = len(caps)
			caps = append(caps, b.envReg)
		case b.envBox >= 0:
			envCap = len(caps)
			envIsBox = true
			caps = append(caps, b.envBox)
		}
	}
	// The closure's prologue loads its captures.
	for i, r := range caps {
		k := b.f.Kinds[r]
		c.f.CapKinds = append(c.f.CapKinds, k)
		reg := c.newReg(k)
		c.emit(op("CAP"), reg, i)
		if i == envCap {
			if envIsBox {
				env := c.newReg(bc.KRef)
				c.emit(op("VGET"), env, reg)
				reg = env
			}
			c.envReg = reg
			c.noteEnv()
			continue
		}
		c.setVar(capVars[i], reg)
	}
	c.f.Flags &= bc.FSized // what its parameters' types said
	if s.throws {
		c.f.Flags |= bc.FThrows
	}
	if s.async {
		c.f.Flags |= bc.FAsync // before the body, which asks (suspends)
	}
	s.body(c)
	// Here: the closure.
	ks := make([]bc.Kind, len(caps))
	for i, r := range caps {
		ks[i] = b.f.Kinds[r]
	}
	blk := b.regs(ks)
	for i, r := range caps {
		b.move(blk+i, r)
	}
	dst := b.temp(bc.KRef)
	if b.holdCaps != nil {
		// Held for a loop (seqfuse.go): the closure's register none
		// before it and let go of after the loop, so that once the
		// optimizer has inlined its calls, nothing reads it and it isn't
		// made.
		b.emit(op("CLR"), dst)
		b.emit(op("CLOS"), dst, c.idx, blk, len(caps))
		*b.holdCaps = append(*b.holdCaps, heldBlock{base: blk, ks: ks, clos: dst})
		b.holdCaps = nil // only this closure's
		return dst
	}
	b.emit(op("CLOS"), dst, c.idx, blk, len(caps))
	b.releaseRegs(blk, ks)
	return dst
}

// thunk is a closure of no arguments returning e's value: a binding.
func (b *fb) thunk(ctx *check.FuncCtx, e syntax.Expr, t check.Type) int {
	return b.thunkNamed("binding", ctx, e, t)
}

// thunkNamed is thunk, its function named name: what the core's
// diagnostics call the site ("let total": Counter's let total at
// app.asuka:4).
func (b *fb) thunkNamed(name string, ctx *check.FuncCtx, e syntax.Expr, t check.Type) int {
	k := b.kind(t)
	return b.makeClosure(closure{name: name, ctxs: []*check.FuncCtx{ctx}, ret: k, throws: true, body: func(c *fb) {
		c.at(e)
		r := c.value(e, -1)
		c.ret(r, k)
	}})
}

// ret returns r (of kind k).
func (b *fb) ret(r int, k bc.Kind) {
	if k == bc.KVoid || r < 0 {
		b.emit(op("RET0"))
		return
	}
	b.emit(op("RET"), b.valueReg(r, k))
}
