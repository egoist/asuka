package check

import (
	"strings"

	"github.com/egoist/asuka/lang/diag"
	"github.com/egoist/asuka/lang/syntax"
)

// loadingT is Loading<T>: what load keeps, matched like an enum.
var loadingT = func() *TypeName {
	t := &TypeName{object: object{name: "Loading"}, Kind: EnumType, resolved: true}
	tp := &TypeParam{Name: "T", id: -1}
	t.TypeParams = []*TypeParam{tp}
	t.Type = &Named{Obj: t, Args: []Type{tp}}
	t.Cases = []*Case{
		{Name: "pending", Index: 0, Owner: t},
		{Name: "ready", Index: 1, Params: []Type{tp}, Labels: []string{""}, Owner: t},
		{Name: "failed", Index: 2, Params: []Type{nil}, Labels: []string{""}, Owner: t},
	}
	t.caseIndex = map[string]int{"pending": 0, "ready": 1, "failed": 2}
	t.Methods = map[string]*Function{}
	t.Consts = map[string]*Global{}
	return t
}()

// IsLoading reports Loading<T>.
func IsLoading(t Type) bool {
	n, ok := t.(*Named)
	return ok && n.Obj == loadingT
}

// semanticColors are ops' SemanticColor values, by their Asuka names
// (ops/schema.go's SemanticColor, lower camel case).
var semanticColors = []string{"none", "label", "secondaryLabel", "tertiaryLabel", "background", "secondaryBackground", "groupedBackground", "separator", "accent", "red", "orange", "yellow", "green", "blue", "purple", "pink", "gray", "clear", "secondaryGroupedBackground", "onAccent"}

// SemanticColor is the ops.Color of a semantic color's name; .default
// is 0, the platform's own (a prop left unset).
func SemanticColor(name string) (int64, bool) {
	if name == "default" {
		return 0, true
	}
	for i, n := range semanticColors {
		if n == name && i > 0 {
			return 1<<32 | int64(i), true
		}
	}
	return 0, false
}

func semanticColorNames() []string { return semanticColors[1:] }

