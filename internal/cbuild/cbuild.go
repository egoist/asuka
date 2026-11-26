// Package cbuild builds Asuka's C core (core/) with a C compiler run as a
// program: the system's (cc: Xcode's clang, gcc or clang on Linux,
// clang on Windows) by default, or Zig's (zig cc), opted into, which
// cross-compiles to every target from any system. Nothing here uses cgo.
package cbuild

import (
	"bytes"
	"fmt"
	"io/fs"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"sort"
	"strings"
	"sync"

	"github.com/egoist/asuka/internal/ccache"
)

// Compiler is a C compiler: its program and the arguments that come
// before every command line (zig's "cc", a target).
type Compiler struct {
	Path string
	Args []string
}

// Find returns the compiler to use for this system:
//
//   - ASUKA_CC=zig: Zig's (zig cc);
//   - ASUKA_CC or CC set to a command: that one ("clang -fcolor-diagnostics");
//   - else the system's: cc, clang or gcc, the first found.
func Find() (Compiler, error) {
	cc := os.Getenv("ASUKA_CC")
	if cc == "" {
		cc = os.Getenv("CC")
	}
	if cc == "zig" {
		return Zig()
	}
	if cc != "" {
		f := strings.Fields(cc)
		p, err := exec.LookPath(f[0])
		if err != nil {
			return Compiler{}, fmt.Errorf("cbuild: the C compiler %q (ASUKA_CC or CC) isn't installed", f[0])
		}
		return Compiler{Path: p, Args: f[1:]}, nil
	}
	for _, name := range []string{"cc", "clang", "gcc"} {
		if p, err := exec.LookPath(name); err == nil {
			return Compiler{Path: p}, nil
		}
	}
	return Compiler{}, fmt.Errorf("cbuild: no C compiler: install your system's (Xcode's command line tools, build-essential, LLVM) or Zig with ASUKA_CC=zig")
}

// Zig returns Zig's C compiler, for builds that cross-compile (Windows
// or Linux from a Mac, the web without an LLVM that targets wasm) or
// when asked for (ASUKA_CC=zig).
func Zig() (Compiler, error) {
	p, err := exec.LookPath("zig")
	if err != nil {
		return Compiler{}, fmt.Errorf("cbuild: zig isn't installed (https://ziglang.org/download)")
	}
	return Compiler{Path: p, Args: []string{"cc"}}, nil
}

// IsZig reports whether c is Zig's.
func (c Compiler) IsZig() bool {
	return len(c.Args) > 0 && c.Args[0] == "cc" && strings.Contains(filepath.Base(c.Path), "zig")
}

// IsGCC reports whether the compiler is GCC (not clang, which defines
// __GNUC__ too), asking it once.
func (c Compiler) IsGCC() bool {
	key := c.Path + " " + strings.Join(c.Args, " ")
	gccMu.Lock()
	defer gccMu.Unlock()
	if v, ok := gccSeen[key]; ok {
		return v
	}
	args := append(append([]string{}, c.Args...), "-dM", "-E", "-x", "c", os.DevNull)
	out, err := exec.Command(c.Path, args...).Output()
	v := err == nil && strings.Contains(string(out), "__GNUC__") && !strings.Contains(string(out), "__clang__")
	gccSeen[key] = v
	return v
}

var (
	gccMu   sync.Mutex
	gccSeen = map[string]bool{}
)

// TargetsGlibc reports whether the compiler builds for Linux with glibc
// (its target x86_64-linux-gnu, aarch64-unknown-linux-gnu; not musl's),
// asking it once.
func (c Compiler) TargetsGlibc() bool {
	key := "glibc " + c.Path + " " + strings.Join(c.Args, " ")
	gccMu.Lock()
	defer gccMu.Unlock()
	if v, ok := gccSeen[key]; ok {
		return v
	}
	out, err := exec.Command(c.Path, append(append([]string{}, c.Args...), "-dumpmachine")...).Output()
	m := strings.TrimSpace(string(out))
	v := err == nil && strings.Contains(m, "-linux-gnu")
	gccSeen[key] = v
	return v
}

