// Package appbuild makes what each platform runs an Asuka app with, from
// its compiled module (lang/bytecode): the C core built for a target with
// the app compiled to C (a release, lang/cgen) or without it (a dev
// player, which loads the bytecode asuka dev sends), the C desktop hosts'
// executables (Windows, Linux) and their settings, the web's page, and the
// runs of a module's tests on the fake host (asuka-run). The CLI
// (cmd/asuka) packages them for each platform.
package appbuild

import (
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"slices"
	"sort"
	"strings"

	"github.com/egoist/asuka/internal/pkgs"
	"github.com/egoist/asuka/lang/bytecode"
	"github.com/egoist/asuka/lang/check"
	"github.com/egoist/asuka/lang/compile"
	"github.com/egoist/asuka/lang/diag"
	"github.com/egoist/asuka/lang/syntax"
	"github.com/egoist/asuka/lang/target"
	"github.com/egoist/asuka/std"
)

// Compile compiles the app (or package) in dir, with its dependencies
// (deps: package ID → directory, from internal/pkgs), for platform (its
// files, @platform; "": target.Default), to a module, and
// says what its program uses (nil: not known: Uses checks it then); its
// diagnostics are "file:line:col: message". An error with no diagnostics
// is one that isn't the source's (a file that can't be read). It's
// lang/compile; tests replace it.
var Compile = compileDir

// DevLang and DevDirection are asuka dev's --lang and --dir: the language
// and the direction compiled into the module for this run, on whatever
// platform runs it ("": the user's, the language's).
var DevLang, DevDirection string

// Warnings receives what's worth saying that doesn't stop a build (the
// catalogs' missing translations), "file:line:col: warning: message".
var Warnings io.Writer = os.Stderr

func compileDir(dir string, deps map[string]string, platform target.Platform) (*bytecode.Module, *Uses, []check.Diagnostic, error) {
	// Imports resolve through the project's asuka.lock (compile.Dir's
	// own), which Load made ready.
	res, err := compile.Dir(dir, &compile.Options{Platform: platform, Lang: DevLang, Direction: DevDirection, Project: binRoot(dir)})
	if err != nil {
		return nil, nil, nil, err
	}
	var u *Uses
	if res.Program != nil {
		u = &Uses{Imports: map[string]bool{}, Refs: map[string]bool{}}
		usesOf(res.Program, u)
	}
	if Warnings != nil {
		for _, w := range res.Warnings {
			if r, err := filepath.Rel(dir, w.Pos.Filename); err == nil && !strings.HasPrefix(r, "..") {
				w.Pos.Filename = r
			}
			fmt.Fprintln(Warnings, w.Format(diag.Warning, ""))
		}
	}
	if len(res.Diags) > 0 {
		return nil, u, res.Diags, nil
	}
	return res.Module, u, nil, nil
}

// Project is an app or a package: its directory and its dependencies'.
type Project struct {
	Dir string
	// Deps maps each dependency's package ID (asuka.lock's), and the name
	// asuka.toml gives it when that's another, to its directory.
	Deps map[string]string
	// Local are the dependencies that are directories of their own
	// (file:), which a change to is a change to the app.
	Local []string
	// File, for a project of one file (asuka run file.asuka), is that file.
	File string
	// Root, for a program of a project's ([bin.<name>], Bin), is the
	// project's directory: its asuka.lock, its catalogs, its packages the
	// program imports (../model).
	Root string
	// Bins are the project's programs ([bin.<name>]), sorted by name.
	Bins []Bin
	// platforms are its compiles, by the platform they're for: a
	// package's files and @platform's branches are a platform's.
	platforms map[target.Platform]*forPlatform
}

// forPlatform is a project's compiles for a platform: uses is what the
// last compile (or check) found its program uses, usesStamp the files'
// stamp they're of; last is the last compile.
type forPlatform struct {
	uses      *Uses
	usesStamp string
	last      *compiled
}

// on is p's compiles for platform.
func (p *Project) on(platform target.Platform) *forPlatform {
	platform = platform.Or()
	if p.platforms == nil {
		p.platforms = map[target.Platform]*forPlatform{}
	}
	f := p.platforms[platform]
	if f == nil {
		f = &forPlatform{}
		p.platforms[platform] = f
	}
	return f
}

