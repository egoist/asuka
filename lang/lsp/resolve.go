package lsp

import (
	"reflect"

	"github.com/egoist/asuka/lang/check"
	"github.com/egoist/asuka/lang/syntax"
)

// ---------------------------------------------------------------------
// Nodes at a position

// children are n's direct children, in source order.
func children(n syntax.Node) []syntax.Node {
	c := &collector{}
	func() {
		defer func() { recover() }() // a tree the parser left odd: what was found
		syntax.Walk(c, n)
	}()
	return c.kids
}

type collector struct {
	started bool
	kids    []syntax.Node
}

func (c *collector) Visit(n syntax.Node) syntax.Visitor {
	if n == nil || isNil(n) {
		return nil
	}
	if !c.started {
		c.started = true
		return c
	}
	c.kids = append(c.kids, n)
	return nil
}

func isNil(n syntax.Node) bool {
	if n == nil {
		return true
	}
	v := reflect.ValueOf(n)
	return v.Kind() == reflect.Pointer && v.IsNil()
}

// pathTo is the nodes from root down to the innermost one at pos: one
// that holds it, or ends right at it (a name the cursor is just after).
func pathTo(root syntax.Node, pos syntax.Pos) []syntax.Node {
	path := []syntax.Node{root}
	n := root
	for depth := 0; depth < 10000; depth++ {
		var best syntax.Node
		for _, k := range children(n) {
			r := k.Range()
			if !r.Start.IsValid() || pos < r.Start || pos > r.End {
				continue
			}
			if pos < r.End {
				best = k // holds it: the best there is
				break
			}
			if best == nil || r.Start < r.End {
				best = k // ends at it: unless one that holds it comes
			}
		}
		if best == nil {
			break
		}
		path = append(path, best)
		n = best
	}
	return path
}

// at is the node i from the end of path (1: the last), nil past its
// start.
func at(path []syntax.Node, i int) syntax.Node {
	if i > len(path) {
		return nil
	}
	return path[len(path)-i]
}

// ---------------------------------------------------------------------
// What a name refers to

// A ref is what a name refers to: an object of the checker, a field, an
// enum's case, a built-in method, a package, a semantic color, ...
type ref struct {
	name    string
	obj     check.Object
	field   *check.Field
	ecase   *check.Case
	owner   *check.TypeName // the type a field, a case or an interface's method is of
	imeth   *check.IfaceMethod
	method  *check.Method // a built-in method
	recv    check.Type    // its receiver's type (or a special member's)
	pkg     *check.Package
	special string // a member the compiler knows: "message", "value", "loading", "color", "auto"
	style   *syntax.StyleDecl
	// An event's parameter (event archive(id: string)): what an emit's
	// label names; event is the event.
	param *check.Param
	event *check.Member
	decl  syntax.Pos // where its name is declared; NoPos for what's built in
}

func (fc *fileCtx) objRef(o check.Object) *ref {
	if o == nil {
		return nil
	}
	r := &ref{name: o.Name(), obj: o, decl: o.Pos()}
	switch o := o.(type) {
	case *check.StyleDef:
		return &ref{name: o.Name(), style: o.Decl, decl: o.Pos()}
	case *check.PkgName:
		r.pkg = o.Pkg
	case *check.Member:
		r.owner = o.Owner
	case *check.Function:
		r.owner = o.Recv
	}
	if fl, m, owner, ok := check.SelfMember(o); ok {
		if fl != nil {
			return &ref{name: fl.Name, field: fl, owner: owner, decl: fl.Pos}
		}
		if owner.Kind == check.InterfaceType {
			// In a provided method: the interface's own.
			return fc.imethRef(owner, o.Name())
		}
		return fc.objRef(m)
	}
	return r
}

func fieldRef(fl *check.Field, owner *check.TypeName) *ref {
	return &ref{name: fl.Name, field: fl, owner: owner, decl: fl.Pos}
}

func caseRef(k *check.Case) *ref {
	return &ref{name: k.Name, ecase: k, owner: k.Owner, decl: k.Pos}
}

