package check

import (
	"fmt"
	"path/filepath"
	"slices"
	"strings"

	"github.com/egoist/asuka/lang/diag"
	"github.com/egoist/asuka/lang/syntax"
)

func (c *Checker) call(e *syntax.CallExpr, want Type) Type {
	if len(e.Handlers) > 0 {
		if _, ok := c.componentOf(e.Fun); !ok {
			c.errorf(e.Handlers[0], diag.UnknownEvent, "on after a block handles a component's event: %s isn't one", exprName(e.Fun))
		}
	}
	switch fn := e.Fun.(type) {
	case *syntax.Ident:
		if x := c.implicitSelf(fn, true); x != nil {
			// self's method by its bare name: self.name(…).
			call := &syntax.CallExpr{Span: e.Span, Fun: x, TypeArgs: e.TypeArgs, Angles: e.Angles, Args: e.Args, Block: e.Block, Handlers: e.Handlers}
			c.info.ImplicitSelf[e] = call
			return c.expr(call, want)
		}
		o := c.lookup(fn.Name)
		if o == nil && fn.Name == "shader" {
			c.errorf(fn, diag.InvalidShader, shaderMoved)
			c.checkArgsLoosely(e)
			return Invalid
		}
		if o == nil {
			c.unknownName(fn)
			c.checkArgsLoosely(e)
			return Invalid
		}
		c.info.Uses[fn] = o
		c.hiddenObj(fn, fn.Name, o)
		switch o := o.(type) {
		case *Function:
			return c.funcCall(e, o, nil, nil, want)
		case *Builtin:
			return c.builtinCall(e, o, want)
		case *TypeName:
			return c.typeCall(e, o, want)
		case *Member:
			if o.Kind == FnMember {
				c.envUse(o.Owner, fn, o.name)
				call := c.funcCallInfo(e, o.Fn, nil, nil, want)
				call.SelfCall = true
				return call.Result
			}
			t := c.useMember(o, fn)
			return c.closureCall(e, fn, t)
		case *selfMember:
			if o.owner.Kind == InterfaceType && o.field == nil {
				// In a provided method: another of the interface's.
				return c.selfIfaceCall(e, fn, want)
			}
			if o.method == nil {
				return c.closureCall(e, fn, c.ident(fn))
			}
			if c.ctx == nil || c.ctx.Self == nil {
				c.errorf(fn, diag.NotAValue, "%s is a method of %s: call it on a value", fn.Name, o.owner.name)
				return Invalid
			}
			c.useVar(c.ctx.Self, fn)
			c.hiddenFn(fn, o.owner.name, o.method)
			call := c.funcCallInfo(e, o.method, nil, c.ctx.Self.Type, want)
			call.SelfCall = true
			return call.Result
		default:
			return c.closureCall(e, fn, c.ident(fn))
		}
	case *syntax.MemberExpr:
		// A message of the app's catalogs.
		if id, ok := fn.X.(*syntax.Ident); ok {
			if tr, ok := c.lookup(id.Name).(*TrName); ok {
				c.info.Uses[id] = tr
				return c.trCall(e, fn, tr)
			}
		}
		// A package's function, builtin or type.
		if id, ok := fn.X.(*syntax.Ident); ok {
			if pn, ok := c.lookup(id.Name).(*PkgName); ok {
				c.info.Uses[id] = pn
				o := pn.Pkg.Scope.LookupLocal(fn.Name.Name)
				if o == nil {
					if !pn.Pkg.Broken {
						c.errorf(fn.Name, diag.UnknownMember, "package %s has no %s", pn.Pkg.Path, fn.Name.Name)
					}
					c.checkArgsLoosely(e)
					return Invalid
				}
				if !c.exported(pn.Pkg, o) {
					c.errorf(fn.Name, diag.NotPub, "%s isn't pub in package %s", fn.Name.Name, pn.Pkg.Path)
				}
				c.info.Uses[fn.Name] = o
				c.info.Selections[fn] = &Selection{Kind: SelPkg, Obj: o}
				switch o := o.(type) {
				case *Function:
					return c.funcCall(e, o, nil, nil, want)
				case *Builtin:
					return c.builtinCall(e, o, want)
				case *TypeName:
					return c.typeCall(e, o, want)
				}
				return c.closureCall(e, fn, c.member(fn, nil))
			}
		}
		// Type.case(…).
		if tn := c.staticType(fn.X); tn != nil {
			c.record(fn.X, Invalid)
			c.resolveType(tn)
			app := c.typeAppOf(fn.X)
			if k := tn.Case(fn.Name.Name); k != nil {
				if app != nil {
					want = app // Result<int, Error>.ok(1)
				}
				return c.caseCall(e, tn, k, want)
			}
			if tn.Type == Color && (fn.Name.Name == "rgb" || fn.Name.Name == "parse") {
				// Color.rgb(215, 58, 74), Color.parse("#d73a4a"): a color
				// the app has only as it runs.
				id := BColorRGB
				if fn.Name.Name == "parse" {
					id = BColorParse
				}
				return c.builtinCall(e, &Builtin{object: object{name: "Color"}, ID: id}, want)
			}
			if IsInteger(tn.Type) && fn.Name.Name == "parse" {
				// u64.parse("18446744073709551615"), u8.parse("ff", radix: 16).
				return c.builtinCall(e, &Builtin{object: object{name: tn.name}, ID: BIntParse}, want)
			}
			f := tn.Statics[fn.Name.Name]
			var pre Subst
			if app != nil {
				pre = app.Subst() // Box<int>.empty(): the type's parameters given
			}
			if f == nil {
				target := tn.Type
				if app != nil {
					target = app
				}
				hit, _ := c.extLookup(target, fn.Name.Name, "static", fn.Name)
				f = hit.f
				if app != nil {
					pre = hit.s // the extension's parameters, by the type's
				}
			}
			if f != nil {
				// A static fn: Money.of(dollars: 2).
				c.hiddenFn(fn.Name, tn.name, f)
				c.info.Uses[fn.Name] = f
				c.info.Selections[fn] = &Selection{Kind: SelPkg, Obj: f}
				return c.funcCallPre(e, f, nil, nil, want, false, pre).Result
			}
			if g := tn.Consts[fn.Name.Name]; g != nil {
				// A constant that's a function: Shortcut.cmd("k").
				return c.closureCall(e, fn, c.expr(fn, nil))
			}
			if hit, _ := c.extLookup(tn.Type, fn.Name.Name, "const", nil); hit.g != nil {
				return c.closureCall(e, fn, c.expr(fn, nil))
			}
			if d, ok := tn.Decl.(*syntax.InterfaceDecl); ok && d.Native.IsValid() {
				c.errorf(fn, diag.NotCallable, "%s is a native interface: make one, %s(), and call %s on it", tn.name, tn.name, fn.Name.Name)
			} else {
				c.errorf(fn.Name, diag.UnknownMember, "%s has no case %s", tn.name, fn.Name.Name)
			}
			c.checkArgsLoosely(e)
			return Invalid
		}
		return c.methodCall(e, fn, want)
	case *syntax.ImplicitMember:
		c.info.Expected[fn] = want
		w := Unopt(want)
		if w != nil && !IsInvalid(w) {
			if hit, _ := c.extLookup(w, fn.Name.Name, "static", fn); hit.f != nil {
				// An extension's static fn: .hex("7c3aed") where a Color is
				// expected.
				c.info.Uses[fn.Name] = hit.f
				return c.funcCall(e, hit.f, nil, nil, want)
			}
			if hit, _ := c.extLookup(w, fn.Name.Name, "const", nil); hit.g != nil {
				return c.closureCall(e, fn, c.expr(fn, want))
			}
		}
		if n, ok := w.(*Named); ok && n.Obj.Statics[fn.Name.Name] != nil {
			// A static fn making one: .of(dollars: 2) where a Money is
			// expected.
			f := n.Obj.Statics[fn.Name.Name]
			c.hiddenFn(fn.Name, n.Obj.name, f)
			c.info.Uses[fn.Name] = f
			return c.funcCall(e, f, nil, nil, want)
		}
		if n, ok := w.(*Named); ok && n.Obj.Consts[fn.Name.Name] != nil {
			// A constant that's a function making one: .cmd("k") where a
			// Shortcut is expected.
			return c.closureCall(e, fn, c.expr(fn, want))
		}
		if n, ok := w.(*Named); ok && n.Obj.Kind == EnumType {
			if k := n.Obj.Case(fn.Name.Name); k != nil {
				return c.caseCall(e, n.Obj, k, n)
			}
			c.errorf(fn, diag.UnknownMember, "%s has no case %s", typeString(n), fn.Name.Name)
		} else if w == nil {
			c.errorf(fn, diag.CannotInferType, "can't tell what .%s is here: write its type, Type.%s(…)", fn.Name.Name, fn.Name.Name)
		} else if !IsInvalid(w) {
			c.errorf(fn, diag.TypeMismatch, ".%s(…) makes an enum's case, and %s isn't an enum", fn.Name.Name, aType(w))
		}
		c.checkArgsLoosely(e)
		return Invalid
	}
	return c.closureCall(e, e.Fun, c.expr(e.Fun, nil))
}

func exprName(e syntax.Expr) string {
	switch e := e.(type) {
	case *syntax.Ident:
		return e.Name
	case *syntax.TypeApp:
		return exprName(e.X) + "<…>"
	case *syntax.MemberExpr:
		if e.Optional {
			return exprName(e.X) + "?." + e.Name.Name
		}
		return exprName(e.X) + "." + e.Name.Name
	case *syntax.CallExpr:
		if e.Args == nil || len(e.Args.List) == 0 {
			return exprName(e.Fun) + "()"
		}
		return exprName(e.Fun) + "(…)"
	case *syntax.ParenExpr:
		return exprName(e.X)
	}
	return "this"
}

// componentOf reports whether a callee names a component.
func (c *Checker) componentOf(fun syntax.Expr) (*TypeName, bool) {
	var o Object
	switch f := fun.(type) {
	case *syntax.Ident:
		o = c.lookup(f.Name)
	case *syntax.MemberExpr:
		if id, ok := f.X.(*syntax.Ident); ok {
			if pn, ok := c.lookup(id.Name).(*PkgName); ok {
				o = pn.Pkg.Scope.LookupLocal(f.Name.Name)
			}
		}
	}
	if tn, ok := o.(*TypeName); ok && tn.Kind == ComponentType {
		return tn, true
	}
	return nil, false
}

