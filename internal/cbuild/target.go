package cbuild

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"

	"github.com/egoist/asuka/internal/androidsdk"
	"github.com/egoist/asuka/internal/ccache"
)

// Target is what the core is built for.
type Target struct {
	// OS: darwin, ios, ios-simulator, tvos, tvos-simulator, watchos,
	// watchos-simulator, android, linux, windows, wasm.
	OS string
	// Arch: arm64, amd64, arm, 386; arm64_32 for an Apple Watch's
	// (ILP32: 64-bit registers, 32-bit pointers); wasm32 for wasm.
	Arch string
}

func (t Target) String() string { return t.OS + "-" + t.Arch }

// Apple reports whether t is one of Apple's systems (Xcode's SDKs):
// macOS, iOS, tvOS, watchOS, their simulators.
func (t Target) Apple() bool {
	switch t.OS {
	case "darwin", "ios", "ios-simulator", "tvos", "tvos-simulator", "watchos", "watchos-simulator":
		return true
	}
	return false
}

// Host is the computer's own target.
func Host() Target {
	return Target{OS: runtime.GOOS, Arch: runtime.GOARCH}
}

// Minimum versions (DESIGN.md: iOS 16, Android API 33, macOS 11; tvOS
// 17, the Apple TV's iOS 16 counterpart with UIKit's menus on buttons;
// watchOS 10, the first with SwiftUI's Observation, which the watch's
// host is written with, and Xcode's recommended one).
const (
	IOSMin     = "16.0"
	TVOSMin    = "17.0"
	WatchOSMin = "10.0"
	MacOSMin   = "11.0"
	AndroidAPI = "24"
	// LinuxGlibc is the glibc Zig builds Linux's C against: what it calls
	// is in this one and every later one, so its libraries link with any
	// glibc system's compiler and its executables run there (Debian 10,
	// Ubuntu 18.10, RHEL 8 on; GTK 4.14 needs far newer).
	LinuxGlibc = "2.28"
)

// appleToolchain is Xcode's clang for an SDK (iphoneos, appletvsimulator...)
// and its target triple.
func appleToolchain(t Target, sdk, triple string) (*Toolchain, error) {
	root, err := output("xcrun", "--sdk", sdk, "--show-sdk-path")
	if err != nil {
		return nil, fmt.Errorf("cbuild: Xcode's %s SDK: %v (install Xcode)", sdk, err)
	}
	clang, err := output("xcrun", "--sdk", sdk, "--find", "clang")
	if err != nil {
		return nil, err
	}
	return &Toolchain{
		Target: t,
		CC:     Compiler{Path: clang},
		Flags:  []string{"-target", triple, "-isysroot", root},
		AR:     []string{"xcrun", "libtool", "-static", "-D", "-o"},
		Shared: []string{"-dynamiclib"},
	}, nil
}

// clangArch is an architecture's name in target triples.
func clangArch(arch string) string {
	switch arch {
	case "amd64":
		return "x86_64"
	case "arm64":
		return "aarch64"
	case "386":
		return "i686"
	case "arm":
		return "armv7a"
	}
	return arch
}

// Toolchain is a compiler and what it needs to build for a target: its
// flags, its archiver, how it links a shared library.
type Toolchain struct {
	Target Target
	CC     Compiler
	Flags  []string // before every compile and link
	// AR makes a static library: the command and its arguments before
	// the output and the objects.
	AR []string
	// Shared links a shared library (or, for wasm, the module).
	Shared []string
	// Musl: a Linux program's, linked statically with musl (Musl).
	Musl bool
}

