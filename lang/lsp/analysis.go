package lsp

import (
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"

	"github.com/egoist/asuka/internal/pkgs"
	"github.com/egoist/asuka/lang/check"
	"github.com/egoist/asuka/lang/compile"
	"github.com/egoist/asuka/lang/prelude"
	"github.com/egoist/asuka/lang/syntax"
	"github.com/egoist/asuka/lang/target"
)

// A pkgState is what checking a package found, and the text it read.
type pkgState struct {
	dir  string
	res  *compile.Result // nil when the checker failed
	err  error           // why there's no result
	gens map[string]int  // each directory read, and its edit count then

	files map[string]*syntax.SourceFile // every file the check parsed (imports' and the prelude's too), by name
	pkgOf map[*syntax.SourceFile]*check.Package
	srcs  map[string][]byte // the root package's files' text, as checked

	published map[string]bool // the files diagnostics were published for

	// platform is the platform it's checked for. A package whose
	// program has platforms' files (tray_linux.asuka) is checked for
	// target.Default and, in others, for each other platform: its
	// diagnostics are every platform's, and a file of a platform's is
	// read in a check for that platform (fileAt).
	platform target.Platform
	others   []*pkgState
}

// platformStates are st and its checks for other platforms.
func (st *pkgState) platformStates() []*pkgState {
	return append([]*pkgState{st}, st.others...)
}

// root is the package checked, nil without a result.
func (st *pkgState) root() *check.Package {
	if st.res == nil || st.res.Program == nil {
		return nil
	}
	return st.res.Program.Root
}

// fresh reports whether nothing st read has changed since.
func (s *server) fresh(st *pkgState) bool {
	for d, g := range st.gens {
		if s.gens[d] != g {
			return false
		}
	}
	return true
}

// check checks the package in dir, unless what checked it last is still
// fresh.
func (s *server) check(dir string) *pkgState {
	if st := s.pkgs[dir]; st != nil && s.fresh(st) {
		return st
	}
	if isPreludeCopy(dir) {
		return s.preludeState(dir)
	}
	old := s.pkgs[dir]
	ov := s.overlay()
	st := s.checkFor(dir, target.Default, ov)
	if old != nil {
		st.published = old.published
	}
	if st.res != nil && st.res.PlatformFiles {
		for _, p := range target.All {
			if p == target.Default {
				continue
			}
			o := s.checkFor(dir, p, ov)
			for d, g := range o.gens {
				st.gens[d] = g
			}
			st.others = append(st.others, o)
		}
	}
	s.pkgs[dir] = st
	return st
}

// checkFor checks the package in dir for platform.
func (s *server) checkFor(dir string, platform target.Platform, ov map[string][]byte) *pkgState {
	st := &pkgState{dir: dir, platform: platform, gens: map[string]int{dir: s.gens[dir]}, files: map[string]*syntax.SourceFile{}, pkgOf: map[*syntax.SourceFile]*check.Package{}, srcs: map[string][]byte{}}
	var res *compile.Result
	var err error
	if !s.safely("checking "+dir, func() {
		res, err = compile.Dir(dir, &compile.Options{Platform: platform, Overlay: ov, CheckOnly: true, Resolve: s.resolver(dir)})
	}) {
		res, err = nil, errors.New("the checker failed on this package (see the server's log)")
	}
	st.res, st.err = res, err
	if res != nil && res.Program != nil {
		for _, p := range res.Program.Packages {
			if p.Dir != "" && !strings.HasPrefix(p.Dir, "pkg:") {
				st.gens[p.Dir] = s.gens[p.Dir]
			}
			for _, f := range p.Files {
				if f != nil && f.File != nil {
					st.files[f.File.Name()] = f
					st.pkgOf[f] = p
				}
			}
		}
		for _, f := range res.Program.Root.Files {
			name := f.File.Name()
			if src, ok := ov[name]; ok {
				st.srcs[name] = src
			} else if src, err := os.ReadFile(name); err == nil {
				st.srcs[name] = src
			}
		}
	}
	return st
}