// checkArgsLoosely checks a bad call's arguments, for their own errors.
func (c *Checker) checkArgsLoosely(e *syntax.CallExpr) {
	if e.Args != nil {
		for _, a := range e.Args.List {
			if _, ok := a.Value.(*syntax.LambdaExpr); ok {
				continue
			}
			c.expr(a.Value, nil)
		}
	}
}

// ---------------------------------------------------------------------
// Arguments

// matchArgs matches a call's arguments to params: positional ones first,
// then by name. It returns the argument per parameter (nil for one left
// to its default).
func (c *Checker) matchArgs(e *syntax.CallExpr, params []*Param, what string) []syntax.Expr {
	var list []*syntax.Arg
	if e.Args != nil {
		list = e.Args.List
	}
	return c.matchArgList(e, list, params, what)
}

func (c *Checker) matchArgList(e *syntax.CallExpr, list []*syntax.Arg, params []*Param, what string) []syntax.Expr {
	out := make([]syntax.Expr, len(params))
	named := false
	// unmatched: an argument went to no parameter (reported), which a
	// parameter left without one was most likely meant to get: that's
	// not reported again.
	unmatched := false
	pos := 0
	for _, a := range list {
		if a.Label == nil {
			if named {
				c.errorf(a, diag.ArgumentLabel, "a positional argument after a named one: put it first")
				unmatched = true
				continue
			}
			if pos >= len(params) {
				c.errorf(a, diag.ArgumentCount, "too many arguments: %s takes %s", what, plural(len(params), "parameter", "parameters"))
				c.expr(a.Value, nil)
				continue
			}
			out[pos] = a.Value
			pos++
			continue
		}
		named = true
		i := paramIndex(params, a.Label.Name)
		if i < 0 {
			c.errorf(a.Label, diag.ArgumentLabel, "%s has no parameter %s%s", what, a.Label.Name, paramSuggest(params, a.Label.Name))
			c.expr(a.Value, nil)
			unmatched = true
			continue
		}
		if out[i] != nil {
			c.errorf(a.Label, diag.ArgumentLabel, "%s is given twice", a.Label.Name)
			continue
		}
		out[i] = a.Value
	}
	for i, p := range params {
		if i == len(params)-1 && e.Block != nil {
			if _, ok := p.Type.(*FuncType); ok {
				continue // the trailing block
			}
		}
		if out[i] == nil && p.Default == nil && !p.optional() && !unmatched {
			at := syntax.Node(e)
			if e.Args != nil {
				at = e.Args
			}
			if n, ok := payloadIndex(p); ok {
				c.errorf(at, diag.ArgumentCount, "%s needs its payload %d: %s", what, n, aType(p.Type))
			} else {
				c.errorf(at, diag.ArgumentCount, "%s needs %s: %s", what, p.Name, typeString(p.Type))
			}
		}
	}
	return out
}

// HasZero reports whether t has a zero value a left-out enum payload
// takes: false, 0, "", an empty array or map, none, a payloadless enum's
// first case. Functions, structs and other enums have none.
func HasZero(t Type) bool {
	switch x := t.(type) {
	case *Basic:
		return x == Bool || x == Int || x == Float || x == String || x == Bytes || x == Duration || x == Color || x == Length
	case *Array, *Map, *Set, *Optional:
		return true
	case *Named:
		return x.Obj.Kind == EnumType && !x.Obj.RefEnum() && len(x.Args) == 0
	}
	return false
}

// optional: a parameter that may be left out with no default: a
// builtin's optional argument.
func (p *Param) optional() bool { return p.Var != nil && p.Var.Kind == optionalParam }

const optionalParam VarKind = 200

func paramIndex(ps []*Param, name string) int {
	for i, p := range ps {
		if p.Name == name {
			return i
		}
	}
	return -1
}

func paramSuggest(ps []*Param, name string) string {
	for _, p := range ps {
		if editDistance(p.Name, name) <= 2 {
			return " (did you mean " + p.Name + "?)"
		}
	}
	return ""
}

// ---------------------------------------------------------------------
// Inference

// unify binds the free type parameters in p so that p fits a.
func (c *Checker) unify(p, a Type, s Subst, free map[*TypeParam]bool) bool {
	if IsInvalid(a) || a == Never {
		return true
	}
	switch x := p.(type) {
	case *TypeParam:
		if !free[x] {
			return Identical(p, a)
		}
		if a == NoneType {
			return true
		}
		if b, ok := s[x]; ok && b != nil {
			if Identical(b, a) {
				return true
			}
			if IsOptional(b) && Identical(Unopt(b), a) {
				return true
			}
			if IsOptional(a) && Identical(Unopt(a), b) {
				s[x] = a
				return true
			}
			return false
		}
		s[x] = a
		return true
	case *Array:
		y, ok := a.(*Array)
		return ok && c.unify(x.Elem, y.Elem, s, free)
	case *Map:
		y, ok := a.(*Map)
		return ok && c.unify(x.Key, y.Key, s, free) && c.unify(x.Elem, y.Elem, s, free)
	case *Set:
		y, ok := a.(*Set)
		return ok && c.unify(x.Elem, y.Elem, s, free)
	case *Tree:
		y, ok := a.(*Tree)
		return ok && c.unify(x.Key, y.Key, s, free) && c.unify(x.Elem, y.Elem, s, free)
	case *Optional:
		if a == NoneType {
			return true
		}
		if y, ok := a.(*Optional); ok {
			return c.unify(x.Elem, y.Elem, s, free)
		}
		return c.unify(x.Elem, a, s, free)
	case *Tuple:
		y, ok := a.(*Tuple)
		if !ok || len(y.Elems) != len(x.Elems) {
			return false
		}
		for i := range x.Elems {
			if !c.unify(x.Elems[i], y.Elems[i], s, free) {
				return false
			}
		}
		return true
	case *FuncType:
		y, ok := a.(*FuncType)
		if !ok || len(y.Params) != len(x.Params) {
			return false
		}
		for i := range x.Params {
			if !c.unify(x.Params[i], y.Params[i], s, free) {
				return false
			}
		}
		return c.unify(x.Result, y.Result, s, free)
	case *Task:
		y, ok := a.(*Task)
		return ok && c.unify(x.Elem, y.Elem, s, free)
	case *Stream:
		y, ok := a.(*Stream)
		return ok && c.unify(x.Elem, y.Elem, s, free)
	case *Seq:
		y, ok := a.(*Seq)
		return ok && c.unify(x.Elem, y.Elem, s, free)
	case *Named:
		y, ok := a.(*Named)
		if !ok || y.Obj != x.Obj || len(y.Args) != len(x.Args) {
			return false
		}
		for i := range x.Args {
			if !c.unify(x.Args[i], y.Args[i], s, free) {
				return false
			}
		}
		return true
	}
	return true
}

// mentionsFree reports whether t mentions a free type parameter.
func mentionsFree(t Type, free map[*TypeParam]bool, s Subst) bool {
	if len(free) == 0 {
		return false
	}
	t = s.Apply(t)
	found := false
	var walk func(Type)
	walk = func(t Type) {
		switch x := t.(type) {
		case *TypeParam:
			if free[x] {
				found = true
			}
		case *Array:
			walk(x.Elem)
		case *Map:
			walk(x.Key)
			walk(x.Elem)
		case *Set:
			walk(x.Elem)
		case *Tree:
			walk(x.Key)
			walk(x.Elem)
		case *Optional:
			walk(x.Elem)
		case *Tuple:
			for _, e := range x.Elems {
				walk(e)
			}
		case *FuncType:
			for _, p := range x.Params {
				walk(p)
			}
			walk(x.Result)
		case *Task:
			walk(x.Elem)
		case *Stream:
			walk(x.Elem)
		case *Seq:
			walk(x.Elem)
		case *Named:
			for _, a := range x.Args {
				walk(a)
			}
		}
	}
	walk(t)
	return found
}

// inferArgs checks args against params whose types mention the free
// type parameters, binding them as it goes; it returns the bindings.
// Lambdas go last, so that their parameters' types are known.
// enterArg enters what an argument is checked in: a binding of its own
// when the callee follows its arguments (a store's: bindingArgs).
func (c *Checker) enterArg(a syntax.Expr) func() {
	if !c.bindingArgs {
		return func() {}
	}
	saved := c.ctx
	c.enter(CtxBinding, a, "binding")
	return func() { c.leave(saved) }
}