// builtinCall checks a call of a function the compiler provides.
func (c *Checker) builtinCall(e *syntax.CallExpr, b *Builtin, want Type) Type {
	call := &Call{Kind: CallBuiltin, Builtin: b}
	c.info.Calls[e] = call
	var list []*syntax.Arg
	if e.Args != nil {
		list = e.Args.List
	}
	positional := func(n int, what string) []syntax.Expr {
		var out []syntax.Expr
		for _, a := range list {
			if a.Label != nil {
				c.errorf(a.Label, diag.ArgumentLabel, "%s's arguments have no names", b.name)
			}
			out = append(out, a.Value)
		}
		if n >= 0 && len(out) != n {
			c.errorf(e, diag.ArgumentCount, "%s takes %s", b.name, what)
		}
		return out
	}
	if e.Block != nil && b.ID != BMount && b.ID != BWithTimeout && b.ID != BAnimate && b.ID != BRuntime {
		c.errorf(e.Block, diag.UnexpectedBlock, "%s takes no block", b.name)
	}
	switch b.ID {
	case BPeek:
		args := positional(1, "one value: peek(count)")
		call.Args = args
		call.Result = Invalid
		if len(args) == 1 {
			call.Result = c.peek(e, args[0], want)
		}
	case BPrint:
		args := positional(1, "one value: print(\"…\")")
		for _, a := range args {
			c.interpolated(a, c.expr(a, nil))
		}
		call.Args = args
		call.Result = Void
	case BExpect:
		args := positional(-1, "a condition: expect(count == 2)")
		if len(args) < 1 || len(args) > 2 {
			c.errorf(e, diag.ArgumentCount, "expect takes a condition, and maybe a message: expect(count == 2, \"…\")")
		}
		if len(args) > 0 {
			c.assign(args[0], Bool, "expect's condition")
		}
		if len(args) > 1 {
			c.assign(args[1], String, "expect's message")
		}
		call.Args = args
		call.Result = Void
	case BPanic:
		args := positional(1, "a message")
		if len(args) > 0 {
			c.assign(args[0], String, "panic's message")
		}
		call.Args = args
		call.Result = Never
	case BSleep:
		args := positional(1, "a duration: sleep(300ms)")
		if len(args) > 0 {
			c.assign(args[0], Duration, "sleep's duration")
		}
		call.Args = args
		call.Result = Void
		c.callEffects(e, "sleep", true, false)
	case BAll, BRace:
		call.Args = positional(-1, "")
		call.Result = c.allRace(e, b, call.Args)
		c.callEffects(e, b.name, true, false)
	case BMount:
		// mount(view, provide: store, platform: .android, at: "/inbox"):
		// the store provided to the view, the platform it runs on (asuka
		// test's unless it says), where its Router starts.
		ps := []*Param{{Name: "view", Type: View}, {Name: "provide", Type: nil, Default: zeroLit},
			{Name: "platform", Type: nil, Default: zeroLit}, {Name: "at", Type: String, Default: zeroLit}}
		args := c.matchArgs(e, ps, "mount")
		if args[2] != nil {
			pt := c.prelude.Scope.LookupLocal("Platform").(*TypeName)
			c.resolveType(pt)
			c.assign(args[2], pt.Type, "mount's platform")
		}
		if args[3] != nil {
			c.assign(args[3], String, "where mount's Router starts")
		}
		if args[0] != nil {
			c.builderExpr(args[0])
		}
		if args[1] != nil {
			pt := c.expr(args[1], nil)
			if n, ok := pt.(*Named); !ok || n.Obj.Kind != StoreType {
				if !IsInvalid(pt) {
					c.errorf(args[1], diag.TypeMismatch, "provide takes a store: this is %s", aType(pt))
				}
			}
		}
		call.Args = args
		call.Result = TestApp
	case BIdle:
		call.Args = positional(0, "nothing")
		call.Result = Void
		c.callEffects(e, "idle", true, false)
	case BNever:
		call.Args = positional(0, "nothing")
		call.Result = Void
		c.callEffects(e, "never", true, false)
	case BEmbedText, BEmbedBytes, BEmbedJSON, BEmbedTexts, BEmbedFiles:
		c.embedCall(e, b, call, want)
	case BNavigate, BReplace:
		args := positional(1, "an address: "+b.name+"(\"/inbox\"), or a routed screen's: "+b.name+"(Thread(id: 7))")
		if len(args) > 0 {
			c.hrefArg(args[0], "the address "+b.name+" goes to")
		}
		call.Args = args
		call.Result = Void
	case BPush, BPresent:
		args := positional(1, "a view: "+b.name+"(Screen())")
		if len(args) > 0 {
			c.builderExpr(args[0])
		}
		call.Args = args
		call.Result = Void
	case BPop, BDismiss:
		call.Args = positional(0, "nothing")
		call.Result = Void
	case BMin, BMax:
		args := positional(2, "two numbers")
		call.Args = args
		if len(args) == 2 {
			call.Result = c.sameNumbers(e, args, want)
		} else {
			call.Result = Invalid
		}
	case BAbs:
		args := positional(1, "a number")
		call.Args = args
		if len(args) == 1 {
			t := c.expr(args[0], numOnly(Unopt(want)))
			if !IsNumeric(t) && !IsInvalid(t) {
				c.errorf(args[0], diag.TypeMismatch, "abs takes a number, not %s", aType(t))
			}
			call.Result = t
		} else {
			call.Result = Invalid
		}
	case BFloat:
		args := positional(1, "an int: float(n)")
		call.Args = args
		if len(args) == 1 {
			t := c.expr(args[0], nil)
			if t != Int && t != Float && !IsInvalid(t) {
				c.errorf(args[0], diag.TypeMismatch, "float(…) takes an int, not %s", aType(t))
			}
		}
		call.Result = Float
	case BInt:
		args := positional(1, "a float: int(x)")
		call.Args = args
		if len(args) == 1 {
			t := c.expr(args[0], nil)
			if t != Int && t != Float && !IsInvalid(t) {
				c.errorf(args[0], diag.TypeMismatch, "int(…) takes a float, not %s", aType(t))
			}
		}
		call.Result = Int
	case BJSONDecode:
		ps := []*Param{{Name: "text", Type: String}}
		args := c.matchArgs(e, ps, "json.decode")
		if args[0] != nil {
			c.assign(args[0], String, "the JSON text")
		}
		call.Args, call.Params = args, ps
		t := want
		if len(e.TypeArgs) == 1 {
			t = c.typeExpr(e.TypeArgs[0]) // json.decode<T>(text)
		} else if len(e.TypeArgs) > 1 {
			c.errorf(e, diag.TypeArguments, "json.decode takes one type: json.decode<T>(text)")
		}
		if t == nil {
			c.errorf(e, diag.CannotInferType, "json.decode needs to know what it reads: json.decode<T>(text), or say what's expected, let x: T = try json.decode(text)")
			t = Invalid
		} else if why := c.jsonable(t); why != "" {
			c.errorf(e, diag.NotJSON, "json can't read %s: %s", aType(t), why)
		}
		call.Result = t
		c.callEffects(e, "json.decode", false, true)
	case BJSONEncode:
		ps := []*Param{{Name: "value", Type: nil}, {Name: "indent", Type: Int, Default: zeroLit}}
		args := c.matchArgs(e, ps, "json.encode")
		if args[0] != nil {
			t := c.valueType(c.expr(args[0], nil), args[0])
			ps[0].Type = t
			if why := c.jsonable(t); why != "" && !IsInvalid(t) {
				c.errorf(args[0], diag.NotJSON, "json can't write %s: %s", aType(t), why)
			}
		}
		if args[1] != nil {
			c.assign(args[1], Int, "the indent")
		}
		call.Args, call.Params = args, ps
		call.Result = String
	case BMath:
		ft := b.Sig
		args := positional(len(ft.Params), plural(len(ft.Params), "argument", "arguments"))
		for i, a := range args {
			if i < len(ft.Params) {
				c.assign(a, ft.Params[i], "argument "+itoa(i+1)+" of "+b.name)
			}
		}
		call.Args = args
		call.Result = ft.Result
	case BRange:
		call.Result = &Array{Int}
	case BWithTimeout:
		args := positional(1, "a duration and a block: withTimeout(5s) { … }")
		if len(args) > 0 {
			c.assign(args[0], Duration, "the timeout")
		}
		if e.Block == nil {
			c.errorf(e, diag.ArgumentCount, "withTimeout takes a block: withTimeout(5s) { … }")
		} else {
			lam := &syntax.LambdaExpr{Span: e.Block.Span, Body: e.Block}
			c.info.Trailing[e] = lam
			// Its value is the block's: what's expected of it, else what
			// the block returns.
			var wf *FuncType
			if want != nil && !IsInvalid(want) && want != Void {
				wf = &FuncType{Result: want, Async: true, Throws: true}
			}
			res := Type(Void)
			if t, ok := c.expr(lam, wf).(*FuncType); ok {
				t.Async, t.Throws = true, true // it runs as a task
				if t.Result != nil {
					res = t.Result
				}
			}
			call.Result = res
			args = append(args, lam)
		}
		call.Args = args
		if call.Result == nil {
			call.Result = Void
		}
		c.callEffects(e, "withTimeout", true, true)
	case BLive:
		args := positional(2, "a value and a stream: live(initial, () => changes())")
		var elem Type
		if len(e.TypeArgs) == 1 {
			elem = c.typeExpr(e.TypeArgs[0])
		} else if ft, ok := Unopt(want).(*FuncType); ok && len(ft.Params) == 0 && ft.Result != nil {
			elem = ft.Result
		}
		if len(args) == 2 {
			if elem == nil {
				elem = c.valueType(c.expr(args[0], nil), args[0])
			} else {
				c.assign(args[0], elem, "the initial value")
			}
			c.assign(args[1], &FuncType{Result: &Stream{elem}}, "the stream it follows")
		}
		if elem == nil {
			elem = Invalid
		}
		call.Args = args
		call.Result = &FuncType{Result: elem}
	case BEnvironment:
		positional(0, "nothing: environment()")
		call.Result = c.prelude.Scope.Lookup("Environment").(*TypeName).Type
	case BSet:
		// Set(xs): xs's distinct elements; Set<T>(): none.
		args := positional(-1, "")
		var elem Type
		if len(e.TypeArgs) == 1 {
			elem = c.typeExpr(e.TypeArgs[0])
		} else if len(e.TypeArgs) > 1 {
			c.errorf(e, diag.TypeArguments, "Set takes one type: Set<T>")
		} else if st, ok := Unopt(want).(*Set); ok {
			elem = st.Elem
		}
		switch len(args) {
		case 0:
			if elem == nil {
				c.errorf(e, diag.CannotInferType, "can't tell the set's elements: Set<T>(), or say what's expected (let s: Set<T> = [])")
				elem = Invalid
			}
		case 1:
			if elem != nil {
				c.assign(args[0], &Array{elem}, "Set(…)'s argument")
			} else if a, ok := c.expr(args[0], nil).(*Array); ok {
				elem = a.Elem
			} else {
				if t := c.info.Types[args[0]]; !IsInvalid(t) {
					c.errorf(args[0], diag.TypeMismatch, "Set(…) takes an array of its elements, not %s", aType(t))
				}
				elem = Invalid
			}
		default:
			c.errorf(e, diag.ArgumentCount, "Set takes an array of its elements: Set([a, b]), or Set<T>() for none")
			for _, a := range args {
				c.expr(a, nil)
			}
			elem = Invalid
		}
		call.Args = args
		call.Result = c.setOf(elem, e, false)
	case BStream:
		args := positional(1, "what sends its values: Stream<T>(send => { … })")
		var elem Type
		if len(e.TypeArgs) == 1 {
			elem = c.typeExpr(e.TypeArgs[0])
		} else if st, ok := Unopt(want).(*Stream); ok {
			elem = st.Elem
		}
		if elem == nil {
			c.errorf(e, diag.CannotInferType, "can't tell what the stream sends: Stream<T>(…), or say what's expected")
			elem = Invalid
		}
		prod := &FuncType{Params: []Type{&FuncType{Params: []Type{elem}, Result: Void}}, Result: Void, Async: true, Throws: true}
		if len(args) == 1 {
			c.assign(args[0], prod, "what sends the stream's values")
		}
		call.Args = args
		call.Params = []*Param{{Name: "produce", Type: prod}}
		call.Result = &Stream{elem}
	case BRuntime:
		c.runtimeCall(e, call, b.Fn)
	case BAnimate:
		c.animateCall(e, call)
	case BShader:
		c.shaderProgramCall(e, call)
	case BRefNew:
		positional(0, "nothing: Ref(), then ref: it on an element")
		call.Result = Ref
	case BPathNew:
		positional(0, "nothing: Path(), then its methods: Path().move(0, 0).line(10, 10)")
		call.Result = Path
	default:
		c.errorf(e, diag.InternalError, "unknown builtin %s", b.name)
		call.Result = Invalid
	}
	return call.Result
}