// Load reads the project in dir (or the one file dir names): its
// dependencies resolved and fetched (asuka.lock; pkgs.Install) when
// asuka.toml has any.
func Load(dir string) (*Project, error) {
	abs, err := filepath.Abs(dir)
	if err != nil {
		return nil, err
	}
	st, err := os.Stat(abs)
	if err != nil {
		return nil, err
	}
	p := &Project{Dir: abs, Deps: map[string]string{}}
	if !st.IsDir() {
		if !strings.HasSuffix(abs, ".asuka") {
			return nil, fmt.Errorf("%s: not an .asuka file", dir)
		}
		p.Dir, p.File = filepath.Dir(abs), abs
		return p, nil
	}
	m, err := pkgs.ReadManifest(abs)
	if err != nil {
		return nil, err
	}
	if p.Bins, err = bins(abs, m); err != nil {
		return nil, err
	}
	if len(m.Dependencies) == 0 {
		return p, nil
	}
	c := pkgs.NewClient(pkgs.LoadNPMRC(abs))
	l, err := pkgs.Install(c, abs, false)
	if err != nil {
		return nil, err
	}
	dirs, err := pkgs.Dirs(c, l, abs)
	if err != nil {
		return nil, err
	}
	for id, d := range dirs {
		p.Deps[id] = d
	}
	for key, id := range l.Root {
		if d, ok := dirs[id]; ok && key != id {
			p.Deps[key] = d
		}
	}
	for _, lp := range l.Packages {
		if strings.HasPrefix(lp.Source, "file:") {
			p.Local = append(p.Local, dirs[lp.ID])
		}
	}
	sort.Strings(p.Local)
	return p, nil
}

// Compile compiles the project for platform (Compile; "":
// target.Default), and notes what its program uses there (Uses). A
// project whose files haven't changed since its last compile for the
// platform (stamp) isn't compiled again: loading an app and building it
// is one compile.
func (p *Project) Compile(platform target.Platform) (*bytecode.Module, []check.Diagnostic, error) {
	f := p.on(platform)
	stamp := p.stamp()
	if l := f.last; l != nil && l.stamp == stamp {
		return l.m, l.diags, l.err
	}
	m, u, diags, err := p.compile(platform.Or())
	if u != nil {
		f.uses, f.usesStamp = u, stamp
	}
	f.last = &compiled{stamp: stamp, m: m, diags: diags, err: err}
	return m, diags, err
}

// Forget drops the last compiles: the next Compile compiles (asuka dev's,
// for a save, which a file's time may not tell from the last).
func (p *Project) Forget() {
	for _, f := range p.platforms {
		f.last = nil
	}
}

// compiled is a project's last compile, and what its files were.
type compiled struct {
	stamp string
	m     *bytecode.Module
	diags []check.Diagnostic
	err   error
}

// stamp is what the project's compile reads, as it is now: its files'
// (and its dependencies') paths, sizes and times, and asuka dev's
// language and direction.
func (p *Project) stamp() string {
	h := sha256.New()
	fmt.Fprintln(h, DevLang, DevDirection, p.File)
	roots := []string{p.Dir}
	if p.Root != "" {
		roots = append(roots, p.Root)
	}
	if p.File != "" {
		roots = []string{p.File}
	}
	for _, d := range p.Deps {
		roots = append(roots, d)
	}
	slices.Sort(roots)
	for _, r := range slices.Compact(roots) {
		filepath.WalkDir(r, func(path string, d fs.DirEntry, err error) error {
			if err != nil {
				return nil
			}
			if d.IsDir() {
				if path != r && skipDir(d.Name()) {
					return filepath.SkipDir
				}
				return nil
			}
			if info, err := d.Info(); err == nil {
				fmt.Fprintln(h, path, info.Size(), info.ModTime().UnixNano())
			}
			return nil
		})
	}
	return hex.EncodeToString(h.Sum(nil))
}

func (p *Project) compile(platform target.Platform) (*bytecode.Module, *Uses, []check.Diagnostic, error) {
	if p.File != "" {
		// One file: a directory of its own, so that what's beside it
		// isn't compiled with it.
		tmp, err := os.MkdirTemp("", "asuka-run-")
		if err != nil {
			return nil, nil, nil, err
		}
		defer os.RemoveAll(tmp)
		b, err := os.ReadFile(p.File)
		if err != nil {
			return nil, nil, nil, err
		}
		if err := os.WriteFile(filepath.Join(tmp, filepath.Base(p.File)), b, 0o644); err != nil {
			return nil, nil, nil, err
		}
		m, u, diags, err := Compile(tmp, p.Deps, platform)
		copied := filepath.Join(tmp, filepath.Base(p.File))
		if m != nil {
			// Where a function is, as an error's trace says it: the file,
			// not its copy.
			for i := range m.Funcs {
				if m.Funcs[i].File == copied {
					m.Funcs[i].File = p.File
				}
			}
		}
		for i := range diags {
			for _, pos := range []*syntax.Position{&diags[i].Pos, &diags[i].End} {
				if pos.Filename == filepath.Join(tmp, filepath.Base(p.File)) {
					pos.Filename = p.File
				}
			}
		}
		if m != nil && m.C != nil && len(m.C.Dirs) > 0 {
			// Its C is the [c] of the asuka.toml beside it.
			c := *m.C
			c.Dirs = append([]string{p.Dir}, c.Dirs[1:]...)
			m.C = &c
		}
		return m, u, diags, err
	}
	return Compile(p.Dir, p.Deps, platform)
}