func (c *Checker) inferArgs(args []syntax.Expr, params []*Param, s Subst, free map[*TypeParam]bool, want, result Type, what string) {
	if ren := c.callersOwn(free); ren != nil {
		// The callee's type parameters that are the caller's too (a
		// generic calling itself, a generic type's method making one of
		// it) are variables of their own here: the caller's T, in its
		// arguments' types, is a type like any other.
		fresh := map[*TypeParam]bool{}
		for tp := range free {
			if r, ok := ren[tp]; ok {
				fresh[r.(*TypeParam)] = true
			} else {
				fresh[tp] = true
			}
		}
		ps := make([]*Param, len(params))
		for i, p := range params {
			q := *p
			q.Type = ren.Apply(p.Type)
			ps[i] = &q
		}
		defer func() {
			for tp, r := range ren {
				if t := s[r.(*TypeParam)]; t != nil {
					s[tp] = t
				}
				delete(s, r.(*TypeParam))
			}
		}()
		params, free, result = ps, fresh, ren.Apply(result)
	}
	savedFree := c.free
	c.free = free
	defer func() { c.free = savedFree }()
	// What's expected of the result binds first: json.decode, Box(…) as
	// a Box<int>.
	// A result that's a type parameter alone, where an optional is
	// expected, is what the optional holds (let f: float? = pick(n => 1))
	// unless an argument says it's the optional (pick(n => maybe(n))):
	// loose is that parameter, until one does. An optional that doesn't
	// meet its constraints can't be it.
	var loose *TypeParam
	if want != nil && mentionsFree(result, free, s) {
		w := want
		if !IsOptional(result) {
			w = Unopt(want)
		}
		tp, alone := result.(*TypeParam)
		c.unify(result, w, s, free)
		if alone && IsOptional(want) && s[tp] != nil && !IsOptional(s[tp]) && !c.isEdges(s[tp]) {
			if failed, _ := c.satisfies(&Optional{s[tp]}, tp); failed == "" {
				loose = tp
			}
		}
	}
	// Closures that give the result wait for the other arguments, when
	// nothing else of the parameters holds the type parameter to one type.
	var lambdas []*looseLambda
	waits := loose != nil && resultOnly(params, loose)
	checked := make([]bool, len(args))
	types := make([]Type, len(args))
	open := make([]bool, len(args))
	for pass := 0; pass < 2; pass++ {
		for i, a := range args {
			if a == nil || checked[i] {
				continue
			}
			_, isLambda := a.(*syntax.LambdaExpr)
			if (isLambda || isEmptyLit(a)) && pass == 0 {
				// [] and [:] say nothing of their types: the other
				// arguments say them first (f([], 1)).
				continue
			}
			pt := params[i].Type
			leave := c.enterArg(a)
			open[i] = loose != nil && !IsOptional(s[loose]) && mentionsOne(pt, loose)
			if !open[i] && !mentionsFree(pt, free, s) {
				c.assign(a, s.Apply(pt), argDesc(params[i])+" of "+what)
				leave()
				checked[i] = true
				continue
			}
			pw := s.Apply(pt)
			if tp, ok := pw.(*TypeParam); ok && free[tp] && s[tp] == nil {
				if arr, ok := a.(*syntax.ArrayExpr); ok && len(arr.Elems) == 0 {
					// [] for a type parameter nothing else tells: an array
					// with no elements is one of any type (db.exec(sql, [])).
					s[tp] = &Array{Int}
					pw = s[tp]
				}
			}
			if isEmptyLit(a) && mentionsFree(pw, free, s) {
				// Nothing tells its type: the call says so (can't tell
				// f's type T).
				c.expr(a, Invalid)
				leave()
				checked[i] = true
				continue
			}
			w := pw
			if !isLambda && mentionsFree(w, free, s) {
				// What isn't known yet doesn't steer the argument.
				w = nil
			}
			if open[i] && pt == Type(loose) {
				// The type parameter whole: the argument may be the optional.
				w = &Optional{pw}
			}
			var l *looseLambda
			if f, ok := Unopt(pt).(*FuncType); ok && open[i] && waits && isLambda && f.Result == Type(loose) {
				l = &looseLambda{}
				c.loose = l
			}
			got := c.expr(a, w)
			leave()
			types[i] = got
			checked[i] = true
			if open[i] {
				// An optional where the parameter has the type parameter
				// makes it the optional, for the arguments after too.
				if l != nil && l.ctx != nil {
					lambdas = append(lambdas, l)
				}
				if l != nil && l.optional() || optionalAt(pt, got, loose) {
					s[loose] = &Optional{s[loose]}
				}
				if !mentionsFree(pt, free, s) {
					continue
				}
				pw = s.Apply(pt)
			}
			if !c.unify(pw, got, s, free) {
				c.errorf(a, diag.ConstraintNotMet, "%s of %s is %s, which doesn't fit %s", argDesc(params[i]), what, aType(got), typeString(s.Apply(pt)))
			}
		}
	}
	for _, l := range lambdas {
		l.settle(c, s[loose])
	}
	for i, a := range args {
		if a == nil || types[i] == nil {
			continue
		}
		pt := s.Apply(params[i].Type)
		if mentionsFree(pt, free, s) {
			continue
		}
		if !c.convert(a, types[i], pt) {
			hint := ""
			if open[i] {
				hint = c.convHint(types[i], pt)
			}
			c.errorf(a, diag.TypeMismatch, "%s of %s is %s, not %s%s", argDesc(params[i]), what, aType(types[i]), aType(pt), hint)
		}
	}
}

// callersOwn renames the type parameters of free that are in scope where
// the call is (the caller's own) to fresh ones, in the order they were
// declared; nil when there are none.
func (c *Checker) callersOwn(free map[*TypeParam]bool) Subst {
	var own []*TypeParam
	for tp := range free {
		if tn := c.scope.LookupType(tp.Name); tn != nil && tn.Type == tp {
			own = append(own, tp)
		}
	}
	if len(own) == 0 {
		return nil
	}
	slices.SortFunc(own, func(a, b *TypeParam) int { return a.id - b.id })
	ren := Subst{}
	for _, tp := range own {
		r := *tp
		r.id, r.obj = c.nextTP, nil
		c.nextTP++
		ren[tp] = &r
	}
	return ren
}

// payloadIndex is the place (from 1) of an enum case's payload without
// a label, which its call's params name _0, _1, ….
func payloadIndex(p *Param) (int, bool) {
	if len(p.Name) < 2 || p.Name[0] != '_' {
		return 0, false
	}
	n := 0
	for _, r := range p.Name[1:] {
		if r < '0' || r > '9' {
			return 0, false
		}
		n = n*10 + int(r-'0')
	}
	return n + 1, true
}

// argDesc names a parameter's argument in a message: argument title, or
// payload 2 (a case's without a label).
func argDesc(p *Param) string {
	if n, ok := payloadIndex(p); ok {
		return "payload " + itoa(n)
	}
	return "argument " + p.Name
}

// isEmptyLit reports [] or [:], which say nothing of their types.
func isEmptyLit(e syntax.Expr) bool {
	switch e := unparen(e).(type) {
	case *syntax.ArrayExpr:
		return len(e.Elems) == 0
	case *syntax.MapExpr:
		return len(e.Entries) == 0
	}
	return false
}

// triedWant returns what's expected of the call e: the optional its try
// was given, where want is what that holds (unary).
func (c *Checker) triedWant(e *syntax.CallExpr, want Type) Type {
	if c.tried == e {
		return c.triedOpt
	}
	return want
}

// mentionsOne reports whether t mentions tp, in a function's type too.
func mentionsOne(t Type, tp *TypeParam) bool {
	return mentionsFree(t, map[*TypeParam]bool{tp: true}, nil)
}

// resultOnly reports whether the parameters have tp only as one's whole
// type or as the result of a function that takes none of it: making tp
// an optional then changes no argument that was checked as what it
// holds, but to put it in one.
func resultOnly(params []*Param, tp *TypeParam) bool {
	for _, p := range params {
		if !mentionsOne(p.Type, tp) || p.Type == Type(tp) {
			continue
		}
		f, ok := Unopt(p.Type).(*FuncType)
		if !ok || f.Result != Type(tp) {
			return false
		}
		for _, fp := range f.Params {
			if mentionsOne(fp, tp) {
				return false
			}
		}
	}
	return true
}

// optionalAt reports whether a, an argument's type, has an optional (or
// is none) where p, its parameter's, has tp.
func optionalAt(p, a Type, tp *TypeParam) bool {
	switch x := p.(type) {
	case *TypeParam:
		return x == tp && (a == NoneType || IsOptional(a))
	case *Array:
		y, ok := a.(*Array)
		return ok && optionalAt(x.Elem, y.Elem, tp)
	case *Map:
		y, ok := a.(*Map)
		return ok && (optionalAt(x.Key, y.Key, tp) || optionalAt(x.Elem, y.Elem, tp))
	case *Set:
		y, ok := a.(*Set)
		return ok && optionalAt(x.Elem, y.Elem, tp)
	case *Tree:
		y, ok := a.(*Tree)
		return ok && (optionalAt(x.Key, y.Key, tp) || optionalAt(x.Elem, y.Elem, tp))
	case *Optional:
		y, ok := a.(*Optional)
		return ok && optionalAt(x.Elem, y.Elem, tp)
	case *Tuple:
		y, ok := a.(*Tuple)
		if !ok || len(y.Elems) != len(x.Elems) {
			return false
		}
		for i := range x.Elems {
			if optionalAt(x.Elems[i], y.Elems[i], tp) {
				return true
			}
		}
	case *FuncType:
		y, ok := a.(*FuncType)
		return ok && optionalAt(x.Result, y.Result, tp)
	case *Task:
		y, ok := a.(*Task)
		return ok && optionalAt(x.Elem, y.Elem, tp)
	case *Stream:
		y, ok := a.(*Stream)
		return ok && optionalAt(x.Elem, y.Elem, tp)
	case *Seq:
		y, ok := a.(*Seq)
		return ok && optionalAt(x.Elem, y.Elem, tp)
	case *Named:
		y, ok := a.(*Named)
		if !ok || y.Obj != x.Obj || len(y.Args) != len(x.Args) {
			return false
		}
		for i := range x.Args {
			if optionalAt(x.Args[i], y.Args[i], tp) {
				return true
			}
		}
	}
	return false
}

// A looseLambda is a closure that gives a call's result, a type
// parameter alone, where an optional is expected: let o: Order? =
// ask(c => last(c)). Its result is what the optional holds, unless one
// of its values is an optional (or none): then it's the optional. Its
// values are checked as they come, and converted once the call's
// arguments have said which (settle).
type looseLambda struct {
	ctx  *FuncCtx
	ft   *FuncType
	vals []looseVal
}

type looseVal struct {
	e    syntax.Expr
	typ  Type
	what string
}

// value checks e, one of the closure's values.
func (l *looseLambda) value(c *Checker, e syntax.Expr, what string) {
	l.vals = append(l.vals, looseVal{e, c.expr(e, &Optional{l.ctx.Result}), what})
}

// optional reports whether one of the values is an optional. An if, a
// match and a catch are the optional expected of them whatever their
// branches are, so the type parameter is the optional for those too:
// one that meets its constraints (inferArgs), and its values are put in
// it either way.
func (l *looseLambda) optional() bool {
	for _, v := range l.vals {
		if v.typ == NoneType || IsOptional(v.typ) {
			return true
		}
	}
	return false
}

// settle makes res the closure's result, and converts its values to it.
func (l *looseLambda) settle(c *Checker, res Type) {
	for _, v := range l.vals {
		if !c.convert(v.e, v.typ, res) {
			c.errorf(v.e, diag.TypeMismatch, "%s is %s, not %s%s", v.what, aType(v.typ), aType(res), c.convHint(v.typ, res))
		}
	}
	l.ft.Result, l.ctx.Result = res, res
}

