package check

import (
	"regexp"

	"github.com/egoist/asuka/lang/syntax"
)

// C from Asuka (RFC 0008, docs/c.md): extern fn, extern type and extern
// struct declare what C implements. Their parameters, results and
// fields are C types; Asuka sees each as one of its own (an Int32 is an
// int, a CString a string), and the glue lang/cffi generates converts
// between the two at the call.

// CKind is a C type's kind.
type CKind uint8

const (
	CVoid CKind = iota
	CInt8
	CInt16
	CInt32
	CInt64 // also Asuka's int
	CUInt8
	CUInt16
	CUInt32
	CUInt64
	CFloat32
	CDouble // Asuka's float
	CBool
	CString  // const char *
	CBytes   // const uint8_t *, size_t: two parameters
	CPointer // void *
	CExtern  // an extern type's pointer
	CStruct  // an extern struct, by value
	CFunc    // a callback: a function pointer and its context, two parameters
)

// cNames are the C types' names, written in extern declarations only.
var cNames = map[string]CKind{
	"Int8": CInt8, "Int16": CInt16, "Int32": CInt32, "Int64": CInt64,
	"UInt8": CUInt8, "UInt16": CUInt16, "UInt32": CUInt32, "UInt64": CUInt64,
	"Float32": CFloat32, "CString": CString,
}

// IsCTypeName reports whether name is one of the C types an extern
// declaration names (Int32, CString, ...).
func IsCTypeName(name string) bool { _, ok := cNames[name]; return ok }

// A CType is a parameter's, a result's or a field's type on C's side.
type CType struct {
	Kind CKind
	// Opt: the type was T?, and none is NULL (CString, Pointer, an
	// extern type, bytes, a callback).
	Opt   bool
	Named *TypeName // CExtern, CStruct
	Func  *CFuncType
}

// IsInt reports an integer kind.
func (t *CType) IsInt() bool { return t.Kind >= CInt8 && t.Kind <= CUInt64 }

// CFuncType is a callback's C signature: C calls it with its context
// first, then Params.
type CFuncType struct {
	Params []*CType
	Result *CType
}

// Extern is what an extern fn calls: the C function named as the Asuka
// one, through the builtin Builtin (ffi.<package>.<name>).
type Extern struct {
	Params  []*CType
	Result  *CType
	Builtin string
	// Releases: the extern type whose release this is (with release
	// name): calling it releases its argument at once.
	Releases *TypeName
}

// ExternBuiltin is the builtin the glue registers for f, which its body
// calls: ffi.<package>.<name>, the package by its import path (main for
// the root).
func ExternBuiltin(f *Function) string {
	p := "main"
	if f.Pkg != nil && f.Pkg.Path != "" {
		p = f.Pkg.Path
	}
	return "ffi." + p + "." + f.name
}

var cIdent = regexp.MustCompile(`^[A-Za-z_][A-Za-z0-9_]*$`)

// cPlace is where a C type is written: what it may be depends on it.
type cPlace uint8

const (
	cParam    cPlace = iota // an extern fn's parameter
	cResult                 // an extern fn's result
	cField                  // an extern struct's field
	cCbParam                // a callback's parameter
	cCbResult               // a callback's result
)

