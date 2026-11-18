package appbuild

import (
	"fmt"
	"os"
	"path/filepath"
	"sort"

	"github.com/egoist/asuka/internal/cbuild"
	"github.com/egoist/asuka/internal/nativegen"
	"github.com/egoist/asuka/internal/toml"
	"github.com/egoist/asuka/lang/bytecode"
	"github.com/egoist/asuka/platform/desktop"
	"github.com/egoist/asuka/sdk"
	"github.com/egoist/asuka/std"
)

// DesktopC is the C a Windows or Linux app's executable is built from
// besides the core and the app (RFC 0008): its host's, the native
// registry's (platform/desktop) and its native packages' (each one's
// desktop/, nativegen.CSources), with their include directories and the
// libraries they link.
type DesktopC struct {
	Sources []string // .c files (and their headers), all of them
	Deps    []string // what the .c files include besides (a vendored library's sources)
	Flags   []string // -I…, all of them
	Libs    []string // -l…
	// Units are those of Sources a build links as libraries (the host's,
	// the registry's, the std and sdk packages': prebuilt, or built into
	// the cache), each with its own flags; Rest are the others (an app's
	// own packages'), compiled with Flags. Without Units (a test's own),
	// a build compiles Sources.
	Units []cbuild.Unit
	Rest  []string
}

// DesktopNatives is the registry and the C for t of the native packages
// in dirs (their directories; a package with no C for t.OS adds
// nothing). A package's asuka.toml names the system libraries its C links
// with on each platform:
//
//	[windows]
//	libraries = ["ole32", "winhttp"]
//	[linux]
//	libraries = []   # GTK, GLib and GIO are resolved at run time, as the host does
func DesktopNatives(t cbuild.Target, dirs []string) (*DesktopC, error) {
	d, err := desktop.Dir()
	if err != nil {
		return nil, err
	}
	core, err := cbuild.CoreDir()
	if err != nil {
		return nil, err
	}
	shared := desktop.Shared(d)
	c := &DesktopC{
		Sources: append(append([]string{}, shared...), filepath.Join(desktop.Include(d), "asuka_desktop.h")),
		Flags:   []string{"-I" + desktop.Include(d)},
		Libs:    desktop.Libs(t.OS),
	}
	c.Flags = append(c.Flags, desktopDefines(t.OS)...)
	c.Units = append(c.Units, desktopUnit(core, d, "desktop", shared, nil, t.OS, false, desktop.Embedded()))
	stdDir, _ := std.Dir()
	sdkDir, _ := sdk.Dir()
	seen := map[string]bool{}
	sorted := append([]string{}, dirs...)
	sort.Strings(sorted)
	for _, pkg := range sorted {
		dir := filepath.Join(pkg, nativegen.DesktopDir)
		srcs := nativegen.CSources(dir, t.OS)
		if len(srcs) == 0 || seen[dir] {
			continue
		}
		seen[dir] = true
		var files []string
		for _, s := range srcs {
			files = append(files, filepath.Join(dir, s))
		}
		hs, _ := filepath.Glob(filepath.Join(dir, "*.h"))
		c.Sources = append(append(c.Sources, files...), hs...)
		c.Deps = append(c.Deps, vendored(dir)...)
		c.Flags = append(c.Flags, "-I"+dir)
		// The standard library's and the platform SDK's packages are
		// libraries of their own (their objects register themselves:
		// linked whole); an app's own packages' C is compiled with it.
		switch parent := filepath.Dir(filepath.Clean(pkg)); {
		case stdDir != "" && parent == filepath.Clean(stdDir):
			c.Units = append(c.Units, desktopUnit(core, d, "std-"+filepath.Base(pkg), files, []string{"-I" + dir}, t.OS, true, std.Embedded() && desktop.Embedded()))
		case sdkDir != "" && parent == filepath.Clean(sdkDir):
			c.Units = append(c.Units, desktopUnit(core, d, "sdk-"+filepath.Base(pkg), files, []string{"-I" + dir}, t.OS, true, sdk.Embedded() && desktop.Embedded()))
		default:
			c.Rest = append(c.Rest, files...)
		}
		libs, err := packageLibs(pkg, t.OS)
		if err != nil {
			return nil, err
		}
		for _, l := range libs {
			c.Libs = append(c.Libs, "-l"+l)
		}
	}
	return c, nil
}

