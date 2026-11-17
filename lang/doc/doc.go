// Package doc reads a package's API and its documentation from its
// source, as `asuka doc` shows it and the website's reference pages
// (/docs/std/<name>, /docs/platform/<name>) render it: the package's
// comment (// Package std/time is …, above its first declaration), and
// each pub declaration's signature and /// comment, in source order.
package doc

import (
	"bytes"
	"fmt"
	"os"
	"path/filepath"
	"slices"
	"sort"
	"strings"

	"github.com/egoist/asuka/lang/syntax"
	"github.com/egoist/asuka/lang/target"
)

// Package is a package's documentation.
type Package struct {
	Path string // its import path: std/time, platform/camera, ./util
	Name string // the name its declarations are qualified by: time
	// Description is its asuka.toml's description, "" for none.
	Description string
	// Doc is its package comment, without the comment marks: "Package
	// std/time is dates and times: …".
	Doc   string
	Decls []*Decl
}

// Kind is what a declaration declares.
type Kind string

const (
	Func       Kind = "fn"
	Struct     Kind = "struct"
	Enum       Kind = "enum"
	Interface  Kind = "interface"
	Component  Kind = "component"
	Shader     Kind = "shader"
	Screen     Kind = "screen"
	Store      Kind = "store"
	TypeAlias  Kind = "type"
	ExternType Kind = "extern type"
	Const      Kind = "let"
	State      Kind = "state"
	Style      Kind = "style"
	// Members.
	Method Kind = "method"
	Field  Kind = "field"
)

// Decl is a declaration: its signature as Asuka source and its ///
// documentation; a type's or a store's members (methods, a store's
// state) are its Members.
type Decl struct {
	Kind    Kind
	Name    string
	Code    string
	Doc     string
	Members []*Decl
	// Platforms are the platforms whose files declare it (tray_linux.asuka:
	// Linux), in target.All's order; nil: every platform's. A name each
	// platform's file declares is one Decl, with the first's code and the
	// first doc comment.
	Platforms []target.Platform
}

// Load reads the package in dir (its .asuka files and asuka.toml), whose
// import path is path.
func Load(dir, path string) (*Package, error) {
	files, err := filepath.Glob(filepath.Join(dir, "*.asuka"))
	if err != nil {
		return nil, err
	}
	sort.Strings(files)
	if len(files) == 0 {
		return nil, fmt.Errorf("%s: no .asuka files", dir)
	}
	srcs := map[string][]byte{}
	for _, f := range files {
		b, err := os.ReadFile(f)
		if err != nil {
			return nil, err
		}
		srcs[filepath.Base(f)] = b
	}
	p, err := FromSources(path, srcs)
	if err != nil {
		return nil, err
	}
	p.Description = manifestDescription(filepath.Join(dir, "asuka.toml"))
	return p, nil
}

// FromSources reads a package from its files' sources (name → source).
func FromSources(path string, srcs map[string][]byte) (*Package, error) {
	p := &Package{Path: path, Name: nameOf(path)}
	names := make([]string, 0, len(srcs))
	for n := range srcs {
		names = append(names, n)
	}
	sort.Strings(names)
	fset := syntax.NewFileSet()
	for _, n := range names {
		f, errs := syntax.ParseFile(fset, n, srcs[n])
		if len(errs) > 0 {
			return nil, errs
		}
		if p.Doc == "" {
			p.Doc = packageComment(f)
		}
		ps, _ := target.FileSuffix(n)
		for _, d := range f.Decls {
			dd := decl(d)
			if dd == nil {
				continue
			}
			dd.Platforms = ps
			if old := p.find(dd.Name, dd.Kind); old != nil && (old.Platforms != nil || ps != nil) {
				// A platform's: one name, on each platform that declares it.
				old.Platforms = unionPlatforms(old.Platforms, ps)
				if old.Doc == "" {
					old.Code, old.Doc, old.Members = dd.Code, dd.Doc, dd.Members
				}
				continue
			}
			p.Decls = append(p.Decls, dd)
		}
	}
	return p, nil
}

// find is the package's declaration of name, of kind; nil when none.
func (p *Package) find(name string, kind Kind) *Decl {
	for _, d := range p.Decls {
		if d.Name == name && d.Kind == kind {
			return d
		}
	}
	return nil
}

