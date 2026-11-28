package check

import (
	"strings"

	"github.com/egoist/asuka/lang/diag"
	"github.com/egoist/asuka/lang/prelude"
	"github.com/egoist/asuka/lang/syntax"
)

// componentMembers declares a component's, a screen's, a store's or an
// element's header parameters and members.
func (c *Checker) componentMembers(t *TypeName, params *syntax.ParamList, body *syntax.Block) {
	comp := t.Comp
	if comp.Worker {
		c.workerMembers(t, params, body)
		return
	}
	comp.Members = map[string]*Member{}
	kind := CtxComponent
	if comp.Store {
		kind = CtxStore
	}
	ctx := &FuncCtx{Kind: kind, Node: t.Decl, Name: t.name, Comp: t, Result: View, Async: false}
	if comp.Store {
		ctx.Result = t.Type
	}
	ctx.scope = NewScope(c.scope)
	comp.Ctx = ctx
	c.info.Ctxs[t.Decl] = ctx
	add := func(id *syntax.Ident, m *Member) {
		m.object = object{id.Name, id.Start}
		m.Owner = t
		c.info.Defs[id] = m
		if c.builtinForm(id) {
			return
		}
		if old := comp.Members[id.Name]; old != nil {
			c.errorf(id, diag.DuplicateDeclaration, "%s is declared twice in %s", id.Name, t.name)
			return
		}
		comp.Members[id.Name] = m
		comp.Order = append(comp.Order, m)
		ctx.scope.Insert(m)
	}
	savedScope := c.scope
	c.scope = ctx.scope
	defer func() { c.scope = savedScope }()
	if params != nil {
		for _, p := range params.List {
			if p.Type == nil {
				c.errorf(p.Name, diag.MissingType, "parameter %s needs a type: %s: Type", p.Name.Name, p.Name.Name)
			}
			m := &Member{Kind: ParamMember, Type: c.typeExpr(p.Type), Default: p.Default, Decl: p, Pub: true}
			add(p.Name, m)
			comp.Params = append(comp.Params, m)
		}
	}
	if body == nil {
		return
	}
	for _, it := range body.Items {
		switch d := it.(type) {
		case *syntax.PropDecl:
			if comp.Store {
				c.errorf(d, diag.MisplacedDeclaration, "a store has no props: give it parameters, store %s(name: T)", t.name)
				continue
			}
			if d.Name.Name == "style" {
				c.errorf(d.Name, diag.ReservedName, "style is the style list every element takes (style: card): name the prop otherwise")
			}
			m := &Member{Kind: PropMember, Default: d.Default, Decl: d, Pub: true}
			if d.Type != nil {
				m.Type = c.typeExpr(d.Type)
			} else if d.Default == nil {
				c.errorf(d.Name, diag.MissingType, "prop %s needs a type or a default", d.Name.Name)
				m.Type = Invalid
			}
			add(d.Name, m)
			comp.Props = append(comp.Props, m)
		case *syntax.EventDecl:
			if comp.Store {
				c.errorf(d, diag.MisplacedDeclaration, "a store has no events")
				continue
			}
			m := &Member{Kind: EventMember, Type: Void, Decl: d, Pub: true}
			if d.Params != nil {
				for _, p := range d.Params.List {
					m.Params = append(m.Params, &Param{Name: p.Name.Name, Type: c.typeExpr(p.Type)})
				}
			}
			add(d.Name, m)
			comp.Events = append(comp.Events, m)
		case *syntax.StateDecl:
			m := &Member{Kind: StateMember, Default: d.Value, Decl: d, Pub: d.Pub.IsValid()}
			if d.Type != nil {
				m.Type = c.typeExpr(d.Type)
			}
			add(d.Name, m)
		case *syntax.LetDecl:
			c.noLetElse(d)
			bp, ok := d.Pattern.(*syntax.BindPat)
			if !ok {
				c.errorf(d.Pattern, diag.InvalidDeclaration, "a component's let names one value: let name = …")
				continue
			}
			if d.Keyword == syntax.VAR {
				c.errorf(d, diag.InvalidDeclaration, "a component's body is declarations: a value that changes is state (state %s = …)", bp.Name.Name)
			}
			m := &Member{Kind: LetMember, Default: d.Value, Decl: d, Pub: d.Pub.IsValid()}
			if d.Type != nil {
				m.Type = c.typeExpr(d.Type)
			}
			add(bp.Name, m)
		case *syntax.UseDecl:
			// use s: Store? is none when nothing above provides one.
			m := &Member{Kind: UseMember, Type: c.typeExpr(d.Type), Decl: d}
			if n, ok := Unopt(m.Type).(*Named); !ok || n.Obj.Kind != StoreType {
				if !IsInvalid(m.Type) {
					c.errorf(d.Type, diag.TypeMismatch, "use takes a store's type (or an optional one, none when nothing provides it), provided by a component above: %s isn't one", typeString(m.Type))
				}
			}
			add(d.Name, m)
		case *syntax.FuncDecl:
			f := &Function{object: object{d.Name.Name, d.Name.Start}, Pkg: t.Pkg, Decl: d, Recv: t}
			m := &Member{Kind: FnMember, Decl: d, Fn: f, Pub: d.Pub.IsValid()}
			add(d.Name, m)
			if t.Methods == nil {
				t.Methods = map[string]*Function{}
			}
			t.Methods[f.name] = f
			c.funcSig(f)
			m.Type = f.Sig.Type()
		case *syntax.ProvideDecl:
			comp.Provides = append(comp.Provides, d)
		case *syntax.VariantDecl:
			switch {
			case comp.Store:
				c.errorf(d, diag.MisplacedDeclaration, "a store has no variants: a component declares them, for its caller's styles")
				continue
			case comp.Screen:
				c.errorf(d, diag.MisplacedDeclaration, "a screen has no variants: a component declares them, for its caller's styles")
				continue
			case isVariant(d.Name.Name):
				c.errorf(d.Name, diag.ReservedName, "%s is an interaction state, which every view that takes variants has: name the variant otherwise", d.Name.Name)
				continue
			case syntax.Lookup(d.Name.Name).IsKeyword():
				c.errorf(d.Name, diag.ReservedName, "%s is a keyword, so %s { … } can't name the variant in a block: name it otherwise", d.Name.Name, d.Name.Name)
				continue
			case comp.Variant(d.Name.Name) != nil:
				c.errorf(d.Name, diag.DuplicateDeclaration, "variant %s is declared twice in %s", d.Name.Name, t.name)
				continue
			}
			v := &VariantDef{object: object{d.Name.Name, d.Name.Start}, Pub: d.Pub.IsValid(), Value: d.Value, Decl: d, Owner: t, Index: len(comp.Variants)}
			c.info.Defs[d.Name] = v
			comp.Variants = append(comp.Variants, v)
		}
	}
	// A native component's (an element's) View prop: its children.
}

