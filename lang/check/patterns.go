package check

import (
	"strings"

	"github.com/egoist/asuka/lang/syntax"
)

// ---------------------------------------------------------------------
// Struct patterns: P(x: 0, y), a struct's fields matched by name.

// patternStruct is the struct a case pattern's path names (P, pkg.P),
// nil when it names none (it's a case then).
func (c *Checker) patternStruct(p *syntax.CasePat) *TypeName {
	if p.Implicit {
		return nil
	}
	var o Object
	switch len(p.Path) {
	case 1:
		o = c.lookup(p.Path[0].Name)
	case 2:
		if pn, ok := c.lookup(p.Path[0].Name).(*PkgName); ok {
			o = pn.Pkg.Scope.LookupLocal(p.Path[1].Name)
			if tn, ok := o.(*TypeName); ok && tn.Kind == StructType {
				c.info.Uses[p.Path[0]] = pn
			}
		}
	}
	tn, ok := o.(*TypeName)
	if !ok || tn.Kind != StructType {
		return nil
	}
	c.resolveType(tn)
	c.info.Uses[p.Path[len(p.Path)-1]] = tn
	return tn
}

// structPattern checks P(x: 0, y) against values of type t: each
// argument names a field (y alone is y: y, a binding of it), and the
// fields it doesn't name match anything.
func (c *Checker) structPattern(p *syntax.CasePat, tn *TypeName, t Type) {
	n, ok := Unopt(t).(*Named)
	if !ok || n.Obj != tn {
		if !IsInvalid(t) {
			c.errorf(p, "a %s can't be a %s", typeString(t), tn.name)
		}
		for _, a := range p.Args {
			c.pattern(a, Invalid, LocalVar)
		}
		return
	}
	kind := LocalVar
	if c.view {
		kind = ViewMemo
	}
	s := n.Subst()
	fields := make([]*Field, len(p.Args))
	seen := map[string]bool{}
	for i, a := range p.Args {
		name := ""
		at := syntax.Node(a)
		if l := p.Label(i); l != nil {
			name, at = l.Name, l
		} else if bp, ok := a.(*syntax.BindPat); ok {
			name = bp.Name.Name
		} else {
			switch a.(type) {
			case *syntax.WildcardPat:
				c.errorf(a, "a struct pattern names the fields it matches: leave out a field to match any")
			default:
				c.errorf(a, "a struct pattern names the fields it matches: write field: pattern (%s(x: …))", tn.name)
			}
			c.pattern(a, Invalid, kind)
			continue
		}
		f := tn.Field(name)
		if f == nil {
			c.errorf(at, "%s has no field %s%s", tn.name, name, c.fieldSuggest(tn, name))
			c.pattern(a, Invalid, kind)
			continue
		}
		if seen[name] {
			c.errorf(at, "%s is matched twice in this pattern", name)
		}
		seen[name] = true
		fields[i] = f
		c.pattern(a, s.Apply(f.Type), kind)
	}
	c.info.FieldPats[p] = fields
}

// fieldSuggest is ": did you mean x?" for a field name close to one of
// tn's, "" when none is.
func (c *Checker) fieldSuggest(tn *TypeName, name string) string {
	best, bestD := "", 3
	for _, f := range tn.Fields {
		if d := editDistance(name, f.Name); d < bestD && d < len(name) {
			best, bestD = f.Name, d
		}
	}
	if best == "" {
		return ""
	}
	return ": did you mean " + best + "?"
}

// ---------------------------------------------------------------------
// Exhaustiveness: the arms' patterns (those without guards) checked as
// Maranget's "Warnings for pattern matching" does, through nested
// patterns: an enum's cases and their payloads, a struct's fields,
// tuples, optionals and bools. What's missing is said as patterns.

// A space is a pattern normalized for the check: a constructor and its
// arguments, anything (ctor ""), or alternatives.
type space struct {
	ctor string
	args []*space
	alts []*space
}

var anything = &space{}

// ctor is a type's constructor: its name and its arguments' types.
type ctor struct {
	name string
	args []Type
}

// ctors lists t's constructors when they're finitely many (an
// optional's none and some, a bool's, an enum's cases, a struct's or a
// tuple's one), nil when they aren't (ints, strings, ...).
func (c *Checker) ctors(t Type) []ctor {
	switch x := t.(type) {
	case *Optional:
		return []ctor{{"none", nil}, {"some", []Type{x.Elem}}}
	case *Tuple:
		return []ctor{{"()", x.Elems}}
	case *Named:
		s := x.Subst()
		switch x.Obj.Kind {
		case EnumType:
			out := make([]ctor, len(x.Obj.Cases))
			for i, k := range x.Obj.Cases {
				args := make([]Type, len(k.Params))
				for j, p := range k.Params {
					args[j] = s.Apply(p)
				}
				out[i] = ctor{"." + k.Name, args}
			}
			return out
		case StructType:
			args := make([]Type, len(x.Obj.Fields))
			for i, f := range x.Obj.Fields {
				args[i] = s.Apply(f.Type)
			}
			return []ctor{{"struct", args}}
		}
	}
	if t == Bool {
		return []ctor{{"true", nil}, {"false", nil}}
	}
	return nil
}