// ---------------------------------------------------------------------
// Functions and methods

func (c *Checker) funcCall(e *syntax.CallExpr, f *Function, recv syntax.Expr, recvType Type, want Type) Type {
	return c.funcCallInfo(e, f, recv, recvType, want).Result
}

func (c *Checker) funcCallInfo(e *syntax.CallExpr, f *Function, recv syntax.Expr, recvType Type, want Type) *Call {
	return c.funcCallAs(e, f, recv, recvType, want, false)
}

// funcCallAs is funcCallInfo; async says the call is async whatever f is
// (a worker's pub fn, called from outside).
func (c *Checker) funcCallAs(e *syntax.CallExpr, f *Function, recv syntax.Expr, recvType Type, want Type, async bool) *Call {
	return c.funcCallPre(e, f, recv, recvType, want, async, nil)
}

// funcCallPre is funcCallAs with some of f's type parameters given (pre):
// a generic type's static fn called as Box<int>.empty().
func (c *Checker) funcCallPre(e *syntax.CallExpr, f *Function, recv syntax.Expr, recvType Type, want Type, async bool, pre Subst) *Call {
	c.funcSig(f)
	c.noteCall(f, e)
	what := f.name
	params := f.Sig.Params
	args := c.matchArgs(e, params, what)
	c.trailingClosure(e, params, args)
	s := Subst{}
	for k, v := range pre {
		s[k] = v
	}
	if f.IsExtMethod() {
		s = ExtSubst(f, recvType)
	} else if n, ok := recvType.(*Named); ok {
		for k, v := range n.Subst() {
			s[k] = v
		}
	}
	free := map[*TypeParam]bool{}
	for _, tp := range f.TypeParams {
		free[tp] = true
	}
	c.explicitTypeArgs(e, what, f.TypeParams, s, free)
	c.inferArgs(args, params, s, free, c.triedWant(e, want), f.Sig.Result, what)
	call := &Call{Kind: CallFunc, Func: f, Args: args, Params: params, Recv: recv, RecvType: recvType}
	if f.Recv != nil || f.IsExtMethod() {
		call.Kind = CallMethod
	}
	for _, tp := range f.TypeParams {
		t := s[tp]
		if t == nil {
			c.errorf(e, diag.CannotInferType, "can't tell %s's type %s from its arguments: pass them, or say what's expected (let x: T = %s(…))", what, tp.Name, what)
			t = Invalid
			s[tp] = Invalid
		} else {
			c.checkConstraints(e, what, tp, t)
		}
		call.TypeArgs = append(call.TypeArgs, t)
	}
	call.Result = s.Apply(f.Sig.Result)
	c.info.Calls[e] = call
	if f.Sig.Mut {
		call.Mut = true
		if recv == nil {
			c.selfWrite(e.Fun, "call "+f.name+" on", "self")
		} else {
			c.mutReceiver(recv, f.name)
		}
	}
	c.callEffects(e, what, f.Sig.Async || async, f.Sig.Throws)
	if len(f.TypeParams) > 0 {
		c.genericCalls = append(c.genericCalls, genericCall{e: e, f: f, args: call.TypeArgs, encl: c.enclosingFunc(), own: c.enclosingParams()})
		c.noteInstance(e, what, f.TypeParams, call.TypeArgs)
	}
	if n, ok := recvType.(*Named); ok && f.Recv != nil && n.Obj == f.Recv {
		// A generic type's method: compiled for its receiver's arguments.
		c.noteInstance(e, f.Recv.name, f.Recv.TypeParams, n.Args)
	}
	return call
}

// checkConstraints reports a type argument t of what's p that doesn't
// meet p's constraints.
func (c *Checker) checkConstraints(at syntax.Node, what string, p *TypeParam, t Type) {
	if IsInvalid(t) {
		return
	}
	if failed, why := c.satisfies(t, p); failed != "" {
		if why != "" {
			why = ": " + why
		}
		c.errorf(at, diag.ConstraintNotMet, "%s's %s must be %s, and %s isn't%s", what, p.Name, failed, typeString(t), why)
	}
}

// satisfies reports the constraint of p that t doesn't meet, and why
// ("", "" when it meets them all).
func (c *Checker) satisfies(t Type, p *TypeParam) (failed, why string) {
	for _, b := range p.Builtins {
		why := ""
		switch b {
		case "Equatable":
			if !IsEquatable(t) {
				why = whyNotEquatable(t)
			}
		case "Comparable":
			why = c.whyNotComparable(t)
		case "Hashable":
			why = whyNotHashable(t)
		}
		if why != "" {
			return b, why
		}
	}
	for _, i := range p.Ifaces {
		if why := c.hasIface(t, i); why != "" {
			return typeString(i), why
		}
	}
	return "", ""
}

// hasIface reports why a type argument t doesn't have interface iface's
// methods and fields, "" when it does: a struct's or an enum's own (as
// implements says, without its storage: a generic function is compiled
// per type, so it calls a payloadless enum's methods as they are), an
// interface's that asks for them, or a type parameter's constraints'.
func (c *Checker) hasIface(t Type, iface *Named) string {
	c.resolveType(iface.Obj)
	if iface.Obj.isErrorIface {
		if IsErrorType(t) {
			return ""
		}
		if p, ok := t.(*TypeParam); ok {
			for _, i := range p.Ifaces {
				if i.Obj.isErrorIface {
					return ""
				}
			}
		}
		return "it isn't an error (declare it one: struct " + typeString(t) + ": Error)"
	}
	if iface.Obj.Iface == nil {
		return "" // still being declared
	}
	switch x := t.(type) {
	case *Named:
		switch x.Obj.Kind {
		case StructType, EnumType:
			return c.implementsMembers(t, iface)
		case InterfaceType:
			if Identical(x, iface) {
				return ""
			}
			c.resolveType(x.Obj)
			return c.ifaceCovers(func(name string) *IfaceMethod { m, _ := ifaceMethod(x, name); return m },
				func(name string) *Field { f, _ := ifaceField(x, name); return f }, iface)
		}
	case *TypeParam:
		for _, i := range x.Ifaces {
			if Identical(i, iface) {
				return ""
			}
		}
		return c.ifaceCovers(func(name string) *IfaceMethod { m, _ := paramMethod(x, name); return m },
			func(name string) *Field { f, _ := paramField(x, name); return f }, iface)
	}
	return "only structs, enums and interfaces have interfaces"
}

// ifaceCovers reports why what method and field look up doesn't cover
// iface's, "" when it does.
func (c *Checker) ifaceCovers(method func(string) *IfaceMethod, field func(string) *Field, iface *Named) string {
	s := iface.Subst()
	for _, m := range iface.Obj.Iface.Methods {
		if m.Provided != nil {
			continue
		}
		got := method(m.Name)
		if got == nil {
			return "it has no " + m.Name + "()"
		}
		if want := s.Apply(m.Sig.Type()); !Identical(got.Sig.Type(), want) {
			return "its " + m.Name + " is " + typeString(got.Sig.Type()) + ", not " + typeString(want)
		}
		if got.Sig.Mut && !m.Sig.Mut {
			return mutNotAsked(m.Name)
		}
	}
	for _, fl := range iface.Obj.Iface.Fields {
		got := field(fl.Name)
		if got == nil {
			return "it has no field " + fl.Name
		}
		if want := s.Apply(fl.Type); !Identical(got.Type, want) {
			return "its field " + fl.Name + " is " + typeString(got.Type) + ", not " + typeString(want)
		}
	}
	return ""
}

// ifaceMethod is an interface's method, its signature read with the
// interface's type arguments, and its number (from 1).
func ifaceMethod(n *Named, name string) (*IfaceMethod, int) {
	if n.Obj.Iface == nil {
		return nil, 0
	}
	for i, m := range n.Obj.Iface.Methods {
		if m.Name == name {
			if s := n.Subst(); s != nil {
				return &IfaceMethod{Name: m.Name, Sig: s.applySig(m.Sig), Provided: m.Provided}, i + 1
			}
			return m, i + 1
		}
	}
	return nil, 0
}

// ifaceField is an interface's field, its type read with the
// interface's type arguments.
func ifaceField(n *Named, name string) (*Field, int) {
	if n.Obj.Iface == nil {
		return nil, 0
	}
	for i, f := range n.Obj.Iface.Fields {
		if f.Name == name {
			if s := n.Subst(); s != nil {
				g := *f
				g.Type = s.Apply(f.Type)
				return &g, i
			}
			return f, i
		}
	}
	return nil, 0
}

// ParamMethod is a method a type parameter's interfaces ask for (or
// provide), and the interface.
func ParamMethod(p *TypeParam, name string) (*IfaceMethod, *Named) { return paramMethod(p, name) }

// IfaceMethodOf is interface n's method name, its signature read with
// n's type arguments; nil when it has none (or n isn't an interface).
func IfaceMethodOf(n *Named, name string) *IfaceMethod {
	if n.Obj.Kind != InterfaceType {
		return nil
	}
	m, _ := ifaceMethod(n, name)
	return m
}

// paramMethod is a method a type parameter's interfaces ask for, and the
// interface.
func paramMethod(p *TypeParam, name string) (*IfaceMethod, *Named) {
	for _, i := range p.Ifaces {
		if m, _ := ifaceMethod(i, name); m != nil {
			return m, i
		}
	}
	return nil, nil
}

// paramField is a field a type parameter's interfaces ask for (an
// error's message among them), and the interface.
func paramField(p *TypeParam, name string) (*Field, *Named) {
	for _, i := range p.Ifaces {
		if f, _ := ifaceField(i, name); f != nil {
			return f, i
		}
	}
	return nil, nil
}

// trailingClosure makes a call's trailing block its last argument when
// that's a function: withTimeout(5s) { … }.
func (c *Checker) trailingClosure(e *syntax.CallExpr, params []*Param, args []syntax.Expr) {
	if e.Block == nil {
		return
	}
	n := len(params)
	if n == 0 || args[n-1] != nil {
		c.errorf(e.Block, diag.UnexpectedBlock, "a block after this call has nowhere to go: it takes no function last")
		return
	}
	if _, ok := params[n-1].Type.(*FuncType); !ok {
		c.errorf(e.Block, diag.UnexpectedBlock, "a block after this call has nowhere to go: its last parameter isn't a function")
		return
	}
	lam := &syntax.LambdaExpr{Span: e.Block.Span, Body: e.Block}
	args[n-1] = lam
	c.info.Trailing[e] = lam
}

