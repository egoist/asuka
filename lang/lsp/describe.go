package lsp

import (
	"bytes"
	"fmt"
	"strings"

	"github.com/egoist/asuka/lang/check"
	"github.com/egoist/asuka/lang/syntax"
)

// How the server shows what a name is: a declaration as Asuka source
// (hover's code block, completion's detail) and its /// documentation.

// exprString is an expression as source.
func exprString(e syntax.Expr) string {
	if e == nil || isNil(e) {
		return ""
	}
	var b bytes.Buffer
	ok := func() (ok bool) {
		defer func() {
			if recover() != nil {
				ok = false
			}
		}()
		return syntax.Fprint(&b, nil, e) == nil
	}()
	if !ok {
		return "…"
	}
	s := strings.TrimSpace(b.String())
	if len(s) > 60 || strings.Contains(s, "\n") {
		return "…"
	}
	return s
}

func typeStr(t check.Type) string {
	if t == nil {
		return "?"
	}
	if check.IsInvalid(t) {
		return "?"
	}
	return t.String()
}

// paramsString is (a: int, b: string = "x").
func paramsString(ps []*check.Param) string {
	var b strings.Builder
	b.WriteByte('(')
	for i, p := range ps {
		if i > 0 {
			b.WriteString(", ")
		}
		b.WriteString(paramString(p))
	}
	b.WriteByte(')')
	return b.String()
}

func paramString(p *check.Param) string {
	s := p.Name
	if p.Type != nil {
		s += ": " + typeStr(p.Type)
	}
	if p.Default != nil {
		s += " = " + exprString(p.Default)
	}
	return s
}

func resultString(t check.Type, async, throws bool) string {
	s := ""
	if throws {
		s += " throws"
	}
	if t != nil && t != check.Void {
		s += " -> " + typeStr(t)
	}
	return s
}

// funcSig is a function's signature: async fn name<T>(params) throws ->
// Result.
func funcSig(f *check.Function) string {
	var b strings.Builder
	if f.Sig != nil && f.Sig.Async || f.Decl != nil && f.Decl.Async.IsValid() {
		b.WriteString("async ")
	}
	b.WriteString("fn ")
	if f.Recv != nil {
		b.WriteString(f.Recv.Name() + ".")
	}
	b.WriteString(f.Name())
	if len(f.TypeParams) > 0 {
		var tps []string
		for _, tp := range f.TypeParams {
			s := tp.Name
			if tp.Constraint() != "" {
				s += ": " + tp.Constraint()
			}
			tps = append(tps, s)
		}
		b.WriteString("<" + strings.Join(tps, ", ") + ">")
	}
	if f.Sig != nil {
		b.WriteString(paramsString(f.Sig.Params))
		b.WriteString(resultString(f.Sig.Result, f.Sig.Async, f.Sig.Throws))
	} else {
		b.WriteString("(…)")
	}
	return b.String()
}

// localFnSig is a fn declared in a block's signature.
func localFnSig(f *check.LocalFn) string {
	s := "fn " + f.Var.Name() + paramsString(f.Sig.Params) + resultString(f.Sig.Result, f.Sig.Async, f.Sig.Throws)
	if f.Sig.Async {
		s = "async " + s
	}
	return s
}

// methodSig is a built-in method's signature, for a receiver of type
// recv: fn [int].map(transform: fn(int) -> U) -> [U].
func methodSig(m *check.Method, recv check.Type) string {
	ps, res := m.Params, m.Result
	if recv != nil {
		ps, res = m.Instantiate(recv)
	}
	r := "fn "
	if m.Async {
		r = "async fn "
	}
	recvS := m.Recv
	if recv != nil {
		recvS = typeStr(recv)
	}
	return r + recvS + "." + m.Name + paramsString(ps) + resultString(res, m.Async, m.Throws)
}