// unionPlatforms is the platforms of a or b (nil: every one), in
// target.All's order; nil when they're every one.
func unionPlatforms(a, b []target.Platform) []target.Platform {
	if a == nil || b == nil {
		return nil
	}
	var out []target.Platform
	for _, p := range target.All {
		if slices.Contains(a, p) || slices.Contains(b, p) {
			out = append(out, p)
		}
	}
	if len(out) == len(target.All) {
		return nil
	}
	return out
}

// nameOf is the name a package's declarations are qualified by: its
// path's last element.
func nameOf(path string) string {
	path = strings.TrimRight(filepath.ToSlash(path), "/")
	if i := strings.LastIndexByte(path, '/'); i >= 0 {
		return path[i+1:]
	}
	return path
}

// manifestDescription is asuka.toml's description = "…", "" for none.
func manifestDescription(file string) string {
	b, err := os.ReadFile(file)
	if err != nil {
		return ""
	}
	for _, line := range strings.Split(string(b), "\n") {
		line = strings.TrimSpace(line)
		if strings.HasPrefix(line, "[") {
			return ""
		}
		if v, ok := strings.CutPrefix(line, "description"); ok {
			v = strings.TrimSpace(v)
			if v, ok = strings.CutPrefix(v, "="); ok {
				v = strings.TrimSpace(v)
				if len(v) >= 2 && v[0] == '"' && v[len(v)-1] == '"' {
					return strings.ReplaceAll(v[1:len(v)-1], `\"`, `"`)
				}
			}
		}
	}
	return ""
}

// packageComment is the // comment above a file's first import or
// declaration that starts "Package ", without its marks.
func packageComment(f *syntax.SourceFile) string {
	first := syntax.Pos(-1)
	if len(f.Imports) > 0 {
		first = f.Imports[0].Span.Start
	} else if len(f.Decls) > 0 {
		first = f.Decls[0].Range().Start
	}
	found := func(lines []string) string {
		if len(lines) == 0 || !strings.HasPrefix(lines[0], "Package ") {
			return ""
		}
		return strings.TrimSpace(strings.Join(lines, "\n"))
	}
	var lines []string
	last := -2
	for _, c := range f.Comments {
		if first >= 0 && c.Pos >= first {
			break
		}
		line := f.File.Line(c.Pos)
		if c.IsDoc() || !strings.HasPrefix(c.Text, "//") || line != last+1 {
			if s := found(lines); s != "" {
				return s
			}
			lines = nil
		}
		last = line
		if c.IsDoc() || !strings.HasPrefix(c.Text, "//") {
			continue
		}
		lines = append(lines, strings.TrimPrefix(c.Text[2:], " "))
	}
	return found(lines)
}

// print is node as source, on one line where it can be.
func print(n syntax.Node) string {
	var b bytes.Buffer
	if err := syntax.Fprint(&b, nil, n); err != nil {
		return ""
	}
	return strings.TrimSpace(b.String())
}

// docOnly is t with only its /// comments, which the printer then
// prints with the node.
func docOnly(t syntax.Trivia) syntax.Trivia {
	if t.Doc() == "" {
		return syntax.Trivia{}
	}
	return syntax.Trivia{Leading: t.Leading[len(t.Leading)-1:]}
}

// funcCode is f's signature: async fn name<T>(params) throws -> Result.
func funcCode(f *syntax.FuncDecl) string {
	c := *f
	c.Body, c.Value, c.Native, c.Pub, c.Trivia = nil, nil, syntax.NoPos, syntax.NoPos, syntax.Trivia{}
	return print(&c)
}

