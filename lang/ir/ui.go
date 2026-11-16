package ir

import (
	"fmt"

	bc "github.com/egoist/asuka/lang/bytecode"
	"github.com/egoist/asuka/lang/check"
	"github.com/egoist/asuka/lang/prelude"
	"github.com/egoist/asuka/lang/syntax"
)

// ---------------------------------------------------------------------
// Functions and methods

// lowerFunc lowers a declared function or method (recv: the receiver's
// type, for a struct's or an enum's).
func (b *fb) lowerFunc(f *check.Function, recv check.Type) {
	ctx := f.Ctx
	if ctx == nil {
		b.emit(op("RET0"))
		return
	}
	b.ctx = ctx
	b.at(f.Decl)
	if f.Recv != nil {
		switch f.Recv.Kind {
		case check.StructType, check.EnumType:
			r := b.newReg(b.l.kind(recv))
			if ctx.Self != nil {
				b.vars[ctx.Self] = r
			}
		default:
			b.envReg = b.newReg(bc.KRef)
			b.comp = f.Recv
		}
	}
	var params []int
	for _, p := range f.Sig.Params {
		params = append(params, b.newReg(b.kind(p.Type)))
	}
	b.openScope()
	for i, p := range f.Sig.Params {
		if p.Var == nil {
			continue
		}
		if p.Var.Boxed() {
			b.declare(p.Var, params[i])
		} else {
			b.vars[p.Var] = params[i]
		}
	}
	if f.Sig.Async || ctx.Awaits {
		b.f.Flags |= bc.FAsync
	}
	if f.Sig.Throws {
		b.f.Flags |= bc.FThrows
	}
	d := f.Decl
	if d.Native.IsValid() {
		b.nativeBody(f, params)
		b.closeScope()
		return
	}
	if f.Extern != nil {
		b.externBody(f, params)
		b.closeScope()
		return
	}
	if d.Value != nil {
		k := b.f.Ret
		if k == bc.KVoid {
			b.exprStmt(d.Value)
			b.emit(op("RET0"))
		} else {
			mark := b.mark()
			r := b.value(d.Value, -1)
			b.ret(r, k)
			b.release(mark)
		}
	} else {
		b.stmts(d.Body.Items)
		b.ret(-1, b.f.Ret)
	}
	b.closeScope()
}

// ---------------------------------------------------------------------
// Components and stores

// compFunc is the function of a component (or a store): its arguments
// are its header parameters, then its props, then its events' handlers.
// publishEnv makes t's env of the members made so far (the others zero)
// and puts it in the env's box, for the closures that read through it;
// whole, it's in a local, the env from then on.
func (b *fb) publishEnv(t *check.TypeName, whole bool) int {
	shape := b.l.envShapeOf(t, b.typ(t.Type))
	var ks []bc.Kind
	var regs []int
	for _, m := range t.Comp.Order {
		if m.Kind == check.FnMember {
			continue
		}
		k := b.l.memberKind(m)
		ks = append(ks, k)
		if r, ok := b.memberRegs[m]; ok {
			regs = append(regs, r)
		} else {
			regs = append(regs, b.zero(k))
		}
	}
	blk := b.regs(ks)
	for i, r := range regs {
		b.move(blk+i, r)
	}
	env := b.temp(bc.KRef)
	if whole {
		env = b.local(bc.KRef)
	}
	b.emit(op("NEWS"), env, shape, blk)
	b.releaseRegs(blk, ks)
	b.emit(op("VSET"), b.envBox, env)
	return env
}

// readsMemo reports whether x reads a memo of t's (a let that follows
// what it reads), which runs its thunk at once.
func (b *fb) readsMemo(x syntax.Expr, t *check.TypeName) bool {
	if x == nil {
		return false
	}
	found := false
	syntax.Inspect(x, func(n syntax.Node) bool {
		if id, ok := n.(*syntax.Ident); ok {
			if m, ok := b.l.info.Uses[id].(*check.Member); ok && m.Owner == t && m.Kind == check.LetMember && m.Memo {
				found = true
			}
		}
		return !found
	})
	return found
}

func (l *Lowerer) compFunc(t *check.TypeName, typ check.Type) int {
	key := fmt.Sprintf("comp|%p", t)
	name := t.Name()
	n, _ := typ.(*check.Named)
	if len(t.TypeParams) > 0 && n != nil {
		// A generic store: a function per instantiation.
		name = l.typeKey(n)
		key += "|" + name
	}
	if i, ok := l.funcs[key]; ok {
		return i
	}
	c := t.Comp
	np := len(c.Params) + len(c.Props) + len(c.Events)
	b := l.newFunc(name, l.fileOf(t.Decl), np, bc.KRef)
	if len(t.TypeParams) > 0 && n != nil {
		b.subst = n.Subst()
	}
	l.funcs[key] = b.idx
	l.work = append(l.work, func() { b.lowerComponent(t) })
	return b.idx
}