// GCCFlags are what GCC is given beyond Flags: its format-truncation
// warning (-Wall's at -O2) guesses at the lengths of strings it can't
// know, and -Werror would make a guess an error.
var GCCFlags = []string{"-Wno-format-truncation"}

// AppFlags are what the C an app compiles to (lang/cgen's app.c, and
// what's compiled with it) is compiled with for t beyond the core's: on
// arm64, x28 reserved for the thread's stack limit (core/src/vm/ops.h's
// AK_CG_X28), which compiled code compares the stack pointer with, as Go
// keeps its g in R28. The core needn't be: x28 is callee-saved, so it
// keeps its own across its calls into the app, and the app's ways in set
// and restore it. Without them the app's C passes the limit as an
// argument, as on amd64. Windows on ARM64 takes the argument too: there
// the app's code found x28 not holding the limit (every app threw a
// stack overflow at its first component, its stack nearly empty).
func AppFlags(t Target) []string {
	if t.Arch == "arm64" && t.OS != "windows" {
		return []string{"-ffixed-x28", "-DAK_CG_X28"}
	}
	return nil
}

// Flags are the flags every file of the core is compiled with.
var Flags = []string{"-std=c17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-Wno-missing-field-initializers"}

// Sources returns core's C files under dir (core/src; with fake, the
// fake host's, core/host/fake, and core/link's, which a test's build
// compiles with the core's flags), sorted. A build's link adds LinkSource,
// compiled with its own defines.
func Sources(core string, fake bool) ([]string, error) {
	var out []string
	dirs := []string{filepath.Join(core, "src")}
	if fake {
		dirs = append(dirs, filepath.Join(core, "host", "fake"), filepath.Join(core, "link"))
	}
	for _, d := range dirs {
		err := filepath.WalkDir(d, func(p string, e fs.DirEntry, err error) error {
			if err != nil {
				return err
			}
			if !e.IsDir() && strings.HasSuffix(p, ".c") {
				out = append(out, p)
			}
			return nil
		})
		if err != nil && !os.IsNotExist(err) {
			return nil, err
		}
	}
	sort.Strings(out)
	return out, nil
}

// Includes are the include directories of the core (with the public
// headers of the Mbed TLS it vendors: mbedtls/, psa/; and mimalloc's,
// which its allocator compiles in: core/src/base/alloc.c).
func Includes(core string) []string {
	return []string{"-I" + filepath.Join(core, "include"), "-I" + filepath.Join(core, "src"), "-I" + filepath.Join(core, "host"), "-I" + filepath.Join(core, "third_party", "mbedtls", "include"), "-I" + filepath.Join(core, "third_party", "mimalloc", "include")}
}

// Compile compiles each source to an object in out, in parallel, and
// returns the objects. extra flags come after Flags. An object already
// in out is kept (out is a cache keyed by the sources and flags), and
// each is written under a name of its own then renamed into place, so
// builds running at once share the cache safely.
func (c Compiler) Compile(srcs []string, out string, extra []string) ([]string, error) {
	return c.compile(srcs, out, append(append([]string{}, Flags...), extra...))
}

// CompileWith is Compile with flags alone, not the core's: C that isn't
// the core's (an app's [c] sources), whose warnings are its own.
func (c Compiler) CompileWith(srcs []string, out string, flags []string) ([]string, error) {
	return c.compile(srcs, out, flags)
}

func (c Compiler) compile(srcs []string, out string, flags []string) ([]string, error) {
	if err := os.MkdirAll(out, 0o755); err != nil {
		return nil, err
	}
	objs := make([]string, len(srcs))
	errs := make([]error, len(srcs))
	sem := make(chan struct{}, runtime.NumCPU())
	var wg sync.WaitGroup
	for i, src := range srcs {
		name := strings.NewReplacer("/", "_", "\\", "_", ":", "_").Replace(src)
		objs[i] = filepath.Join(out, strings.TrimSuffix(name, ".c")+".o")
		wg.Add(1)
		go func(i int, src string) {
			defer wg.Done()
			sem <- struct{}{}
			defer func() { <-sem }()
			if _, err := os.Stat(objs[i]); err == nil {
				return
			}
			tmp := tmpName(objs[i])
			args := append(append([]string{}, c.Args...), flags...)
			args = append(args, "-c", src, "-o", tmp)
			if errs[i] = c.run(args); errs[i] == nil {
				errs[i] = os.Rename(tmp, objs[i])
			}
		}(i, src)
	}
	wg.Wait()
	var msgs []string
	for _, e := range errs {
		if e != nil {
			msgs = append(msgs, e.Error())
		}
	}
	if msgs != nil {
		return nil, fmt.Errorf("%s", strings.Join(msgs, "\n"))
	}
	return objs, nil
}

