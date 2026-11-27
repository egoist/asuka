package cbuild

import (
	"path/filepath"

	"github.com/egoist/asuka/internal/ccache"
)

// ProgramOptions say what ProgramExe builds a program's executable of
// (docs/programs.md: a package whose fn main is its entry, with no UI).
type ProgramOptions struct {
	// App is the program compiled to C (lang/cgen: its main()).
	App string
	// Host are a headless host's C sources (platform/desktop/program:
	// the main loop and asuka_desktop.h's host half, with the native
	// registry and packages' C), compiled with HostFlags. Deps were the
	// files they include beside their headers: what a compile includes is
	// its depfile's now, so they're unused.
	Host, HostFlags, Deps []string
	// Foreign is the C the program's externs call.
	Foreign *Foreign
	// Link are libraries and flags the link takes besides the C library.
	Link []string
	// Static links the C library in (Linux's musl): nothing to install
	// beside the executable.
	Static bool
	Debug  bool
	// Builtins are the ones App calls (its module's): only the std
	// packages' tables of them are linked. nil: every table.
	Builtins []string
}

// ProgramExe builds a program's executable for the toolchain's target at
// out: the core's library (prebuilt, or built into the cache), the
// program and its host from the object cache.
func (tc *Toolchain) ProgramExe(core string, o ProgramOptions, out string) error {
	defer ccache.Flush()
	coreLib, err := tc.UnitArchive(CoreUnit(core), o.Debug)
	if err != nil {
		return err
	}
	flags := tc.coreFlags(core, o.Debug)
	cflags := append([]string{"-I" + filepath.Join(core, "include")}, tc.base(o.Debug)...)
	link, err := tc.linkSource(core, o.Debug, true, o.Foreign.glue(), o.Foreign.libs(), o.Builtins)
	if err != nil {
		return err
	}
	objs := Paths(link)
	srcs := []string{o.App}
	if o.Foreign.glue() {
		srcs = append(srcs, o.Foreign.Glue)
	}
	more, err := tc.CC.CompileCached(srcs, append(append([]string{}, flags...), AppFlags(tc.CC, tc.Target)...))
	if err != nil {
		return err
	}
	objs = append(objs, Paths(more)...)
	if o.Foreign != nil {
		units, err := o.Foreign.objects(tc.CC, cflags)
		if err != nil {
			return err
		}
		objs = append(objs, units...)
		o.Link = append(o.Link, o.Foreign.Link...)
	}
	if len(o.Host) > 0 {
		host, err := tc.CC.CompileCached(o.Host, append(append([]string{}, flags...), o.HostFlags...))
		if err != nil {
			return err
		}
		objs = append(objs, Paths(host)...)
	}
	ld := append([]string{}, tc.Flags...)
	if o.Static {
		ld = append(ld, "-static")
	}
	ld = append(ld, "-lm")
	switch tc.Target.OS {
	case "linux":
		ld = append(ld, "-lpthread", "-ldl")
	case "windows":
		// The console's program (no -mwindows); the core's sockets
		// (abi/dev.c), its arguments in UTF-16, its randomness, its crash
		// reports' stacks.
		ld = append(ld, "-lws2_32", "-lshell32", "-lbcrypt", "-ladvapi32", "-ldbghelp", "-lole32")
	}
	ld = append(ld, o.Link...)
	if !o.Debug {
		ld = append(ld, deadStrip(tc.Target)...)
	}
	as := []archiveRef{{coreLib, false}}
	for _, u := range o.Foreign.libs() {
		p, err := tc.UnitArchive(u, o.Debug)
		if err != nil {
			return err
		}
		if p != "" {
			as = append(as, archiveRef{p, false})
		}
	}
	return tc.link(objs, as, ld, out)
}

// Musl is the toolchain for a Linux program linked statically (musl, with
// Zig): an executable that runs on any Linux of its architecture.
func Musl(arch string) (*Toolchain, error) {
	z, err := Zig()
	if err != nil {
		return nil, err
	}
	return &Toolchain{Target: Target{OS: "linux", Arch: arch}, CC: z, Flags: []string{"-target", clangArch(arch) + "-linux-musl"},
		AR: []string{z.Path, "ar", "rcs"}, Musl: true}, nil
}