func (b *fb) lowerComponent(t *check.TypeName) {
	comp := t.Comp
	ctx := comp.Ctx
	b.ctx = ctx
	b.comp = t
	b.at(t.Decl)
	for _, list := range [][]*check.Member{comp.Params, comp.Props, comp.Events} {
		for _, m := range list {
			b.memberRegs[m] = b.newReg(bc.KRef)
		}
	}
	b.openScope()
	// The env's box: closures made before the env is whole capture it.
	none := b.temp(bc.KRef)
	b.emit(op("KNONE"), none)
	b.envBox = b.local(bc.KRef)
	b.emit(op("VBOX"), b.envBox, none, int(bc.KRef))
	for _, m := range comp.Order {
		mark := b.mark()
		b.at(m.Decl)
		if b.readsMemo(m.Default, t) {
			// A memo it reads is computed now, and its thunk reads what it
			// reads through the env's box: give it the env so far.
			b.publishEnv(t, false)
		}
		switch m.Kind {
		case check.ParamMember, check.PropMember:
			if m.Default == nil {
				break
			}
			r := b.memberRegs[m]
			skip := b.f.NewLabel()
			b.emitJump(op("JSOME"), r, skip)
			if m.IsSlot() {
				b.move(r, b.builder(m.Default))
			} else {
				b.move(r, b.accessor(m.Default, m.Type))
			}
			b.f.Place(skip)
		case check.StateMember:
			k := b.kind(m.Type)
			var init int
			if m.Default != nil {
				init = b.value(m.Default, -1)
			}
			init = b.valueReg(init, k)
			r := b.local(bc.KRef)
			b.emit(op("CELLK"), r, init, int(k), b.l.str(t.Name()+"."+m.Name()))
			b.memberRegs[m] = r
		case check.LetMember:
			switch {
			case isLoad(m.Default):
				r := b.local(bc.KRef)
				b.move(r, b.load(m))
				b.memberRegs[m] = r
			case m.Memo:
				clos := b.thunk(b.l.info.Ctxs[m.Default], m.Default, m.Type)
				r := b.local(bc.KRef)
				b.emit(op("MEMO"), r, clos, int(b.kind(m.Type)))
				b.memberRegs[m] = r
			default:
				r := b.local(b.kind(m.Type))
				b.value(m.Default, r)
				b.memberRegs[m] = r
			}
		case check.UseMember:
			r := b.local(bc.KRef)
			key := b.kstr(b.l.typeKey(b.typ(m.Type)))
			b.callBTo(uiUse, r, key)
			b.memberRegs[m] = r
		}
		b.release(mark)
	}
	b.envReg = b.publishEnv(t, true)
	// What it provides, its effects and tasks.
	for _, it := range comp.Body.Items {
		mark := b.mark()
		b.at(it)
		switch d := it.(type) {
		case *syntax.ProvideDecl:
			v := b.value(d.Value, -1)
			key := b.kstr(b.l.typeKey(b.rawType(d.Value)))
			b.callBTo(uiProvide, bc.None, key, v)
		case *syntax.EffectDecl:
			ectx := b.l.info.Ctxs[d]
			clos := b.makeClosure(closure{name: "effect", ctxs: []*check.FuncCtx{ectx}, ret: bc.KVoid, throws: true, body: func(c *fb) {
				c.ctx = ectx
				c.block(d.Body)
				c.emit(op("RET0"))
			}})
			b.emit(op("EFFECT"), clos)
		case *syntax.TaskDecl:
			b.taskDecl(d, comp.Store)
		}
		b.release(mark)
	}
	if comp.Store {
		b.emit(op("RET"), b.envReg)
		b.closeScopeNoClear()
		return
	}
	var views []syntax.Item
	for _, it := range comp.Body.Items {
		switch it.(type) {
		case *syntax.PropDecl, *syntax.EventDecl, *syntax.StateDecl, *syntax.LetDecl, *syntax.UseDecl,
			*syntax.FuncDecl, *syntax.ProvideDecl, *syntax.TaskDecl, *syntax.EffectDecl, *syntax.BadItem:
		default:
			views = append(views, it)
		}
	}
	views = b.bodyCalls(views)
	var v int
	if call := b.l.info.ScreenRoots[t]; call != nil {
		v = b.element(call)
	} else {
		v = b.viewsOf(views)
	}
	r := b.newReg(bc.KRef)
	b.move(r, v)
	b.emit(op("RET"), r)
	b.closeScopeNoClear()
}

// closeScopeNoClear ends a scope whose code returned already.
func (b *fb) closeScopeNoClear() {
	n := len(b.scopes)
	for _, r := range b.scopes[n-1] {
		k := b.f.Kinds[r]
		b.free[k] = append(b.free[k], r)
	}
	b.scopes = b.scopes[:n-1]
}

// taskDecl starts a task: once (a component's starts when it mounts),
// or, with dependencies, an effect that restarts it when they change.
func (b *fb) taskDecl(d *syntax.TaskDecl, store bool) {
	bodyCtx := b.l.info.Ctxs[d.Body]
	body := func(c *fb) int {
		return c.makeClosure(closure{name: "task", ctxs: []*check.FuncCtx{bodyCtx}, ret: bc.KVoid, throws: true, body: func(c2 *fb) {
			c2.ctx = bodyCtx
			c2.block(d.Body)
			c2.emit(op("RET0"))
			if bodyCtx.Awaits {
				c2.f.Flags |= bc.FAsync
			}
		}})
	}
	if len(d.Deps) == 0 {
		clos := body(b)
		if store {
			f := b.temp(bc.KRef)
			b.emit(op("TASK"), f, clos)
		} else {
			b.callBTo(uiTask, bc.None, clos)
		}
		return
	}
	depsCtx := b.l.info.Ctxs[d]
	clos := b.makeClosure(closure{name: "task deps", ctxs: []*check.FuncCtx{depsCtx, bodyCtx}, ret: bc.KVoid, throws: true, body: func(c *fb) {
		c.ctx = depsCtx
		for _, x := range d.Deps {
			c.value(x, -1)
		}
		t := body(c)
		f := c.temp(bc.KRef)
		c.emit(op("TASK"), f, t)
		c.emit(op("RET0"))
	}})
	b.emit(op("EFFECT"), clos)
}

// load makes a component's let x = load f(…): the runtime's loader of a
// thunk that starts the call as a task.
func (b *fb) load(m *check.Member) int {
	u := m.Default.(*syntax.UnaryExpr)
	ctx := b.l.info.Ctxs[m.Default]
	inner := b.typ(b.l.info.Types[u.X])
	thunk := b.makeClosure(closure{name: "load", ctxs: []*check.FuncCtx{ctx}, ret: bc.KRef, throws: true, body: func(c *fb) {
		c.ctx = ctx
		f := c.taskOf(u.X, -1, inner, false)
		c.emit(op("RET"), f)
	}})
	return b.callB(uiLoad, bc.KRef, thunk)
}

// accessor passes e to a component or store: a value when it reads
// nothing reactive; the cell, memo or accessor it reads alone; or a
// thunk.
func (b *fb) accessor(e syntax.Expr, t check.Type) int {
	k := b.kind(t)
	r := b.temp(bc.KRef)
	if !b.l.prog.Reactive(e) {
		v := b.value(e, -1)
		b.emit(op("ACCV"), r, b.valueReg(v, k), int(k))
		return r
	}
	if ref := b.readsAlone(e); ref >= 0 {
		b.emit(op("ACCC"), r, ref)
		return r
	}
	ctx := b.l.info.Ctxs[e]
	clos := b.thunk(ctx, e, t)
	b.emit(op("ACCF"), r, clos, int(k))
	return r
}

