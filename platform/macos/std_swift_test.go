//go:build darwin && !ios

package macos

import (
	"maps"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"runtime"
	"slices"
	"strings"
	"testing"

	"github.com/egoist/asuka/internal/appbuild"
	"github.com/egoist/asuka/internal/cbuild"
	"github.com/egoist/asuka/internal/nativegen"
)

// Every standard library and platform SDK package's Swift
// ({std,sdk}/<name>/swift) builds for
// macOS, as an app's executable has it (with module Asuka: the Mac's
// host), and typechecks for the iOS simulator. A package whose
// Package.swift depends on more than Asuka needs SwiftPM to resolve it,
// and is skipped.
func TestStdSwift(t *testing.T) {
	if _, err := exec.LookPath("xcrun"); err != nil {
		t.Skip("no Xcode command line tools")
	}
	dir := t.TempDir()
	mac, ios, simSDK, arch := asukaModules(t, dir)
	pkgs, _ := filepath.Glob("../../std/*/Package.swift")
	sdk, _ := filepath.Glob("../../sdk/*/Package.swift")
	pkgs = append(pkgs, sdk...)
	for _, ps := range pkgs {
		pkg := filepath.Dir(ps)
		name := filepath.Base(pkg)
		t.Run(name, func(t *testing.T) {
			manifest, _ := os.ReadFile(ps)
			if strings.Count(string(manifest), ".package(") > 1 || strings.Contains(string(manifest), ".binaryTarget(") {
				t.Skip("depends on more than Asuka: SwiftPM resolves it")
			}
			p, err := nativegen.LoadAsuka(pkg)
			if err != nil {
				t.Fatal(err)
			}
			if stale, err := nativegen.Stale(p); err != nil || len(stale) > 0 {
				t.Fatalf("stale generated files (asuka native generate %s): %v %v", pkg, stale, err)
			}
			srcs, _ := filepath.Glob(filepath.Join(pkg, nativegen.SwiftDir, "*.swift"))
			mod := strings.ToUpper(name[:1]) + name[1:] + "Native"
			// The macOS the package says it needs, where it says it builds
			// there (platform/car is iOS's alone: CarPlay).
			if m := regexp.MustCompile(`\.macOS\("([0-9.]+)"\)`).FindStringSubmatch(string(manifest)); m != nil {
				swiftc(t, append([]string{"-target", arch + "-apple-macos" + m[1], "-module-name", mod, "-I", mac, "-I", filepath.Join("Sources", "CAsuka", "include"), "-wmo", "-c", "-o", filepath.Join(dir, name+".o")}, srcs...)...)
			}
			// iOS where the package says it builds there (std/watch is
			// the Mac's alone).
			if simSDK != "" && strings.Contains(string(manifest), ".iOS(") {
				swiftc(t, append([]string{"-sdk", simSDK, "-target", arch + "-apple-ios16.0-simulator", "-module-name", mod, "-I", ios, "-I", filepath.Join("..", "ios", "Sources", "CAsuka", "include"), "-typecheck"}, srcs...)...)
			}
		})
	}
}

// asukaModules builds module Asuka as a package's Swift sees it: for
// macOS in mac and, when there's the iOS simulator's SDK (simSDK), for it
// in ios: each its Asuka.swiftmodule and Asuka.o.
//
// Module Asuka is the host itself, as an app's Swift packages build
// against it (the CLI's Mac and iOS builds): its Sources/AsukaHost and
// Sources/AsukaOps, CAsuka's headers, its conditions (the Mac's
// HostDefines; on iOS every part, as asuka dev's player has), not its
// App's entry point. Its Asuka.o links with the core and, on the Mac, the
// desktop hosts' C the host calls (hostLink).
func asukaModules(t *testing.T, dir string) (mac, ios, simSDK, arch string) {
	t.Helper()
	arch = map[string]string{"arm64": "arm64", "amd64": "x86_64"}[runtime.GOARCH]
	mac = filepath.Join(dir, "mac")
	os.MkdirAll(mac, 0o755)
	var srcs []string
	for _, d := range []string{"AsukaHost", "AsukaOps"} {
		fs, _ := filepath.Glob(filepath.Join("Sources", d, "*.swift"))
		srcs = append(srcs, fs...)
	}
	args := []string{"-target", arch + "-apple-macos11", "-module-name", "Asuka", "-emit-module", "-emit-module-path", filepath.Join(mac, "Asuka.swiftmodule"), "-wmo", "-c",
		"-I", filepath.Join("Sources", "CAsuka", "include"), "-o", filepath.Join(mac, "Asuka.o")}
	for _, d := range HostDefines {
		args = append(args, "-D", d)
	}
	swiftc(t, append(args, srcs...)...)
	sim, _ := exec.Command("xcrun", "--sdk", "iphonesimulator", "--show-sdk-path").Output()
	simSDK = strings.TrimSpace(string(sim))
	if simSDK != "" {
		// The iOS host likewise, with every part (asuka dev's player's
		// conditions: each ASUKA_ one its sources test).
		ios = filepath.Join(dir, "ios")
		os.MkdirAll(ios, 0o755)
		iosSrc := filepath.Join("..", "ios", "Sources")
		var srcs []string
		for _, d := range []string{"AsukaHost", "AsukaOps"} {
			fs, _ := filepath.Glob(filepath.Join(iosSrc, d, "*.swift"))
			srcs = append(srcs, fs...)
		}
		args := []string{"-sdk", simSDK, "-target", arch + "-apple-ios16.0-simulator", "-module-name", "Asuka", "-emit-module", "-emit-module-path", filepath.Join(ios, "Asuka.swiftmodule"),
			"-wmo", "-c", "-I", filepath.Join(iosSrc, "CAsuka", "include"), "-o", filepath.Join(ios, "Asuka.o")}
		for _, d := range hostConditions(t, srcs) {
			args = append(args, "-D", d)
		}
		swiftc(t, append(args, srcs...)...)
	}
	return mac, ios, simSDK, arch
}