// ForTarget finds the toolchain for t: the platform's own compiler where
// there is one (Xcode's for Apple platforms, the NDK's for Android, the
// system's for this computer), else Zig's, which cross-compiles to
// anything. ASUKA_CC=zig makes it Zig's always.
func ForTarget(t Target) (*Toolchain, error) {
	forceZig := os.Getenv("ASUKA_CC") == "zig"
	switch t.OS {
	case "ios", "ios-simulator":
		if runtime.GOOS != "darwin" {
			return nil, fmt.Errorf("cbuild: building for iOS needs a Mac with Xcode")
		}
		sdk := "iphoneos"
		triple := "arm64-apple-ios" + IOSMin
		if t.OS == "ios-simulator" {
			sdk = "iphonesimulator"
			triple = clangArchApple(t.Arch) + "-apple-ios" + IOSMin + "-simulator"
		}
		return appleToolchain(t, sdk, triple)
	case "tvos", "tvos-simulator":
		if runtime.GOOS != "darwin" {
			return nil, fmt.Errorf("cbuild: building for tvOS needs a Mac with Xcode")
		}
		sdk := "appletvos"
		triple := "arm64-apple-tvos" + TVOSMin
		if t.OS == "tvos-simulator" {
			sdk = "appletvsimulator"
			triple = clangArchApple(t.Arch) + "-apple-tvos" + TVOSMin + "-simulator"
		}
		return appleToolchain(t, sdk, triple)
	case "watchos", "watchos-simulator":
		if runtime.GOOS != "darwin" {
			return nil, fmt.Errorf("cbuild: building for watchOS needs a Mac with Xcode")
		}
		// A watch is arm64_32 (Series 4 to 8, SE, Ultra 1) or arm64 (Series
		// 9 and later, Ultra 2 and later): a device build is both, lipo'd.
		sdk := "watchos"
		triple := clangArchApple(t.Arch) + "-apple-watchos" + WatchOSMin
		if t.OS == "watchos-simulator" {
			sdk = "watchsimulator"
			triple += "-simulator"
		}
		return appleToolchain(t, sdk, triple)
	case "android":
		sys := androidsdk.Local()
		ndk, bin, err := sys.NDK(sys.SDK())
		if err != nil {
			return nil, err
		}
		_ = ndk
		clang := sys.Clang(bin)
		triple := clangArch(t.Arch) + "-linux-android"
		if t.Arch == "arm" {
			triple = "armv7a-linux-androideabi"
		}
		return &Toolchain{
			Target: t,
			CC:     Compiler{Path: clang},
			Flags:  []string{"--target=" + triple + AndroidAPI, "-fPIC"},
			AR:     []string{filepath.Join(bin, sys.Exe("llvm-ar")), "rcsD"},
			Shared: []string{"-shared", "-Wl,-z,max-page-size=16384", "-llog"},
		}, nil
	case "wasm":
		z, err := Zig()
		if err != nil {
			return nil, fmt.Errorf("cbuild: building for the web needs Zig (ASUKA_CC=zig): %v", err)
		}
		return &Toolchain{
			Target: t,
			CC:     z,
			Flags:  []string{"-target", "wasm32-wasi", "-mbulk-memory"},
			AR:     []string{z.Path, "ar", "rcsD"},
			Shared: []string{"-Wl,--no-entry", "-Wl,--export-dynamic", "-mexec-model=reactor"},
		}, nil
	}
	// Desktops: this computer's own compiler for its own system and
	// architecture (a Mac's for both of its); Zig to cross-compile.
	if t.OS == runtime.GOOS && (t.Arch == runtime.GOARCH || t.OS == "darwin") && !forceZig {
		cc, err := Find()
		if err != nil {
			return nil, err
		}
		tc := &Toolchain{Target: t, CC: cc}
		switch t.OS {
		case "darwin":
			tc.Flags = []string{"-arch", clangArchApple(t.Arch), "-mmacosx-version-min=" + MacOSMin}
			tc.AR = []string{"xcrun", "libtool", "-static", "-D", "-o"}
			tc.Shared = []string{"-dynamiclib", "-install_name", "@rpath/libasuka.dylib"}
		case "windows":
			tc.AR = []string{"llvm-ar", "rcsD"}
			tc.Shared = []string{"-shared"}
		default:
			tc.Flags = []string{"-fPIC"}
			tc.AR = []string{"ar", "rcsD"}
			tc.Shared = []string{"-shared"}
		}
		if tc.CC.IsGCC() {
			tc.Flags = append(tc.Flags, GCCFlags...)
		}
		return tc, nil
	}
	z, err := Zig()
	if err != nil {
		return nil, fmt.Errorf("cbuild: cross-compiling for %s needs Zig: %v", t, err)
	}
	triple := map[string]string{"linux": "linux-gnu." + LinuxGlibc, "windows": "windows-gnu", "darwin": "macos"}[t.OS]
	if triple == "" {
		return nil, fmt.Errorf("cbuild: no toolchain for %s", t)
	}
	tc := &Toolchain{Target: t, CC: z, Flags: []string{"-target", clangArch(t.Arch) + "-" + triple}, AR: []string{z.Path, "ar", "rcsD"}, Shared: []string{"-shared"}}
	if t.OS == "linux" {
		tc.Flags = append(tc.Flags, "-fPIC")
	}
	return tc, nil
}

