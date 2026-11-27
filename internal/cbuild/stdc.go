package cbuild

import (
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"github.com/egoist/asuka/internal/ccache"
	"github.com/egoist/asuka/internal/pkgs"
)

// CPlatform is the [c.<platform>] table (pkgs.CPlatforms) a target reads.
func (t Target) CPlatform() string {
	switch t.OS {
	case "ios", "ios-simulator", "tvos", "tvos-simulator", "watchos", "watchos-simulator":
		return "ios" // tvOS's and watchOS's C is iOS's: its [c.ios]
	case "darwin":
		return "macos"
	case "wasm":
		return "web"
	}
	return t.OS // android, windows, linux
}

// CTarget is the target's key in asuka.toml's [c.prebuilt]
// (pkgs.CTargets): ios-arm64, ios-simulator-x64, macos-arm64,
// android-arm, linux-x64, web.
func (t Target) CTarget() string {
	if t.OS == "wasm" {
		return "web"
	}
	arch := t.Arch
	if arch == "amd64" {
		arch = "x64"
	}
	p := t.OS
	if p == "darwin" {
		p = "macos"
	}
	return p + "-" + arch
}

// HasC reports whether the package in dir has C of its own: its
// asuka.toml's [c].
func HasC(dir string) bool {
	c, err := pkgs.ReadC(dir)
	return err == nil && !c.Empty()
}

// StdC is the C of a std or sdk package (its asuka.toml's [c], paths
// relative to dir) for t, a unit: std/sqlite's is SQLite and asq,
// std-sqlite-c. A release prebuilds it for every target (internal/
// prebuilt, embedded: dir is the CLI's own copy); an app that imports the
// package links it (core/link names it: AK_STD_<NAME>). Its libraries and
// frameworks are the link's.
func StdC(dir string, t Target, embedded bool) (Unit, []string, error) {
	cfg, err := pkgs.ReadC(dir)
	if err != nil {
		return Unit{}, nil, err
	}
	s := cfg.For(t.CPlatform())
	manifest := filepath.Join(dir, pkgs.ManifestFile)
	srcs, err := cfg.CSources(dir, s)
	if err != nil {
		return Unit{}, nil, fmt.Errorf("%s: %v", manifest, err)
	}
	kind, name := "std", filepath.Base(dir)
	if filepath.Base(filepath.Dir(dir)) == "sdk" {
		kind = "sdk"
	}
	u := Unit{Name: kind + "-" + name + "-c", Sources: srcs, Embedded: embedded,
		Link: "AK_" + strings.ToUpper(kind+"_"+strings.ReplaceAll(name, "-", "_"))}
	if len(cfg.Download) > 0 {
		u.Fetch = func() error { return cfg.FetchFor(s) }
	}
	for _, inc := range s.Include {
		p, _, err := cfg.Path(dir, inc)
		if err != nil {
			return Unit{}, nil, fmt.Errorf("%s: %v", manifest, err)
		}
		u.Flags = append(u.Flags, "-I"+p)
	}
	for _, d := range s.Defines {
		u.Flags = append(u.Flags, "-D"+d)
	}
	u.Flags = append(u.Flags, s.Flags...)
	var link []string
	for _, l := range s.Libraries {
		if strings.ContainsAny(l, `/\`) {
			p, d, err := cfg.Path(dir, l)
			if err != nil {
				return Unit{}, nil, fmt.Errorf("%s: %v", manifest, err)
			}
			if d != nil {
				if err := d.Fetch(); err != nil {
					return Unit{}, nil, err
				}
			}
			link = append(link, p)
		} else {
			link = append(link, "-l"+l)
		}
	}
	if t.Apple() {
		for _, fw := range s.Frameworks {
			link = append(link, "-framework", fw)
		}
	}
	return u, link, nil
}

// WasmModule links u (a std package's C for the web) into a WebAssembly
// module of its own, as a worker of the page instantiates it (std/sqlite's
// asqlite.wasm), exporting exports and importing only what its C
// declares: u's library (prebuilt, or built into the cache), linked once
// into the cache for each version of it.
func (tc *Toolchain) WasmModule(u Unit, exports []string, debug bool, name string) (string, error) {
	defer ccache.Flush()
	if tc.Target.OS != "wasm" {
		return "", fmt.Errorf("cbuild: a WebAssembly module of %s for %s", u.Name, tc.Target)
	}
	lib, err := tc.UnitArchive(u, debug)
	if err != nil {
		return "", err
	}
	data, err := os.ReadFile(lib)
	if err != nil {
		return "", err
	}
	sum := sha256.Sum256(data)
	key := ccache.Key(append([]string{"c-wasm-module-1", tc.CC.Identity(), hex.EncodeToString(sum[:]), Config(debug)}, exports...)...)
	out := filepath.Join(ccache.Sharded("c", "x", key, ""), name)
	if _, err := os.Stat(out); err == nil {
		ccache.Touch(filepath.Dir(out))
		return out, nil
	}
	ld := append([]string{}, tc.Flags...)
	ld = append(ld, "-mexec-model=reactor", "-Wl,--gc-sections",
		// SQLite's parser and alloca want more than wasm-ld's 64 KiB.
		"-Wl,-z,stack-size=1048576")
	if !debug {
		ld = append(ld, "-s")
	}
	for _, e := range exports {
		ld = append(ld, "-Wl,--export="+e)
	}
	if err := tc.link(nil, []archiveRef{{lib, true}}, ld, out); err != nil {
		return "", err
	}
	return out, nil
}