// memberType is a member's type, inferring it from its value when it has
// no type written.
func (c *Checker) memberType(m *Member) Type {
	if m.Type != nil {
		return m.Type
	}
	if c.resolving[m] {
		if m.Kind == StateMember {
			c.errorf(m.Decl, diag.MissingType, "%s reads itself (its current value, none the first time), so its type must be written: state %s: T = …", m.name, m.name)
		} else {
			c.errorf(m.Decl, diag.RecursiveDefinition, "%s's value is defined in terms of itself", m.name)
		}
		m.Type = Invalid
		return m.Type
	}
	c.resolving[m] = true
	defer delete(c.resolving, m)
	c.memberValue(m)
	if m.Type == nil {
		m.Type = Invalid
	}
	return m.Type
}

// memberValue checks a member's value (a state's initial value, a let's
// binding, a prop's default) and infers its type.
func (c *Checker) memberValue(m *Member) {
	if m.Default == nil || c.info.Types[m.Default] != nil && m.Type != nil {
		return
	}
	comp := m.Owner.Comp
	savedScope, savedCtx, savedPkg := c.scope, c.ctx, c.pkg
	savedTry, savedAwait, savedHandled, savedView, savedLoad := c.inTry, c.inAwait, c.handled, c.view, c.loadOK
	c.inTry, c.inAwait, c.handled, c.view = 0, 0, 0, false
	c.pkg = m.Owner.Pkg
	c.scope = comp.Ctx.scope
	c.ctx = comp.Ctx
	kind := CtxComponent
	switch {
	case comp.Worker:
		// Made on the worker's thread as it starts: plain values.
		kind = CtxWorker
	case m.Kind == LetMember:
		kind = CtxBinding
		c.loadOK = true
	case m.Kind == PropMember, m.Kind == ParamMember:
		kind = CtxBinding
	case m.Kind == StateMember:
		// A state follows what its initializer reads (rfcs/0012): the
		// initializer is a binding, run again when that changes.
		kind = CtxBinding
		if d, ok := m.Decl.(*syntax.StateDecl); ok {
			c.deps(d.Deps)
		}
	}
	if kind == CtxBinding {
		c.enter(kind, m.Default, m.name)
	}
	var self *Var
	if m.Kind == StateMember {
		self = c.stateSelf(m.name, m.pos, m.Type)
	}
	var t Type
	if m.Type != nil {
		c.assign(m.Default, m.Type, "the value of "+m.name)
		t = m.Type
	} else {
		t = c.valueType(c.expr(m.Default, nil), m.Default)
	}
	m.Self = c.selfUsed(self, m.Default)
	if kind == CtxBinding {
		c.leave(comp.Ctx)
	}
	c.scope, c.ctx, c.pkg = savedScope, savedCtx, savedPkg
	c.inTry, c.inAwait, c.handled, c.view, c.loadOK = savedTry, savedAwait, savedHandled, savedView, savedLoad
	if m.Type == nil {
		m.Type = t
	}
}

// variantDecls checks a component's variants: each a bool binding of its
// body's, on the one element it shows, whose name no slot has.
func (c *Checker) variantDecls(t *TypeName) {
	comp := t.Comp
	if len(comp.Variants) == 0 {
		return
	}
	for _, v := range comp.Variants {
		if m := comp.Members[v.name]; m != nil && m.IsSlot() {
			c.errorf(v.Decl.Name, diag.InvalidVariant, "variant %s has the name of %s's slot %s: %s { … } in a caller's block would be either; name one otherwise", v.name, t.name, v.name, v.name)
		}
		saved := c.ctx
		c.enter(CtxBinding, v.Value, "variant "+v.name)
		c.assign(v.Value, Bool, "variant "+v.name)
		c.leave(saved)
	}
	if !c.TakesStyle(t) {
		c.errorf(comp.Variants[0].Decl, diag.InvalidVariant, "a component with variants shows one element: its caller's %s { … } goes on that element (%s's views are more than one call, or one that takes no style)", comp.Variants[0].name, t.name)
	}
}

// componentBody checks a component's, a screen's or a store's body:
// its members' values, then its effects, tasks and views.
func (c *Checker) componentBody(t *TypeName) {
	comp := t.Comp
	if comp.Worker {
		c.workerBody(t)
		return
	}
	if comp.Screen && prelude.FindElement("Screen") == nil {
		c.errorf(t.Decl, diag.NotSupported, "screen %s needs the Screen element, which the runtime doesn't have yet: make it a component", t.name)
	}
	if comp.Screen {
		c.screenAt(t)
	}
	for _, m := range comp.Order {
		switch m.Kind {
		case StateMember, LetMember, PropMember, ParamMember:
			if m.Default != nil {
				c.memberValue(m)
			}
		}
	}
	savedScope, savedCtx := c.scope, c.ctx
	c.scope, c.ctx = comp.Ctx.scope, comp.Ctx
	defer func() { c.scope, c.ctx = savedScope, savedCtx }()
	c.variantDecls(t)
	for _, it := range comp.Body.Items {
		switch d := it.(type) {
		case *syntax.ProvideDecl:
			pt := c.expr(d.Value, nil)
			if n, ok := pt.(*Named); !ok || n.Obj.Kind != StoreType {
				if !IsInvalid(pt) {
					c.errorf(d.Value, diag.TypeMismatch, "provide takes a store, for use in the components below: this is %s", aType(pt))
				}
			}
		case *syntax.TaskDecl:
			c.taskDecl(d)
		case *syntax.EffectDecl:
			c.effectDecl(d, t.name+".effect")
		case *syntax.CleanupStmt:
			c.cleanupStmt(d, true)
		}
	}
	if comp.Store {
		for _, it := range comp.Body.Items {
			switch it.(type) {
			case *syntax.PropDecl, *syntax.EventDecl, *syntax.StateDecl, *syntax.LetDecl, *syntax.UseDecl,
				*syntax.FuncDecl, *syntax.VariantDecl, *syntax.ProvideDecl, *syntax.TaskDecl, *syntax.EffectDecl, *syntax.CleanupStmt, *syntax.BadItem:
			default:
				c.errorf(it, diag.MisplacedDeclaration, "a store has no views or statements: it's state, lets, fns, tasks, effects and cleanups")
			}
		}
		return
	}
	var views []syntax.Item
	for _, it := range comp.Body.Items {
		switch it.(type) {
		case *syntax.PropDecl, *syntax.EventDecl, *syntax.StateDecl, *syntax.LetDecl, *syntax.UseDecl,
			*syntax.FuncDecl, *syntax.VariantDecl, *syntax.ProvideDecl, *syntax.TaskDecl, *syntax.EffectDecl, *syntax.CleanupStmt, *syntax.BadItem:
		default:
			views = append(views, it)
		}
	}
	views = c.bodyCalls(views)
	if comp.Screen {
		c.screenRoot(t, views)
		return
	}
	c.viewBlock(views, nil, t.name)
}