func methodDoc(m *check.Method) string {
	doc := m.Doc
	if doc != "" {
		doc = strings.ToUpper(doc[:1]) + doc[1:] + "."
	}
	var notes []string
	if m.Inout {
		notes = append(notes, "changes its receiver (a `var` or state)")
	}
	if m.Needs != "" {
		notes = append(notes, "needs "+m.Needs+" elements")
	}
	if len(notes) > 0 {
		if doc != "" {
			doc += "\n\n"
		}
		doc += "*" + strings.Join(notes, "; ") + "*"
	}
	return doc
}

// docOf is a declaration's /// comment.
func docOf(n syntax.Node) string {
	if n == nil || isNil(n) {
		return ""
	}
	if t := syntax.TriviaOf(n); t != nil {
		return t.Doc()
	}
	return ""
}

// typeDecl is a type's declaration header and its insides, briefly.
func typeDecl(tn *check.TypeName) string {
	var b strings.Builder
	pub := ""
	if pubDecl(tn.Decl) {
		pub = "pub "
	}
	tps := ""
	if len(tn.TypeParams) > 0 {
		var ns []string
		for _, tp := range tn.TypeParams {
			ns = append(ns, tp.Name)
		}
		tps = "<" + strings.Join(ns, ", ") + ">"
	}
	const most = 24
	switch tn.Kind {
	case check.BasicType:
		return "type " + tn.Name()
	case check.ParamType:
		if tp, ok := tn.Type.(*check.TypeParam); ok && tp.Constraint() != "" {
			return "type parameter " + tn.Name() + ": " + tp.Constraint()
		}
		return "type parameter " + tn.Name()
	case check.AliasType:
		return pub + "type " + tn.Name() + tps + " = " + typeStr(tn.Type)
	case check.StructType:
		fmt.Fprintf(&b, "%sstruct %s%s%s {", pub, tn.Name(), tps, conforms(tn))
		for i, f := range tn.Fields {
			if i == most {
				b.WriteString("\n  …")
				break
			}
			fmt.Fprintf(&b, "\n  %s: %s", f.Name, typeStr(f.Type))
			if f.Default != nil {
				b.WriteString(" = " + exprString(f.Default))
			}
		}
		if len(tn.Fields) == 0 {
			b.WriteString("}")
		} else {
			b.WriteString("\n}")
		}
	case check.EnumType:
		fmt.Fprintf(&b, "%senum %s%s%s {", pub, tn.Name(), tps, conforms(tn))
		for i, k := range tn.Cases {
			if i == most {
				b.WriteString("\n  …")
				break
			}
			b.WriteString("\n  " + caseString(k))
		}
		if len(tn.Cases) == 0 {
			b.WriteString("}")
		} else {
			b.WriteString("\n}")
		}
	case check.InterfaceType:
		native := ""
		if tn.Native {
			native = "native "
		}
		fmt.Fprintf(&b, "%s%sinterface %s%s {", pub, native, tn.Name(), tps)
		n := 0
		if tn.Iface != nil {
			for _, f := range tn.Iface.Fields {
				fmt.Fprintf(&b, "\n  %s: %s", f.Name, typeStr(f.Type))
				n++
			}
			for _, m := range tn.Iface.Methods {
				b.WriteString("\n  " + imethSig(m))
				n++
			}
		}
		if n == 0 {
			b.WriteString("}")
		} else {
			b.WriteString("\n}")
		}
	case check.StoreType, check.ComponentType:
		b.WriteString(compHeader(tn))
	}
	return b.String()
}

func conforms(tn *check.TypeName) string {
	var ns []string
	if tn.IsError {
		ns = append(ns, "Error")
	}
	for _, c := range tn.Conforms {
		ns = append(ns, c.Name())
	}
	if len(ns) == 0 {
		return ""
	}
	return ": " + strings.Join(ns, ", ")
}

func caseString(k *check.Case) string {
	if len(k.Params) == 0 {
		return k.Name
	}
	var ps []string
	for i, t := range k.Params {
		s := typeStr(t)
		if i < len(k.Labels) && k.Labels[i] != "" {
			s = k.Labels[i] + ": " + s
		}
		ps = append(ps, s)
	}
	return k.Name + "(" + strings.Join(ps, ", ") + ")"
}

