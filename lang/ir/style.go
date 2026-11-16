package ir

import (
	bc "github.com/egoist/asuka/lang/bytecode"
	"github.com/egoist/asuka/lang/check"
	"github.com/egoist/asuka/lang/prelude"
	"github.com/egoist/asuka/lang/syntax"
)

// Styles (lang/LOWERING.md's "Styles", rfcs/0009): an element's style:
// list, flattened where the element is built. Each style applied is an
// application: its parameters' arguments evaluated once (a memo when
// they read what changes), its uses looked up, and its props inlined.
// A prop no condition holds is set as the block's are, under them; the
// props a condition (an if's branch, a match's arm) holds are a layer
// of the view, on while the condition holds; a style's variants are
// layers of their state.

const (
	uiLayer     = "ui.layer"      // (view, tier int, state int) -> int: the layer
	uiLayerSet  = "ui.layer_set"  // (view, layer int, key int, value) -> void
	uiLayerBind = "ui.layer_bind" // (view, layer, key, thunk) -> void
	uiLayerWhen = "ui.layer_when" // (view, layer, cond closure -> bool) -> void
)

// The layer tiers (core/src/ui/ui.h's AK_LAYER_*).
const (
	layerOwn = iota
	layerOuter
	layerState
)

// A styleEnv is an application of a style: the registers of its
// parameters and uses, and which hold memos.
type styleEnv struct {
	vars map[*check.Var]int
	memo map[*check.Var]bool
}

// A styleGuard is a condition a prop is under: its branch is taken.
type styleGuard struct {
	cond   *check.StyleCond
	branch int
	env    *styleEnv // the application its conditions read
}

// A stylePiece is a prop a style list sets, in the list's order.
type stylePiece struct {
	member *check.Member
	key    int
	value  syntax.Expr
	env    *styleEnv
	guards []styleGuard
	state  string // a variant's state; "" for none
}

// A styleTarget is where a list's props go.
type styleTarget struct {
	v        int
	outer    bool         // a component's caller's: over the component's own
	variants bool         // the view takes variants
	block    map[int]bool // the keys the block sets, over the list's
}

// bindEnv makes an application's locals the registers its code reads.
func (b *fb) bindEnv(env *styleEnv) {
	if env == nil {
		return
	}
	if b.l.styleMemo == nil {
		b.l.styleMemo = map[*check.Var]bool{}
	}
	for v, r := range env.vars {
		b.vars[v] = r
		b.l.styleMemo[v] = env.memo[v]
	}
}

// styleReactive reports whether a style's value (or an argument) reads
// what changes: as any binding, or one of its application's memos.
func (b *fb) styleReactive(x syntax.Expr) bool {
	if b.l.prog.Reactive(x) {
		return true
	}
	found := false
	syntax.Inspect(x, func(n syntax.Node) bool {
		if id, ok := n.(*syntax.Ident); ok {
			if v, ok := b.l.info.Uses[id].(*check.Var); ok && v.Kind == check.StyleLocal && b.l.styleMemo[v] {
				found = true
			}
		}
		return !found
	})
	return found
}

// styleApply evaluates an application of d: its arguments (outer the
// application they're written in), defaults and uses.
func (b *fb) styleApply(d *check.StyleDef, args []syntax.Expr, outer *styleEnv) *styleEnv {
	env := &styleEnv{vars: map[*check.Var]int{}, memo: map[*check.Var]bool{}}
	for i, p := range d.Params {
		x := p.Default
		inner := false
		if i < len(args) && args[i] != nil {
			x = args[i]
		} else {
			inner = true // a default may read the parameters before it
		}
		if x == nil || p.Var == nil {
			continue
		}
		if inner {
			b.bindEnv(env)
		} else {
			b.bindEnv(outer)
		}
		mark := b.mark()
		if b.styleReactive(x) {
			clos := b.thunk(b.l.info.Ctxs[x], x, p.Type)
			r := b.local(bc.KRef)
			b.emit(op("MEMO"), r, clos, int(b.kind(p.Type)))
			env.vars[p.Var], env.memo[p.Var] = r, true
		} else {
			r := b.local(b.kind(p.Type))
			b.value(x, r)
			env.vars[p.Var] = r
		}
		b.release(mark)
	}
	for _, it := range d.Items {
		if v := it.Use; v != nil {
			mark := b.mark()
			r := b.local(bc.KRef)
			b.callBTo(uiUse, r, b.kstr(b.l.typeKey(b.typ(v.Type))))
			env.vars[v] = r
			b.release(mark)
		}
	}
	return env
}