func clangArchApple(arch string) string {
	switch arch {
	case "amd64":
		return "x86_64"
	case "arm64_32":
		return "arm64_32"
	}
	return "arm64"
}

func output(name string, args ...string) (string, error) {
	out, err := exec.Command(name, args...).Output()
	if err != nil {
		return "", fmt.Errorf("%s %s: %v", name, strings.Join(args, " "), err)
	}
	return strings.TrimSpace(string(out)), nil
}

// LibKind is what Lib makes.
type LibKind int

const (
	Static LibKind = iota // libasuka.a: linked into an app (iOS, the Mac)
	Shared                // libasuka.so: loaded by a host (Android)
	Module                // asuka.wasm: the web's
)

// Lib builds the core (and extra sources: an app compiled to C) for the
// toolchain's target into out, optimized for release unless debug: the
// core's library (prebuilt, or built into the cache), the extra sources'
// objects from the object cache, so a new build of an app compiles only
// the app.
func (tc *Toolchain) Lib(core string, kind LibKind, extra []string, debug bool, out string) error {
	return tc.LibWith(core, kind, LibOptions{Extra: extra, Debug: debug}, out)
}

// LibOptions say what Lib builds the core with.
type LibOptions struct {
	Extra   []string // more of the core's C: an app compiled to C
	Foreign *Foreign // the C an app's externs call
	Debug   bool     // a player's build: assertions on, optimized less
	// Builtins are the ones the app compiled in calls (its module's):
	// only the std packages' tables of them are linked. nil: every table.
	Builtins []string
	// Defines are more of core/link/link.c's macros: a phone release's
	// AK_FINGERPRINT and its updates' feed and key (RFC 0006).
	Defines []string
}

