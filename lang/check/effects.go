package check

import (
	"fmt"
	"sort"
	"strings"

	"github.com/egoist/asuka/lang/diag"
	"github.com/egoist/asuka/lang/syntax"
)

// The check of an effect's writes (rfcs/0012): an effect that writes a
// state it follows runs again after its own write, a pass late (and
// forever, when the write doesn't converge). What it follows is what
// its body reads (effect { }), or what its dependencies read (effect(a,
// b) { }), through the functions it calls and the lets and following
// states it reads, as far as the compiler sees them. A state is its
// declaration, not an instance. A read under peek isn't followed; a
// write into part of a state (x.f = v, x[k] = v, x.push(v)) is a write
// of it, not a read. A task may write what it follows: it restarts by
// cancelling, and ends.

// A stateUse is a read or a write of a state (a Member or a Global) at a
// node of the body.
type stateUse struct {
	obj Object
	at  syntax.Node
}

// A callUse is a call of a function at a node of the body; or, with ctx
// and no f, what runs because of it there: a store made, whose body's
// cleanups run when the owner it's made under goes (an effect's next
// run, for one made in an effect).
type callUse struct {
	f   *Function
	at  syntax.Node
	ctx *FuncCtx
}

// body is the context a callUse reaches.
func (u callUse) body() *FuncCtx {
	if u.f != nil {
		return u.f.Ctx
	}
	return u.ctx
}

// An effectCheck is an effect whose writes are checked once every body
// is: its body's context, and its dependencies' (effect(a, b)).
type effectCheck struct {
	decl *syntax.EffectDecl
	body *FuncCtx
	deps []*FuncCtx
	// untracked: effect(…) { }, whose body follows nothing (effect() { }
	// follows nothing at all).
	untracked bool
	follows   stateSet // what it follows, once computed (followSet)
}

// followSet is what e follows: its dependencies' reads, or its body's
// when it has none (effect { }).
func (c *Checker) followSet(e *effectCheck) stateSet {
	if e.follows == nil {
		if e.untracked {
			e.follows = c.readSet(e.deps...)
		} else {
			e.follows = c.readSet(e.body)
		}
	}
	return e.follows
}

// noteRead records a read of state (a member or a global) in the
// current context, unless it's under peek.
func (c *Checker) noteRead(obj Object, at syntax.Node) {
	if c.ctx == nil || c.ctx.peeks > 0 {
		return
	}
	c.ctx.stateReads = append(c.ctx.stateReads, stateUse{obj, at})
}

// noteWrite records what x, an lvalue just checked, writes: the state at
// its root. Its root's read, which checking x recorded, isn't one.
func (c *Checker) noteWrite(x syntax.Expr) {
	obj, at := c.writeRoot(x)
	if obj == nil || c.ctx == nil {
		return
	}
	reads := c.ctx.stateReads[:0]
	for _, r := range c.ctx.stateReads {
		if r.at != at {
			reads = append(reads, r)
		}
	}
	c.ctx.stateReads = reads
	c.ctx.stateWrites = append(c.ctx.stateWrites, stateUse{obj, at})
}

// writeRoot is the state an lvalue changes (x in x.f[k]), and its node.
func (c *Checker) writeRoot(x syntax.Expr) (Object, syntax.Node) {
	for {
		switch e := x.(type) {
		case *syntax.ParenExpr:
			x = e.X
		case *syntax.IndexExpr:
			x = e.X
		case *syntax.MemberExpr:
			sel := c.info.Selections[e]
			if sel == nil {
				return nil, nil
			}
			switch sel.Kind {
			case SelField, SelTupleElem:
				x = e.X
			case SelStoreMember:
				return sel.Member, e
			case SelPkg:
				if g, ok := sel.Obj.(*Global); ok {
					return g, e
				}
				return nil, nil
			default:
				return nil, nil
			}
		case *syntax.Ident:
			switch o := c.info.Uses[e].(type) {
			case *Member:
				return o, e
			case *Global:
				return o, e
			}
			return nil, nil
		default:
			return nil, nil
		}
	}
}

