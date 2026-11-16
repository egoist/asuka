package ir

import (
	"unicode"

	bc "github.com/egoist/asuka/lang/bytecode"
	"github.com/egoist/asuka/lang/check"
	"github.com/egoist/asuka/lang/prelude"
	"github.com/egoist/asuka/lang/syntax"
)

// The UI runtime's interaction, media, graphics and system parts
// (LOWERING.md's "Interaction states and variants" to "Media and the
// system"): refs, variants, animate, the prelude's runtime functions.

const (
	uiVariant     = "ui.variant"      // (view, state int, key int, value) -> void
	uiVariantBind = "ui.variant_bind" // (view, state, key, thunk) -> void
	uiAnimate     = "ui.animate"      // (Animation?, body closure) -> void
	uiAnimateWait = "ui.animate_wait" // (Animation?, body closure) -> future: settles once the animation is complete
	pathNew       = "path.new"        // () -> Path
)

// A Ref is a cell holding the view (none while there's none): CELL of
// none; reading its view is CGET, so a binding that reads a state
// through it follows the view coming and going.

// refNew makes a Ref: a cell of none.
func (b *fb) refNew(dst int) int {
	none := b.temp(bc.KRef)
	b.emit(op("KNONE"), none)
	r := b.out(dst, bc.KRef)
	b.emit(op("CELL"), r, none, int(bc.KRef))
	return r
}

// setRef puts the view v in the Ref cell, and takes it out when the
// owner building v is disposed (unless the Ref holds another view by
// then).
func (b *fb) setRef(cell, v int) {
	b.emit(op("CSET"), cell, v)
	cv, vv := &check.Var{Type: check.Ref}, &check.Var{Type: check.View}
	clos := b.makeClosure(closure{name: "ref", extra: []extraCap{{cv, cell}, {vv, v}}, ret: bc.KVoid, body: func(c *fb) {
		cur := c.temp(bc.KRef)
		c.emit(op("CPEEK"), cur, c.vars[cv])
		eq := c.temp(bc.KBool)
		c.emit(op("EQ"), eq, cur, c.vars[vv], int(bc.KRef))
		end := c.f.NewLabel()
		c.emitJump(op("JF"), eq, end)
		none := c.temp(bc.KRef)
		c.emit(op("KNONE"), none)
		c.emit(op("CSET"), c.vars[cv], none)
		c.f.Place(end)
		c.emit(op("RET0"))
	}})
	b.emit(op("CLEANUP"), clos)
}

// elementRef binds an element's ref: r and its as name, once v is made.
func (b *fb) elementRef(ec *check.ElementCall, v int) {
	if ec.As != nil {
		cell := b.local(bc.KRef)
		b.refNew(cell)
		b.vars[ec.As] = cell
		b.setRef(cell, v)
	}
	if ec.Ref != nil {
		mark := b.mark()
		b.setRef(b.value(ec.Ref, -1), v)
		b.release(mark)
	}
}

// refView is the view a Ref cell holds (none when none), read tracked.
func (b *fb) refView(cell int) int {
	v := b.temp(bc.KRef)
	b.emit(op("CGET"), v, cell)
	return v
}

// variants sets a container's variants: each prop as a prop set is,
// with the state.
func (b *fb) variants(v int, ec *check.ElementCall) {
	for _, va := range ec.Variants {
		b.variantStyle(v, va, prelude.StateID(va.State))
		state := b.konst(int64(prelude.StateID(va.State)))
		last := map[*check.Member]int{}
		for i, p := range va.Props {
			last[p.Member] = i
		}
		for i, p := range va.Props {
			if last[p.Member] != i {
				continue
			}
			mark := b.mark()
			key := prelude.KeyID(ec.Comp.Name(), p.Member.Name())
			k := b.konst(int64(key))
			if b.l.prog.Reactive(p.Value) {
				t := b.l.thunkType(p.Value, p.Member.Type)
				clos := b.thunk(b.l.info.Ctxs[p.Value], p.Value, t)
				b.callBTo(uiVariantBind, bc.None, v, state, k, clos)
			} else {
				val := b.value(p.Value, -1)
				b.callBTo(uiVariant, bc.None, v, state, k, val)
			}
			b.release(mark)
		}
	}
}

