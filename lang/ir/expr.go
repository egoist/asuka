package ir

import (
	"slices"
	"strings"

	bc "github.com/egoist/asuka/lang/bytecode"
	"github.com/egoist/asuka/lang/check"
	"github.com/egoist/asuka/lang/diag"
	"github.com/egoist/asuka/lang/syntax"
)

// out is where an expression's value goes: dst when it's given, else a
// temp of kind k.
func (b *fb) out(dst int, k bc.Kind) int {
	if dst >= 0 {
		return dst
	}
	return b.temp(k)
}

// value lowers e as it's used: with its implicit conversion. It returns
// the register holding the value (dst when given; else maybe a local's
// own register, which the caller must not change).
func (b *fb) value(e syntax.Expr, dst int) int {
	conv, ok := b.l.info.Convs[e]
	if !ok {
		return b.expr(e, dst)
	}
	to := b.typ(conv)
	from := b.rawType(e)
	if from == check.Never {
		// It leaves: the code after it isn't reached, but is compiled
		// with a register of the type expected (an argument's, …).
		b.expr(e, -1)
		if dst >= 0 {
			return dst
		}
		return b.valueReg(-1, b.l.kind(to))
	}
	inner := check.Unopt(to)
	if n, ok := inner.(*check.Named); ok && n.Obj.Kind == check.InterfaceType {
		b.l.implement(check.Unopt(from), n, e)
	}
	if n, ok := inner.(*check.Named); ok && check.IsJSONValue(n) && !check.IsJSONValue(check.Unopt(from)) {
		r := b.jsonValue(e, from, n, -1)
		return b.toOptional(r, inner, to, dst)
	}
	// What's converted when compiling: lengths and edges.
	if inner == check.Length && from != check.Length {
		r := b.temp(bc.KInt)
		if check.IsConstNumber(e) {
			b.loadInt(r, check.PackLength(1, check.ConstNumber(e)))
		} else {
			x := b.expr(e, -1)
			if b.f.Kinds[x] == bc.KInt {
				f := b.temp(bc.KFloat)
				b.emit(op("I2F"), f, x)
				x = f
			}
			b.callBTo("length.points", r, x)
		}
		return b.toOptional(r, check.Length, to, dst)
	}
	if b.isEdges(inner) && !b.isEdges(from) {
		r := b.edges(e, inner)
		return b.toOptional(r, inner, to, dst)
	}
	var r int
	if check.IsOptional(to) && !check.IsOptional(from) && from != check.NoneType {
		r = b.expr(e, -1)
		return b.toOptional(r, from, to, dst)
	}
	r = b.expr(e, dst)
	return r
}

// toOptional makes r (a from) a to, an optional when it's one.
func (b *fb) toOptional(r int, from, to check.Type, dst int) int {
	if !check.IsOptional(to) {
		if dst >= 0 && dst != r {
			b.move(dst, r)
			return dst
		}
		return r
	}
	k := b.l.kind(from)
	if k == bc.KRef {
		if dst >= 0 && dst != r {
			b.move(dst, r)
			return dst
		}
		return r
	}
	o := b.out(dst, bc.KRef)
	b.emit(op("BOX"), o, r, int(k))
	return o
}

// isEdges reports the prelude's Edges or Corners (check's isEdges).
func (b *fb) isEdges(t check.Type) bool {
	n, ok := t.(*check.Named)
	return ok && n.Obj.Pkg != nil && n.Obj.Pkg.Prelude && (n.Obj.Name() == "Edges" || n.Obj.Name() == "Corners")
}

// edges makes Edges from a number or [v, h] / [t, r, b, l], or Corners
// (want's) from a number or [tl, tr, br, bl]: the four physical ones,
// the logical ones 0.
func (b *fb) edges(e syntax.Expr, want check.Type) int {
	var t, r, bo, l int
	num := func(x syntax.Expr) int {
		v := b.expr(x, -1)
		if b.f.Kinds[v] == bc.KInt {
			f := b.temp(bc.KFloat)
			b.emit(op("I2F"), f, v)
			return f
		}
		return v
	}
	if a, ok := e.(*syntax.ArrayExpr); ok {
		if len(a.Elems) == 2 {
			t, r = num(a.Elems[0]), num(a.Elems[1])
			bo, l = t, r
		} else {
			t, r, bo, l = num(a.Elems[0]), num(a.Elems[1]), num(a.Elems[2]), num(a.Elems[3])
		}
	} else {
		t = num(e)
		r, bo, l = t, t, t
	}
	name, fields := "Edges", 6
	if n, ok := want.(*check.Named); ok && n.Obj.Name() == "Corners" {
		name, fields = "Corners", 8
	}
	box := b.l.prog.Prelude.Scope.LookupLocal(name).(*check.TypeName)
	shape := b.l.shape(box.Type)
	ks := repeatKind(bc.KFloat, fields)
	blk := b.regs(ks)
	for i, v := range []int{t, r, bo, l} {
		b.move(blk+i, v)
	}
	z := b.temp(bc.KFloat)
	b.emit(op("KFLOAT"), z, b.l.floatConst(0))
	for i := 4; i < fields; i++ {
		b.move(blk+i, z)
	}
	o := b.temp(bc.KRef)
	b.emit(op("NEWS"), o, shape, blk)
	b.releaseRegs(blk, ks)
	return o
}