func imethSig(m *check.IfaceMethod) string {
	s := "fn " + m.Name
	if m.Sig != nil {
		if m.Sig.Async {
			s = "async " + s
		}
		s += paramsString(m.Sig.Params) + resultString(m.Sig.Result, m.Sig.Async, m.Sig.Throws)
	}
	return s
}

// compHeader is component Name(params), screen Name, store Name(params),
// native component Text(text: string).
func compHeader(tn *check.TypeName) string {
	comp := tn.Comp
	kw := "component"
	switch {
	case comp == nil:
	case comp.Store:
		kw = "store"
	case comp.Screen:
		kw = "screen"
	case comp.Native:
		kw = "native component"
	}
	s := kw + " " + tn.Name()
	if pubDecl(tn.Decl) {
		s = "pub " + s
	}
	if comp != nil && len(comp.Params) > 0 {
		var ps []string
		for _, m := range comp.Params {
			ps = append(ps, memberParam(m))
		}
		s += "(" + strings.Join(ps, ", ") + ")"
	}
	return s
}

func memberParam(m *check.Member) string {
	s := m.Name() + ": " + typeStr(m.Type)
	if m.Default != nil {
		s += " = " + exprString(m.Default)
	}
	return s
}

// compMembers lists a component's props, events and slots, for hover.
func compMembers(tn *check.TypeName) string {
	if tn.Comp == nil {
		return ""
	}
	var props, events, slots []string
	for _, m := range tn.Comp.Order {
		switch m.Kind {
		case check.PropMember:
			if m.Name() == "children" {
				continue
			}
			if m.IsSlot() {
				slots = append(slots, "`"+m.Name()+"`")
			} else {
				props = append(props, "`"+m.Name()+": "+typeStr(m.Type)+"`")
			}
		case check.EventMember:
			events = append(events, "`"+eventSig(m)+"`")
		}
	}
	var b strings.Builder
	if len(props) > 0 {
		b.WriteString("**Props:** " + strings.Join(props, " · ") + "\n\n")
	}
	if len(events) > 0 {
		b.WriteString("**Events:** " + strings.Join(events, " · ") + "\n\n")
	}
	if len(slots) > 0 {
		b.WriteString("**Slots:** " + strings.Join(slots, " · ") + "\n\n")
	}
	if c := tn.Comp; c.Native && (c.Element == nil || !c.Element.NoStyle) && !c.Style {
		b.WriteString("Takes every element's style props too (`padding`, `background`, `font`, …).\n\n")
	}
	return strings.TrimSpace(b.String())
}

func eventSig(m *check.Member) string {
	s := m.Name()
	if len(m.Params) > 0 {
		s += paramsString(m.Params)
	}
	return s
}

func pubDecl(n syntax.Node) bool {
	switch d := n.(type) {
	case *syntax.StructDecl:
		return d.Pub.IsValid()
	case *syntax.EnumDecl:
		return d.Pub.IsValid()
	case *syntax.InterfaceDecl:
		return d.Pub.IsValid()
	case *syntax.ComponentDecl:
		return d.Pub.IsValid()
	case *syntax.ScreenDecl:
		return d.Pub.IsValid()
	case *syntax.StoreDecl:
		return d.Pub.IsValid()
	case *syntax.TypeAliasDecl:
		return d.Pub.IsValid()
	case *syntax.ExternTypeDecl:
		return d.Pub.IsValid()
	case *syntax.FuncDecl:
		return d.Pub.IsValid()
	}
	return false
}

