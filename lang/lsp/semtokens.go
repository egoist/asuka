package lsp

import (
	"sort"

	"github.com/egoist/asuka/lang/check"
	"github.com/egoist/asuka/lang/syntax"
)

// Semantic tokens: each name colored by what it refers to (a type, a
// prop, an enum's case, a parameter), which a grammar can't know.

func tokenType(name string) int {
	for i, t := range tokenTypes {
		if t == name {
			return i
		}
	}
	return -1
}

const (
	modDeclaration = 1 << iota
	modReadonly
	modDefaultLibrary
	modModification
)

type semToken struct {
	start, end int // byte offsets
	typ        int
	mods       int
}

func (s *server) semanticTokens(p *SemanticTokensParams) (*SemanticTokens, error) {
	fc, err := s.fileAt(p.TextDocument.URI)
	if err != nil {
		return &SemanticTokens{Data: []uint32{}}, nil
	}
	var toks []semToken
	walkPaths(fc.file, func(path []syntax.Node) {
		if a, ok := path[len(path)-1].(*syntax.AtExpr); ok {
			// @platform, @debug, @define: constants the compiler gives (a
			// @define's arguments are tokens of their own).
			end := fc.offset(a.End)
			if a.Args != nil {
				end = fc.offset(a.Start) + 1 + len(a.Name)
			}
			toks = append(toks, semToken{start: fc.offset(a.Start), end: end, typ: tokenType("macro"), mods: modReadonly | modDefaultLibrary})
			return
		}
		id, ok := path[len(path)-1].(*syntax.Ident)
		if !ok || id.Name == "_" {
			return
		}
		if !fc.written(id) {
			return
		}
		r := fc.resolve(path)
		if r == nil {
			return
		}
		typ, mods := classify(r)
		if typ < 0 {
			return
		}
		if r.decl == id.Start {
			mods |= modDeclaration
		}
		if f := fc.fset.File(r.decl); f != nil && len(f.Name()) > len(preludePrefix) && f.Name()[:len(preludePrefix)] == preludePrefix || r.decl == syntax.NoPos {
			mods |= modDefaultLibrary
		}
		toks = append(toks, semToken{start: fc.offset(id.Start), end: fc.offset(id.End), typ: typ, mods: mods})
	})
	sort.Slice(toks, func(i, j int) bool { return toks[i].start < toks[j].start })
	data := make([]uint32, 0, len(toks)*5)
	prevLine, prevChar, last := 0, 0, -1
	for _, t := range toks {
		if t.start < last {
			continue // overlapping: keep the first
		}
		last = t.end
		a, b := fc.m.position(t.start), fc.m.position(t.end)
		if a.Line != b.Line {
			continue
		}
		dl := a.Line - prevLine
		dc := a.Character
		if dl == 0 {
			dc = a.Character - prevChar
		}
		data = append(data, uint32(dl), uint32(dc), uint32(b.Character-a.Character), uint32(t.typ), uint32(t.mods))
		prevLine, prevChar = a.Line, a.Character
	}
	return &SemanticTokens{Data: data}, nil
}

// classify is a ref's token type and modifiers.
func classify(r *ref) (int, int) {
	switch {
	case r.style != nil:
		return tokenType("variable"), modReadonly
	case r.special == "color" || r.special == "auto":
		return tokenType("enumMember"), modDefaultLibrary
	case r.special != "":
		return tokenType("property"), modDefaultLibrary
	case r.method != nil:
		if r.method.Field {
			return tokenType("property"), modDefaultLibrary
		}
		return tokenType("method"), modDefaultLibrary
	case r.imeth != nil:
		return tokenType("method"), 0
	case r.field != nil:
		return tokenType("property"), 0
	case r.ecase != nil:
		return tokenType("enumMember"), 0
	case r.param != nil:
		return tokenType("parameter"), 0
	}
	switch o := r.obj.(type) {
	case *check.PkgName:
		return tokenType("namespace"), 0
	case *check.TypeName:
		switch o.Kind {
		case check.StructType:
			return tokenType("struct"), 0
		case check.EnumType:
			return tokenType("enum"), 0
		case check.InterfaceType:
			return tokenType("interface"), 0
		case check.ParamType:
			return tokenType("typeParameter"), 0
		case check.ComponentType, check.StoreType:
			return tokenType("class"), 0
		}
		return tokenType("type"), 0
	case *check.Var:
		mods := 0
		if !o.Mutable {
			mods = modReadonly
		}
		if o.Kind == check.SelfVar {
			return tokenType("parameter"), modReadonly
		}
		if o.Kind == check.LocalFnVar {
			return tokenType("function"), 0
		}
		return tokenType("variable"), mods
	case *check.Member:
		switch o.Kind {
		case check.ParamMember, check.PropMember:
			return tokenType("property"), modReadonly
		case check.EventMember:
			return tokenType("event"), 0
		case check.FnMember:
			return tokenType("method"), 0
		case check.LetMember, check.UseMember:
			return tokenType("variable"), modReadonly
		}
		return tokenType("variable"), 0
	case *check.Global:
		if o.Owner != nil {
			return tokenType("property"), modReadonly
		}
		if o.Kind == check.StateGlobal || o.Kind == check.VarGlobal {
			return tokenType("variable"), 0
		}
		return tokenType("variable"), modReadonly
	case *check.Function:
		if o.Recv != nil {
			return tokenType("method"), 0
		}
		return tokenType("function"), 0
	case *check.Builtin:
		return tokenType("function"), modDefaultLibrary
	}
	if r.pkg != nil {
		return tokenType("namespace"), 0
	}
	return -1, 0
}