// readsAlone is the cell, memo or accessor e reads when it's that read
// alone (count, a prop, a row's item), or -1.
func (b *fb) readsAlone(e syntax.Expr) int {
	if _, conv := b.l.info.Convs[e]; conv {
		return -1
	}
	id, ok := unparen(e).(*syntax.Ident)
	if !ok {
		return -1
	}
	switch o := b.l.info.Uses[id].(type) {
	case *check.Member:
		switch o.Kind {
		case check.StateMember, check.ParamMember, check.PropMember:
			if !o.IsSlot() {
				return b.memberRef(o, e)
			}
		case check.LetMember:
			if o.Memo && !isLoad(o.Default) {
				return b.memberRef(o, e)
			}
		}
	case *check.Global:
		if o.Kind == check.StateGlobal || o.Kind == check.MemoGlobal {
			return b.globalCell(o)
		}
	case *check.Var:
		if (o.Kind == check.RowItem || o.Kind == check.ViewMemo) && !o.Boxed() {
			if r, ok := b.vars[o]; ok {
				return r
			}
		}
	}
	return -1
}

func (b *fb) storeCall(e *syntax.CallExpr, call *check.Call, dst int) int {
	t := call.Type.Obj
	idx := b.l.compFunc(t, b.typ(call.Type))
	var vals []int
	for i, a := range call.Args {
		if a == nil {
			r := b.temp(bc.KRef)
			b.emit(op("KNONE"), r)
			vals = append(vals, r)
			continue
		}
		vals = append(vals, b.accessor(a, call.Params[i].Type))
	}
	return b.invoke(idx, vals, bc.KRef, dst, false)
}

// ---------------------------------------------------------------------
// The app

func (b *fb) lowerApp(d *syntax.AppDecl) {
	b.at(d)
	b.openScope()
	var views []syntax.Item
	for _, it := range d.Body.Items {
		if p, ok := it.(*syntax.ProvideDecl); ok {
			mark := b.mark()
			v := b.value(p.Value, -1)
			key := b.kstr(b.l.typeKey(b.rawType(p.Value)))
			b.callBTo(uiProvide, bc.None, key, v)
			b.release(mark)
			continue
		}
		if _, ok := it.(*syntax.OnHandler); ok {
			continue // the app's events: appHandlers
		}
		if _, ok := it.(*syntax.TaskDecl); ok {
			continue // after its handlers
		}
		if _, ok := it.(*syntax.EffectDecl); ok {
			continue
		}
		views = append(views, it)
	}
	b.appHandlers()
	// Its tasks and effects, while it runs.
	for _, it := range d.Body.Items {
		mark := b.mark()
		switch x := it.(type) {
		case *syntax.TaskDecl:
			b.at(x)
			b.taskDecl(x, false)
		case *syntax.EffectDecl:
			b.at(x)
			ectx := b.l.info.Ctxs[x]
			clos := b.makeClosure(closure{name: "effect", ctxs: []*check.FuncCtx{ectx}, ret: bc.KVoid, throws: true, body: func(c *fb) {
				c.ctx = ectx
				c.block(x.Body)
				c.emit(op("RET0"))
			}})
			b.emit(op("EFFECT"), clos)
		}
		b.release(mark)
	}
	views = b.bodyCalls(views)
	// The first Window among the app's views is its main window.
	for _, it := range views {
		if s, ok := it.(*syntax.ExprStmt); ok {
			if e, ok := unparen(s.X).(*syntax.CallExpr); ok {
				if ec := b.l.info.Elements[e]; ec != nil && ec.Comp.Comp.Native && ec.Comp.Pkg.Prelude && ec.Comp.Name() == "Window" {
					b.mainWindow = ec
					break
				}
			}
		}
	}
	v := b.viewsOf(views)
	r := b.newReg(bc.KRef)
	b.move(r, v)
	b.emit(op("RET"), r)
	b.closeScopeNoClear()
}

// ---------------------------------------------------------------------
// Views

// viewsOf builds view items: one view, or a fragment of them.
func (b *fb) viewsOf(items []syntax.Item) int {
	if len(items) == 1 {
		if v, opt := b.viewItem(items[0]); !opt {
			return v
		}
	}
	frag := b.callB(uiFragment, bc.KRef)
	for _, it := range items {
		v, opt := b.viewItem(it)
		if v < 0 {
			continue
		}
		skip := b.f.NewLabel()
		if opt {
			b.emitJump(op("JNONE"), v, skip)
		}
		b.callBTo(uiAdd, bc.None, frag, v)
		b.f.Place(skip)
	}
	return frag
}

// viewItem builds a view item; opt: the view may be none.
func (b *fb) viewItem(it syntax.Item) (int, bool) {
	b.at(it)
	switch s := it.(type) {
	case *syntax.ExprStmt:
		return b.viewExpr(s.X)
	case *syntax.IfStmt:
		if f := b.l.foldCond(s.Cond); f.known && len(f.effects) == 0 {
			return b.knownView(s, f.val), false
		}
		return b.showView(s), false
	case *syntax.ForStmt:
		return b.forView(s), false
	case *syntax.MatchStmt:
		return b.matchView(s), false
	}
	b.l.errorf(it, "can't lower this view item")
	return -1, false
}

// viewExpr builds a view-typed expression: an element, a component, a
// slot (its builder called).
func (b *fb) viewExpr(x syntax.Expr) (int, bool) {
	t := b.rawType(x)
	if id, ok := unparen(x).(*syntax.Ident); ok {
		if m, ok := b.l.info.Uses[id].(*check.Member); ok && m.IsSlot() {
			bl := b.memberRef(m, x)
			r := b.temp(bc.KRef)
			if check.IsOptional(m.Type) {
				skip := b.f.NewLabel()
				b.emit(op("KNONE"), r)
				b.emitJump(op("JNONE"), bl, skip)
				b.emit(op("CALLC"), r, bl, 0, 0)
				b.f.Place(skip)
				return r, true
			}
			b.emit(op("CALLC"), r, bl, 0, 0)
			return r, false
		}
	}
	return b.value(x, -1), check.IsOptional(t)
}