// refMethod is a Ref's method or property: the builtin called with the
// view the Ref holds, or nothing done (a property false) when it holds
// none. vals are the arguments, in the builtin's order.
func (b *fb) refMethod(m *check.Method, cell int, vals []int, res check.Type, dst int) int {
	k := b.l.kind(res)
	view := b.refView(cell)
	for _, c := range m.Consts {
		vals = append(vals, b.konst(c))
	}
	args := append([]int{view}, vals...)
	if m.Async {
		// The runtime fails the future of a view that isn't there.
		f := b.callB(m.Builtin, bc.KRef, args...)
		if k == bc.KVoid {
			b.emit(op("AWAIT"), b.temp(bc.KRef), f)
			return -1
		}
		r := b.out(dst, k)
		b.emit(op("AWAIT"), r, f)
		return r
	}
	end := b.f.NewLabel()
	if k == bc.KVoid {
		b.emitJump(op("JNONE"), view, end)
		b.callBTo(m.Builtin, bc.None, args...)
		b.f.Place(end)
		return -1
	}
	r := b.out(dst, k)
	b.move(r, b.zeroOf(res))
	b.emitJump(op("JNONE"), view, end)
	b.callBTo(m.Builtin, r, args...)
	b.f.Place(end)
	return r
}

// zeroOf is a register holding t's zero value: a tuple's or a struct's
// of its fields' zeros (a Ref's property of a view that isn't there),
// else its kind's.
func (b *fb) zeroOf(t check.Type) int {
	var elems []check.Type
	switch x := t.(type) {
	case *check.Tuple:
		elems = x.Elems
	case *check.Named:
		if x.Obj.Kind != check.StructType || x.Obj.IsError {
			return b.zero(b.kind(t))
		}
		s := x.Subst()
		for _, f := range x.Obj.Fields {
			elems = append(elems, s.Apply(f.Type))
		}
	default:
		return b.zero(b.kind(t))
	}
	ks := make([]bc.Kind, len(elems))
	for i, e := range elems {
		ks[i] = b.kind(e)
	}
	blk := b.regs(ks)
	for i, e := range elems {
		b.move(blk+i, b.zeroOf(e))
	}
	o := b.temp(bc.KRef)
	b.emit(op("NEWS"), o, b.l.shape(t), blk)
	b.releaseRegs(blk, ks)
	return o
}

// zero is a register holding kind k's zero value.
func (b *fb) zero(k bc.Kind) int {
	switch k {
	case bc.KFloat:
		r := b.temp(bc.KFloat)
		b.emit(op("KFLOAT"), r, b.l.floatConst(0))
		return r
	case bc.KRef:
		return b.valueReg(-1, bc.KRef)
	}
	r := b.temp(k)
	b.loadInt(r, 0)
	return r
}

// builtinField reads a built-in type's property: g.width, card.hovered.
func (b *fb) builtinField(sel *check.Selection, x int, res check.Type, dst int) int {
	m := sel.Method
	if m.Recv == "ref" {
		return b.refMethod(m, x, nil, res, dst)
	}
	args := []int{x}
	for _, c := range m.Consts {
		args = append(args, b.konst(c))
	}
	k := b.l.kind(res)
	r := b.out(dst, k)
	b.callBTo(m.Builtin, r, args...)
	return r
}