// memberDecl is a component's or a store's member as declared.
func memberDecl(m *check.Member) string {
	pub := ""
	if d, ok := m.Decl.(*syntax.StateDecl); ok && d.Pub.IsValid() {
		pub = "pub "
	}
	if d, ok := m.Decl.(*syntax.LetDecl); ok && d.Pub.IsValid() {
		pub = "pub "
	}
	switch m.Kind {
	case check.ParamMember:
		return memberParam(m)
	case check.PropMember:
		s := "prop " + m.Name() + ": " + typeStr(m.Type)
		if m.Default != nil {
			s += " = " + exprString(m.Default)
		}
		return s
	case check.EventMember:
		return "event " + eventSig(m)
	case check.StateMember:
		return pub + "state " + m.Name() + ": " + typeStr(m.Type)
	case check.LetMember:
		return pub + "let " + m.Name() + ": " + typeStr(m.Type)
	case check.UseMember:
		return "use " + m.Name() + ": " + typeStr(m.Type)
	case check.FnMember:
		if m.Fn != nil {
			s := funcSig(m.Fn)
			if m.Pub {
				s = "pub " + s
			}
			return s
		}
	}
	return m.Name() + ": " + typeStr(m.Type)
}

func memberKindName(m *check.Member) string {
	switch m.Kind {
	case check.ParamMember:
		return "parameter"
	case check.PropMember:
		if m.IsSlot() {
			return "slot"
		}
		return "prop"
	case check.EventMember:
		return "event"
	case check.StateMember:
		return "state"
	case check.LetMember:
		return "let"
	case check.UseMember:
		return "use"
	case check.FnMember:
		return "fn"
	}
	return "member"
}

func globalDecl(g *check.Global) string {
	pub := ""
	if g.Pub && g.Owner == nil {
		pub = "pub "
	}
	kw := "let"
	if g.Kind == check.StateGlobal {
		kw = "state"
	}
	name := g.Name()
	if g.Owner != nil {
		name = g.Owner.Name() + "." + name
	}
	s := pub + kw + " " + name + ": " + typeStr(g.Type)
	if g.Kind == check.ConstGlobal && g.Value != nil {
		if v := exprString(g.Value); v != "…" && len(v) <= 40 {
			s += " = " + v
		}
	}
	return s
}

// The functions the compiler provides: their signatures as the docs
// write them, and what they do.
var builtinDocs = map[string][2]string{
	"print":       {"fn print(value)", "Prints a value (a string, a number, or anything shown as text) on a line of the log."},
	"expect":      {"fn expect(condition: bool, message: string = \"\")", "Fails the test unless the condition holds."},
	"sleep":       {"async fn sleep(duration: Duration)", "Waits for the duration, without blocking anything."},
	"all":         {"async fn all(tasks…) -> (…)", "Waits for every task and gives their results; the first error cancels the rest."},
	"race":        {"async fn race(tasks…) -> T", "Waits for the first task to finish; the rest are cancelled."},
	"mount":       {"fn mount(view: View, provide: Store = none) -> TestApp", "Mounts a view in a test, on the fake host, and returns the app to drive and check: `tap`, `type`, `expect`, ..."},
	"idle":        {"async fn idle()", "Waits until nothing is left to run."},
	"push":        {"fn push(view: View)", "Pushes a screen onto the navigation stack."},
	"pop":         {"fn pop()", "Pops the top screen off the navigation stack."},
	"present":     {"fn present(view: View)", "Presents a screen modally (a sheet)."},
	"dismiss":     {"fn dismiss()", "Dismisses the screen presented modally."},
	"min":         {"fn min(a: T, b: T) -> T", "The smaller of two numbers (ints or floats)."},
	"max":         {"fn max(a: T, b: T) -> T", "The larger of two numbers (ints or floats)."},
	"abs":         {"fn abs(x: T) -> T", "A number's absolute value."},
	"float":       {"fn float(n: int) -> float", "An int as a float."},
	"int":         {"fn int(x: float) -> int", "A float as an int, truncated toward zero."},
	"panic":       {"fn panic(message: string) -> never", "Stops the program with a message: for what can't happen."},
	"withTimeout": {"async fn withTimeout(duration: Duration) { … } throws", "Runs the block, cancelling it with an error when it takes longer than the duration."},
	"Stream":      {"fn Stream<T>(produce: async fn(send: fn(T)) throws) -> Stream<T>", "A stream whose values the block sends; read it with `for await`."},
	"Set":         {"fn Set<T: Hashable>(elements: [T] = []) -> Set<T>", "A set of the array's distinct elements, in the order they first come (`Set<T>()`: none). Where a Set is expected, an array literal is one: `let tags: Set<string> = [\"a\", \"b\"]`."},
	"decode":      {"fn decode<T>(text: string) throws -> T", "Reads JSON text as a T (json.decode<T>(text), or where a T is expected)."},
	"encode":      {"fn encode(value: T, indent: int = 0) -> string", "Writes a value as JSON text, indented when indent > 0."},
}

