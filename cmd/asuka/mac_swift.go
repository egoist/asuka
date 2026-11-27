package main

import (
	"bytes"
	"debug/macho"
	"encoding/binary"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"

	"github.com/egoist/asuka/internal/appbuild"
	"github.com/egoist/asuka/internal/cbuild"
	"github.com/egoist/asuka/lang/bytecode"
	"github.com/egoist/asuka/platform/macos"
)

// A Mac app's executable: the Swift host (platform/macos, AppKit), with the
// app's native packages' Swift (their Package.swift, built with the host's
// as Swift packages: native_swift.go), linked with the C core (libasuka.a:
// the app compiled into it for a release, none in asuka dev's player), for
// the build's architecture. Without native packages the host's object is
// the same for every app, compiled once per architecture and kept in the
// user's cache.

// macHostDefines are the host's compilation conditions on the Mac
// (macos.HostDefines).
var macHostDefines = macos.HostDefines

// macSwiftArch is swiftc's name for a Go architecture.
func macSwiftArch(goarch string) string {
	if goarch == "amd64" {
		return "x86_64"
	}
	return goarch
}

// macBuild builds a Mac app's executable.
type macBuild struct {
	app     *asukaApp
	work    string // the build's work directory
	release bool   // optimized, dead code stripped
	run     func(dir string, env []string, cmd string, args ...string) error
	// program: the executable is a program's (docs/programs.md), its entry
	// AsukaProgram's (no application, no window) in place of the app's.
	program bool
	// packages is what the app's Swift packages bring besides objects
	// (nil without any): their frameworks and resource bundles, which go
	// in the bundle.
	packages *swiftPackageOutput
}

// executable builds the app's executable at exe for arch (Go's name):
// the host and the core (with m compiled in; nil, the player's).
func (b *macBuild) executable(exe, arch string, m *bytecode.Module) error {
	natives, err := nativeFor(b.app, "macos")
	if err != nil {
		return err
	}
	dir := filepath.Join(b.work, "mac-"+arch)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return err
	}
	// The core: a release's with the app compiled in, the player's
	// without, both with the app's C (asuka.toml's [c], its externs').
	lib := filepath.Join(dir, "libasuka.a")
	target := cbuild.Target{OS: "darwin", Arch: arch}
	if m != nil {
		err = appbuild.Lib(target, cbuild.Static, m, lib)
	} else {
		err = appbuild.PlayerLib(target, cbuild.Static, b.app.c, lib)
	}
	if err != nil {
		return err
	}
	objs, pkgs, err := b.host(arch, natives)
	if err != nil {
		return err
	}
	b.packages = pkgs
	// The C the desktop hosts share that the host calls (CAsuka's
	// desktop.h: the route state, flyouts' places, Asuka's words).
	shared, err := appbuild.MacShared(arch, !b.release)
	if err != nil {
		return err
	}
	objs = append(objs, shared...)
	out := filepath.Join(dir, "app")
	args := []string{"swiftc", "-target", macSwiftArch(arch) + "-apple-macos" + macMin, "-sdk", macSDK()}
	args = append(args, objs...)
	args = append(args, lib, "-o", out, "-lresolv",
		// The Swift runtime: the system's, else (macOS 11, for Swift
		// concurrency) the copy in Frameworks; packages' frameworks there.
		"-Xlinker", "-rpath", "-Xlinker", "/usr/lib/swift",
		"-Xlinker", "-rpath", "-Xlinker", "@executable_path/../Frameworks")
	args = append(args, appleLinkRepro()...) // the same objects, the same binary (repro.go)
	if b.release {
		// Without a debug map (-S): it would name the objects in this
		// computer's cache, which a release's users don't have, and make
		// its bytes this cache's (reproducible builds).
		args = append(args, "-Xlinker", "-dead_strip", "-Xlinker", "-S")
	}
	args = append(args, pkgs.linkFlags()...)
	if err := b.run(b.work, nil, "xcrun", args...); err != nil {
		return err
	}
	if err := os.MkdirAll(filepath.Dir(exe), 0o755); err != nil {
		return err
	}
	return copyFile(out, exe)
}

// host builds the host's Swift for arch with the app's native packages'
// (swiftBuild: cached modules, or prebuilt ones): their objects, and
// what else the build made.
func (b *macBuild) host(arch string, natives []nativePackage) ([]string, *swiftPackageOutput, error) {
	sources, cAsuka, err := macHostSwift(b.program)
	if err != nil {
		return nil, nil, err
	}
	variant := "app"
	if b.program {
		variant = "program"
	}
	sb := &swiftBuild{dir: filepath.Join(b.work, "swift-"+arch), platform: "macos", sdkName: "macosx",
		arch: macSwiftArch(arch), release: b.release, defines: macHostDefines, variant: variant, host: sources, cAsuka: cAsuka, natives: natives, run: b.run}
	out, err := sb.build()
	if err != nil {
		return nil, nil, err
	}
	return out.objects, out, nil
}