// expr lowers e, its own type (no conversion).
func (b *fb) expr(e syntax.Expr, dst int) int {
	b.at(e)
	switch e := e.(type) {
	case *syntax.ParenExpr:
		return b.value(e.X, dst)
	case *syntax.Ident:
		return b.ident(e, dst)
	case *syntax.BasicLit:
		return b.basicLit(e, dst)
	case *syntax.StringLit:
		return b.stringLit(e, dst)
	case *syntax.BoolLit:
		r := b.out(dst, bc.KBool)
		v := 0
		if e.Value {
			v = 1
		}
		b.emit(op("KSMALL"), r, v)
		return r
	case *syntax.NoneLit:
		r := b.out(dst, bc.KRef)
		b.emit(op("KNONE"), r)
		return r
	case *syntax.SelfExpr:
		if _, ok := b.l.info.StoreSelf[e]; ok {
			return b.copyTo(b.env(e), dst)
		}
		return b.copyTo(b.readVar(b.ctx.Self, e), dst)
	case *syntax.AtExpr:
		return b.atValue(e, dst)
	case *syntax.ImplicitMember:
		return b.selection(e, nil, dst)
	case *syntax.MemberExpr:
		if r, ok := b.pathRead(e, dst); ok {
			return r
		}
		return b.selection(e, e.X, dst)
	case *syntax.CallExpr:
		if r, ok := b.pathRead(e, dst); ok {
			return r
		}
		if r, ok := b.pathMethod(e, dst); ok {
			return r
		}
		return b.call(e, dst)
	case *syntax.IndexExpr:
		if r, ok := b.pathRead(e, dst); ok {
			return r
		}
		if xt := b.rawType(e.X); check.IsOptional(xt) {
			// In a ?. chain (a?.items[0]): none when what's before is.
			return b.chainedIndex(e, xt, dst)
		}
		x := b.value(e.X, -1)
		i := b.value(e.Index, -1)
		if _, ok := b.rawType(e.X).(*check.Map); ok && e.Default != nil {
			// m[k, default: v]: the value as it is, or v (evaluated only
			// then).
			r := b.out(dst, b.kind(b.l.info.Types[e]))
			end, miss := b.f.NewLabel(), b.f.NewLabel()
			b.emit(op("MGETJ"), r, x, i, int(miss))
			b.jmp(end)
			b.f.Place(miss)
			b.move(r, b.value(e.Default, r))
			b.f.Place(end)
			return r
		}
		if _, ok := b.rawType(e.X).(*check.Map); ok {
			r := b.out(dst, bc.KRef)
			b.emit(op("MGET"), r, x, i)
			return r
		}
		if b.rawType(e.X) == check.Bytes {
			return b.copyTo(b.callB("bytes.at", bc.KInt, x, i), dst)
		}
		r := b.out(dst, b.kind(b.l.info.Types[e]))
		b.emit(op("IDX"), r, x, i)
		return r
	case *syntax.UnwrapExpr:
		x := b.value(e.X, -1)
		return b.unwrap(x, b.rawType(e), dst, e)
	case *syntax.UnaryExpr:
		return b.unary(e, dst)
	case *syntax.SpawnExpr:
		return b.spawn(e, dst)
	case *syntax.BinaryExpr:
		return b.binary(e, dst)
	case *syntax.LambdaExpr:
		return b.copyTo(b.lambda(e), dst)
	case *syntax.TupleExpr:
		if r, ok := b.hoist(e, dst); ok {
			return r
		}
		t := b.rawType(e).(*check.Tuple)
		var vals []int
		for _, x := range e.Elems {
			vals = append(vals, b.value(x, -1))
		}
		return b.newStruct(b.l.shape(t), t.Elems, vals, dst)
	case *syntax.ArrayExpr:
		if r, ok := b.hoist(e, dst); ok {
			return r
		}
		elems, rest := e.Elems, []syntax.Expr(nil)
		if len(elems) > literalChunk {
			elems, rest = elems[:literalChunk], elems[literalChunk:]
		}
		if st, ok := b.rawType(e).(*check.Set); ok {
			// A set literal: set.of(kind, its elements...).
			args := []int{b.konst(int64(b.l.kind(st.Elem)))}
			mark := b.mark()
			for _, x := range elems {
				args = append(args, b.value(x, -1))
			}
			r := b.literalOut(dst, len(rest) > 0)
			b.copyTo(b.callB("set.of", bc.KRef, args...), r)
			b.release(mark)
			b.appendEach(r, rest, "set.insert")
			return b.literalDone(r, dst)
		}
		at, _ := b.rawType(e).(*check.Array)
		if at == nil {
			return b.valueReg(-1, bc.KRef)
		}
		k := b.l.kind(at.Elem)
		ks := repeatKind(k, len(elems))
		r := b.literalOut(dst, len(rest) > 0)
		mark := b.mark()
		var vals []int
		for _, x := range elems {
			vals = append(vals, b.value(x, -1))
		}
		blk := b.regs(ks)
		for i, v := range vals {
			b.move(blk+i, v)
		}
		sh := b.l.shape(check.Unopt(at.Elem))
		if sh < 0 {
			sh = bc.NoShape
		}
		b.emit(op("NEWA"), r, b.l.arrKind(at.Elem), sh, blk, len(vals))
		b.releaseRegs(blk, ks)
		b.release(mark)
		b.appendEach(r, rest, "arr.push")
		return b.literalDone(r, dst)
	case *syntax.MapExpr:
		if r, ok := b.hoist(e, dst); ok {
			return r
		}
		mt, _ := b.rawType(e).(*check.Map)
		if mt == nil {
			return b.valueReg(-1, bc.KRef)
		}
		kk, vk := b.l.kind(mt.Key), b.l.kind(mt.Elem)
		entries, rest := e.Entries, []*syntax.MapEntry(nil)
		if len(entries) > literalChunk {
			entries, rest = entries[:literalChunk], entries[literalChunk:]
		}
		r := b.literalOut(dst, len(rest) > 0)
		mark := b.mark()
		var ks []bc.Kind
		var vals []int
		for _, en := range entries {
			ks = append(ks, kk, vk)
			vals = append(vals, b.value(en.Key, -1), b.value(en.Value, -1))
		}
		blk := b.regs(ks)
		for i, v := range vals {
			b.move(blk+i, v)
		}
		b.emit(op("NEWM"), r, int(kk), int(vk), blk, len(entries))
		b.releaseRegs(blk, ks)
		b.release(mark)
		for _, en := range rest {
			mark := b.mark()
			k := b.value(en.Key, -1)
			v := b.value(en.Value, -1)
			b.emit(op("MSET"), r, k, v)
			b.release(mark)
		}
		return b.literalDone(r, dst)
	case *syntax.IfExpr:
		return b.ifExpr(e, dst)
	case *syntax.MatchExpr:
		return b.matchExpr(e, dst)
	case *syntax.JumpExpr:
		return b.jumpExpr(e, dst)
	case *syntax.CatchExpr:
		return b.catchExpr(e, dst)
	}
	b.l.errorf(e, diag.NotSupported, "can't lower this expression yet")
	return b.valueReg(-1, b.kind(b.l.info.Types[e]))
}

// copyTo copies r into dst when dst is given.
func (b *fb) copyTo(r, dst int) int {
	if dst >= 0 && r != dst {
		b.move(dst, r)
		return dst
	}
	return r
}

// newStruct makes a struct (or a tuple) of shape from vals.
func (b *fb) newStruct(shape int, types []check.Type, vals []int, dst int) int {
	ks := make([]bc.Kind, len(vals))
	for i, t := range types {
		ks[i] = b.l.kind(t)
	}
	blk := b.regs(ks)
	for i, v := range vals {
		b.move(blk+i, b.valueReg(v, ks[i]))
	}
	r := b.out(dst, bc.KRef)
	b.emit(op("NEWS"), r, shape, blk)
	b.releaseRegs(blk, ks)
	return r
}

// durationFloat is a Duration scaled by a float, or a Duration divided
// by another: in floats, a Duration back to nanoseconds.
func (b *fb) durationFloat(e *syntax.BinaryExpr, dst int) int {
	f := func(x syntax.Expr) int {
		v := b.value(x, -1)
		if b.f.Kinds[v] == bc.KFloat {
			return v
		}
		r := b.temp(bc.KFloat)
		b.emit(op("I2F"), r, v)
		return r
	}
	x, y := f(e.X), f(e.Y)
	r := b.temp(bc.KFloat)
	if e.Op == syntax.MUL {
		b.emit(op("MULF"), r, x, y)
	} else {
		b.emit(op("DIVF"), r, x, y)
	}
	if b.typ(b.l.info.Types[e]) == check.Float {
		return b.copyTo(r, dst)
	}
	out := b.out(dst, bc.KInt)
	b.emit(op("F2I"), out, r)
	return out
}

// operator is a binary operator a struct's method does: the method's
// call, its compare's result against 0.
func (b *fb) operator(e *syntax.BinaryExpr, o *check.Operator, dst int) int {
	rt := b.typ(o.Recv)
	x := b.value(e.X, -1)
	y := b.value(e.Y, -1)
	idx := b.l.funcIndex(o.Func, nil, rt)
	s := b.calleeSubst(o.Func, nil, rt)
	k := b.l.kind(s.Apply(o.Func.Sig.Result))
	if !o.Compare {
		return b.invoke(idx, []int{x, y}, k, dst, false)
	}
	c := b.invoke(idx, []int{x, y}, bc.KInt, -1, false)
	zero := b.konst(0)
	r := b.out(dst, bc.KBool)
	switch e.Op {
	case syntax.LSS:
		b.emit(op("LTI"), r, c, zero)
	case syntax.LEQ:
		b.emit(op("LEI"), r, c, zero)
	case syntax.GTR:
		b.emit(op("LTI"), r, zero, c)
	default:
		b.emit(op("LEI"), r, zero, c)
	}
	return r
}

// unwrap is the value of the optional x (t is the value's type); none
// throws.
func (b *fb) unwrap(x int, t check.Type, dst int, n syntax.Node) int {
	ok := b.f.NewLabel()
	b.emitJump(op("JSOME"), x, ok)
	b.throwMsg("unwrapped none (" + b.where(n) + ")")
	b.f.Place(ok)
	return b.unbox(x, t, dst)
}

