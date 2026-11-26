package main

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"slices"
	"sort"
	"strconv"
	"strings"

	"github.com/egoist/asuka/sdk"
	"github.com/egoist/asuka/std"
)

// The Swift of an app's native packages (their Package.swift), built with
// the host's Swift by xcodebuild, which builds Swift packages for iOS as
// for the Mac, as Xcode does: their Swift, C and Objective-C, the packages
// they depend on, prebuilt binary frameworks, resources and macros. The
// CLI writes two packages: asuka, the host's Swift (the module Asuka,
// which the native packages import, standing in for their dependency on
// https://github.com/egoist/asuka: SwiftPM takes a root's path package
// over another's of its name), and the app's root, which depends on it
// and on every native package (by its directory: SwiftPM knows a path
// package by its folder's name, so two native packages' must differ).
// The objects xcodebuild makes are then linked as the swiftc build's are
// (one object on iOS; the natives library on the Mac), their frameworks
// embedded and their resource bundles copied into the app.

// swiftBuild builds an app's Swift as packages.
type swiftBuild struct {
	dir      string // where its packages and build go
	platform string // ios, macos
	sdkName  string // iphoneos, iphonesimulator, macosx
	arch     string // swiftc's: arm64, x86_64
	release  bool
	defines  []string // the host's compilation conditions
	// variant names the host's when it's one a release prebuilds (iOS:
	// base, none of the optional parts, or all, a player's; the Mac: app,
	// program), else "".
	variant string
	host    map[string][]byte // the host package's sources, by their names
	cAsuka  map[string][]byte // iOS: the host's C headers (include/...)
	natives []nativePackage
	run     func(dir string, env []string, name string, args ...string) error
}

// swiftPackageOutput is what the build makes: the objects of its targets
// (the host's, the native packages', their dependencies'), the static
// libraries and dynamic frameworks of binary targets, and resource
// bundles.
type swiftPackageOutput struct {
	products   string
	objects    []string
	forceLoad  []string // modules' static libraries, linked whole
	archives   []string
	frameworks []string
	// static are binary targets' static frameworks (Google Mobile Ads',
	// Firebase's): linked whole, whether or not the Swift imports them,
	// and not embedded; their privacy
	// manifests go in the app in bundles of their own.
	static  []string
	bundles []string
}

// swiftPlatform is a Package.swift's platforms for platform.
func swiftPlatform(platform string) string {
	if platform == "ios" {
		return fmt.Sprintf(".iOS(%q)", iosMin)
	}
	return fmt.Sprintf(".macOS(%q)", macMin)
}

// hostManifest is the asuka package's Package.swift: the host's Swift as
// the module Asuka.
func (b *swiftBuild) hostManifest() string {
	var s strings.Builder
	fmt.Fprintf(&s, `// swift-tools-version:5.9
// The host's Swift, as the app's native packages import it (Asuka),
// written by the Asuka CLI.
import PackageDescription

let package = Package(
    name: "asuka",
    platforms: [%s],
    products: [.library(name: "Asuka", targets: ["Asuka"])],
    targets: [
`, swiftPlatform(b.platform))
	deps := ""
	if b.cAsuka != nil {
		s.WriteString("        .target(name: \"CAsuka\"),\n")
		deps = `"CAsuka"`
	}
	settings := []string{`.define("DEBUG", .when(configuration: .debug))`}
	for _, d := range b.defines {
		settings = append(settings, fmt.Sprintf(".define(%s)", strconv.Quote(d)))
	}
	fmt.Fprintf(&s, "        .target(name: \"Asuka\", dependencies: [%s], swiftSettings: [%s]),\n    ]\n)\n", deps, strings.Join(settings, ", "))
	return s.String()
}