// WatchRoots are the directories whose .asuka files make the app: its
// own and its local dependencies'.
func (p *Project) WatchRoots() []string {
	return append([]string{p.Dir}, p.Local...)
}

// skipDir reports whether a directory is no part of a package's sources:
// hidden ones, build outputs, platforms' projects.
func skipDir(name string) bool {
	return strings.HasPrefix(name, ".") || name == "dist" || name == "build" || name == "node_modules" || name == "testdata"
}

// Sources are the .asuka files of the project and its dependencies.
func (p *Project) Sources() []string {
	if p.File != "" {
		return []string{p.File}
	}
	var out []string
	roots := []string{p.Dir}
	for _, d := range p.Deps {
		roots = append(roots, d)
	}
	slices.Sort(roots)
	for _, r := range slices.Compact(roots) {
		filepath.WalkDir(r, func(path string, d fs.DirEntry, err error) error {
			if err != nil {
				return nil
			}
			if d.IsDir() {
				if path != r && skipDir(d.Name()) {
					return filepath.SkipDir
				}
				return nil
			}
			if strings.HasSuffix(path, ".asuka") {
				out = append(out, path)
			}
			return nil
		})
	}
	return out
}

// Uses is what the program (the project and every package it imports,
// all the way down) imports and refers to, as the checker resolved it,
// which decides what a platform's build needs: a package's manifest's
// needs (asuka.toml), and those of its members (its [uses.<member>]
// tables), the parts of a host that link frameworks of their own.
type Uses struct {
	// Imports are the packages imported anywhere in the program, by
	// import path (platform/location, @acme/charts).
	Imports map[string]bool
	// Refs are the members referred to from outside the package that
	// declares them, anywhere in the program (a dependency that calls
	// location.requestAlways counts as the app would): "<import
	// path>.<name>" for a package's function, type, store, state or let
	// (platform/location.requestAlways), "<import path>.<Type>.<name>" for a
	// member of its type (platform/icloud.Value.set: a method or a field,
	// which the program names as it reads it, assigns it or makes the
	// struct with it); the prelude's by name alone (Video). Test blocks'
	// don't count: they aren't the app's.
	Refs map[string]bool
	// Embeds are the files and globs the program embeds (std/embed): a
	// change to a file one matches is a change to the program's source.
	Embeds []check.EmbedPath
}

// Embedded reports whether the program embeds the file at path (an
// absolute path), or would if it were there (a glob's).
func (u *Uses) Embedded(path string) bool {
	for _, p := range u.Embeds {
		if p.Match(path) {
			return true
		}
	}
	return false
}

// Import reports whether the program imports pkg.
func (u *Uses) Import(pkg string) bool { return u.Imports[pkg] }

// Ref reports whether the program refers to pkg's member (pkg "": the
// prelude's), from outside pkg.
func (u *Uses) Ref(pkg, member string) bool {
	if pkg == "" {
		return u.Refs[member]
	}
	return u.Refs[pkg+"."+member]
}

// Uses is what the project's program uses on platform, as its compile's
// checker resolved it (a program with errors counts for what it
// resolved): Compile's, which it compiles if its files changed since. A
// Compile that doesn't say (a test's) has the program checked alone.
// What a program uses may be another on each platform: a package's files
// for a platform import what they need there.
func (p *Project) Uses(platform target.Platform) *Uses {
	p.Compile(platform)
	f := p.on(platform)
	if f.uses != nil && f.last != nil && f.usesStamp == f.last.stamp {
		return f.uses
	}
	u := p.check(platform)
	if f.last != nil {
		f.usesStamp = f.last.stamp
	}
	return u
}

// check checks the project's program alone (nothing more than the
// checker: no lowering) for what it imports and refers to on platform.
func (p *Project) check(platform target.Platform) *Uses {
	u := &Uses{Imports: map[string]bool{}, Refs: map[string]bool{}}
	p.on(platform).uses = u
	dir := p.Dir
	if p.File != "" {
		// One file: a directory of its own, as Compile does.
		tmp, err := os.MkdirTemp("", "asuka-uses-")
		if err != nil {
			return u
		}
		defer os.RemoveAll(tmp)
		b, err := os.ReadFile(p.File)
		if err != nil {
			return u
		}
		if err := os.WriteFile(filepath.Join(tmp, filepath.Base(p.File)), b, 0o644); err != nil {
			return u
		}
		dir = tmp
	}
	res, err := compile.Dir(dir, &compile.Options{Platform: platform, CheckOnly: true, NoCatalogs: true})
	if err != nil || res.Program == nil {
		return u
	}
	usesOf(res.Program, u)
	return u
}