// unbox is the value in the optional x, known not none.
func (b *fb) unbox(x int, t check.Type, dst int) int {
	k := b.l.kind(t)
	if k == bc.KRef {
		return b.copyTo(x, dst)
	}
	if k == bc.KVoid {
		// Nothing (a void task's place in all's results).
		return -1
	}
	r := b.out(dst, k)
	b.emit(op("UNBOX"), r, x)
	return r
}

func (b *fb) where(n syntax.Node) string {
	p := b.l.fset.Position(n.Range().Start)
	name := p.Filename
	if i := strings.LastIndexByte(name, '/'); i >= 0 {
		name = name[i+1:]
	}
	return name + ":" + itoa(p.Line)
}

// throwMsg throws a runtime error with a message.
func (b *fb) throwMsg(msg string) {
	m := b.kstr(msg)
	e := b.callB("error", bc.KRef, m)
	b.emit(op("THROW"), e)
}

// ---------------------------------------------------------------------
// Names

func (b *fb) ident(e *syntax.Ident, dst int) int {
	if x := b.l.info.ImplicitSelf[e]; x != nil {
		// self's property by its bare name.
		return b.value(x, dst)
	}
	switch o := b.l.info.Uses[e].(type) {
	case *check.Var:
		if o.Fn != nil {
			return b.copyTo(b.localFnValue(o.Fn, e), dst)
		}
		return b.copyTo(b.readVar(o, e), dst)
	case *check.Member:
		return b.copyTo(b.readMember(o, e), dst)
	case *check.Global:
		return b.readGlobal(o, dst)
	case *check.Function:
		return b.copyTo(b.funcValue(o), dst)
	}
	if sm := b.selfField(e); sm >= 0 {
		return b.copyTo(sm, dst)
	}
	b.l.errorf(e, diag.InternalError, "internal: can't lower %s", e.Name)
	return b.valueReg(-1, bc.KRef)
}

// selfField reads a field of self named by its bare name in a method
// (an interface's field in a provided method's).
func (b *fb) selfField(e *syntax.Ident) int {
	if b.ctx == nil || b.ctx.Self == nil {
		return -1
	}
	if fl, _, owner, ok := check.SelfMember(b.l.info.Uses[e]); ok && fl != nil && owner.Kind == check.InterfaceType {
		self := b.readVar(b.ctx.Self, e)
		return b.ifaceField(self, b.typ(b.ctx.Self.Type), fl.Name, fl.Type, -1)
	}
	n, ok := b.typ(b.ctx.Self.Type).(*check.Named)
	if !ok || n.Obj.Kind != check.StructType {
		return -1
	}
	f := n.Obj.Field(e.Name)
	if f == nil {
		return -1
	}
	self := b.readVar(b.ctx.Self, e)
	return b.getField(self, n, f, -1)
}

// getField reads struct value x's field f (t nil: a tuple's element,
// f its place).
func (b *fb) getField(x int, t *check.Named, f *check.Field, dst int) int {
	var k bc.Kind
	if t == nil {
		k = b.kind(f.Type)
	} else {
		k = b.l.kind(t.Subst().Apply(f.Type))
	}
	r := b.out(dst, k)
	if t != nil && t.Obj.IsError {
		b.emit(op("GETP"), r, x, f.Index)
	} else {
		b.emit(op("GETF"), r, x, f.Index)
	}
	return r
}

// funcValue is a declared function as a closure.
func (b *fb) funcValue(f *check.Function) int {
	idx := b.l.funcIndex(f, nil, nil)
	r := b.temp(bc.KRef)
	b.emit(op("CLOS"), r, idx, 0, 0)
	return r
}

func (b *fb) readGlobal(g *check.Global, dst int) int {
	slot := b.l.global(g)
	k := b.l.kind(g.Type)
	switch g.Kind {
	case check.StateGlobal:
		cell := b.temp(bc.KRef)
		b.emit(op("GGET"), cell, slot, int(bc.KRef))
		r := b.out(dst, k)
		b.emit(op("CGET"), r, cell)
		return r
	case check.MemoGlobal:
		m := b.temp(bc.KRef)
		b.emit(op("GGET"), m, slot, int(bc.KRef))
		r := b.out(dst, k)
		b.emit(op("MREAD"), r, m)
		return r
	}
	if r, ok := b.literalGlobal(g, dst); ok {
		return r
	}
	r := b.out(dst, k)
	b.emit(op("GGET"), r, slot, int(k))
	return r
}

// literalGlobal reads a let whose value is a literal number, duration,
// color or bool (let nsPerSecond = 1_000_000_000) as the literal: a
// constant the optimizer and the C compiler see (a division by it a
// multiply), not a read of the global's slot.
func (b *fb) literalGlobal(g *check.Global, dst int) (int, bool) {
	if g.Kind != check.ConstGlobal || g.Value == nil {
		return 0, false
	}
	if _, conv := b.l.info.Convs[g.Value]; conv {
		return 0, false
	}
	switch x := g.Value.(type) {
	case *syntax.BasicLit:
		if x.Kind == syntax.INT || x.Kind == syntax.FLOAT || x.Kind == syntax.DURATION || x.Kind == syntax.COLOR {
			return b.basicLit(x, dst), true
		}
	case *syntax.BoolLit:
		r := b.out(dst, bc.KBool)
		v := 0
		if x.Value {
			v = 1
		}
		b.emit(op("KSMALL"), r, v)
		return r, true
	case *syntax.UnaryExpr:
		lit, ok := x.X.(*syntax.BasicLit)
		if !ok || x.Op != syntax.SUB {
			break
		}
		switch t := b.l.info.Types[g.Value]; {
		case lit.Kind == syntax.INT && t == check.Int:
			v, err := check.ParseInt(lit.Value)
			if err != nil {
				break
			}
			r := b.out(dst, bc.KInt)
			b.loadInt(r, -v)
			return r, true
		case lit.Kind == syntax.FLOAT && t == check.Float:
			v, err := check.ParseFloat(lit.Value)
			if err != nil {
				break
			}
			r := b.out(dst, bc.KFloat)
			b.emit(op("KFLOAT"), r, b.l.floatConst(-v))
			return r, true
		}
	}
	return 0, false
}

// globalCell is a state global's cell (or a memo global's memo).
func (b *fb) globalCell(g *check.Global) int {
	r := b.temp(bc.KRef)
	b.emit(op("GGET"), r, b.l.global(g), int(bc.KRef))
	return r
}

// ---------------------------------------------------------------------
// Literals

func (b *fb) basicLit(e *syntax.BasicLit, dst int) int {
	t := b.l.info.Types[e]
	switch e.Kind {
	case syntax.INT:
		if t == check.Float {
			u, _ := check.ParseUint(e.Value)
			r := b.out(dst, bc.KFloat)
			b.emit(op("KFLOAT"), r, b.l.floatConst(float64(u)))
			return r
		}
		v := check.LitBits(e.Value, false) // a u64 past int's largest: its bits
		r := b.out(dst, bc.KInt)
		b.loadInt(r, v)
		return r
	case syntax.FLOAT:
		v, _ := check.ParseFloat(e.Value)
		r := b.out(dst, bc.KFloat)
		b.emit(op("KFLOAT"), r, b.l.floatConst(v))
		return r
	case syntax.DURATION:
		v, _ := check.ParseDuration(e.Value)
		r := b.out(dst, bc.KInt)
		b.loadInt(r, v)
		return r
	case syntax.COLOR:
		v, _ := check.ParseColor(e.Value)
		r := b.out(dst, bc.KInt)
		b.loadInt(r, v)
		return r
	case syntax.LENGTH:
		v, _ := check.ParseLength(e.Value)
		r := b.out(dst, bc.KInt)
		b.loadInt(r, v)
		return r
	case syntax.CHAR:
		r := b.out(dst, bc.KInt)
		b.loadInt(r, int64(check.CharValue(e.Value)))
		return r
	}
	return b.valueReg(-1, bc.KInt)
}

