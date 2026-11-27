package lsp

import (
	"bytes"
	"path/filepath"
	"sort"
	"strings"
	"unicode/utf8"

	"github.com/egoist/asuka/lang/syntax"
)

// ---------------------------------------------------------------------
// Formatting

func (s *server) formatting(p *DocumentFormattingParams) ([]TextEdit, error) {
	path := uriToPath(p.TextDocument.URI)
	src, err := s.readFile(path)
	if err != nil {
		return nil, nil
	}
	out, err := syntax.Format(path, src)
	if err != nil {
		// Syntax errors: the diagnostics say where; nothing to format.
		return nil, nil
	}
	if bytes.Equal(src, out) {
		return []TextEdit{}, nil
	}
	return []TextEdit{minimalEdit(newMapper(src, s.utf8), src, out)}, nil
}

// minimalEdit replaces only what differs between src and out: the
// editor keeps the cursor and the scroll where nothing changed.
func minimalEdit(m *mapper, src, out []byte) TextEdit {
	pre := 0
	for pre < len(src) && pre < len(out) && src[pre] == out[pre] {
		pre++
	}
	for pre > 0 && pre < len(src) && !utf8.RuneStart(src[pre]) {
		pre--
	}
	suf := 0
	for suf < len(src)-pre && suf < len(out)-pre && src[len(src)-1-suf] == out[len(out)-1-suf] {
		suf++
	}
	for suf > 0 && len(src)-suf < len(src) && !utf8.RuneStart(src[len(src)-suf]) {
		suf--
	}
	return TextEdit{Range: m.rng(pre, len(src)-suf), NewText: string(out[pre : len(out)-suf])}
}

// ---------------------------------------------------------------------
// Symbols

func (s *server) documentSymbol(p *DocumentSymbolParams) ([]DocumentSymbol, error) {
	path := uriToPath(p.TextDocument.URI)
	src, err := s.readFile(path)
	if err != nil {
		if src, err = s.source(path); err != nil {
			return nil, nil
		}
	}
	f, _ := syntax.ParseFile(syntax.NewFileSet(), path, src)
	m := newMapper(src, s.utf8)
	sb := &symbolBuilder{file: f.File, m: m}
	syms := []DocumentSymbol{}
	for _, d := range f.Decls {
		if sym, ok := sb.decl(d); ok {
			syms = append(syms, sym)
		}
	}
	return syms, nil
}

type symbolBuilder struct {
	file *syntax.File
	m    *mapper
}

func (sb *symbolBuilder) rng(sp syntax.Span) Range {
	b := sb.file.Base()
	end := int(sp.End) - b
	if end > len(sb.m.src) {
		end = len(sb.m.src)
	}
	return sb.m.rng(int(sp.Start)-b, end)
}

func (sb *symbolBuilder) sym(name string, kind int, detail string, whole syntax.Node, id *syntax.Ident) DocumentSymbol {
	sel := whole.Range()
	if id != nil && id.Start.IsValid() {
		sel = id.Span
	}
	return DocumentSymbol{Name: name, Kind: kind, Detail: detail, Range: sb.rng(whole.Range()), SelectionRange: sb.rng(sel)}
}