func (fc *fileCtx) imethRef(tn *check.TypeName, name string) *ref {
	if tn == nil || tn.Iface == nil {
		return nil
	}
	for _, m := range tn.Iface.Methods {
		if m.Name == name {
			r := &ref{name: name, imeth: m, owner: tn}
			if d, ok := tn.Decl.(*syntax.InterfaceDecl); ok {
				for _, mem := range d.Members {
					if f, ok := mem.(*syntax.FuncDecl); ok && f.Name.Name == name {
						r.decl = f.Name.Start
					}
				}
			}
			return r
		}
	}
	for _, f := range tn.Iface.Fields {
		if f.Name == name {
			return fieldRef(f, tn)
		}
	}
	return nil
}

func methodRef(m *check.Method, recv check.Type) *ref {
	return &ref{name: m.Name, method: m, recv: recv}
}

// builtinMethod is t's built-in method named name taking n arguments
// (any number, n < 0).
func builtinMethod(t check.Type, name string, n int) *check.Method {
	var first *check.Method
	for _, m := range check.MethodsOf(t) {
		if m.Name != name {
			continue
		}
		if first == nil {
			first = m
		}
		if n >= 0 {
			req := 0
			for _, p := range m.Params {
				if p.Default == nil {
					req++
				}
			}
			if n >= req && n <= len(m.Params) {
				return m
			}
		}
	}
	return first
}

// selRef is what a selection selects.
func (fc *fileCtx) selRef(sel *check.Selection, name string) *ref {
	switch sel.Kind {
	case check.SelField, check.SelIfaceField:
		if sel.Field != nil {
			return fieldRef(sel.Field, memberOwner(sel.Recv, name))
		}
	case check.SelTupleElem:
		if sel.Field != nil {
			return &ref{name: name, special: "tuple", field: sel.Field, recv: sel.Recv}
		}
	case check.SelCase:
		if sel.Case != nil {
			return caseRef(sel.Case)
		}
	case check.SelConst:
		return fc.objRef(sel.Global)
	case check.SelPkg:
		return fc.objRef(sel.Obj)
	case check.SelStoreMember:
		return fc.objRef(sel.Member)
	case check.SelMethod:
		if tn := memberOwner(sel.Recv, name); tn != nil {
			if tn.Kind == check.InterfaceType {
				return fc.imethRef(tn, name)
			}
			if f := tn.Methods[name]; f != nil {
				return fc.objRef(f)
			}
		}
	case check.SelBuiltinMethod:
		if m := builtinMethod(sel.Recv, name, -1); m != nil {
			return methodRef(m, sel.Recv)
		}
	case check.SelErrorMessage:
		return &ref{name: "message", special: "message", recv: sel.Recv}
	case check.SelLoading:
		return &ref{name: name, special: sel.Name, recv: sel.Recv} // "value", "loading"
	case check.SelColor:
		return &ref{name: name, special: "color", recv: check.Color}
	case check.SelLengthAuto:
		return &ref{name: "auto", special: "auto", recv: check.Length}
	case check.SelBuiltinField:
		if sel.Method != nil {
			return methodRef(sel.Method, sel.Recv)
		}
	case check.SelVariant:
		return fc.objRef(sel.Obj)
	}
	return nil
}

// namedObj is the declared type t is (through an optional), nil if none.
func namedObj(t check.Type) *check.TypeName {
	if n, ok := check.Unopt(t).(*check.Named); ok {
		return n.Obj
	}
	return nil
}

// memberOwner is the declared type a member name of a t is of: t's own
// (through an optional), or for a type parameter the interface among
// its constraints that asks for it.
func memberOwner(t check.Type, name string) *check.TypeName {
	if p, ok := check.Unopt(t).(*check.TypeParam); ok {
		for _, i := range p.Ifaces {
			if i.Obj.Iface == nil {
				continue
			}
			for _, m := range i.Obj.Iface.Methods {
				if m.Name == name {
					return i.Obj
				}
			}
			for _, f := range i.Obj.Iface.Fields {
				if f.Name == name {
					return i.Obj
				}
			}
		}
		return nil
	}
	return namedObj(t)
}