// literalText is a string literal's text, escapes resolved.
func literalText(seg string, raw, multiline bool) string {
	s, _ := check.Unescape(seg, raw)
	return s
}

func (b *fb) stringLit(e *syntax.StringLit, dst int) int {
	segs := e.Segments
	if e.Multiline {
		// Dedent the text as one, keeping the interpolations' places.
		const mark = "\x00\x01"
		joined := strings.Join(segs, mark)
		segs = strings.Split(check.Dedent(joined), mark)
	}
	if len(e.Interps) == 0 {
		r := b.out(dst, bc.KRef)
		b.emit(op("KSTR"), r, b.l.str(literalText(segs[0], e.Raw, e.Multiline)))
		return r
	}
	// INTERP over a block: text, value, text, ...
	type part struct {
		text string
		x    syntax.Expr // nil for text
		spec string      // x's format ("" none)
	}
	var parts []part
	for i, x := range e.Interps {
		if s := literalText(segs[i], e.Raw, e.Multiline); s != "" {
			parts = append(parts, part{text: s})
		}
		pt := part{x: x}
		if i < len(e.Specs) && e.Specs[i] != nil {
			pt.spec = e.Specs[i].Text
		}
		parts = append(parts, pt)
	}
	if s := literalText(segs[len(segs)-1], e.Raw, e.Multiline); s != "" {
		parts = append(parts, part{text: s})
	}
	interp := func(parts []part, r int) {
		var ks []bc.Kind
		var vals []int
		var kinds []byte
		for _, p := range parts {
			if p.x == nil {
				ks = append(ks, bc.KRef)
				vals = append(vals, b.kstr(p.text))
				kinds = append(kinds, byte(bc.KRef))
				continue
			}
			v, k, kb := b.stringPart(p.x, p.spec)
			ks = append(ks, k)
			vals = append(vals, v)
			kinds = append(kinds, kb...)
		}
		blk := b.regs(ks)
		for i, v := range vals {
			b.move(blk+i, v)
		}
		if !slices.ContainsFunc(kinds, func(k byte) bool { return k != byte(bc.KRef) }) {
			kinds = nil // all strings: joined at their size
		}
		b.emit(op("INTERP"), r, blk, len(ks), b.l.str(string(kinds)))
		b.releaseRegs(blk, ks)
	}
	if len(parts) <= literalChunk {
		r := b.out(dst, bc.KRef)
		interp(parts, r)
		return r
	}
	// A string of more parts: literalChunk of them at a time, each
	// chunk's text concatenated to the ones before, so that it takes as
	// many registers as one of literalChunk.
	acc := -1
	for len(parts) > 0 {
		n := min(len(parts), literalChunk)
		r := b.temp(bc.KRef)
		mark := b.mark()
		interp(parts[:n], r)
		b.release(mark)
		parts = parts[n:]
		if acc >= 0 {
			t := b.temp(bc.KRef)
			b.emit(op("CONCAT"), t, acc, r)
			r = t
		}
		acc = r
	}
	return b.copyTo(acc, dst)
}

// interpFmt flags an INTERP part written by its format (ak_op_interp's
// AK_INTERP_FMT).
const interpFmt = 0x80

// stringPart is an interpolated value: a scalar INTERP prints itself, or
// a string; with what INTERP's kinds say of it: its kind, or, written by
// its format (spec), its kind flagged (a u64's, KU64Text: unsigned),
// the format's length and the format.
func (b *fb) stringPart(x syntax.Expr, spec string) (int, bc.Kind, []byte) {
	t := b.typeOf(x)
	v := b.value(x, -1)
	if spec != "" {
		// Formatted ({x:>8.2f}): a number by its digits, anything else by
		// its text (core/src/rt/lib.c's ak_strb_fmt), written by INTERP;
		// a format too long for its length byte, by str.fmt_*.
		if len(spec) > 255 {
			f := b.kstr(spec)
			switch {
			case t == check.U64:
				return b.callB("str.fmt_int", bc.KRef, v, f, b.konst(int64(check.IntCode(t)))), bc.KRef, []byte{byte(bc.KRef)}
			case check.IsInteger(t):
				return b.callB("str.fmt_int", bc.KRef, v, f), bc.KRef, []byte{byte(bc.KRef)}
			case t == check.Float:
				return b.callB("str.fmt_float", bc.KRef, v, f), bc.KRef, []byte{byte(bc.KRef)}
			}
			return b.callB("str.fmt_text", bc.KRef, b.describe(v, t), f), bc.KRef, []byte{byte(bc.KRef)}
		}
		formatted := func(k bc.Kind) []byte {
			return append([]byte{byte(k) | interpFmt, byte(len(spec))}, spec...)
		}
		switch {
		case t == check.U64:
			return v, bc.KInt, formatted(bc.KU64Text)
		case check.IsInteger(t):
			return v, bc.KInt, formatted(bc.KInt)
		case t == check.Float:
			return v, bc.KFloat, formatted(bc.KFloat)
		}
		return b.describe(v, t), bc.KRef, formatted(bc.KRef)
	}
	switch t {
	case check.Int, check.Float, check.Bool, check.I8, check.I16, check.I32, check.U8, check.U16, check.U32:
		k := b.l.kind(t)
		return v, k, []byte{byte(k)}
	}
	return b.describe(v, t), bc.KRef, []byte{byte(bc.KRef)}
}

// describe is the text of v, a t: what interpolation and print show.
func (b *fb) describe(v int, t check.Type) int {
	if r, ok := b.ownDescription(v, t); ok {
		return r
	}
	switch t {
	case check.String:
		return v
	case check.Color:
		// As it's written: #rrggbb, .accent.
		return b.callB("color.hex", bc.KRef, v)
	case check.Char:
		return b.callB("char.string", bc.KRef, v) // its one code point
	case check.Int, check.Float, check.Bool, check.Length, check.I8, check.I16, check.I32, check.U8, check.U16, check.U32:
		r := b.temp(bc.KRef)
		b.emit(op("TOSTR"), r, v, int(b.l.kind(t)))
		return r
	case check.U64:
		r := b.temp(bc.KRef)
		b.emit(op("TOSTR"), r, v, int(bc.KU64Text))
		return r
	case check.Duration:
		// As it's written: 300ms, 2s, 1min.
		return b.callB("show.text", bc.KRef, v, b.kstr("d"))
	}
	switch x := t.(type) {
	case *check.Optional:
		r := b.temp(bc.KRef)
		none, end := b.f.NewLabel(), b.f.NewLabel()
		b.emitJump(op("JNONE"), v, none)
		in := b.unbox(v, x.Elem, -1)
		b.move(r, b.describe(in, x.Elem))
		b.jmp(end)
		b.f.Place(none)
		b.emit(op("KSTR"), r, b.l.str("none"))
		b.f.Place(end)
		return r
	case *check.Named:
		if x.Obj.Kind == check.EnumType && !x.Obj.RefEnum() {
			var names []string
			for _, k := range x.Obj.Cases {
				names = append(names, k.Name)
			}
			g := b.l.frozenStrings(names, "cases."+b.l.typeKey(x))
			arr := b.temp(bc.KRef)
			b.emit(op("GGET"), arr, g, int(bc.KRef))
			r := b.temp(bc.KRef)
			b.emit(op("IDX"), r, arr, v)
			return r
		}
		if x.Obj.IsError || x.Obj.Kind == check.InterfaceType && b.isErrorIface(x) {
			return b.errorMessage(v, x)
		}
	}
	// The rest as the language writes it (show.text, core/src/vm/
	// literal.c): Point(x: 1, y: 2), circle(radius: 1.5), [1, 2],
	// ["a": 1]. A function, which has no text, as its type.
	d := b.l.showDesc(t)
	if d == "-" {
		return b.kstr("<" + t.String() + ">")
	}
	return b.callB("show.text", bc.KRef, v, b.kstr(d))
}

