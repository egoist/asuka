package cbuild

import (
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"sort"
	"strings"

	"github.com/egoist/asuka/internal/ccache"
	"github.com/egoist/asuka/internal/prebuilt"
)

// Unit is C built into one static library per target and configuration
// (dev, release): the core's, a desktop host's, a std or sdk package's.
// A release of the CLI carries them prebuilt (internal/prebuilt: an npm
// package per target); else, or for sources that aren't the CLI's own
// (a checkout's), they're built into the cache, from the object cache.
type Unit struct {
	Name    string   // core, desktop, linux-host, windows-host, mac-shared; std-<name>, sdk-<name> (a package's desktop C); std-sqlite-c (a package's [c])
	Sources []string // its C (headers among them are skipped)
	// Flags are its own compile flags (warnings, include directories,
	// defines): the toolchain's and the configuration's come with them.
	Flags []string
	// Whole links it whole: its objects register themselves as the
	// program starts (constructors), and nothing names them.
	Whole bool
	// Embedded: its sources are the CLI's own copies, which its prebuilt
	// libraries were built from (else a checkout's, or another's).
	Embedded bool
	// Link is the macro core/link/link.c is compiled with in a build that
	// links it (AK_STD_SQLITE: std/sqlite's C, which the core calls), or
	// "".
	Link string
	// Fetch, when set, gets what its sources are in before they're
	// compiled (a [c.download] of its package's): a build that links the
	// prebuilt library never needs it.
	Fetch func() error
}

// Config is a build's configuration's name: dev (a player's: assertions
// on, optimized less, with symbols) or release.
func Config(debug bool) string {
	if debug {
		return "dev"
	}
	return "release"
}

// ConfigFlags are a configuration's flags.
func ConfigFlags(debug bool) []string {
	if debug {
		return []string{"-g", "-O1", "-DAK_DEBUG=1"}
	}
	return []string{"-O2", "-DNDEBUG"}
}

// base is what everything a build compiles gets: the toolchain's flags,
// a section per function and datum (the link drops what nothing uses),
// and the configuration's.
func (tc *Toolchain) base(debug bool) []string {
	f := append(append([]string{}, tc.Flags...), "-ffunction-sections", "-fdata-sections")
	return append(f, ConfigFlags(debug)...)
}

// CoreUnit is the core: core/src, compiled with the core's warnings and
// include directories, its symbols hidden but asuka.h's (ASUKA_API).
// Every build's core is the same: what differs (an app compiled in, its
// externs' glue) is LinkSource's, compiled per build.
func CoreUnit(core string) Unit {
	srcs, _ := Sources(core, false)
	flags := append(append(append([]string{}, Flags...), Includes(core)...), "-fvisibility=hidden")
	return Unit{Name: "core", Sources: srcs, Flags: flags, Embedded: CoreEmbedded()}
}

// coreFlags are what C compiled as the core is gets (an app compiled to
// C, its externs' glue, core/link): the core's unit's flags and the
// build's.
func (tc *Toolchain) coreFlags(core string, debug bool) []string {
	return append(append([]string{}, CoreUnit(core).Flags...), tc.base(debug)...)
}

// variant is the toolchain's prebuilt libraries' target: its target's
// name, or "" when it has none (a Linux program's musl, a compiler that
// isn't the one they're built with).
func (tc *Toolchain) variant() string {
	if tc.Musl {
		return ""
	}
	if !tc.links(prebuilt.Compiler(tc.Target.String())) {
		return ""
	}
	return tc.Target.String()
}

// links reports whether the toolchain links libraries the family built
// ("": none): its own family's, and on Linux Zig's with any compiler
// for glibc (gcc, clang), as Zig builds them for LinuxGlibc and they need
// nothing of Zig's (TestLinuxPrebuiltFromZig links them with both).
// Windows' are MinGW's, which a compiler for MSVC's runtime can't link.
func (tc *Toolchain) links(family string) bool {
	switch {
	case family == "":
		return false
	case family == tc.CC.kind():
		return true
	case family == "zig" && tc.Target.OS == "linux":
		return tc.CC.TargetsGlibc()
	}
	return false
}

// CompilerKind is the toolchain's compiler's family, as prebuilt
// libraries record what built them: zig, apple, ndk, or other.
func (tc *Toolchain) CompilerKind() string { return tc.CC.kind() }

// kind is the compiler's family, as prebuilt libraries name what they
// were built with: zig, apple (Xcode's clang), ndk, or another's.
func (c Compiler) kind() string {
	p := filepath.ToSlash(c.Path)
	switch {
	case c.IsZig():
		return "zig"
	case strings.Contains(p, "/ndk/"):
		return "ndk"
	case runtime.GOOS == "darwin" && (strings.Contains(p, ".app/Contents/Developer/") || strings.HasPrefix(p, "/Library/Developer/CommandLineTools/") ||
		p == "/usr/bin/cc" || p == "/usr/bin/clang"):
		return "apple"
	}
	return "other"
}

// UnitArchive is u's static library for the toolchain's target and the
// configuration: the CLI's prebuilt one when it has it and u's sources
// are its own, else one built into the cache.
func (tc *Toolchain) UnitArchive(u Unit, debug bool) (string, error) {
	if u.Embedded {
		if v := tc.variant(); v != "" {
			p, ok, err := prebuilt.Archive(v, Config(debug), u.Name)
			if err != nil {
				return "", err
			}
			if ok {
				return p, nil
			}
		}
	}
	return tc.BuildUnit(u, debug)
}