// callRef is what a call's callee refers to.
func (fc *fileCtx) callRef(call *syntax.CallExpr, name string) *ref {
	c := fc.info.Calls[call]
	if c == nil {
		return nil
	}
	switch c.Kind {
	case check.CallFunc, check.CallMethod, check.CallWorker:
		if c.Func != nil {
			// A store's, a worker's or a component's fn: its member.
			if c.Func.Recv != nil && c.Func.Recv.Comp != nil {
				if m := c.Func.Recv.Comp.Members[c.Func.Name()]; m != nil && m.Fn == c.Func {
					return fc.objRef(m)
				}
			}
			return fc.objRef(c.Func)
		}
	case check.CallBuiltinMethod:
		if c.Method != nil {
			return methodRef(c.Method, c.RecvType)
		}
	case check.CallIface, check.CallNative:
		return fc.imethRef(memberOwner(c.RecvType, name), name)
	case check.CallCase:
		if c.Case != nil {
			return caseRef(c.Case)
		}
	case check.CallBuiltin:
		if c.Builtin != nil {
			return fc.objRef(c.Builtin)
		}
	case check.CallStruct, check.CallStore, check.CallComponent, check.CallNativeNew, check.CallWorkerNew:
		if c.Type != nil {
			return fc.objRef(c.Type.Obj)
		}
	}
	return nil
}

// styleType is the prelude's Style: the props every element takes.
func (fc *fileCtx) styleType() *check.TypeName {
	if fc.prog.Prelude == nil {
		return nil
	}
	tn, _ := fc.prog.Prelude.Scope.LookupLocal("Style").(*check.TypeName)
	return tn
}

// enclosingElement is the element (or component) whose block the item
// at the end of path is in, and whether it's a style's (any element's).
func (fc *fileCtx) enclosingElement(path []syntax.Node) (tn *check.TypeName, style bool) {
	for i := len(path) - 1; i >= 0; i-- {
		switch n := path[i].(type) {
		case *syntax.CallExpr:
			if ec := fc.info.Elements[n]; ec != nil && ec.Comp != nil {
				return ec.Comp, false
			}
			if tn := fc.calleeComponent(n); tn != nil {
				return tn, false
			}
			return nil, false
		case *syntax.StyleDecl:
			return fc.styleType(), true
		case *syntax.LambdaExpr, *syntax.FuncDecl, *syntax.ComponentDecl, *syntax.ScreenDecl, *syntax.StoreDecl, *syntax.WorkerDecl, *syntax.AppDecl, *syntax.TestDecl, *syntax.BenchDecl, *syntax.PreviewDecl:
			return nil, false
		}
	}
	return nil, false
}

// calleeComponent is the component a call's callee names, whether the
// checker got as far as the call or not.
func (fc *fileCtx) calleeComponent(call *syntax.CallExpr) *check.TypeName {
	var o check.Object
	switch f := call.Fun.(type) {
	case *syntax.Ident:
		o = fc.info.Uses[f]
		if o == nil {
			o = fc.lookup(f.Name)
		}
	case *syntax.MemberExpr:
		o = fc.info.Uses[f.Name]
	}
	if tn, ok := o.(*check.TypeName); ok && tn.Comp != nil {
		return tn
	}
	return nil
}

// lookup finds a name in the file's scope (its package's, the
// prelude's, the universe's).
func (fc *fileCtx) lookup(name string) check.Object {
	if fc.pkg == nil {
		return nil
	}
	if s := fc.pkg.FileScope(fc.file); s != nil {
		return s.Lookup(name)
	}
	return fc.pkg.Scope.Lookup(name)
}