func (sb *symbolBuilder) decl(d syntax.Node) (DocumentSymbol, bool) {
	switch d := d.(type) {
	case *syntax.ComponentDecl:
		kw := "component"
		if d.Native.IsValid() {
			kw = "native component"
		}
		s := sb.sym(d.Name.Name, symClass, kw, d, d.Name)
		s.Children = sb.block(d.Body)
		return s, true
	case *syntax.ShaderDecl:
		return sb.sym(d.Name.Name, symClass, "shader", d, d.Name), true
	case *syntax.ScreenDecl:
		s := sb.sym(d.Name.Name, symClass, "screen", d, d.Name)
		s.Children = sb.block(d.Body)
		return s, true
	case *syntax.StoreDecl:
		s := sb.sym(d.Name.Name, symClass, "store", d, d.Name)
		s.Children = sb.block(d.Body)
		return s, true
	case *syntax.WorkerDecl:
		s := sb.sym(d.Name.Name, symClass, "worker", d, d.Name)
		s.Children = sb.block(d.Body)
		return s, true
	case *syntax.StructDecl:
		kw := "struct"
		if d.Extern.IsValid() {
			kw = "extern struct"
		}
		s := sb.sym(d.Name.Name, symStruct, kw, d, d.Name)
		for _, m := range d.Members {
			if c, ok := sb.member(m); ok {
				s.Children = append(s.Children, c)
			}
		}
		return s, true
	case *syntax.EnumDecl:
		s := sb.sym(d.Name.Name, symEnum, "enum", d, d.Name)
		for _, k := range d.Cases {
			s.Children = append(s.Children, sb.sym(k.Name.Name, symEnumMember, "", k, k.Name))
		}
		for _, m := range d.Members {
			if c, ok := sb.member(m); ok {
				s.Children = append(s.Children, c)
			}
		}
		return s, true
	case *syntax.InterfaceDecl:
		kw := "interface"
		if d.Native.IsValid() {
			kw = "native interface"
		}
		s := sb.sym(d.Name.Name, symInterface, kw, d, d.Name)
		for _, m := range d.Members {
			if c, ok := sb.member(m); ok {
				s.Children = append(s.Children, c)
			}
		}
		return s, true
	case *syntax.FuncDecl:
		return sb.sym(d.Name.Name, symFunction, funcHeader(d), d, d.Name), true
	case *syntax.TypeAliasDecl:
		return sb.sym(d.Name.Name, symClass, "type", d, d.Name), true
	case *syntax.ExternTypeDecl:
		return sb.sym(d.Name.Name, symClass, "extern type", d, d.Name), true
	case *syntax.StyleDecl:
		return sb.sym(d.Name.Name, symProperty, "style", d, d.Name), true
	case *syntax.StateDecl:
		return sb.sym(d.Name.Name, symVariable, "state", d, d.Name), true
	case *syntax.LetDecl:
		if bp, ok := d.Pattern.(*syntax.BindPat); ok {
			return sb.sym(bp.Name.Name, symConstant, "let", d, bp.Name), true
		}
	case *syntax.AppDecl:
		s := sb.sym("app", symModule, "", d, nil)
		return s, true
	case *syntax.TestDecl:
		name := "test"
		if d.Name != nil {
			name = "test " + strings.Join(d.Name.Segments, "")
		}
		return sb.sym(name, symFunction, "", d, nil), true
	case *syntax.BenchDecl:
		name := "bench"
		if d.Name != nil {
			name = "bench " + strings.Join(d.Name.Segments, "")
		}
		return sb.sym(name, symFunction, "", d, nil), true
	case *syntax.PreviewDecl:
		return sb.sym("preview "+previewName(d), symFunction, "", d, nil), true
	}
	return DocumentSymbol{}, false
}

func (sb *symbolBuilder) member(m syntax.Node) (DocumentSymbol, bool) {
	switch m := m.(type) {
	case *syntax.Field:
		return sb.sym(m.Name.Name, symField, typeSrc(m.Type), m, m.Name), true
	case *syntax.FuncDecl:
		return sb.sym(m.Name.Name, symMethod, funcHeader(m), m, m.Name), true
	case *syntax.LetDecl:
		if bp, ok := m.Pattern.(*syntax.BindPat); ok {
			return sb.sym(bp.Name.Name, symConstant, "let", m, bp.Name), true
		}
	}
	return DocumentSymbol{}, false
}