// rootManifest is the app's package: it depends on the host's and on each
// native package's library products.
func (b *swiftBuild) rootManifest() (string, error) {
	min, err := swiftMinimum(b.natives, b.platform)
	if err != nil {
		return "", err
	}
	platform := fmt.Sprintf(".iOS(%q)", min)
	if b.platform != "ios" {
		platform = fmt.Sprintf(".macOS(%q)", min)
	}
	var deps, products []string
	seen := map[string]string{"asuka": "the host"}
	for _, p := range b.natives {
		id := strings.ToLower(filepath.Base(p.dir))
		if other, ok := seen[id]; ok {
			return "", fmt.Errorf("the Swift packages of %s and %s are one to SwiftPM, which knows a path package by its folder's name (%s): rename one's", other, p.path, filepath.Base(p.dir))
		}
		seen[id] = p.path
		info, err := swiftPackageInfo(p.dir)
		if err != nil {
			return "", err
		}
		deps = append(deps, fmt.Sprintf("        .package(path: %s),", strconv.Quote(p.dir)))
		for _, pr := range info.Products {
			products = append(products, fmt.Sprintf(".product(name: %s, package: %s)", strconv.Quote(pr), strconv.Quote(id)))
		}
	}
	return fmt.Sprintf(`// swift-tools-version:5.9
// The app's Swift: the host's and its native packages', written by the
// Asuka CLI, which builds it with xcodebuild.
import PackageDescription

let package = Package(
    name: "AsukaApp",
    platforms: [%s],
    products: [.library(name: "AsukaApp", targets: ["AsukaApp"])],
    dependencies: [
        .package(path: "../asuka"),
%s
    ],
    targets: [
        .target(name: "AsukaApp", dependencies: [.product(name: "Asuka", package: "asuka"), %s]),
    ]
)
`, platform, strings.Join(deps, "\n"), strings.Join(products, ", ")), nil
}

// build builds the host's Swift and the native packages': as modules,
// cached (swiftModules), when swiftc can build each alone; else as
// packages, written here and built by xcodebuild.
func (b *swiftBuild) build() (*swiftPackageOutput, error) {
	if out, ok, err := b.modules(); ok || err != nil {
		return out, err
	}
	if !exists("/usr/bin/xcodebuild") {
		return nil, errors.New("the app's native packages have Swift, which xcodebuild builds: install Xcode")
	}
	host := map[string][]byte{"Package.swift": []byte(b.hostManifest())}
	for name, data := range b.host {
		host[filepath.Join("Sources", "Asuka", name)] = data
	}
	for name, data := range b.cAsuka {
		host[filepath.Join("Sources", "CAsuka", name)] = data
	}
	if _, ok := b.cAsuka["shim.c"]; b.cAsuka != nil && !ok {
		// SwiftPM builds a C target from its sources: an empty one.
		host[filepath.Join("Sources", "CAsuka", "shim.c")] = []byte("// The host's C declarations are its headers'.\n")
	}
	if err := syncDir(filepath.Join(b.dir, "asuka"), host, ""); err != nil {
		return nil, err
	}
	root, err := b.rootManifest()
	if err != nil {
		return nil, err
	}
	rootFiles := map[string][]byte{
		"Package.swift": []byte(root),
		filepath.Join("Sources", "AsukaApp", "AsukaApp.swift"): []byte("// The app's Swift is its packages': the host's and its native packages'.\n"),
	}
	if err := syncDir(filepath.Join(b.dir, "root"), rootFiles, ".build"); err != nil {
		return nil, err
	}
	config := "Debug"
	if b.release {
		config = "Release"
	}
	dest := map[string]string{"iphoneos": "generic/platform=iOS", "iphonesimulator": "generic/platform=iOS Simulator", "macosx": "generic/platform=macOS"}[b.sdkName]
	derived := filepath.Join(b.dir, "build")
	products := filepath.Join(derived, "Build", "Products", config)
	if b.sdkName != "macosx" {
		products += "-" + b.sdkName
	}
	// What an earlier build made goes, so what's there is this build's
	// (what it didn't change, it makes again from what it kept).
	os.RemoveAll(products)
	fmt.Println("asuka: building the app's Swift with its native packages' (xcodebuild)")
	err = b.run(filepath.Join(b.dir, "root"), nil, "xcodebuild", "-scheme", "AsukaApp", "-destination", dest,
		"-derivedDataPath", derived, "-configuration", config, "ARCHS="+b.arch, "ONLY_ACTIVE_ARCH=NO",
		"-skipPackagePluginValidation", "-skipMacroValidation", "-quiet", "build")
	if err != nil {
		return nil, err
	}
	out := &swiftPackageOutput{products: products}
	for _, dir := range []string{products, filepath.Join(products, "PackageFrameworks")} {
		es, _ := os.ReadDir(dir)
		for _, e := range es {
			p := filepath.Join(dir, e.Name())
			switch filepath.Ext(e.Name()) {
			case ".o":
				out.objects = append(out.objects, p)
			case ".a":
				out.archives = append(out.archives, p)
			case ".framework":
				// A binary target's static framework (Firebase's,
				// GoogleAppMeasurement's) is linked, as Xcode links it,
				// whether or not the Swift imports it; a dynamic one is
				// embedded.
				if staticFramework(p) {
					out.static = append(out.static, p)
				} else {
					out.frameworks = append(out.frameworks, p)
				}
			case ".bundle":
				out.bundles = append(out.bundles, p)
			}
		}
	}
	if !slices.Contains(out.objects, filepath.Join(products, "Asuka.o")) {
		return nil, fmt.Errorf("xcodebuild made no Asuka.o in %s", products)
	}
	return out, nil
}