// LibWith is Lib as o says: with o.Foreign, its glue and units are
// compiled in, and a shared library or a module links what it links. A
// static library is the core's with the build's objects added (libtool);
// a shared library or a module links the core's whole (nothing in it
// names the core: its exports are its roots).
func (tc *Toolchain) LibWith(core string, kind LibKind, o LibOptions, out string) error {
	defer ccache.Flush()
	debug, ffi := o.Debug, o.Foreign
	coreLib, err := tc.UnitArchive(CoreUnit(core), debug)
	if err != nil {
		return err
	}
	flags := tc.coreFlags(core, debug)
	// What the app's own C gets of the core's flags: the target, the
	// optimization, the C core's include directory (asuka_ffi.h).
	cflags := append([]string{"-I" + filepath.Join(core, "include")}, tc.base(debug)...)
	if kind != Static {
		cflags = append(cflags, "-fvisibility=hidden")
	}
	link, err := tc.linkSource(core, debug, len(o.Extra) > 0, ffi.glue(), ffi.libs(), o.Builtins, o.Defines...)
	if err != nil {
		return err
	}
	objs := Paths(link)
	more, err := tc.CC.CompileCached(o.Extra, append(append([]string{}, flags...), AppFlags(tc.CC, tc.Target)...))
	if err != nil {
		return err
	}
	objs = append(objs, Paths(more)...)
	if ffi.glue() {
		glue, err := tc.CC.CompileCached([]string{ffi.Glue}, flags)
		if err != nil {
			return err
		}
		objs = append(objs, Paths(glue)...)
	}
	if ffi != nil {
		units, err := ffi.objects(tc.CC, cflags)
		if err != nil {
			return err
		}
		objs = append(objs, units...)
	}
	// The std packages' C the app imports (std/sqlite's): their libraries,
	// which link.c names.
	var libs []archiveRef
	for _, u := range ffi.libs() {
		p, err := tc.UnitArchive(u, debug)
		if err != nil {
			return err
		}
		if p != "" {
			libs = append(libs, archiveRef{p, false})
		}
	}
	if err := os.MkdirAll(filepath.Dir(out), 0o755); err != nil {
		return err
	}
	if kind == Static {
		members := []string{coreLib}
		for _, l := range libs {
			members = append(members, l.path)
		}
		if !tc.archivesArchives() {
			dir, err := os.MkdirTemp(filepath.Dir(out), ".asuka-ar")
			if err != nil {
				return err
			}
			defer os.RemoveAll(dir)
			if members, err = tc.extract(members, dir); err != nil {
				return err
			}
		}
		tmp := ccache.TmpName(out)
		if err := tc.archive(tmp, append(members, objs...)); err != nil {
			os.Remove(tmp)
			return err
		}
		return os.Rename(tmp, out)
	}
	ld := append(append([]string{}, tc.Flags...), tc.Shared...)
	if kind == Module {
		ld = append(ld, exportFlags()...)
	}
	if ffi != nil {
		ld = append(ld, ffi.Link...)
	}
	ld = append(ld, "-lm")
	switch tc.Target.OS {
	case "linux":
		ld = append(ld, "-lpthread", "-ldl")
	case "windows":
		ld = append(ld, "-lws2_32") // asuka dev's connection (abi/dev.c)
		if tc.CC.IsZig() {
			// lld writes an import library beside the DLL, named for an
			// object; the hosts load the DLL by its path and need none.
			implib := filepath.Join(os.TempDir(), fmt.Sprintf("asuka-%d.lib", os.Getpid()))
			ld = append(ld, "-Wl,--out-implib,"+implib)
			defer os.Remove(implib)
		}
	}
	if !debug {
		ld = append(ld, deadStrip(tc.Target)...)
	}
	return tc.link(objs, append([]archiveRef{{coreLib, true}}, libs...), ld, out)
}

// Objects compiles srcs, C a host links beside the core (the C the
// desktop hosts share, in the Mac's host), with the toolchain's flags and
// flags, into the object cache, and returns the objects.
func (tc *Toolchain) Objects(srcs, flags []string) ([]string, error) {
	defer ccache.Flush()
	objs, err := tc.CC.CompileCached(srcs, append(append([]string{}, tc.Flags...), flags...))
	return Paths(objs), err
}

// deadStrip drops what nothing calls, and symbols and debug information
// a release doesn't need.
func deadStrip(t Target) []string {
	switch t.OS {
	case "android":
		// Its symbols stay for Gradle, which strips what it packages and
		// keeps them for crash reports (native-debug-symbols.zip).
		return []string{"-Wl,--gc-sections"}
	case "darwin", "ios", "ios-simulator", "tvos", "tvos-simulator", "watchos", "watchos-simulator":
		return []string{"-Wl,-dead_strip", "-Wl,-x"}
	case "windows":
		return []string{"-s"}
	}
	return []string{"-Wl,--gc-sections", "-s"}
}

// exportFlags are the wasm module's exports: the C ABI (asuka.h), and
// allocation for the JavaScript host to pass bytes in.
func exportFlags() []string {
	var f []string
	for _, name := range []string{
		"asuka_start", "asuka_load", "asuka_event", "asuka_list_count", "asuka_list_bind", "asuka_list_unbind",
		"asuka_run_posted", "asuka_measure_max", "asuka_attach", "asuka_timer_fire", "asuka_native_done", "asuka_native_invoke", "asuka_alloc", "asuka_free",
		"asuka_run", "asuka_dev_reload", "asuka_dev_restart", "asuka_dev_frame", "asuka_free_string", "asuka_web_start", "asuka_io", "asuka_set_app_dir", "asuka_set_dir", "asuka_set_start",
		// spawn on the page's workers (core/src/abi/web.c)
		"asuka_web_workers", "asuka_spawn_done", "asuka_worker_start", "asuka_worker_call", "asuka_worker_answered",
		// std/sqlite's answers from the page's SQLite worker (core/src/sqlite/sqlite.c)
		"asuka_sqlite_done",
		// platform/crash's record (core/src/std/crash.c)
		"asuka_crash_start", "asuka_crash_notify", "asuka_crash_where", "asuka_crash_read",
	} {
		f = append(f, "-Wl,--export="+name)
	}
	return f
}

