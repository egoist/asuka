package compile

import (
	"fmt"
	"path/filepath"
	"sort"
	"strings"

	"github.com/egoist/asuka/lang/check"
	"github.com/egoist/asuka/lang/syntax"
	"github.com/egoist/asuka/lang/target"
)

// A package's files for some platforms (tray_linux.asuka, menu_desktop.asuka)
// are part of it only when it compiles for one of them. A name is declared
// once on each platform: in a file every platform has, or in a file for
// each platform that has it. Two files a platform has both of can't
// declare the same name, whichever platform the compile is for: that's
// reported the same on every platform (dupNames), so asuka check, which
// checks each, says it once.

// declared is a top-level name a file declares: its key (a style's is
// "style Name", the app block's "app"), what to call it, where.
type declared struct {
	key, what string
	pos       syntax.Pos
}

func declsOf(f *syntax.SourceFile) []declared {
	var out []declared
	add := func(id *syntax.Ident) {
		if id != nil && id.Name != "_" {
			out = append(out, declared{id.Name, id.Name, id.Start})
		}
	}
	for _, d := range f.Decls {
		switch d := d.(type) {
		case *syntax.ComponentDecl:
			add(d.Name)
		case *syntax.ScreenDecl:
			add(d.Name)
		case *syntax.StoreDecl:
			add(d.Name)
		case *syntax.StructDecl:
			add(d.Name)
		case *syntax.ShaderDecl:
			add(d.Name)
		case *syntax.EnumDecl:
			add(d.Name)
		case *syntax.InterfaceDecl:
			add(d.Name)
		case *syntax.FuncDecl:
			add(d.Name)
		case *syntax.ExternTypeDecl:
			add(d.Name)
		case *syntax.TypeAliasDecl:
			add(d.Name)
		case *syntax.StateDecl:
			add(d.Name)
		case *syntax.LetDecl:
			if bp, ok := d.Pattern.(*syntax.BindPat); ok {
				add(bp.Name)
			}
		case *syntax.StyleDecl:
			if d.Name != nil {
				out = append(out, declared{"style " + d.Name.Name, "style " + d.Name.Name, d.Name.Start})
			}
		case *syntax.AppDecl:
			out = append(out, declared{"app", "the app block", d.Start})
		}
	}
	return out
}

// platformsOf is the platforms a file of a package is for: all, or its
// suffix's.
func platformsOf(name string) []target.Platform {
	if ps, _ := target.FileSuffix(name); ps != nil {
		return ps
	}
	return target.All
}

// overlap is the platforms both a and b have.
func overlap(a, b []target.Platform) []target.Platform {
	var out []target.Platform
	for _, p := range a {
		for _, q := range b {
			if p == q {
				out = append(out, p)
			}
		}
	}
	return out
}

// dupNames reports the names two of u's files declare that a platform
// has both of, where one is a platform's file (two files every platform
// has are the checker's to report): at the declaration in the file more
// platforms have, which is part of the package whenever the other is
// (the suffixes nest: every platform, desktop or mobile, one), so that
// every platform's compile reports it alike. It notes where, so that
// the checker's own report of the same is left out (withoutTwice).
func (l *loader) dupNames(u *unit) {
	type file struct {
		f      *syntax.SourceFile
		name   string
		ps     []target.Platform
		active bool
	}
	var files []file
	for _, f := range u.files {
		files = append(files, file{f, f.File.Name(), platformsOf(f.File.Name()), true})
	}
	for _, f := range u.others {
		files = append(files, file{f, f.File.Name(), platformsOf(f.File.Name()), false})
	}
	sort.SliceStable(files, func(i, j int) bool {
		if len(files[i].ps) != len(files[j].ps) {
			return len(files[i].ps) > len(files[j].ps)
		}
		return files[i].name > files[j].name
	})
	decls := make([][]declared, len(files))
	for i := range files {
		decls[i] = declsOf(files[i].f)
	}
	for i, a := range files {
		if !a.active {
			continue
		}
		for j := i + 1; j < len(files); j++ {
			b := files[j]
			both := overlap(a.ps, b.ps)
			if len(both) == 0 || len(a.ps) == len(target.All) && len(b.ps) == len(target.All) {
				continue
			}
			for _, da := range decls[i] {
				for _, db := range decls[j] {
					if da.key != db.key {
						continue
					}
					pos := l.fset.Position(da.pos)
					if l.twice == nil {
						l.twice = map[syntax.Position]bool{}
					}
					l.twice[pos] = true
					if b.active {
						l.twice[l.fset.Position(db.pos)] = true
					}
					l.diags = append(l.diags, check.Diagnostic{Pos: pos, End: pos, Msg: fmt.Sprintf(
						"%s is declared twice for %s: here and in %s (a name is declared once on each platform: in a file every platform has, or in a file for each platform)",
						da.what, target.List(both), filepath.Base(b.name))})
				}
			}
		}
	}
}