// callEffects checks that a call to an async or throwing function is
// awaited or tried.
func (c *Checker) callEffects(at syntax.Node, what string, async, throws bool) {
	c.callEffectsWhy(at, what, async, throws, nil)
}

// An inferredFx is what made a closure without an expected type async
// or throwing: the first await in its body (NoPos when it says async),
// the first thing that lets an error out, and the let that names it.
type inferredFx struct {
	lambda  *syntax.LambdaExpr
	awaitAt syntax.Pos
	threwAt syntax.Node
	letName string // "let refresh", when a let names it
}

// callEffectsWhy is callEffects for a call of a closure whose effects
// came from its body (fx, or nil): the errors say where.
func (c *Checker) callEffectsWhy(at syntax.Node, what string, async, throws bool, fx *inferredFx) {
	missAwait := async && c.inAwait == 0
	missTry := throws && c.inTry == 0
	awaitWhy, throwWhy := "", ""
	if missAwait && fx != nil && fx.awaitAt.IsValid() {
		awaitWhy = fmt.Sprintf("; it's async because it awaits at %s (if it's meant to be, mark it: %s)", c.where(fx.awaitAt), fx.markedAsync())
	}
	if missTry && fx != nil && fx.threwAt != nil {
		throwWhy = fmt.Sprintf("; it can throw because of %s at %s", throwCause(fx.threwAt), c.where(fx.threwAt.Range().Start))
	}
	// One error for the call: what it misses, and what a try would then
	// need of what's around it.
	also := ""
	if missTry {
		if msg := c.throwHere(at); msg != "" {
			also = "; and " + msg
		}
	}
	canWait := c.ctx != nil && c.ctx.Async
	switch {
	case missAwait && missTry && canWait:
		c.errorf(at, diag.MissingAwait, "%s is async and can throw: call it with try await (try await %s(…)), or try? await for none on an error%s%s%s", what, what, awaitWhy, throwWhy, also)
	case missAwait && missTry:
		c.errorf(at, c.awaitCode(), "%s is async and can throw, and this can't wait: %s%s%s%s", what, c.asyncHint(), awaitWhy, throwWhy, also)
	case missAwait && canWait:
		c.errorf(at, diag.MissingAwait, "%s is async: await it (await %s(…)), or start it with spawn%s", what, what, awaitWhy)
	case missAwait:
		c.errorf(at, c.awaitCode(), "%s is async, and this can't wait: %s%s", what, c.asyncHint(), awaitWhy)
	case missTry:
		c.errorf(at, diag.MissingTry, "%s can throw: call it with try (try %s(…)), or try? for none on an error%s%s", what, what, throwWhy, also)
	}
}

// where is pos as its file's name, line and column: app.asuka:4:5.
func (c *Checker) where(pos syntax.Pos) string {
	p := c.fset.Position(pos)
	p.Filename = filepath.Base(p.Filename)
	return p.String()
}

// markedAsync is the closure written with async: let refresh = async
// () => ….
func (fx *inferredFx) markedAsync() string {
	var b strings.Builder
	if fx.letName != "" {
		b.WriteString(fx.letName + " = ")
	}
	b.WriteString("async ")
	l := fx.lambda
	if !l.Parens && len(l.Params) == 1 {
		b.WriteString(l.Params[0].Name.Name)
	} else {
		b.WriteString("(")
		for i, p := range l.Params {
			if i > 0 {
				b.WriteString(", ")
			}
			b.WriteString(p.Name.Name)
			if p.Type != nil {
				b.WriteString(": …")
			}
		}
		b.WriteString(")")
	}
	b.WriteString(" => …")
	return b.String()
}

// throwCause names what let an error out of a closure's body.
func throwCause(n syntax.Node) string {
	switch n := n.(type) {
	case *syntax.UnaryExpr:
		if n.Op == syntax.TRY {
			return "the try"
		}
	case *syntax.ThrowStmt:
		return "the throw"
	case *syntax.JumpExpr:
		return "the throw"
	case *syntax.ForStmt:
		return "the for await"
	}
	return "the call"
}

func (c *Checker) closureCall(e *syntax.CallExpr, fun syntax.Expr, t Type) Type {
	if IsInvalid(t) {
		c.checkArgsLoosely(e)
		return Invalid
	}
	ft, ok := t.(*FuncType)
	if !ok {
		c.errorf(fun, diag.NotCallable, "%s is %s, not a function", exprName(fun), aType(t))
		c.checkArgsLoosely(e)
		return Invalid
	}
	var args []syntax.Expr
	if e.Args != nil {
		for i, a := range e.Args.List {
			if a.Label != nil {
				c.errorf(a.Label, diag.ArgumentLabel, "a function value's arguments have no names")
			}
			if i < len(ft.Params) {
				what := "argument " + itoa(i+1)
				if n := exprName(fun); n != "this" {
					what += " of " + n
				}
				c.assign(a.Value, ft.Params[i], what)
			} else {
				c.expr(a.Value, nil)
			}
			args = append(args, a.Value)
		}
	}
	if len(args) != len(ft.Params) {
		c.errorf(e, diag.ArgumentCount, "%s takes %s, not %d", exprName(fun), plural(len(ft.Params), "argument", "arguments"), len(args))
	}
	c.info.Calls[e] = &Call{Kind: CallClosure, Recv: fun, Args: args, Result: ft.Result, RecvType: ft}
	c.callEffectsWhy(e, exprName(fun), ft.Async, ft.Throws, c.inferredFx[ft])
	if c.ctx != nil {
		c.ctx.readsState = true // what it does isn't known here
	}
	return ft.Result
}

// methodCall is x.name(…) on a value.
func (c *Checker) methodCall(e *syntax.CallExpr, fn *syntax.MemberExpr, want Type) Type {
	name := fn.Name.Name
	var xt Type
	if implicitRoot(fn.X) {
		xt = c.expr(fn.X, want)
	} else {
		xt = c.expr(fn.X, nil)
	}
	if IsInvalid(xt) {
		c.checkArgsLoosely(e)
		return Invalid
	}
	optional := false
	if fn.Optional || IsOptional(xt) && c.chained[unparen(fn.X)] {
		c.chained[e] = true
		if !IsOptional(xt) {
			c.errorf(fn, diag.NotOptional, "%s isn't optional: use . (?. is for optionals)", typeString(xt))
		}
		xt = Unopt(xt)
		optional = true
	} else if IsOptional(xt) && lookupMethod(xt, name) == nil {
		c.errorf(fn, diag.MaybeNone, "%s may be none: use ?. (x?.%s()), or unwrap it first", typeString(xt), name)
		c.checkArgsLoosely(e)
		return Invalid
	}
	res := c.methodCall1(e, fn, xt, name, want)
	if call := c.info.Calls[e]; call != nil {
		call.Optional = optional
	}
	if optional && !IsInvalid(res) && res != Void {
		return MakeOptional(res)
	}
	return res
}

// ifaceMethodCall is x.name(…) of an interface's method m (index: its
// number, from 1, for a native interface's).
func (c *Checker) ifaceMethodCall(e *syntax.CallExpr, fn *syntax.MemberExpr, xt Type, m *IfaceMethod, kind CallKind, index int) Type {
	name := m.Name
	args := c.matchArgs(e, m.Sig.Params, name)
	for i, a := range args {
		if a != nil {
			c.assign(a, m.Sig.Params[i].Type, "argument "+m.Sig.Params[i].Name+" of "+name)
		}
	}
	c.info.Calls[e] = &Call{Kind: kind, Recv: fn.X, RecvType: xt, Args: args, Params: m.Sig.Params, Result: m.Sig.Result, Bound: map[string]Type{"name": nil}, Index: index, Name: name, Mut: m.Sig.Mut}
	if m.Sig.Mut {
		c.mutReceiver(fn.X, name)
	}
	c.info.Selections[fn] = &Selection{Kind: SelMethod, Name: name, Recv: xt}
	c.callEffects(e, name, m.Sig.Async, m.Sig.Throws)
	if c.ctx != nil {
		c.ctx.readsState = true
	}
	return m.Sig.Result
}

// selfIfaceCall is name(…) in an interface's provided method: a method
// of the interface, called on self (a CallIface of self's type
// parameter, resolved per type the body is compiled for).
func (c *Checker) selfIfaceCall(e *syntax.CallExpr, fn *syntax.Ident, want Type) Type {
	if c.ctx == nil || c.ctx.Self == nil {
		c.errorf(fn, diag.NotAValue, "%s is an interface's method: call it on a value", fn.Name)
		c.checkArgsLoosely(e)
		return Invalid
	}
	self, _ := c.ctx.Self.Type.(*TypeParam)
	var m *IfaceMethod
	if self != nil {
		m, _ = paramMethod(self, fn.Name)
	}
	if m == nil {
		c.errorf(fn, diag.NotAValue, "%s is an interface's method: call it on a value", fn.Name)
		c.checkArgsLoosely(e)
		return Invalid
	}
	c.useVar(c.ctx.Self, fn)
	args := c.matchArgs(e, m.Sig.Params, m.Name)
	for i, a := range args {
		if a != nil {
			c.assign(a, m.Sig.Params[i].Type, "argument "+m.Sig.Params[i].Name+" of "+m.Name)
		}
	}
	c.info.Calls[e] = &Call{Kind: CallIface, RecvType: self, Args: args, Params: m.Sig.Params, Result: m.Sig.Result, Bound: map[string]Type{"name": nil}, Name: m.Name, SelfCall: true}
	c.callEffects(e, m.Name, m.Sig.Async, m.Sig.Throws)
	if c.ctx != nil {
		c.ctx.readsState = true
	}
	return m.Sig.Result
}

// paramHint says what a type parameter without the member could name.
func paramHint(p *TypeParam) string {
	if len(p.Ifaces) == 0 {
		return ": constrain it to an interface that has it (<" + p.Name + ": Shape>)"
	}
	return " (its constraints, " + p.Constraint() + ", don't ask for it)"
}