// hostLink is what an executable with a host's Asuka.o links besides, for
// target (darwin, ios-simulator): the core (a player's: no app compiled
// in) and, on the Mac, the desktop hosts' shared C the host calls, as the
// CLI links an app.
func hostLink(t *testing.T, dir, target string) []string {
	t.Helper()
	core := filepath.Join(dir, "core-"+target, "libasuka.a")
	if err := appbuild.PlayerLib(cbuild.Target{OS: target, Arch: runtime.GOARCH}, cbuild.Static, nil, core); err != nil {
		t.Fatal(err)
	}
	out := []string{core, "-lresolv"}
	if target == "darwin" {
		shared, err := appbuild.MacShared(runtime.GOARCH, true)
		if err != nil {
			t.Fatal(err)
		}
		out = append(shared, out...)
	}
	return out
}

// hostConditions are the ASUKA_ conditions the host's sources test.
func hostConditions(t *testing.T, srcs []string) []string {
	t.Helper()
	cond := regexp.MustCompile(`^\s*#(?:if|elseif)\b.*`)
	name := regexp.MustCompile(`ASUKA_[A-Z0-9_]+`)
	seen := map[string]bool{}
	for _, f := range srcs {
		b, err := os.ReadFile(f)
		if err != nil {
			t.Fatal(err)
		}
		for _, l := range strings.Split(string(b), "\n") {
			if cond.MatchString(l) {
				for _, n := range name.FindAllString(l, -1) {
					seen[n] = true
				}
			}
		}
	}
	return slices.Sorted(maps.Keys(seen))
}

func swiftc(t *testing.T, args ...string) {
	t.Helper()
	out, err := exec.Command("xcrun", append([]string{"swiftc", "-swift-version", "5", "-parse-as-library", "-Onone"}, args...)...).CombinedOutput()
	if err != nil {
		t.Fatalf("swiftc: %v\n%s", err, out)
	}
}

// bootedSimulator is a booted iOS simulator's UDID, or "".
func bootedSimulator() string {
	out, err := exec.Command("xcrun", "simctl", "list", "devices", "booted").Output()
	if err != nil {
		return ""
	}
	m := regexp.MustCompile(`\(([0-9A-F-]{36})\) \(Booted\)`).FindStringSubmatch(string(out))
	if m == nil {
		return ""
	}
	return m[1]
}

// TestStdSwiftViews runs the std and platform packages' Swift view tests:
// each package's tests/*ViewTest.swift (its @main), built with the package's
// Swift (one module, so it reaches the package's own types) into an
// executable for macOS, run here, and for the iOS simulator, run in a
// booted one (xcrun simctl spawn; none booted: skipped). Its first
// argument is its package's tests directory. It passes when it exits 0.
func TestStdSwiftViews(t *testing.T) {
	if _, err := exec.LookPath("xcrun"); err != nil {
		t.Skip("no Xcode command line tools")
	}
	tests, _ := filepath.Glob("../../std/*/tests/*ViewTest.swift")
	sdk, _ := filepath.Glob("../../sdk/*/tests/*ViewTest.swift")
	tests = append(tests, sdk...)
	if len(tests) == 0 {
		t.Skip("no std or sdk package has Swift view tests")
	}
	dir := t.TempDir()
	mac, ios, simSDK, arch := asukaModules(t, dir)
	for _, test := range tests {
		pkg := filepath.Dir(filepath.Dir(test))
		name := filepath.Base(pkg)
		data, _ := filepath.Abs(filepath.Dir(test))
		srcs, _ := filepath.Glob(filepath.Join(pkg, nativegen.SwiftDir, "*.swift"))
		srcs = append(srcs, test)
		mod := strings.ToUpper(name[:1]) + name[1:] + "Native"
		run := func(t *testing.T, cmd ...string) {
			out, err := exec.Command(cmd[0], cmd[1:]...).CombinedOutput()
			t.Logf("%s", out)
			if err != nil {
				t.Fatal(err)
			}
		}
		manifest, _ := os.ReadFile(filepath.Join(pkg, "Package.swift"))
		t.Run(name+"/macos", func(t *testing.T) {
			if !strings.Contains(string(manifest), ".macOS(") {
				t.Skip("the package builds for iOS alone")
			}
			exe := filepath.Join(dir, name+"-macos")
			args := []string{"-target", arch + "-apple-macos13", "-module-name", mod, "-I", mac, "-I", filepath.Join("Sources", "CAsuka", "include"), "-wmo", filepath.Join(mac, "Asuka.o"), "-o", exe}
			swiftc(t, append(append(args, hostLink(t, dir, "darwin")...), srcs...)...)
			run(t, exe, filepath.Join(data, "stream.json"))
		})
		t.Run(name+"/ios", func(t *testing.T) {
			if simSDK == "" {
				t.Skip("no iOS simulator SDK")
			}
			udid := bootedSimulator()
			if udid == "" {
				t.Skip("no booted iOS simulator")
			}
			exe := filepath.Join(dir, name+"-ios")
			args := []string{"-sdk", simSDK, "-target", arch + "-apple-ios16.0-simulator", "-module-name", mod, "-I", ios, "-I", filepath.Join("..", "ios", "Sources", "CAsuka", "include"), "-wmo",
				filepath.Join(ios, "Asuka.o"), "-o", exe}
			swiftc(t, append(append(args, hostLink(t, dir, "ios-simulator")...), srcs...)...)
			run(t, "xcrun", "simctl", "spawn", udid, exe, filepath.Join(data, "stream.json"))
		})
	}
}