func (b *fb) isErrorIface(n *check.Named) bool {
	return n.Obj.Name() == "Error" && n.Obj.Pkg == nil
}

// errorMessage is an error's message: a user error's message method, a
// runtime error's message field.
func (b *fb) errorMessage(e int, t *check.Named) int {
	r := b.temp(bc.KRef)
	if t.Obj.IsError {
		b.emit(op("CALLI"), r, b.l.str("message"), b.argBlock1(e), 1)
		b.l.shape(t)
		return r
	}
	c := b.temp(bc.KInt)
	b.emit(op("ECASE"), c, e)
	user, end := b.f.NewLabel(), b.f.NewLabel()
	b.emitJump(op("JT"), c, user)
	b.callBTo("error.message", r, e)
	b.jmp(end)
	b.f.Place(user)
	blk := b.regs([]bc.Kind{bc.KRef})
	b.move(blk, e)
	b.emit(op("CALLI"), r, b.l.str("message"), blk, 1)
	b.releaseRegs(blk, []bc.Kind{bc.KRef})
	b.f.Place(end)
	return r
}

// argBlock1 copies r into a block of one (for CALLI's object).
func (b *fb) argBlock1(r int) int {
	blk := b.temp(bc.KRef)
	b.move(blk, r)
	return blk
}

// ---------------------------------------------------------------------
// Members

func (b *fb) selection(e syntax.Expr, x syntax.Expr, dst int) int {
	sel := b.l.info.Selections[e]
	if sel == nil {
		b.l.errorf(e, diag.InternalError, "internal: no selection")
		return b.valueReg(-1, bc.KRef)
	}
	resT := b.rawType(e)
	switch sel.Kind {
	case check.SelCase:
		return b.caseValue(b.typ(sel.Recv), sel.Case, dst)
	case check.SelConst:
		return b.readGlobal(sel.Global, dst)
	case check.SelColor, check.SelIntConst:
		r := b.out(dst, bc.KInt)
		b.loadInt(r, sel.Value)
		return r
	case check.SelLengthAuto:
		r := b.out(dst, bc.KInt)
		b.loadInt(r, 0)
		return r
	case check.SelPkg:
		switch o := sel.Obj.(type) {
		case *check.Global:
			return b.readGlobal(o, dst)
		case *check.Function:
			return b.copyTo(b.funcValue(o), dst)
		}
		b.l.errorf(e, diag.InternalError, "internal: package member")
		return b.valueReg(-1, bc.KRef)
	}
	// A member of a value.
	xv := b.value(x, -1)
	recv := b.typ(sel.Recv)
	if sel.Optional {
		// x?.name: none when x is.
		r := b.out(dst, bc.KRef)
		none, end := b.f.NewLabel(), b.f.NewLabel()
		b.emitJump(op("JNONE"), xv, none)
		in := b.unbox(xv, recv, -1)
		v := b.member(e, sel, in, recv, -1)
		mt := sel.MemberType
		if mt == nil {
			mt = check.Unopt(resT)
		} else {
			// In a generic function, its type in this instantiation: a
			// T that's an int is boxed.
			mt = b.typ(mt)
		}
		if check.IsOptional(mt) {
			b.move(r, v)
		} else {
			b.toOptional(v, mt, resT, r)
		}
		b.jmp(end)
		b.f.Place(none)
		b.emit(op("KNONE"), r)
		b.f.Place(end)
		return r
	}
	return b.member(e, sel, xv, recv, dst)
}

// member reads a member of value x (of type recv).
func (b *fb) member(e syntax.Expr, sel *check.Selection, x int, recv check.Type, dst int) int {
	switch sel.Kind {
	case check.SelBuiltinField:
		return b.builtinField(sel, x, b.rawType(e), dst)
	case check.SelVariant:
		// Component as c, then c.checked: the variant on the view the Ref
		// holds (false while it holds none), tracked.
		d := sel.Obj.(*check.VariantDef)
		r := b.out(dst, bc.KBool)
		view := b.refView(x)
		none, end := b.f.NewLabel(), b.f.NewLabel()
		b.emitJump(op("JNONE"), view, none)
		b.callBTo(uiVariantRead, r, view, b.konst(b.l.variantID(d)))
		b.jmp(end)
		b.f.Place(none)
		b.emit(op("KSMALL"), r, 0)
		b.f.Place(end)
		return r
	case check.SelField:
		return b.getField(x, recv.(*check.Named), sel.Field, dst)
	case check.SelTupleElem:
		return b.getField(x, nil, sel.Field, dst)
	case check.SelStoreMember:
		ref := b.storeMemberRef(x, sel.Member)
		m := sel.Member
		k := b.l.kind(memberTypeIn(recv, m))
		switch m.Kind {
		case check.StateMember:
			r := b.out(dst, k)
			b.emit(op("CGET"), r, ref)
			return r
		case check.LetMember:
			if m.Path {
				r := b.out(dst, k)
				b.emit(op("AGET"), r, ref)
				return r
			}
			if m.Memo && !isLoad(m.Default) {
				r := b.out(dst, k)
				b.emit(op("MREAD"), r, ref)
				return r
			}
		case check.ParamMember:
			r := b.out(dst, k)
			b.emit(op("AGET"), r, ref)
			return r
		}
		return b.copyTo(ref, dst)
	case check.SelIfaceField:
		return b.ifaceField(x, recv, sel.Name, sel.Field.Type, dst)
	case check.SelErrorMessage:
		n, _ := recv.(*check.Named)
		return b.copyTo(b.errorMessage(x, n), dst)
	case check.SelLoading:
		if sel.Name == "loading" {
			return b.copyTo(b.callB(uiLoadLoading, bc.KBool, x), dst)
		}
		return b.copyTo(b.callB(uiLoadValue, bc.KRef, x), dst)
	}
	b.l.errorf(e, diag.InternalError, "internal: can't lower member")
	return b.valueReg(-1, bc.KRef)
}

// ifaceField reads field name (of type ft) that an interface asks for of
// x, a recv: a constrained type parameter's, this instantiation's type's
// own (a struct's field, an enum's method), or an interface's value's
// through CALLI.
func (b *fb) ifaceField(x int, recv check.Type, name string, ft check.Type, dst int) int {
	if n, ok := recv.(*check.Named); ok && (n.Obj.Kind == check.StructType || n.Obj.Kind == check.EnumType) {
		if f := n.Obj.Field(name); f != nil && n.Obj.Kind == check.StructType {
			return b.getField(x, n, f, dst)
		}
		if m := n.Obj.Methods[name]; m != nil {
			return b.invoke(b.l.funcIndex(m, nil, n), []int{x}, b.kind(ft), dst, false)
		}
	}
	r := b.out(dst, b.kind(ft))
	blk := b.regs([]bc.Kind{bc.KRef})
	b.move(blk, x)
	b.emit(op("CALLI"), r, b.l.str(name), blk, 1)
	b.releaseRegs(blk, []bc.Kind{bc.KRef})
	return r
}

// caseValue is a payloadless case of enum t.
func (b *fb) caseValue(t check.Type, k *check.Case, dst int) int {
	n, ok := t.(*check.Named)
	if !ok {
		return b.valueReg(-1, bc.KInt)
	}
	if !n.Obj.RefEnum() {
		r := b.out(dst, bc.KInt)
		b.loadInt(r, int64(k.Index))
		return r
	}
	r := b.out(dst, bc.KRef)
	b.emit(op("NEWE"), r, b.l.shape(n), k.Index+caseBase(n), 0)
	return r
}

// ---------------------------------------------------------------------
// Operators