// BuildUnit builds u's static library into the cache (its objects from
// the object cache), as a release's prebuilt one is built.
func (tc *Toolchain) BuildUnit(u Unit, debug bool) (string, error) {
	// Its own flags after the toolchain's and the configuration's, so
	// they can change them (std/sqlite's -Os on the web), as an app's
	// [c] flags do.
	flags := append(tc.base(debug), u.Flags...)
	if u.Fetch != nil {
		if err := u.Fetch(); err != nil {
			return "", fmt.Errorf("cbuild: %s: %v", u.Name, err)
		}
	}
	objs, err := tc.CC.CompileCached(u.Sources, flags)
	if err != nil {
		return "", fmt.Errorf("cbuild: %s for %s: %v", u.Name, tc.Target, err)
	}
	if len(objs) == 0 {
		return "", nil
	}
	return tc.ArchiveCached(objs)
}

// archiveRef is a static library a link takes, and whether whole.
type archiveRef struct {
	path  string
	whole bool
}

// archiveArgs are a link's arguments for archives, after its objects:
// those it takes whole forced in, the others searched for what the
// objects name (as a group, on ELF: they name each other).
func (tc *Toolchain) archiveArgs(as []archiveRef) []string {
	var out []string
	switch tc.Target.OS {
	case "darwin", "ios", "ios-simulator":
		for _, a := range as {
			if a.whole {
				out = append(out, "-Wl,-force_load,"+a.path)
			} else {
				out = append(out, a.path)
			}
		}
		return out
	case "windows":
		if !tc.CC.IsZig() && !tc.mingw() {
			for _, a := range as {
				if a.whole {
					out = append(out, "-Wl,/WHOLEARCHIVE:"+a.path)
				} else {
					out = append(out, a.path)
				}
			}
			return out
		}
	}
	group := tc.Target.OS != "wasm" && tc.Target.OS != "windows"
	if group {
		out = append(out, "-Wl,--start-group")
	}
	for _, a := range as {
		if a.whole {
			out = append(out, "-Wl,--whole-archive", a.path, "-Wl,--no-whole-archive")
		} else {
			out = append(out, a.path)
		}
	}
	if group {
		out = append(out, "-Wl,--end-group")
	}
	return out
}

// mingw reports whether a Windows toolchain builds against MinGW-w64
// (Zig's, a MinGW clang or gcc) rather than the Windows SDK.
func (tc *Toolchain) mingw() bool {
	if tc.CC.IsZig() {
		return true
	}
	out, err := output(tc.CC.Path, append(append([]string{}, tc.CC.Args...), "-dumpmachine")...)
	return err == nil && (strings.Contains(out, "mingw") || strings.Contains(out, "gnu"))
}

// linkSource compiles core/link/link.c for a build: whether an app is
// compiled in, whether its externs' glue is, the units it links that the
// core calls (their Link macros), and the builtins the app calls.
func (tc *Toolchain) linkSource(core string, debug, app, ffi bool, units []Unit, builtins []string, more ...string) ([]Object, error) {
	flags := append(append(tc.coreFlags(core, debug), linkDefines(app, ffi, units, builtins)...), more...)
	return tc.CC.CompileCached([]string{LinkSource(core)}, flags)
}

// linkDefines are core/link/link.c's macros for a build (linkSource). An
// app compiled in registers the std packages' tables of the builtins it
// calls (AK_USES_FS for fs.join: what's before the dot), and no others,
// which the link then leaves out; builtins nil: every table.
func linkDefines(app, ffi bool, units []Unit, builtins []string) []string {
	var flags []string
	if app {
		flags = append(flags, "-DAK_APP_COMPILED=1")
		flags = append(flags, usesDefines(builtins)...)
	}
	if ffi {
		flags = append(flags, "-DAK_FFI=1")
	}
	for _, u := range units {
		if u.Link != "" {
			flags = append(flags, "-D"+u.Link+"=1")
		}
	}
	return flags
}

// usesDefines are the AK_USES_<NAME> macros of builtins' prefixes, sorted
// (the link's object is cached by its flags); none given: AK_USES_ALL.
func usesDefines(builtins []string) []string {
	if builtins == nil {
		return []string{"-DAK_USES_ALL=1"}
	}
	seen := map[string]bool{}
	var out []string
	for _, b := range builtins {
		name, _, ok := strings.Cut(b, ".")
		if !ok || name == "" {
			continue
		}
		m := strings.Map(func(r rune) rune {
			switch {
			case r >= 'a' && r <= 'z':
				return r - 'a' + 'A'
			case r >= 'A' && r <= 'Z', r >= '0' && r <= '9':
				return r
			}
			return '_'
		}, name)
		if !seen[m] {
			seen[m] = true
			out = append(out, "-DAK_USES_"+m+"=1")
		}
	}
	sort.Strings(out)
	if out == nil {
		out = []string{}
	}
	return out
}

// link links objects and archives into out (written whole, then renamed:
// another build may be running it).
func (tc *Toolchain) link(objs []string, as []archiveRef, ld []string, out string) error {
	if err := os.MkdirAll(filepath.Dir(out), 0o755); err != nil {
		return err
	}
	args := append(append([]string{}, objs...), tc.archiveArgs(as)...)
	tmp := ccache.TmpName(out)
	if err := tc.CC.Link(args, tmp, ld); err != nil {
		os.Remove(tmp)
		return err
	}
	// lld's PDB beside a Windows executable, which nothing reads.
	os.Remove(strings.TrimSuffix(tmp, filepath.Ext(tmp)) + ".pdb")
	if err := os.Rename(tmp, out); err != nil {
		os.Remove(tmp)
		return err
	}
	return nil
}