// builtinSig is a built-in function's signature and doc.
func builtinSig(b *check.Builtin) (string, string) {
	if b.Sig != nil {
		var ps []string
		for i, p := range b.Sig.Params {
			ps = append(ps, fmt.Sprintf("_%d: %s", i+1, typeStr(p)))
		}
		return "fn " + b.Name() + "(" + strings.Join(ps, ", ") + ")" + resultString(b.Sig.Result, false, false), mathDocs[b.Name()]
	}
	if d, ok := builtinDocs[b.Name()]; ok {
		return d[0], d[1]
	}
	return "fn " + b.Name() + "(…)", ""
}

var mathDocs = map[string]string{
	"sqrt": "The square root.", "floor": "Rounded down.", "ceil": "Rounded up.", "round": "Rounded to the nearest integer.",
	"sin": "The sine (radians).", "cos": "The cosine (radians).", "tan": "The tangent (radians).", "atan": "The arctangent, in radians.",
	"exp": "e to the power x.", "log": "The natural logarithm.", "log2": "The base-2 logarithm.", "log10": "The base-10 logarithm.", "pow": "x to the power y.", "atan2": "The angle of (x, y), in radians.",
	"random": "A random float in [0, 1).", "randomInt": "A random int in [lo, hi].", "seed": "Seeds the random numbers, to repeat them.",
	"clamp": "x kept between lo and hi.", "now": "The time now, in milliseconds since 1970.", "monotonic": "A clock that only goes forward: for measuring durations.",
}

// describe is how hover shows what r refers to: a code block's source,
// and markdown under it.
func (fc *fileCtx) describe(r *ref) (code, doc string) {
	switch {
	case r.style != nil:
		return "style " + r.name, docOf(r.style)
	case r.special != "":
		switch r.special {
		case "message":
			return "message: string", "The error's message."
		case "value":
			return "value: " + typeStr(loadingValue(r.recv)), "The value once it's loaded, none until then."
		case "color":
			return "Color." + r.name, "A semantic color: it follows the platform's light and dark appearance."
		case "auto":
			return "Length.auto", "The size its content and its parent's layout give it."
		}
	case r.method != nil:
		return methodSig(r.method, r.recv), methodDoc(r.method)
	case r.imeth != nil:
		code = imethSig(r.imeth)
		if r.owner != nil {
			code = strings.Replace(code, "fn ", "fn "+r.owner.Name()+".", 1)
		}
		return code, fc.declDoc(r.decl)
	case r.field != nil:
		code = r.field.Name + ": " + typeStr(r.field.Type)
		if r.field.Default != nil {
			code += " = " + exprString(r.field.Default)
		}
		if r.owner != nil {
			doc = "Field of `" + r.owner.Name() + "`."
		}
		return code, joinDoc(fc.declDoc(r.decl), doc)
	case r.ecase != nil:
		owner := ""
		if r.owner != nil {
			owner = r.owner.Name() + "."
		}
		return "case " + owner + caseString(r.ecase), fc.declDoc(r.decl)
	case r.pkg != nil:
		return "package " + pkgPath(r.pkg), pkgDoc(r.pkg)
	}
	switch o := r.obj.(type) {
	case *check.Var:
		if o.Fn != nil {
			return localFnSig(o.Fn), docOf(o.Fn.Decl)
		}
		return fc.varDecl(o), ""
	case *check.Global:
		d := ""
		if g, ok := o.Decl.(syntax.Node); ok {
			d = docOf(g)
		}
		return globalDecl(o), d
	case *check.Member:
		code = memberDecl(o)
		owner := ""
		if o.Owner != nil {
			owner = memberKindName(o) + " of `" + o.Owner.Name() + "`"
			if o.Owner.Comp != nil && o.Owner.Comp.Style {
				owner = "style prop: every element takes it"
			}
		}
		return code, joinDoc(docOf(o.Decl), owner)
	case *check.Function:
		code = funcSig(o)
		if o.Decl != nil && o.Decl.Pub.IsValid() {
			code = "pub " + code
		}
		return code, docOf(o.Decl)
	case *check.TypeName:
		code = typeDecl(o)
		doc = docOf(o.Decl)
		if o.Comp != nil {
			doc = joinDoc(doc, compMembers(o))
		}
		return code, doc
	case *check.Builtin:
		return builtinSig(o)
	}
	if r.decl.IsValid() {
		return r.name, ""
	}
	return "", ""
}