// macProgramMain is a program's entry, in place of the app's (App/
// main.swift).
const macProgramMain = `// A program's entry point: its main, run with no window.
@main
enum AsukaMain {
    static func main() { AsukaProgram.main() }
}
`

// embed puts what the app's Swift packages bring in the bundle (their
// frameworks in Frameworks, their resource bundles in Resources), and
// Swift's concurrency runtime for macOS 11 when the executable links it.
func (b *macBuild) embed(bundle, exe string) error {
	fw := filepath.Join(bundle, "Contents", "Frameworks")
	if b.packages != nil {
		if err := os.MkdirAll(fw, 0o755); err != nil {
			return err
		}
		if err := b.packages.embed(fw, filepath.Join(bundle, "Contents", "Resources")); err != nil {
			return err
		}
	}
	return macSwiftBackDeploy(exe, fw)
}

var (
	macSDKPath   string
	swiftcVer    string
	macSDKLoaded bool
)

// macSDK is the macOS SDK's path.
func macSDK() string {
	if !macSDKLoaded {
		macSDKLoaded = true
		macSDKPath, _ = output("xcrun", "--sdk", "macosx", "--show-sdk-path")
		swiftcVer, _ = output("xcrun", "swiftc", "--version")
	}
	return macSDKPath
}

func swiftcVersion() string {
	macSDK()
	return swiftcVer
}

// macSwiftBackDeploy copies Swift's concurrency runtime into Frameworks
// when the executable links it: macOS 12 and later have it, macOS 11 (the
// apps' minimum) doesn't, so the executable finds this copy there (its
// rpath), as Xcode's builds do.
func macSwiftBackDeploy(exe, fw string) error {
	const name = "libswift_Concurrency.dylib"
	dst := filepath.Join(fw, name)
	if !macImports(exe, "@rpath/"+name) {
		os.Remove(dst)
		return nil
	}
	out, err := exec.Command("xcrun", "--find", "swiftc").Output()
	if err != nil {
		return fmt.Errorf("asuka: finding swiftc: %v", err)
	}
	src := filepath.Join(filepath.Dir(strings.TrimSpace(string(out))), "..", "lib", "swift-5.5", "macosx", name)
	if _, err := os.Stat(src); err != nil {
		return fmt.Errorf("asuka: the app's Swift uses Swift concurrency, which macOS 11 doesn't have, and this toolchain has no copy to ship (%s)", src)
	}
	if err := os.MkdirAll(fw, 0o755); err != nil {
		return err
	}
	return copyFile(src, dst)
}

// macImports reports whether a Mach-O file (thin or universal) links a
// library.
func macImports(path, lib string) bool {
	var files []*macho.File
	if fat, err := macho.OpenFat(path); err == nil {
		defer fat.Close()
		for _, a := range fat.Arches {
			files = append(files, a.File)
		}
	} else if f, err := macho.Open(path); err == nil {
		defer f.Close()
		files = append(files, f)
	}
	for _, f := range files {
		for _, l := range f.Loads {
			if machoDylib(l.Raw(), f.ByteOrder) == lib {
				return true
			}
		}
	}
	return false
}

// machoDylib is the library a load command links (LC_LOAD_DYLIB,
// LC_LOAD_WEAK_DYLIB: debug/macho's ImportedLibraries leaves the weak
// ones out, and Swift links its concurrency runtime weakly), or "".
func machoDylib(raw []byte, bo binary.ByteOrder) string {
	const loadDylib, loadWeakDylib = 0xc, 0x80000018
	if len(raw) < 12 {
		return ""
	}
	if cmd := bo.Uint32(raw); cmd != loadDylib && cmd != loadWeakDylib {
		return ""
	}
	off := int(bo.Uint32(raw[8:]))
	if off >= len(raw) {
		return ""
	}
	name := raw[off:]
	if i := bytes.IndexByte(name, 0); i >= 0 {
		name = name[:i]
	}
	return string(name)
}

// copyFile copies an executable or a library (executable, as the linker
// makes them).
func copyFile(src, dst string) error {
	data, err := os.ReadFile(src)
	if err != nil {
		return err
	}
	os.Remove(dst) // a signed file is replaced, not written over
	return os.WriteFile(dst, data, 0o755)
}