// componentMember is a component's member named name, or (for an
// element) the Style prop.
func (fc *fileCtx) componentMember(tn *check.TypeName, name string) *check.Member {
	if tn == nil || tn.Comp == nil {
		return nil
	}
	if m := tn.Comp.Members[name]; m != nil {
		return m
	}
	if tn.Comp.Native && (tn.Comp.Element == nil || !tn.Comp.Element.NoStyle) || tn.Comp.Style {
		if st := fc.styleType(); st != nil && st.Comp != nil {
			if m := st.Comp.Members[name]; m != nil && m.Kind == check.PropMember {
				return m
			}
		}
	}
	return nil
}

// resolve is what the name at the end of path refers to.
func (fc *fileCtx) resolve(path []syntax.Node) *ref {
	id, ok := at(path, 1).(*syntax.Ident)
	if !ok {
		return nil
	}
	info := fc.info
	parent, gp := at(path, 2), at(path, 3)
	switch p := parent.(type) {
	case *syntax.MemberExpr:
		if p.Name == id {
			if call, ok := gp.(*syntax.CallExpr); ok && call.Fun == p {
				if r := fc.callRef(call, id.Name); r != nil {
					return r
				}
			}
			if sel := info.Selections[p]; sel != nil {
				if r := fc.selRef(sel, id.Name); r != nil {
					return r
				}
			}
			// A built-in method of a call the checker gave up on, as
			// one being typed is (xs.map() with no argument yet): its
			// receiver's.
			if call, ok := gp.(*syntax.CallExpr); ok && call.Fun == p {
				if t := info.Types[p.X]; t != nil && !check.IsInvalid(t) {
					n := -1
					if call.Args != nil {
						n = len(call.Args.List)
					}
					if m := builtinMethod(t, id.Name, n); m != nil {
						return methodRef(m, t)
					}
				}
			}
		}
	case *syntax.ImplicitMember:
		if call, ok := gp.(*syntax.CallExpr); ok && call.Fun == p {
			if r := fc.callRef(call, id.Name); r != nil {
				return r
			}
		}
		if sel := info.Selections[p]; sel != nil {
			return fc.selRef(sel, id.Name)
		}
		return nil
	case *syntax.PropSet:
		if p.Name == id {
			tn, _ := fc.enclosingElement(path[:len(path)-2])
			return fc.objRef(memberObj(fc.componentMember(tn, id.Name)))
		}
	case *syntax.BindItem:
		if p.Name == id {
			tn, _ := fc.enclosingElement(path[:len(path)-2])
			return fc.objRef(memberObj(fc.componentMember(tn, id.Name)))
		}
	case *syntax.SlotItem:
		if p.Name == id {
			tn, _ := fc.enclosingElement(path[:len(path)-2])
			return fc.objRef(memberObj(fc.componentMember(tn, id.Name)))
		}
	case *syntax.OnHandler:
		if p.Name == id {
			tn, _ := fc.enclosingElement(path[:len(path)-2])
			return fc.objRef(memberObj(fc.componentMember(tn, id.Name)))
		}
	case *syntax.EmitStmt:
		if p.Name == id {
			if m := info.Emits[p]; m != nil {
				return fc.objRef(m)
			}
			return nil
		}
	case *syntax.Arg:
		if p.Label == id {
			return fc.labelRef(path, id.Name)
		}
	case *syntax.Param:
		// An event's parameter, in its declaration.
		if p.Name == id {
			if ev, ok := at(path, 4).(*syntax.EventDecl); ok {
				if m, ok := info.Defs[ev.Name].(*check.Member); ok {
					return fc.eventParamRef(m, id.Name)
				}
				return nil
			}
		}
	case *syntax.Field:
		if p.Name == id {
			if tn := fc.declType(gp); tn != nil {
				if fl := tn.Field(id.Name); fl != nil {
					return fieldRef(fl, tn)
				}
				if tn.Iface != nil {
					return fc.imethRef(tn, id.Name)
				}
			}
			return nil
		}
	case *syntax.EnumCase:
		if p.Name == id {
			if tn := fc.declType(gp); tn != nil {
				if k := tn.Case(id.Name); k != nil {
					return caseRef(k)
				}
			}
			return nil
		}
	case *syntax.FuncDecl:
		if p.Name == id {
			if _, ok := gp.(*syntax.InterfaceDecl); ok {
				return fc.imethRef(fc.declType(gp), id.Name)
			}
		}
	case *syntax.CasePat:
		// A label: a struct pattern's field, a case's payload's.
		for i := range p.Args {
			if p.Label(i) != id {
				continue
			}
			if fields := info.FieldPats[p]; i < len(fields) && fields[i] != nil {
				tn, _ := info.Uses[p.Path[len(p.Path)-1]].(*check.TypeName)
				return fieldRef(fields[i], tn)
			}
			if k := info.Cases[p]; k != nil {
				return caseRef(k)
			}
			return nil
		}
		if len(p.Path) > 0 && p.Path[len(p.Path)-1] == id {
			if k := info.Cases[p]; k != nil {
				return caseRef(k)
			}
			if !p.Implicit {
				break // the checker may name the type (catch T)
			}
			return nil
		}
	case *syntax.StyleUse:
		if d := fc.styleDecl(id.Name); d != nil {
			return &ref{name: id.Name, style: d, decl: d.Name.Start}
		}
		return nil
	case *syntax.StyleDecl:
		if p.Name == id {
			return &ref{name: id.Name, style: p, decl: p.Name.Start}
		}
	case *syntax.ImportDecl:
		if ip := info.Imports[p]; ip != nil {
			for _, n := range p.Names {
				if n == id {
					return fc.objRef(info.Defs[id])
				}
			}
			r := &ref{name: ip.Name, pkg: ip, decl: p.Start}
			if p.Alias != nil {
				r.name = p.Alias.Name
			}
			return r
		}
		return nil
	}
	if o := info.Defs[id]; o != nil {
		return fc.objRef(o)
	}
	if o := info.Uses[id]; o != nil {
		return fc.objRef(o)
	}
	return nil
}

