// Command npm builds the asuka CLI for every platform and lays out its
// npm packages: @egoist/asuka (the launcher, npm/cli) and one
// @egoist/asuka-<os>-<cpu> per build, with only its binary. With
// -prebuilt, a directory of what asuka prebuilt build made for each of
// its targets (the release workflow's), it packs those too, one
// @egoist/asuka-prebuilt-<target> each, and writes their manifest
// (internal/prebuilt/manifest.json: each package's version and tarball's
// integrity, each library's path and SHA-256) before building the CLI,
// which embeds it. With -publish it publishes them (npm publish, with
// the user's npm credentials; --provenance on CI): the prebuilt packages
// and the platform packages, then, once the registry serves every one of
// them (their tarballs downloaded and checked as the CLI does; up to
// -wait), the launcher. A registry accepts a package before it serves it,
// a new package's tarball for minutes after: a launcher published before
// would install a CLI whose builds can't fetch what they need.
//
//	go run ./scripts/npm -version 0.1.0 [-prebuilt dist/prebuilt] [-out dist/npm] [-publish] [-wait 30m]
package main

import (
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"sync"
	"time"

	"github.com/egoist/asuka/internal/prebuilt"
)

var platforms = []struct{ goos, goarch, os, cpu string }{
	{"darwin", "arm64", "darwin", "arm64"}, // Apple silicon: an Intel Mac isn't one Asuka develops on
	{"linux", "arm64", "linux", "arm64"},
	{"linux", "amd64", "linux", "x64"},
	{"windows", "arm64", "win32", "arm64"},
	{"windows", "amd64", "win32", "x64"},
}

func main() {
	version := flag.String("version", "", "the release's version (semver)")
	out := flag.String("out", filepath.Join("dist", "npm"), "where the packages go")
	publish := flag.Bool("publish", false, "npm publish them")
	tag := flag.String("tag", "latest", "the dist-tag to publish under")
	pre := flag.String("prebuilt", "", "a directory of asuka prebuilt build's output, a directory per target")
	manifest := flag.String("manifest", filepath.Join("internal", "prebuilt", "manifest.json"), "where -prebuilt writes the CLI's manifest")
	only := flag.Bool("prebuilt-only", false, "only pack the prebuilt packages and write their manifest")
	wait := flag.Duration("wait", 30*time.Minute, "how long -publish waits for the registry to serve the packages before it publishes the launcher")
	flag.Parse()
	if *version == "" {
		fail("npm: -version is required")
	}
	if err := os.RemoveAll(*out); err != nil {
		fail(err.Error())
	}
	var tarballs []tarball
	if *pre != "" {
		var err error
		if tarballs, err = packPrebuilt(*pre, *out, *version, *manifest); err != nil {
			fail(err.Error())
		}
		if *only {
			return
		}
	}
	cliFlags := "-s -w -X main.version=" + *version
	deps := map[string]string{}
	for _, p := range platforms {
		name := "@egoist/asuka-" + p.os + "-" + p.cpu
		dir := filepath.Join(*out, "cli-"+p.os+"-"+p.cpu)
		exe := "asuka"
		if p.goos == "windows" {
			exe += ".exe"
		}
		bin := filepath.Join(dir, "bin", exe)
		fmt.Println("building", name)
		cmd := exec.Command("go", "build", "-trimpath", "-ldflags", cliFlags, "-o", bin, "./cmd/asuka")
		cmd.Env = append(os.Environ(), "GOOS="+p.goos, "GOARCH="+p.goarch, "CGO_ENABLED=0")
		cmd.Stdout, cmd.Stderr = os.Stdout, os.Stderr
		if err := cmd.Run(); err != nil {
			fail(fmt.Sprintf("npm: building %s: %v", name, err))
		}
		writeJSON(filepath.Join(dir, "package.json"), map[string]any{
			"name":        name,
			"version":     *version,
			"description": "The asuka CLI for " + p.os + " " + p.cpu + " (installed by @egoist/asuka)",
			"license":     "MIT",
			"repository":  "github:egoist/asuka",
			"os":          []string{p.os},
			"cpu":         []string{p.cpu},
			"files":       []string{"bin"},
		})
		deps[name] = *version
		tgz, integrity, err := pack(dir, filepath.Join(*out, "tarballs"))
		if err != nil {
			fail(err.Error())
		}
		tarballs = append(tarballs, tarball{tgz, prebuilt.Package{Name: name, Version: *version, Integrity: integrity}})
	}
	// The launcher: npm/cli with the version and its dependencies set.
	main := filepath.Join(*out, "cli")
	if err := os.MkdirAll(filepath.Join(main, "bin"), 0o755); err != nil {
		fail(err.Error())
	}
	b, err := os.ReadFile(filepath.Join("npm", "cli", "bin", "asuka.js"))
	if err != nil {
		fail(err.Error())
	}
	if err := os.WriteFile(filepath.Join(main, "bin", "asuka.js"), b, 0o755); err != nil {
		fail(err.Error())
	}
	var pkg map[string]any
	b, err = os.ReadFile(filepath.Join("npm", "cli", "package.json"))
	if err != nil {
		fail(err.Error())
	}
	if err := json.Unmarshal(b, &pkg); err != nil {
		fail(err.Error())
	}
	pkg["version"] = *version
	pkg["optionalDependencies"] = deps
	writeJSON(filepath.Join(main, "package.json"), pkg)
	if !*publish {
		fmt.Println("packages in", *out, "(add -publish to publish them)")
		return
	}
	for _, t := range tarballs {
		npmPublish("", t.path, *tag)
	}
	if err := waitServed(tarballs, *wait); err != nil {
		fail(err.Error())
	}
	npmPublish(main, "", *tag)
}

