package main

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"io/fs"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"

	"github.com/egoist/asuka/internal/appbuild"
	"github.com/egoist/asuka/internal/cbuild"
	"github.com/egoist/asuka/internal/nativegen"
	"github.com/egoist/asuka/internal/prebuilt"
	androidHost "github.com/egoist/asuka/platform/android"
	iosHost "github.com/egoist/asuka/platform/ios"
	"github.com/egoist/asuka/platform/macos"
	"github.com/egoist/asuka/std"
)

// asuka prebuilt (not in the usage: the release's, scripts/npm's) builds
// what a release carries prebuilt for one of its npm packages
// (internal/prebuilt): the C units' static libraries for its targets and
// both configurations, the Swift modules (the host's variants and the std
// and sdk packages', with library evolution: an XCFramework each, of the
// package's slice), or the Android AARs, and prebuilt.json, what's in it:
//
//	asuka prebuilt targets                       the packages' names
//	asuka prebuilt build --target name --version v --out dir

// prebuiltTarget is one npm package's contents.
type prebuiltTarget struct {
	name  string
	c     []cbuild.Target // its C's target
	swift []swiftSlice    // its Swift's slice
	aar   bool            // the Android AARs
}

// swiftSlice is an SDK and architecture Swift is built for.
type swiftSlice struct{ sdkName, arch string }

var prebuiltTargets = []prebuiltTarget{
	{name: "ios-arm64", c: []cbuild.Target{{OS: "ios", Arch: "arm64"}}, swift: []swiftSlice{{"iphoneos", "arm64"}}},
	{name: "ios-sim-arm64", c: []cbuild.Target{{OS: "ios-simulator", Arch: "arm64"}}, swift: []swiftSlice{{"iphonesimulator", "arm64"}}},
	{name: "darwin-arm64", c: []cbuild.Target{{OS: "darwin", Arch: "arm64"}}, swift: []swiftSlice{{"macosx", "arm64"}}},
	{name: "darwin-x64", c: []cbuild.Target{{OS: "darwin", Arch: "amd64"}}, swift: []swiftSlice{{"macosx", "x86_64"}}},
	{name: "android-arm64-v8a", c: []cbuild.Target{{OS: "android", Arch: "arm64"}}},
	{name: "android-armeabi-v7a", c: []cbuild.Target{{OS: "android", Arch: "arm"}}},
	{name: "android-x86_64", c: []cbuild.Target{{OS: "android", Arch: "amd64"}}},
	{name: "android-aar", aar: true},
	{name: "wasm32", c: []cbuild.Target{{OS: "wasm", Arch: "wasm32"}}},
	{name: "linux-x64", c: []cbuild.Target{{OS: "linux", Arch: "amd64"}}},
	{name: "linux-arm64", c: []cbuild.Target{{OS: "linux", Arch: "arm64"}}},
	{name: "windows-x64", c: []cbuild.Target{{OS: "windows", Arch: "amd64"}}},
	{name: "windows-arm64", c: []cbuild.Target{{OS: "windows", Arch: "arm64"}}},
}

// prebuiltContents is prebuilt.json: what a package holds, which
// scripts/npm puts in the CLI's manifest.
type prebuiltContents struct {
	Name     string                   `json:"name"`
	Compiler string                   `json:"compiler,omitempty"`
	Swift    string                   `json:"swift,omitempty"`
	Files    map[string]prebuilt.File `json:"files"` // by manifest key; Package left out
	Maven    string                   `json:"maven,omitempty"`
}