// screenRoot checks a screen's views as the block of its root, a Screen
// element: title: "Inbox", toolbar { … }, bind search: q and on
// searchSubmit { } are the element's, the rest its children.
func (c *Checker) screenRoot(t *TypeName, items []syntax.Item) {
	d := t.Decl.(*syntax.ScreenDecl)
	id := &syntax.Ident{Span: d.Name.Span, Name: "Screen"}
	call := &syntax.CallExpr{Span: d.Body.Span, Fun: id, Block: &syntax.Block{Span: d.Body.Span, Items: items}}
	c.info.ScreenRoots[t] = call
	tn, ok := c.prelude.Scope.LookupLocal("Screen").(*TypeName)
	if !ok {
		return
	}
	c.resolveType(tn)
	savedView := c.view
	c.view = true
	c.record(call, c.componentCall(call, tn))
	c.view = savedView
}

func (c *Checker) taskDecl(d *syntax.TaskDecl) {
	saved := c.ctx
	c.deps(d.Deps)
	ctx := c.enter(CtxTask, d.Body, "task")
	ctx.Async, ctx.Throws = true, true
	c.block(d.Body)
	c.leave(saved)
}

// effectDecl checks effect { … }, whose body's reads are what it follows
// (its context keyed by d), and effect(a, b) { … }, which follows a and b
// and whose body follows nothing (its context keyed by d.Body).
func (c *Checker) effectDecl(d *syntax.EffectDecl, name string) {
	saved := c.ctx
	var ctx *FuncCtx
	ec := effectCheck{decl: d}
	if d.Lparen.IsValid() {
		c.deps(d.Deps)
		for _, x := range d.Deps {
			ec.deps = append(ec.deps, c.info.Ctxs[x])
		}
		// effect() { … } follows nothing: it runs once, as its component
		// is built, and its cleanups as it goes (rfcs/0016).
		ec.untracked = true
		ctx = c.enter(CtxEffect, d.Body, name)
		ctx.Untracked = true
	} else {
		ctx = c.enter(CtxEffect, d, name)
	}
	ctx.Throws = true
	c.block(d.Body)
	c.leave(saved)
	ec.body = ctx
	c.effects = append(c.effects, ec)
}

// deps checks the dependencies of a task, an effect or a state: each a
// binding of its own, compared when it changes as a memo is (a value by
// what it holds; a store, a closure or a native object by which it is).
func (c *Checker) deps(deps []syntax.Expr) {
	saved := c.ctx
	for _, x := range deps {
		c.enter(CtxBinding, x, "dependency")
		c.valueType(c.expr(x, nil), x)
		c.leave(saved)
	}
}

// viewBlock checks the view items of a component's body or app: what it
// shows.
func (c *Checker) viewBlock(items []syntax.Item, el *ElementCall, what string) {
	savedView := c.view
	c.view = true
	c.viewItems(items, el)
	c.view = savedView
}

// viewItems checks items in a view: elements, control flow, View values.
// el is the element whose block they're in, nil in a body.
func (c *Checker) viewItems(items []syntax.Item, el *ElementCall) {
	for _, it := range items {
		c.viewItem(it, el)
	}
}

func (c *Checker) viewItem(it syntax.Item, el *ElementCall) {
	switch s := it.(type) {
	case *syntax.IfStmt:
		saved := c.ctx
		c.openScope()
		if ViewCondChain(s.Cond) {
			c.viewCondChain(s)
		} else if l, ok := s.Cond.(*syntax.LetCond); ok {
			c.enter(CtxBinding, s.Cond, "condition")
			lt := c.expr(l.Value, nil)
			c.leave(saved)
			c.info.Types[s.Cond] = lt
			c.enter(CtxBuilder, s.Then, "then")
			c.letCondPattern(l, lt, ViewMemo)
		} else {
			c.enter(CtxBinding, s.Cond, "condition")
			c.assign(s.Cond, Bool, "the condition")
			c.leave(saved)
			c.enter(CtxBuilder, s.Then, "then")
		}
		c.viewItems(s.Then.Items, nil)
		c.leave(saved)
		c.closeScope()
		switch e := s.Else.(type) {
		case *syntax.IfStmt:
			c.enter(CtxBuilder, e, "else")
			c.viewItem(e, nil)
			c.leave(saved)
		case *syntax.Block:
			c.enter(CtxBuilder, e, "else")
			c.viewItems(e.Items, nil)
			c.leave(saved)
		}
	case *syntax.ForStmt:
		c.forStmt(s, true)
		if el != nil && el.Comp.Comp.Element != nil && el.Comp.Comp.Element.LazyFor {
			c.info.Fors[s].Lazy = true
		}
	case *syntax.MatchStmt:
		saved := c.ctx
		c.enter(CtxBinding, s.Subject, "subject")
		t := c.expr(s.Subject, nil)
		c.leave(saved)
		mi := &MatchInfo{Subject: t, View: true}
		c.info.Matches[s] = mi
		for _, a := range s.Arms {
			c.enter(CtxBuilder, a, "arm")
			c.topPattern(a.Pattern, s.Subject, t, ViewMemo)
			if a.Guard != nil {
				c.errorf(a.Guard, diag.StatementInView, "a match in a view takes no guards: put an if in the arm")
			}
			switch b := a.Body.(type) {
			case *syntax.Block:
				c.viewItems(b.Items, nil)
			case syntax.Item:
				c.viewItem(b, nil)
			}
			c.leave(saved)
		}
		// A view may show nothing for the cases it doesn't match.
		mi.Exhaustive, _ = c.exhaustive(s.Subject, t, s.Arms)
	case *syntax.ExprStmt:
		t := c.expr(s.X, View)
		if !IsInvalid(t) && t != View && !(IsOptional(t) && Unopt(t) == View) {
			c.errorf(s, diag.NotAView, "%s isn't a view: a body shows elements and components (Text(\"{x}\") shows a value)", aType(t))
		}
	case *syntax.PropSet, *syntax.OnHandler, *syntax.BindItem, *syntax.StyleUse, *syntax.SlotItem:
		c.errorf(it, diag.StatementInView, "%s goes in an element's block, not here", itemName(it))
	case *syntax.LetDecl:
		if s.Keyword == syntax.VAR {
			c.errorf(it, diag.StatementInView, "var in a view: a component's body is declarations; a value that changes is state, declared in the body (state x = …)")
		} else {
			c.errorf(it, diag.StatementInView, "a let in a view: a component's body is declarations; declare it in the body, beside its state (a let there is kept up to date)")
		}
		// Its names, so their uses don't say they're unknown.
		c.bindIrrefutable(s.Pattern, Invalid, false, true)
	case *syntax.AssignStmt, *syntax.WhileStmt, *syntax.DoStmt, *syntax.ReturnStmt,
		*syntax.ThrowStmt, *syntax.BranchStmt, *syntax.DeferStmt, *syntax.EmitStmt:
		c.errorf(it, diag.StatementInView, "%s in a view: a component's body is declarations; put statements in a handler (on press { … }), a fn, a task or an effect", stmtName(it))
	case *syntax.StateDecl, *syntax.PropDecl, *syntax.EventDecl, *syntax.VariantDecl, *syntax.UseDecl, *syntax.ProvideDecl,
		*syntax.TaskDecl, *syntax.EffectDecl, *syntax.FuncDecl:
		c.errorf(it, diag.StatementInView, "%s belongs in a component's body, not in an element's block", itemName(it))
	case *syntax.CleanupStmt:
		c.errorf(it, diag.StatementInView, "a cleanup in a view: it goes in a component's body (it runs when the component goes), or in an effect's")
		c.cleanupStmt(s, true)
	case *syntax.StyleDecl:
		c.errorf(it, diag.MisplacedDeclaration, "a style is declared at the top of a file")
	case *syntax.BadItem:
	default:
		c.errorf(it, diag.NotAView, "this isn't a view")
	}
}