func joinDoc(a, b string) string {
	switch {
	case a == "":
		return b
	case b == "":
		return a
	}
	return a + "\n\n" + b
}

// declDoc is the doc comment of the declaration whose name is at pos.
func (fc *fileCtx) declDoc(pos syntax.Pos) string {
	if !pos.IsValid() {
		return ""
	}
	f := fc.fset.File(pos)
	if f == nil {
		return ""
	}
	sf := fc.st.files[f.Name()]
	if sf == nil {
		return ""
	}
	path := pathTo(sf, pos)
	for i := len(path) - 1; i >= 0; i-- {
		if t := syntax.TriviaOf(path[i]); t != nil {
			return t.Doc()
		}
	}
	return ""
}

// varDecl is a local as declared: let x: T, var x: T, or a parameter.
func (fc *fileCtx) varDecl(v *check.Var) string {
	kw := "let "
	if v.Mutable {
		kw = "var "
	}
	switch v.Kind {
	case check.SelfVar:
		return "self: " + typeStr(v.Type)
	case check.RowItem:
		kw = "for "
	}
	if f := fc.fset.File(v.Pos()); f != nil {
		if sf := fc.st.files[f.Name()]; sf != nil {
			path := pathTo(sf, v.Pos())
			switch at(path, 2).(type) {
			case *syntax.Param, *syntax.OnHandler, *syntax.LambdaExpr:
				kw = "(parameter) "
			case *syntax.CatchClause:
				kw = "catch "
			}
			for i := len(path) - 1; i >= 0; i-- {
				if _, ok := path[i].(*syntax.ForStmt); ok {
					if p := path[i].(*syntax.ForStmt).Pattern; p != nil && p.Range().Start <= v.Pos() && v.Pos() < p.Range().End {
						kw = "for "
					}
					break
				}
			}
		}
	}
	return kw + v.Name() + ": " + typeStr(v.Type)
}

func loadingValue(t check.Type) check.Type {
	if n, ok := t.(*check.Named); ok && len(n.Args) > 0 {
		return check.MakeOptional(n.Args[0])
	}
	return nil
}

func pkgPath(p *check.Package) string {
	if p.Path == "" {
		return p.Name
	}
	return p.Path
}

// pkgDoc is a package's comment: its first file's comments at the top,
// above its imports and declarations.
func pkgDoc(p *check.Package) string {
	if p == nil || len(p.Files) == 0 {
		return ""
	}
	f := p.Files[0]
	var start syntax.Pos
	if len(f.Imports) > 0 {
		start = f.Imports[0].Start
	} else if len(f.Decls) > 0 {
		start = f.Decls[0].Range().Start
	}
	var lines []string
	for _, c := range f.Comments {
		if start.IsValid() && c.Pos >= start {
			break
		}
		t := strings.TrimPrefix(strings.TrimPrefix(c.Text, "//"), "/")
		lines = append(lines, strings.TrimPrefix(t, " "))
	}
	return strings.TrimSpace(strings.Join(lines, "\n"))
}
