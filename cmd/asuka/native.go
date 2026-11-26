package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"slices"
	"sort"
	"strconv"
	"strings"

	"github.com/egoist/asuka/internal/cachedir"

	"github.com/egoist/asuka/internal/nativegen"
)

// Native packages: Asuka packages that declare native interfaces (a
// module's objects) and native components (views), with their native code
// beside them, a project of each platform's own (internal/nativegen): a
// Swift package (Package.swift, its Swift in swift/) for iOS and macOS, a
// Gradle module (android/) for Android, JavaScript (web/) for the web. The
// CLI keeps their generated files current, and builds each platform's
// project into the apps that import them: the Swift with xcodebuild, as a
// package of the app's (native_swift.go), the Kotlin as Gradle modules of
// the shell app, the JavaScript among the page's files.

// nativePackage is one of an app's packages (its own, a dependency's)
// with native code.
type nativePackage struct {
	path, name, dir string
	main            bool // the app's own, or a local dependency's: asuka writes its generated files
}

// appNatives are the app's packages that declare something native: the
// packages in its directory, and its dependencies.
func appNatives(app *asukaApp) ([]nativePackage, error) {
	type cand struct {
		dir  bool
		path string
		main bool
	}
	var dirs []string
	byDir := map[string]cand{}
	filepath.WalkDir(app.dir, func(p string, d os.DirEntry, err error) error {
		if err != nil || !d.IsDir() {
			return nil
		}
		if p != app.dir && skipDir(d.Name()) {
			return filepath.SkipDir
		}
		if hasDirectives(p) {
			r, _ := filepath.Rel(app.dir, p)
			dirs = append(dirs, p)
			byDir[p] = cand{path: "./" + filepath.ToSlash(r), main: true}
		}
		return nil
	})
	var deps map[string]string
	var local []string
	if app.proj != nil {
		deps, local = app.packageDirs(), app.proj.Local
	}
	for id, d := range deps {
		if _, ok := byDir[d]; ok || !hasDirectives(d) {
			continue
		}
		dirs = append(dirs, d)
		byDir[d] = cand{path: id, main: slices.Contains(local, d)}
	}
	sort.Strings(dirs)
	var out []nativePackage
	for _, d := range dirs {
		p, err := nativegen.LoadAsuka(d)
		if err != nil {
			return nil, err
		}
		if p == nil || p.Empty() {
			continue
		}
		c := byDir[d]
		path := c.path
		if p.Path != "" && !c.main {
			path = p.Path
		}
		out = append(out, nativePackage{path: path, name: p.Name, dir: d, main: c.main})
	}
	return out, nil
}

// nativeFor are the app's native packages with native code for platform
// (ios, macos, android, web).
func nativeFor(app *asukaApp, platform string) ([]nativePackage, error) {
	ps, err := appNatives(app)
	if err != nil {
		return nil, err
	}
	var out []nativePackage
	for _, p := range ps {
		switch platform {
		case "ios", "tvos", "macos":
			if !nativegen.HasPlatform(p.dir, "swift") {
				continue
			}
			info, err := swiftPackageInfo(p.dir)
			if err != nil {
				return nil, err
			}
			if !info.supports(platform) {
				continue
			}
		default:
			if !nativegen.HasPlatform(p.dir, platform) {
				continue
			}
		}
		// Google Mobile Ads ends an app without its app ID as it starts:
		// a build without asuka.toml's says so instead.
		if p.path == adsPkg && (platform == "ios" || platform == "android") && app.cfg != nil {
			if err := app.cfg.adsCheck(platform); err != nil {
				return nil, err
			}
		}
		out = append(out, p)
	}
	return out, nil
}

// generateNatives keeps the app's native packages' generated files
// current: those of its own (and of its local dependencies) are written
// again where their declarations changed; a published dependency's,
// stale, is an error (what's published is what it is).
func generateNatives(app *asukaApp) error {
	ps, err := appNatives(app)
	if err != nil {
		return err
	}
	for _, p := range ps {
		if p.main {
			err = generateNative(p.dir)
		} else {
			err = checkNative(p.dir)
		}
		if err != nil {
			return err
		}
	}
	return nil
}