// styleFlatten adds the props a list sets to out, in order.
func (b *fb) styleFlatten(entries []*check.StyleEntry, env *styleEnv, guards []styleGuard, state string, variants bool, out *[]stylePiece) {
	for _, e := range entries {
		if e.Cond != nil {
			for i, br := range e.Cond.Branches {
				g := append(append([]styleGuard(nil), guards...), styleGuard{e.Cond, i, env})
				b.styleFlatten(br, env, g, state, variants, out)
			}
			continue
		}
		d := e.Style
		if d == nil || d.Cyclic {
			continue
		}
		app := b.styleApply(d, e.Args, env)
		b.styleItems(d.Items, app, guards, state, variants, out)
	}
}

func (b *fb) styleItems(items []*check.StyleItem, env *styleEnv, guards []styleGuard, state string, variants bool, out *[]stylePiece) {
	for _, it := range items {
		switch {
		case it.Prop != nil:
			name := it.Prop.Member.Name()
			*out = append(*out, stylePiece{member: it.Prop.Member, key: prelude.KeyID("", name), value: it.Prop.Value, env: env, guards: guards, state: state})
		case it.Style != nil:
			b.styleFlatten(it.Style, env, guards, state, variants, out)
		case it.Variant != nil:
			if state == "" && variants {
				b.styleItems(it.Variant.Items, env, guards, it.Variant.State, variants, out)
			}
		}
	}
}

func sameLayer(a, b stylePiece) bool {
	if a.state != b.state || len(a.guards) != len(b.guards) {
		return false
	}
	for i := range a.guards {
		if a.guards[i] != b.guards[i] {
			return false
		}
	}
	return true
}

// applyStyle sets the props of an element's style: list on its view.
func (b *fb) applyStyle(entries []*check.StyleEntry, t styleTarget) {
	if len(entries) == 0 {
		return
	}
	var pieces []stylePiece
	b.styleFlatten(entries, nil, nil, "", t.variants, &pieces)
	plain := func(p stylePiece) bool { return p.state == "" && len(p.guards) == 0 }
	// The props under no condition: the last of each key, under the
	// block's.
	lastPlain := map[int]int{}
	for i, p := range pieces {
		if plain(p) {
			lastPlain[p.key] = i
		}
	}
	for i, p := range pieces {
		if plain(p) && !t.block[p.key] && lastPlain[p.key] == i {
			b.styleSet(t, p)
		}
	}
	// The layers: each run of props under the same conditions (and
	// state), in order. A prop the block sets, or a later one under no
	// condition, is over a condition's: the layer leaves it out.
	for i := 0; i < len(pieces); {
		p := pieces[i]
		if plain(p) {
			i++
			continue
		}
		j := i + 1
		for j < len(pieces) && sameLayer(pieces[j], p) {
			j++
		}
		var keep []stylePiece
		for k := i; k < j; k++ {
			q := pieces[k]
			if q.state == "" {
				if t.block[q.key] {
					continue
				}
				if lp, ok := lastPlain[q.key]; ok && lp > k {
					continue
				}
			}
			last := true
			for _, r := range pieces[k+1 : j] {
				if r.key == q.key {
					last = false
				}
			}
			if last {
				keep = append(keep, q)
			}
		}
		if len(keep) > 0 {
			b.styleLayer(t, keep)
		}
		i = j
	}
}

// styleSet sets a prop no condition holds: as the block's props are
// (ui.set, ui.bind), or as a component's caller's (ui.set_outer,
// ui.bind_outer).
func (b *fb) styleSet(t styleTarget, p stylePiece) {
	mark := b.mark()
	defer b.release(mark)
	if p.key == 0 {
		b.l.errorf(p.value, "%s isn't a style key the runtime has", p.member.Name())
		return
	}
	b.bindEnv(p.env)
	k := b.konst(int64(p.key))
	if b.styleReactive(p.value) {
		clos := b.thunk(b.l.info.Ctxs[p.value], p.value, b.l.thunkType(p.value, p.member.Type))
		if t.outer {
			b.callBTo(uiBindOuter, bc.None, t.v, k, clos)
		} else {
			b.callBTo(uiBind, bc.None, t.v, k, clos)
		}
		return
	}
	val := b.value(p.value, -1)
	if t.outer {
		b.callBTo(uiSetOuter, bc.None, t.v, k, val)
	} else {
		b.callBTo(uiSet, bc.None, t.v, k, val)
	}
}