// npmPublish publishes a package: a tarball, or the directory dir.
func npmPublish(dir, tgz, tag string) {
	args := []string{"publish"}
	if tgz != "" {
		args = append(args, tgz)
	}
	args = append(args, "--access", "public", "--tag", tag)
	if os.Getenv("CI") != "" {
		args = append(args, "--provenance")
	}
	cmd := exec.Command("npm", args...)
	cmd.Dir = dir
	cmd.Stdout, cmd.Stderr = os.Stdout, os.Stderr
	if err := cmd.Run(); err != nil {
		fail(fmt.Sprintf("npm publish %s: %v", dir+tgz, err))
	}
}

// waitServed waits until the registry serves every tarball (the one
// published, by its integrity, fetched as the CLI fetches its prebuilt
// packages), trying each again every 20s, for up to timeout.
func waitServed(tarballs []tarball, timeout time.Duration) error {
	deadline := time.Now().Add(timeout)
	errs := make([]error, len(tarballs))
	var wg sync.WaitGroup
	for i, t := range tarballs {
		wg.Add(1)
		go func() {
			defer wg.Done()
			p := t.pkg
			for {
				err := prebuilt.Check(p)
				if err == nil {
					fmt.Printf("served: %s@%s\n", p.Name, p.Version)
					return
				}
				if errors.Is(err, prebuilt.ErrIntegrity) || time.Now().After(deadline) {
					errs[i] = fmt.Errorf("%s@%s: %v", p.Name, p.Version, err)
					return
				}
				fmt.Printf("waiting for the registry to serve %s@%s (%v)\n", p.Name, p.Version, err)
				time.Sleep(20 * time.Second)
			}
		}()
	}
	wg.Wait()
	if err := errors.Join(errs...); err != nil {
		return fmt.Errorf("npm: the registry doesn't serve what was published, so the launcher (@egoist/asuka) wasn't published:\n%v", err)
	}
	return nil
}

func writeJSON(path string, v any) {
	b, err := json.MarshalIndent(v, "", "  ")
	if err != nil {
		fail(err.Error())
	}
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		fail(err.Error())
	}
	if err := os.WriteFile(path, append(b, '\n'), 0o644); err != nil {
		fail(err.Error())
	}
}

func fail(msg string) {
	fmt.Fprintln(os.Stderr, msg)
	os.Exit(1)
}