func stmtName(it syntax.Item) string {
	switch it := it.(type) {
	case *syntax.LetDecl:
		if it.Keyword == syntax.VAR {
			return "var"
		}
		return "a let"
	case *syntax.AssignStmt:
		return "an assignment"
	case *syntax.WhileStmt:
		return "while"
	case *syntax.DoStmt:
		return "do"
	case *syntax.ReturnStmt:
		return "return"
	case *syntax.ThrowStmt:
		return "throw"
	case *syntax.BranchStmt:
		return it.Tok.String()
	case *syntax.DeferStmt:
		return "defer"
	case *syntax.EmitStmt:
		return "emit"
	}
	return "a statement"
}

// ---------------------------------------------------------------------
// Components used

// styleMember finds a prop of the Style pseudo-component.
func (c *Checker) styleMember(name string) *Member {
	o, ok := c.prelude.Scope.LookupLocal("Style").(*TypeName)
	if !ok || o.Comp == nil {
		return nil
	}
	if m := o.Comp.Members[name]; m != nil && m.Kind == PropMember {
		return m
	}
	return nil
}

// componentCall checks a component's, a screen's or an element's use:
// header arguments in the parentheses, the rest in the block.
func (c *Checker) componentCall(e *syntax.CallExpr, tn *TypeName) Type {
	comp := tn.Comp
	if comp.Style {
		c.errorf(e.Fun, diag.NotAView, "Style isn't an element: its props are every element's")
	}
	ec := &ElementCall{Comp: tn, Type: tn.Type.(*Named)}
	c.info.Elements[e] = ec
	c.info.Calls[e] = &Call{Kind: CallComponent, Type: ec.Type, Result: View}
	if c.ctx != nil {
		c.ctx.readsState = true
	}
	// Header arguments.
	var params []*Param
	for _, m := range comp.Params {
		params = append(params, &Param{Name: m.name, Type: m.Type, Default: m.Default})
	}
	var list []*syntax.Arg
	if e.Args != nil {
		for _, a := range e.Args.List {
			if a.Label != nil {
				if m := comp.Members[a.Label.Name]; m != nil && m.Kind != ParamMember || (comp.Native || c.TakesStyle(tn)) && c.styleMember(a.Label.Name) != nil {
					c.errorf(a.Label, diag.PropOrArgument, "%s is a prop of %s: set it in the block, %s(…) { %s: … }", a.Label.Name, tn.name, tn.name, a.Label.Name)
					c.expr(a.Value, nil)
					continue
				}
			}
			list = append(list, a)
		}
	}
	ec.Args = c.matchArgList(e, list, params, tn.name)
	for i, a := range ec.Args {
		if a != nil {
			c.propValue(a, comp.Params[i], "argument "+params[i].Name+" of "+tn.name)
		}
	}
	if e.As != nil {
		switch {
		case comp.Native:
			ec.As = c.declareVar(e.As, Ref, false)
		case c.TakesStyle(tn):
			// The view of the one element it shows; its variants read
			// through it (name.checked).
			ec.As = c.declareVar(e.As, Ref, false)
			ec.As.RefComp = tn
		default:
			c.errorf(e.As, diag.InvalidRef, "as names an element's view, and %s shows more than one: name an element inside it", tn.name)
		}
	}
	if e.Block != nil {
		c.elementBlock(e.Block.Items, ec)
	}
	c.defaultStyle(ec)
	for _, h := range e.Handlers {
		c.onHandler(h, ec)
	}
	// Required props.
	for _, m := range comp.Props {
		if m.Default != nil || IsOptional(m.Type) || comp.Native {
			continue
		}
		set := false
		for _, p := range ec.Props {
			if p.Member == m {
				set = true
			}
		}
		for _, s := range ec.Slots {
			if s.Prop == m {
				set = true
			}
		}
		if m.name == "children" && len(ec.Children) > 0 {
			set = true
		}
		for _, bd := range ec.Binds {
			if bd.Prop == m {
				set = true
			}
		}
		if !set {
			c.errorf(e, diag.MissingProp, "%s needs %s: set it in the block, %s { %s: … }", tn.name, m.name, tn.name, m.name)
		}
	}
	return View
}

// propValue checks a value passed to a component: a binding (or a
// builder, for a View).
func (c *Checker) propValue(x syntax.Expr, m *Member, what string) {
	saved := c.ctx
	t := c.memberType(m)
	if Identical(Unopt(t), View) {
		c.enter(CtxBuilder, x, "slot")
		savedView := c.view
		c.view = true
		c.assign(x, t, what)
		c.view = savedView
		c.leave(saved)
		return
	}
	if lam, ok := unparen(x).(*syntax.LambdaExpr); ok && m.Owner != nil && m.Owner.Comp != nil && m.Owner.Comp.Element != nil {
		for _, k := range m.Owner.Comp.Element.Keys {
			if k.Name == m.name && k.Builder {
				if ft, ok := t.(*FuncType); ok {
					c.builderLambda(lam, ft, m.Owner.name+"'s "+m.name)
					return
				}
			}
		}
	}
	c.enter(CtxBinding, x, "binding")
	if m.name == "href" && m.Owner != nil && m.Owner.Comp != nil && m.Owner.Comp.Native && t == String {
		// A Button's (a NavLink's) href: a string, or a routed screen's
		// address.
		c.hrefArg(x, what)
	} else {
		c.assign(x, t, what)
	}
	c.leave(saved)
}