func (c *Checker) methodCall1(e *syntax.CallExpr, fn *syntax.MemberExpr, xt Type, name string, want Type) Type {
	if n, ok := xt.(*Named); ok {
		switch n.Obj.Kind {
		case StructType, EnumType:
			if f := n.Obj.Methods[name]; f != nil {
				c.hiddenFn(fn.Name, n.Obj.name, f)
				return c.funcCall(e, f, fn.X, xt, want)
			}
			if n.Obj.Kind == StructType {
				if fl := n.Obj.Field(name); fl != nil {
					c.hiddenField(fn.Name, n.Obj.name, fl)
					if ft, ok := n.Subst().Apply(fl.Type).(*FuncType); ok {
						c.info.Selections[fn] = &Selection{Kind: SelField, Field: fl, Recv: xt}
						c.record(fn, ft)
						return c.closureCall(e, fn, ft)
					}
				}
			}
		case StoreType:
			if m := n.Obj.Comp.Members[name]; m != nil {
				if n.Obj.Pkg != c.pkg && !m.Pub {
					c.errorf(fn.Name, diag.NotPub, "%s isn't pub in store %s", name, n.Obj.name)
				}
				if m.Kind == FnMember {
					c.spawnState(fn, n.Obj.name+"."+name)
					if c.ctx != nil {
						c.ctx.readsState = true
					}
					return c.funcCall(e, m.Fn, fn.X, xt, want)
				}
				t := c.valueMember(fn, xt, name)
				c.record(fn, t)
				return c.closureCall(e, fn, t)
			}
		case WorkerType:
			if t, ok := c.workerMethodCall(e, fn, n, name, want); ok {
				return t
			}
		case InterfaceType:
			if m, i := ifaceMethod(n, name); m != nil {
				kind := CallIface
				if n.Obj.Native {
					kind = CallNative
				}
				return c.ifaceMethodCall(e, fn, xt, m, kind, i)
			}
		}
	}
	if hit, why := c.extLookup(xt, name, "method", fn.Name); hit.f != nil {
		return c.extMethodCall(e, fn, xt, hit, want)
	} else if why != "" {
		c.errorf(fn.Name, diag.ConstraintNotMet, "%s", why)
		c.checkArgsLoosely(e)
		return Invalid
	}
	if p, ok := xt.(*TypeParam); ok {
		// A constrained type parameter's method: CallIface, which the
		// lowering resolves per instantiation (a struct's or an enum's
		// method called directly, an interface's through CALLI).
		if m, _ := paramMethod(p, name); m != nil {
			return c.ifaceMethodCall(e, fn, xt, m, CallIface, 0)
		}
		if f, _ := paramField(p, name); f != nil {
			if ft, ok := f.Type.(*FuncType); ok {
				c.info.Selections[fn] = &Selection{Kind: SelIfaceField, Field: f, Recv: xt, Name: name}
				c.record(fn, ft)
				return c.closureCall(e, fn, ft)
			}
			c.errorf(fn.Name, diag.NotCallable, "%s is a field of %s, not a method: %s", name, p.Name, name)
			c.checkArgsLoosely(e)
			return Invalid
		}
		c.errorf(fn.Name, diag.UnknownMember, "%s has no method %s%s", p.Name, name, paramHint(p))
		c.checkArgsLoosely(e)
		return Invalid
	}
	if ms := methodsArity(xt, name, argCount(e)); len(ms) > 0 {
		m := ms[0]
		if len(ms) > 1 {
			if m = c.overloadByLabel(e, ms); m == nil {
				m = c.overload(e, ms)
			}
		}
		return c.builtinMethodCall(e, fn, xt, m, want)
	}
	if m := lookupMethod(xt, name); m != nil {
		if m.Field {
			c.errorf(e, diag.NotCallable, "%s is a property of %s, not a method: %s", name, typeString(xt), name)
		} else {
			c.errorf(e, diag.ArgumentCount, "%s takes %s", name, plural(len(m.Params), "argument", "arguments"))
		}
		c.checkArgsLoosely(e)
		return Invalid
	}
	if name == "optimistic" {
		c.errorf(fn.Name, diag.OptimisticNotLoading, "%s is %s, not a load: change it, it's state", exprName(fn.X), aType(xt))
		c.checkArgsLoosely(e)
		return Invalid
	}
	c.errorf(fn.Name, diag.UnknownMember, "%s has no method %s%s", typeString(xt), name, c.memberSuggest(xt, name))
	c.checkArgsLoosely(e)
	return Invalid
}

func argCount(e *syntax.CallExpr) int {
	if e.Args == nil {
		return 0
	}
	return len(e.Args.List)
}

// builtinMethodCall is a built-in type's method: [T].map, string.trim.
func (c *Checker) builtinMethodCall(e *syntax.CallExpr, fn *syntax.MemberExpr, xt Type, m *Method, want Type) Type {
	params, result := c.methodSig(m)
	if m.Variadic && argCount(e) > len(params) {
		// The last parameter, as many times as there are arguments.
		last := params[len(params)-1]
		ps := append([]*Param{}, params...)
		for len(ps) < argCount(e) {
			ps = append(ps, &Param{Name: last.Name, Type: last.Type})
		}
		params = ps
	}
	args := c.matchArgs(e, params, m.Name)
	c.trailingClosure(e, params, args)
	s := m.bind(xt)
	free := map[*TypeParam]bool{tvU: true}
	c.inferArgs(args, params, s, free, c.triedWant(e, want), result, m.Name)
	c.typeDefaults(args, params)
	if t := s[tvT]; m.Needs == "Comparable" && t != nil {
		c.whyNotComparable(t) // its compare's signature read
	}
	if u := s[tvU]; m.Needs == "ComparableU" && u != nil {
		c.whyNotComparable(u)
	}
	if why := m.requires(s); why != "" {
		if !strings.HasPrefix(why, "(") {
			why = " " + why // sorted() orders …, contains compares …
		}
		c.errorf(fn.Name, diag.ConstraintNotMet, "%s%s", m.Name, why)
	}
	bound := map[string]Type{}
	for tp, t := range s {
		bound[tp.Name] = t
	}
	if result != nil && mentionsFree(result, free, s) {
		c.errorf(e, diag.CannotInferType, "can't tell what %s returns", m.Name)
	}
	res := s.Apply(result)
	c.info.Calls[e] = &Call{Kind: CallBuiltinMethod, Method: m, Recv: fn.X, RecvType: xt, Args: args, Params: params, Result: res, Bound: bound}
	c.info.Selections[fn] = &Selection{Kind: SelBuiltinMethod, Name: m.Name, Recv: xt}
	if m.Inout {
		c.mutates(fn.X, m.Name)
	}
	throws := m.Throws || m.Rethrows && c.closuresThrow(args, params)
	c.info.Calls[e].Throws = throws
	c.callEffects(e, m.Name, m.Async, throws)
	if m.Recv == "loading" && m.Name == "optimistic" {
		c.optimisticCall(e, fn, args)
	}
	return res
}

// overload picks the one of a built-in method's overloads of the same
// arity (g.fill(p, .red), g.fill(p, LinearGradient(…))) by the first
// parameter whose type tells them apart: the argument's type, checked
// with nothing expected (an implicit member is an enum's or a color's).
func (c *Checker) overload(e *syntax.CallExpr, ms []*Method) *Method {
	p0, _ := c.methodSig(ms[0])
	at := -1
	for i := range p0 {
		for _, m := range ms[1:] {
			pi, _ := c.methodSig(m)
			if i < len(pi) && !Identical(pi[i].Type, p0[i].Type) && at < 0 {
				at = i
			}
		}
	}
	if at < 0 || e.Args == nil || at >= len(e.Args.List) || e.Args.List[at].Label != nil {
		return ms[0]
	}
	arg := e.Args.List[at].Value
	if _, ok := unparen(arg).(*syntax.ImplicitMember); ok {
		for _, m := range ms {
			ps, _ := c.methodSig(m)
			if ps[at].Type == Color || AsNamed(ps[at].Type, EnumType) != nil {
				return m
			}
		}
		return ms[0]
	}
	t := c.expr(arg, nil)
	for _, m := range ms {
		ps, _ := c.methodSig(m)
		if Identical(Unopt(ps[at].Type), Unopt(t)) {
			return m
		}
	}
	return ms[0]
}

// typeDefaults gives the defaults a call leaves to its parameters their
// parameters' types (a signature's literals have none of their own).
func (c *Checker) typeDefaults(args []syntax.Expr, params []*Param) {
	for i, a := range args {
		if a == nil && i < len(params) && params[i].Default != nil && c.info.Types[params[i].Default] == nil {
			c.info.Types[params[i].Default] = params[i].Type
		}
	}
}

// mutReceiver checks that x, the receiver of mut fn name's call, is a
// place: a var, state, or a field, an element or a map's entry of one.
func (c *Checker) mutReceiver(x syntax.Expr, name string) {
	what := "call " + name + " on"
	if c.ctx != nil {
		switch c.ctx.Kind {
		case CtxBinding, CtxGlobal, CtxBuilder, CtxComponent, CtxStore, CtxApp:
			c.errorf(x, diag.WriteInBinding, "can't %s %s here: what the UI follows is computed, not changed; change state in a handler (on press { … }), a fn, a task or an effect", what, exprName(x))
			return
		}
	}
	savedWhole, savedMut := c.lvalueWhole, c.mutName
	c.lvalueWhole, c.mutName = nil, name
	c.checkLvalue(x, what)
	c.lvalueWhole, c.mutName = savedWhole, savedMut
}

// mutates checks that x, which a method changes, can be changed.
func (c *Checker) mutates(x syntax.Expr, what string) {
	c.lvalue(x, "call "+what+" on")
}

// ---------------------------------------------------------------------
// Types called: structs, stores, components, cases

