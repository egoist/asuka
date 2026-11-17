package main

import (
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"

	"github.com/egoist/asuka/internal/prebuilt"
)

// packPrebuilt makes an npm package of each target's directory in dir
// (asuka prebuilt build's: its files and prebuilt.json), packs it into
// out/tarballs, and writes the CLI's manifest of them, pinning each
// tarball's integrity, into manifest (internal/prebuilt/manifest.json)
// and out.
// It returns the tarballs.
func packPrebuilt(dir, out, version, manifest string) ([]tarball, error) {
	es, err := os.ReadDir(dir)
	if err != nil {
		return nil, err
	}
	m := prebuilt.Manifest{Version: version, Packages: map[string]prebuilt.Package{}, Archives: map[string]prebuilt.File{}}
	tdir, _ := filepath.Abs(filepath.Join(out, "tarballs"))
	var tarballs []tarball
	for _, e := range es {
		if !e.IsDir() {
			continue
		}
		src := filepath.Join(dir, e.Name())
		b, err := os.ReadFile(filepath.Join(src, "prebuilt.json"))
		if err != nil {
			return nil, fmt.Errorf("%s: %v (asuka prebuilt build's output?)", src, err)
		}
		var c struct {
			Name     string                   `json:"name"`
			Compiler string                   `json:"compiler"`
			Swift    string                   `json:"swift"`
			Files    map[string]prebuilt.File `json:"files"`
			Maven    string                   `json:"maven"`
		}
		if err := json.Unmarshal(b, &c); err != nil {
			return nil, fmt.Errorf("%s: %v", src, err)
		}
		name := prebuilt.Scope + "/asuka-prebuilt-" + c.Name
		files := []string{"prebuilt.json"}
		for _, f := range []string{"lib", "swift", "maven"} {
			if _, err := os.Stat(filepath.Join(src, f)); err == nil {
				files = append(files, f)
			}
		}
		what := "the C libraries"
		if c.Swift != "" {
			what += " and Swift modules"
		}
		if c.Maven != "" {
			what = "the Android libraries (AARs)"
		}
		writeJSON(filepath.Join(src, "package.json"), map[string]any{
			"name":        name,
			"version":     version,
			"description": "Asuka's prebuilt " + what + " for " + c.Name + " (the asuka CLI fetches it when a build needs it)",
			"license":     "MIT",
			"repository":  "github:egoist/asuka",
			"files":       files,
		})
		tgz, integrity, err := pack(src, tdir)
		if err != nil {
			return nil, err
		}
		// What npm publish uploads is this file: the integrity the
		// registry will say, which the CLI pins.
		m.Packages[c.Name] = prebuilt.Package{Name: name, Version: version, Integrity: integrity, Compiler: c.Compiler, Swift: c.Swift}
		for k, f := range c.Files {
			f.Package = c.Name
			m.Archives[k] = f
		}
		if c.Maven != "" {
			m.Maven = &prebuilt.File{Package: c.Name, Path: c.Maven}
		}
		tarballs = append(tarballs, tarball{tgz, m.Packages[c.Name]})
		fmt.Println("packed", name, "→", tgz)
	}
	sort.Slice(tarballs, func(i, j int) bool { return tarballs[i].path < tarballs[j].path })
	b, err := json.MarshalIndent(m, "", "  ")
	if err != nil {
		return nil, err
	}
	b = append(b, '\n')
	if err := os.WriteFile(manifest, b, 0o644); err != nil {
		return nil, err
	}
	if err := os.WriteFile(filepath.Join(out, "prebuilt-manifest.json"), b, 0o644); err != nil {
		return nil, err
	}
	fmt.Printf("the manifest: %d packages, %d libraries (%s)\n", len(m.Packages), len(m.Archives), strings.Join(sortedKeys(m.Packages), ", "))
	return tarballs, nil
}

// tarball is a package packed for npm publish: its file, and its name,
// version and integrity, which the registry serves it with.
type tarball struct {
	path string
	pkg  prebuilt.Package
}

// pack packs the npm package in src into dir (npm pack), returning the
// tarball and its integrity: what npm publish uploads, so what the
// registry will serve.
func pack(src, dir string) (tgz, integrity string, err error) {
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return "", "", err
	}
	cmd := exec.Command("npm", "pack", "--pack-destination", dir, "--json")
	cmd.Dir = src
	cmd.Stderr = os.Stderr
	res, err := cmd.Output()
	if err != nil {
		return "", "", fmt.Errorf("npm pack %s: %v", src, err)
	}
	var packed []struct {
		Filename string `json:"filename"`
	}
	if err := json.Unmarshal(res, &packed); err != nil || len(packed) != 1 {
		return "", "", fmt.Errorf("npm pack %s: %s", src, res)
	}
	tgz = filepath.Join(dir, filepath.Base(packed[0].Filename))
	data, err := os.ReadFile(tgz)
	if err != nil {
		return "", "", err
	}
	return tgz, prebuilt.Integrity(data), nil
}

func sortedKeys[V any](m map[string]V) []string {
	var out []string
	for k := range m {
		out = append(out, k)
	}
	sort.Strings(out)
	return out
}