// block lists a component's or a store's members.
func (sb *symbolBuilder) block(b *syntax.Block) []DocumentSymbol {
	if b == nil {
		return nil
	}
	var out []DocumentSymbol
	for _, it := range b.Items {
		switch d := it.(type) {
		case *syntax.PropDecl:
			out = append(out, sb.sym(d.Name.Name, symProperty, "prop", d, d.Name))
		case *syntax.EventDecl:
			out = append(out, sb.sym(d.Name.Name, symEvent, "event", d, d.Name))
		case *syntax.VariantDecl:
			out = append(out, sb.sym(d.Name.Name, symProperty, "variant", d, d.Name))
		case *syntax.StateDecl:
			out = append(out, sb.sym(d.Name.Name, symVariable, "state", d, d.Name))
		case *syntax.LetDecl:
			if bp, ok := d.Pattern.(*syntax.BindPat); ok {
				out = append(out, sb.sym(bp.Name.Name, symVariable, d.Keyword.String(), d, bp.Name)) // let, or a worker's var
			}
		case *syntax.UseDecl:
			out = append(out, sb.sym(d.Name.Name, symVariable, "use", d, d.Name))
		case *syntax.FuncDecl:
			out = append(out, sb.sym(d.Name.Name, symMethod, funcHeader(d), d, d.Name))
		case *syntax.StyleDecl:
			out = append(out, sb.sym(d.Name.Name, symProperty, "style", d, d.Name))
		}
	}
	return out
}

// funcHeader is a function's header as source: fn f(a: int) -> int.
func funcHeader(d *syntax.FuncDecl) string {
	c := *d
	c.Trivia = syntax.Trivia{}
	c.Body, c.Value = nil, nil
	var b bytes.Buffer
	ok := func() (ok bool) {
		defer func() {
			if recover() != nil {
				ok = false
			}
		}()
		return syntax.Fprint(&b, nil, &c) == nil
	}()
	if !ok {
		return "fn"
	}
	return strings.TrimSpace(b.String())
}

func typeSrc(t syntax.Type) string {
	if t == nil || isNil(t) {
		return ""
	}
	var b bytes.Buffer
	ok := func() (ok bool) {
		defer func() {
			if recover() != nil {
				ok = false
			}
		}()
		return syntax.Fprint(&b, nil, t) == nil
	}()
	if !ok {
		return ""
	}
	return strings.TrimSpace(b.String())
}

func (s *server) workspaceSymbol(p *WorkspaceSymbolParams) ([]SymbolInformation, error) {
	q := strings.ToLower(p.Query)
	out := []SymbolInformation{}
	for _, dir := range s.workspaceDirs() {
		files, _ := filepath.Glob(filepath.Join(dir, "*.asuka"))
		for path := range s.docs {
			if filepath.Dir(path) == dir && !contains(files, path) {
				files = append(files, path)
			}
		}
		sort.Strings(files)
		for _, path := range files {
			src, err := s.readFile(path)
			if err != nil {
				continue
			}
			f, _ := syntax.ParseFile(syntax.NewFileSet(), path, src)
			sb := &symbolBuilder{file: f.File, m: newMapper(src, s.utf8)}
			uri := s.uri(path)
			var visit func(syms []DocumentSymbol, container string)
			visit = func(syms []DocumentSymbol, container string) {
				for _, sym := range syms {
					if fuzzy(strings.ToLower(sym.Name), q) {
						out = append(out, SymbolInformation{Name: sym.Name, Kind: sym.Kind, Location: Location{URI: uri, Range: sym.SelectionRange}, ContainerName: container})
					}
					visit(sym.Children, sym.Name)
				}
			}
			for _, d := range f.Decls {
				if sym, ok := sb.decl(d); ok {
					visit([]DocumentSymbol{sym}, "")
				}
			}
			if len(out) > 1000 {
				return out[:1000], nil
			}
		}
	}
	return out, nil
}

func contains(list []string, x string) bool {
	for _, y := range list {
		if y == x {
			return true
		}
	}
	return false
}

// fuzzy reports whether q's characters come in s in order.
func fuzzy(s, q string) bool {
	i := 0
	for j := 0; j < len(s) && i < len(q); j++ {
		if s[j] == q[i] {
			i++
		}
	}
	return i == len(q)
}