// space normalizes p, a pattern of values of type t.
func (c *Checker) space(p syntax.Pattern, t Type) *space {
	if opt, ok := t.(*Optional); ok {
		switch p := p.(type) {
		case *syntax.WildcardPat, *syntax.BindPat, *syntax.BadPat, nil:
			return anything
		case *syntax.LitPat:
			if _, none := p.Value.(*syntax.NoneLit); none {
				return &space{ctor: "none"}
			}
		case *syntax.OrPat:
			s := &space{}
			for _, a := range p.Alts {
				s.alts = append(s.alts, c.space(a, t))
			}
			return s
		}
		return &space{ctor: "some", args: []*space{c.space(p, opt.Elem)}}
	}
	switch p := p.(type) {
	case *syntax.LitPat:
		switch v := p.Value.(type) {
		case *syntax.BoolLit:
			return &space{ctor: strconvBool(v.Value)}
		case *syntax.BasicLit:
			return &space{ctor: "=" + v.Value}
		}
		return &space{ctor: "=" + exprName(p.Value)}
	case *syntax.TuplePat:
		tt, ok := t.(*Tuple)
		if !ok || len(tt.Elems) != len(p.Elems) {
			return anything
		}
		s := &space{ctor: "()"}
		for i, e := range p.Elems {
			s.args = append(s.args, c.space(e, tt.Elems[i]))
		}
		return s
	case *syntax.OrPat:
		s := &space{}
		for _, a := range p.Alts {
			s.alts = append(s.alts, c.space(a, t))
		}
		return s
	case *syntax.CasePat:
		n, _ := t.(*Named)
		if n == nil {
			return anything
		}
		if fields, ok := c.info.FieldPats[p]; ok {
			s := &space{ctor: "struct", args: make([]*space, len(n.Obj.Fields))}
			for i := range s.args {
				s.args[i] = anything
			}
			sub := n.Subst()
			for i, f := range fields {
				if f != nil {
					s.args[f.Index] = c.space(p.Args[i], sub.Apply(f.Type))
				}
			}
			return s
		}
		k := c.info.Cases[p]
		if k == nil {
			return anything
		}
		s := &space{ctor: "." + k.Name, args: make([]*space, len(k.Params))}
		sub := n.Subst()
		for i := range s.args {
			s.args[i] = anything
			if p.Lparen.IsValid() && i < len(p.Args) {
				s.args[i] = c.space(p.Args[i], sub.Apply(k.Params[i]))
			}
		}
		return s
	}
	return anything
}

// exhaust is one exhaustiveness check: its budget of witnesses.
type exhaust struct {
	c    *Checker
	work int
}

const maxWitnesses = 8

// missing lists values (as spaces, one per column) no row matches: rows
// are the rows' spaces, tys the columns' types.
func (x *exhaust) missing(rows [][]*space, tys []Type) [][]*space {
	x.work++
	if len(tys) == 0 {
		if len(rows) == 0 {
			return [][]*space{{}}
		}
		return nil
	}
	if x.work > 10000 {
		return nil // too big to tell: say nothing rather than guess
	}
	rows = expandAlts(rows)
	t := tys[0]
	cs := x.c.ctors(t)
	if cs == nil || IsInvalid(t) {
		ws := x.missing(defaults(rows), tys[1:])
		for i, w := range ws {
			ws[i] = append([]*space{anything}, w...)
		}
		return ws
	}
	used := map[string]bool{}
	for _, r := range rows {
		if r[0].ctor != "" {
			used[r[0].ctor] = true
		}
	}
	var out [][]*space
	var dws [][]*space
	defDone := false
	for _, k := range cs {
		if len(out) >= maxWitnesses {
			break
		}
		if used[k.name] {
			ws := x.missing(specialize(rows, k.name, len(k.args)), append(append([]Type{}, k.args...), tys[1:]...))
			for _, w := range ws {
				head := &space{ctor: k.name, args: w[:len(k.args)]}
				out = append(out, append([]*space{head}, w[len(k.args):]...))
			}
			continue
		}
		if !defDone {
			dws, defDone = x.missing(defaults(rows), tys[1:]), true
		}
		for _, w := range dws {
			head := &space{ctor: k.name, args: make([]*space, len(k.args))}
			for i := range head.args {
				head.args[i] = anything
			}
			out = append(out, append([]*space{head}, w...))
		}
	}
	return out
}