func cmdPrebuilt(args []string) error {
	if len(args) > 0 && args[0] == "targets" {
		for _, t := range prebuiltTargets {
			fmt.Println(t.name)
		}
		return nil
	}
	if len(args) == 0 || args[0] != "build" {
		return fmt.Errorf("usage: asuka prebuilt targets | build --target name --version v --out dir")
	}
	var name, version, out string
	for i := 1; i+1 < len(args); i += 2 {
		switch args[i] {
		case "--target":
			name = args[i+1]
		case "--version":
			version = args[i+1]
		case "--out":
			out = args[i+1]
		default:
			return fmt.Errorf("asuka prebuilt build: unknown flag %s", args[i])
		}
	}
	var t *prebuiltTarget
	for i := range prebuiltTargets {
		if prebuiltTargets[i].name == name {
			t = &prebuiltTargets[i]
		}
	}
	if t == nil || version == "" || out == "" {
		return fmt.Errorf("asuka prebuilt build: --target (asuka prebuilt targets), --version and --out are required")
	}
	out, _ = filepath.Abs(out)
	if err := os.MkdirAll(out, 0o755); err != nil {
		return err
	}
	c := prebuiltContents{Name: t.name, Files: map[string]prebuilt.File{}}
	if err := t.buildC(out, &c); err != nil {
		return err
	}
	if err := t.buildSwift(out, &c); err != nil {
		return err
	}
	if t.aar {
		if err := buildPrebuiltAARs(out, version); err != nil {
			return err
		}
		c.Maven = "maven"
	}
	b, _ := json.MarshalIndent(c, "", "  ")
	return os.WriteFile(filepath.Join(out, "prebuilt.json"), append(b, '\n'), 0o644)
}

// buildC builds every unit for t's targets, both configurations, into
// out/lib/<config>/<target>/lib<unit>.a.
func (t *prebuiltTarget) buildC(out string, c *prebuiltContents) error {
	if len(t.c) == 0 {
		return nil
	}
	for _, debug := range []bool{true, false} {
		config := cbuild.Config(debug)
		libs := map[string][]string{} // unit → each target's library
		var order []string
		for _, tg := range t.c {
			tc, err := cbuild.ForTarget(tg)
			if err != nil {
				return err
			}
			c.Compiler = tc.CompilerKind()
			// A build for Apple's platforms links with Xcode's clang (it has
			// Xcode for the Swift), which uses only Xcode's libraries:
			// Zig's (ASUKA_CC=zig) would ship and never be used.
			if (tg.OS == "darwin" || strings.HasPrefix(tg.OS, "ios")) && c.Compiler != "apple" {
				return fmt.Errorf("asuka prebuilt: %s's libraries are built with Xcode's clang, not %s's (unset ASUKA_CC and CC)", tg, c.Compiler)
			}
			units, err := appbuild.Units(tg)
			if err != nil {
				return err
			}
			for _, u := range units {
				fmt.Printf("asuka: %s for %s (%s)\n", u.Name, tg, config)
				p, err := tc.BuildUnit(u, debug)
				if err != nil {
					return err
				}
				if p == "" {
					continue
				}
				if _, ok := libs[u.Name]; !ok {
					order = append(order, u.Name)
				}
				libs[u.Name] = append(libs[u.Name], p)
			}
		}
		for _, unit := range order {
			rel := filepath.ToSlash(filepath.Join("lib", config, t.c[0].String(), "lib"+unit+".a"))
			dst := filepath.Join(out, filepath.FromSlash(rel))
			if err := os.MkdirAll(filepath.Dir(dst), 0o755); err != nil {
				return err
			}
			if err := copyPlain(libs[unit][0], dst); err != nil {
				return err
			}
			sum, err := fileSHA256(dst)
			if err != nil {
				return err
			}
			for _, tg := range t.c {
				c.Files[prebuilt.ArchiveKey(tg.String(), config, unit)] = prebuilt.File{Path: rel, SHA256: sum}
			}
		}
	}
	return nil
}