var zeroLit = &syntax.BasicLit{Kind: syntax.INT, Value: "0"}

func (c *Checker) sameNumbers(e *syntax.CallExpr, args []syntax.Expr, want Type) Type {
	var xt, yt Type
	switch {
	case literalInt(args[0]) && !literalInt(args[1]):
		yt = c.expr(args[1], numOnly(Unopt(want)))
		xt = c.expr(args[0], numOnly(yt))
	default:
		xt = c.expr(args[0], numOnly(Unopt(want)))
		yt = c.expr(args[1], numOnly(xt))
	}
	if IsInvalid(xt) || IsInvalid(yt) {
		return Invalid
	}
	if !IsNumeric(xt) || !Identical(xt, yt) {
		c.errorf(e, diag.BadOperand, "this takes two ints or two floats, not %s and %s", aType(xt), aType(yt))
		return Invalid
	}
	return xt
}

// allRace checks all(…) and race(…): calls of async functions, or
// tasks, started at once.
func (c *Checker) allRace(e *syntax.CallExpr, b *Builtin, args []syntax.Expr) Type {
	if len(args) == 0 {
		c.errorf(e, diag.NotAsyncCall, "%s takes the calls it runs at once: %s(f(), g())", b.name, b.name)
		return Invalid
	}
	var types []Type
	for _, a := range args {
		c.inAwait++
		t := c.expr(a, nil)
		c.inAwait--
		if tk, ok := t.(*Task); ok {
			t = tk.Elem
		} else if arr, ok := t.(*Array); ok && len(args) == 1 {
			if tk, ok := arr.Elem.(*Task); ok {
				if b.ID == BRace {
					return tk.Elem
				}
				return &Array{tk.Elem}
			}
		} else if ce, ok := unparen(a).(*syntax.CallExpr); !ok || c.info.Calls[ce] == nil || !c.isAsyncCall(c.info.Calls[ce]) {
			if !IsInvalid(t) {
				c.errorf(a, diag.NotAsyncCall, "%s runs async calls (or tasks) at once, and this isn't one", b.name)
			}
		}
		types = append(types, t)
	}
	if b.ID == BRace {
		t := types[0]
		for i, u := range types[1:] {
			if j := c.join(t, u); j != nil {
				t = j
			} else {
				c.errorf(args[i+1], diag.TypeMismatch, "race's calls must have one type: this is %s, the first %s", aType(u), aType(t))
			}
		}
		return t
	}
	if len(types) == 1 {
		return types[0]
	}
	// all of calls that return nothing returns nothing.
	allVoid := true
	for _, t := range types {
		if t != Void {
			allVoid = false
		}
	}
	if allVoid {
		return Void
	}
	return &Tuple{types}
}

