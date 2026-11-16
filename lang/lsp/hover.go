package lsp

import (
	"path/filepath"
	"sort"
	"strings"

	"github.com/egoist/asuka/lang/check"
	"github.com/egoist/asuka/lang/syntax"
	"github.com/egoist/asuka/lang/target"
)

func (s *server) hover(p *TextDocumentPositionParams) (*Hover, error) {
	fc, err := s.fileAt(p.TextDocument.URI)
	if err != nil {
		return nil, nil
	}
	pos := fc.pos(p.Position)
	path, id := fc.identAt(pos)
	if id != nil {
		r := fc.resolve(path)
		if r == nil {
			return fc.exprHover(path)
		}
		code, doc := fc.describe(r)
		if code == "" && doc == "" {
			return nil, nil
		}
		// What the name's value is here, when that says more than its
		// declaration: x.count() on [Todo] is an int.
		rg := fc.rng(id.Span)
		return &Hover{Contents: markdown(code, doc), Range: &rg}, nil
	}
	return fc.exprHover(path)
}

// exprHover shows the type of the expression at the end of path: a
// call's result, a literal's type, an operator's value.
func (fc *fileCtx) exprHover(path []syntax.Node) (*Hover, error) {
	for i := len(path) - 1; i >= 0; i-- {
		e, ok := path[i].(syntax.Expr)
		if !ok {
			continue
		}
		switch e := e.(type) {
		case *syntax.Ident, *syntax.LambdaExpr, *syntax.BadExpr:
			continue
		case *syntax.AtExpr:
			if e.Name != "platform" {
				return nil, nil
			}
			rg := fc.rng(e.Span)
			return &Hover{Contents: markdown("@platform: Platform", platformDoc(fc.st.platform)), Range: &rg}, nil
		}
		t := fc.info.Types[e]
		if t == nil || check.IsInvalid(t) || t == check.Void {
			continue
		}
		rg := fc.rng(e.Range())
		return &Hover{Contents: markdown(typeStr(t), ""), Range: &rg}, nil
	}
	return nil, nil
}

func markdown(code, doc string) MarkupContent {
	var b strings.Builder
	if code != "" {
		b.WriteString("```asuka\n" + code + "\n```")
	}
	if doc != "" {
		if code != "" {
			b.WriteString("\n\n---\n\n")
		}
		b.WriteString(doc)
	}
	return MarkupContent{Kind: "markdown", Value: b.String()}
}

// ---------------------------------------------------------------------
// Go to definition

func (s *server) definition(p *TextDocumentPositionParams) ([]Location, error) {
	fc, err := s.fileAt(p.TextDocument.URI)
	if err != nil {
		return nil, nil
	}
	path, id := fc.identAt(fc.pos(p.Position))
	if id == nil {
		return nil, nil
	}
	r := fc.resolve(path)
	if r == nil {
		return nil, nil
	}
	// A package: its files.
	if r.pkg != nil && (r.obj == nil || isPkgName(r.obj)) {
		if imp, ok := at(path, 2).(*syntax.ImportDecl); !ok || imp.Alias != id {
			return fc.pkgLocations(r.pkg), nil
		}
	}
	return fc.declLocations(r), nil
}

func isPkgName(o check.Object) bool {
	_, ok := o.(*check.PkgName)
	return ok
}

// declLocations is where r is declared.
func (fc *fileCtx) declLocations(r *ref) []Location {
	if !r.decl.IsValid() {
		return nil
	}
	end := r.decl + syntax.Pos(len(r.name))
	if r.pkg != nil {
		end = r.decl // an import: its start
	}
	if l, ok := fc.location(r.decl, end); ok {
		return []Location{l}
	}
	return nil
}

// pkgLocations are a package's files, each at its start.
func (fc *fileCtx) pkgLocations(p *check.Package) []Location {
	var locs []Location
	for _, f := range p.Files {
		if f == nil || f.File == nil {
			continue
		}
		name := f.File.Name()
		if !filepath.IsAbs(name) && !strings.HasPrefix(name, preludePrefix) {
			continue
		}
		locs = append(locs, Location{URI: fc.s.uri(name)})
	}
	sort.Slice(locs, func(i, j int) bool { return locs[i].URI < locs[j].URI })
	return locs
}

func (s *server) typeDefinition(p *TextDocumentPositionParams) ([]Location, error) {
	fc, err := s.fileAt(p.TextDocument.URI)
	if err != nil {
		return nil, nil
	}
	path, id := fc.identAt(fc.pos(p.Position))
	var t check.Type
	if id != nil {
		if r := fc.resolve(path); r != nil {
			t = refType(r)
		}
		if t == nil {
			if e, ok := at(path, 2).(*syntax.MemberExpr); ok && e.Name == id {
				t = fc.info.Types[e]
			} else {
				t = fc.info.Types[id]
			}
		}
	} else {
		for i := len(path) - 1; i >= 0 && t == nil; i-- {
			if e, ok := path[i].(syntax.Expr); ok {
				t = fc.info.Types[e]
			}
		}
	}
	tn := typeNameOf(t)
	if tn == nil || !tn.Pos().IsValid() {
		return nil, nil
	}
	return fc.declLocations(fc.objRef(tn)), nil
}

// refType is the type of the value r names.
func refType(r *ref) check.Type {
	switch {
	case r.field != nil:
		return r.field.Type
	case r.ecase != nil && r.owner != nil:
		return r.owner.Type
	case r.method != nil:
		_, res := r.method.Instantiate(r.recv)
		return res
	}
	switch o := r.obj.(type) {
	case *check.Var:
		return o.Type
	case *check.Global:
		return o.Type
	case *check.Member:
		if o.Kind == check.FnMember && o.Fn != nil && o.Fn.Sig != nil {
			return o.Fn.Sig.Result
		}
		return o.Type
	case *check.Function:
		if o.Sig != nil {
			return o.Sig.Result
		}
	case *check.TypeName:
		return o.Type
	}
	return nil
}

// typeNameOf is the declared type at the heart of t: T of T?, [T], Task<T>.
func typeNameOf(t check.Type) *check.TypeName {
	for i := 0; i < 8 && t != nil; i++ {
		switch x := t.(type) {
		case *check.Named:
			return x.Obj
		case *check.Optional:
			t = x.Elem
		case *check.Array:
			t = x.Elem
		case *check.Map:
			t = x.Elem
		case *check.Set:
			t = x.Elem
		case *check.Task:
			t = x.Elem
		case *check.Stream:
			t = x.Elem
		case *check.FuncType:
			t = x.Result
		default:
			return nil
		}
	}
	return nil
}

// platformDoc is @platform's hover: what it is, and the platform the
// editor checks the file for (whose files it reads).
func platformDoc(p target.Platform) string {
	return "The platform the app is compiled for, a constant: `.iOS`, `.android`, `.macOS`, `.windows`, `.linux` or `.web`. " +
		"Every branch is checked; one it doesn't take on a platform isn't part of that platform's build.\n\n" +
		"This file is read here with " + p.Or().Title() + "'s files; its diagnostics are every platform's."
}
