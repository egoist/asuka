package main

import (
	"errors"
	"fmt"
	"io/fs"
	"maps"
	"os"
	"path/filepath"
	"runtime"
	"slices"
	"strconv"
	"strings"
	"sync"

	"github.com/egoist/asuka/internal/cachedir"
	"github.com/egoist/asuka/internal/ccache"
	"github.com/egoist/asuka/internal/prebuilt"
)

// Swift modules (DESIGN.md, "The build cache"): the host's Swift (the
// module Asuka, with its compilation conditions) and each native
// package's (its one target, against the host's module) are compiled as
// modules with library evolution, each a static library (one object) and
// a module interface (.swiftinterface), which later compilers read: the
// stable form Apple ships its own frameworks in.
//
// A release of the CLI carries the host's and the std and sdk packages'
// prebuilt for each Apple target (internal/prebuilt), built with the
// oldest Xcode Asuka supports; a build uses them when its swiftc is that
// one or newer, and the host's variant is one prebuilt (iOS: none of the
// optional parts, or all of them, a player's; the Mac: an app's or a
// program's). Else, or from a checkout, they're compiled here, each once
// per version of its sources, swiftc, the SDK, the target and the
// configuration, into the build cache's Swift store. An app with no Swift
// of its own then compiles no Swift: it links. Its own packages compile
// against the modules' interfaces. A package swiftc can't build alone
// (one with SwiftPM dependencies of its own, resources, settings:
// std/lottie) is built with the others by xcodebuild (swiftBuild.build),
// as before.

// swiftLib is a module ready to link: its directory (what -I names, for
// what compiles against it), and its static library or object.
type swiftLib struct {
	dir string
	lib string
}

// modules builds b's Swift as modules when every native package is one
// (swiftInfo.Module); ok false when one isn't.
func (b *swiftBuild) modules() (out *swiftPackageOutput, ok bool, err error) {
	infos := make([]swiftInfo, len(b.natives))
	for i, p := range b.natives {
		info, err := swiftPackageInfo(p.dir)
		if err != nil {
			return nil, false, err
		}
		if info.Module == nil {
			return nil, false, nil
		}
		infos[i] = info
	}
	sdk, err := swiftSDK(b.sdkName)
	if err != nil {
		return nil, false, err
	}
	hostMin := map[string]string{"ios": iosMin, "tvos": tvosMin, "macos": macMin}[b.platform]
	pre := b.prebuiltSwift()
	var host swiftLib
	if lib, ok := pre.module("Asuka@" + b.variant); ok {
		host = lib
	} else {
		mod := swiftModule{name: "Asuka", sources: b.host, headers: b.cAsuka, defines: b.defines, min: hostMin}
		if host, err = b.module(sdk, mod, swiftLib{}); err != nil {
			return nil, true, err
		}
	}
	// The packages' modules, each against the host's alone: at once.
	libs := make([]swiftLib, len(b.natives))
	errs := make([]error, len(b.natives))
	sem := make(chan struct{}, max(1, runtime.NumCPU()/2))
	var wg sync.WaitGroup
	for i, p := range b.natives {
		mod := infos[i].Module
		if androidPackageArtifact(p.dir) != "" && swiftPackagesEmbedded() {
			if lib, ok := pre.module(mod.Name); ok {
				libs[i] = lib
				continue
			}
		}
		wg.Add(1)
		go func(i int, p nativePackage) {
			defer wg.Done()
			sem <- struct{}{}
			defer func() { <-sem }()
			srcs, err := swiftFiles(filepath.Join(p.dir, mod.Path))
			if err != nil {
				errs[i] = fmt.Errorf("%s: %v", p.path, err)
				return
			}
			min := map[string]string{"ios": mod.IOS, "tvos": mod.TVOS, "macos": mod.MacOS}[b.platform]
			if min == "" {
				min = hostMin
			}
			libs[i], err = b.module(sdk, swiftModule{name: mod.Name, sources: srcs, min: min, swift6: mod.Swift6, pkg: true}, host)
			if err != nil {
				errs[i] = fmt.Errorf("%s: %v", p.path, err)
			}
		}(i, p)
	}
	wg.Wait()
	if err := errors.Join(errs...); err != nil {
		return nil, true, err
	}
	// Each linked whole: the host finds a package's provider by its
	// class's name (NSClassFromString), which nothing else names.
	out = &swiftPackageOutput{}
	for _, l := range append([]swiftLib{host}, libs...) {
		if strings.HasSuffix(l.lib, ".a") {
			out.forceLoad = append(out.forceLoad, l.lib)
		} else {
			out.objects = append(out.objects, l.lib)
		}
	}
	return out, true, nil
}