// elementBlock checks an element's or a component's block.
func (c *Checker) elementBlock(items []syntax.Item, ec *ElementCall) {
	comp := ec.Comp.Comp
	var children []syntax.Item
	for _, it := range items {
		switch s := it.(type) {
		case *syntax.PropSet:
			if s.Name.Name == "style" {
				c.styleProp(s, ec)
				continue
			}
			c.propSet(s.Name, s.Value, ec, false)
		case *syntax.OnHandler:
			c.onHandler(s, ec)
		case *syntax.BindItem:
			c.bindItem(s, ec)
		case *syntax.StyleUse:
			c.errorf(s, diag.DeprecatedSyntax, "write style: %s: an element applies styles with the style: prop (asuka fmt rewrites it)", styleName(s.Name))
		case *syntax.SlotItem:
			if m := comp.Members[s.Name.Name]; m == nil && isVariant(s.Name.Name) || (m == nil || !m.IsSlot()) && c.FindVariant(ec.Comp, s.Name.Name) != nil {
				c.variantItem(s, ec)
			} else {
				c.slotItem(s, ec)
			}
		case *syntax.IfStmt, *syntax.ForStmt, *syntax.MatchStmt, *syntax.ExprStmt:
			children = append(children, it)
		default:
			c.viewItem(it, ec)
		}
	}
	ec.Children = children
	if len(children) == 0 {
		return
	}
	cm := comp.Members["children"]
	if cm == nil || cm.Kind != PropMember {
		c.errorf(children[0], diag.NoChildren, "%s takes no children%s", ec.Comp.name, childHint(ec.Comp))
		return
	}
	if comp.Native && isRouteElement(ec) {
		// Routes, a Route's screen, routed screens.
		c.routeChildren(children, ec)
		return
	}
	if comp.Native && !LazyChildren(ec) {
		// An element's children are built in place.
		savedView := c.view
		c.view = true
		c.viewItems(children, ec)
		c.view = savedView
		if comp.Element != nil && comp.Element.Name == "Alert" {
			c.alertChildren(children)
		}
		if comp.Element != nil && comp.Element.Name == "List" {
			c.listChildren(children)
		}
		return
	}
	// A component's: one builder of them all, its children prop.
	saved := c.ctx
	c.enter(CtxBuilder, children[0], "children")
	savedView := c.view
	c.view = true
	c.viewItems(children, ec)
	c.view = savedView
	c.leave(saved)
}

// listChildren checks that a List's block is one for: its rows, made
// as they come into view. Another view there (a header, a status line)
// is no row of the list's.
func (c *Checker) listChildren(items []syntax.Item) {
	fors := 0
	for _, it := range items {
		if _, ok := it.(*syntax.ForStmt); ok {
			fors++
			if fors == 1 {
				continue
			}
			c.errorf(it, diag.NoChildren, "a List has one for: its rows; put the items of both in one array (an enum of the kinds of row)")
			continue
		}
		c.errorf(it, diag.NoChildren, "a List's rows come from one for in it: make this a row of the for's items (an enum of the kinds of row), or put it beside the List")
	}
}

// alertChildren checks that an Alert's block holds Buttons and
// TextFields (its buttons and fields), through ifs and fors.
func (c *Checker) alertChildren(items []syntax.Item) {
	for _, it := range items {
		switch s := it.(type) {
		case *syntax.ExprStmt:
			call, _ := s.X.(*syntax.CallExpr)
			ec := c.info.Elements[call]
			if ec == nil {
				c.errorf(s, diag.NoChildren, "an Alert's block holds Buttons and TextFields: its buttons and its fields")
				continue
			}
			if el := ec.Comp.Comp.Element; el == nil || el.Name != "Button" && el.Name != "TextField" {
				c.errorf(s, diag.NoChildren, "an Alert's block holds Buttons and TextFields (its buttons and its fields), not a %s", ec.Comp.name)
			}
		case *syntax.IfStmt:
			c.alertChildren(s.Then.Items)
			switch e := s.Else.(type) {
			case *syntax.Block:
				c.alertChildren(e.Items)
			case *syntax.IfStmt:
				c.alertChildren([]syntax.Item{e})
			}
		case *syntax.ForStmt:
			c.alertChildren(s.Body.Items)
		}
	}
}

// LazyChildren reports whether an element's children are one builder
// the runtime calls when they show (ui.lazy: a Window's content, a
// Sheet's): an element that says so (lang/prelude's LazyChildren) and
// whose builder takes no parameters (a Route's takes its Params).
func LazyChildren(ec *ElementCall) bool {
	el := ec.Comp.Comp.Element
	return el != nil && el.LazyChildren && len(el.Params) == 0
}

func childHint(t *TypeName) string {
	if t.Comp.Native {
		return ""
	}
	return ": declare prop children: View for them"
}

// isVariant reports an interaction state's name: pressed, hovered, …
func isVariant(name string) bool {
	for _, v := range prelude.Variants {
		if v == name {
			return true
		}
	}
	return false
}

// FindVariant is the variant name a component declares: t's own, else
// the one of the component its one element is (whose element is t's),
// nil when none does.
func (p *Program) FindVariant(t *TypeName, name string) *VariantDef {
	return p.checker.FindVariant(t, name)
}

func (c *Checker) FindVariant(t *TypeName, name string) *VariantDef {
	for i := 0; t != nil && i < 64; i++ {
		comp := t.Comp
		if comp == nil || comp.Native || comp.Store {
			return nil
		}
		if v := comp.Variant(name); v != nil {
			return v
		}
		if !c.TakesStyle(t) {
			return nil
		}
		t = c.rootComponent(t)
	}
	return nil
}

// variantNames lists the variants t's caller may style, for a message.
func (c *Checker) variantNames(t *TypeName) string {
	var names []string
	seen := map[string]bool{}
	if c.styleTakesVariants(t) {
		names = append(names, prelude.Variants...)
	}
	for i := 0; t != nil && i < 64; i++ {
		comp := t.Comp
		if comp == nil || comp.Native || comp.Store {
			break
		}
		for _, v := range comp.Variants {
			if !seen[v.name] && (v.Pub || v.Owner.Pkg == c.pkg) {
				seen[v.name] = true
				names = append(names, v.name)
			}
		}
		if !c.TakesStyle(t) {
			break
		}
		t = c.rootComponent(t)
	}
	if len(names) == 0 {
		return "none"
	}
	return strings.Join(names, ", ")
}