// builder is a closure building the view x, where it's placed.
func (b *fb) builder(x syntax.Expr) int {
	ctx := b.l.info.Ctxs[x]
	return b.makeClosure(closure{name: "builder", ctxs: []*check.FuncCtx{ctx}, ret: bc.KRef, throws: true, body: func(c *fb) {
		if ctx != nil {
			c.ctx = ctx
		}
		v, opt := c.viewExpr(x)
		if opt {
			skip := c.f.NewLabel()
			c.emitJump(op("JSOME"), v, skip)
			c.move(v, c.callB(uiFragment, bc.KRef))
			c.f.Place(skip)
		}
		c.emit(op("RET"), v)
	}})
}

// builderItems is a closure building view items.
func (b *fb) builderItems(ctx *check.FuncCtx, items []syntax.Item, extra ...*check.FuncCtx) int {
	ctxs := append([]*check.FuncCtx{ctx}, extra...)
	return b.makeClosure(closure{name: "builder", ctxs: ctxs, ret: bc.KRef, throws: true, body: func(c *fb) {
		if ctx != nil {
			c.ctx = ctx
		}
		v := c.viewsOf(items)
		c.emit(op("RET"), v)
	}})
}

// knownView builds the views of the branch of a view's if that @platform
// takes (val: its then's), once: the other branch's never exist.
func (b *fb) knownView(s *syntax.IfStmt, val bool) int {
	var build int
	switch e := s.Else.(type) {
	case nil:
		if !val {
			return b.callB(uiFragment, bc.KRef)
		}
	case *syntax.Block:
		if !val {
			build = b.builderItems(b.l.info.Ctxs[e], e.Items)
		}
	case *syntax.IfStmt:
		if !val {
			build = b.builderItems(b.l.info.Ctxs[e], []syntax.Item{e})
		}
	}
	if val {
		build = b.builderItems(b.l.info.Ctxs[s.Then], s.Then.Items)
	}
	r := b.temp(bc.KRef)
	b.emit(op("CALLC"), r, build, 0, 0)
	return r
}

func (b *fb) showView(s *syntax.IfStmt) int {
	var cond, then int
	if vc := b.l.info.ViewConds[s]; vc != nil {
		cond, then = b.showChain(s, vc)
	} else if lc, ok := s.Cond.(*syntax.LetCond); ok {
		condCtx := b.l.info.Ctxs[s.Cond]
		thenCtx := b.l.info.Ctxs[s.Then]
		cond = b.makeClosure(closure{name: "condition", ctxs: []*check.FuncCtx{condCtx}, ret: bc.KBool, throws: true, body: func(c *fb) {
			c.ctx = condCtx
			v := c.value(lc.Value, -1)
			r := c.temp(bc.KBool)
			c.emit(op("ISNONE"), r, v)
			c.emit(op("NOT"), r, r)
			c.emit(op("RET"), r)
		}})
		bp, _ := lc.Pattern.(*syntax.BindPat)
		var u *check.Var
		if bp != nil {
			u, _ = b.l.info.Defs[bp.Name].(*check.Var)
		}
		vt := check.Unopt(b.rawType(lc.Value))
		then = b.makeClosure(closure{name: "then", ctxs: []*check.FuncCtx{thenCtx, condCtx}, ret: bc.KRef, throws: true, body: func(c *fb) {
			c.ctx = thenCtx
			if u != nil {
				k := c.l.kind(vt)
				m := c.makeClosure(closure{name: "unwrap", ctxs: []*check.FuncCtx{condCtx}, ret: k, throws: true, body: func(c2 *fb) {
					c2.ctx = condCtx
					v := c2.value(lc.Value, -1)
					c2.ret(c2.unwrap(v, vt, -1, lc), k)
				}})
				r := c.newReg(bc.KRef)
				c.emit(op("MEMO"), r, m, int(k))
				c.vars[u] = r
			}
			v := c.viewsOf(s.Then.Items)
			c.emit(op("RET"), v)
		}})
	} else {
		cond = b.thunk(b.l.info.Ctxs[s.Cond], s.Cond, check.Bool)
		then = b.builderItems(b.l.info.Ctxs[s.Then], s.Then.Items)
	}
	var els int
	switch e := s.Else.(type) {
	case *syntax.Block:
		els = b.builderItems(b.l.info.Ctxs[e], e.Items)
	case *syntax.IfStmt:
		els = b.builderItems(b.l.info.Ctxs[e], []syntax.Item{e})
	default:
		els = b.temp(bc.KRef)
		b.emit(op("KNONE"), els)
	}
	return b.callB(uiShow, bc.KRef, cond, then, els)
}

func (b *fb) forView(s *syntax.ForStmt) int {
	items, key, row, kk := b.forParts(s)
	return b.callB(uiFor, bc.KRef, items, key, row, b.konst(int64(kk)))
}

