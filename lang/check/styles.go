package check

import (
	"strings"

	"github.com/egoist/asuka/lang/syntax"
)

// Styles (docs/language.md's "Styles", rfcs/0009): a style's body is
// checked once, where it's declared, its parameters and uses its own
// locals; an element's style: list is resolved where it's written, and
// lowered there (lang/ir/style.go), each style's props inlined with its
// arguments, each conditional entry a layer.

// A StyleDef is a declared style: style name { … } or style
// name(params) { … }.
type StyleDef struct {
	object
	Pkg    *Package
	Decl   *syntax.StyleDecl
	Pub    bool
	Params []*Param     // their Var is the parameter in the body (StyleLocal)
	Ctx    *FuncCtx     // its body's (CtxStyle)
	Items  []*StyleItem // its body, in order
	// Default is the element or component it's the default style of (a
	// style named after one): the package's elements of that kind take
	// it, under their style: list.
	Default *TypeName
	// Cyclic: it applies itself, directly or through others; it's never
	// expanded.
	Cyclic bool

	scope *Scope // its file's
	state uint8  // 0: unchecked, 1: being checked, 2: checked
	refs  []*StyleDef
}

// A StyleItem is one of a style's (or a style's variant's) body, in
// order: a prop, a style: list, a variant, or a use.
type StyleItem struct {
	Prop    *PropArg      // name: value; its Member is the style prop's
	Style   []*StyleEntry // style: …
	Variant *StyleVariant // pressed { … }
	Use     *Var          // use t: T
}

// A StyleVariant is a state's block in a style: laid over the view
// while it's in the state, on the elements that take variants.
type StyleVariant struct {
	State string // prelude.Variants' name
	Items []*StyleItem
	Node  *syntax.SlotItem
}

// A StyleEntry is one of a style: list's: a style applied (card,
// chip(.red)), or a condition choosing entries.
type StyleEntry struct {
	Node  syntax.Expr
	Style *StyleDef
	Args  []syntax.Expr // per parameter; nil: its default
	Cond  *StyleCond
}

// A StyleCond is an if or a match in a style: list: the entries of the
// branch it takes apply (none, for an if without an else that's false).
// Its conditions (an if's, a match's subject and guards) are one
// binding, Info.Ctxs[Node].
type StyleCond struct {
	Node     syntax.Expr   // an *syntax.IfExpr or a *syntax.MatchExpr
	Conds    []syntax.Expr // an if's, a branch's each (nil: the else)
	Subject  syntax.Expr   // a match's
	Arms     []*syntax.MatchArm
	Branches [][]*StyleEntry
}

// collectStyle declares a style of p's.
func (c *Checker) collectStyle(p *Package, d *syntax.StyleDecl) {
	def := &StyleDef{object: object{d.Name.Name, d.Name.Start}, Pkg: p, Decl: d, Pub: d.Pub.IsValid(), scope: c.declScope(d)}
	c.info.Defs[d.Name] = def
	if _, ok := c.styles[d.Name.Name]; ok {
		c.errorf(d.Name, "style %s is declared twice", d.Name.Name)
		return
	}
	c.styles[d.Name.Name] = def
	if p.Styles == nil {
		p.Styles = map[string]*StyleDef{}
	}
	p.Styles[d.Name.Name] = def
	p.styleOrder = append(p.styleOrder, def)
}

// checkStyles checks p's styles: their parameters, then their bodies,
// then that none applies itself.
func (c *Checker) checkStyles(p *Package) {
	for _, d := range p.styleOrder {
		c.styleSig(d)
	}
	for _, d := range p.styleOrder {
		c.styleBody(d)
	}
	for _, d := range p.styleOrder {
		if path := styleCycle(d, d, nil); path != nil {
			d.Cyclic = true
			names := make([]string, len(path)+1)
			names[0] = d.name
			for i, s := range path {
				names[i+1] = s.name
			}
			c.errorf(d.Decl.Name, "style %s applies itself (%s): a style can't include itself", d.name, strings.Join(names, " → "))
		}
	}
}