// hasDirectives reports whether a package's Asuka declares a native
// interface or a native component. A generated file's are the prelude's
// elements (lang/prelude's elements.asuka), the runtime's, not a
// package's.
func hasDirectives(dir string) bool {
	es, _ := os.ReadDir(dir)
	for _, e := range es {
		n := e.Name()
		if e.IsDir() || !strings.HasSuffix(n, ".asuka") {
			continue
		}
		if b, err := os.ReadFile(filepath.Join(dir, n)); err == nil && nativeDecl.Match(b) && !bytes.HasPrefix(b, []byte("// Code generated ")) {
			return true
		}
	}
	return false
}

// nativeDecl finds an Asuka file's native declarations.
var nativeDecl = regexp.MustCompile(`(?m)^\s*(pub\s+)?native\s+(interface|component)\b`)

// nativeDirs are the packages under moduleDir with native declarations
// (hasDirectives).
func nativeDirs(moduleDir string) ([]string, error) {
	var dirs []string
	err := filepath.WalkDir(moduleDir, func(p string, d os.DirEntry, err error) error {
		if err != nil || !d.IsDir() {
			return nil
		}
		name := d.Name()
		if p != moduleDir && (strings.HasPrefix(name, ".") || strings.HasPrefix(name, "_") || name == "testdata" || name == "node_modules" ||
			exists(filepath.Join(p, "go.mod"))) {
			return filepath.SkipDir
		}
		// The prelude declares the built-in elements as native components:
		// the runtime's, not a native package's.
		if rel, _ := filepath.Rel(moduleDir, p); filepath.ToSlash(rel) == "lang/prelude" {
			return filepath.SkipDir
		}
		if hasDirectives(p) {
			dirs = append(dirs, p)
		}
		return nil
	})
	sort.Strings(dirs)
	return dirs, err
}

// nativeStamp is what a package's generation depends on: its Asuka
// files' contents, and its projects' presence.
func nativeStamp(dir string) string {
	h := sha256.New()
	es, _ := os.ReadDir(dir)
	for _, e := range es {
		n := e.Name()
		if e.IsDir() || !strings.HasSuffix(n, ".asuka") {
			continue
		}
		b, _ := os.ReadFile(filepath.Join(dir, n))
		fmt.Fprintf(h, "%s %d\n", n, len(b))
		h.Write(b)
	}
	for _, pl := range nativegen.Platforms {
		fmt.Fprintln(h, pl, nativegen.HasPlatform(dir, pl))
	}
	return hex.EncodeToString(h.Sum(nil))
}

// nativeStampPath is where a package's last generation's stamp is kept.
func nativeStampPath(dir string) string {
	sum := sha256.Sum256([]byte(dir))
	return cachedir.Path("native", hex.EncodeToString(sum[:])[:16])
}

// generateNative writes a package's generated files, unless nothing it
// depends on changed since it last did.
func generateNative(dir string) error {
	stamp := nativeStamp(dir)
	sp := nativeStampPath(dir)
	if b, err := os.ReadFile(sp); err == nil && string(b) == stamp {
		if p, _ := nativeHashes(dir); p {
			return nil
		}
	}
	p, err := nativegen.LoadAsuka(dir)
	if err != nil {
		return err
	}
	if p == nil {
		return nil
	}
	wrote, err := nativegen.Write(p)
	if err != nil {
		return err
	}
	for _, w := range wrote {
		fmt.Printf("asuka: wrote %s\n", filepath.Join(rel(dir), w))
	}
	os.MkdirAll(filepath.Dir(sp), 0o755)
	return os.WriteFile(sp, []byte(stamp), 0o644)
}

// nativeHashes reports whether a package's generated files are there,
// all recording one hash.
func nativeHashes(dir string) (bool, string) {
	var hash string
	ok := true
	filepath.WalkDir(dir, func(p string, d os.DirEntry, err error) error {
		if err != nil || d.IsDir() || !strings.Contains(d.Name(), ".asuka.") {
			return nil
		}
		b, _ := os.ReadFile(p)
		h := nativegen.HashOf(b)
		if hash == "" {
			hash = h
		}
		ok = ok && h != "" && h == hash
		return nil
	})
	return ok && hash != "", hash
}