// resolver resolves dependencies through the asuka.lock of the project
// dir is in (a package of it may be in a subdirectory); nil without one.
func (s *server) resolver(dir string) func(from, path string) (string, string, error) {
	root := projectRoot(dir)
	if root == "" {
		return nil
	}
	im := s.imports[root]
	if im == nil {
		im = pkgs.NewImports(root)
		s.imports[root] = im
	}
	return im.Resolve
}

// publish sends the client a package's diagnostics, file by file, and
// clears them for files that have none any more.
func (s *server) publish(st *pkgState) {
	byFile := map[string][]check.Diagnostic{}
	warnings := map[string][]check.Diagnostic{}
	var files []string
	if root := st.root(); root != nil {
		// Every platform's files, and their diagnostics: once each, and
		// saying the platforms when not every one's.
		for _, o := range st.platformStates() {
			if r := o.root(); r != nil {
				for _, f := range r.Files {
					if _, ok := byFile[f.File.Name()]; !ok {
						files = append(files, f.File.Name())
						byFile[f.File.Name()] = nil
					}
				}
			}
		}
		diags, warns := platformDiags(st)
		for _, d := range diags {
			if _, ok := byFile[d.Pos.Filename]; ok {
				byFile[d.Pos.Filename] = append(byFile[d.Pos.Filename], d)
			}
		}
		for _, d := range warns {
			if _, ok := byFile[d.Pos.Filename]; ok {
				warnings[d.Pos.Filename] = append(warnings[d.Pos.Filename], d)
			}
		}
	} else {
		// No result: the files' syntax errors, at least.
		for p, d := range s.docs {
			if filepath.Dir(p) != st.dir {
				continue
			}
			files = append(files, p)
			_, errs := syntax.ParseFile(syntax.NewFileSet(), p, d.text)
			for _, e := range errs {
				byFile[p] = append(byFile[p], check.Diagnostic{Pos: e.Pos, End: e.End, Msg: e.Msg})
			}
			if st.err != nil && len(errs) == 0 {
				byFile[p] = append(byFile[p], check.Diagnostic{Pos: syntax.Position{Filename: p, Line: 1, Column: 1}, Msg: st.err.Error()})
			}
		}
	}
	sort.Strings(files)
	now := map[string]bool{}
	for _, name := range files {
		src := st.srcs[name]
		if src == nil {
			src, _ = s.source(name)
		}
		m := newMapper(src, s.utf8)
		diags := []Diagnostic{}
		for _, d := range byFile[name] {
			diags = append(diags, Diagnostic{Range: diagRange(m, d), Severity: 1, Source: "asuka", Message: d.Msg})
		}
		for _, d := range warnings[name] {
			diags = append(diags, Diagnostic{Range: diagRange(m, d), Severity: 2, Source: "asuka", Message: d.Msg})
		}
		params := &PublishDiagnosticsParams{URI: s.uri(name), Diagnostics: diags}
		if doc := s.docs[name]; doc != nil {
			v := doc.version
			params.Version = &v
		}
		s.conn.notify("textDocument/publishDiagnostics", params)
		now[name] = true
	}
	for name := range st.published {
		if !now[name] {
			s.conn.notify("textDocument/publishDiagnostics", &PublishDiagnosticsParams{URI: s.uri(name), Diagnostics: []Diagnostic{}})
		}
	}
	st.published = now
}

// platformDiags are st's diagnostics and warnings and its other
// platforms' checks' (when it has them), each once, a message saying the
// platforms it's for when not every one checked.
func platformDiags(st *pkgState) (diags, warns []check.Diagnostic) {
	states := st.platformStates()
	if len(states) == 1 {
		return st.res.Diags, st.res.Warnings
	}
	merge := func(get func(*compile.Result) []check.Diagnostic) []check.Diagnostic {
		var out []check.Diagnostic
		var ps [][]target.Platform
		idx := map[string]int{}
		checked := 0
		for _, o := range states {
			if o.res == nil {
				continue
			}
			checked++
			for _, d := range get(o.res) {
				k := d.String()
				if i, ok := idx[k]; ok {
					ps[i] = append(ps[i], o.platform)
					continue
				}
				idx[k] = len(out)
				out = append(out, d)
				ps = append(ps, []target.Platform{o.platform})
			}
		}
		for i := range out {
			if len(ps[i]) < checked {
				sort.Slice(ps[i], func(a, b int) bool { return platformIndex(ps[i][a]) < platformIndex(ps[i][b]) })
				out[i].Msg += " (for " + target.List(ps[i]) + ")"
			}
		}
		return out
	}
	return merge(func(r *compile.Result) []check.Diagnostic { return r.Diags }), merge(func(r *compile.Result) []check.Diagnostic { return r.Warnings })
}