// styleCycle is the path from d back to start through the styles d
// applies, nil when there's none.
func styleCycle(start, d *StyleDef, seen map[*StyleDef]bool) []*StyleDef {
	if seen == nil {
		seen = map[*StyleDef]bool{}
	}
	for _, r := range d.refs {
		if r == start {
			return []*StyleDef{r}
		}
		if seen[r] || r.Pkg != start.Pkg {
			continue // another package's can't come back: imports don't cycle
		}
		seen[r] = true
		if p := styleCycle(start, r, seen); p != nil {
			return append([]*StyleDef{r}, p...)
		}
	}
	return nil
}

// styleSig resolves a style's parameters, and whether it's an element's
// or a component's default style.
func (c *Checker) styleSig(d *StyleDef) {
	savedScope := c.scope
	c.scope = d.scope
	if l := d.Decl.Params; l != nil {
		seen := map[string]bool{}
		for _, p := range l.List {
			if seen[p.Name.Name] {
				c.errorf(p.Name, "parameter %s is declared twice", p.Name.Name)
			}
			seen[p.Name.Name] = true
			var t Type = Invalid
			if p.Type != nil {
				t = c.typeExpr(p.Type)
			}
			d.Params = append(d.Params, &Param{Name: p.Name.Name, Type: t, Default: p.Default})
		}
	}
	if tn := c.scope.LookupType(d.name); tn != nil && tn.Comp != nil && !tn.Comp.Store && !tn.Comp.Style {
		d.Default = tn
		if d.Decl.Params != nil {
			c.errorf(d.Decl.Params, "style %s is %s's default style, and a default takes no parameters", d.name, d.name)
		}
		if c.pkg.defaults == nil {
			c.pkg.defaults = map[*TypeName]*StyleDef{}
		}
		c.pkg.defaults[tn] = d
		c.laterChecks = append(c.laterChecks, func() {
			if !c.TakesStyle(tn) || tn.Comp.Screen {
				c.errorf(d.Decl.Name, "style %s would be %s's default style, and %s doesn't take the style props: name it otherwise", d.name, d.name, d.name)
			}
		})
	}
	c.scope = savedScope
}

// styleBody checks a style's body: once, in its own context, where its
// parameters and uses are locals the lowering gives each application.
func (c *Checker) styleBody(d *StyleDef) {
	if d.state != 0 {
		return
	}
	d.state = 1
	savedScope, savedCtx := c.scope, c.ctx
	savedTry, savedAwait, savedHandled, savedLoops, savedView := c.inTry, c.inAwait, c.handled, c.loops, c.view
	c.inTry, c.inAwait, c.handled, c.loops, c.view = 0, 0, 0, 0, false
	c.scope = d.scope
	c.ctx = nil
	d.Ctx = c.enter(CtxStyle, d.Decl, "style "+d.name)
	for i, p := range d.Params {
		if p.Default != nil {
			c.styleArg(p.Default, p.Type, "parameter "+p.Name)
		}
		v := c.declareVar(d.Decl.Params.List[i].Name, p.Type, false)
		v.Kind = StyleLocal
		p.Var = v
		d.Ctx.Params = append(d.Ctx.Params, v)
	}
	d.Items = c.styleItems(d.Decl.Body.Items, d, "")
	c.leave(nil)
	c.scope, c.ctx = savedScope, savedCtx
	c.inTry, c.inAwait, c.handled, c.loops, c.view = savedTry, savedAwait, savedHandled, savedLoops, savedView
	d.state = 2
}