// Link links objects into the executable exe.
func (c Compiler) Link(objs []string, exe string, extra []string) error {
	args := append(append([]string{}, c.Args...), objs...)
	args = append(args, extra...)
	args = append(args, "-o", exe)
	return c.run(args)
}

func (c Compiler) run(args []string) error {
	cmd := exec.Command(c.Path, args...)
	var out bytes.Buffer
	cmd.Stdout, cmd.Stderr = &out, &out
	if err := cmd.Run(); err != nil {
		return fmt.Errorf("%s %s: %v\n%s", filepath.Base(c.Path), strings.Join(args, " "), err, out.String())
	}
	return nil
}

// Runner builds asuka-run (core/runner: the VM, the core and the fake
// host) into the cache, once per version of its objects, and returns its
// path. sanitize builds it with ASan and UBSan (tests).
func Runner(core string, sanitize bool) (string, error) { return RunnerWith(core, nil, sanitize, "") }

// RunnerWith is Runner with more C sources built in (an app compiled to
// C, lang/cgen, which asuka-run -aot runs), linked into dir, a directory
// of the caller's that it removes. Every object comes from the object
// cache; with no extra sources (dir ""), the runner itself is cached.
func RunnerWith(core string, extra []string, sanitize bool, dir string) (string, error) {
	return RunnerFor(core, RunnerOptions{Extra: extra, Sanitize: sanitize, Dir: dir})
}

// RunnerOptions say what asuka-run is built with.
type RunnerOptions struct {
	Extra    []string // more of the core's C: an app compiled to C
	Foreign  *Foreign // the C an app's externs call
	Sanitize bool     // ASan and UBSan (tests)
	// Dir is the caller's directory (it removes it) the runner is linked
	// into when it has Extra or Foreign sources, which differ from run to
	// run.
	Dir string
	// Builtins are the ones Extra's app calls (its module's): only the std
	// packages' tables of them are linked. nil: every table.
	Builtins []string
}