// frameworkPaths are the build's frameworks (none without one).
func (o *swiftPackageOutput) frameworkPaths() []string {
	if o == nil {
		return nil
	}
	return o.frameworks
}

// linkFlags link the build's static libraries and frameworks.
func (o *swiftPackageOutput) linkFlags() []string {
	if o == nil {
		return nil
	}
	flags := slices.Clone(o.archives)
	for _, a := range o.forceLoad {
		flags = append(flags, "-Xlinker", "-force_load", "-Xlinker", a)
	}
	for _, f := range o.static {
		flags = append(flags, "-Xlinker", "-force_load", "-Xlinker", frameworkBinary(f))
	}
	if len(o.frameworks)+len(o.static) > 0 {
		flags = append(flags, "-F", o.products, "-F", filepath.Join(o.products, "PackageFrameworks"))
	}
	if len(o.static) > 0 {
		// Other static libraries' Objective-C categories, which nothing
		// names, loaded too: what Firebase's setup asks of an app (-ObjC).
		flags = append(flags, "-Xlinker", "-ObjC")
	}
	return flags
}

// frameworkBinary is a framework's binary: Name.framework/Name.
func frameworkBinary(fw string) string {
	return filepath.Join(fw, strings.TrimSuffix(filepath.Base(fw), ".framework"))
}

// staticFramework reports whether fw's binary is a static library (an
// archive, or a universal file of archives) rather than a dynamic one.
func staticFramework(fw string) bool {
	f, err := os.Open(frameworkBinary(fw))
	if err != nil {
		return false
	}
	defer f.Close()
	head := make([]byte, 8)
	if _, err := io.ReadFull(f, head); err != nil {
		return false
	}
	if string(head) == "!<arch>\n" {
		return true
	}
	// A universal file (big endian fat_header): its first slice's.
	if binary.BigEndian.Uint32(head) != 0xcafebabe || binary.BigEndian.Uint32(head[4:]) == 0 {
		return false
	}
	arch := make([]byte, 20) // fat_arch: cputype, cpusubtype, offset, size, align
	if _, err := io.ReadFull(f, arch); err != nil {
		return false
	}
	if _, err := f.Seek(int64(binary.BigEndian.Uint32(arch[8:])), io.SeekStart); err != nil {
		return false
	}
	if _, err := io.ReadFull(f, head); err != nil {
		return false
	}
	return string(head) == "!<arch>\n"
}

// embed copies the build's frameworks into frameworks and its resource
// bundles into resources, as an app bundle has them.
func (o *swiftPackageOutput) embed(frameworks, resources string) error {
	if o == nil {
		return nil
	}
	for _, f := range o.frameworks {
		dst := filepath.Join(frameworks, filepath.Base(f))
		os.RemoveAll(dst)
		if err := copyTree(f, dst); err != nil {
			return err
		}
	}
	for _, r := range o.bundles {
		dst := filepath.Join(resources, filepath.Base(r))
		os.RemoveAll(dst)
		if err := copyTree(r, dst); err != nil {
			return err
		}
	}
	// A static framework's privacy manifest, which App Store Connect reads
	// of the SDKs an app links, in a resource bundle as a static library's
	// is.
	for _, f := range o.static {
		manifest, err := os.ReadFile(filepath.Join(f, "PrivacyInfo.xcprivacy"))
		if os.IsNotExist(err) {
			continue
		} else if err != nil {
			return err
		}
		name := strings.TrimSuffix(filepath.Base(f), ".framework")
		dst := filepath.Join(resources, name+"_Privacy.bundle")
		os.RemoveAll(dst)
		if err := os.MkdirAll(dst, 0o755); err != nil {
			return err
		}
		info := plist(map[string]any{
			"CFBundleIdentifier":            "dev.asuka.privacy." + idPart.ReplaceAllString(name, "-"),
			"CFBundleName":                  name + "_Privacy",
			"CFBundlePackageType":           "BNDL",
			"CFBundleInfoDictionaryVersion": "6.0",
		})
		if err := os.WriteFile(filepath.Join(dst, "Info.plist"), []byte(info), 0o644); err != nil {
			return err
		}
		if err := os.WriteFile(filepath.Join(dst, "PrivacyInfo.xcprivacy"), manifest, 0o644); err != nil {
			return err
		}
	}
	return nil
}

