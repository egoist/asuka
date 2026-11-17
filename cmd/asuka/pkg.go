package main

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"github.com/egoist/asuka/internal/pkgs"
)

// Packages (docs/packages.md): asuka add, remove, install; asuka pkg
// pack and publish.

func cmdAdd(args []string) error {
	var dir string
	fs := newFlagSet("add", "<package>[@range]...")
	fs.about = "Adds dependencies to the project's asuka.toml (the latest version, as ^x.y.z, unless a\n" +
		"range is given; npm:<name>@<range>, github:<owner>/<repo>#<ref> and file:<dir> too), resolves\n" +
		"their versions into asuka.lock and fetches them."
	fs.String(&dir, "dir", 'C', ".", "the project's directory")
	if err := fs.parse(args); err != nil {
		return err
	}
	if len(fs.Args()) == 0 {
		return errors.New("add: name the packages to add")
	}
	c := pkgs.NewClient(pkgs.LoadNPMRC(dir))
	for _, a := range fs.Args() {
		key, spec, err := addSpec(c, a)
		if err != nil {
			return fmt.Errorf("add: %v", err)
		}
		if err := pkgs.SetDependency(dir, key, spec); err != nil {
			return fmt.Errorf("add: %v", err)
		}
		fmt.Printf("added %s = %q\n", key, spec)
	}
	return install(c, dir, false)
}

// addSpec turns what asuka add was given into a dependency's key and
// spec: name, name@range, key=spec.
func addSpec(c *pkgs.Client, a string) (key, spec string, err error) {
	if pkgs.CheckName(a) != nil {
		// key=spec's key, name@range's name: what it'd be imported as.
		return "", "", fmt.Errorf("%s: the standard library (std/) and the platform SDK (platform/) come with the asuka CLI: import it, nothing to add (https://asuka.egoist.dev/docs/stdlib)", a)
	}
	if k, s, ok := strings.Cut(a, "="); ok {
		return k, s, nil
	}
	for _, p := range []string{"github:", "file:"} {
		if strings.HasPrefix(a, p) {
			name := strings.TrimPrefix(a, p)
			name = strings.TrimSuffix(filepath.Base(strings.SplitN(name, "#", 2)[0]), ".git")
			return name, a, nil
		}
	}
	name, rng := a, ""
	if at := strings.LastIndexByte(a, '@'); at > 0 {
		name, rng = a[:at], a[at+1:]
	}
	if rng != "" {
		if _, err := pkgs.ParseRange(rng); err != nil {
			return "", "", err
		}
		return name, rng, nil
	}
	doc, err := c.Packument(name)
	if err != nil {
		return "", "", err
	}
	latest := doc.DistTags["latest"]
	if latest == "" {
		return "", "", fmt.Errorf("%s has no latest version", name)
	}
	return name, "^" + latest, nil
}

func cmdRemove(args []string) error {
	var dir string
	fs := newFlagSet("remove", "<package>...")
	fs.about = "Removes dependencies from the project's asuka.toml and asuka.lock."
	fs.String(&dir, "dir", 'C', ".", "the project's directory")
	if err := fs.parse(args); err != nil {
		return err
	}
	m, err := pkgs.ReadManifest(dir)
	if err != nil {
		return err
	}
	for _, name := range fs.Args() {
		if _, ok := m.Dependencies[name]; !ok {
			return fmt.Errorf("remove: %s isn't a dependency (asuka.toml's [dependencies])", name)
		}
		if err := pkgs.SetDependency(dir, name, ""); err != nil {
			return err
		}
	}
	return install(pkgs.NewClient(pkgs.LoadNPMRC(dir)), dir, false)
}

func cmdInstall(args []string) error {
	var dir string
	var update bool
	fs := newFlagSet("install", "[dir]")
	fs.about = "Fetches the dependencies asuka.lock names into the cache (resolving asuka.toml's ranges\n" +
		"when asuka.lock doesn't answer them, or with -update)."
	fs.Bool(&update, "update", 'u', "resolve the versions again from asuka.toml's ranges")
	if err := fs.parse(args); err != nil {
		return err
	}
	dir = "."
	if len(fs.Args()) > 0 {
		dir = fs.Args()[0]
	}
	return install(pkgs.NewClient(pkgs.LoadNPMRC(dir)), dir, update)
}