// forParts makes a for's closures in a view: its items' thunk, its
// key's function of (item, index) (or none), its row's builder.
func (b *fb) forParts(s *syntax.ForStmt) (items, key, row int, keyKind bc.Kind) {
	fi := b.l.info.Fors[s]
	itemsCtx := b.l.info.Ctxs[s.Seq]
	items = b.makeClosure(closure{name: "items", ctxs: []*check.FuncCtx{itemsCtx}, ret: bc.KRef, throws: true, body: func(c *fb) {
		c.ctx = itemsCtx
		v := c.value(fi.Seq, -1)
		if fi.Kind == check.ForSet {
			v = c.callB("map.keys", bc.KRef, v) // the rows: its elements, in order
		}
		c.emit(op("RET"), v)
	}})
	if id, ok := s.Key.(*syntax.Ident); ok && fi.Kind == check.ForEnumerated && len(b.l.info.Ctxs[s.Key].Params) > 0 && b.l.info.Uses[id] == check.Object(b.l.info.Ctxs[s.Key].Params[0]) {
		// for (i, x) in xs.enumerated() key i: keyed by the index, as a
		// for without a key is.
		key = b.temp(bc.KRef)
		b.emit(op("KNONE"), key)
	} else if s.Key != nil {
		// The key's function takes (item, index): an enumerated for's
		// key may read either (its context's params are (index, item)).
		kctx := b.l.info.Ctxs[s.Key]
		var item, index *check.Var
		if len(kctx.Params) > 0 {
			item = kctx.Params[0]
			if fi.Kind == check.ForEnumerated && len(kctx.Params) > 1 {
				item, index = kctx.Params[1], kctx.Params[0]
			}
		}
		kt := kctx.Result
		kk := b.kind(kt)
		keyKind = kk
		key = b.makeClosure(closure{name: "key", ctxs: []*check.FuncCtx{kctx}, params: []*check.Var{item, index}, pkinds: []bc.Kind{b.kind(fi.Elem), bc.KInt}, ret: kk, throws: true, body: func(c *fb) {
			c.ctx = kctx
			c.ret(c.value(s.Key, -1), kk)
		}})
	} else {
		key = b.temp(bc.KRef)
		b.emit(op("KNONE"), key)
	}
	rctx := b.l.info.Ctxs[s.Body]
	params := make([]*check.Var, 2)
	copy(params, rctx.Params)
	row = b.makeClosure(closure{name: "row", ctxs: []*check.FuncCtx{rctx}, params: params, ret: bc.KRef, throws: true, body: func(c *fb) {
		c.ctx = rctx
		v := c.viewsOf(s.Body.Items)
		c.emit(op("RET"), v)
	}})
	return items, key, row, keyKind
}

func (b *fb) matchView(s *syntax.MatchStmt) int {
	subjCtx := b.l.info.Ctxs[s.Subject]
	st := b.rawType(s.Subject)
	arms := b.l.platformArms(s.Subject, s.Arms)
	if check.IsPlatformConst(s.Subject) && len(arms) <= 1 && (len(arms) == 0 || len(patternVars(arms[0].Pattern, b.l.info)) == 0) {
		// A match of @platform: the views of the arm it takes, once.
		if len(arms) == 0 {
			return b.callB(uiFragment, bc.KRef)
		}
		var items []syntax.Item
		switch body := arms[0].Body.(type) {
		case *syntax.Block:
			items = body.Items
		case syntax.Item:
			items = []syntax.Item{body}
		}
		r := b.temp(bc.KRef)
		b.emit(op("CALLC"), r, b.builderItems(b.l.info.Ctxs[arms[0]], items), 0, 0)
		return r
	}
	sel := b.makeClosure(closure{name: "selector", ctxs: []*check.FuncCtx{subjCtx}, ret: bc.KInt, throws: true, body: func(c *fb) {
		c.ctx = subjCtx
		v := c.value(s.Subject, -1)
		r := c.temp(bc.KInt)
		for i, a := range arms {
			next := c.f.NewLabel()
			c.testPattern(a.Pattern, v, st, next)
			c.loadInt(r, int64(i))
			c.emit(op("RET"), r)
			c.f.Place(next)
		}
		c.loadInt(r, -1)
		c.emit(op("RET"), r)
	}})
	ctxs := []*check.FuncCtx{subjCtx}
	for _, a := range arms {
		ctxs = append(ctxs, b.l.info.Ctxs[a])
	}
	arm := &check.Var{}
	build := b.makeClosure(closure{name: "arms", ctxs: ctxs, params: []*check.Var{arm}, pkinds: []bc.Kind{bc.KInt}, ret: bc.KRef, throws: true, body: func(c *fb) {
		c.ctx = subjCtx
		c.f.Kinds[0] = bc.KInt
		for i, a := range arms {
			next := c.f.NewLabel()
			want := c.konst(int64(i))
			eq := c.temp(bc.KBool)
			c.emit(op("EQI"), eq, 0, want)
			c.emitJump(op("JF"), eq, next)
			actx := b.l.info.Ctxs[a]
			c.ctx = actx
			// Each name the pattern binds: a memo of its part of the
			// subject.
			for _, v := range patternVars(a.Pattern, c.l.info) {
				v := v
				k := c.kind(v.Type)
				m := c.makeClosure(closure{name: "payload", ctxs: []*check.FuncCtx{subjCtx}, ret: k, throws: true, body: func(c2 *fb) {
					c2.ctx = subjCtx
					sv := c2.value(s.Subject, -1)
					c2.ret(c2.extract(a.Pattern, v, sv, st), k)
				}})
				r := c.newReg(bc.KRef)
				c.emit(op("MEMO"), r, m, int(k))
				c.vars[v] = r
			}
			var view int
			switch body := a.Body.(type) {
			case *syntax.Block:
				view = c.viewsOf(body.Items)
			case syntax.Item:
				view = c.viewsOf([]syntax.Item{body})
			}
			c.emit(op("RET"), view)
			c.f.Place(next)
		}
		c.emit(op("RET"), c.callB(uiFragment, bc.KRef))
	}})
	return b.callB(uiSwitch, bc.KRef, sel, build)
}

// patternVars lists the names a pattern binds.
func patternVars(p syntax.Pattern, info *check.Info) []*check.Var {
	var out []*check.Var
	syntax.Inspect(p, func(n syntax.Node) bool {
		if bp, ok := n.(*syntax.BindPat); ok {
			if v, ok := info.Defs[bp.Name].(*check.Var); ok {
				out = append(out, v)
			}
		}
		return true
	})
	return out
}