// cType resolves a type written in an extern declaration: the Asuka
// type the rest of the program sees, and its C side. It reports what
// can't cross, and returns Invalid then.
func (c *Checker) cType(t syntax.Type, at cPlace, what string) (Type, *CType) {
	switch x := t.(type) {
	case nil:
		return Invalid, nil
	case *syntax.OptionalType:
		e, ct := c.cType(x.Elem, at, what)
		if ct == nil {
			return Invalid, nil
		}
		switch ct.Kind {
		case CString, CPointer, CExtern:
			if at == cField {
				break // the field's own error follows
			}
			if at == cCbResult && ct.Kind != CPointer {
				break
			}
			ct.Opt = true
			return MakeOptional(e), ct
		case CBytes, CFunc:
			if at == cParam {
				ct.Opt = true
				return MakeOptional(e), ct
			}
		}
		c.errorf(t, "%s can't be none: C's NULL is a CString?, a Pointer?, an extern type's T? or (as a parameter) bytes? or a function's (fn(…))?", what)
		return Invalid, nil
	case *syntax.FuncType:
		switch at {
		case cParam:
		case cResult:
			c.errorf(t, "%s can't be a function: C gives none back (a callback is a parameter)", what)
			return Invalid, nil
		default:
			c.errorf(t, "%s can't be a function: a callback is an extern fn's parameter", what)
			return Invalid, nil
		}
		if x.Async.IsValid() {
			c.errorf(t, "a callback isn't async: C calls it and it runs to its end")
			return Invalid, nil
		}
		if x.Throws.IsValid() {
			c.errorf(t, "a callback doesn't throw: C can't catch Asuka's errors")
			return Invalid, nil
		}
		ft := &FuncType{Result: Void}
		cf := &CFuncType{Result: &CType{Kind: CVoid}}
		ok := true
		for i, p := range x.Params {
			pt, pc := c.cType(p, cCbParam, "the callback's parameter "+itoa(i+1))
			if pc == nil {
				ok = false
				continue
			}
			ft.Params = append(ft.Params, pt)
			cf.Params = append(cf.Params, pc)
		}
		if x.Result != nil {
			rt, rc := c.cType(x.Result, cCbResult, "the callback's result")
			if rc == nil {
				ok = false
			} else {
				ft.Result, cf.Result = rt, rc
			}
		}
		if !ok {
			return Invalid, nil
		}
		return ft, &CType{Kind: CFunc, Func: cf}
	case *syntax.TupleType:
		if len(x.Elems) == 0 && (at == cResult || at == cCbResult) {
			return Void, &CType{Kind: CVoid}
		}
		if len(x.Elems) == 1 {
			return c.cType(x.Elems[0], at, what)
		}
		c.errorf(t, "%s can't be a tuple: C has none (an extern struct is C's)", what)
		return Invalid, nil
	case *syntax.ArrayType:
		c.errorf(t, "%s can't be an array: C takes bytes, or a Pointer to memory of its own", what)
		return Invalid, nil
	case *syntax.MapType:
		c.errorf(t, "%s can't be a map: C has none", what)
		return Invalid, nil
	case *syntax.NamedType:
		if len(x.Path) == 1 && len(x.Args) == 0 {
			name := x.Path[0].Name
			if k, ok := cNames[name]; ok && c.lookupType(name) == nil {
				ct := &CType{Kind: k}
				if k == CString {
					if at == cField {
						c.errorf(t, "%s can't be a CString: a struct crosses by value, and the string's memory wouldn't outlive the call (hold a Pointer, or pass the string as a parameter)", what)
						return Invalid, nil
					}
					return String, ct
				}
				if k == CFloat32 {
					return Float, ct
				}
				return Int, ct
			}
		}
		ty := c.typeExpr(t)
		return c.cOf(t, ty, at, what)
	}
	ty := c.typeExpr(t)
	if IsInvalid(ty) {
		return Invalid, nil
	}
	return c.cOf(t, ty, at, what)
}

// cOf is the C side of an Asuka type written in an extern declaration.
func (c *Checker) cOf(at syntax.Node, t Type, place cPlace, what string) (Type, *CType) {
	if IsInvalid(t) {
		return Invalid, nil
	}
	switch t {
	case Int:
		return t, &CType{Kind: CInt64}
	case Float:
		return t, &CType{Kind: CDouble}
	case Bool:
		return t, &CType{Kind: CBool}
	case Pointer:
		return t, &CType{Kind: CPointer}
	case Void:
		if place == cResult || place == cCbResult {
			return t, &CType{Kind: CVoid}
		}
	case String:
		c.errorf(at, "%s is a string: it crosses to C as a CString (a NUL-terminated const char *), so write CString", what)
		return Invalid, nil
	case Bytes:
		switch place {
		case cParam:
			return t, &CType{Kind: CBytes}
		case cResult:
			c.errorf(at, "%s can't be bytes: their length can't come back from C with them (return a Pointer or an extern type, and read it with more functions)", what)
		default:
			c.errorf(at, "%s can't be bytes: bytes cross as an extern fn's parameter only (a pointer and a length)", what)
		}
		return Invalid, nil
	}
	if n, ok := t.(*Named); ok {
		switch {
		case n.Obj.Kind == ExternType:
			switch place {
			case cField:
				c.errorf(at, "%s can't be an extern type: Asuka frees it as its last reference goes, which a struct C copies can't follow (hold a Pointer)", what)
				return Invalid, nil
			case cCbParam, cCbResult:
				c.errorf(at, "%s can't be an extern type: who would free it? (pass a Pointer)", what)
				return Invalid, nil
			}
			return t, &CType{Kind: CExtern, Named: n.Obj}
		case n.Obj.Kind == StructType && n.Obj.Extern:
			switch place {
			case cCbParam, cCbResult:
				c.errorf(at, "%s can't be an extern struct: a callback takes numbers, bools, CStrings and Pointers", what)
				return Invalid, nil
			}
			return t, &CType{Kind: CStruct, Named: n.Obj}
		case n.Obj.Kind == StructType:
			c.errorf(at, "%s is %s, an Asuka struct: C takes an extern struct, whose fields are C types (extern struct %s { … })", what, n.Obj.name, n.Obj.name)
			return Invalid, nil
		}
	}
	c.errorf(at, "%s can't cross to C: a %s isn't a C type (an extern declaration takes Int8…Int64, UInt8…UInt64, int, Float32, float, bool, CString, bytes, Pointer, an extern type or struct, or a function; https://asuka.egoist.dev/docs/c)", what, typeString(t))
	return Invalid, nil
}