func unparen(e syntax.Expr) syntax.Expr {
	for {
		p, ok := e.(*syntax.ParenExpr)
		if !ok {
			return e
		}
		e = p.X
	}
}

// isAsyncCall reports a call of an async function.
func (c *Checker) isAsyncCall(call *Call) bool {
	switch call.Kind {
	case CallFunc, CallMethod:
		return call.Func.Sig.Async
	case CallClosure:
		ft, ok := call.RecvType.(*FuncType)
		return ok && ft.Async
	case CallIface:
		return false
	case CallWorker:
		return true
	case CallBuiltinMethod:
		return call.Method.Async
	case CallBuiltin:
		switch call.Builtin.ID {
		case BSleep, BAll, BRace, BIdle, BWithTimeout, BNever:
			return true
		case BRuntime:
			return call.Builtin.Fn.Async
		}
	}
	return false
}

// builderExpr checks a view that's built later, where it's placed: a
// closure of its own (a builder).
func (c *Checker) builderExpr(x syntax.Expr) {
	saved := c.ctx
	savedView := c.view
	c.enter(CtxBuilder, x, "builder")
	c.view = true
	t := c.expr(x, View)
	if !IsInvalid(t) && t != View {
		c.errorf(x, diag.NotAView, "this is %s, not a view", aType(t))
	}
	c.view = savedView
	c.leave(saved)
}

