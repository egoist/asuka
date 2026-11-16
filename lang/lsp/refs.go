package lsp

import (
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"sort"
	"strings"

	"github.com/egoist/asuka/lang/check"
	"github.com/egoist/asuka/lang/syntax"
)

// walkPaths calls f with the path to each node under n, depth first, in
// source order. f mustn't keep path.
func walkPaths(n syntax.Node, f func(path []syntax.Node)) {
	var walk func(n syntax.Node, path []syntax.Node, depth int)
	walk = func(n syntax.Node, path []syntax.Node, depth int) {
		if depth > 5000 {
			return
		}
		path = append(path, n)
		f(path)
		for _, k := range children(n) {
			walk(k, path, depth+1)
		}
	}
	walk(n, nil, 0)
}

// An occurrence is a name that refers to a declaration.
type occurrence struct {
	loc  Location
	decl bool // the declaration's own name
}

// occurrences finds the names in fc's file that refer to what key names.
func (fc *fileCtx) occurrences(key declKey, out *[]occurrence) {
	walkPaths(fc.file, func(path []syntax.Node) {
		id, ok := path[len(path)-1].(*syntax.Ident)
		if !ok || id.Name == "_" || fc.textOf(id.Span) != id.Name {
			return
		}
		if imp, ok := at(path, 2).(*syntax.ImportDecl); ok && imp.Alias != id {
			// An import's path names the package, but isn't a use of
			// its name.
			isName := false
			for _, n := range imp.Names {
				isName = isName || n == id
			}
			if !isName {
				return
			}
		}
		r := fc.resolve(path)
		if r == nil {
			return
		}
		k, ok := fc.key(r)
		if !ok || k != key {
			return
		}
		decl := false
		if f := fc.fset.File(r.decl); f != nil && f == fc.file.File {
			decl = r.decl == id.Start || r.pkg != nil && at(path, 2) != nil && isImportAlias(at(path, 2), id)
		}
		*out = append(*out, occurrence{loc: Location{URI: fc.s.uri(fc.path), Range: fc.rng(id.Span)}, decl: decl})
	})
}

func isImportAlias(n syntax.Node, id *syntax.Ident) bool {
	imp, ok := n.(*syntax.ImportDecl)
	return ok && imp.Alias == id
}

// targetAt is what the name at a position refers to.
func (s *server) targetAt(p *TextDocumentPositionParams) (*fileCtx, *syntax.Ident, *ref, []syntax.Node) {
	fc, err := s.fileAt(p.TextDocument.URI)
	if err != nil {
		return nil, nil, nil, nil
	}
	path, id := fc.identAt(fc.pos(p.Position))
	if id == nil {
		return fc, nil, nil, path
	}
	return fc, id, fc.resolve(path), path
}

// findOccurrences finds what refers to r, across the packages that can
// see it.
func (s *server) findOccurrences(fc *fileCtx, r *ref) []occurrence {
	key, ok := fc.key(r)
	if !ok {
		return nil
	}
	var out []occurrence
	// A local: its file.
	if v, ok := r.obj.(*check.Var); ok && v.Ctx != nil || r.obj != nil && isTypeParam(r.obj) {
		fc.occurrences(key, &out)
		return out
	}
	dirs := []string{fc.st.dir}
	if filepath.IsAbs(key.file) {
		dirs = append(dirs, filepath.Dir(key.file))
	}
	if !s.private(r) {
		dirs = append(dirs, s.workspaceDirs()...)
	}
	seen := map[string]bool{}
	for _, dir := range dirs {
		if seen[dir] {
			continue
		}
		seen[dir] = true
		st := s.check(dir)
		if st == nil || st.root() == nil {
			continue
		}
		// Every platform's files (tray_linux.asuka in Linux's check).
		scanned := map[string]bool{}
		for _, o := range st.platformStates() {
			if o.root() == nil {
				continue
			}
			for _, f := range o.root().Files {
				if scanned[f.File.Name()] {
					continue
				}
				scanned[f.File.Name()] = true
				ofc, err := s.fileIn(o, f)
				if err != nil {
					continue
				}
				ofc.occurrences(key, &out)
			}
		}
	}
	// One file read by two checks (a package and its importer, checked
	// as roots) is scanned once each: keep each place once.
	uniq := out[:0]
	have := map[Location]bool{}
	for _, o := range out {
		if !have[o.loc] {
			have[o.loc] = true
			uniq = append(uniq, o)
		}
	}
	sort.SliceStable(uniq, func(i, j int) bool {
		a, b := uniq[i].loc, uniq[j].loc
		if a.URI != b.URI {
			return a.URI < b.URI
		}
		if a.Range.Start.Line != b.Range.Start.Line {
			return a.Range.Start.Line < b.Range.Start.Line
		}
		return a.Range.Start.Character < b.Range.Start.Character
	})
	return uniq
}

func isTypeParam(o check.Object) bool {
	tn, ok := o.(*check.TypeName)
	return ok && tn.Kind == check.ParamType
}

// private reports whether only r's own package can refer to it.
func (s *server) private(r *ref) bool {
	switch o := r.obj.(type) {
	case *check.TypeName:
		return o.Pkg != nil && !o.Pkg.Prelude && !check.Exported(o.Pkg, o)
	case *check.Function:
		return o.Pkg != nil && o.Recv == nil && !check.Exported(o.Pkg, o)
	case *check.Global:
		return o.Pkg != nil && o.Owner == nil && !check.Exported(o.Pkg, o)
	case *check.PkgName:
		return true // the file's import
	}
	return false
}