func (c *Checker) typeCall(e *syntax.CallExpr, tn *TypeName, want Type) Type {
	c.resolveType(tn)
	switch tn.Kind {
	case StructType:
		return c.structCall(e, tn, want)
	case StoreType:
		return c.storeCall(e, tn, want)
	case WorkerType:
		return c.workerNew(e, tn)
	case ComponentType:
		return c.componentCall(e, tn)
	case InterfaceType:
		if tn == c.errT {
			ps := []*Param{{Name: "message", Type: String}}
			args := c.matchArgs(e, ps, "Error")
			if args[0] != nil {
				c.assign(args[0], String, "the message")
			}
			c.info.Calls[e] = &Call{Kind: CallBuiltin, Builtin: errorBuiltin, Args: args, Params: ps, Result: c.errT.Type}
			return c.errT.Type
		}
		if tn.Native {
			if w := c.workerOf(); w != nil {
				c.errorf(e.Fun, diag.WorkerMisuse, "worker %s can't make %s: a native interface's object is the platform's, on the main thread", w.name, tn.name)
			}
			if argCount(e) > 0 || e.Block != nil {
				c.errorf(e, diag.ArgumentCount, "%s() takes nothing: its platforms' code makes it", tn.name)
			}
			c.info.Calls[e] = &Call{Kind: CallNativeNew, Type: tn.Type.(*Named), Result: tn.Type}
			c.callEffects(e, tn.name, false, true)
			return tn.Type
		}
		c.errorf(e.Fun, diag.NotCallable, "%s is an interface: make a struct or an enum that has it", tn.name)
	case EnumType:
		c.errorf(e.Fun, diag.NotCallable, "%s is an enum: make one of its cases, %s.name", tn.name, tn.name)
	case ExternType:
		c.errorf(e.Fun, diag.NotCallable, "%s is an extern type: C makes one, and an extern fn returns it", tn.name)
	case BasicType:
		switch tn.Type {
		case Float, Int, I8, I16, I32, U8, U16, U32, U64:
			id := BInt
			if tn.Type == Float {
				id = BFloat
			}
			return c.builtinCall(e, &Builtin{object: object{name: tn.name}, ID: id}, want)
		case Bytes:
			// bytes(): none; bytes([0x04, 0xa1]): those (each 0 to 255,
			// as a byte); s.bytes() makes them from text.
			if e.Args != nil && len(e.Args.List) > 0 {
				return c.builtinCall(e, &Builtin{object: object{name: tn.name}, ID: BBytesFrom}, want)
			}
			return c.builtinCall(e, &Builtin{object: object{name: tn.name}, ID: BMath, RT: "bytes.empty", Sig: &FuncType{Result: Bytes}}, want)
		case Char:
			return c.builtinCall(e, &Builtin{object: object{name: tn.name}, ID: BChar}, want)
		case Ref:
			return c.builtinCall(e, &Builtin{object: object{name: tn.name}, ID: BRefNew}, want)
		case Path:
			return c.builtinCall(e, &Builtin{object: object{name: tn.name}, ID: BPathNew}, want)
		}
		c.errorf(e.Fun, diag.NotCallable, "%s is a type, not a function", tn.name)
	default:
		c.errorf(e.Fun, diag.NotCallable, "%s is a type, not a function", tn.name)
	}
	c.checkArgsLoosely(e)
	return Invalid
}

var errorBuiltin = &Builtin{object: object{name: "Error"}, ID: BError}

// fieldParams are a struct's fields as a constructor's parameters.
func fieldParams(tn *TypeName) []*Param {
	ps := make([]*Param, len(tn.Fields))
	for i, f := range tn.Fields {
		ps[i] = &Param{Name: f.Name, Type: f.Type, Default: f.Default}
	}
	return ps
}

// explicitTypeArgs binds the type arguments a call writes out, f<T>(…)
// or Stack<int>(), to its type parameters, which inference then leaves.
func (c *Checker) explicitTypeArgs(e *syntax.CallExpr, what string, tps []*TypeParam, s Subst, free map[*TypeParam]bool) {
	if len(e.TypeArgs) == 0 {
		return
	}
	if len(e.TypeArgs) != len(tps) {
		c.errorf(e, diag.TypeArguments, "%s takes %s", what, plural(len(tps), "type argument", "type arguments"))
	}
	for i, ta := range e.TypeArgs {
		t := c.typeExpr(ta)
		if i < len(tps) {
			s[tps[i]] = t
			delete(free, tps[i])
		}
	}
}

func (c *Checker) structCall(e *syntax.CallExpr, tn *TypeName, want Type) Type {
	if e.Block != nil {
		c.errorf(e.Block, diag.UnexpectedBlock, "%s is a struct: its fields go in parentheses, %s(name: value)", tn.name, tn.name)
	}
	params := fieldParams(tn)
	for _, f := range tn.Fields {
		// Its private fields are its file's: elsewhere it's made without
		// them, so each needs a default.
		if f.Private && f.Default == nil && !c.sameFile(f.Pos, e) {
			c.errorf(e.Fun, diag.NotPub, "%s is made in %s only: its field %s is private there, with no default; make one with what it gives (its static fns)", tn.name, c.fileName(f.Pos), f.Name)
			c.checkArgsLoosely(e)
			return tn.Type
		}
	}
	args := c.matchArgs(e, params, tn.name)
	for i, f := range tn.Fields {
		if args[i] != nil {
			c.hiddenField(args[i], tn.name, f)
		}
	}
	s := Subst{}
	free := map[*TypeParam]bool{}
	for _, tp := range tn.TypeParams {
		free[tp] = true
	}
	c.explicitTypeArgs(e, tn.name, tn.TypeParams, s, free)
	c.inferArgs(args, params, s, free, want, tn.Type, tn.name)
	n := &Named{Obj: tn}
	for _, tp := range tn.TypeParams {
		t := s[tp]
		if t == nil {
			c.errorf(e, diag.CannotInferType, "can't tell %s's %s: pass its fields, or say what's expected (let x: %s<…> = …)", tn.name, tp.Name, tn.name)
			t = Invalid
		} else {
			c.checkConstraints(e, tn.name, tp, t)
		}
		n.Args = append(n.Args, t)
	}
	var t Type = n
	if len(n.Args) == 0 {
		t = tn.Type
	}
	c.info.Calls[e] = &Call{Kind: CallStruct, Type: n, Args: args, Params: params, Result: t}
	return t
}

func (c *Checker) caseCall(e *syntax.CallExpr, tn *TypeName, k *Case, want Type) Type {
	if len(k.Params) == 0 {
		c.errorf(e, diag.ArgumentCount, "case %s of %s has no payload: write it without parentheses", k.Name, tn.name)
		return tn.Type
	}
	params := make([]*Param, len(k.Params))
	for i, p := range k.Params {
		name := k.Labels[i]
		if name == "" {
			name = "_" + itoa(i) // payloadIndex's
		}
		params[i] = &Param{Name: name, Type: p}
		// A labeled payload with a zero value may be left out: it's
		// that (.button(title: "Copy", run: copy)).
		if k.Labels[i] != "" && HasZero(p) {
			params[i].Var = &Var{Kind: optionalParam}
		}
	}
	args := c.matchArgs(e, params, tn.name+"."+k.Name)
	s := Subst{}
	free := map[*TypeParam]bool{}
	for _, tp := range tn.TypeParams {
		free[tp] = true
	}
	c.inferArgs(args, params, s, free, want, tn.Type, tn.name+"."+k.Name)
	n := &Named{Obj: tn}
	for _, tp := range tn.TypeParams {
		t := s[tp]
		if t == nil {
			c.errorf(e, diag.CannotInferType, "can't tell %s's %s: say what's expected (let x: %s<…> = …)", tn.name, tp.Name, tn.name)
			t = Invalid
		} else {
			c.checkConstraints(e, tn.name, tp, t)
		}
		n.Args = append(n.Args, t)
	}
	var t Type = n
	if len(n.Args) == 0 {
		t = tn.Type
	}
	c.info.Calls[e] = &Call{Kind: CallCase, Type: n, Case: k, Args: args, Params: params, Result: t}
	return t
}

func (c *Checker) storeCall(e *syntax.CallExpr, tn *TypeName, want Type) Type {
	if c.ctx != nil && tn.Comp != nil && tn.Comp.Ctx != nil {
		// What its body's cleanups write is written where it's made, when
		// that owner goes: an effect making one writes it before its next
		// run (rfcs/0016).
		c.ctx.writeCalls = append(c.ctx.writeCalls, callUse{at: e, ctx: tn.Comp.Ctx})
	}
	if e.Block != nil {
		c.errorf(e.Block, diag.UnexpectedBlock, "%s is a store: its parameters go in parentheses", tn.name)
	}
	var params []*Param
	for _, m := range tn.Comp.Params {
		params = append(params, &Param{Name: m.name, Type: m.Type, Default: m.Default})
	}
	args := c.matchArgs(e, params, tn.name)
	if len(tn.TypeParams) == 0 {
		c.explicitTypeArgs(e, tn.name, nil, Subst{}, nil)
		for i, a := range args {
			if a != nil {
				c.bindingArg(a, params[i].Type, argDesc(params[i])+" of "+tn.name)
			}
		}
		c.info.Calls[e] = &Call{Kind: CallStore, Type: tn.Type.(*Named), Args: args, Params: params, Result: tn.Type}
		return tn.Type
	}
	// A generic store: its type arguments from its arguments (each a
	// binding, as the store follows them) or from what's expected.
	s := Subst{}
	free := map[*TypeParam]bool{}
	for _, tp := range tn.TypeParams {
		free[tp] = true
	}
	c.explicitTypeArgs(e, tn.name, tn.TypeParams, s, free)
	saved := c.bindingArgs
	c.bindingArgs = true
	c.inferArgs(args, params, s, free, want, tn.Type, tn.name)
	c.bindingArgs = saved
	n := &Named{Obj: tn}
	for _, tp := range tn.TypeParams {
		t := s[tp]
		if t == nil {
			c.errorf(e, diag.CannotInferType, "can't tell %s's %s: pass its parameters, or say what's expected (let x: %s<…> = …)", tn.name, tp.Name, tn.name)
			t = Invalid
		} else {
			c.checkConstraints(e, tn.name, tp, t)
		}
		n.Args = append(n.Args, t)
	}
	inst := make([]*Param, len(params))
	for i, p := range params {
		inst[i] = &Param{Name: p.Name, Type: s.Apply(p.Type), Default: p.Default}
	}
	c.info.Calls[e] = &Call{Kind: CallStore, Type: n, Args: args, Params: inst, Result: n}
	c.genericCalls = append(c.genericCalls, genericCall{e: e, store: tn, args: n.Args, encl: c.enclosingFunc(), own: c.enclosingParams()})
	c.noteInstance(e, tn.name, tn.TypeParams, n.Args)
	return n
}