// extract is the part of v (which matched p) that p binds to target.
func (b *fb) extract(p syntax.Pattern, target *check.Var, v int, t check.Type) int {
	switch p := p.(type) {
	case *syntax.BindPat:
		if b.l.info.Defs[p.Name] == target {
			return v
		}
	case *syntax.TuplePat:
		tt, _ := t.(*check.Tuple)
		for i, e := range p.Elems {
			if tt == nil || !containsVar(e, target, b.l.info) {
				continue
			}
			r := b.temp(b.l.kind(tt.Elems[i]))
			b.emit(op("GETF"), r, v, i)
			return b.extract(e, target, r, tt.Elems[i])
		}
	case *syntax.CasePat:
		k := b.l.info.Cases[p]
		et := check.Unopt(t)
		x := v
		if check.IsOptional(t) {
			x = b.unbox(v, et, -1)
		}
		n, _ := et.(*check.Named)
		if fields, ok := b.l.info.FieldPats[p]; ok {
			for i, a := range p.Args {
				if fields[i] == nil || n == nil || !containsVar(a, target, b.l.info) {
					continue
				}
				r := b.getField(x, n, fields[i], -1)
				return b.extract(a, target, r, n.Subst().Apply(fields[i].Type))
			}
			return v
		}
		for i, a := range p.Args {
			if !containsVar(a, target, b.l.info) {
				continue
			}
			r := b.payload(x, n, k, i)
			return b.extract(a, target, r, b.payloadType(n, k, i))
		}
	}
	return v
}

func containsVar(p syntax.Pattern, v *check.Var, info *check.Info) bool {
	for _, x := range patternVars(p, info) {
		if x == v {
			return true
		}
	}
	return false
}

// ---------------------------------------------------------------------
// Elements and components used

// element builds an element's or a component's view.
func (b *fb) element(e *syntax.CallExpr) int {
	ec := b.l.info.Elements[e]
	if ec == nil {
		return b.valueReg(-1, bc.KRef)
	}
	if ec.Comp.Comp.Native {
		if p := ec.Comp.Pkg; p != nil && !p.Prelude {
			return b.nativeView(ec)
		}
		return b.builtinElement(ec)
	}
	return b.componentCall(ec)
}

func (b *fb) builtinElement(ec *check.ElementCall) int {
	name := ec.Comp.Name()
	el := prelude.ElementID(name)
	if el == 0 {
		b.l.errorf(ec.Comp.Decl, "%s isn't an element the runtime has", name)
		return b.valueReg(-1, bc.KRef)
	}
	v := b.local(bc.KRef)
	b.callBTo(uiNew, v, b.konst(int64(el)))
	if b.mainWindow != nil && b.mainWindow == ec {
		// The app block's Window: the main window, its keys the root's.
		b.callBTo(uiMainWindow, bc.None, v)
	}
	b.elementRef(ec, v)
	for i, m := range ec.Comp.Comp.Params {
		if a := ec.Args[i]; a != nil {
			b.setProp(v, name, m, a)
		}
	}
	// Its style: list, under the block's props.
	el0 := ec.Comp.Comp.Element
	b.applyStyle(ec.Style, styleTarget{v: v, variants: el0 != nil && el0.Variants, block: blockKeys(name, ec.Props, false)})
	// A prop set twice: the last one wins.
	last := map[*check.Member]int{}
	for i, p := range ec.Props {
		last[p.Member] = i
	}
	for i, p := range ec.Props {
		if last[p.Member] == i {
			b.setProp(v, name, p.Member, p.Value)
		}
	}
	b.variants(v, ec)
	for _, h := range ec.Handlers {
		ev := prelude.EventID(name, h.Event.Name())
		var clos int
		if check.EventBuilds(ec, h.Event.Name()) {
			clos = b.buildingHandler(h.Handler, h.Event)
		} else {
			clos = b.handler(h.Handler, h.Event)
		}
		b.callBTo(uiOn, bc.None, v, b.konst(int64(ev)), clos)
	}
	for _, bd := range ec.Binds {
		b.bind(v, name, bd)
	}
	for _, s := range ec.Slots {
		key := prelude.KeyID(name, s.Prop.Name())
		view := b.viewsOf(s.Block.Items)
		b.callBTo(uiSlot, bc.None, v, b.konst(int64(key)), view)
	}
	if check.LazyChildren(ec) && len(ec.Children) > 0 {
		// Built when they show, under an owner of the element's own.
		mark := b.mark()
		build := b.builderItems(b.l.info.Ctxs[ec.Children[0]], ec.Children)
		b.callBTo(uiLazy, bc.None, v, build)
		b.release(mark)
		return v
	}
	rb := b.l.info.RouteBuilders[ec]
	for _, ch := range ec.Children {
		mark := b.mark()
		if rb != nil && ch == rb.Item {
			b.release(mark)
			continue
		}
		if tn := b.l.info.RoutedScreens[ch]; tn != nil {
			// A routed screen: a Route of its pattern.
			b.callBTo(uiAdd, bc.None, v, b.routedRoute(tn))
			b.release(mark)
			continue
		}
		if f, ok := ch.(*syntax.ForStmt); ok && b.l.info.Fors[f] != nil && b.l.info.Fors[f].Lazy {
			// Rows the element builds only while they're on screen.
			items, key, row, kk := b.forParts(f)
			b.callBTo(uiLazyFor, bc.None, v, items, key, row, b.konst(int64(kk)))
			b.release(mark)
			continue
		}
		cv, opt := b.viewItem(ch)
		if cv >= 0 {
			skip := b.f.NewLabel()
			if opt {
				b.emitJump(op("JNONE"), cv, skip)
			}
			b.callBTo(uiAdd, bc.None, v, cv)
			b.f.Place(skip)
		}
		b.release(mark)
	}
	if rb != nil {
		// Its screen: built for each of its screens, with their Params.
		mark := b.mark()
		b.callBTo(uiLazy, bc.None, v, b.routeBuilder(rb))
		b.release(mark)
	}
	return v
}

// setProp sets an element's key: once when its value reads nothing
// reactive, else as a binding.
func (b *fb) setProp(v int, el string, m *check.Member, x syntax.Expr) {
	mark := b.mark()
	defer b.release(mark)
	key := prelude.KeyID(el, m.Name())
	if key == 0 {
		b.l.errorf(x, "%s has no key %s in the runtime", el, m.Name())
		return
	}
	k := b.konst(int64(key))
	if b.l.prog.Reactive(x) {
		t := b.l.thunkType(x, m.Type)
		clos := b.thunk(b.l.info.Ctxs[x], x, t)
		b.callBTo(uiBind, bc.None, v, k, clos)
		return
	}
	val := b.value(x, -1)
	b.callBTo(uiSet, bc.None, v, k, val)
}