// runtimeCall calls a RuntimeFunc: its arguments in its parameters'
// order, a Ref as the view it holds.
func (b *fb) runtimeCall(call *check.Call, dst int) int {
	f := call.Builtin.Fn
	vals := b.args(call, nil)
	if i := f.Follow - 1; i >= 0 && i < len(call.Args) && call.Args[i] != nil && b.l.prog.Reactive(call.Args[i]) {
		// Followed: a thunk of it.
		x := call.Args[i]
		vals[i] = b.thunk(b.l.info.Ctxs[x], x, b.l.thunkType(x, call.Params[i].Type))
	}
	skip := b.f.NewLabel()
	for i, p := range call.Params {
		if check.Unopt(p.Type) != check.Ref {
			continue
		}
		view := b.temp(bc.KRef)
		if check.IsOptional(p.Type) {
			b.emit(op("KNONE"), view)
			l := b.f.NewLabel()
			b.emitJump(op("JNONE"), vals[i], l)
			b.emit(op("CGET"), view, vals[i])
			b.f.Place(l)
		} else {
			b.emit(op("CGET"), view, vals[i])
		}
		if f.NeedsView {
			b.emitJump(op("JNONE"), view, skip)
		}
		vals[i] = view
	}
	k := b.l.kind(call.Result)
	r := -1
	switch {
	case f.Async:
		fut := b.callB(f.Builtin, bc.KRef, vals...)
		if k == bc.KVoid {
			b.emit(op("AWAIT"), b.temp(bc.KRef), fut)
		} else {
			r = b.out(dst, k)
			b.emit(op("AWAIT"), r, fut)
		}
	case k == bc.KVoid:
		b.callBTo(f.Builtin, bc.None, vals...)
	default:
		r = b.out(dst, k)
		b.callBTo(f.Builtin, r, vals...)
	}
	b.f.Place(skip)
	return r
}

// animateCall is animate(anim) { body }: the body runs at once, its
// changes animating; awaited, the call returns once they're complete.
func (b *fb) animateCall(call *check.Call) int {
	anim := b.valueReg(-1, bc.KRef)
	if call.Args[0] != nil {
		anim = b.value(call.Args[0], -1)
	}
	body := b.value(call.Args[1], -1)
	if call.Awaited {
		f := b.callB(uiAnimateWait, bc.KRef, anim, body)
		b.emit(op("AWAIT"), b.temp(bc.KRef), f)
		return -1
	}
	b.callBTo(uiAnimate, bc.None, anim, body)
	return -1
}

// shaderProgram is shader("""…"""): the program compiled with the app,
// as bytes.
func (b *fb) shaderProgram(e *syntax.CallExpr, dst int) int {
	code := b.l.info.Shaders[e]
	s := b.kstr(string(code))
	return b.copyTo(b.callB("str.to_bytes", bc.KRef, s), dst)
}

// bodyCalls lowers a body's registrations (useKeys(…), onOpenURL(…)):
// statements of its function, before its views.
func (b *fb) bodyCalls(items []syntax.Item) []syntax.Item {
	var rest []syntax.Item
	for _, it := range items {
		if b.l.info.BodyCalls[it] {
			mark := b.mark()
			b.at(it)
			if s, ok := it.(*syntax.SlotItem); ok {
				b.exprStmt(b.l.info.SlotCalls[s])
			} else {
				b.exprStmt(it.(*syntax.ExprStmt).X)
			}
			b.release(mark)
			continue
		}
		rest = append(rest, it)
	}
	return rest
}

// appHandlers registers the app block's events' handlers.
func (b *fb) appHandlers() {
	for _, h := range b.l.info.AppHandlers {
		mark := b.mark()
		name := ""
		for _, a := range check.AppEvents {
			if a.Name == h.Event.Name() {
				name = a.Builtin
			}
		}
		clos := b.handler(h.Handler, h.Event)
		b.callBTo(name, bc.None, clos)
		b.release(mark)
	}
}

// Native components (LOWERING.md's "Native components"): a package's
// native component X is one native view, of the kind "<class>#X" (the
// class its platforms' code registers the package under: check.Package.
// NativeClass). Its handlers are set first, then its props (its header
// parameters', then the rest: the caller's values, else their defaults),
// each by its exported name (Selected, Changed: internal/nativegen's),
// its value encoded by its type's descriptor; then the style.

const (
	uiNative     = "ui.native"      // (type string) -> view
	uiNativeSet  = "ui.native_set"  // (view, name, value, desc) -> void
	uiNativeBind = "ui.native_bind" // (view, name, thunk, desc) -> void
	uiNativeOn   = "ui.native_on"   // (view, name, handler, desc) -> void: desc its parameters', in order
)

