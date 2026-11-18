// Package prebuilt is what a release of the CLI carries prebuilt beside
// it (DESIGN.md, "The build cache"): the core's, the desktop hosts' and
// the std and sdk packages' C as static libraries for every target and
// configuration, and the Android host's and packages' Kotlin as AARs,
// attached to the release on GitHub, an archive per target
// (asuka-prebuilt-<name>-<version>.tar.gz). The CLI embeds only their
// manifest (manifest.json: each archive's name and SHA-256, each
// library's path and SHA-256), and fetches a target's archive from the
// release the first time a build needs it, into the build cache.
// A CLI built from a checkout has none (an empty manifest): its builds
// build the same libraries into the cache.
package prebuilt

import (
	_ "embed"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"sync"

	"github.com/egoist/asuka/internal/ccache"
)

// Manifest is what the CLI knows of its prebuilt packages.
type Manifest struct {
	// Version is the CLI's, which its packages share.
	Version string `json:"version,omitempty"`
	// Packages by their short names (darwin-arm64, android-aar).
	Packages map[string]Package `json:"packages"`
	// Archives by target/config/unit (darwin-arm64/release/core).
	Archives map[string]File `json:"archives"`
	// Maven is the Android AARs' local Maven repository: the package and
	// the directory in it.
	Maven *File `json:"maven,omitempty"`
}

// Package is one target's prebuilt set: a release asset, a gzip
// compressed tar archive.
type Package struct {
	Asset   string `json:"asset"`   // asuka-prebuilt-darwin-arm64-0.3.0.tar.gz (AssetName)
	Version string `json:"version"` // the release's
	SHA256  string `json:"sha256"`  // the archive's, which the CLI pins
	// Size is the archive's, in bytes (for what a fetch says).
	Size int64 `json:"size,omitempty"`
	// Compiler is what built its C (zig, apple, ndk): a build links a
	// prebuilt library only with the same family's.
	Compiler string `json:"compiler,omitempty"`
	// Swift is the version of the swiftc that built its Swift modules
	// (6.1.2): a build uses them with that one or newer.
	Swift string `json:"swift,omitempty"`
}

// File is a file (or a directory) in a package.
type File struct {
	Package string `json:"package"`          // its target (darwin-arm64)
	Path    string `json:"path"`             // in the archive (lib/release/libcore.a)
	SHA256  string `json:"sha256,omitempty"` // a file's
}

//go:embed manifest.json
var embedded []byte

var (
	manifestOnce sync.Once
	manifest     Manifest
	manifestErr  error
)

// Current is the manifest: ASUKA_PREBUILT_MANIFEST's (a file: tests, a
// release's checks), else the one embedded in the CLI.
func Current() (Manifest, error) {
	manifestOnce.Do(func() {
		b := embedded
		if p := os.Getenv("ASUKA_PREBUILT_MANIFEST"); p != "" {
			if b, manifestErr = os.ReadFile(p); manifestErr != nil {
				return
			}
		}
		manifestErr = json.Unmarshal(b, &manifest)
		if manifestErr != nil {
			manifestErr = fmt.Errorf("prebuilt: the manifest: %v", manifestErr)
		}
	})
	return manifest, manifestErr
}

// reset forgets the manifest read (tests).
func reset() {
	manifestOnce = sync.Once{}
	manifest = Manifest{}
	manifestErr = nil
}

// ArchiveKey is an archive's key in the manifest.
func ArchiveKey(target, config, unit string) string { return target + "/" + config + "/" + unit }

// Compiler is the compiler family target's libraries were built with,
// or "" when the CLI has none for it.
func Compiler(target string) string {
	m, err := Current()
	if err != nil {
		return ""
	}
	for k, f := range m.Archives {
		if len(k) > len(target) && k[:len(target)+1] == target+"/" {
			return m.Packages[f.Package].Compiler
		}
	}
	return ""
}