// styleItems checks a style's body, or (state set) one of its variants'.
func (c *Checker) styleItems(items []syntax.Item, d *StyleDef, state string) []*StyleItem {
	var out []*StyleItem
	var styleSet *syntax.PropSet
	for _, it := range items {
		switch s := it.(type) {
		case *syntax.PropSet:
			if s.Name.Name == "style" {
				if styleSet != nil {
					c.errorf(s.Name, "style: is set twice: put the styles in one list, style: [a, b]")
				}
				styleSet = s
				entries := c.styleList(s.Value, d)
				if state != "" {
					c.variantEntries(entries, state)
				}
				out = append(out, &StyleItem{Style: entries})
				continue
			}
			m := c.styleMember(s.Name.Name)
			switch {
			case m == nil:
				c.errorf(s.Name, "a style holds style props, and %s isn't one%s", s.Name.Name, c.propSuggest(c.styleComp(), s.Name.Name))
			case state != "" && (s.Name.Name == "keys" || s.Name.Name == "actions"):
				c.errorf(s.Name, "%s don't change with a state: set them outside %s { … }", s.Name.Name, state)
				m = nil
			}
			if m == nil {
				saved := c.ctx
				c.enter(CtxBinding, s.Value, "binding")
				c.expr(s.Value, nil)
				c.leave(saved)
				continue
			}
			c.propValue(s.Value, m, s.Name.Name)
			out = append(out, &StyleItem{Prop: &PropArg{Member: m, Value: s.Value, Style: true}})
		case *syntax.UseDecl:
			t := c.typeExpr(s.Type)
			if n, ok := t.(*Named); !ok || n.Obj.Kind != StoreType {
				if !IsInvalid(t) {
					c.errorf(s.Type, "use takes a store's type, provided by a component above: %s isn't one", typeString(t))
				}
			}
			if state != "" {
				c.errorf(s, "use goes in the style's body, not in %s { … }", state)
			}
			v := c.declareVar(s.Name, t, false)
			v.Kind = StyleLocal
			out = append(out, &StyleItem{Use: v})
		case *syntax.SlotItem:
			if !isVariant(s.Name.Name) && !c.variantDeclared(s.Name.Name) {
				c.errorf(s.Name, "a style holds style props (name: value), style: …, use and variants (pressed { … }, or a variant a component declares); no component this package sees declares variant %s", s.Name.Name)
				continue
			}
			if state != "" {
				c.errorf(s.Name, "%s { … } in %s { … }: a variant holds style props", s.Name.Name, state)
				continue
			}
			v := &StyleVariant{State: s.Name.Name, Node: s}
			v.Items = c.styleItems(s.Body.Items, d, s.Name.Name)
			out = append(out, &StyleItem{Variant: v})
		case *syntax.StyleUse:
			c.errorf(s, "write style: %s: a style applies others with style:, as an element does (asuka fmt rewrites it)", styleName(s.Name))
		case *syntax.BadItem:
		default:
			c.errorf(it, "a style holds style props (name: value), style: …, use and variants (pressed { … }), not %s", itemName(it))
		}
	}
	return out
}

func styleName(x syntax.Expr) string {
	switch n := x.(type) {
	case *syntax.Ident:
		return n.Name
	case *syntax.MemberExpr:
		return styleName(n.X) + "." + n.Name.Name
	}
	return "…"
}

// variantEntries checks a variant's style: list: styles without
// conditions or variants of their own (it's laid on with the state).
func (c *Checker) variantEntries(entries []*StyleEntry, state string) {
	for _, e := range entries {
		if e.Cond != nil {
			c.errorf(e.Node, "the style: of %s { … } takes no conditions: put the condition around the whole style instead", state)
			continue
		}
		d := e.Style
		c.laterChecks = append(c.laterChecks, func() {
			if styleHasVariants(d, nil) {
				c.errorf(e.Node, "style %s holds variants, and %s { … } can't: apply it outside", d.name, state)
			}
		})
	}
}

