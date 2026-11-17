package ir

import (
	"strings"

	bc "github.com/egoist/asuka/lang/bytecode"
	"github.com/egoist/asuka/lang/check"
	"github.com/egoist/asuka/lang/prelude"
)

// Routes (lang/LOWERING.md's "Routes"): a Route's screen, the routed
// screens named in a Route's block, and the hrefs made of routed
// screens' calls.

const (
	uiNavigate = "ui.navigate" // (href: string, mode: int: 0 navigate, 1 push, 2 replace)
	uiHrefOf   = "ui.href_of"  // (pattern: string, values: [string]) -> string
)

// preludeFn is the prelude's function named name (types.asuka's), its
// index once it's lowered.
func (b *fb) preludeFn(name string) int {
	f, ok := b.l.prog.Prelude.Scope.LookupLocal(name).(*check.Function)
	if !ok {
		panic("ir: the prelude has no fn " + name)
	}
	return b.l.funcIndex(f, nil, nil)
}

// routeBuilder makes a Route's screen, p => View: the closure the
// runtime builds each of its screens with, given their Params (an
// accessor, p's register: reading p is AGET of it).
func (b *fb) routeBuilder(rb *check.RouteBuilder) int {
	ctx := b.l.info.Ctxs[rb.Lambda]
	params := rb.Params
	if params == nil {
		params = []*check.Var{rb.Param}
	}
	pkinds := make([]bc.Kind, len(params))
	for i := range pkinds {
		pkinds[i] = bc.KRef // accessors
	}
	return b.makeClosure(closure{name: "builder", ctxs: []*check.FuncCtx{ctx}, params: params, pkinds: pkinds, ret: bc.KRef, throws: true, body: func(c *fb) {
		c.ctx = ctx
		v := c.viewsOf(rb.Items)
		c.emit(op("RET"), v)
	}})
}

// routedRoute is the Route a routed screen named in a Route's block
// makes: of its pattern, built with each of the screen's parameters read
// from the address (its path's parameter, else its query's), converted
// to its type; one that's missing takes its default, else (as one that
// doesn't convert) throws a RouteError.
func (b *fb) routedRoute(tn *check.TypeName) int {
	pattern, _ := check.RoutePattern(tn)
	el := prelude.ElementID("Route")
	v := b.local(bc.KRef)
	b.callBTo(uiNew, v, b.konst(int64(el)))
	b.callBTo(uiSet, bc.None, v, b.konst(int64(prelude.KeyID("Route", "path"))), b.kstr(pattern))
	comp := tn.Comp
	idx := b.l.compFunc(tn, tn.Type)
	pv := &check.Var{Type: b.l.prog.Prelude.Scope.LookupLocal("Params").(*check.TypeName).Type, Kind: check.RowItem}
	build := b.makeClosure(closure{name: "route " + tn.Name(), params: []*check.Var{pv}, ret: bc.KRef, throws: true, body: func(c *fb) {
		p := c.temp(bc.KRef)
		c.emit(op("AGET"), p, c.vars[pv])
		var vals []int
		for _, m := range comp.Params {
			vals = append(vals, c.routeParam(p, m))
		}
		for range comp.Props {
			r := c.temp(bc.KRef)
			c.emit(op("KNONE"), r)
			vals = append(vals, r)
		}
		for range comp.Events {
			r := c.temp(bc.KRef)
			c.emit(op("KNONE"), r)
			vals = append(vals, r)
		}
		for range assignedProps(comp) {
			r := c.temp(bc.KRef)
			c.emit(op("KNONE"), r) // not bound
			vals = append(vals, r)
		}
		view := c.invoke(idx, vals, bc.KRef, -1, false)
		c.emit(op("RET"), view)
	}})
	b.callBTo(uiLazy, bc.None, v, build)
	return v
}