// Archive is the prebuilt library of unit for target and config, fetched
// into the cache first if it isn't there; ok is false when the CLI has
// none.
func Archive(target, config, unit string) (path string, ok bool, err error) {
	m, err := Current()
	if err != nil {
		return "", false, err
	}
	f, ok := m.Archives[ArchiveKey(target, config, unit)]
	if !ok {
		return "", false, nil
	}
	p, err := m.file(f)
	return p, err == nil, err
}

// SwiftKey is a Swift module's key in the manifest's archives: a
// package's (DeviceNative) or the host's variant (Asuka@base).
func SwiftKey(target, config, module string) string {
	return target + "/" + config + "/swift/" + module
}

// SwiftModule is the directory of a prebuilt Swift module (a static
// library, lib<module>.a, and its interfaces, <module>.swiftmodule/
// <triple>.swiftinterface: an XCFramework's slice) for target and
// config, fetched first if it isn't there, and the version of the swiftc
// that built it; ok is false when the CLI has none.
func SwiftModule(target, config, module string) (dir, swift string, ok bool, err error) {
	m, err := Current()
	if err != nil {
		return "", "", false, err
	}
	f, ok := m.Archives[SwiftKey(target, config, module)]
	if !ok {
		return "", "", false, nil
	}
	p, err := m.file(f)
	if err != nil {
		return "", "", false, err
	}
	return p, m.Packages[f.Package].Swift, true, nil
}

// Released is whether the CLI carries prebuilt packages: a release's
// does, one built from a checkout doesn't.
func Released() bool {
	m, err := Current()
	return err == nil && len(m.Packages) > 0
}

// HostTarget is the package of what this computer builds for itself
// (goos and goarch: runtime's, darwin and arm64): its desktop apps'
// libraries, darwin-arm64, linux-x64, windows-arm64.
func HostTarget(goos, goarch string) string {
	if goarch == "amd64" {
		goarch = "x64"
	}
	return goos + "-" + goarch
}

// FetchTarget makes sure target's package (darwin-arm64, ios-arm64:
// asuka prebuilt targets) is in the cache, as a build would the first
// time it needs one of its libraries, and returns its directory and
// package; ok is false when the CLI has none for it (a CLI built from a
// checkout has none at all).
func FetchTarget(target string) (dir string, pkg Package, ok bool, err error) {
	m, err := Current()
	if err != nil {
		return "", Package{}, false, err
	}
	pkg, ok = m.Packages[target]
	if !ok {
		return "", Package{}, false, nil
	}
	dir, err = Fetch(pkg)
	return dir, pkg, err == nil, err
}

// MavenRepo is the local Maven repository of the Android host's and the
// std and sdk packages' AARs, fetched first if it isn't there; ok is
// false when the CLI has none.
func MavenRepo() (dir string, ok bool, err error) {
	m, err := Current()
	if err != nil {
		return "", false, err
	}
	if m.Maven == nil {
		return "", false, nil
	}
	p, err := m.file(*m.Maven)
	return p, err == nil, err
}

var (
	verifiedMu sync.Mutex
	verified   = map[string]bool{}
)

// file is f's path in its package, fetched; a file is checked against
// its SHA-256 once per process.
func (m Manifest) file(f File) (string, error) {
	pkg, ok := m.Packages[f.Package]
	if !ok {
		return "", fmt.Errorf("prebuilt: the manifest names no package %q", f.Package)
	}
	dir, err := Fetch(pkg)
	if err != nil {
		return "", err
	}
	p := filepath.Join(dir, filepath.FromSlash(f.Path))
	if f.SHA256 == "" {
		return p, nil
	}
	verifiedMu.Lock()
	done := verified[p]
	verifiedMu.Unlock()
	if done {
		return p, nil
	}
	sum, err := sha256File(p)
	if err != nil {
		return "", fmt.Errorf("prebuilt: %s: %v", p, err)
	}
	if sum != f.SHA256 {
		// Damaged in the cache: fetched again next time.
		os.RemoveAll(dir)
		return "", fmt.Errorf("prebuilt: %s isn't the library %s names (SHA-256); it was removed, build again", p, pkg.Asset)
	}
	verifiedMu.Lock()
	verified[p] = true
	verifiedMu.Unlock()
	ccache.Touch(dir)
	return p, nil
}