// load checks load f(args): an async call whose result a component
// keeps.
func (c *Checker) load(e *syntax.UnaryExpr) Type {
	if c.ctx == nil || c.ctx.Kind != CtxBinding || !c.loadOK {
		c.errorf(e, diag.LoadOutsideComponent, "load is a component's or a store's: let x = load f(…)")
	}
	if group, ok := e.X.(*syntax.TupleExpr); ok {
		return c.loadGroup(group)
	}
	call, ok := unparen(e.X).(*syntax.CallExpr)
	if !ok {
		c.errorf(e.X, diag.NotAsyncCall, "load takes an async call: load f(…)")
		c.expr(e.X, nil)
		return Invalid
	}
	c.inAwait++
	c.inTry++
	t := c.expr(call, nil)
	c.inTry--
	c.inAwait--
	if ci := c.info.Calls[call]; ci != nil && !c.isAsyncCall(ci) {
		c.errorf(call, diag.NotAsyncCall, "load takes an async call, and this isn't one")
	}
	c.noteLoad(call)
	if IsInvalid(t) {
		return Invalid
	}
	return &Named{Obj: loadingT, Args: []Type{t}}
}

// loadGroup checks load (c₁, c₂, …) (rfcs/0017): each element an async
// call, the load's a Loading of the tuple of their results.
func (c *Checker) loadGroup(g *syntax.TupleExpr) Type {
	var elems []Type
	bad := false
	for _, x := range g.Elems {
		call, ok := unparen(x).(*syntax.CallExpr)
		if !ok {
			c.errorf(x, diag.GroupNotCalls, "load (a(), b()) groups calls: load (%s) has %s", groupText(g), exprName(x))
			c.expr(x, nil)
			bad = true
			continue
		}
		c.inAwait++
		c.inTry++
		t := c.expr(call, nil)
		c.inTry--
		c.inAwait--
		if ci := c.info.Calls[call]; ci != nil && !c.isAsyncCall(ci) {
			c.errorf(call, diag.NotAsyncCall, "load takes async calls, and %s isn't one", exprName(call.Fun))
		}
		c.noteLoad(call)
		if IsInvalid(t) {
			bad = true
		}
		elems = append(elems, t)
	}
	if bad {
		return Invalid
	}
	tt := &Tuple{Elems: elems}
	c.record(g, tt)
	return &Named{Obj: loadingT, Args: []Type{tt}}
}