// thunkType is the type a binding of x returns.
func (l *Lowerer) thunkType(x syntax.Expr, want check.Type) check.Type {
	if t, ok := l.info.Convs[x]; ok {
		return t
	}
	if t := l.info.Types[x]; t != nil {
		return t
	}
	return want
}

// handler makes an on handler's closure: it takes the event's values.
func (b *fb) handler(h *syntax.OnHandler, ev *check.Member) int {
	ctx := b.l.info.Ctxs[h]
	vars := b.l.info.Binds[h]
	params := make([]*check.Var, len(ev.Params))
	pkinds := make([]bc.Kind, len(ev.Params))
	for i, p := range ev.Params {
		pkinds[i] = b.kind(p.Type)
		if i < len(vars) {
			params[i] = vars[i]
		}
	}
	return b.makeClosure(closure{name: "on " + h.Name.Name, ctxs: []*check.FuncCtx{ctx}, params: params, pkinds: pkinds, ret: bc.KVoid, throws: true, body: func(c *fb) {
		c.ctx = ctx
		c.block(h.Body)
		c.emit(op("RET0"))
		if ctx.Awaits {
			c.f.Flags |= bc.FAsync
		}
	}})
}

// buildingHandler makes the handler of an event that Builds (a
// Boundary's error): it takes the event's values and returns the View
// its block's views make.
func (b *fb) buildingHandler(h *syntax.OnHandler, ev *check.Member) int {
	ctx := b.l.info.Ctxs[h]
	vars := b.l.info.Binds[h]
	params := make([]*check.Var, len(ev.Params))
	pkinds := make([]bc.Kind, len(ev.Params))
	for i, p := range ev.Params {
		pkinds[i] = b.kind(p.Type)
		if i < len(vars) {
			params[i] = vars[i]
		}
	}
	return b.makeClosure(closure{name: "on " + h.Name.Name, ctxs: []*check.FuncCtx{ctx}, params: params, pkinds: pkinds, ret: bc.KRef, throws: true, body: func(c *fb) {
		c.ctx = ctx
		v := c.viewsOf(h.Body.Items)
		c.emit(op("RET"), v)
	}})
}

// bind makes bind name: target: a binding of the key to target, and a
// handler of the control's change writing it.
func (b *fb) bind(v int, el string, bd *check.BindArg) {
	mark := b.mark()
	defer b.release(mark)
	key := prelude.KeyID(el, bd.Prop.Name())
	readCtx := b.l.info.Ctxs[bd.Item]
	clos := b.thunk(readCtx, bd.Target, bd.Prop.Type)
	b.callBTo(uiBind, bc.None, v, b.konst(int64(key)), clos)
	if bd.Event == nil {
		return
	}
	h := b.bindWriter(bd)
	ev := prelude.EventID(el, bd.Event.Name())
	b.callBTo(uiOn, bc.None, v, b.konst(int64(ev)), h)
}

// bindWriter is the handler of a bind's event (an element's, a native
// view's, a component's bound prop's change): it writes the event's
// value to the bind's target.
func (b *fb) bindWriter(bd *check.BindArg) int {
	wctx := b.l.info.Ctxs[bd.Target]
	params := make([]*check.Var, len(bd.Event.Params))
	pkinds := make([]bc.Kind, len(bd.Event.Params))
	for i, p := range bd.Event.Params {
		pkinds[i] = b.kind(p.Type)
	}
	if len(params) > 0 && len(wctx.Params) > 0 {
		params[0] = wctx.Params[0]
	}
	return b.makeClosure(closure{name: "bind " + bd.Prop.Name(), ctxs: []*check.FuncCtx{wctx}, params: params, pkinds: pkinds, ret: bc.KVoid, throws: true, body: func(c *fb) {
		c.ctx = wctx
		if len(params) > 0 && params[0] != nil {
			lv := c.prepare(bd.Target)
			c.set(lv, c.vars[params[0]])
		}
		c.emit(op("RET0"))
	}})
}

// bindAccessor is the accessor of a component's bound prop: of its
// target, read as a binding (its cell when it's one read alone).
func (b *fb) bindAccessor(bd *check.BindArg) int {
	t := bd.Prop.Type
	r := b.temp(bc.KRef)
	if ref := b.readsAlone(bd.Target); ref >= 0 {
		b.emit(op("ACCC"), r, ref)
		return r
	}
	clos := b.thunk(b.l.info.Ctxs[bd.Item], bd.Target, t)
	b.emit(op("ACCF"), r, clos, int(b.kind(t)))
	return r
}

// componentCall calls a component's function: accessors of its
// parameters and props, builders of its slots, its events' handlers.
func (b *fb) componentCall(ec *check.ElementCall) int {
	comp := ec.Comp.Comp
	idx := b.l.compFunc(ec.Comp, ec.Comp.Type)
	var vals []int
	none := func() int {
		r := b.temp(bc.KRef)
		b.emit(op("KNONE"), r)
		return r
	}
	for i, m := range comp.Params {
		a := ec.Args[i]
		switch {
		case a == nil:
			vals = append(vals, none())
		case m.IsSlot():
			vals = append(vals, b.builder(a))
		default:
			vals = append(vals, b.accessor(a, m.Type))
		}
	}
	for _, m := range comp.Props {
		var val syntax.Expr
		for _, p := range ec.Props {
			if p.Member == m {
				val = p.Value
			}
		}
		var slot *check.SlotArg
		for _, s := range ec.Slots {
			if s.Prop == m {
				slot = s
			}
		}
		var bd *check.BindArg
		for _, x := range ec.Binds {
			if x.Prop == m {
				bd = x
			}
		}
		switch {
		case bd != nil:
			// bind p: target: the prop reads the target.
			vals = append(vals, b.bindAccessor(bd))
		case val != nil && m.IsSlot():
			vals = append(vals, b.builder(val))
		case val != nil:
			vals = append(vals, b.accessor(val, m.Type))
		case slot != nil:
			vals = append(vals, b.builderItems(b.l.info.Ctxs[slot.Block], slot.Block.Items))
		case m.Name() == "children" && len(ec.Children) > 0:
			vals = append(vals, b.builderItems(b.l.info.Ctxs[ec.Children[0]], ec.Children))
		default:
			vals = append(vals, none())
		}
	}
	for _, m := range comp.Events {
		var h *check.HandlerArg
		for _, x := range ec.Handlers {
			if x.Event == m {
				h = x
			}
		}
		var bd *check.BindArg
		for _, x := range ec.Binds {
			if x.Event == m {
				bd = x
			}
		}
		switch {
		case h != nil:
			vals = append(vals, b.handler(h.Handler, m))
		case bd != nil:
			// bind p: target: its pChange writes the target.
			vals = append(vals, b.bindWriter(bd))
		default:
			vals = append(vals, none())
		}
	}
	v := b.invoke(idx, vals, bc.KRef, -1, false)
	b.outerStyle(v, ec)
	return v
}