func (b *fb) unary(e *syntax.UnaryExpr, dst int) int {
	switch e.Op {
	case syntax.NOT:
		if f := b.l.fold(e); f.known {
			b.effects(f)
			return b.constBool(f.val, dst)
		}
		x := b.value(e.X, -1)
		r := b.out(dst, bc.KBool)
		b.emit(op("NOT"), r, x)
		return r
	case syntax.TILDE:
		x := b.value(e.X, -1)
		r := b.out(dst, bc.KInt)
		b.emit(op("BNOT"), r, x)
		if t := b.rawType(e); check.IsSized(t) && !check.IsSigned(t) && t != check.U64 {
			// ~ of a u8: its 8 bits flipped, not the int's 64.
			b.emit(op("WRAP"), r, r, check.IntCode(t))
		}
		return r
	case syntax.SUB:
		if o := b.l.info.Negations[e]; o != nil {
			// -x is x.neg().
			rt := b.typ(o.Recv)
			x := b.value(e.X, -1)
			s := b.calleeSubst(o.Func, nil, rt)
			return b.invoke(b.l.funcIndex(o.Func, nil, rt), []int{x}, b.l.kind(s.Apply(o.Func.Sig.Result)), dst, false)
		}
		if lit, ok := e.X.(*syntax.BasicLit); ok && lit.Kind == syntax.INT && check.IsInteger(b.rawType(e)) {
			r := b.out(dst, bc.KInt)
			b.loadInt(r, check.LitBits(lit.Value, true))
			return r
		}
		x := b.value(e.X, -1)
		t := b.rawType(e)
		if t == check.Float {
			r := b.out(dst, bc.KFloat)
			b.emit(op("NEGF"), r, x)
			return r
		}
		r := b.out(dst, bc.KInt)
		if check.IsSized(t) {
			b.emit(op("NEGN"), r, x, check.IntCode(t))
			return r
		}
		b.emit(op("NEGI"), r, x)
		return r
	case syntax.AWAIT:
		xt := b.rawType(e.X)
		if tk, ok := xt.(*check.Task); ok {
			f := b.value(e.X, -1)
			k := b.l.kind(tk.Elem)
			r := bc.None
			if k != bc.KVoid {
				r = b.out(dst, k)
			} else {
				r = b.temp(bc.KRef)
			}
			b.emit(op("AWAIT"), r, f)
			if k == bc.KVoid {
				return -1
			}
			return r
		}
		return b.value(e.X, dst)
	case syntax.TRY:
		if e.Optional {
			return b.tryOptional(e, dst)
		}
		return b.value(e.X, dst)
	case syntax.LOAD:
		b.l.errorf(e, diag.LoadOutsideComponent, "load is a component's let")
		return b.valueReg(-1, bc.KRef)
	case syntax.RELOAD:
		return b.reload(e, dst)
	}
	return b.valueReg(-1, bc.KRef)
}

func (b *fb) binary(e *syntax.BinaryExpr, dst int) int {
	if o := b.l.info.Operators[e]; o != nil {
		return b.operator(e, o, dst)
	}
	if f := b.l.fold(e); f.known {
		b.effects(f)
		return b.constBool(f.val, dst)
	}
	if b.l.info.LengthScales[e] {
		// The Length's value scaled, its unit kept.
		l, n := e.X, e.Y
		if b.typ(b.l.info.Types[e.X]) != check.Length {
			l, n = e.Y, e.X
		}
		lv := b.value(l, -1)
		nv := b.value(n, -1)
		if b.f.Kinds[nv] != bc.KFloat {
			f := b.temp(bc.KFloat)
			b.emit(op("I2F"), f, nv)
			nv = f
		}
		return b.copyTo(b.callB("len.scale", bc.KInt, lv, nv), dst)
	}
	if b.l.info.DurationFloats[e] {
		return b.durationFloat(e, dst)
	}
	switch e.Op {
	case syntax.LAND, syntax.LOR:
		// A side @platform decides that doesn't decide the whole: the
		// whole is the other side (true && x, x || false).
		if f := b.l.fold(e.X); f.known {
			b.effects(f)
			return b.value(e.Y, dst)
		}
		if f := b.l.fold(e.Y); f.known && len(f.effects) == 0 {
			return b.value(e.X, dst)
		}
		r := b.out(dst, bc.KBool)
		end := b.f.NewLabel()
		b.value(e.X, r)
		if e.Op == syntax.LAND {
			b.emitJump(op("JF"), r, end)
		} else {
			b.emitJump(op("JT"), r, end)
		}
		b.value(e.Y, r)
		b.f.Place(end)
		return r
	case syntax.COALESCE:
		res := b.rawType(e)
		xt := b.rawType(e.X)
		if check.IsOptional(res) {
			x := b.value(e.X, -1)
			r := b.out(dst, bc.KRef)
			end := b.f.NewLabel()
			b.move(r, x)
			b.emitJump(op("JSOME"), r, end)
			b.value(e.Y, r)
			b.f.Place(end)
			return r
		}
		k := b.l.kind(res)
		other, end := b.f.NewLabel(), b.f.NewLabel()
		var r int
		if pr, ok := b.pathMapRead(e.X, k, dst, other); ok {
			r = pr
		} else if m, key, ok := b.mapRead(e.X); ok {
			// m[k] ?? d: the value read as it is, no optional made.
			r = b.out(dst, k)
			b.emit(op("MGETJ"), r, m, key, int(other))
		} else {
			x := b.value(e.X, -1)
			r = b.out(dst, k)
			b.emitJump(op("JNONE"), x, other)
			b.unbox(x, check.Unopt(xt), r)
		}
		b.jmp(end)
		b.f.Place(other)
		b.value(e.Y, r)
		b.f.Place(end)
		return r
	case syntax.RANGE, syntax.RANGELT:
		from := b.value(e.X, -1)
		to := b.value(e.Y, -1)
		if a, ok := b.rawType(e).(*check.Array); ok && a.Elem == check.U64 {
			// A u64's: ordered unsigned.
			return b.copyTo(b.callB("arr.range", bc.KRef, from, to, b.konst(map[bool]int64{true: 1}[e.Op == syntax.RANGE]), b.konst(1)), dst)
		}
		if e.Op == syntax.RANGE {
			// a..b takes b, Int.max too: no b + 1.
			return b.copyTo(b.callB("arr.range", bc.KRef, from, to, b.konst(1)), dst)
		}
		return b.copyTo(b.callB("arr.range", bc.KRef, from, to), dst)
	}
	if parts := b.stringChain(e); len(parts) > 2 {
		// a + b + c: one string made of them all, not one a +.
		if len(parts) <= literalChunk {
			// Each computed into its place in INTERP's block.
			ks := make([]bc.Kind, len(parts))
			for i := range ks {
				ks[i] = bc.KRef
			}
			blk := b.regs(ks)
			for i, p := range parts {
				b.move(blk+i, b.chainPart(p, blk+i))
			}
			r := b.out(dst, bc.KRef)
			b.emit(op("INTERP"), r, blk, len(parts), b.l.str("")) // no kinds: all strings
			b.releaseRegs(blk, ks)
			return r
		}
		vals := make([]int, len(parts))
		for i, p := range parts {
			vals[i] = b.chainPart(p, -1)
		}
		return b.joinStrings(vals, dst)
	}
	xt, yt := b.typeOf(e.X), b.typeOf(e.Y)
	x := b.value(e.X, -1)
	y := b.value(e.Y, -1)
	return b.binop(e.Op, x, y, xt, yt, b.rawType(e), dst)
}