// decl is d's documentation, or nil when another package can't use it.
func decl(d syntax.Decl) *Decl {
	switch x := d.(type) {
	case *syntax.FuncDecl:
		if !x.Pub.IsValid() {
			return nil
		}
		return &Decl{Kind: Func, Name: x.Name.Name, Code: funcCode(x), Doc: x.Doc()}
	case *syntax.StructDecl:
		if !x.Pub.IsValid() {
			return nil
		}
		c := *x
		c.Pub, c.Trivia, c.Dangling = syntax.NoPos, syntax.Trivia{}, nil
		out := &Decl{Kind: Struct, Name: x.Name.Name, Doc: x.Doc()}
		c.Members = nil
		for _, m := range x.Members {
			switch m := m.(type) {
			case *syntax.Field:
				f := *m
				f.Pub, f.Trivia = syntax.NoPos, docOnly(m.Trivia)
				c.Members = append(c.Members, &f)
			case *syntax.LetDecl:
				l := *m
				l.Pub, l.Trivia = syntax.NoPos, docOnly(m.Trivia)
				c.Members = append(c.Members, &l)
			case *syntax.FuncDecl:
				out.Members = append(out.Members, method(x.Name.Name, m))
			}
		}
		out.Code = print(&c)
		return out
	case *syntax.EnumDecl:
		if !x.Pub.IsValid() {
			return nil
		}
		c := *x
		c.Pub, c.Trivia, c.Dangling, c.Members = syntax.NoPos, syntax.Trivia{}, nil, nil
		out := &Decl{Kind: Enum, Name: x.Name.Name, Doc: x.Doc()}
		c.Cases = nil
		for _, k := range x.Cases {
			k2 := *k
			k2.Trivia = docOnly(k.Trivia)
			c.Cases = append(c.Cases, &k2)
		}
		for _, m := range x.Members {
			switch m := m.(type) {
			case *syntax.LetDecl:
				l := *m
				l.Pub, l.Trivia = syntax.NoPos, docOnly(m.Trivia)
				c.Members = append(c.Members, &l)
			case *syntax.FuncDecl:
				out.Members = append(out.Members, method(x.Name.Name, m))
			}
		}
		out.Code = print(&c)
		return out
	case *syntax.InterfaceDecl:
		if !x.Pub.IsValid() {
			return nil
		}
		c := *x
		c.Pub, c.Trivia, c.Dangling = syntax.NoPos, syntax.Trivia{}, nil
		c.Members = nil
		for _, m := range x.Members {
			switch m := m.(type) {
			case *syntax.FuncDecl:
				f := *m
				f.Pub, f.Trivia = syntax.NoPos, docOnly(m.Trivia)
				c.Members = append(c.Members, &f)
			case *syntax.Field:
				f := *m
				f.Pub, f.Trivia = syntax.NoPos, docOnly(m.Trivia)
				c.Members = append(c.Members, &f)
			}
		}
		return &Decl{Kind: Interface, Name: x.Name.Name, Code: print(&c), Doc: x.Doc()}
	case *syntax.ComponentDecl:
		if !x.Pub.IsValid() {
			return nil
		}
		c := *x
		c.Pub, c.Trivia, c.Body = syntax.NoPos, syntax.Trivia{}, nil
		return &Decl{Kind: Component, Name: x.Name.Name, Code: componentCode(trimBody(print(&c)), x), Doc: x.Doc()}
	case *syntax.ShaderDecl:
		if !x.Pub.IsValid() {
			return nil
		}
		c := *x
		c.Pub, c.Trivia, c.Members, c.Dangling = syntax.NoPos, syntax.Trivia{}, nil, nil
		return &Decl{Kind: Shader, Name: x.Name.Name, Code: trimBody(print(&c)), Doc: x.Doc()}
	case *syntax.ScreenDecl:
		if !x.Pub.IsValid() {
			return nil
		}
		c := *x
		c.Pub, c.Trivia, c.Body = syntax.NoPos, syntax.Trivia{}, nil
		return &Decl{Kind: Screen, Name: x.Name.Name, Code: trimBody(print(&c)), Doc: x.Doc()}
	case *syntax.StoreDecl:
		if !x.Pub.IsValid() {
			return nil
		}
		c := *x
		c.Pub, c.Trivia, c.Body = syntax.NoPos, syntax.Trivia{}, nil
		out := &Decl{Kind: Store, Name: x.Name.Name, Code: trimBody(print(&c)), Doc: x.Doc()}
		if x.Body != nil {
			for _, s := range x.Body.Items {
				if m := storeMember(x.Name.Name, s); m != nil {
					out.Members = append(out.Members, m)
				}
			}
		}
		return out
	case *syntax.TypeAliasDecl:
		if !x.Pub.IsValid() {
			return nil
		}
		c := *x
		c.Pub, c.Trivia = syntax.NoPos, syntax.Trivia{}
		return &Decl{Kind: TypeAlias, Name: x.Name.Name, Code: print(&c), Doc: x.Doc()}
	case *syntax.ExternTypeDecl:
		if !x.Pub.IsValid() {
			return nil
		}
		c := *x
		c.Pub, c.Trivia = syntax.NoPos, syntax.Trivia{}
		return &Decl{Kind: ExternType, Name: x.Name.Name, Code: print(&c), Doc: x.Doc()}
	case *syntax.LetDecl:
		if !x.Pub.IsValid() {
			return nil
		}
		c := *x
		c.Pub, c.Trivia, c.Else = syntax.NoPos, syntax.Trivia{}, nil
		return &Decl{Kind: Const, Name: patternName(x.Pattern), Code: print(&c), Doc: x.Doc()}
	case *syntax.StateDecl:
		if !x.Pub.IsValid() {
			return nil
		}
		c := *x
		c.Pub, c.Trivia = syntax.NoPos, syntax.Trivia{}
		return &Decl{Kind: State, Name: x.Name.Name, Code: print(&c), Doc: x.Doc()}
	case *syntax.StyleDecl:
		if !x.Pub.IsValid() {
			return nil
		}
		c := *x
		c.Pub, c.Trivia = syntax.NoPos, syntax.Trivia{}
		return &Decl{Kind: Style, Name: x.Name.Name, Code: print(&c), Doc: x.Doc()}
	}
	return nil
}

