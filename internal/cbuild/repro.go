package cbuild

import (
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"sort"
	"strings"
	"sync"
)

// Builds are reproducible (docs/shipping.md): the same sources, built by
// the same CLI with the same compiler, make the same bytes, wherever the
// checkout, the cache and the app are and whenever the build runs. What
// a compile writes of where it ran is fixed here; what a link and an
// archiver write, in reproLink and the toolchains' archivers.

// prefixMaps are what every compile is given so that its object is the
// same wherever its sources are: the roots' paths (the repository's, the
// cache's, the project's) written as fixed names in __FILE__ (the core's
// assertions), debug information and coverage (-ffile-prefix-map), and
// the compile's own directory as "." (debug information's DW_AT_comp_dir).
// The object cache shares an object by its sources' paths relative to
// the roots (keyPath), so an object made in one checkout is the one
// another would make.
//
// A path under two roots (the project in a checkout) takes the longer's:
// the maps go shortest first, as GCC and Zig's clang take the last map
// that matches and Apple's and the NDK's clang the longest.
func (c Compiler) prefixMaps() []string {
	type pm struct{ flag, from, to string }
	var ms []pm
	for _, r := range pathRoots() {
		ms = append(ms, pm{"-ffile-prefix-map=", r.dir + string(filepath.Separator), r.mapped})
	}
	var out []string
	if c.IsGCC() {
		// GCC has no -fdebug-compilation-dir: its directory, mapped as
		// another root, in debug information only.
		if wd, err := os.Getwd(); err == nil {
			ms = append(ms, pm{"-fdebug-prefix-map=", wd, "."})
		}
	} else {
		out = append(out, "-fdebug-compilation-dir=.")
	}
	sort.SliceStable(ms, func(i, j int) bool { return len(ms[i].from) < len(ms[j].from) })
	for _, m := range ms {
		if !strings.ContainsAny(m.from+m.to, "=") {
			out = append(out, m.flag+m.from+"="+m.to)
		}
	}
	return out
}

// reproLink are what a link for t with the toolchain's compiler adds so
// that its executable is the same from one link to the next: a Windows
// executable's time stamp the hash of its contents (/Brepro; MinGW's
// ld: none), which the linker would set to the time it ran; Apple's
// linker's -reproducible. ELF's build ID and Mach-O's UUID are their
// contents' hashes already, and Apple's linker writes no object's time
// (ZERO_AR_DATE, Compiler.run).
func (tc *Toolchain) reproLink() []string {
	if tc.Target.Apple() {
		if tc.CC.kind() == "apple" && AppleLinkerReproducible() {
			return []string{"-Wl,-reproducible"}
		}
		return nil
	}
	if tc.Target.OS != "windows" {
		return nil
	}
	switch {
	case tc.CC.IsZig():
		// Zig's cc asks lld for it itself when it compiles a source, not
		// when it links objects alone, which is how a build links.
		return []string{"-Wl,-Brepro"}
	case !tc.mingw():
		return []string{"-Wl,/Brepro"} // lld-link's and link.exe's
	}
	return []string{"-Wl,--no-insert-timestamp"} // MinGW's ld and lld's MinGW driver
}

var (
	appleFlagsMu sync.Mutex
	appleFlags   = map[string]bool{}
)

// AppleLinkerTakes reports whether Apple's linker (Xcode's) takes the
// option flag, asking it once.
func AppleLinkerTakes(flag string) bool {
	if runtime.GOOS != "darwin" {
		return false
	}
	appleFlagsMu.Lock()
	defer appleFlagsMu.Unlock()
	v, ok := appleFlags[flag]
	if !ok {
		v = exec.Command("xcrun", "ld", flag, "-v").Run() == nil
		appleFlags[flag] = v
	}
	return v
}

// AppleLinkerReproducible reports whether Apple's linker takes
// -reproducible (its own promise that what it makes depends on nothing
// but what it links: Xcode 15's linker on).
func AppleLinkerReproducible() bool { return AppleLinkerTakes("-reproducible") }