// styleLayer makes a layer of props under the same conditions: on while
// they hold (and, a variant's, while the view is in its state).
func (b *fb) styleLayer(t styleTarget, pieces []stylePiece) {
	p := pieces[0]
	tier, state := layerOwn, 0
	if t.outer {
		tier = layerOuter
	}
	if p.state != "" {
		tier, state = layerState, prelude.StateID(p.state)
	}
	id := b.local(bc.KInt)
	mark := b.mark()
	b.callBTo(uiLayer, id, t.v, b.konst(int64(tier)), b.konst(int64(state)))
	b.release(mark)
	if len(p.guards) > 0 {
		mark := b.mark()
		b.callBTo(uiLayerWhen, bc.None, t.v, id, b.styleCond(p.guards))
		b.release(mark)
	}
	for _, q := range pieces {
		mark := b.mark()
		if q.key == 0 {
			b.l.errorf(q.value, "%s isn't a style key the runtime has", q.member.Name())
			b.release(mark)
			continue
		}
		b.bindEnv(q.env)
		k := b.konst(int64(q.key))
		if b.styleReactive(q.value) {
			clos := b.thunk(b.l.info.Ctxs[q.value], q.value, b.l.thunkType(q.value, q.member.Type))
			b.callBTo(uiLayerBind, bc.None, t.v, id, k, clos)
		} else {
			b.callBTo(uiLayerSet, bc.None, t.v, id, k, b.value(q.value, -1))
		}
		b.release(mark)
	}
}

// styleCond is a closure that reports whether every guard's branch is
// the one its condition takes.
func (b *fb) styleCond(guards []styleGuard) int {
	var ctxs []*check.FuncCtx
	for _, g := range guards {
		b.bindEnv(g.env)
		ctxs = append(ctxs, b.l.info.Ctxs[g.cond.Node])
	}
	return b.makeClosure(closure{name: "style condition", ctxs: ctxs, ret: bc.KBool, throws: true, body: func(c *fb) {
		fail := c.f.NewLabel()
		for _, g := range guards {
			c.at(g.cond.Node)
			c.styleTaken(g.cond, g.branch, fail)
		}
		yes := c.temp(bc.KBool)
		c.emit(op("KSMALL"), yes, 1)
		c.ret(yes, bc.KBool)
		c.f.Place(fail)
		no := c.temp(bc.KBool)
		c.emit(op("KSMALL"), no, 0)
		c.ret(no, bc.KBool)
	}})
}

// styleTaken jumps to fail unless sc takes branch k: an if's earlier
// conditions are false and its own true (an else: none was); a match's
// earlier arms don't match and its own does.
func (b *fb) styleTaken(sc *check.StyleCond, k int, fail Label) {
	if sc.Subject == nil {
		for j := 0; j < k; j++ {
			next := b.f.NewLabel()
			b.condJump(sc.Conds[j], next)
			b.jmp(fail)
			b.f.Place(next)
		}
		if sc.Conds[k] != nil {
			b.condJump(sc.Conds[k], fail)
		}
		return
	}
	mark := b.mark()
	subj := b.value(sc.Subject, -1)
	st := b.rawType(sc.Subject)
	for j := 0; j < k; j++ {
		a := sc.Arms[j]
		next := b.f.NewLabel()
		b.testPattern(a.Pattern, subj, st, next)
		if a.Guard != nil {
			g := b.value(a.Guard, -1)
			b.emitJump(op("JF"), g, next)
		}
		b.jmp(fail)
		b.f.Place(next)
	}
	a := sc.Arms[k]
	b.testPattern(a.Pattern, subj, st, fail)
	if a.Guard != nil {
		g := b.value(a.Guard, -1)
		b.emitJump(op("JF"), g, fail)
	}
	b.release(mark)
}

// blockKeys are the style keys an element's (or a component's caller's)
// block sets: they're over its style: list's.
func blockKeys(el string, props []*check.PropArg, outer bool) map[int]bool {
	keys := map[int]bool{}
	for _, p := range props {
		if outer {
			keys[prelude.KeyID("", p.Member.Name())] = true
		} else {
			keys[prelude.KeyID(el, p.Member.Name())] = true
		}
	}
	return keys
}

// variantStyle sets a variant's style: list in the block's variant of
// its state, under the variant's own props.
func (b *fb) variantStyle(v int, va *check.VariantArg, state int) {
	var pieces []stylePiece
	b.styleFlatten(va.Style, nil, nil, "", false, &pieces)
	own := map[int]bool{}
	for _, p := range va.Props {
		own[prelude.KeyID("", p.Member.Name())] = true
	}
	last := map[int]int{}
	for i, p := range pieces {
		last[p.key] = i
	}
	for i, p := range pieces {
		if own[p.key] || last[p.key] != i || p.key == 0 {
			continue
		}
		mark := b.mark()
		b.bindEnv(p.env)
		k := b.konst(int64(p.key))
		s := b.konst(int64(state))
		if b.styleReactive(p.value) {
			clos := b.thunk(b.l.info.Ctxs[p.value], p.value, b.l.thunkType(p.value, p.member.Type))
			b.callBTo(uiVariantBind, bc.None, v, s, k, clos)
		} else {
			b.callBTo(uiVariant, bc.None, v, s, k, b.value(p.value, -1))
		}
		b.release(mark)
	}
}