// styleHasVariants reports whether d, or a style it applies, holds a
// variant.
func styleHasVariants(d *StyleDef, seen map[*StyleDef]bool) bool {
	if d == nil || d.Cyclic {
		return false
	}
	if seen == nil {
		seen = map[*StyleDef]bool{}
	}
	if seen[d] {
		return false
	}
	seen[d] = true
	var entries func([]*StyleEntry) bool
	entries = func(list []*StyleEntry) bool {
		for _, e := range list {
			if e.Style != nil && styleHasVariants(e.Style, seen) {
				return true
			}
			if e.Cond != nil {
				for _, b := range e.Cond.Branches {
					if entries(b) {
						return true
					}
				}
			}
		}
		return false
	}
	for _, it := range d.Items {
		if it.Variant != nil || entries(it.Style) {
			return true
		}
	}
	return false
}

// styleList checks a style: value: a style, a list of them, an if or a
// match choosing them. owner is the style whose body it's in (nil for an
// element's).
func (c *Checker) styleList(x syntax.Expr, owner *StyleDef) []*StyleEntry {
	switch e := x.(type) {
	case *syntax.ParenExpr:
		return c.styleList(e.X, owner)
	case *syntax.ArrayExpr:
		var out []*StyleEntry
		for _, el := range e.Elems {
			out = append(out, c.styleList(el, owner)...)
		}
		return out
	case *syntax.Ident, *syntax.MemberExpr:
		d := c.styleNamed(e)
		if d == nil {
			return nil
		}
		c.styleRef(owner, d)
		for _, p := range d.Params {
			if p.Default == nil {
				c.errorf(e, "style %s takes arguments: %s(…)", d.name, styleName(e))
				return nil
			}
		}
		return []*StyleEntry{{Node: e, Style: d, Args: make([]syntax.Expr, len(d.Params))}}
	case *syntax.CallExpr:
		d := c.styleNamed(e.Fun)
		if d == nil {
			if e.Args != nil {
				for _, a := range e.Args.List {
					c.expr(a.Value, nil)
				}
			}
			return nil
		}
		c.styleRef(owner, d)
		if d.Decl.Params == nil {
			c.errorf(e, "style %s takes no arguments: style: %s", d.name, styleName(e.Fun))
			return nil
		}
		if e.Block != nil {
			c.errorf(e.Block, "a style's call takes its arguments in the parentheses")
		}
		args := c.matchArgs(e, d.Params, "style "+d.name)
		for i, a := range args {
			if a != nil {
				c.styleArg(a, d.Params[i].Type, "parameter "+d.Params[i].Name)
			}
		}
		return []*StyleEntry{{Node: e, Style: d, Args: args}}
	case *syntax.IfExpr:
		return []*StyleEntry{{Node: e, Cond: c.styleIf(e, owner)}}
	case *syntax.MatchExpr:
		return []*StyleEntry{{Node: e, Cond: c.styleMatch(e, owner)}}
	}
	c.errorf(x, "a style list holds styles: a style's name (card, theme.card), its call (chip(.red)), an if or a match choosing them")
	saved := c.ctx
	c.enter(CtxBinding, x, "binding")
	c.expr(x, nil)
	c.leave(saved)
	return nil
}

// styleRef notes that owner applies d (for the cycle check).
func (c *Checker) styleRef(owner, d *StyleDef) {
	if owner != nil {
		for _, r := range owner.refs {
			if r == d {
				return
			}
		}
		owner.refs = append(owner.refs, d)
	}
}