// desktopDefines are what the desktop's C is compiled with on goos:
// POSIX's and GNU's declarations beside C17's on Linux, Win32's wide
// functions on Windows.
func desktopDefines(goos string) []string {
	switch goos {
	case "linux":
		return []string{"-D_GNU_SOURCE"}
	case "windows":
		return []string{"-DUNICODE", "-D_UNICODE"}
	}
	return nil
}

// desktopUnit is desktop C as a unit (the registry's, a package's):
// compiled with the core's warnings and include directories,
// asuka_desktop.h's, the platform's defines and its own flags.
func desktopUnit(core, desk, name string, srcs, own []string, goos string, whole, embedded bool) cbuild.Unit {
	flags := append(append(append([]string{}, cbuild.Flags...), cbuild.Includes(core)...), "-I"+desktop.Include(desk))
	flags = append(append(flags, desktopDefines(goos)...), own...)
	return cbuild.Unit{Name: name, Sources: srcs, Flags: flags, Whole: whole, Embedded: embedded && cbuild.CoreEmbedded()}
}

// vendored are the files of the libraries a package's C vendors, in
// desktop/third_party/ (each with its license): the package's own C
// includes their sources, so they count in the cache's key but are never
// compiled alone.
func vendored(dir string) []string {
	var out []string
	filepath.WalkDir(filepath.Join(dir, "third_party"), func(p string, d os.DirEntry, err error) error {
		if err == nil && !d.IsDir() {
			out = append(out, p)
		}
		return nil
	})
	return out
}

// packageLibs are the libraries a package's asuka.toml says its C links
// with on goos.
func packageLibs(dir, goos string) ([]string, error) {
	b, err := os.ReadFile(filepath.Join(dir, "asuka.toml"))
	if err != nil {
		return nil, nil
	}
	type libs struct {
		Libraries []string `json:"libraries"`
	}
	var m struct {
		Windows libs `json:"windows"`
		Linux   libs `json:"linux"`
	}
	if err := toml.Unmarshal(b, &m); err != nil {
		return nil, fmt.Errorf("%s: %v", filepath.Join(dir, "asuka.toml"), err)
	}
	if goos == "windows" {
		return m.Windows.Libraries, nil
	}
	return m.Linux.Libraries, nil
}

// DesktopExe builds a Windows or Linux app's executable for t at out:
// host's C, the core, the native C and m compiled to C (a release's), with
// the C m calls (its externs', the std packages': std/sqlite's SQLite),
// or with m nil a dev player's, which loads the bytecode asuka dev sends.
func DesktopExe(t cbuild.Target, m *bytecode.Module, host, natives *DesktopC, out string) error {
	var c *bytecode.C
	if m != nil {
		c = m.C
	}
	return desktopExe(t, m, c, host, natives, out)
}

// desktopExe is DesktopExe with c, the C the app calls (a player's: its
// last compile's).
func desktopExe(t cbuild.Target, m *bytecode.Module, c *bytecode.C, host, natives *DesktopC, out string) error {
	core, err := cbuild.CoreDir()
	if err != nil {
		return err
	}
	tc, err := cbuild.ForTarget(t)
	if err != nil {
		return err
	}
	o := cbuild.ExeOptions{Debug: m == nil}
	if o.Foreign, err = Foreign(c, t); err != nil {
		return err
	}
	for _, d := range []*DesktopC{host, natives} {
		if d == nil {
			continue
		}
		o.Units = append(o.Units, d.Units...)
		if d.Units != nil {
			o.Sources = append(o.Sources, d.Rest...)
		} else {
			o.Sources = append(o.Sources, d.Sources...)
		}
		o.Flags = append(o.Flags, d.Flags...)
		o.Link = append(o.Link, d.Libs...)
	}
	if m != nil {
		if o.App, err = AppC(m); err != nil {
			return err
		}
		o.Builtins = m.Builtins
	}
	return tc.ExeWith(core, o, out)
}