// stringChain is the operands of a chain of string +s (a + b + c, as
// it's grouped: (a + b) + c, or with any parentheses), in order; nil
// when x isn't a string +.
func (b *fb) stringChain(x syntax.Expr) []syntax.Expr {
	e, ok := x.(*syntax.BinaryExpr)
	if !ok || e.Op != syntax.ADD || b.l.info.Operators[e] != nil || b.rawType(e) != check.String {
		return nil
	}
	var parts []syntax.Expr
	var walk func(x syntax.Expr)
	walk = func(x syntax.Expr) {
		for {
			p, ok := x.(*syntax.ParenExpr)
			if !ok {
				break
			}
			x = p.X
		}
		if e, ok := x.(*syntax.BinaryExpr); ok && e.Op == syntax.ADD && b.l.info.Operators[e] == nil && b.rawType(e) == check.String {
			walk(e.X)
			walk(e.Y)
			return
		}
		parts = append(parts, x)
	}
	walk(e)
	return parts
}

// chainPart is an operand of a string + chain as a string: a char's
// text (its one code point), as binop's + makes it, anything else (a
// string) as it is.
func (b *fb) chainPart(x syntax.Expr, dst int) int {
	if b.typeOf(x) == check.Char {
		// Into an int temp of its own, not dst (a string's register).
		return b.describe(b.value(x, -1), check.Char)
	}
	return b.value(x, dst)
}

// joinStrings is the strings in vals, one after another, in one string
// (an INTERP of them: one allocation, literalChunk of them at a time).
func (b *fb) joinStrings(vals []int, dst int) int {
	acc := -1
	for len(vals) > 0 {
		n := min(len(vals), literalChunk)
		ks := make([]bc.Kind, n)
		for i := range ks {
			ks[i] = bc.KRef
		}
		blk := b.regs(ks)
		for i, v := range vals[:n] {
			b.move(blk+i, v)
		}
		r := b.temp(bc.KRef)
		b.emit(op("INTERP"), r, blk, n, b.l.str("")) // no kinds: all strings
		b.releaseRegs(blk, ks)
		vals = vals[n:]
		if acc >= 0 {
			t := b.temp(bc.KRef)
			b.emit(op("CONCAT"), t, acc, r)
			r = t
		}
		acc = r
	}
	return b.copyTo(acc, dst)
}

// mentions reports whether x reads the var v anywhere in it.
func (b *fb) mentions(x syntax.Node, v *check.Var) bool {
	found := false
	syntax.Inspect(x, func(n syntax.Node) bool {
		if id, ok := n.(*syntax.Ident); ok && b.l.info.Uses[id] == v {
			found = true
		}
		return !found
	})
	return found
}

// intOps are the int operators' instructions.
var intOps = map[syntax.Token]string{
	syntax.ADD: "ADDI", syntax.SUB: "SUBI", syntax.MUL: "MULI", syntax.QUO: "DIVI", syntax.REM: "MODI",
	syntax.AMP: "BAND", syntax.PIPE: "BOR", syntax.CARET: "BXOR", syntax.SHL: "SHL", syntax.SHR: "SHR",
	syntax.WADD: "WADDI", syntax.WSUB: "WSUBI", syntax.WMUL: "WMULI", syntax.POW: "POWI",
}

// binop applies a binary operator to registers x and y.
func (b *fb) binop(o syntax.Token, x, y int, xt, yt, res check.Type, dst int) int {
	switch o {
	case syntax.EQL, syntax.NEQ:
		if check.Identical(xt, yt) {
			if r, ok := b.ownEquals(x, y, xt, o == syntax.NEQ, dst); ok {
				return r
			}
		}
		r := b.out(dst, bc.KBool)
		if xt == check.NoneType || yt == check.NoneType {
			v := x
			if xt == check.NoneType {
				v = y
			}
			b.emit(op("ISNONE"), r, v)
			if o == syntax.NEQ {
				b.emit(op("NOT"), r, r)
			}
			return r
		}
		k := b.f.Kinds[x]
		switch k {
		case bc.KInt, bc.KBool:
			b.emit(op(map[bool]string{true: "EQI", false: "NEI"}[o == syntax.EQL]), r, x, y)
		case bc.KFloat:
			b.emit(op(map[bool]string{true: "EQF", false: "NEF"}[o == syntax.EQL]), r, x, y)
		default:
			if b.f.Kinds[y] != bc.KRef {
				// T? == T: box the T.
				y = b.toOptional(y, yt, check.MakeOptional(yt), -1)
			}
			b.emit(op(map[bool]string{true: "EQ", false: "NE"}[o == syntax.EQL]), r, x, y, int(bc.KRef))
		}
		return r
	case syntax.LSS, syntax.LEQ, syntax.GTR, syntax.GEQ:
		r := b.out(dst, bc.KBool)
		if o == syntax.GTR || o == syntax.GEQ {
			x, y = y, x
			if o == syntax.GTR {
				o = syntax.LSS
			} else {
				o = syntax.LEQ
			}
		}
		if n, ok := xt.(*check.Named); ok {
			if f := check.CompareMethod(n); f != nil {
				// A generic T's: its compare's result against 0.
				c := b.invoke(b.l.funcIndex(f, nil, n), []int{x, y}, bc.KInt, -1, false)
				x, y = c, b.konst(0)
			} else if n.Obj.RefEnum() {
				// An enum without payloads stored as a reference: by its
				// cases' order.
				cx, cy := b.temp(bc.KInt), b.temp(bc.KInt)
				b.emit(op("ECASE"), cx, x)
				b.emit(op("ECASE"), cy, y)
				x, y = cx, cy
			}
		}
		switch b.f.Kinds[x] {
		case bc.KFloat:
			b.emit(op(map[bool]string{true: "LTF", false: "LEF"}[o == syntax.LSS]), r, x, y)
		case bc.KInt:
			if xt == check.U64 {
				b.emit(op(map[bool]string{true: "LTU", false: "LEU"}[o == syntax.LSS]), r, x, y)
				break
			}
			b.emit(op(map[bool]string{true: "LTI", false: "LEI"}[o == syntax.LSS]), r, x, y)
		case bc.KRef:
			if o == syntax.LSS {
				b.emit(op("LTS"), r, x, y)
			} else {
				b.emit(op("LTS"), r, y, x)
				b.emit(op("NOT"), r, r)
			}
		default:
			b.emit(op(map[bool]string{true: "LTI", false: "LEI"}[o == syntax.LSS]), r, x, y)
		}
		return r
	}
	k := b.l.kind(res)
	switch {
	case res == check.String:
		if xt == check.Char {
			x = b.describe(x, xt) // its one code point
		}
		if yt == check.Char {
			y = b.describe(y, yt)
		}
		r := b.out(dst, bc.KRef)
		b.emit(op("CONCAT"), r, x, y)
		return r
	case k == bc.KRef:
		return b.copyTo(b.callB("arr.concat", bc.KRef, x, y), dst)
	case k == bc.KFloat:
		if o == syntax.POW && b.f.Kinds[y] == bc.KInt {
			// float ** n: n as a float.
			f := b.temp(bc.KFloat)
			b.emit(op(map[bool]string{true: "U2F", false: "I2F"}[yt == check.U64]), f, y)
			y = f
		}
		r := b.out(dst, bc.KFloat)
		b.emit(op(map[syntax.Token]string{syntax.ADD: "ADDF", syntax.SUB: "SUBF", syntax.MUL: "MULF", syntax.QUO: "DIVF", syntax.REM: "MODF", syntax.POW: "POWF"}[o]), r, x, y)
		return r
	}
	r := b.out(dst, bc.KInt)
	if check.IsSized(res) || check.IsSized(yt) && (o == syntax.SHL || o == syntax.SHR || o == syntax.POW) {
		b.sizedOp(o, r, x, y, res, yt)
		return r
	}
	b.emit(op(intOps[o]), r, x, y)
	return r
}