// testFlags are a test build's (the runner's, a host test's): with
// symbols, optimized a little, with ASan and UBSan when sanitize (not on
// Windows); and what its link adds.
func testFlags(core string, sanitize bool) (flags, opt, link []string) {
	flags = append(append(append([]string{}, Flags...), Includes(core)...), "-g", "-O1")
	link = []string{"-lm"}
	if runtime.GOOS == "linux" {
		link = append(link, "-lpthread", "-ldl")
	}
	if sanitize && runtime.GOOS != "windows" {
		opt = []string{"-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "-fno-omit-frame-pointer"}
		flags = append(append(flags, opt...), "-DAK_TEST=1")
		link = append(link, "-fsanitize=address,undefined")
	} else {
		opt = []string{"-O2"}
		flags = append(flags, "-O2")
	}
	return flags, opt, link
}

// RunnerFor is Runner built as o says.
func RunnerFor(core string, o RunnerOptions) (string, error) {
	defer ccache.Flush()
	f := o.Foreign
	cc, err := Find()
	if err != nil {
		return "", err
	}
	srcs, err := Sources(core, true)
	if err != nil {
		return "", err
	}
	runner, _ := filepath.Glob(filepath.Join(core, "runner", "*.c"))
	srcs = append(srcs, runner...)
	flags, opt, link := testFlags(core, o.Sanitize)
	own := len(o.Extra) > 0 || f != nil
	if own {
		// core/link/link.c, with what the build has.
		var keep []string
		for _, s := range srcs {
			if s != LinkSource(core) {
				keep = append(keep, s)
			}
		}
		srcs = keep
	}
	objs, err := cc.CompileCached(srcs, flags)
	if err != nil {
		return "", err
	}
	name := "asuka-run"
	if runtime.GOOS == "windows" {
		name += ".exe"
	}
	if !own {
		key := ccache.Key(append([]string{"c-runner-1", cc.Identity(), strings.Join(link, "\x00")}, Keys(objs)...)...)
		exe := filepath.Join(ccache.Sharded("c", "x", key, ""), name)
		if _, err := os.Stat(exe); err == nil {
			ccache.Touch(filepath.Dir(exe))
			return exe, nil
		}
		if err := os.MkdirAll(filepath.Dir(exe), 0o755); err != nil {
			return "", err
		}
		tmp := ccache.TmpName(exe)
		if err := cc.Link(Paths(objs), tmp, link); err != nil {
			os.Remove(tmp)
			return "", err
		}
		return exe, ccache.Commit(tmp, exe)
	}
	if o.Dir == "" {
		return "", fmt.Errorf("cbuild: a runner with extra sources needs a directory of the caller's")
	}
	lflags := append(append([]string{}, flags...), linkDefines(len(o.Extra) > 0, f.glue(), f.libs(), o.Builtins)...)
	if len(o.Extra) > 0 {
		lflags = append(lflags, AppFlags(Host())...)
	}
	if f != nil {
		link = append(link, f.Link...)
	}
	mine := append([]string{LinkSource(core)}, o.Extra...)
	if f.glue() {
		mine = append(mine, f.Glue)
	}
	more, err := cc.CompileCached(mine, lflags)
	if err != nil {
		return "", err
	}
	paths := append(Paths(objs), Paths(more)...)
	if f != nil {
		units, err := f.objects(cc, append(append([]string{"-g"}, opt...), "-I"+filepath.Join(core, "include")))
		if err != nil {
			return "", err
		}
		paths = append(paths, units...)
		// The std packages' C (std/sqlite's SQLite), compiled with the
		// runner's compiler, optimized and without the sanitizers: it's
		// not the code under test.
		for _, u := range f.Libs {
			if u.Fetch != nil {
				if err := u.Fetch(); err != nil {
					return "", fmt.Errorf("cbuild: %s: %v", u.Name, err)
				}
			}
			objs, err := cc.CompileCached(u.Sources, append(append([]string{"-I" + filepath.Join(core, "include")}, u.Flags...), "-O2"))
			if err != nil {
				return "", fmt.Errorf("cbuild: %s: %v", u.Name, err)
			}
			paths = append(paths, Paths(objs)...)
		}
	}
	exe := filepath.Join(o.Dir, name)
	return exe, cc.Link(paths, exe, link)
}

// tmpName is a name of its own beside path, to write then rename into
// place.
func tmpName(path string) string { return ccache.TmpName(path) }

// Exe builds an executable of the core (core/src, without the fake host)
// and extra C sources (a host, the native registry, packages' C, an app
// compiled to C) for this computer, into dir (the caller's), and returns
// its path: every object comes from the object cache, the extra ones
// compiled with extraFlags (their include directories) too. link is what
// the executable links with besides the C library. sanitize builds it
// with ASan and UBSan (tests).
func Exe(core string, extra, extraFlags, link []string, sanitize bool, dir, name string) (string, error) {
	defer ccache.Flush()
	cc, err := Find()
	if err != nil {
		return "", err
	}
	srcs, err := Sources(core, false)
	if err != nil {
		return "", err
	}
	flags, _, ldflags := testFlags(core, sanitize)
	if !sanitize {
		// A host test's build, unsanitized: optimized a little, as before.
		flags = append(append(append([]string{}, Flags...), Includes(core)...), "-g", "-O1")
	}
	ldflags = append(ldflags, link...)
	objs, err := cc.CompileCached(append(srcs, LinkSource(core)), flags)
	if err != nil {
		return "", err
	}
	more, err := cc.CompileCached(extra, append(append([]string{}, flags...), extraFlags...))
	if err != nil {
		return "", err
	}
	if runtime.GOOS == "windows" {
		name += ".exe"
	}
	exe := filepath.Join(dir, name)
	return exe, cc.Link(append(Paths(objs), Paths(more)...), exe, ldflags)
}