// styleNamed finds the style name names (card, or theme.card: a pub
// style of a package the file imports); nil after an error.
func (c *Checker) styleNamed(name syntax.Expr) *StyleDef {
	switch n := name.(type) {
	case *syntax.Ident:
		d := c.styles[n.Name]
		if d == nil {
			if o := c.lookup(n.Name); o != nil {
				if _, ok := o.(*TypeName); !ok {
					c.errorf(n, "%s isn't a style: a style list holds styles (for one while %s holds, if %s { card })", n.Name, n.Name, n.Name)
					return nil
				}
			}
			c.errorf(n, "unknown style %s%s", n.Name, c.styleSuggest(n.Name))
			return nil
		}
		c.info.Uses[n] = d
		return d
	case *syntax.MemberExpr:
		id, ok := n.X.(*syntax.Ident)
		pn, _ := c.lookup(idName(id, ok)).(*PkgName)
		if !ok || pn == nil {
			c.errorf(n.X, "a style is named name, or pkg.name for another package's")
			return nil
		}
		c.info.Uses[id] = pn
		d := pn.Pkg.Styles[n.Name.Name]
		if d == nil {
			c.errorf(n.Name, "%s has no style %s", pn.Pkg.Path, n.Name.Name)
			return nil
		}
		if !d.Pub {
			c.errorf(n.Name, "style %s isn't pub in %s", n.Name.Name, pn.Pkg.Path)
			return nil
		}
		c.info.Uses[n.Name] = d
		return d
	}
	c.errorf(name, "a style is named name, or pkg.name for another package's")
	return nil
}

func (c *Checker) styleSuggest(name string) string {
	best, bestD := "", 3
	for n := range c.styles {
		if d := editDistance(name, n); d < len(name) && (d < bestD || d == bestD && n < best) {
			best, bestD = n, d
		}
	}
	if best != "" {
		return " (did you mean " + best + "?)"
	}
	return ""
}

// styleArg checks a style's argument (or a parameter's default): a
// binding, which the lowering evaluates once per application (a memo
// when it reads what changes).
func (c *Checker) styleArg(x syntax.Expr, t Type, what string) {
	saved := c.ctx
	c.enter(CtxBinding, x, "argument")
	c.assign(x, t, what)
	c.leave(saved)
}

// styleIf checks an if in a style list: its conditions are one binding;
// its branches, entries.
func (c *Checker) styleIf(e *syntax.IfExpr, owner *StyleDef) *StyleCond {
	sc := &StyleCond{Node: e}
	var chain []*syntax.IfExpr
	var els *syntax.Block
	for x := e; x != nil; {
		chain = append(chain, x)
		switch el := x.Else.(type) {
		case *syntax.IfExpr:
			x = el
		case *syntax.Block:
			els = el
			x = nil
		default:
			x = nil
		}
	}
	saved := c.ctx
	c.enter(CtxBinding, e, "style condition")
	for _, x := range chain {
		if hasLetCond(x.Cond) {
			c.errorf(x.Cond, "a style list's if takes a bool: if let names a value the list can't use")
			continue
		}
		c.cond(x.Cond)
		sc.Conds = append(sc.Conds, x.Cond)
	}
	c.leave(saved)
	if len(sc.Conds) != len(chain) {
		return nil
	}
	for _, x := range chain {
		sc.Branches = append(sc.Branches, c.styleBranch(x.Then, owner))
	}
	if els != nil {
		sc.Conds = append(sc.Conds, nil)
		sc.Branches = append(sc.Branches, c.styleBranch(els, owner))
	}
	return sc
}

func hasLetCond(x syntax.Expr) bool {
	switch e := x.(type) {
	case *syntax.LetCond:
		return true
	case *syntax.CondList:
		for _, y := range e.List {
			if hasLetCond(y) {
				return true
			}
		}
	}
	return false
}

// styleBranch checks a branch of a style list's if or match: a style, a
// list, or nothing.
func (c *Checker) styleBranch(b *syntax.Block, owner *StyleDef) []*StyleEntry {
	switch {
	case len(b.Items) == 0:
		return nil
	case len(b.Items) == 1:
		if es, ok := b.Items[0].(*syntax.ExprStmt); ok {
			return c.styleList(es.X, owner)
		}
	}
	c.errorf(b, "a branch of a style list holds the styles it applies: a style, or a list of them")
	return nil
}