// closureUses takes what a closure made here (child) reads, as its
// maker's: a lambda runs where it's passed (filter's runs in the effect).
// A local fn's writes are the maker's too; a lambda's aren't (a callback
// writes when it's called, later).
func (x *FuncCtx) closureUses(child *FuncCtx) {
	if child.Kind == CtxCleanup {
		// A cleanup runs untracked, as its owner goes or before its
		// effect runs again: what it reads isn't followed, and what it
		// writes is written then (rfcs/0016).
		x.stateWrites = append(x.stateWrites, child.stateWrites...)
		x.writeCalls = append(x.writeCalls, child.writeCalls...)
		return
	}
	switch child.Kind {
	case CtxLambda, CtxLocalFn:
	default:
		return
	}
	if x.peeks == 0 {
		x.stateReads = append(x.stateReads, child.stateReads...)
		x.readCalls = append(x.readCalls, child.readCalls...)
	}
	if child.Kind == CtxLocalFn {
		x.stateWrites = append(x.stateWrites, child.stateWrites...)
		x.writeCalls = append(x.writeCalls, child.writeCalls...)
	}
}

// stateSet is what a context reads or writes, through what it calls:
// each state, and where the context (not what it calls) reaches it.
type stateSet map[Object]syntax.Node

// readSet is what x follows: its reads, the reads of the lets and
// following states it reads (their initializers', or their
// dependencies'), and those of what it calls.
func (c *Checker) readSet(xs ...*FuncCtx) stateSet {
	out := stateSet{}
	seen := map[*FuncCtx]bool{}
	var walk func(x *FuncCtx, at syntax.Node)
	add := func(obj Object, at syntax.Node) {
		if _, ok := out[obj]; !ok {
			out[obj] = at
		}
	}
	walkObj := func(obj Object, at syntax.Node) {
		add(obj, at)
		switch o := obj.(type) {
		case *Member:
			switch {
			case o.Kind == LetMember && o.Default != nil:
				walk(c.info.Ctxs[o.Default], at)
			case o.Kind == StateMember && o.Follows:
				if d, ok := o.Decl.(*syntax.StateDecl); ok && len(d.Deps) > 0 {
					for _, dep := range d.Deps {
						walk(c.info.Ctxs[dep], at)
					}
				} else if o.Default != nil {
					walk(c.info.Ctxs[o.Default], at)
				}
			}
		case *Global:
			if o.Kind == MemoGlobal || o.Kind == StateGlobal && o.Follows {
				if d, ok := o.Decl.(*syntax.StateDecl); ok && len(d.Deps) > 0 {
					for _, dep := range d.Deps {
						walk(c.info.Ctxs[dep], at)
					}
				} else {
					walk(o.Ctx, at)
				}
			}
		}
	}
	walk = func(x *FuncCtx, at syntax.Node) {
		if x == nil || seen[x] {
			return
		}
		seen[x] = true
		for _, r := range x.stateReads {
			where := at
			if where == nil {
				where = r.at
			}
			walkObj(r.obj, where)
		}
		for _, call := range x.readCalls {
			where := at
			if where == nil {
				where = call.at
			}
			if b := call.body(); b != nil {
				walk(b, where)
			}
		}
	}
	for _, x := range xs {
		walk(x, nil)
	}
	return out
}

// writeSet is what x writes, itself or through what it calls.
func (c *Checker) writeSet(x *FuncCtx) stateSet {
	out := stateSet{}
	seen := map[*FuncCtx]bool{}
	var walk func(x *FuncCtx, at syntax.Node)
	walk = func(x *FuncCtx, at syntax.Node) {
		if x == nil || seen[x] {
			return
		}
		seen[x] = true
		for _, w := range x.stateWrites {
			where := at
			if where == nil {
				where = w.at
			}
			if _, ok := out[w.obj]; !ok {
				out[w.obj] = where
			}
		}
		for _, call := range x.writeCalls {
			where := at
			if where == nil {
				where = call.at
			}
			if b := call.body(); b != nil {
				walk(b, where)
			}
		}
	}
	walk(x, nil)
	return out
}

