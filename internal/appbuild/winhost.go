package appbuild

import (
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"

	"github.com/egoist/asuka/internal/cbuild"
	"github.com/egoist/asuka/internal/srcdir"
	"github.com/egoist/asuka/lang/bytecode"
	"github.com/egoist/asuka/platform/desktop"
	"github.com/egoist/asuka/platform/windows"
)

// The Windows host's sources (RFC 0008): the repository's, or the CLI's
// embedded copy (platform/desktop's are desktop.Dir's).
var windowsTree = srcdir.Tree{Env: "ASUKA_WINDOWS_HOST", Sub: filepath.Join("platform", "windows"), Valid: func(d string) bool {
	_, err := os.Stat(filepath.Join(d, "src", "host.h"))
	return err == nil
}, Files: windows.Sources, Cache: "windows-src"}

// WindowsOptions say how a Windows executable is built.
type WindowsOptions struct {
	// App are an app's C sources (lang/cgen's app.c, its packages' glue):
	// none for asuka dev's player, which loads the app's bytecode.
	App []string
	// Builtins are the ones the app calls (its module's): only the std
	// packages' tables of them are linked. nil: every table.
	Builtins []string
	// Native is the native packages' C (DesktopNatives: the registry's
	// and the packages'), their include directories and libraries.
	Native *DesktopC
	// Resources is a COFF object with the executable's resources (its
	// icon, manifest and version).
	Resources string
	// Console makes it a console program (the player, tests: asuka dev
	// reads its output); else a GUI program with no console window.
	Console bool
	// Debug builds the dev configuration (assertions, symbols, less
	// optimized: cbuild.ConfigFlags), with what's for development only
	// in the core: the performance overlay and recordings (AK_PERF), the
	// profiler. asuka dev's player (no App, no Tests) is always one, as
	// Linux's is (DesktopExe).
	Debug bool
	// Tests are a test program's C sources (platform/windows/test), with
	// the host in place of its main.
	Tests []string
	// C is what the app calls (a module's: its externs', the std packages'
	// it imports, std/sqlite's SQLite; asuka dev's player's: the last
	// compile's), or nil.
	C *bytecode.C
}

// WindowsLibs are the system libraries the Windows host links.
var WindowsLibs = []string{
	"-lgdiplus", "-lcomctl32", "-luxtheme", "-ldwmapi", "-lole32", "-loleaut32", "-luuid", "-lshell32", "-lshlwapi",
	"-luser32", "-lgdi32", "-ladvapi32", "-lwinhttp", "-lcrypt32", "-lncrypt", "-lws2_32", "-lmsimg32", "-lwindowscodecs", "-limm32", "-lversion",
	"-lcomdlg32", "-lwinmm", "-lcredui", "-ld3d11", "-ldxgi", "-luiautomationcore", "-lpropsys", "-lbcrypt", "-ldbghelp",
}

