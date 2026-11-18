package ir

import (
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
	// tooBig: the function was said to need more than the bytecode holds
	// (registers, an operand), once.
	tooBig bool

	// inout: the place an inout builtin's call changes, when it's x?.f's
	// (places.go), and what's done with its result there.
	inout      *lvalue
	inoutAfter func(r int)

	vars map[*check.Var]int // a local's register (its box, accessor or memo)
	// unboxed: boxed vars this function holds the values of (a spawn's
	// copies), in their registers.
	unboxed map[*check.Var]bool
	// locals: the local fns in scope, their functions' indexes (shared
	// with the closures and local fns made inside).
	locals map[*check.LocalFn]int

	// Registers: per kind, those free to reuse; temps die at the end of
	// the statement that made them; locals at the end of their block.
	free      map[bc.Kind][]int
	temps     []int
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
		if line := b.l.line(n); line > 0 {
			b.line = line
		}
	}
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
	return r
}

func (b *fb) mark() int { return len(b.temps) }

// release frees the temps made since mark.
func (b *fb) release(mark int) {
	for _, r := range b.temps[mark:] {
		k := b.f.Kinds[r]
		b.free[k] = append(b.free[k], r)
	}
	b.temps = b.temps[:mark]
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
func (b *fb) typ(t check.Type) check.Type { return b.subst.Apply(t) }

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
		b.vars[v] = r
		return r
	}
	r := b.local(k)
	if init >= 0 {
		b.move(r, init)
	}
	b.vars[v] = r
	return r
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
		c.vars[v] = r
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
			continue
		}
		c.vars[capVars[i]] = reg
	}
	c.f.Flags = 0
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