func platformIndex(p target.Platform) int {
	for i, q := range target.All {
		if q == p {
			return i
		}
	}
	return len(target.All)
}

// publishNone clears a package's diagnostics.
func (s *server) publishNone(st *pkgState) {
	for name := range st.published {
		s.conn.notify("textDocument/publishDiagnostics", &PublishDiagnosticsParams{URI: s.uri(name), Diagnostics: []Diagnostic{}})
	}
	st.published = nil
}

// diagRange is a diagnostic's range; one that's a point covers the word
// (or the character) there, so the editor has something to underline.
func diagRange(m *mapper, d check.Diagnostic) Range {
	start := d.Pos.Offset
	if !d.Pos.IsValid() {
		start = 0
	}
	end := start
	if d.End.IsValid() && d.End.Filename == d.Pos.Filename && d.End.Offset > start {
		end = d.End.Offset
	}
	if end == start {
		end = wordEnd(m.src, start)
		if end == start && start < len(m.src) && m.src[start] != '\n' {
			end = start + 1
		}
	}
	return m.rng(start, end)
}

func wordEnd(src []byte, i int) int {
	for i < len(src) && isWordByte(src[i]) {
		i++
	}
	return i
}

func isWordByte(b byte) bool {
	return b == '_' || '0' <= b && b <= '9' || 'a' <= b && b <= 'z' || 'A' <= b && b <= 'Z' || b >= 0x80
}

// ---------------------------------------------------------------------
// A file in a checked package

// A fileCtx is a file in what checking its package found.
type fileCtx struct {
	s    *server
	st   *pkgState
	path string
	name string // the file's name in the check (the prelude's: prelude/…)
	file *syntax.SourceFile
	pkg  *check.Package
	prog *check.Program
	info *check.Info
	fset *syntax.FileSet
	m    *mapper
}

var errNoFile = errors.New("the file isn't part of a checked package")

// fileAt checks the package of the file at uri (when it isn't fresh)
// and returns the file in it.
func (s *server) fileAt(uri string) (*fileCtx, error) {
	path := uriToPath(uri)
	if path == "" || !strings.HasSuffix(path, ".asuka") {
		return nil, errNoFile
	}
	st := s.check(filepath.Dir(path))
	if st == nil || st.res == nil || st.res.Program == nil {
		return nil, errNoFile
	}
	name := path
	if isPreludeCopy(filepath.Dir(path)) {
		name = preludePrefix + filepath.Base(path)
	}
	f := st.files[name]
	if f == nil {
		// A platform's file another platform's check has: the first
		// platform's of its, this computer's when it's one of them.
		var found *pkgState
		for _, o := range st.others {
			if o.files[name] != nil && o.res != nil && o.res.Program != nil && (found == nil || o.platform == target.Host()) {
				found = o
			}
		}
		if found == nil {
			return nil, errNoFile
		}
		st, f = found, found.files[name]
	}
	src := st.srcs[name]
	if src == nil {
		var err error
		if src, err = s.source(name); err != nil {
			return nil, err
		}
	}
	prog := st.res.Program
	return &fileCtx{s: s, st: st, path: path, name: name, file: f, pkg: st.pkgOf[f], prog: prog, info: prog.Info, fset: prog.Fset, m: newMapper(src, s.utf8)}, nil
}

// pos is p as a position in the check.
func (fc *fileCtx) pos(p Position) syntax.Pos {
	return syntax.Pos(fc.file.File.Base() + fc.m.offset(p))
}