// bindingArg checks an argument the callee follows (an accessor): it's
// a binding of its own.
func (c *Checker) bindingArg(a syntax.Expr, want Type, what string) {
	saved := c.ctx
	c.enter(CtxBinding, a, "binding")
	c.assign(a, want, what)
	c.leave(saved)
}

// ---------------------------------------------------------------------
// Lambdas and spawn

func (c *Checker) lambda(e *syntax.LambdaExpr, want Type) Type {
	wf, _ := Unopt(want).(*FuncType)
	loose := c.loose
	c.loose = nil
	saved := c.ctx
	savedTry, savedAwait, savedHandled, savedLoops, savedView := c.inTry, c.inAwait, c.handled, c.loops, c.view
	c.inTry, c.inAwait, c.handled, c.loops, c.view = 0, 0, 0, 0, false
	ctx := c.enter(CtxLambda, e, "closure")
	ft := &FuncType{Result: Void}
	if wf != nil && len(wf.Params) != len(e.Params) {
		c.errorf(e, diag.ArgumentCount, "this closure takes %s, where one taking %s is expected", plural(len(e.Params), "parameter", "parameters"), plural(len(wf.Params), "parameter", "parameters"))
		wf = nil
	}
	for i, p := range e.Params {
		var t Type
		switch {
		case p.Type != nil:
			t = c.typeExpr(p.Type)
		case wf != nil && !mentionsFree(wf.Params[i], c.free, nil):
			t = wf.Params[i]
		default:
			c.errorf(p.Name, diag.CannotInferType, "can't tell %s's type: write it, (%s: T) => …", p.Name.Name, p.Name.Name)
			t = Invalid
		}
		ft.Params = append(ft.Params, t)
		ctx.Params = append(ctx.Params, c.declareVar(p.Name, t, false))
	}
	marked := e.Async.IsValid()
	if wf != nil {
		if marked && !wf.Async {
			c.errorPos(e.Async, diag.TypeMismatch, "this closure is async, where %s is expected, which can't wait: remove async (and the awaits in it)", aType(wf))
		}
		// Marked async where a sync function is expected: an error, and
		// its body may still await (no second error for each await).
		ctx.Async, ctx.Throws = wf.Async || marked, wf.Throws
		ft.Async, ft.Throws = wf.Async, wf.Throws
	} else {
		ctx.Async, ctx.Throws = true, true
	}
	infer := wf == nil || mentionsFree(wf.Result, c.free, nil)
	if !infer {
		ctx.Result = wf.Result
		if loose != nil {
			loose.ctx, loose.ft = ctx, ft
			ctx.loose = loose
		}
	} else {
		ctx.inferRes = true
	}
	switch body := e.Body.(type) {
	case syntax.Expr:
		if infer {
			ft.Result = c.expr(body, nil)
			if ft.Result == NoneType {
				ft.Result = Invalid
			}
			// A return in the body (x => x ?? return 0) gives a value
			// too.
			for _, r := range ctx.retTypes {
				if j := c.join(ft.Result, r); j != nil {
					ft.Result = j
				} else {
					c.errorf(body, diag.TypeMismatch, "this closure's value is %s, and it returns %s: make them the same type", aType(ft.Result), aType(r))
					break
				}
			}
		} else if ctx.Result == Void {
			c.expr(body, nil)
		} else if ctx.loose != nil {
			ctx.loose.value(c, body, "the closure's result")
			ft.Result = ctx.Result
		} else {
			c.assign(body, ctx.Result, "the closure's result")
			ft.Result = ctx.Result
		}
	case *syntax.Block:
		c.stmts(body.Items)
		if infer {
			ft.Result = Void
			for _, r := range ctx.retTypes {
				if j := c.join(ft.Result, r); j != nil && ft.Result != Void {
					ft.Result = j
				} else {
					ft.Result = r
				}
			}
			if len(ctx.retTypes) > 0 && !c.terminates(body.Items) {
				c.errorPos(body.Rbrace, diag.MissingReturn, "this closure returns a value, but can reach its end without one")
			}
		} else {
			ft.Result = ctx.Result
			if ctx.Result != Void && !c.terminates(body.Items) {
				c.errorPos(body.Rbrace, diag.MissingReturn, "this closure must return %s: it can reach its end without a return", aType(ctx.Result))
			}
		}
	}
	if wf == nil {
		ft.Async = marked || ctx.Awaits
		ft.Throws = ctx.threw
		if fx := (&inferredFx{lambda: e, threwAt: ctx.threwAt}); ft.Throws || ft.Async && !marked {
			if !marked {
				fx.awaitAt = ctx.awaitAt
			}
			if c.inferredFx == nil {
				c.inferredFx = map[Type]*inferredFx{}
			}
			c.inferredFx[ft] = fx
		}
	}
	ctx.Result = ft.Result
	if c.lambdaThrew == nil {
		c.lambdaThrew = map[*syntax.LambdaExpr]bool{}
	}
	c.lambdaThrew[e] = ctx.threw
	c.leave(saved)
	c.inTry, c.inAwait, c.handled, c.loops, c.view = savedTry, savedAwait, savedHandled, savedLoops, savedView
	return ft
}

// closuresThrow reports whether a function a call is given may throw: a
// closure whose body does, or a function value whose type says so.
func (c *Checker) closuresThrow(args []syntax.Expr, params []*Param) bool {
	for i, a := range args {
		if a == nil || i >= len(params) {
			continue
		}
		if _, ok := params[i].Type.(*FuncType); !ok {
			continue
		}
		if lam, ok := unparen(a).(*syntax.LambdaExpr); ok {
			if c.lambdaThrew[lam] {
				return true
			}
			continue
		}
		if ft, ok := Unopt(c.info.Types[a]).(*FuncType); ok && ft.Throws {
			return true
		}
	}
	return false
}

func (c *Checker) spawn(e *syntax.SpawnExpr) Type {
	saved := c.ctx
	savedTry, savedAwait, savedHandled, savedLoops, savedView := c.inTry, c.inAwait, c.handled, c.loops, c.view
	c.inTry, c.inAwait, c.handled, c.loops, c.view = 0, 0, 0, 0, false
	if w := c.workerOf(); w != nil {
		c.errorf(e, diag.WorkerMisuse, "worker %s's fns already run on its own thread: do the work here, without spawn", w.name)
	}
	ctx := c.enter(CtxSpawn, e, "spawn")
	ctx.Throws = true
	ctx.inferRes = true
	ctx.Comp = nil
	items := e.Body.Items
	var res Type = Void
	if n := len(items); n > 0 {
		if es, ok := items[n-1].(*syntax.ExprStmt); ok && !hasReturnValue(items) {
			c.stmts(items[:n-1])
			res = c.expr(es.X, nil)
			c.info.Ctxs[es] = ctx // the block's value is its last expression's
		} else {
			c.stmts(items)
			for _, r := range ctx.retTypes {
				res = r
			}
		}
	}
	if !sendable(res) {
		c.errorf(e, diag.NotSendable, "spawn's result is %s, which can't cross threads: return values only", aType(res))
	}
	ctx.Result = res
	c.leave(saved)
	c.inTry, c.inAwait, c.handled, c.loops, c.view = savedTry, savedAwait, savedHandled, savedLoops, savedView
	c.spawns = append(c.spawns, ctx)
	return &Task{res}
}

func hasReturnValue(items []syntax.Item) bool {
	found := false
	for _, it := range items {
		syntax.Inspect(it, func(n syntax.Node) bool {
			switch n := n.(type) {
			case *syntax.LambdaExpr, *syntax.SpawnExpr:
				return false
			case *syntax.ReturnStmt:
				if n.Value != nil {
					found = true
				}
			}
			return !found
		})
	}
	return found
}

// sendable reports whether values of t can cross to another thread:
// values only, no state, stores, views, tasks or closures.
// Sendable reports whether a value of t can cross threads (spawn's
// captures and result, the file's values a spawn's worker makes).
func Sendable(t Type) bool { return sendable(t) }

func sendable(t Type) bool {
	return sendable1(t, map[*TypeName]bool{})
}

func sendable1(t Type, seen map[*TypeName]bool) bool {
	switch x := t.(type) {
	case *Basic:
		// A C pointer is the main thread's: on the web a spawn's worker
		// has memory of its own, where it points at nothing.
		return x != View && x != TestApp && x != Pointer
	case *Array:
		return sendable1(x.Elem, seen)
	case *Map:
		return sendable1(x.Elem, seen)
	case *Set:
		return sendable1(x.Elem, seen)
	case *Optional:
		return sendable1(x.Elem, seen)
	case *Tuple:
		for _, e := range x.Elems {
			if !sendable1(e, seen) {
				return false
			}
		}
		return true
	case *FuncType, *Task, *Stream, *Seq, *Tree:
		return false
	case *TypeParam:
		return true
	case *Named:
		if seen[x.Obj] {
			return true
		}
		seen[x.Obj] = true
		s := x.Subst()
		switch x.Obj.Kind {
		case StructType:
			for _, f := range x.Obj.Fields {
				if !sendable1(s.Apply(f.Type), seen) {
					return false
				}
			}
			return true
		case EnumType:
			if x.Obj == loadingT {
				return false
			}
			for _, k := range x.Obj.Cases {
				for _, p := range k.Params {
					if !sendable1(s.Apply(p), seen) {
						return false
					}
				}
			}
			return true
		case InterfaceType:
			// A native interface's object is the platform's, which a
			// spawn's worker hasn't on the web: it stays on the main thread.
			return !x.Obj.Native
		}
		return false
	}
	return true
}

func itoa(i int) string {
	const digits = "0123456789"
	if i < 10 && i >= 0 {
		return digits[i : i+1]
	}
	neg := i < 0
	if neg {
		i = -i
	}
	var b []byte
	for i > 0 {
		b = append([]byte{digits[i%10]}, b...)
		i /= 10
	}
	if neg {
		b = append([]byte{'-'}, b...)
	}
	return string(b)
}