// styleMatch checks a match in a style list: its subject and guards are
// one binding; its arms bind no names; their bodies are entries.
func (c *Checker) styleMatch(e *syntax.MatchExpr, owner *StyleDef) *StyleCond {
	sc := &StyleCond{Node: e, Subject: e.Subject, Arms: e.Arms}
	saved := c.ctx
	c.enter(CtxBinding, e, "style condition")
	t := c.expr(e.Subject, nil)
	c.info.Matches[e] = &MatchInfo{Subject: t}
	ok := true
	for _, a := range e.Arms {
		c.openScope()
		c.pattern(a.Pattern, t, LocalVar)
		if len(c.scope.Names()) > 0 {
			c.errorf(a.Pattern, "a style list's match binds no names: match the case alone (.tint, not .tint(c))")
			ok = false
		}
		if a.Guard != nil {
			c.assign(a.Guard, Bool, "the guard")
		}
		c.closeScope()
	}
	c.leave(saved)
	if !ok {
		return nil
	}
	for _, a := range e.Arms {
		switch b := a.Body.(type) {
		case *syntax.Block:
			sc.Branches = append(sc.Branches, c.styleBranch(b, owner))
		case syntax.Expr:
			sc.Branches = append(sc.Branches, c.styleList(b, owner))
		default:
			sc.Branches = append(sc.Branches, nil)
		}
	}
	return sc
}

// takesStyleProps reports whether an element or a component takes the
// style props (and so style:).
func (c *Checker) takesStyleProps(ec *ElementCall) bool {
	comp := ec.Comp.Comp
	if comp.Native {
		return !comp.Style && !(comp.Element != nil && comp.Element.NoStyle)
	}
	return c.TakesStyle(ec.Comp)
}

// styleProp checks an element's style: list.
func (c *Checker) styleProp(s *syntax.PropSet, ec *ElementCall) {
	if ec.StyleNode != nil {
		c.errorf(s.Name, "style: is set twice: put the styles in one list, style: [a, b]")
	}
	ec.StyleNode = s
	entries := c.styleList(s.Value, nil)
	if !c.takesStyleProps(ec) {
		hint := ""
		if !ec.Comp.Comp.Native {
			hint = ": a component takes them when its body is one element"
		}
		c.errorf(s.Name, "%s doesn't take the style props%s", ec.Comp.name, hint)
		return
	}
	ec.Style = append(ec.Style, entries...)
}

// defaultStyle puts the package's default style for an element's kind
// under its style: list.
func (c *Checker) defaultStyle(ec *ElementCall) {
	d := c.pkg.defaults[ec.Comp]
	if d == nil || !c.takesStyleProps(ec) {
		return
	}
	ec.Style = append([]*StyleEntry{{Style: d, Args: []syntax.Expr{}}}, ec.Style...)
}

// StyleTakesVariants reports whether the view a component's caller's
// style goes on takes variants: a container, or a component whose one
// element is one.
func (p *Program) StyleTakesVariants(t *TypeName) bool { return p.checker.styleTakesVariants(t) }

// RootComponent is the component (or element) t's body shows when its
// views are that one call, nil otherwise.
func (p *Program) RootComponent(t *TypeName) *TypeName {
	if t == nil || t.Comp == nil || t.Comp.Native {
		return nil
	}
	return p.checker.rootComponent(t)
}

func (c *Checker) styleTakesVariants(t *TypeName) bool {
	for i := 0; t != nil && i < 64; i++ {
		comp := t.Comp
		if comp == nil {
			return false
		}
		if comp.Native {
			return comp.Element != nil && comp.Element.Variants
		}
		if !c.TakesStyle(t) {
			return false
		}
		t = c.rootComponent(t)
	}
	return false
}

// variantDeclared reports whether a component this package sees declares
// variant name: one of its own components (any), or another package's
// (a pub one), so a style's name { … } names one.
func (c *Checker) variantDeclared(name string) bool {
	pkgs := append([]*Package{c.pkg}, c.pkgs...)
	for _, p := range pkgs {
		if p == nil {
			continue
		}
		for _, t := range p.Types {
			if t.Comp == nil {
				continue
			}
			if v := t.Comp.Variant(name); v != nil && (v.Pub || p == c.pkg) {
				return true
			}
		}
	}
	return false
}