// WindowsHost builds the Windows host's executable for t at out: the host
// (platform/windows) with the C the desktop hosts share (platform/
// desktop), a unit; the native packages' C (theirs, a std or sdk
// package's a unit too); the core, with the app compiled in (o.App) or
// none (asuka dev's player).
func WindowsHost(t cbuild.Target, o WindowsOptions, out string) error {
	tc, err := cbuild.ForTarget(t)
	if err != nil {
		return err
	}
	core, err := cbuild.CoreDir()
	if err != nil {
		return err
	}
	win, embedded, err := windowsTree.Find()
	if err != nil {
		return err
	}
	dsk, err := desktop.Dir()
	if err != nil {
		return err
	}
	unit := windowsHostUnit(core, win, dsk, embedded)
	host := unit.Sources
	hostFlags := []string{"-I" + filepath.Join(win, "src"), "-I" + filepath.Join(dsk, "include"), "-DUNICODE", "-D_UNICODE"}
	eo := cbuild.ExeOptions{Debug: o.Debug || len(o.App) == 0 && len(o.Tests) == 0}
	if eo.Foreign, err = Foreign(o.C, t); err != nil {
		return err
	}
	if len(o.Tests) > 0 {
		// The tests' own main: the host's is left out, the rest compiled
		// with them.
		kept := host[:0]
		for _, s := range host {
			if filepath.Base(s) != "main.c" || !strings.HasPrefix(s, filepath.Join(win, "src")) {
				kept = append(kept, s)
			}
		}
		eo.Sources = append(kept, o.Tests...)
		eo.Flags = append(append([]string{}, hostFlags...), "-I"+filepath.Join(win, "test"), "-DWH_TEST=1")
	} else {
		eo.Units = []cbuild.Unit{unit}
		eo.Flags = append(eo.Flags, hostFlags...)
	}
	if o.Native != nil {
		// The registry and what else of platform/desktop's the natives
		// name is the host's already.
		have := map[string]bool{}
		for _, s := range host {
			have[filepath.Clean(s)] = true
		}
		if o.Native.Units != nil {
			for _, u := range o.Native.Units {
				if u.Name != "desktop" {
					eo.Units = append(eo.Units, u)
				}
			}
			eo.Sources = append(eo.Sources, o.Native.Rest...)
		} else {
			for _, s := range o.Native.Sources {
				if strings.HasSuffix(s, ".c") && !have[filepath.Clean(s)] {
					eo.Sources = append(eo.Sources, s)
					have[filepath.Clean(s)] = true
				}
			}
		}
		eo.Flags = append(eo.Flags, o.Native.Flags...)
	}
	if len(o.App) > 0 {
		// lang/cgen's app.c first; packages' glue after, compiled with it.
		eo.App = o.App[0]
		eo.Sources = append(eo.Sources, o.App[1:]...)
		eo.Builtins = o.Builtins
	}
	if o.Resources != "" {
		eo.Objects = append(eo.Objects, o.Resources)
	}
	if mingw(tc) {
		eo.Link = append(eo.Link, "-municode", subsystem(o.Console))
	} else if o.Console {
		eo.Link = append(eo.Link, "-Wl,/subsystem:console")
	} else {
		eo.Link = append(eo.Link, "-Wl,/subsystem:windows", "-Wl,/entry:wWinMainCRTStartup")
	}
	eo.Link = append(eo.Link, WindowsLibs...)
	if o.Native != nil {
		eo.Link = append(eo.Link, o.Native.Libs...)
	}
	return tc.ExeWith(core, eo, out)
}

// subsystem is the MinGW linker's flag for a console program or a GUI
// one: the linker's own, which Zig's cc passes on (it drops gcc's
// -mwindows and -mconsole, and the program was a console one, a console
// window opening with it).
func subsystem(console bool) string {
	if console {
		return "-Wl,--subsystem,console"
	}
	return "-Wl,--subsystem,windows"
}

// mingw reports whether tc builds against MinGW-w64 (Zig's Windows target,
// a MinGW clang or gcc) rather than the Windows SDK (clang for MSVC).
func mingw(tc *cbuild.Toolchain) bool {
	if tc.CC.IsZig() {
		return true
	}
	out, err := cbuildOutput(tc.CC.Path, append(append([]string{}, tc.CC.Args...), "-dumpmachine")...)
	return err == nil && (strings.Contains(out, "mingw") || strings.Contains(out, "gnu"))
}

func cbuildOutput(name string, args ...string) (string, error) {
	out, err := exec.Command(name, args...).Output()
	return strings.TrimSpace(string(out)), err
}

// windowsHostUnit is the Windows host's C (platform/windows's, with all
// of platform/desktop's) as a unit.
func windowsHostUnit(core, win, dsk string, embedded bool) cbuild.Unit {
	var host []string
	for _, d := range []string{filepath.Join(win, "src"), filepath.Join(dsk, "src")} {
		m, _ := filepath.Glob(filepath.Join(d, "*.c"))
		host = append(host, m...)
	}
	sort.Strings(host)
	flags := append(append(append([]string{}, cbuild.Flags...), cbuild.Includes(core)...),
		"-I"+filepath.Join(win, "src"), "-I"+filepath.Join(dsk, "include"), "-DUNICODE", "-D_UNICODE")
	return cbuild.Unit{Name: "windows-host", Sources: host, Flags: flags, Embedded: embedded && desktop.Embedded() && cbuild.CoreEmbedded()}
}