// buildSwift builds the host's prebuilt variants and every std and sdk
// package's Swift module for t's slices, both configurations, into
// out/swift/<config>/<Module>.xcframework/<slice>/: an XCFramework each
// (one slice: the package's), with library evolution.
func (t *prebuiltTarget) buildSwift(out string, c *prebuiltContents) error {
	if len(t.swift) == 0 {
		return nil
	}
	c.Swift = strings.Join(swiftVersionOfString(swiftcVersion()), ".")
	platform := "ios"
	if t.swift[0].sdkName == "macosx" {
		platform = "macos"
	}
	natives, err := stdSwiftNatives(platform)
	if err != nil {
		return err
	}
	for _, release := range []bool{false, true} {
		config := map[bool]string{true: "release", false: "dev"}[release]
		// module → its slice built for each architecture
		parts := map[string][]swiftLib{}
		var names []string
		for _, sl := range t.swift {
			sb := &swiftBuild{platform: platform, sdkName: sl.sdkName, arch: sl.arch, release: release, run: func(dir string, env []string, name string, args ...string) error {
				return run(dir, name, args...)
			}}
			sdk, err := swiftSDK(sl.sdkName)
			if err != nil {
				return err
			}
			hosts, err := prebuiltHosts(platform)
			if err != nil {
				return err
			}
			var against swiftLib
			for _, h := range hosts {
				hostMin := map[string]string{"ios": iosMin, "macos": macMin}[platform]
				lib, err := sb.module(sdk, swiftModule{name: "Asuka", sources: h.sources, headers: h.cAsuka, defines: h.defines, min: hostMin}, swiftLib{})
				if err != nil {
					return err
				}
				key := "Asuka@" + h.variant
				if _, ok := parts[key]; !ok {
					names = append(names, key)
				}
				parts[key] = append(parts[key], lib)
				if h.packages {
					against = lib // the packages' host: a superset of every variant's interface
				}
			}
			for _, p := range natives {
				info, _ := swiftPackageInfo(p.dir)
				mod := info.Module
				srcs, err := swiftFiles(filepath.Join(p.dir, mod.Path))
				if err != nil {
					return err
				}
				min := map[string]string{"ios": mod.IOS, "macos": mod.MacOS}[platform]
				if min == "" {
					min = map[string]string{"ios": iosMin, "macos": macMin}[platform]
				}
				lib, err := sb.module(sdk, swiftModule{name: mod.Name, sources: srcs, min: min, swift6: mod.Swift6, pkg: true}, against)
				if err != nil {
					return fmt.Errorf("%s: %v", p.path, err)
				}
				if _, ok := parts[mod.Name]; !ok {
					names = append(names, mod.Name)
				}
				parts[mod.Name] = append(parts[mod.Name], lib)
			}
		}
		for _, name := range names {
			rel, err := writeXCFramework(out, config, name, platform, t.swift, parts[name])
			if err != nil {
				return err
			}
			for _, tg := range t.c {
				c.Files[prebuilt.SwiftKey(tg.String(), config, name)] = prebuilt.File{Path: rel}
			}
		}
	}
	return nil
}

// hostVariant is a prebuilt variant of the host's Swift.
type hostVariant struct {
	variant  string
	sources  map[string][]byte
	cAsuka   map[string][]byte
	defines  []string
	packages bool // the one the packages are built against
}

// prebuiltHosts are the host's variants a release prebuilds: iOS's base
// (none of its optional parts: most apps') and all (asuka dev's player,
// and the interface the packages are built against, a superset of every
// variant's); the Mac's app and program (its entry the only difference).
func prebuiltHosts(platform string) ([]hostVariant, error) {
	if platform == "ios" {
		src, err := swiftSources(iosHost.Files, "Sources/AsukaHost", "Sources/AsukaOps", "App")
		if err != nil {
			return nil, err
		}
		inc, err := filesUnder(iosHost.Files, "Sources/CAsuka")
		if err != nil {
			return nil, err
		}
		return []hostVariant{{variant: "base", sources: src, cAsuka: inc}, {variant: "all", sources: src, cAsuka: inc, defines: allFeatures(), packages: true}}, nil
	}
	var out []hostVariant
	for _, program := range []bool{false, true} {
		src, inc, err := macHostSwift(program)
		if err != nil {
			return nil, err
		}
		v := hostVariant{variant: "app", sources: src, cAsuka: inc, defines: macHostDefines, packages: true}
		if program {
			v.variant, v.packages = "program", false
		}
		out = append(out, v)
	}
	return out, nil
}