// groupText spells a group's elements for its error: calls by their
// function's name (api.user(…)), the rest as written.
func groupText(g *syntax.TupleExpr) string {
	var parts []string
	for _, x := range g.Elems {
		if call, ok := unparen(x).(*syntax.CallExpr); ok {
			parts = append(parts, exprName(call.Fun)+"(…)")
		} else {
			parts = append(parts, exprName(x))
		}
	}
	return strings.Join(parts, ", ")
}

// jsonable says why t can't be JSON ("" when it can). A type parameter
// of the function being checked (or of the store it's in) can: each
// call's type argument is checked instead (jsonGenerics).
func (c *Checker) jsonable(t Type) string {
	return jsonable1(t, map[*TypeName]bool{}, func(tp *TypeParam) bool {
		if !contains(c.enclosingParams(), tp) {
			return false
		}
		if c.jsonTP == nil {
			c.jsonTP = map[*TypeParam]bool{}
		}
		c.jsonTP[tp] = true
		return true
	})
}

// enclosingFunc is the declared function whose body is being checked.
func (c *Checker) enclosingFunc() *Function {
	for x := c.ctx; x != nil; x = x.Parent {
		if x.Func != nil {
			return x.Func
		}
	}
	return nil
}

// enclosingParams are the type parameters in scope where c is: the
// enclosing function's and its store's (or the store's whose member is
// being checked).
func (c *Checker) enclosingParams() []*TypeParam {
	var out []*TypeParam
	if f := c.enclosingFunc(); f != nil {
		out = append(out, f.TypeParams...)
	}
	for x := c.ctx; x != nil; x = x.Parent {
		if x.Comp != nil {
			out = append(out, x.Comp.TypeParams...)
			break
		}
	}
	return out
}

// A genericCall is a call of a generic function, or the making of a
// generic store, with its type arguments.
type genericCall struct {
	e     *syntax.CallExpr
	f     *Function // a function's
	store *TypeName // a store's
	args  []Type
	encl  *Function    // the function the call is in
	own   []*TypeParam // the type parameters where the call is
}

func (g genericCall) params() []*TypeParam {
	if g.store != nil {
		return g.store.TypeParams
	}
	return g.f.TypeParams
}

func (g genericCall) name() string {
	if g.store != nil {
		return g.store.name
	}
	return g.f.name
}

// jsonGenerics checks the type arguments of generic calls whose type
// parameters are read or written as JSON: a call passing a type
// parameter of its own function (or store) makes that one JSON too,
// until nothing changes; then every concrete argument must be JSON.
func (c *Checker) jsonGenerics() {
	if c.jsonTP == nil {
		c.jsonTP = map[*TypeParam]bool{}
	}
	for changed := true; changed; {
		changed = false
		for _, g := range c.genericCalls {
			for i, tp := range g.params() {
				if !c.jsonTP[tp] || i >= len(g.args) {
					continue
				}
				for _, own := range g.own {
					if mentionsParam(g.args[i], own) && !c.jsonTP[own] {
						c.jsonTP[own] = true
						changed = true
					}
				}
			}
		}
	}
	for _, g := range c.genericCalls {
		for i, tp := range g.params() {
			if !c.jsonTP[tp] || i >= len(g.args) || IsInvalid(g.args[i]) || HasTypeParams(g.args[i]) {
				continue
			}
			if why := jsonable1(g.args[i], map[*TypeName]bool{}, func(*TypeParam) bool { return true }); why != "" {
				c.errorf(g.e, diag.NotJSON, "%s reads or writes its %s as JSON, and %s can't be: %s", g.name(), tp.Name, aType(g.args[i]), why)
			}
		}
	}
}

// mentionsParam reports whether t mentions tp.
func mentionsParam(t Type, tp *TypeParam) bool {
	switch x := t.(type) {
	case *TypeParam:
		return x == tp
	case *Array:
		return mentionsParam(x.Elem, tp)
	case *Optional:
		return mentionsParam(x.Elem, tp)
	case *Map:
		return mentionsParam(x.Key, tp) || mentionsParam(x.Elem, tp)
	case *Set:
		return mentionsParam(x.Elem, tp)
	case *Named:
		for _, a := range x.Args {
			if mentionsParam(a, tp) {
				return true
			}
		}
	case *Tuple:
		for _, e := range x.Elems {
			if mentionsParam(e, tp) {
				return true
			}
		}
	}
	return false
}