// sizedOp is a sized integer type's operator, its result of type t
// (the operands' type; a shift's or a power's right one is ct): the N
// instructions, which take the type's code; the bitwise ones are the
// int's, the wrapping ones the int's wrapped to its width.
func (b *fb) sizedOp(o syntax.Token, r, x, y int, t, ct check.Type) {
	code := check.IntCode(t)
	switch o {
	case syntax.ADD, syntax.SUB, syntax.MUL, syntax.QUO, syntax.REM:
		name := map[syntax.Token]string{syntax.ADD: "ADDN", syntax.SUB: "SUBN", syntax.MUL: "MULN", syntax.QUO: "DIVN", syntax.REM: "MODN"}[o]
		b.emit(op(name), r, x, y, code)
	case syntax.SHL, syntax.SHR, syntax.POW:
		name := map[syntax.Token]string{syntax.SHL: "SHLN", syntax.SHR: "SHRN", syntax.POW: "POWN"}[o]
		b.emit(op(name), r, x, y, code|check.IntCode(ct)<<4)
	case syntax.WADD, syntax.WSUB, syntax.WMUL:
		b.emit(op(intOps[o]), r, x, y)
		if t != check.U64 {
			b.emit(op("WRAP"), r, r, code)
		}
	default: // & | ^: of two canonical values, canonical
		b.emit(op(intOps[o]), r, x, y)
	}
}

// literalChunk is how many of a literal's elements (an array's, a
// set's, a map's entries) are built from a block of registers at once;
// the rest are added one by one, so that a literal of any length takes
// as many registers as one of this.
const literalChunk = 256

// literalOut is the register a literal is built in: dst, or a temp of
// its own when elements are added after it's made (they may read dst).
func (b *fb) literalOut(dst int, adds bool) int {
	if adds {
		return b.temp(bc.KRef)
	}
	return b.out(dst, bc.KRef)
}

// literalDone puts the literal built in r in dst, if that's another
// register, letting r go: the value stays unique.
func (b *fb) literalDone(r, dst int) int {
	if dst < 0 || r == dst {
		return r
	}
	b.move(dst, r)
	b.emit(op("CLR"), r)
	return dst
}

// appendEach adds each of xs to the array or set in r, in place, with
// the inout builtin add (arr.push, set.insert): r is moved into its
// arguments' block and back, so the value stays unique.
func (b *fb) appendEach(r int, xs []syntax.Expr, add string) {
	for _, x := range xs {
		mark := b.mark()
		b.appendReg(r, b.value(x, -1), add)
		b.release(mark)
	}
}

// appendReg adds the value in v to the array or set in r, as appendEach
// does.
func (b *fb) appendReg(r, v int, add string) {
	ks := []bc.Kind{bc.KRef, b.f.Kinds[v]}
	blk := b.regs(ks)
	null := b.null()
	b.move(blk+1, v)
	b.emit(op("MOV"), blk, r)
	b.emit(op("MOV"), r, null)
	b.emit(op("CALLB"), bc.None, b.l.builtin(add), blk, 2)
	b.emit(op("MOV"), r, blk)
	b.emit(op("MOV"), blk, null)
	b.releaseRegs(blk, ks)
}

// mapRead is a map's index (m[k]) of a value that isn't optional, its
// map and key evaluated: what MGETJ reads without making an optional.
func (b *fb) mapRead(x syntax.Expr) (m, key int, ok bool) {
	ix, isIndex := unparen(x).(*syntax.IndexExpr)
	if !isIndex || ix.Default != nil {
		return 0, 0, false
	}
	mt, isMap := b.rawType(ix.X).(*check.Map)
	if !isMap || check.IsOptional(b.typ(mt.Elem)) {
		return 0, 0, false
	}
	return b.value(ix.X, -1), b.value(ix.Index, -1), true
}

// ---------------------------------------------------------------------
// if and match as values

// valueBlock lowers a block whose value, its last expression, goes in
// dst: computed before the block's defers run.
func (b *fb) valueBlock(bl *syntax.Block, dst int) {
	b.openScope()
	n := len(bl.Items)
	if n > 0 {
		last := bl.Items[n-1]
		switch {
		case b.l.info.ValueItems[last]:
			// An if or a match: the block's value is its.
			b.stmtsThen(bl.Items[:n-1], func() {
				b.at(last)
				b.cover(last)
				switch s := last.(type) {
				case *syntax.IfStmt:
					b.ifValue(s.Cond, s.Then, s.Else, dst)
				case *syntax.MatchStmt:
					b.matchValue(s, s.Subject, s.Arms, dst)
				}
			})
		case isExprStmt(last):
			es := last.(*syntax.ExprStmt)
			b.stmtsThen(bl.Items[:n-1], func() {
				b.at(es)
				b.cover(es)
				b.value(es.X, dst)
			})
		default:
			b.stmts(bl.Items)
		}
	}
	b.closeScope()
}

func isExprStmt(it syntax.Item) bool {
	_, ok := it.(*syntax.ExprStmt)
	return ok
}

func (b *fb) ifExpr(e *syntax.IfExpr, dst int) int {
	r := b.out(dst, b.kind(b.l.info.Types[e]))
	b.ifValue(e.Cond, e.Then, e.Else, r)
	return r
}

// ifValue lowers an if as a value into r: an IfExpr, or an IfStmt that's
// its block's value (els an *IfExpr, an *IfStmt or a *Block).
func (b *fb) ifValue(cond syntax.Expr, then *syntax.Block, els syntax.Node, r int) {
	elseValue := func() {
		switch el := els.(type) {
		case *syntax.IfExpr:
			b.value(el, r)
		case *syntax.IfStmt:
			b.ifValue(el.Cond, el.Then, el.Else, r)
		case *syntax.Block:
			b.valueBlock(el, r)
		}
	}
	if b.letNever(cond) {
		// A type test this instantiation never passes: the else.
		els0 := b.f.NewLabel()
		b.openScope()
		b.condJump(cond, els0)
		b.closeScope()
		b.f.Place(els0)
		elseValue()
		return
	}
	if f := b.l.foldCond(cond); f.known {
		// Only the branch @platform takes.
		b.effects(f)
		if f.val {
			b.openScope()
			b.valueBlock(then, r)
			b.closeScope()
			return
		}
		elseValue()
		return
	}
	b.openScope()
	els0, end := b.f.NewLabel(), b.f.NewLabel()
	b.condJump(cond, els0)
	b.valueBlock(then, r)
	b.closeScope()
	b.jmp(end)
	b.f.Place(els0)
	elseValue()
	b.f.Place(end)
}

func (b *fb) matchExpr(e *syntax.MatchExpr, dst int) int {
	r := b.out(dst, b.kind(b.l.info.Types[e]))
	b.matchValue(e, e.Subject, e.Arms, r)
	return r
}

// matchValue lowers a match as a value into r: a MatchExpr, or a
// MatchStmt that's its block's value (an arm's body may then be a
// statement that leaves).
func (b *fb) matchValue(n syntax.Node, subject syntax.Expr, arms []*syntax.MatchArm, r int) {
	subj := b.value(subject, -1)
	st := b.rawType(subject)
	end := b.f.NewLabel()
	for _, a := range b.matchArms(subject, arms) {
		next := b.f.NewLabel()
		b.openScope()
		b.testPattern(a.Pattern, subj, st, next)
		b.bindPattern(a.Pattern, subj, st)
		if a.Guard != nil {
			g := b.value(a.Guard, -1)
			b.emitJump(op("JF"), g, next)
		}
		switch body := a.Body.(type) {
		case *syntax.Block:
			b.valueBlock(body, r)
		case *syntax.ExprStmt:
			b.at(body)
			b.cover(body)
			b.value(body.X, r)
		case syntax.Expr:
			b.cover(body)
			b.value(body, r)
		case syntax.Item:
			b.stmt(body)
		}
		b.closeScope()
		b.jmp(end)
		b.f.Place(next)
	}
	b.throwMsg("no arm of the match matched (" + b.where(n) + ")")
	b.f.Place(end)
}