// usesOf adds prog's imports and its packages' references to each
// other's members to u.
func usesOf(prog *check.Program, u *Uses) {
	info := prog.Info
	for p := range info.EmbedPaths {
		u.Embeds = append(u.Embeds, p)
	}
	slices.SortFunc(u.Embeds, func(a, b check.EmbedPath) int { return strings.Compare(a.Dir+"\x00"+a.Pattern, b.Dir+"\x00"+b.Pattern) })
	for _, pkg := range prog.Packages {
		if pkg.Prelude || pkg == prog.Root {
			continue
		}
		u.Imports[pkg.Path] = true
	}
	// ref notes that from refers to name, of decl (nil: the prelude) or
	// of its type owner.
	ref := func(from, decl *check.Package, owner *check.TypeName, name string) {
		if decl == from || decl == nil && owner == nil {
			return
		}
		if owner != nil {
			if owner.Pkg == from {
				return
			}
			decl, name = owner.Pkg, owner.Name()+"."+name
		}
		if decl == nil || decl.Prelude {
			u.Refs[name] = true
			return
		}
		if decl.Path == "" || strings.HasPrefix(decl.Path, ".") {
			return // the app's own
		}
		u.Refs[decl.Path+"."+name] = true
	}
	named := func(t check.Type) *check.TypeName {
		for {
			switch x := t.(type) {
			case *check.Named:
				return x.Obj
			case *check.Optional:
				t = x.Elem
			default:
				return nil
			}
		}
	}
	object := func(from *check.Package, o check.Object) {
		switch o := o.(type) {
		case *check.Function:
			if o.Recv != nil {
				ref(from, nil, o.Recv, o.Name())
			} else {
				ref(from, o.Pkg, nil, o.Name())
			}
		case *check.TypeName:
			ref(from, o.Pkg, nil, o.Name())
		case *check.Global:
			if o.Owner != nil {
				ref(from, nil, o.Owner, o.Name())
			} else {
				ref(from, o.Pkg, nil, o.Name())
			}
		case *check.Member:
			if o.Owner != nil {
				ref(from, nil, o.Owner, o.Name())
			}
		}
	}
	for _, pkg := range prog.Packages {
		if pkg.Prelude {
			continue
		}
		for _, f := range pkg.Files {
			syntax.Inspect(f, func(n syntax.Node) bool {
				switch n := n.(type) {
				case *syntax.TestDecl:
					return false
				case *syntax.Ident:
					if o := info.Uses[n]; o != nil {
						object(pkg, o)
					}
				case *syntax.CallExpr:
					if e := info.Elements[n]; e != nil && e.Comp != nil {
						object(pkg, e.Comp)
					}
					// A method's call (n.from(…)): the method. A struct
					// made with a field (Notification(sender: p)): that
					// field, as a read or an assignment of it is; a field
					// left at its default isn't.
					if c := info.Calls[n]; c != nil {
						if c.Func != nil {
							object(pkg, c.Func)
						}
						if c.Kind == check.CallStruct && c.Type != nil {
							for i, a := range c.Args {
								if a != nil && i < len(c.Params) {
									ref(pkg, nil, c.Type.Obj, c.Params[i].Name)
								}
							}
						}
					}
				case *syntax.MemberExpr:
					if sel := info.Selections[n]; sel != nil {
						switch sel.Kind {
						case check.SelField, check.SelCase, check.SelIfaceField, check.SelMethod:
							// A method's call has its receiver's type, no object.
							ref(pkg, nil, named(sel.Recv), sel.Name)
						case check.SelPkg:
						default:
							if sel.Obj != nil {
								object(pkg, sel.Obj)
							}
						}
					}
				}
				return true
			})
		}
	}
}

// ImportList is u's imports, sorted.
func (u *Uses) ImportList() []string {
	out := make([]string, 0, len(u.Imports))
	for k := range u.Imports {
		out = append(out, k)
	}
	sort.Strings(out)
	return out
}

// Std are the standard library's and the platform SDK's packages the
// project imports on platform, with those they import: import path
// (std/http, platform/network) → directory (std.Package's). The
// compiler's own (std/json, std/math) aren't directories, and aren't
// here.
func (p *Project) Std(platform target.Platform) map[string]string {
	u := p.on(platform).uses
	if u == nil {
		u = p.Uses(platform)
	}
	out := map[string]string{}
	for imp := range u.Imports {
		if dir, ok := std.Package(imp); ok {
			out[imp] = dir
		}
	}
	return out
}