// offset is a position of the file as an offset in its text.
func (fc *fileCtx) offset(p syntax.Pos) int {
	return int(p) - fc.file.File.Base()
}

// rng is a span of the file as a Range.
func (fc *fileCtx) rng(sp syntax.Span) Range {
	return fc.m.rng(fc.offset(sp.Start), fc.offset(sp.End))
}

// location is a span anywhere in the check as a Location.
func (fc *fileCtx) location(start, end syntax.Pos) (Location, bool) {
	f := fc.fset.File(start)
	if f == nil {
		return Location{}, false
	}
	name := f.Name()
	var m *mapper
	if name == fc.name {
		m = fc.m
	} else {
		src, err := fc.s.source(name)
		if err != nil {
			return Location{}, false
		}
		m = newMapper(src, fc.s.utf8)
	}
	so := int(start) - f.Base()
	eo := so
	if end.IsValid() && int(end) >= int(start) && int(end) <= f.Base()+f.Size() {
		eo = int(end) - f.Base()
	}
	return Location{URI: fc.s.uri(name), Range: m.rng(so, eo)}, true
}

// ---------------------------------------------------------------------
// The prelude, as files an editor can open

// The checker names the prelude's files prelude/<name>.asuka; going to a
// definition there opens a copy written to the user's cache.
const preludePrefix = "prelude/"

var (
	// userCacheDir is where the prelude's copy goes (tests change it).
	userCacheDir = os.UserCacheDir

	preludeOnce sync.Once
	preludeDir  string
	preludeErr  error
)

// preludeCopyDir is the directory of the prelude's copy.
func preludeCopyDir() (string, error) {
	preludeOnce.Do(func() {
		h := sha256.New()
		for _, src := range prelude.Sources() {
			h.Write([]byte(src.Name))
			h.Write(src.Text)
		}
		base, err := userCacheDir()
		if err != nil {
			base = os.TempDir()
		}
		preludeDir = filepath.Join(base, "asuka", "prelude-"+hex.EncodeToString(h.Sum(nil))[:12])
		if err := os.MkdirAll(preludeDir, 0o755); err != nil {
			preludeErr = err
			return
		}
		for _, src := range prelude.Sources() {
			p := filepath.Join(preludeDir, filepath.Base(src.Name))
			if old, err := os.ReadFile(p); err == nil && string(old) == string(src.Text) {
				continue
			}
			if err := os.WriteFile(p, src.Text, 0o444); err != nil {
				preludeErr = err
				return
			}
		}
	})
	return preludeDir, preludeErr
}

// preludeFile is the path of the copy of the prelude's file name.
func preludeFile(name string) (string, error) {
	dir, err := preludeCopyDir()
	if err != nil {
		return "", err
	}
	return filepath.Join(dir, strings.TrimPrefix(name, preludePrefix)), nil
}

func preludeSource(name string) ([]byte, error) {
	for _, src := range prelude.Sources() {
		if src.Name == name {
			return src.Text, nil
		}
	}
	return nil, os.ErrNotExist
}

// isPreludeCopy reports whether dir is the prelude's copy: its files are
// the prelude, not a package.
func isPreludeCopy(dir string) bool {
	if !strings.Contains(filepath.Base(dir), "prelude-") {
		return false
	}
	d, err := preludeCopyDir()
	return err == nil && d == dir
}

// preludeState is a check of an empty package, for the prelude's files.
func (s *server) preludeState(dir string) *pkgState {
	st := &pkgState{dir: dir, gens: map[string]int{}, files: map[string]*syntax.SourceFile{}, pkgOf: map[*syntax.SourceFile]*check.Package{}, srcs: map[string][]byte{}}
	res := compile.Files(map[string][]byte{"empty.asuka": nil}, &compile.Options{CheckOnly: true})
	st.res = res
	for _, f := range res.Program.Prelude.Files {
		st.files[f.File.Name()] = f
		st.pkgOf[f] = res.Program.Prelude
	}
	s.pkgs[dir] = st
	return st
}