// swiftPackagesEmbedded reports whether the std and sdk packages are the
// CLI's own (what its prebuilt modules were built from).
func swiftPackagesEmbedded() bool { return stdEmbedded() && sdkEmbedded() }

// swiftModule is a module's sources and how it's compiled.
type swiftModule struct {
	name    string
	sources map[string][]byte // its Swift, by path
	headers map[string][]byte // the host's C module (CAsuka: include/...)
	defines []string          // compilation conditions
	min     string            // its minimum OS version
	swift6  bool              // Swift 6's language mode
	pkg     bool              // a native package's (SwiftPM's conditions)
}

// moduleTriple is a module directory's name for a target: its
// architecture, vendor and system, without a version
// (arm64-apple-ios-simulator).
func (b *swiftBuild) moduleTriple() string {
	switch {
	case b.platform == "macos":
		return b.arch + "-apple-macos"
	case strings.HasSuffix(b.sdkName, "simulator"):
		return b.arch + "-apple-" + b.platform + "-simulator"
	}
	return b.arch + "-apple-" + b.platform
}

// target is swiftc's -target for a module of minimum version min.
func (b *swiftBuild) target(min string) string {
	t := b.arch + "-apple-macos" + min
	if b.platform == "ios" || b.platform == "tvos" {
		t = b.arch + "-apple-" + b.platform + min
		if strings.HasSuffix(b.sdkName, "simulator") {
			t += "-simulator"
		}
	}
	return t
}

// moduleCache is where swiftc keeps the modules it builds from
// interfaces (the SDK's, prebuilt ones'), per swiftc.
func moduleCache() string {
	return filepath.Join(ccache.Dir("swift"), "mc", ccache.Key(swiftcVersion())[:16])
}

// module compiles m into the cache (once): its directory holds
// <name>.swiftmodule/<triple>.{swiftinterface,swiftmodule}, lib<name>.a
// and, the host's, its C module's headers (CAsuka/include). host is the
// host's module, for a package's.
func (b *swiftBuild) module(sdk string, m swiftModule, host swiftLib) (swiftLib, error) {
	return b.moduleIn(sdk, m, host, "")
}