// checkNative fails when a dependency's generated files are stale.
func checkNative(dir string) error {
	stamp := nativeStamp(dir)
	sp := nativeStampPath(dir)
	if b, err := os.ReadFile(sp); err == nil && string(b) == "checked "+stamp {
		return nil
	}
	p, err := nativegen.LoadAsuka(dir)
	if err != nil {
		return err
	}
	if p == nil {
		return nil
	}
	stale, err := nativegen.Stale(p)
	if err != nil {
		return err
	}
	if len(stale) > 0 {
		return fmt.Errorf("%s: its generated native code is stale (%s): run asuka native generate in its module, and publish it", p.Path, strings.Join(stale, ", "))
	}
	os.MkdirAll(filepath.Dir(sp), 0o755)
	return os.WriteFile(sp, []byte("checked "+stamp), 0o644)
}

// rel is p relative to the working directory, when it's under it.
func rel(p string) string {
	if wd, err := os.Getwd(); err == nil {
		if r, err := filepath.Rel(wd, p); err == nil && !strings.HasPrefix(r, "..") {
			return r
		}
	}
	return p
}

// swiftInfo is what a package's Package.swift says that the CLI needs:
// the platforms it's for (none: all), and its library products.
type swiftInfo struct {
	Platforms []string `json:"platforms"`
	Products  []string `json:"products"`
	// Module is its one target when the CLI compiles it as a module of
	// its own (swiftModules): Swift alone, depending on the host's only,
	// with no resources or settings. Nil: xcodebuild builds it.
	Module *swiftModuleInfo `json:"module,omitempty"`
	// Its minimum versions, as its platforms say ("" where they don't).
	IOS   string `json:"ios"`
	TVOS  string `json:"tvos"`
	MacOS string `json:"macos"`
}

// swiftMinimum is the least version of platform ("ios", "macos") an
// app runs on with natives' Swift: the host's, or a package's higher
// one. The app's own Package.swift and Info.plist say it, so a package
// that needs a later system (a camera's, macOS 13) builds into the app.
func swiftMinimum(natives []nativePackage, platform string) (string, error) {
	min := map[string]string{"ios": iosMin, "tvos": tvosMin, "macos": macMin}[platform]
	for _, p := range natives {
		if !nativegen.HasPlatform(p.dir, "swift") {
			continue
		}
		info, err := swiftPackageInfo(p.dir)
		if err != nil {
			return "", err
		}
		v := map[string]string{"ios": info.IOS, "tvos": info.TVOS, "macos": info.MacOS}[platform]
		if versionLess(min, v) {
			min = v
		}
	}
	return min, nil
}

// versionLess reports whether dotted version a is below b (11.0 < 13.0,
// 13 < 13.1); an empty b is below everything.
func versionLess(a, b string) bool {
	if b == "" {
		return false
	}
	as, bs := strings.Split(a, "."), strings.Split(b, ".")
	for i := 0; i < max(len(as), len(bs)); i++ {
		var x, y int
		if i < len(as) {
			x, _ = strconv.Atoi(as[i])
		}
		if i < len(bs) {
			y, _ = strconv.Atoi(bs[i])
		}
		if x != y {
			return x < y
		}
	}
	return false
}

// swiftModuleInfo is a package's target that swiftModules compiles.
type swiftModuleInfo struct {
	Name   string `json:"name"`
	Path   string `json:"path"` // its directory in the package
	IOS    string `json:"ios"`  // its minimum versions ("" for the app's)
	TVOS   string `json:"tvos"`
	MacOS  string `json:"macos"`
	Swift6 bool   `json:"swift6"` // tools version 6: Swift 6's language mode
}

func (s swiftInfo) supports(platform string) bool {
	return len(s.Platforms) == 0 || slices.Contains(s.Platforms, platform)
}