// routeParam is the accessor of a routed screen's parameter m, read from
// the Params p.
func (b *fb) routeParam(p int, m *check.Member) int {
	t := m.Type
	name := b.kstr(m.Name())
	s := b.invoke(b.preludeFn("routeParam"), []int{p, name}, bc.KRef, -1, false)
	acc := b.local(bc.KRef)
	missing, done := b.f.NewLabel(), b.f.NewLabel()
	b.emitJump(op("JNONE"), s, missing)
	var v int
	switch {
	case t == check.String:
		v = s
	case t == check.Int:
		v = b.invoke(b.preludeFn("routeInt"), []int{s, name}, bc.KInt, -1, false)
	case t == check.Float:
		v = b.invoke(b.preludeFn("routeFloat"), []int{s, name}, bc.KFloat, -1, false)
	case t == check.Bool:
		v = b.invoke(b.preludeFn("routeBool"), []int{s, name}, bc.KBool, -1, false)
	default:
		// An enum: its case's index, by its name.
		n := check.AsNamed(t, check.EnumType)
		var names []string
		for _, k := range n.Obj.Cases {
			names = append(names, k.Name)
		}
		cases := b.frozenNames(names, "route."+b.l.typeKey(t))
		v = b.invoke(b.preludeFn("routeCase"), []int{s, name, cases}, bc.KInt, -1, false)
	}
	k := b.kind(t)
	b.emit(op("ACCV"), acc, b.valueReg(v, k), int(k))
	b.jmp(done)
	b.f.Place(missing)
	if m.Default == nil {
		b.invoke(b.preludeFn("routeMissing"), []int{name}, bc.KVoid, -1, true)
	}
	b.emit(op("KNONE"), acc)
	b.f.Place(done)
	return acc
}

// frozenNames is a constant array of strings (a global, frozen at init).
func (b *fb) frozenNames(ss []string, key string) int {
	g := b.l.frozenStrings(ss, key)
	r := b.temp(bc.KRef)
	b.emit(op("GGET"), r, g, int(bc.KRef))
	return r
}

// href is a routed screen's address made from a call of it:
// ui.href_of(its whole pattern, its path's values), then its query's.
func (b *fb) href(h *check.Href, dst int) int {
	comp := h.Screen.Comp
	inPath := map[string]bool{}
	var vals []int
	for _, name := range check.PatternParams(h.Pattern) {
		inPath[name] = true
		for i, m := range comp.Params {
			if m.Name() == name && h.Args[i] != nil {
				vals = append(vals, b.describe(b.value(h.Args[i], -1), m.Type))
			}
		}
	}
	ks := repeatKind(bc.KRef, len(vals))
	blk := b.regs(ks)
	for i, v := range vals {
		b.move(blk+i, v)
	}
	arr := b.temp(bc.KRef)
	b.emit(op("NEWA"), arr, int(bc.KRef), bc.None, blk, len(vals))
	b.releaseRegs(blk, ks)
	path := b.callB(uiHrefOf, bc.KRef, b.kstr(h.Pattern), arr)
	// The parameters the path doesn't say: its query, in the screen's
	// order, the ones the call gives.
	var parts []int
	for i, m := range comp.Params {
		if inPath[m.Name()] || h.Args[i] == nil {
			continue
		}
		sep := "&"
		if len(parts) == 0 {
			sep = "?"
		}
		v := b.describe(b.value(h.Args[i], -1), m.Type)
		parts = append(parts, b.kstr(sep+escapeQuery(m.Name())+"="), b.callB("url.escape", bc.KRef, v))
	}
	if len(parts) == 0 {
		return b.copyTo(path, dst)
	}
	parts = append([]int{path}, parts...)
	ks = repeatKind(bc.KRef, len(parts))
	blk = b.regs(ks)
	for i, v := range parts {
		b.move(blk+i, v)
	}
	all := b.temp(bc.KRef)
	b.emit(op("NEWA"), all, int(bc.KRef), bc.None, blk, len(parts))
	b.releaseRegs(blk, ks)
	return b.copyTo(b.callB("str.join", bc.KRef, all, b.kstr("")), dst)
}

// escapeQuery escapes a query's key as url.QueryEscape does.
func escapeQuery(s string) string {
	var out strings.Builder
	for i := 0; i < len(s); i++ {
		c := s[i]
		switch {
		case c >= 'a' && c <= 'z', c >= 'A' && c <= 'Z', c >= '0' && c <= '9', c == '-', c == '_', c == '.', c == '~':
			out.WriteByte(c)
		case c == ' ':
			out.WriteByte('+')
		default:
			out.WriteString("%" + strings.ToUpper(string("0123456789abcdef"[c>>4])) + strings.ToUpper(string("0123456789abcdef"[c&15])))
		}
	}
	return out.String()
}

// navigate is navigate(href) and replace(href): the Router's.
func (b *fb) navigate(call *check.Call, mode int64) int {
	v := b.value(call.Args[0], -1)
	b.callBTo(uiNavigate, bc.None, v, b.konst(mode))
	return -1
}