func install(c *pkgs.Client, dir string, update bool) error {
	l, err := pkgs.Install(c, dir, update)
	if err != nil {
		return err
	}
	if len(l.Packages) > 0 {
		fmt.Printf("%d packages ready (asuka.lock)\n", len(l.Packages))
	}
	return nil
}

func cmdPkg(args []string) error {
	if len(args) == 0 {
		return errors.New("pkg: pack or publish")
	}
	switch args[0] {
	case "pack":
		return cmdPkgPack(args[1:])
	case "publish":
		return cmdPkgPublish(args[1:])
	}
	return fmt.Errorf("pkg: no command %q (pack, publish)", args[0])
}

func packDir(dir string) ([]byte, map[string]any, error) {
	m, err := pkgs.ReadManifest(dir)
	if err != nil {
		return nil, nil, err
	}
	if m.Name == "" || m.Version == "" {
		return nil, nil, errors.New("a package's asuka.toml names it and its version (name = \"@you/pkg\", version = \"1.0.0\")")
	}
	if err := pkgs.CheckName(m.Name); err != nil {
		return nil, nil, fmt.Errorf("asuka.toml's name: %v", err)
	}
	return pkgs.Pack(dir, pkgs.Package{Name: m.Name, Version: m.Version, Description: m.Description, License: m.License, Repository: m.Repository, Dependencies: m.Dependencies})
}

func cmdPkgPack(args []string) error {
	var out string
	fs := newFlagSet("pkg pack", "[dir]")
	fs.about = "Packs a package as asuka pkg publish would: <name>-<version>.tgz, and its package.json\n" +
		"and files in a directory beside it (for npm publish --provenance on CI)."
	fs.String(&out, "out", 'o', "dist", "where to write them")
	if err := fs.parse(args); err != nil {
		return err
	}
	dir := "."
	if len(fs.Args()) > 0 {
		dir = fs.Args()[0]
	}
	tgz, pj, err := packDir(dir)
	if err != nil {
		return err
	}
	name := strings.ReplaceAll(strings.TrimPrefix(pj["name"].(string), "@"), "/", "-") + "-" + pj["version"].(string)
	if err := os.MkdirAll(out, 0o755); err != nil {
		return err
	}
	p := filepath.Join(out, name+".tgz")
	if err := os.WriteFile(p, tgz, 0o644); err != nil {
		return err
	}
	if err := pkgs.Extract(tgz, filepath.Join(out, name)); err != nil {
		return err
	}
	fmt.Printf("%s (%d bytes) and %s\n", p, len(tgz), filepath.Join(out, name))
	return nil
}

func cmdPkgPublish(args []string) error {
	var tag, otp string
	var dry bool
	fs := newFlagSet("pkg publish", "[dir]")
	fs.about = "Publishes a package to its registry (npm's, or the one .npmrc names for its scope), with\n" +
		"your npm login or NPM_TOKEN."
	fs.String(&tag, "tag", 't', "latest", "the dist-tag to publish under (next, beta, ...)")
	fs.String(&otp, "otp", 0, "", "a one-time password, for an account with two-factor authentication")
	fs.Bool(&dry, "dry-run", 'n', "pack and check, but don't upload")
	if err := fs.parse(args); err != nil {
		return err
	}
	dir := "."
	if len(fs.Args()) > 0 {
		dir = fs.Args()[0]
	}
	tgz, pj, err := packDir(dir)
	if err != nil {
		return err
	}
	if err := checkPackageAPI(dir, pj); err != nil {
		return err
	}
	if dry {
		fmt.Printf("%s@%s: %d bytes, not published (-dry-run)\n", pj["name"], pj["version"], len(tgz))
		return nil
	}
	c := pkgs.NewClient(pkgs.LoadNPMRC(dir))
	if err := c.Publish(pj, tgz, tag, otp); err != nil {
		return err
	}
	fmt.Printf("published %s@%s (%s)\n", pj["name"], pj["version"], tag)
	return nil
}