// withoutTwice is the checker's diagnostics without its reports of names
// declared twice that dupNames reported.
func (l *loader) withoutTwice(diags []check.Diagnostic) []check.Diagnostic {
	if len(l.twice) == 0 {
		return diags
	}
	var out []check.Diagnostic
	for _, d := range diags {
		if l.twice[d.Pos] && (strings.Contains(d.Msg, "declared twice") || d.Msg == "a package has one app") {
			continue
		}
		out = append(out, d)
	}
	return out
}

// A PlatformDiagnostic is a diagnostic of a check for several platforms
// (DirPlatforms): Platforms are those whose compile reports it, nil when
// every one checked does.
type PlatformDiagnostic struct {
	check.Diagnostic
	Platforms []target.Platform
}

// String is file:line:col: message, and the platforms it's for when it
// isn't every one's: "(for Linux)".
func (d PlatformDiagnostic) String() string {
	s := d.Diagnostic.String()
	if d.Platforms != nil {
		s += " (for " + target.List(d.Platforms) + ")"
	}
	return s
}

// Platforms is a package checked for several platforms.
type Platforms struct {
	Diags, Warnings []PlatformDiagnostic
	// Results are each platform's compile, by platform; a platform the
	// package compiles the same files for as the first's (one without
	// platforms' files) shares its Result.
	Results map[target.Platform]*Result
	// First is the first platform's compile.
	First *Result
}

// DirPlatforms compiles the package in dir (Dir) for each of ps (nil:
// target.All), opts' Platform aside: what asuka check and an editor
// report. A program whose packages have no platforms' files compiles
// once, the same for every one; else once for each, and a diagnostic
// two report is reported once, saying which when not all do. Platforms
// the package has no files for are skipped; it's an error when none has.
func DirPlatforms(dir string, opts *Options, ps []target.Platform) (*Platforms, error) {
	if ps == nil {
		ps = target.All
	}
	if opts == nil {
		opts = &Options{}
	}
	out := &Platforms{Results: map[target.Platform]*Result{}}
	type seen struct {
		i  int
		ps []target.Platform
	}
	diags := map[string]*seen{}
	warns := map[string]*seen{}
	add := func(list *[]PlatformDiagnostic, idx map[string]*seen, d check.Diagnostic, p target.Platform) {
		key := d.String()
		if s := idx[key]; s != nil {
			s.ps = append(s.ps, p)
			return
		}
		idx[key] = &seen{len(*list), []target.Platform{p}}
		*list = append(*list, PlatformDiagnostic{Diagnostic: d})
	}
	var checked []target.Platform
	var firstErr error
	for _, p := range ps {
		o := *opts
		o.Platform = p
		res, err := Dir(dir, &o)
		if err != nil {
			if firstErr == nil {
				firstErr = err
			}
			continue
		}
		if out.First == nil {
			out.First = res
		}
		checked = append(checked, p)
		out.Results[p] = res
		for _, d := range res.Diags {
			add(&out.Diags, diags, d, p)
		}
		for _, d := range res.Warnings {
			add(&out.Warnings, warns, d, p)
		}
		if !res.PlatformFiles && len(checked) == 1 {
			// The same files on every platform: the same diagnostics.
			for _, q := range ps[1:] {
				out.Results[q] = res
			}
			checked = ps
			for _, s := range diags {
				s.ps = ps
			}
			for _, s := range warns {
				s.ps = ps
			}
			break
		}
	}
	if out.First == nil {
		return nil, firstErr
	}
	for _, x := range []struct {
		list []PlatformDiagnostic
		idx  map[string]*seen
	}{{out.Diags, diags}, {out.Warnings, warns}} {
		for _, s := range x.idx {
			if len(s.ps) < len(checked) {
				x.list[s.i].Platforms = s.ps
			}
		}
	}
	return out, nil
}