// swiftPackageInfo reads a package's Package.swift (swift package
// dump-package), once for each of its contents.
func swiftPackageInfo(dir string) (swiftInfo, error) {
	manifest, err := os.ReadFile(filepath.Join(dir, "Package.swift"))
	if err != nil {
		return swiftInfo{}, err
	}
	sum := sha256.Sum256(manifest)
	cache := cachedir.Path("swiftpm", "4-"+hex.EncodeToString(sum[:])[:16]+".json")
	var info swiftInfo
	if b, err := os.ReadFile(cache); err == nil && json.Unmarshal(b, &info) == nil {
		return info, nil
	}
	if _, err := exec.LookPath("swift"); err != nil {
		return info, fmt.Errorf("%s has Swift (Package.swift): install Xcode", dir)
	}
	c := exec.Command("swift", "package", "dump-package", "--package-path", dir)
	var stderr bytes.Buffer
	c.Stderr = &stderr
	out, err := c.Output()
	if err != nil {
		return info, fmt.Errorf("%s/Package.swift: %v\n%s", dir, err, stderr.Bytes())
	}
	var dump struct {
		Platforms []struct {
			PlatformName string `json:"platformName"`
			Version      string `json:"version"`
		} `json:"platforms"`
		Products []struct {
			Name string                     `json:"name"`
			Type map[string]json.RawMessage `json:"type"`
		} `json:"products"`
		Dependencies []map[string][]struct {
			Identity string `json:"identity"`
		} `json:"dependencies"`
		Targets []struct {
			Name         string             `json:"name"`
			Type         string             `json:"type"`
			Path         *string            `json:"path"`
			Sources      []string           `json:"sources"`
			Exclude      []string           `json:"exclude"`
			Resources    []json.RawMessage  `json:"resources"`
			Settings     []json.RawMessage  `json:"settings"`
			Dependencies []map[string][]any `json:"dependencies"`
		} `json:"targets"`
		ToolsVersion struct {
			Version string `json:"_version"`
		} `json:"toolsVersion"`
	}
	if err := json.Unmarshal(out, &dump); err != nil {
		return info, fmt.Errorf("%s/Package.swift: %v", dir, err)
	}
	mod := &swiftModuleInfo{Swift6: strings.HasPrefix(dump.ToolsVersion.Version, "6")}
	for _, p := range dump.Platforms {
		switch p.PlatformName {
		case "ios":
			mod.IOS, info.IOS = p.Version, p.Version
		case "tvos":
			mod.TVOS, info.TVOS = p.Version, p.Version
		case "macos":
			mod.MacOS, info.MacOS = p.Version, p.Version
		}
		if p.PlatformName == "ios" || p.PlatformName == "tvos" || p.PlatformName == "macos" {
			info.Platforms = append(info.Platforms, p.PlatformName)
		}
	}
	// A module of its own: its packages' only dependency the host's
	// (asuka), one regular target that depends on the host's product
	// alone, with no resources, settings or lists of files.
	for _, d := range dump.Dependencies {
		for _, refs := range d {
			for _, r := range refs {
				if r.Identity != "asuka" {
					mod = nil
				}
			}
		}
	}
	var targets int
	for _, t := range dump.Targets {
		if t.Type == "test" {
			continue
		}
		targets++
		if mod == nil {
			continue
		}
		if t.Type != "regular" || len(t.Resources) > 0 || len(t.Settings) > 0 || len(t.Sources) > 0 || len(t.Exclude) > 0 {
			mod = nil
			continue
		}
		for _, d := range t.Dependencies {
			p, ok := d["product"]
			if !ok || len(p) < 2 || p[0] != "Asuka" || p[1] != "asuka" {
				mod = nil
			}
		}
		if mod != nil {
			mod.Name = t.Name
			mod.Path = filepath.Join("Sources", t.Name)
			if t.Path != nil {
				mod.Path = filepath.FromSlash(*t.Path)
			}
		}
	}
	if targets == 1 && mod != nil {
		info.Module = mod
	}
	for _, p := range dump.Products {
		if _, ok := p.Type["library"]; ok {
			info.Products = append(info.Products, p.Name)
		}
	}
	if len(info.Products) == 0 {
		return info, fmt.Errorf("%s/Package.swift has no library product: the app links one", dir)
	}
	os.MkdirAll(filepath.Dir(cache), 0o755)
	b, _ := json.Marshal(info)
	os.WriteFile(cache, b, 0o644)
	return info, nil
}