// componentCode is a component's header with what its caller gives and
// gets: its props (slots among them) and events, and the variants it
// declares pub (their names: a caller styles them, name { … }), each
// with its /// comment.
func componentCode(header string, x *syntax.ComponentDecl) string {
	if x.Body == nil {
		return header
	}
	var b strings.Builder
	n := 0
	line := func(doc, code string) {
		if n == 0 {
			b.WriteString(header + " {\n")
		} else if doc != "" {
			b.WriteString("\n")
		}
		n++
		for _, l := range strings.Split(doc, "\n") {
			if doc != "" {
				b.WriteString("  /// " + l + "\n")
			}
		}
		b.WriteString("  " + code + "\n")
	}
	for _, it := range x.Body.Items {
		switch m := it.(type) {
		case *syntax.PropDecl:
			c := *m
			c.Pub, c.Trivia = syntax.NoPos, syntax.Trivia{}
			line(m.Doc(), print(&c))
		case *syntax.EventDecl:
			c := *m
			c.Pub, c.Trivia = syntax.NoPos, syntax.Trivia{}
			line(m.Doc(), print(&c))
		case *syntax.VariantDecl:
			if m.Pub.IsValid() {
				line(m.Doc(), "variant "+m.Name.Name)
			}
		}
	}
	if n == 0 {
		return header
	}
	b.WriteString("}")
	return b.String()
}

// method is a type's method: its name is Type.name.
func method(typ string, f *syntax.FuncDecl) *Decl {
	return &Decl{Kind: Method, Name: typ + "." + f.Name.Name, Code: funcCode(f), Doc: f.Doc()}
}

// storeMember is a store's pub member (state, let, fn), or nil.
func storeMember(store string, s syntax.Node) *Decl {
	switch m := s.(type) {
	case *syntax.FuncDecl:
		if m.Pub.IsValid() {
			return method(store, m)
		}
	case *syntax.StateDecl:
		if m.Pub.IsValid() {
			c := *m
			c.Pub, c.Trivia = syntax.NoPos, syntax.Trivia{}
			return &Decl{Kind: State, Name: store + "." + m.Name.Name, Code: print(&c), Doc: m.Doc()}
		}
	case *syntax.LetDecl:
		if m.Pub.IsValid() {
			c := *m
			c.Pub, c.Trivia, c.Else = syntax.NoPos, syntax.Trivia{}, nil
			return &Decl{Kind: Const, Name: store + "." + patternName(m.Pattern), Code: print(&c), Doc: m.Doc()}
		}
	}
	return nil
}

func patternName(p syntax.Pattern) string {
	s := print(p)
	if i := strings.IndexAny(s, ":= "); i > 0 {
		s = s[:i]
	}
	return s
}

// trimBody drops the {} a declaration printed without its body ends in.
func trimBody(s string) string { return strings.TrimSuffix(strings.TrimSpace(s), " {}") }

// Lookup is the declaration named name (Time, Time.add, now), or nil.
func (p *Package) Lookup(name string) *Decl {
	for _, d := range p.Decls {
		if d.Name == name {
			return d
		}
		for _, m := range d.Members {
			if m.Name == name {
				return m
			}
		}
	}
	return nil
}