// variantItem checks a variant: a block of style props laid over the
// view while it's in an interaction state (a container's, or the one
// element of a component's), or while a variant its component declares
// holds.
func (c *Checker) variantItem(s *syntax.SlotItem, ec *ElementCall) {
	comp := ec.Comp.Comp
	va := &VariantArg{State: s.Name.Name, Item: s}
	switch d := c.FindVariant(ec.Comp, s.Name.Name); {
	case d != nil:
		if !d.Pub && d.Owner.Pkg != c.pkg {
			c.errorf(s.Name, diag.NotPub, "%s's variant %s isn't pub: its package styles it, or declares it pub variant %s", d.Owner.name, d.name, d.name)
			return
		}
		va.Decl = d
		c.info.Uses[s.Name] = d // its declaration, for an editor
	case comp.Native && (comp.Element == nil || !comp.Element.Variants):
		c.errorf(s.Name, diag.UnknownProp, "%s takes no variants: %s { … } goes in a View, a Row, a Column or a SafeArea", ec.Comp.name, s.Name.Name)
		return
	case !comp.Native && !c.styleTakesVariants(ec.Comp):
		c.errorf(s.Name, diag.UnknownProp, "%s has no variant %s (its variants: %s)", ec.Comp.name, s.Name.Name, c.variantNames(ec.Comp))
		return
	}
	set := func(name *syntax.Ident, value syntax.Expr) {
		m := c.styleMember(name.Name)
		switch {
		case m == nil && comp.Members[name.Name] != nil:
			c.errorf(name, diag.InvalidVariant, "a variant holds style props, and %s is %s's own: set it outside %s { … }", name.Name, ec.Comp.name, s.Name.Name)
		case m == nil:
			c.errorf(name, diag.UnknownProp, "a variant holds style props, and %s isn't one%s", name.Name, c.propSuggest(c.styleComp(), name.Name))
		case name.Name == "keys" || name.Name == "actions":
			c.errorf(name, diag.InvalidVariant, "%s don't change with a state: set them outside %s { … }", name.Name, s.Name.Name)
			m = nil
		}
		if m == nil {
			c.expr(value, Invalid) // said: what it'd be is unknown
			return
		}
		c.propValue(value, m, name.Name)
		va.Props = append(va.Props, &PropArg{Member: m, Value: value})
	}
	var styleSet *syntax.PropSet
	for _, it := range s.Body.Items {
		switch x := it.(type) {
		case *syntax.PropSet:
			if x.Name.Name == "style" {
				// The variant's styles, under its own props.
				if styleSet != nil {
					c.errorf(x.Name, diag.InvalidStyle, "style: is set twice: put the styles in one list, style: [a, b]")
				}
				styleSet = x
				entries := c.styleList(x.Value, nil)
				c.variantEntries(entries, s.Name.Name)
				va.Style = append(va.Style, entries...)
				continue
			}
			set(x.Name, x.Value)
		case *syntax.StyleUse:
			c.errorf(x, diag.DeprecatedSyntax, "write style: %s: a variant applies styles with style: (asuka fmt rewrites it)", styleName(x.Name))
		case *syntax.BadItem:
		default:
			c.errorf(it, diag.InvalidVariant, "a variant holds style props only (name: value, style: …), not %s", itemName(it))
		}
	}
	ec.Variants = append(ec.Variants, va)
}

// styleComp is the prelude's Style pseudo-component.
func (c *Checker) styleComp() *Component {
	o, _ := c.prelude.Scope.LookupLocal("Style").(*TypeName)
	return o.Comp
}

// refProp checks ref: r, which puts the element's view in the Ref r.
func (c *Checker) refProp(name *syntax.Ident, value syntax.Expr, ec *ElementCall) {
	if !ec.Comp.Comp.Native {
		c.errorf(name, diag.InvalidRef, "ref names an element's view, and %s is a component: put it on an element inside it", ec.Comp.name)
		c.expr(value, nil)
		return
	}
	if ec.Ref != nil {
		c.errorf(name, diag.InvalidRef, "ref is set twice")
	}
	c.assign(value, Ref, "ref")
	ec.Ref = value
}

// propSet checks name: value in a block.
func (c *Checker) propSet(name *syntax.Ident, value syntax.Expr, ec *ElementCall, style bool) {
	comp := ec.Comp.Comp
	m := comp.Members[name.Name]
	if m == nil && (comp.Native && !(comp.Element != nil && comp.Element.NoStyle) || c.TakesStyle(ec.Comp)) {
		m = c.styleMember(name.Name)
	}
	if m == nil && name.Name == "ref" && !style {
		c.refProp(name, value, ec)
		return
	}
	if m == nil {
		c.errorf(name, diag.UnknownProp, "%s has no prop %s%s", ec.Comp.name, name.Name, c.propSuggest(comp, name.Name))
		c.expr(value, Invalid) // what it'd be is unknown: not asked again
		return
	}
	switch m.Kind {
	case ParamMember:
		c.errorf(name, diag.PropOrArgument, "%s is a parameter of %s: pass it in the parentheses, %s(%s: …)", name.Name, ec.Comp.name, ec.Comp.name, name.Name)
	case EventMember:
		c.errorf(name, diag.PropOrArgument, "%s is an event: handle it with on %s { … }", name.Name, name.Name)
	case PropMember:
		if name.Name == "children" {
			c.errorf(name, diag.PropOrArgument, "children are the block's views: put them in the block")
		}
	default:
		c.errorf(name, diag.UnknownProp, "%s isn't a prop of %s", name.Name, ec.Comp.name)
	}
	if m.Kind != PropMember {
		c.expr(value, nil)
		return
	}
	c.propValue(value, m, name.Name)
	ec.Props = append(ec.Props, &PropArg{Member: m, Value: value, Style: style})
}

func (c *Checker) propSuggest(comp *Component, name string) string {
	best, bestD := "", 3
	try := func(n string) {
		if d := editDistance(name, n); d < len(name) && (d < bestD || d == bestD && n < best) {
			best, bestD = n, d
		}
	}
	for n := range comp.Members {
		try(n)
	}
	if comp.Native || comp.styled > 0 {
		for _, k := range prelude.Style {
			try(k.Name)
		}
	}
	if best != "" {
		return " (did you mean " + best + "?)"
	}
	return ""
}

// holdsHandler notes an async handler of a component's event declared
// in ctx: it's a task of the component around it, whose owner its env
// keeps, and the closures between reach that env.
func (c *Checker) holdsHandler(ctx *FuncCtx) {
	var comp *FuncCtx
	for cc := ctx; cc != nil && comp == nil; cc = cc.Parent {
		switch cc.Kind {
		case CtxComponent, CtxMethod:
			comp = cc
		case CtxFunc, CtxStore, CtxGlobal, CtxTest, CtxApp, CtxSpawn, CtxStyle:
			// No component's: it's held where it's declared.
			return
		}
	}
	if comp == nil || comp.Comp == nil || comp.Comp.Comp == nil || comp.Comp.Comp.Store {
		return
	}
	comp.Comp.Comp.HoldsHandlers = true
	for cc := ctx; cc != comp; cc = cc.Parent {
		cc.UsesEnv = true
	}
}

func (c *Checker) onHandler(h *syntax.OnHandler, ec *ElementCall) {
	comp := ec.Comp.Comp
	m := comp.Members[h.Name.Name]
	saved := c.ctx
	builds := EventBuilds(ec, h.Name.Name)
	kind := CtxHandler
	if builds {
		kind = CtxBuilder
	}
	ctx := c.enter(kind, h, ec.Comp.name+".on "+h.Name.Name)
	ctx.Async, ctx.Throws = !builds, true
	savedView, savedLoops, savedTry, savedAwait, savedHandled := c.view, c.loops, c.inTry, c.inAwait, c.handled
	c.view, c.loops, c.inTry, c.inAwait, c.handled = false, 0, 0, 0, 0
	if m == nil || m.Kind != EventMember {
		c.errorf(h.Name, diag.UnknownEvent, "%s has no event %s%s", ec.Comp.name, h.Name.Name, c.eventSuggest(comp, h.Name.Name))
		for _, p := range h.Params {
			c.declareVar(p, Invalid, false)
		}
	} else {
		if len(h.Params) > len(m.Params) {
			c.errorf(h, diag.ArgumentCount, "event %s gives %s", h.Name.Name, plural(len(m.Params), "value", "values"))
		}
		var vars []*Var
		for i, p := range h.Params {
			t := Type(Invalid)
			if i < len(m.Params) {
				t = m.Params[i].Type
			}
			v := c.declareVar(p, t, false)
			ctx.Params = append(ctx.Params, v)
			vars = append(vars, v)
		}
		c.info.Binds[h] = vars
		ec.Handlers = append(ec.Handlers, &HandlerArg{Event: m, Handler: h})
	}
	if builds {
		// What shows instead: views, as a builder's.
		c.view = true
		c.viewItems(h.Body.Items, nil)
	} else {
		c.stmts(h.Body.Items)
	}
	c.view, c.loops, c.inTry, c.inAwait, c.handled = savedView, savedLoops, savedTry, savedAwait, savedHandled
	c.leave(saved)
	if !builds && ctx.Awaits && !comp.Native {
		c.holdsHandler(saved)
	}
}