func (b *fb) nativeView(ec *check.ElementCall) int {
	t := ec.Comp
	comp := t.Comp
	v := b.local(bc.KRef)
	b.callBTo(uiNative, v, b.kstr(t.Pkg.NativeClass+"#"+t.Name()))
	b.elementRef(ec, v)
	set := func(m *check.Member, x syntax.Expr) {
		mark := b.mark()
		defer b.release(mark)
		name, desc := b.kstr(nativeName(m.Name())), b.kstr(b.l.nativeDesc(b.typ(m.Type)))
		if b.l.prog.Reactive(x) {
			clos := b.thunk(b.l.info.Ctxs[x], x, m.Type)
			b.callBTo(uiNativeBind, bc.None, v, name, clos, desc)
			return
		}
		b.callBTo(uiNativeSet, bc.None, v, name, b.value(x, -1), desc)
	}
	// bind name: target: the prop follows target, and the prop's change
	// event (<prop>Change, the checker's) writes it, before the app's
	// handlers of it run.
	bound := map[*check.Member]bool{}
	for _, bd := range ec.Binds {
		mark := b.mark()
		bound[bd.Prop] = true
		clos := b.thunk(b.l.info.Ctxs[bd.Item], bd.Target, bd.Prop.Type)
		b.callBTo(uiNativeBind, bc.None, v, b.kstr(nativeName(bd.Prop.Name())), clos, b.kstr(b.l.nativeDesc(b.typ(bd.Prop.Type))))
		if bd.Event != nil {
			desc := ""
			for _, p := range bd.Event.Params {
				desc += b.l.nativeDesc(b.typ(p.Type))
			}
			h := b.bindWriter(bd)
			b.callBTo(uiNativeOn, bc.None, v, b.kstr(nativeName(bd.Event.Name())), h, b.kstr(desc))
		}
		b.release(mark)
	}
	for i, m := range comp.Params {
		if a := ec.Args[i]; a != nil {
			set(m, a)
		} else if m.Default != nil {
			set(m, m.Default)
		}
	}
	// The app's handlers, after the binds' writes (a handler of a bound
	// prop's change sees the state written).
	for _, h := range ec.Handlers {
		mark := b.mark()
		desc := ""
		for _, p := range h.Event.Params {
			desc += b.l.nativeDesc(b.typ(p.Type))
		}
		clos := b.handler(h.Handler, h.Event)
		b.callBTo(uiNativeOn, bc.None, v, b.kstr(nativeName(h.Event.Name())), clos, b.kstr(desc))
		b.release(mark)
	}
	var style []*check.PropArg
	for _, m := range comp.Props {
		if bound[m] {
			continue
		}
		var val syntax.Expr
		for _, p := range ec.Props {
			if p.Member == m {
				val = p.Value
			}
		}
		if val == nil {
			val = m.Default
		}
		if val != nil {
			set(m, val)
		}
	}
	for _, p := range ec.Props {
		if p.Member.Owner != t {
			style = append(style, p)
		}
	}
	// Its style: list, under the block's style props.
	b.applyStyle(ec.Style, styleTarget{v: v, block: blockKeys("", style, true)})
	// The style, a later set of a key winning.
	last := map[*check.Member]int{}
	for i, p := range style {
		last[p.Member] = i
	}
	for i, p := range style {
		if last[p.Member] == i {
			b.setProp(v, t.Name(), p.Member, p.Value)
		}
	}
	return v
}

// nativeName is a member's name on the native wire: internal/nativegen's
// exported name (selected → Selected, is_on → IsOn), which the
// platforms' generated code decodes props and sends events by.
func nativeName(s string) string {
	var out []rune
	up := true
	for _, c := range s {
		if !unicode.IsLetter(c) && !unicode.IsDigit(c) {
			up = true
			continue
		}
		if up {
			out = append(out, unicode.ToUpper(c))
			up = false
		} else {
			out = append(out, c)
		}
	}
	return string(out)
}