func memberObj(m *check.Member) check.Object {
	if m == nil {
		return nil
	}
	return m
}

// declType is the type a declaration (a struct, an enum, an interface)
// declares.
func (fc *fileCtx) declType(n syntax.Node) *check.TypeName {
	var name *syntax.Ident
	switch d := n.(type) {
	case *syntax.PreviewDecl:
		// A preview's body is a component of no name's.
		if fc.pkg != nil {
			for _, pv := range fc.pkg.Previews {
				if pv.Decl == d {
					return pv.Comp
				}
			}
		}
		return nil
	case *syntax.StructDecl:
		name = d.Name
	case *syntax.EnumDecl:
		name = d.Name
	case *syntax.InterfaceDecl:
		name = d.Name
	case *syntax.ComponentDecl:
		name = d.Name
	case *syntax.ScreenDecl:
		name = d.Name
	case *syntax.StoreDecl:
		name = d.Name
	case *syntax.WorkerDecl:
		name = d.Name
	}
	if name == nil {
		return nil
	}
	tn, _ := fc.info.Defs[name].(*check.TypeName)
	return tn
}

// styleDecl finds the package's style named name.
func (fc *fileCtx) styleDecl(name string) *syntax.StyleDecl {
	if fc.pkg == nil {
		return nil
	}
	for _, f := range fc.pkg.Files {
		for _, d := range f.Decls {
			if s, ok := d.(*syntax.StyleDecl); ok && s.Name.Name == name {
				return s
			}
		}
	}
	return nil
}