// EventBuilds reports an element's event whose handler's block is views
// and returns the View it builds (a Boundary's error).
func EventBuilds(ec *ElementCall, name string) bool {
	el := ec.Comp.Comp.Element
	if el == nil {
		return false
	}
	for _, e := range el.Events {
		if e.Name == name {
			return e.Builds
		}
	}
	return false
}

func (c *Checker) eventSuggest(comp *Component, name string) string {
	var events []string
	for _, m := range comp.Order {
		if m.Kind == EventMember {
			events = append(events, m.name)
		}
	}
	if len(events) == 0 {
		return " (it has no events)"
	}
	for _, e := range events {
		if editDistance(e, name) <= 2 {
			return " (did you mean " + e + "?)"
		}
	}
	s := ""
	for i, e := range events {
		if i > 0 {
			s += ", "
		}
		s += e
	}
	return " (its events: " + s + ")"
}

// nativeBindEvent is the event a native component reports its prop m
// with, <name>Change(value: m's type), or nil.
func nativeBindEvent(comp *Component, m *Member) *Member {
	ev := comp.Members[m.name+"Change"]
	if ev == nil || ev.Kind != EventMember || len(ev.Params) != 1 || !Identical(ev.Params[0].Type, m.Type) {
		return nil
	}
	return ev
}

// bindItem checks bind name: target: a control's two-way binding.
func (c *Checker) bindItem(b *syntax.BindItem, ec *ElementCall) {
	comp := ec.Comp.Comp
	m := comp.Members[b.Name.Name]
	var key *prelude.Key
	if comp.Element != nil {
		for i := range comp.Element.Keys {
			if comp.Element.Keys[i].Name == b.Name.Name {
				key = &comp.Element.Keys[i]
			}
		}
	}
	var ev *Member
	if m != nil && !comp.Native && !comp.Store {
		c.memberType(m)
	}
	switch {
	case comp.Element == nil && !comp.Native && m != nil && m.Kind == PropMember:
		// A component binds a prop it reports with an event
		// <prop>Change(value), as a native component does.
		ev = nativeBindEvent(comp, m)
		if ev == nil {
			c.errorf(b.Name, diag.NotBindable, "%s can't bind %s: a component binds a prop it reports with event %sChange(%s: %s)", ec.Comp.name, b.Name.Name, b.Name.Name, b.Name.Name, typeString(m.Type))
			c.expr(b.Target, nil)
			return
		}
	case comp.Element == nil && comp.Native && m != nil && m.Kind == PropMember:
		// A package's native component binds a prop it reports with an
		// event <prop>Change(value) (docs/native.md).
		ev = nativeBindEvent(comp, m)
		if ev == nil {
			c.errorf(b.Name, diag.NotBindable, "%s can't bind %s: it binds a prop it reports with event %sChange(%s: %s)", ec.Comp.name, b.Name.Name, b.Name.Name, b.Name.Name, typeString(m.Type))
			c.expr(b.Target, nil)
			return
		}
	case m == nil || key == nil || !key.Bind:
		c.errorf(b.Name, diag.NotBindable, "%s can't bind %s: only a control's own value binds (TextField's text, Toggle's on)", ec.Comp.name, b.Name.Name)
		c.expr(b.Target, nil)
		return
	default:
		ev = comp.Members[key.Event]
	}
	saved := c.ctx
	c.enter(CtxBinding, b, "bind "+b.Name.Name)
	t := c.expr(b.Target, nil)
	c.leave(saved)
	ctx := c.enter(CtxHandler, b.Target, ec.Comp.name+".bind "+b.Name.Name)
	ctx.Throws = true
	savedView := c.view
	c.view = false
	v := &Var{object: object{"_value", b.Start}, Type: m.Type, Ctx: ctx}
	ctx.Params = append(ctx.Params, v)
	c.lvalue(b.Target, "bind")
	c.view = savedView
	c.leave(saved)
	if !IsInvalid(t) && !Identical(t, m.Type) {
		c.errorf(b.Target, diag.TypeMismatch, "bind %s takes %s, and this is %s", b.Name.Name, aType(m.Type), aType(t))
	}
	ec.Binds = append(ec.Binds, &BindArg{Prop: m, Event: ev, Target: b.Target, Item: b})
}

func idName(id *syntax.Ident, ok bool) string {
	if !ok || id == nil {
		return ""
	}
	return id.Name
}

// slotItem checks name { … }: views for a slot.
func (c *Checker) slotItem(s *syntax.SlotItem, ec *ElementCall) {
	comp := ec.Comp.Comp
	m := comp.Members[s.Name.Name]
	if m == nil || !m.IsSlot() {
		if m == nil && s.Name.Name == "cleanup" {
			// In a call's block, cleanup { } names a slot (rfcs/0016).
			c.errorf(s.Name, diag.StatementInView, "%s has no slot cleanup, and a cleanup in a view isn't one: it goes in a component's body (it runs when the component goes), or in an effect's", ec.Comp.name)
			return
		}
		if !comp.Native && m == nil && c.variantNames(ec.Comp) != "none" {
			c.errorf(s.Name, diag.UnknownProp, "%s has no slot or variant %s (a slot is a prop of type View; its variants: %s)", ec.Comp.name, s.Name.Name, c.variantNames(ec.Comp))
			return
		}
		c.errorf(s.Name, diag.UnknownProp, "%s has no slot %s (a slot is a prop of type View)", ec.Comp.name, s.Name.Name)
		return
	}
	saved := c.ctx
	c.enter(CtxBuilder, s.Body, s.Name.Name)
	savedView := c.view
	c.view = true
	c.viewItems(s.Body.Items, nil)
	c.view = savedView
	c.leave(saved)
	ec.Slots = append(ec.Slots, &SlotArg{Prop: m, Block: s.Body, Item: s})
}