// outerStyle sets the style props a component's caller gives on the
// element the component shows (check's TakesStyle), over what the
// component sets of them: ui.set_outer, or ui.bind_outer when the value
// reads what changes. The last of a prop set twice wins.
func (b *fb) outerStyle(v int, ec *check.ElementCall) {
	// The caller's style: list, under the caller's style props.
	var props []*check.PropArg
	for _, p := range ec.Props {
		if p.Member.Owner != ec.Comp {
			props = append(props, p)
		}
	}
	b.applyStyle(ec.Style, styleTarget{v: v, outer: true, variants: b.l.prog.StyleTakesVariants(ec.Comp), block: blockKeys("", props, true)})
	last := map[*check.Member]int{}
	for i, p := range ec.Props {
		if p.Member.Owner != ec.Comp {
			last[p.Member] = i
		}
	}
	for i, p := range ec.Props {
		if j, ok := last[p.Member]; !ok || j != i {
			continue
		}
		mark := b.mark()
		key := prelude.KeyID("", p.Member.Name())
		if key == 0 {
			b.l.errorf(p.Value, "%s isn't a style key the runtime has", p.Member.Name())
			b.release(mark)
			continue
		}
		k := b.konst(int64(key))
		if b.l.prog.Reactive(p.Value) {
			t := b.l.thunkType(p.Value, p.Member.Type)
			clos := b.thunk(b.l.info.Ctxs[p.Value], p.Value, t)
			b.callBTo(uiBindOuter, bc.None, v, k, clos)
		} else {
			b.callBTo(uiSetOuter, bc.None, v, k, b.value(p.Value, -1))
		}
		b.release(mark)
	}
}

// showChain makes a view's if's condition and then builder when its
// condition is a chain (if let a = x, let b = f(a), b > 0 { … }). A
// chain with no names is a condition like any other. One with names is
// a memo of them, a tuple, none when a clause fails: the condition is
// whether it's none, and in the then block each name is a memo of its
// element, so the chain runs once for both.
func (b *fb) showChain(s *syntax.IfStmt, vc *check.ViewCond) (cond, then int) {
	condCtx := b.l.info.Ctxs[s.Cond]
	thenCtx := b.l.info.Ctxs[s.Then]
	if len(vc.Chain) == 0 {
		cond = b.makeClosure(closure{name: "condition", ctxs: []*check.FuncCtx{condCtx}, ret: bc.KBool, throws: true, body: func(c *fb) {
			c.ctx = condCtx
			fail := c.f.NewLabel()
			c.openScope()
			c.condJump(s.Cond, fail)
			c.closeScope()
			r := c.temp(bc.KBool)
			c.emit(op("KSMALL"), r, 1)
			c.emit(op("RET"), r)
			c.f.Place(fail)
			c.emit(op("KSMALL"), r, 0)
			c.emit(op("RET"), r)
		}})
		return cond, b.builderItems(thenCtx, s.Then.Items)
	}
	tt := &check.Tuple{}
	for _, v := range vc.Chain {
		tt.Elems = append(tt.Elems, b.typ(v.Type))
	}
	shape := b.l.shape(tt)
	chain := b.makeClosure(closure{name: "chain", ctxs: []*check.FuncCtx{condCtx}, ret: bc.KRef, throws: true, body: func(c *fb) {
		c.ctx = condCtx
		fail := c.f.NewLabel()
		c.openScope()
		c.condJump(s.Cond, fail)
		var vals []int
		for _, v := range vc.Chain {
			vals = append(vals, c.readVar(v, s.Cond))
		}
		r := c.newStruct(shape, tt.Elems, vals, -1)
		c.emit(op("RET"), r)
		c.closeScope()
		c.f.Place(fail)
		n := c.temp(bc.KRef)
		c.emit(op("KNONE"), n)
		c.emit(op("RET"), n)
	}})
	mv := &check.Var{}
	m := b.newReg(bc.KRef)
	b.emit(op("MEMO"), m, chain, int(bc.KRef))
	cond = b.makeClosure(closure{name: "condition", extra: []extraCap{{mv, m}}, ret: bc.KBool, throws: true, body: func(c *fb) {
		v := c.temp(bc.KRef)
		c.emit(op("MREAD"), v, c.vars[mv])
		r := c.temp(bc.KBool)
		c.emit(op("ISNONE"), r, v)
		c.emit(op("NOT"), r, r)
		c.emit(op("RET"), r)
	}})
	then = b.makeClosure(closure{name: "then", ctxs: []*check.FuncCtx{thenCtx}, extra: []extraCap{{mv, m}}, ret: bc.KRef, throws: true, body: func(c *fb) {
		c.ctx = thenCtx
		for i, u := range vc.Then {
			k := c.kind(u.Type)
			i := i
			um := c.makeClosure(closure{name: "unwrap", extra: []extraCap{{mv, c.vars[mv]}}, ret: k, throws: true, body: func(c2 *fb) {
				v := c2.temp(bc.KRef)
				c2.emit(op("MREAD"), v, c2.vars[mv])
				x := c2.unwrap(v, tt, -1, s.Cond)
				r := c2.temp(k)
				c2.emit(op("GETF"), r, x, i)
				c2.ret(r, k)
			}})
			r := c.newReg(bc.KRef)
			c.emit(op("MEMO"), r, um, int(k))
			c.vars[u] = r
		}
		v := c.viewsOf(s.Then.Items)
		c.emit(op("RET"), v)
	}})
	return cond, then
}