// externSig resolves an extern fn's signature, its C side in f.Extern.
func (c *Checker) externSig(f *Function) {
	d := f.Decl
	s := &Signature{Result: Void}
	f.Sig = s
	ext := &Extern{Builtin: ExternBuiltin(f), Result: &CType{Kind: CVoid}}
	f.Extern = ext
	seen := map[string]bool{}
	for _, p := range d.Params.List {
		if seen[p.Name.Name] {
			c.errorf(p.Name, "parameter %s is declared twice", p.Name.Name)
		}
		seen[p.Name.Name] = true
		var pt Type = Invalid
		var pc *CType
		if p.Type == nil {
			c.errorf(p.Name, "parameter %s needs a type: %s: Type", p.Name.Name, p.Name.Name)
		} else {
			pt, pc = c.cType(p.Type, cParam, "parameter "+p.Name.Name)
		}
		if pc == nil {
			pc = &CType{Kind: CInt64}
		}
		s.Params = append(s.Params, &Param{Name: p.Name.Name, Type: pt, Default: p.Default})
		ext.Params = append(ext.Params, pc)
	}
	if d.Result != nil {
		rt, rc := c.cType(d.Result, cResult, "the result of "+f.name)
		s.Result = rt
		if rc != nil {
			ext.Result = rc
		}
	}
}

// externFunc checks an extern fn's declaration (its signature is
// externSig's).
func (c *Checker) externFunc(f *Function) {
	d := f.Decl
	switch {
	case d.Body != nil || d.Value != nil:
		c.errorf(d.Name, "an extern fn has no body: C implements it")
	case len(d.TypeParams) > 0:
		c.errorf(d.Name, "an extern fn isn't generic: C's function has one signature")
	case d.Async.IsValid():
		c.errorf(d.Name, "an extern fn isn't async: a C call runs to its end (spawn runs one on another thread)")
	case d.Throws.IsValid():
		c.errorf(d.Name, "an extern fn doesn't throw: C has no errors to raise (return a status, and throw in Asuka)")
	case f.Recv != nil:
		c.errorf(d.Name, "an extern fn is declared at the top of a file, not in a type")
	case !cIdent.MatchString(f.name):
		c.errorf(d.Name, "%s isn't a C function's name (letters, digits and _)", f.name)
	}
}

// externType checks extern type T with release fn: fn is an extern fn of
// the package that takes a T.
func (c *Checker) externType(t *TypeName) {
	d := t.Decl.(*syntax.ExternTypeDecl)
	if d.Release == nil {
		return
	}
	o := t.Pkg.Scope.LookupLocal(d.Release.Name)
	f, ok := o.(*Function)
	if !ok || f.Decl == nil || !f.Decl.Extern.IsValid() {
		c.errorf(d.Release, "release %s: the function that frees a %s is an extern fn of this package (extern fn %s(x: %s))", d.Release.Name, t.name, d.Release.Name, t.name)
		return
	}
	c.info.Uses[d.Release] = f
	c.funcSig(f)
	if f.Extern == nil || len(f.Extern.Params) != 1 || f.Extern.Params[0].Kind != CExtern || f.Extern.Params[0].Named != t {
		c.errorf(d.Release, "release %s: it takes one %s, the pointer it frees", d.Release.Name, t.name)
		return
	}
	if f.Extern.Releases != nil {
		c.errorf(d.Release, "%s already releases %s", d.Release.Name, f.Extern.Releases.name)
		return
	}
	t.Release = f
	f.Extern.Releases = t
}

// externStruct checks an extern struct: fields only of C's value types,
// not generic, declaring nothing, not containing itself.
func (c *Checker) externStructDecl(t *TypeName, d *syntax.StructDecl) {
	if len(d.TypeParams) > 0 {
		c.errorf(d.Name, "an extern struct isn't generic: C's struct has one layout")
	}
	if len(d.Conforms) > 0 {
		c.errorf(d.Conforms[0], "an extern struct declares nothing: it's C's layout")
	}
}

// externStructCycle reports an extern struct that holds itself, which C
// can't lay out.
func (c *Checker) externStructCycle(t *TypeName) {
	var visit func(u *TypeName, path map[*TypeName]bool) bool
	visit = func(u *TypeName, path map[*TypeName]bool) bool {
		if path[u] {
			return true
		}
		path[u] = true
		defer delete(path, u)
		for _, f := range u.Fields {
			if f.C != nil && f.C.Kind == CStruct && visit(f.C.Named, path) {
				return true
			}
		}
		return false
	}
	if visit(t, map[*TypeName]bool{}) {
		c.errorf(t.Decl.(*syntax.StructDecl).Name, "extern struct %s holds itself: C can't lay it out (hold a Pointer)", t.name)
	}
}