// stdSwiftNatives are the std and sdk packages with Swift for platform
// that build as modules of their own.
func stdSwiftNatives(platform string) ([]nativePackage, error) {
	ps, err := std.Packages()
	if err != nil {
		return nil, err
	}
	var paths []string
	for p := range ps {
		paths = append(paths, p)
	}
	sort.Strings(paths)
	var out []nativePackage
	for _, path := range paths {
		dir := ps[path]
		if _, err := os.Stat(filepath.Join(dir, "Package.swift")); err != nil {
			continue
		}
		info, err := swiftPackageInfo(dir)
		if err != nil {
			return nil, err
		}
		if !info.supports(platform) || info.Module == nil {
			continue
		}
		out = append(out, nativePackage{path: path, dir: dir})
	}
	return out, nil
}

// writeXCFramework writes a module's slice (its architectures' libraries
// made one, their interfaces together) as an XCFramework of that slice
// into out/swift/<config>/, and returns the slice's directory relative to
// out.
func writeXCFramework(out, config, name, platform string, slices []swiftSlice, libs []swiftLib) (string, error) {
	base, variant, _ := strings.Cut(name, "@")
	fw := base
	if variant != "" {
		fw += "-" + variant
	}
	var archs []string
	for _, s := range slices {
		archs = append(archs, s.arch)
	}
	id := platform + "-" + strings.Join(archs, "_")
	if slices[0].sdkName == "iphonesimulator" {
		id += "-simulator"
	}
	xc := filepath.Join(out, "swift", config, fw+".xcframework")
	slice := filepath.Join(xc, id)
	os.RemoveAll(xc)
	if err := os.MkdirAll(slice, 0o755); err != nil {
		return "", err
	}
	lib := filepath.Join(slice, "lib"+base+".a")
	var ins []string
	for _, l := range libs {
		ins = append(ins, l.lib)
		// Each architecture's public interface, the stable form (not the
		// compiler's own binary module, docs or source info), and the
		// host's C module.
		keep := func(rel string) bool {
			rel = filepath.ToSlash(rel)
			return strings.HasPrefix(rel, "CAsuka/") ||
				strings.HasSuffix(rel, ".swiftinterface") && !strings.HasSuffix(rel, ".private.swiftinterface")
		}
		if err := copyFiltered(l.dir, slice, keep); err != nil {
			return "", err
		}
	}
	if err := copyPlain(ins[0], lib); err != nil {
		return "", err
	}
	variantKey := ""
	if slices[0].sdkName == "iphonesimulator" {
		variantKey = "\n\t\t\t<key>SupportedPlatformVariant</key>\n\t\t\t<string>simulator</string>"
	}
	var archXML strings.Builder
	for _, a := range archs {
		fmt.Fprintf(&archXML, "\n\t\t\t\t<string>%s</string>", a)
	}
	plist := fmt.Sprintf(`<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>AvailableLibraries</key>
	<array>
		<dict>
			<key>LibraryIdentifier</key>
			<string>%s</string>
			<key>LibraryPath</key>
			<string>lib%s.a</string>
			<key>SupportedArchitectures</key>
			<array>%s
			</array>
			<key>SupportedPlatform</key>
			<string>%s</string>%s
		</dict>
	</array>
	<key>CFBundlePackageType</key>
	<string>XFWK</string>
	<key>XCFrameworkFormatVersion</key>
	<string>1.0</string>
</dict>
</plist>
`, id, base, archXML.String(), platform, variantKey)
	if err := os.WriteFile(filepath.Join(xc, "Info.plist"), []byte(plist), 0o644); err != nil {
		return "", err
	}
	rel, _ := filepath.Rel(out, slice)
	return filepath.ToSlash(rel), nil
}