// fileIn is a file of a checked package.
func (s *server) fileIn(st *pkgState, f *syntax.SourceFile) (*fileCtx, error) {
	name := f.File.Name()
	src := st.srcs[name]
	if src == nil {
		var err error
		if src, err = s.source(name); err != nil {
			return nil, err
		}
	}
	prog := st.res.Program
	return &fileCtx{s: s, st: st, path: name, name: name, file: f, pkg: st.pkgOf[f], prog: prog, info: prog.Info, fset: prog.Fset, m: newMapper(src, s.utf8)}, nil
}

// workspaceDirs are the directories with .asuka files under the
// workspace's folders.
func (s *server) workspaceDirs() []string {
	if s.wsDirs != nil {
		return s.wsDirs
	}
	dirs := []string{}
	seen := map[string]bool{}
	for _, root := range s.roots {
		filepath.WalkDir(root, func(p string, d fs.DirEntry, err error) error {
			if err != nil {
				return nil
			}
			if d.IsDir() {
				name := d.Name()
				if p != root && (strings.HasPrefix(name, ".") || name == "node_modules") {
					return filepath.SkipDir
				}
				return nil
			}
			if strings.HasSuffix(p, ".asuka") {
				dir := filepath.Dir(p)
				if !seen[dir] && len(dirs) < 2000 {
					seen[dir] = true
					dirs = append(dirs, dir)
				}
			}
			return nil
		})
	}
	for p := range s.docs {
		if dir := filepath.Dir(p); !seen[dir] && !isPreludeCopy(dir) {
			seen[dir] = true
			dirs = append(dirs, dir)
		}
	}
	s.wsDirs = dirs
	return dirs
}

func (s *server) references(p *ReferenceParams) ([]Location, error) {
	fc, _, r, _ := s.targetAt(&p.TextDocumentPositionParams)
	if r == nil {
		return nil, nil
	}
	locs := []Location{}
	for _, o := range s.findOccurrences(fc, r) {
		if o.decl && !p.Context.IncludeDeclaration {
			continue
		}
		locs = append(locs, o.loc)
	}
	return locs, nil
}

func (s *server) documentHighlight(p *TextDocumentPositionParams) ([]DocumentHighlight, error) {
	fc, _, r, _ := s.targetAt(p)
	if r == nil {
		return nil, nil
	}
	key, ok := fc.key(r)
	if !ok {
		return nil, nil
	}
	var out []occurrence
	fc.occurrences(key, &out)
	hs := []DocumentHighlight{}
	for _, o := range out {
		kind := 2
		if o.decl {
			kind = 3
		}
		hs = append(hs, DocumentHighlight{Range: o.loc.Range, Kind: kind})
	}
	return hs, nil
}

// renamable says why r can't be renamed, "" when it can.
func (s *server) renamable(fc *fileCtx, r *ref) string {
	if r == nil {
		return "nothing to rename here"
	}
	if !r.decl.IsValid() || r.method != nil || r.special != "" {
		return r.name + " is built into the language"
	}
	f := fc.fset.File(r.decl)
	if f == nil {
		return "can't find " + r.name + "'s declaration"
	}
	name := f.Name()
	if strings.HasPrefix(name, preludePrefix) {
		return r.name + " is the prelude's: it can't be renamed"
	}
	if _, open := s.docs[name]; !open && !s.inWorkspace(name) {
		return r.name + " is declared outside the workspace (" + name + ")"
	}
	if r.pkg != nil {
		if _, ok := r.obj.(*check.PkgName); ok || r.obj == nil {
			if !s.hasAlias(fc, r) {
				return "an import is named by its path: give it a name to rename (import " + pkgPath(r.pkg) + " as name)"
			}
		}
	}
	return ""
}

func (s *server) hasAlias(fc *fileCtx, r *ref) bool {
	f := fc.fset.File(r.decl)
	if f == nil {
		return false
	}
	sf := fc.st.files[f.Name()]
	if sf == nil {
		return false
	}
	for _, imp := range sf.Imports {
		if imp.Start == r.decl {
			return imp.Alias != nil
		}
	}
	return false
}

func (s *server) inWorkspace(path string) bool {
	for _, root := range s.roots {
		if rel, err := filepath.Rel(root, path); err == nil && !strings.HasPrefix(rel, "..") {
			return true
		}
	}
	return false
}

func (s *server) prepareRename(p *TextDocumentPositionParams) (*PrepareRenameResult, error) {
	fc, id, r, _ := s.targetAt(p)
	if id == nil {
		return nil, nil
	}
	if why := s.renamable(fc, r); why != "" {
		return nil, &rpcError{Code: codeRequestFailed, Message: why}
	}
	return &PrepareRenameResult{Range: fc.rng(id.Span), Placeholder: id.Name}, nil
}

func (s *server) rename(p *RenameParams) (*WorkspaceEdit, error) {
	fc, id, r, _ := s.targetAt(&p.TextDocumentPositionParams)
	if id == nil {
		return nil, &rpcError{Code: codeRequestFailed, Message: "no name here to rename"}
	}
	if why := s.renamable(fc, r); why != "" {
		return nil, &rpcError{Code: codeRequestFailed, Message: why}
	}
	if !syntax.IsIdentifier(p.NewName) {
		return nil, &rpcError{Code: codeInvalidParams, Message: fmt.Sprintf("%q isn't a name: a letter or _, then letters, digits and _, and not a keyword", p.NewName)}
	}
	edit := &WorkspaceEdit{Changes: map[string][]TextEdit{}}
	for _, o := range s.findOccurrences(fc, r) {
		edit.Changes[o.loc.URI] = append(edit.Changes[o.loc.URI], TextEdit{Range: o.loc.Range, NewText: p.NewName})
	}
	return edit, nil
}

// readFile is a file's text, the editor's when it's open.
func (s *server) readFile(path string) ([]byte, error) {
	if d := s.docs[path]; d != nil {
		return d.text, nil
	}
	return os.ReadFile(path)
}