// labelRef is what an argument's label names: a parameter, a struct's
// field, a component's parameter, a style's, an event's. path ends with
// the label, in its Arg, in the argument list of what it's passed to.
func (fc *fileCtx) labelRef(path []syntax.Node, name string) *ref {
	var call *syntax.CallExpr
	switch o := at(path, 4).(type) {
	case *syntax.CallExpr:
		call = o
	case *syntax.EmitStmt:
		if m := fc.info.Emits[o]; m != nil {
			return fc.eventParamRef(m, name)
		}
		return nil
	default:
		return nil
	}
	c := fc.info.Calls[call]
	if c == nil {
		if tn := fc.calleeComponent(call); tn != nil {
			return fc.objRef(memberObj(fc.componentMember(tn, name)))
		}
		if sd := fc.calleeStyle(call); sd != nil {
			for _, p := range sd.Params {
				if p.Name == name && p.Var != nil {
					return fc.objRef(p.Var)
				}
			}
		}
		return nil
	}
	switch c.Kind {
	case check.CallComponent, check.CallStore, check.CallWorkerNew:
		if c.Type != nil {
			return fc.objRef(memberObj(fc.componentMember(c.Type.Obj, name)))
		}
	case check.CallStruct:
		if c.Type != nil {
			if fl := c.Type.Obj.Field(name); fl != nil {
				return fieldRef(fl, c.Type.Obj)
			}
		}
	case check.CallFunc, check.CallMethod, check.CallWorker:
		if f := c.Func; f != nil && f.Decl != nil && f.Decl.Params != nil {
			for _, p := range f.Decl.Params.List {
				if p.Name.Name == name {
					if o := fc.info.Defs[p.Name]; o != nil {
						return fc.objRef(o)
					}
					return &ref{name: name, decl: p.Name.Start}
				}
			}
		}
	case check.CallCase:
		if c.Case != nil {
			return caseRef(c.Case)
		}
	}
	return nil
}

// calleeStyle is the style a call applies (style: chip(.green)), if
// it's one.
func (fc *fileCtx) calleeStyle(call *syntax.CallExpr) *check.StyleDef {
	var id *syntax.Ident
	switch f := call.Fun.(type) {
	case *syntax.Ident:
		id = f
	case *syntax.MemberExpr:
		id = f.Name
	}
	if id == nil {
		return nil
	}
	sd, _ := fc.info.Uses[id].(*check.StyleDef)
	return sd
}

// eventParamRef is an event's parameter named name, declared where the
// event's declaration names it.
func (fc *fileCtx) eventParamRef(m *check.Member, name string) *ref {
	for i, p := range m.Params {
		if p.Name != name {
			continue
		}
		r := &ref{name: name, param: p, event: m}
		if ev, ok := m.Decl.(*syntax.EventDecl); ok && ev.Params != nil && i < len(ev.Params.List) {
			r.decl = ev.Params.List[i].Name.Start
		}
		return r
	}
	return nil
}

// identAt is the name at pos and the path to it, if there's one.
func (fc *fileCtx) identAt(pos syntax.Pos) ([]syntax.Node, *syntax.Ident) {
	path := pathTo(fc.file, pos)
	// The parser stands in "_" for a name that's missing: not a name.
	if id, ok := at(path, 1).(*syntax.Ident); ok && fc.written(id) {
		return path, id
	}
	// A name the cursor is just after (Text|(…)): the editor's word.
	if pos > syntax.Pos(fc.file.File.Base()) {
		p2 := pathTo(fc.file, pos-1)
		if id, ok := at(p2, 1).(*syntax.Ident); ok && id.End == pos && fc.written(id) {
			return p2, id
		}
	}
	return path, nil
}

// written reports whether id is a name the file has where id says (not
// one the parser stood in for a missing name): its text, which may be
// spelled in another Unicode normal form than its Name (NFC).
func (fc *fileCtx) written(id *syntax.Ident) bool {
	return syntax.NormalizeIdent(fc.textOf(id.Span)) == id.Name
}

// textOf is the file's text in a span.
func (fc *fileCtx) textOf(sp syntax.Span) string {
	a, b := fc.offset(sp.Start), fc.offset(sp.End)
	if a < 0 || b > len(fc.m.src) || a > b {
		return ""
	}
	return string(fc.m.src[a:b])
}

// key names a declaration across checks: its file and offset.
type declKey struct {
	file   string
	offset int
}

func (fc *fileCtx) key(r *ref) (declKey, bool) {
	if r == nil || !r.decl.IsValid() {
		return declKey{}, false
	}
	f := fc.fset.File(r.decl)
	if f == nil {
		return declKey{}, false
	}
	return declKey{f.Name(), int(r.decl) - f.Base()}, true
}