// ExeOptions say what ExeWith builds an executable of besides the core.
type ExeOptions struct {
	// Units are libraries it links (a host's, the native registry's,
	// packages': prebuilt, or built into the cache), Whole ones whole.
	Units []Unit
	// Sources are C compiled for the build with the core's flags and
	// Flags (an app's own native packages', a host test's), Objects are
	// linked as they are (a Windows resource object).
	Sources, Flags, Objects []string
	// App is the app compiled to C (lang/cgen), or "" for asuka dev's
	// player, which loads bytecode.
	App string
	// Link are what the link takes besides the C library.
	Link []string
	// Foreign is the C the app's externs call, and the std packages' it
	// imports (std/sqlite's SQLite).
	Foreign *Foreign
	Debug   bool
	// Builtins are the ones App calls (its module's): only the std
	// packages' tables of them are linked. nil: every table.
	Builtins []string
}

// ExeWith builds an executable for the toolchain's target into out: the
// core's library and o's units' (prebuilt, or built into the cache), its
// sources' objects from the object cache, optimized for release unless
// o.Debug.
func (tc *Toolchain) ExeWith(core string, o ExeOptions, out string) error {
	defer ccache.Flush()
	coreLib, err := tc.UnitArchive(CoreUnit(core), o.Debug)
	if err != nil {
		return err
	}
	as := []archiveRef{{coreLib, false}}
	units := append(append([]Unit{}, o.Units...), o.Foreign.libs()...)
	for _, u := range units {
		p, err := tc.UnitArchive(u, o.Debug)
		if err != nil {
			return err
		}
		if p != "" {
			as = append(as, archiveRef{p, u.Whole})
		}
	}
	flags := tc.coreFlags(core, o.Debug)
	link, err := tc.linkSource(core, o.Debug, o.App != "", o.Foreign.glue(), units, o.Builtins)
	if err != nil {
		return err
	}
	objs := Paths(link)
	srcs := []string{}
	if o.App != "" {
		srcs = append(srcs, o.App)
	}
	if o.Foreign.glue() {
		srcs = append(srcs, o.Foreign.Glue)
	}
	if len(srcs) > 0 {
		app, err := tc.CC.CompileCached(srcs, append(append([]string{}, flags...), AppFlags(tc.CC, tc.Target)...))
		if err != nil {
			return err
		}
		objs = append(objs, Paths(app)...)
	}
	if o.Foreign != nil {
		cflags := append([]string{"-I" + filepath.Join(core, "include")}, tc.base(o.Debug)...)
		fobjs, err := o.Foreign.objects(tc.CC, cflags)
		if err != nil {
			return err
		}
		objs = append(objs, fobjs...)
	}
	more, err := tc.CC.CompileCached(o.Sources, append(append([]string{}, flags...), o.Flags...))
	if err != nil {
		return err
	}
	objs = append(append(objs, Paths(more)...), o.Objects...)
	ld := append(append([]string{}, tc.Flags...), "-lm")
	switch tc.Target.OS {
	case "linux":
		ld = append(ld, "-lpthread", "-ldl")
	case "windows":
		ld = append(ld, "-lws2_32")
	}
	ld = append(ld, o.Link...)
	if o.Foreign != nil {
		ld = append(ld, o.Foreign.Link...)
	}
	if !o.Debug {
		ld = append(ld, deadStrip(tc.Target)...)
	}
	return tc.link(objs, as, ld, out)
}