// buildPrebuiltAARs publishes the Android host's modules and every std
// and sdk package's into out/maven, versioned as the release.
func buildPrebuiltAARs(out, version string) error {
	tc, err := newAndroidToolchain(false)
	if err != nil {
		return err
	}
	src, err := os.MkdirTemp("", "asuka-aar-")
	if err != nil {
		return err
	}
	defer os.RemoveAll(src)
	err = fs.WalkDir(androidHost.Files, ".", func(p string, d fs.DirEntry, err error) error {
		if err != nil || d.IsDir() {
			return err
		}
		b, _ := androidHost.Files.ReadFile(p)
		dst := filepath.Join(src, p)
		os.MkdirAll(filepath.Dir(dst), 0o755)
		return os.WriteFile(dst, b, 0o755)
	})
	if err != nil {
		return err
	}
	ps, err := std.Packages()
	if err != nil {
		return err
	}
	var tasks, natives, versions, artifacts []string
	for _, mod := range androidHostModules {
		tasks = append(tasks, ":"+mod+":publishPrebuiltPublicationToPrebuiltRepository")
		versions = append(versions, mod+"="+version)
	}
	var paths []string
	for p := range ps {
		paths = append(paths, p)
	}
	sort.Strings(paths)
	for _, path := range paths {
		dir := filepath.Join(ps[path], "android")
		if _, err := os.Stat(filepath.Join(dir, "build.gradle.kts")); err != nil {
			continue
		}
		mod := "native-" + strings.ToLower(nativegen.Sanitize(path)) // as androidNatives names it
		art := androidPackageArtifact(ps[path])
		natives = append(natives, mod+"="+dir)
		artifacts = append(artifacts, mod+"="+art)
		versions = append(versions, mod+"="+version)
		tasks = append(tasks, ":"+mod+":publishPrebuiltPublicationToPrebuiltRepository")
	}
	args := append(tasks, "-PasukaPublishRepo="+filepath.Join(out, "maven"), "-PasukaPublishVersions="+strings.Join(versions, ";"),
		"-PasukaPublishArtifacts="+strings.Join(artifacts, ";"), "-PasukaNativeModules="+strings.Join(natives, ";"))
	return tc.gradle(src, args...)
}

// macHostSwift is the Mac host's Swift as a module's sources (its entry,
// an app's or a program's, among them as AsukaMain.swift) and its C
// module's headers.
func macHostSwift(program bool) (sources, cAsuka map[string][]byte, err error) {
	sources, err = swiftSources(macos.Files, "Sources/AsukaHost", "Sources/AsukaOps", "App")
	if err != nil {
		return nil, nil, err
	}
	if program {
		sources["main.swift"] = []byte(macProgramMain)
	}
	if m, ok := sources["main.swift"]; ok {
		delete(sources, "main.swift")
		sources["AsukaMain.swift"] = m
	}
	cAsuka, err = filesUnder(macos.Files, "Sources/CAsuka")
	if err != nil {
		return nil, nil, err
	}
	for name := range cAsuka {
		if name == "stubs.c" || strings.HasPrefix(name, "shared_") {
			delete(cAsuka, name)
		}
	}
	return sources, cAsuka, nil
}

// swiftVersionOfString is swiftc's version as strings ("6", "4", "2").
func swiftVersionOfString(s string) []string {
	v := swiftVersionOf(s)
	if v == nil {
		return nil
	}
	return []string{fmt.Sprint(v[0]), fmt.Sprint(v[1]), fmt.Sprint(v[2])}
}

func run(dir, name string, args ...string) error {
	cmd := exec.Command(name, args...)
	cmd.Dir = dir
	b, err := cmd.CombinedOutput()
	if err != nil {
		return fmt.Errorf("%s %s: %v\n%s", name, strings.Join(args, " "), err, b)
	}
	return nil
}

func copyPlain(src, dst string) error {
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()
	o, err := os.Create(dst)
	if err != nil {
		return err
	}
	if _, err := io.Copy(o, in); err != nil {
		o.Close()
		return err
	}
	return o.Close()
}

// copyFiltered copies src's files that keep says into dst.
func copyFiltered(src, dst string, keep func(rel string) bool) error {
	return filepath.WalkDir(src, func(p string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		rel, _ := filepath.Rel(src, p)
		if d.IsDir() || !keep(rel) {
			return nil
		}
		to := filepath.Join(dst, rel)
		if err := os.MkdirAll(filepath.Dir(to), 0o755); err != nil {
			return err
		}
		return copyPlain(p, to)
	})
}

func fileSHA256(p string) (string, error) {
	f, err := os.Open(p)
	if err != nil {
		return "", err
	}
	defer f.Close()
	h := sha256.New()
	if _, err := io.Copy(h, f); err != nil {
		return "", err
	}
	return hex.EncodeToString(h.Sum(nil)), nil
}