// moduleIn is module, written to out (a release's prebuilt modules) when
// it's not "", not the cache.
func (b *swiftBuild) moduleIn(sdk string, m swiftModule, host swiftLib, out string) (swiftLib, error) {
	target := b.target(m.min)
	opt := []string{"-Onone", "-g", "-D", "DEBUG"}
	if b.release {
		opt = []string{"-O", "-g"}
	}
	if m.pkg {
		opt = append(opt, "-D", "SWIFT_PACKAGE")
	}
	lang := "5"
	if m.swift6 {
		lang = "6"
	}
	triple := b.moduleTriple()
	parts := []string{"swift-module-3", swiftcVersion(), sdk, swiftSDKVersion(b.sdkName), target, lang, m.name, strings.Join(opt, " "), strings.Join(m.defines, " "), host.dir}
	for _, n := range slices.Sorted(maps.Keys(m.sources)) {
		parts = append(parts, n, string(m.sources[n]))
	}
	for _, n := range slices.Sorted(maps.Keys(m.headers)) {
		parts = append(parts, n, string(m.headers[n]))
	}
	dir := out
	if dir == "" {
		dir = ccache.Sharded("swift", "m", ccache.Key(parts...), "")
	}
	lib := filepath.Join(dir, "lib"+m.name+".a")
	if _, err := os.Stat(lib); err == nil {
		ccache.Touch(dir)
		ccache.Hit("swift")
		return swiftLib{dir, lib}, nil
	}
	ccache.Miss("swift")
	tmp := ccache.TmpName(dir)
	os.RemoveAll(tmp)
	defer os.RemoveAll(tmp)
	src := filepath.Join(tmp, "src")
	var files []string
	for _, n := range slices.Sorted(maps.Keys(m.sources)) {
		p := filepath.Join(src, filepath.FromSlash(n))
		if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
			return swiftLib{}, err
		}
		if err := os.WriteFile(p, m.sources[n], 0o644); err != nil {
			return swiftLib{}, err
		}
		// Named relative to the directory swiftc runs in: what the code
		// says of its files (#filePath's length, which a closure's check
		// passes even where the path's mapped) is then the same wherever
		// the cache and this directory are (reproducible builds).
		files = append(files, filepath.Join("src", filepath.FromSlash(n)))
	}
	inc := filepath.Join(tmp, "CAsuka")
	for n, data := range m.headers {
		if !strings.HasPrefix(n, "include/") {
			continue // the host's C module is its headers; its sources are the core's
		}
		p := filepath.Join(inc, filepath.FromSlash(n))
		if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
			return swiftLib{}, err
		}
		if err := os.WriteFile(p, data, 0o644); err != nil {
			return swiftLib{}, err
		}
	}
	what := m.name
	if !m.pkg {
		what = "the host"
	}
	fmt.Printf("asuka: compiling %s's Swift for %s (once)\n", what, target)
	mod := filepath.Join(tmp, m.name+".swiftmodule")
	if err := os.MkdirAll(mod, 0o755); err != nil {
		return swiftLib{}, err
	}
	obj := filepath.Join(tmp, m.name+".o")
	args := []string{"swiftc", "-target", target, "-sdk", sdk, "-swift-version", lang, "-module-name", m.name,
		"-parse-as-library", "-wmo", "-enable-library-evolution", "-c", "-o", obj,
		"-emit-module", "-emit-module-path", filepath.Join(mod, triple+".swiftmodule"),
		"-emit-module-interface-path", filepath.Join(mod, triple+".swiftinterface"),
		"-module-cache-path", moduleCache(),
		// The directory it's compiled in, and the cache, named as the
		// same wherever they are (its name is this process's, the cache's
		// this user's) in #filePath and debug information, as cbuild's
		// compiles name them: the directory's map both before and after
		// the cache's (it's in the cache), as swiftc takes the last map
		// that matches a path and its Clang the first.
		"-file-prefix-map", tmp + "=" + m.name, "-file-prefix-map", cachedir.Root() + "=asuka-cache", "-file-prefix-map", tmp + "=" + m.name}
	args = append(args, opt...)
	for _, d := range m.defines {
		args = append(args, "-D", d)
	}
	if host.dir != "" {
		args = append(args, "-I", host.dir, "-I", filepath.Join(host.dir, "CAsuka", "include"))
	} else {
		args = append(args, "-I", filepath.Join(inc, "include"))
	}
	if err := b.run(tmp, nil, "xcrun", append(args, files...)...); err != nil {
		return swiftLib{}, err
	}
	if err := b.run(tmp, nil, "xcrun", "libtool", "-static", "-D", "-o", filepath.Join(tmp, "lib"+m.name+".a"), obj); err != nil {
		return swiftLib{}, err
	}
	os.Remove(obj)
	os.RemoveAll(src)
	if out != "" {
		os.RemoveAll(out)
		if err := os.Rename(tmp, out); err != nil {
			return swiftLib{}, err
		}
		return swiftLib{out, filepath.Join(out, "lib"+m.name+".a")}, nil
	}
	if err := ccache.Commit(tmp, dir); err != nil {
		return swiftLib{}, err
	}
	return swiftLib{dir, lib}, nil
}

// prebuiltSwift is the CLI's prebuilt modules for b's target and
// configuration, when this swiftc can read them.
func (b *swiftBuild) prebuiltSwift() prebuiltSwift {
	target := map[string]string{"iphoneos": "ios", "iphonesimulator": "ios-simulator", "appletvos": "tvos", "appletvsimulator": "tvos-simulator", "macosx": "darwin"}[b.sdkName]
	arch := map[string]string{"arm64": "arm64", "x86_64": "amd64"}[b.arch]
	return prebuiltSwift{target: target + "-" + arch, config: map[bool]string{true: "release", false: "dev"}[b.release], triple: b.moduleTriple()}
}