// TakesStyle says whether a component takes the style props: when what
// it shows is one element (a built-in one that takes them, a package's
// native component, or a component that takes them), the style its
// caller gives goes on that element, over the element's own.
func (c *Checker) TakesStyle(t *TypeName) bool {
	comp := t.Comp
	if comp == nil {
		return false
	}
	if comp.Native {
		return !comp.Style && !(comp.Element != nil && comp.Element.NoStyle)
	}
	if comp.styled == 0 {
		comp.styled = -1 // while it's worked out: a cycle doesn't
		rs := c.rootComponents(t)
		ok := len(rs) > 0
		for _, r := range rs {
			ok = ok && c.TakesStyle(r)
		}
		if ok {
			comp.styled = 1
		}
	}
	return comp.styled > 0
}

// rootComponent is the element or component a component's body shows,
// when its views are that one call.
func (c *Checker) rootComponent(t *TypeName) *TypeName {
	if rs := c.rootComponents(t); len(rs) == 1 {
		return rs[0]
	}
	return nil
}

// rootComponents are what a component's body shows when it's one call,
// or an if on @platform or @debug whose branches each are, as a build
// compiles one of them: each branch's. Nil when it shows anything else.
func (c *Checker) rootComponents(t *TypeName) []*TypeName {
	comp := t.Comp
	if comp.Store || comp.Screen || comp.Body == nil || comp.Ctx == nil {
		return nil
	}
	calls := rootCalls(comp.Body.Items)
	var out []*TypeName
	for _, call := range calls {
		r := c.calledComponent(comp, call)
		if r == nil {
			return nil
		}
		out = append(out, r)
	}
	return out
}

// rootCalls is the one call items show: each branch's of an if on
// @platform or @debug that picks one per build. Nil when they show none,
// or more than one, or anything else.
func rootCalls(items []syntax.Item) []*syntax.CallExpr {
	var out []*syntax.CallExpr
	views := 0
	for _, it := range items {
		switch it := it.(type) {
		case *syntax.PropDecl, *syntax.EventDecl, *syntax.StateDecl, *syntax.LetDecl, *syntax.UseDecl,
			*syntax.FuncDecl, *syntax.VariantDecl, *syntax.ProvideDecl, *syntax.TaskDecl, *syntax.EffectDecl, *syntax.CleanupStmt, *syntax.BadItem:
		case *syntax.ExprStmt:
			views++
			e, ok := unparen(it.X).(*syntax.CallExpr)
			if !ok {
				return nil
			}
			out = append(out, e)
		case *syntax.IfStmt:
			views++
			cs := buildIfCalls(it)
			if cs == nil {
				return nil
			}
			out = append(out, cs...)
		default:
			return nil
		}
	}
	if views != 1 {
		return nil
	}
	return out
}

// buildIfCalls is each branch's one call of an if that a build decides
// (its condition only @platform's and @debug's comparisons), with an
// else; nil otherwise.
func buildIfCalls(s *syntax.IfStmt) []*syntax.CallExpr {
	if s.Else == nil || !buildCond(s.Cond) {
		return nil
	}
	then := rootCalls(s.Then.Items)
	if then == nil {
		return nil
	}
	var rest []*syntax.CallExpr
	switch e := s.Else.(type) {
	case *syntax.IfStmt:
		rest = buildIfCalls(e)
	case *syntax.Block:
		rest = rootCalls(e.Items)
	}
	if rest == nil {
		return nil
	}
	return append(then, rest...)
}

// buildCond reports whether e is decided as the program compiles:
// @debug, a @define (a bool's), a comparison of @platform with a case,
// and !, && and || of them.
func buildCond(e syntax.Expr) bool {
	switch x := unparen(e).(type) {
	case *syntax.AtExpr:
		return x.Name == "debug" || x.Name == "define"
	case *syntax.UnaryExpr:
		return x.Op == syntax.NOT && buildCond(x.X)
	case *syntax.BinaryExpr:
		switch x.Op {
		case syntax.LAND, syntax.LOR:
			return buildCond(x.X) && buildCond(x.Y)
		case syntax.EQL, syntax.NEQ:
			return IsPlatformConst(x.X) || IsPlatformConst(x.Y)
		}
	}
	return false
}

// calledComponent is the element or component call calls, in comp's
// scope.
func (c *Checker) calledComponent(comp *Component, call *syntax.CallExpr) *TypeName {
	var o Object
	switch f := call.Fun.(type) {
	case *syntax.Ident:
		o = comp.Ctx.scope.Lookup(f.Name)
	case *syntax.MemberExpr:
		if id, ok := f.X.(*syntax.Ident); ok {
			if pn, ok := comp.Ctx.scope.Lookup(id.Name).(*PkgName); ok {
				o = pn.Pkg.Scope.LookupLocal(f.Name.Name)
			}
		}
	}
	if tn, ok := o.(*TypeName); ok && tn.Comp != nil && !tn.Comp.Store {
		return tn
	}
	return nil
}

// ViewCondChain reports a view's if condition that's checked as a
// chain: clauses (if let a = x, a > 0), or one let with a pattern other
// than a name (if let .ready(v) = state).
func ViewCondChain(cond syntax.Expr) bool {
	switch x := cond.(type) {
	case *syntax.CondList:
		return true
	case *syntax.LetCond:
		_, name := x.Pattern.(*syntax.BindPat)
		return !name
	}
	return false
}

// A ViewCond is a view's if with a chain for its condition: the chain
// runs as one binding (a memo of what it binds, none when a clause
// fails), with its names as locals (Chain); the then block's names are
// memos of the chain's values (Then, in the same order).
type ViewCond struct {
	Chain, Then []*Var
}

// viewCondChain checks a view's if whose condition is a chain, entering
// the then block's builder.
func (c *Checker) viewCondChain(s *syntax.IfStmt) {
	saved := c.ctx
	c.enter(CtxBinding, s.Cond, "condition")
	c.openScope()
	vc := &ViewCond{}
	// The chain's names are its closure's locals.
	savedView := c.view
	c.view = false
	c.cond(s.Cond)
	c.view = savedView
	vc.Chain = CondVars(c.info, s.Cond)
	c.closeScope()
	c.leave(saved)
	c.enter(CtxBuilder, s.Then, "then")
	for _, v := range vc.Chain {
		m := &Var{object: v.object, Type: v.Type, Kind: ViewMemo, Ctx: c.ctx}
		c.scope.Insert(m)
		vc.Then = append(vc.Then, m)
	}
	c.info.ViewConds[s] = vc
}

// CondVars are the names a condition's lets bind, in order.
func CondVars(info *Info, cond syntax.Expr) []*Var {
	var vs []*Var
	clauses := []syntax.Expr{cond}
	if l, ok := cond.(*syntax.CondList); ok {
		clauses = l.List
	}
	for _, x := range clauses {
		lc, ok := x.(*syntax.LetCond)
		if !ok {
			continue
		}
		syntax.Inspect(lc.Pattern, func(n syntax.Node) bool {
			if bp, ok := n.(*syntax.BindPat); ok {
				if v, ok := info.Defs[bp.Name].(*Var); ok && v.Name() != "_" {
					vs = append(vs, v)
				}
			}
			return true
		})
	}
	return vs
}