// copyTree copies a directory as it is: its files, and its symbolic
// links as links (a framework's Versions/Current).
func copyTree(src, dst string) error {
	return filepath.WalkDir(src, func(p string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		r, _ := filepath.Rel(src, p)
		target := filepath.Join(dst, r)
		info, err := os.Lstat(p)
		if err != nil {
			return err
		}
		switch {
		case info.Mode()&os.ModeSymlink != 0:
			link, err := os.Readlink(p)
			if err != nil {
				return err
			}
			return os.Symlink(link, target)
		case d.IsDir():
			return os.MkdirAll(target, 0o755)
		}
		data, err := os.ReadFile(p)
		if err != nil {
			return err
		}
		return os.WriteFile(target, data, info.Mode().Perm())
	})
}

// syncDir makes dir hold want (by paths relative to it), writing only the
// files that changed (xcodebuild builds what did) and taking away the
// ones it no longer has, but under keep and SwiftPM's own.
func syncDir(dir string, want map[string][]byte, keep string) error {
	for r, data := range want {
		p := filepath.Join(dir, r)
		if old, err := os.ReadFile(p); err == nil && bytes.Equal(old, data) {
			continue
		}
		if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
			return err
		}
		if err := os.WriteFile(p, data, 0o644); err != nil {
			return err
		}
	}
	return filepath.WalkDir(dir, func(p string, d fs.DirEntry, err error) error {
		if err != nil {
			return nil
		}
		r, _ := filepath.Rel(dir, p)
		if r == "." {
			return nil
		}
		top, _, _ := strings.Cut(filepath.ToSlash(r), "/")
		if top == keep && keep != "" || top == ".swiftpm" || top == ".build" || r == "Package.resolved" {
			if d.IsDir() {
				return filepath.SkipDir
			}
			return nil
		}
		if !d.IsDir() {
			if _, ok := want[r]; !ok {
				os.Remove(p)
			}
		}
		return nil
	})
}

// swiftSources are the Swift files of an embedded tree's directories, by
// their names.
func swiftSources(fsys fs.FS, dirs ...string) (map[string][]byte, error) {
	out := map[string][]byte{}
	for _, d := range dirs {
		es, err := fs.ReadDir(fsys, d)
		if err != nil {
			return nil, err
		}
		for _, e := range es {
			if e.IsDir() || !strings.HasSuffix(e.Name(), ".swift") {
				continue
			}
			if _, dup := out[e.Name()]; dup {
				return nil, fmt.Errorf("two of the host's Swift files are %s", e.Name())
			}
			b, err := fs.ReadFile(fsys, d+"/"+e.Name())
			if err != nil {
				return nil, err
			}
			out[e.Name()] = b
		}
	}
	return out, nil
}

// filesUnder are an embedded tree's files under dir, by their paths
// relative to it.
func filesUnder(fsys fs.FS, dir string) (map[string][]byte, error) {
	out := map[string][]byte{}
	err := fs.WalkDir(fsys, dir, func(p string, d fs.DirEntry, err error) error {
		if err != nil || d.IsDir() {
			return err
		}
		b, err := fs.ReadFile(fsys, p)
		r := strings.TrimPrefix(p, dir+"/")
		out[filepath.FromSlash(r)] = b
		return err
	})
	keys := make([]string, 0, len(out))
	for k := range out {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	return out, err
}

// bundled reports whether the packages bring what goes in an app's
// bundle beside its executable (frameworks, resource bundles).
func (o *swiftPackageOutput) bundled() bool { return len(o.frameworks) > 0 || len(o.bundles) > 0 }

func stdEmbedded() bool { return std.Embedded() }
func sdkEmbedded() bool { return sdk.Embedded() }