// nativeNeeds gives the app what its native packages' Swift needs of the
// platform (ios, macos): their Info.plist's keys (beside Package.swift),
// arrays joining what others add, the rest where nothing else set the
// key; and their entitlements (iOS.entitlements, macOS.entitlements).
func (c *appConfig) nativeNeeds(ps []nativePackage, platform string) error {
	ent := map[string]string{"ios": "iOS.entitlements", "tvos": "tvOS.entitlements", "macos": "macOS.entitlements"}[platform]
	for _, p := range ps {
		if m, err := readPlistDict(filepath.Join(p.dir, "Info.plist")); err != nil {
			return err
		} else if m != nil {
			for k, v := range m {
				if c.nativePlist == nil {
					c.nativePlist = map[string]any{}
				}
				switch x := v.(type) {
				case []any:
					have, _ := c.nativePlist[k].([]string)
					for _, e := range x {
						if s, ok := e.(string); ok && !slices.Contains(have, s) {
							have = append(have, s)
						}
					}
					sort.Strings(have)
					c.nativePlist[k] = have
				case string:
					// A purpose string is a usage: the app's asuka.toml says better.
					if strings.HasSuffix(k, "UsageDescription") {
						if c.IOS.Usage == nil {
							c.IOS.Usage = map[string]string{}
						}
						if c.IOS.Usage[k] == "" {
							c.IOS.Usage[k] = x
						}
					} else if _, ok := c.nativePlist[k]; !ok {
						c.nativePlist[k] = x
					}
				default:
					if _, ok := c.nativePlist[k]; !ok {
						c.nativePlist[k] = plistEntitlement(v)
					}
				}
			}
		}
		m, err := readPlistDict(filepath.Join(p.dir, ent))
		if err != nil {
			return err
		}
		for k, v := range m {
			if c.entitlements == nil {
				c.entitlements = map[string]any{}
			}
			if _, ok := c.entitlements[k]; !ok {
				c.entitlements[k] = plistEntitlement(v)
			}
			if c.entitlementsFor == nil {
				c.entitlementsFor = map[string]string{}
			}
			if c.entitlementsFor[k] == "" {
				c.entitlementsFor[k] = k + " (" + p.path + ")"
			}
		}
	}
	return nil
}

// readPlistDict reads a property list's dictionary (nil, no error: no
// file).
func readPlistDict(path string) (map[string]any, error) {
	b, err := os.ReadFile(path)
	if os.IsNotExist(err) {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	v, err := parsePlist(b)
	if err != nil {
		return nil, fmt.Errorf("%s: %w", path, err)
	}
	m, ok := v.(map[string]any)
	if !ok {
		return nil, fmt.Errorf("%s: not a dictionary", path)
	}
	return m, nil
}

// nativeDirsOf are the directories asuka dev watches for a platform's
// native code (the Swift's, the Kotlin's, the JavaScript's).
func nativeDirsOf(ps []nativePackage, platform string) []string {
	var out []string
	for _, p := range ps {
		switch platform {
		case "ios", "tvos", "macos":
			out = append(out, filepath.Join(p.dir, nativegen.SwiftDir), filepath.Join(p.dir, "Package.swift"))
		default:
			out = append(out, filepath.Join(p.dir, platform))
		}
	}
	return out
}

// androidNatives sets the app's native packages' Gradle modules (their
// android/), named by their packages.
func (c *appConfig) androidNatives(app *asukaApp) error {
	natives, err := nativeFor(app, "android")
	if err != nil {
		return err
	}
	c.nativeModules = nil
	for _, p := range natives {
		c.nativeModules = append(c.nativeModules, [2]string{"native-" + strings.ToLower(nativegen.Sanitize(p.path)), filepath.Join(p.dir, "android")})
	}
	return nil
}