type prebuiltSwift struct{ target, config, triple string }

// module is a prebuilt module (Asuka@<variant>, a package's), ok when
// the CLI has it for the target and this swiftc can read its interface
// (the one it was built with, or newer).
func (p prebuiltSwift) module(name string) (swiftLib, bool) {
	dir, built, ok, err := prebuilt.SwiftModule(p.target, p.config, name)
	if err != nil {
		fmt.Fprintln(os.Stderr, "asuka:", err)
		return swiftLib{}, false
	}
	if !ok || !swiftAtLeast(swiftcVersion(), built) {
		return swiftLib{}, false
	}
	base, _, _ := strings.Cut(name, "@")
	lib := filepath.Join(dir, "lib"+base+".a")
	if _, err := os.Stat(filepath.Join(dir, base+".swiftmodule", p.triple+".swiftinterface")); err != nil {
		return swiftLib{}, false
	}
	return swiftLib{dir, lib}, true
}

// swiftAtLeast reports whether swiftc's --version (Apple Swift version
// 6.4.2 ...) is version or newer.
func swiftAtLeast(versionOutput, version string) bool {
	have := swiftVersionOf(versionOutput)
	want := swiftVersionOf("Swift version " + version)
	if have == nil || want == nil {
		return false
	}
	for i := 0; i < 3; i++ {
		if have[i] != want[i] {
			return have[i] > want[i]
		}
	}
	return true
}

// swiftVersionOf is the version after "Swift version", as three numbers.
func swiftVersionOf(s string) []int {
	_, rest, ok := strings.Cut(s, "Swift version ")
	if !ok {
		return nil
	}
	f := strings.Fields(rest)
	if len(f) == 0 {
		return nil
	}
	out := make([]int, 3)
	for i, p := range strings.SplitN(f[0], ".", 3) {
		n, err := strconv.Atoi(strings.TrimRightFunc(p, func(r rune) bool { return r < '0' || r > '9' }))
		if err != nil {
			return nil
		}
		out[i] = n
	}
	return out
}

// swiftFiles are the .swift files under dir, by their paths relative to
// it.
func swiftFiles(dir string) (map[string][]byte, error) {
	out := map[string][]byte{}
	err := filepath.WalkDir(dir, func(p string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if d.IsDir() || !strings.HasSuffix(p, ".swift") {
			return nil
		}
		b, err := os.ReadFile(p)
		if err != nil {
			return err
		}
		r, _ := filepath.Rel(dir, p)
		out[filepath.ToSlash(r)] = b
		return nil
	})
	if len(out) == 0 && err == nil {
		err = fmt.Errorf("no Swift in %s", dir)
	}
	return out, err
}

var (
	sdkMu       sync.Mutex
	sdkPaths    = map[string]string{}
	sdkVersions = map[string]string{}
)

// swiftSDK is an SDK's path (iphoneos, iphonesimulator, macosx).
func swiftSDK(name string) (string, error) {
	sdkMu.Lock()
	defer sdkMu.Unlock()
	if p, ok := sdkPaths[name]; ok {
		return p, nil
	}
	p, err := output("xcrun", "--sdk", name, "--show-sdk-path")
	if err != nil {
		return "", fmt.Errorf("Xcode's %s SDK: %v (install Xcode)", name, err)
	}
	sdkPaths[name] = p
	return p, nil
}

// swiftSDKVersion is an SDK's version and build.
func swiftSDKVersion(name string) string {
	sdkMu.Lock()
	defer sdkMu.Unlock()
	if v, ok := sdkVersions[name]; ok {
		return v
	}
	v, _ := output("xcrun", "--sdk", name, "--show-sdk-version")
	b, _ := output("xcrun", "--sdk", name, "--show-sdk-build-version")
	sdkVersions[name] = v + " " + b
	return sdkVersions[name]
}