// expandAlts makes a row per alternative of a row's first space.
func expandAlts(rows [][]*space) [][]*space {
	var out [][]*space
	var add func(r []*space)
	add = func(r []*space) {
		if len(r[0].alts) == 0 {
			out = append(out, r)
			return
		}
		for _, a := range r[0].alts {
			add(append([]*space{a}, r[1:]...))
		}
	}
	for _, r := range rows {
		add(r)
	}
	return out
}

// specialize keeps the rows that match constructor k first, its
// arguments in place of it.
func specialize(rows [][]*space, k string, arity int) [][]*space {
	var out [][]*space
	for _, r := range rows {
		switch r[0].ctor {
		case "":
			row := make([]*space, 0, arity+len(r)-1)
			for i := 0; i < arity; i++ {
				row = append(row, anything)
			}
			out = append(out, append(row, r[1:]...))
		case k:
			row := append(append([]*space{}, r[0].args...), r[1:]...)
			for len(row) < arity+len(r)-1 {
				row = append([]*space{anything}, row...)
			}
			out = append(out, row)
		}
	}
	return out
}

// defaults keeps the rows that match anything first, without it.
func defaults(rows [][]*space) [][]*space {
	var out [][]*space
	for _, r := range rows {
		if r[0].ctor == "" {
			out = append(out, r[1:])
		}
	}
	return out
}

// exhaustive reports whether arms cover every value of t, and what they
// miss.
func (c *Checker) exhaustive(subject syntax.Expr, t Type, arms []*syntax.MatchArm) (bool, []string) {
	if IsInvalid(t) {
		return true, nil
	}
	var rows [][]*space
	if IsPlatformConst(subject) {
		// @platform is never .unknown: a match of it needn't say.
		rows = append(rows, []*space{{ctor: ".unknown"}})
	}
	for _, a := range arms {
		if a.Guard != nil {
			continue
		}
		rows = append(rows, []*space{c.space(a.Pattern, t)})
	}
	x := &exhaust{c: c}
	ws := x.missing(rows, []Type{t})
	if len(ws) == 0 {
		return true, nil
	}
	var missing []string
	seen := map[string]bool{}
	for _, w := range ws {
		s := c.witness(w[0], t, true)
		if !seen[s] {
			seen[s] = true
			missing = append(missing, s)
		}
	}
	if len(ws) >= maxWitnesses {
		missing = append(missing, "…")
	}
	return false, missing
}

// witness says a missing value: a pattern that would match it. A value
// of a type with infinitely many is "the rest" at the top, _ inside.
func (c *Checker) witness(s *space, t Type, top bool) string {
	if s.ctor == "" {
		if top {
			return "the rest"
		}
		return "_"
	}
	switch s.ctor {
	case "none", "true", "false":
		return s.ctor
	case "some":
		return c.witness(s.args[0], Unopt(t), top)
	case "()":
		tt, _ := t.(*Tuple)
		var parts []string
		for i, a := range s.args {
			var et Type = Invalid
			if tt != nil && i < len(tt.Elems) {
				et = tt.Elems[i]
			}
			parts = append(parts, c.witness(a, et, false))
		}
		return "(" + strings.Join(parts, ", ") + ")"
	case "struct":
		n, _ := t.(*Named)
		if n == nil {
			return "_"
		}
		var parts []string
		sub := n.Subst()
		for i, a := range s.args {
			if a.ctor == "" || i >= len(n.Obj.Fields) {
				continue
			}
			f := n.Obj.Fields[i]
			parts = append(parts, f.Name+": "+c.witness(a, sub.Apply(f.Type), false))
		}
		if len(parts) == 0 {
			if top {
				return "the rest"
			}
			return "_"
		}
		return n.Obj.name + "(" + strings.Join(parts, ", ") + ")"
	}
	if strings.HasPrefix(s.ctor, ".") {
		n, _ := t.(*Named)
		all := true
		for _, a := range s.args {
			if a.ctor != "" {
				all = false
			}
		}
		if all || n == nil {
			return s.ctor
		}
		k := n.Obj.Case(s.ctor[1:])
		sub := n.Subst()
		var parts []string
		for i, a := range s.args {
			var at Type = Invalid
			if k != nil && i < len(k.Params) {
				at = sub.Apply(k.Params[i])
			}
			parts = append(parts, c.witness(a, at, false))
		}
		return s.ctor + "(" + strings.Join(parts, ", ") + ")"
	}
	return "_"
}

func strconvBool(b bool) string {
	if b {
		return "true"
	}
	return "false"
}