// checkEffectWrites reports each effect that writes a state it follows.
func (c *Checker) checkEffectWrites() {
	for i := range c.effects {
		e := &c.effects[i]
		reads := c.followSet(e)
		writes := c.writeSet(e.body)
		var cycle []Object
		for obj := range writes {
			if _, ok := reads[obj]; ok {
				cycle = append(cycle, obj)
			}
		}
		if len(cycle) == 0 {
			continue
		}
		pos := func(o Object) syntax.Pos { return writes[o].Range().Start }
		sort.Slice(cycle, func(i, j int) bool {
			if pos(cycle[i]) != pos(cycle[j]) {
				return pos(cycle[i]) < pos(cycle[j])
			}
			return cycle[i].Name() < cycle[j].Name()
		})
		obj := cycle[0]
		name := obj.Name()
		// What else it reads itself, which a form that follows names.
		direct := e.body.stateReads
		if e.untracked {
			direct = nil
		}
		var others []string
		for _, r := range direct {
			if _, written := writes[r.obj]; !written && isStateLike(r.obj) && !contains(others, r.obj.Name()) {
				others = append(others, r.obj.Name())
			}
		}
		line := func(n syntax.Node) int { return c.fset.Position(n.Range().Start).Line }
		what := "it reads"
		if e.untracked {
			what = "its dependencies read"
		}
		msg := fmt.Sprintf("this effect writes %s (at %d), which %s at %d: it would run again after its own write, a pass late", name, line(writes[obj]), what, line(reads[obj]))
		var ways []string
		if len(others) > 0 {
			list := others
			if len(list) > 3 {
				list = append(list[:3:3], "…")
			}
			ways = append(ways,
				fmt.Sprintf("to run it when %s changes, compared: effect(%s) { … }", strings.Join(list, " or "), strings.Join(list, ", ")),
				fmt.Sprintf("to reset %s when they change: state %s(%s) = … (a let, let %s = …, when nothing else writes it)", name, name, strings.Join(list, ", "), name))
		}
		ways = append(ways, fmt.Sprintf("to keep it in range: state %s: T = … (in its initializer, %s is its current value)", name, name))
		ways = append(ways, "or write it in the handler that changes what it reads")
		c.errorf(e.decl, diag.EffectWritesWhatItFollows, "%s; %s", msg, strings.Join(ways, "; "))
	}
}

// isStateLike reports whether o is something a following state can say
// it follows: a state, a let, a prop or a parameter.
func isStateLike(o Object) bool {
	switch o := o.(type) {
	case *Member:
		switch o.Kind {
		case StateMember, LetMember, PropMember, ParamMember:
			return true
		}
	case *Global:
		return o.Kind == StateGlobal || o.Kind == MemoGlobal
	}
	return false
}

// checkKeys reports a for in a view over an array that reads what changes
// without a key (rfcs/0012): its rows are kept by position, so a row's
// state, tasks and views stay where they are when the items move. A
// constant array never moves; a set's or a range's rows are keyed by
// their items (ForInfo.KeyItem).
func (c *Checker) checkKeys() {
	for _, s := range c.viewFors {
		fi := c.info.Fors[s]
		if fi == nil || IsInvalid(fi.Elem) || !c.reactive(fi.Seq) {
			continue
		}
		item, index := "item", ""
		switch p := s.Pattern.(type) {
		case *syntax.BindPat:
			item = p.Name.Name
		case *syntax.TuplePat:
			if len(p.Elems) == 2 {
				if b, ok := p.Elems[0].(*syntax.BindPat); ok {
					index = b.Name.Name
				}
				if b, ok := p.Elems[1].(*syntax.BindPat); ok {
					item = b.Name.Name
				}
			}
		}
		seq := exprText(fi.Seq)
		if len(seq) > 24 {
			seq = "the items"
		}
		key := keySuggestion(fi.Elem, item)
		msg := fmt.Sprintf("rows are kept by position here, so a row's state stays where it is when %s move: say what identifies %s, key %s", seq, aType(fi.Elem), key)
		if index != "" {
			msg += fmt.Sprintf(" (or key %s, for the position on purpose)", index)
		}
		c.errorf(s, diag.RowWithoutKey, "%s", msg)
	}
}

// keySuggestion is what a key of item, an elem, would be: a field of a
// struct (its id, else a field named like one, else its first int or
// string field), the item itself when it's hashable, else a placeholder.
// A field, not the struct: a for keyed by a field path follows one
// signal for all its items (rfcs/0013), and a key that reads the item
// whole runs the for on any change inside an item.
func keySuggestion(elem Type, item string) string {
	if n, ok := elem.(*Named); ok && n.Obj.Kind == StructType {
		if f := keyField(n.Obj.Fields); f != "" {
			return item + "." + f
		}
	}
	if IsHashable(elem) {
		if n, ok := elem.(*Named); !ok || n.Obj.Kind != StructType {
			return item
		}
	}
	return item + ".<what identifies it>"
}

// keyField is the field of a struct that likely identifies it, or "".
func keyField(fields []*Field) string {
	keyish := func(t Type) bool { return t == Int || t == String }
	for _, f := range fields {
		if f.Name == "id" && keyish(f.Type) {
			return f.Name
		}
	}
	for _, f := range fields {
		low := strings.ToLower(f.Name)
		if keyish(f.Type) && (strings.HasSuffix(f.Name, "Id") || strings.HasSuffix(f.Name, "ID") || low == "key" || low == "uuid" || low == "slug") {
			return f.Name
		}
	}
	for _, f := range fields {
		if keyish(f.Type) {
			return f.Name
		}
	}
	return ""
}