func jsonable1(t Type, seen map[*TypeName]bool, param func(*TypeParam) bool) string {
	switch x := t.(type) {
	case *TypeParam:
		if param(x) {
			return ""
		}
	case *Basic:
		switch x {
		case Int, Float, Bool, String, Bytes, Duration, Color, Invalid:
			// Invalid: a type that isn't one, which is said where it's
			// written.
			return ""
		}
		return "it's a " + x.name
	case *Array:
		return jsonable1(x.Elem, seen, param)
	case *Optional:
		return jsonable1(x.Elem, seen, param)
	case *Map:
		if x.Key != String {
			return "a JSON object's keys are strings"
		}
		return jsonable1(x.Elem, seen, param)
	case *Set:
		// An array of its elements.
		return jsonable1(x.Elem, seen, param)
	case *Named:
		if seen[x.Obj] {
			return ""
		}
		seen[x.Obj] = true
		s := x.Subst()
		switch x.Obj.Kind {
		case StructType:
			for _, f := range x.Obj.Fields {
				if why := jsonable1(s.Apply(f.Type), seen, param); why != "" {
					return "its field " + f.Name + ": " + why
				}
			}
			return ""
		case EnumType:
			if x.Obj == loadingT {
				return "it's a Loading"
			}
			for _, k := range x.Obj.Cases {
				for _, p := range k.Params {
					if why := jsonable1(s.Apply(p), seen, param); why != "" {
						return "its case " + k.Name + ": " + why
					}
				}
			}
			return ""
		}
		return "it's a " + x.Obj.name
	case *Tuple:
		return "tuples aren't JSON: use a struct"
	}
	return "it's a " + typeString(t)
}

// ---------------------------------------------------------------------
// The standard library the compiler provides

// StdPath reports whether path is the standard library's (std/json) or
// the platform SDK's (platform/camera): packages the CLI carries, whose
// native fns the runtime implements.
func StdPath(path string) bool {
	return strings.HasPrefix(path, "std/") || strings.HasPrefix(path, "platform/")
}

// ProvidesStd reports whether the compiler itself provides the standard
// library's package at path (the rest are directories of std/).
func ProvidesStd(path string) bool {
	switch path {
	case "std/json", "std/math", "std/embed":
		return true
	}
	return false
}

func (c *Checker) makeStd(path string) *Package {
	p := &Package{Path: path, Prelude: false}
	p.Scope = NewScope(nil)
	add := func(name string, id BuiltinID, rt string, sig *FuncType) {
		p.Scope.Insert(&Builtin{object: object{name: name}, ID: id, RT: rt, Sig: sig})
	}
	f1 := &FuncType{Params: []Type{Float}, Result: Float}
	f2 := &FuncType{Params: []Type{Float, Float}, Result: Float}
	switch path {
	case "std/json":
		p.Name = "json"
		add("decode", BJSONDecode, "", nil)
		add("encode", BJSONEncode, "", nil)
	case "std/math":
		p.Name = "math"
		for _, n := range []string{"sqrt", "floor", "ceil", "round", "sin", "cos", "tan", "atan", "exp", "log", "log2", "log10"} {
			add(n, BMath, "math."+n, f1)
		}
		add("pow", BMath, "math.pow", f2)
		add("atan2", BMath, "math.atan2", f2)
		add("hypot", BMath, "math.hypot", f2)
		add("random", BMath, "math.random", &FuncType{Result: Float})
		add("randomInt", BMath, "math.random_int", &FuncType{Params: []Type{Int, Int}, Result: Int})
		add("seed", BMath, "math.seed", &FuncType{Params: []Type{Int}, Result: Void})
		add("clamp", BMath, "math.clampf", &FuncType{Params: []Type{Float, Float, Float}, Result: Float})
		pi := &Global{object: object{name: "pi"}, Pkg: p, Kind: ConstGlobal, Type: Float, Value: &syntax.BasicLit{Kind: syntax.FLOAT, Value: "3.141592653589793"}, Pub: true}
		p.Scope.Insert(pi)
		p.Globals = append(p.Globals, pi)
	case "std/embed":
		p.Name = "embed"
		add("text", BEmbedText, "", nil)
		add("bytes", BEmbedBytes, "", nil)
		add("json", BEmbedJSON, "", nil)
		add("texts", BEmbedTexts, "", nil)
		add("files", BEmbedFiles, "", nil)
	default:
		return nil
	}
	c.pkgs = append(c.pkgs, p)
	return p
}
