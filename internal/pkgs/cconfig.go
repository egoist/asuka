package pkgs

import (
	"encoding/json"
	"errors"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"regexp"
	"slices"
	"sort"
	"strings"

	"github.com/egoist/asuka/internal/toml"
)

// CConfig is asuka.toml's [c] (docs/asuka-toml.md): the C an app's or a
// package's extern declarations call (RFC 0008, docs/c.md), compiled
// into the core's build for every target, and what it links. Each
// platform's table adds to the top's.
type CConfig struct {
	CSettings
	IOS     CSettings `json:"ios"`
	Android CSettings `json:"android"`
	MacOS   CSettings `json:"macos"`
	Windows CSettings `json:"windows"`
	Linux   CSettings `json:"linux"`
	Web     CSettings `json:"web"`
	// Prebuilt are libraries a vendor ships built ([c.prebuilt]), by
	// target (CTargets): each a static library's path, or several,
	// relative to the project, linked for that target only.
	Prebuilt map[string]Paths `json:"prebuilt"`
	// Download are archives its paths name as $<name>/… ([c.download.
	// <name>]): fetched when a build needs them, into the cache.
	Download map[string]CDownload `json:"download"`
}

// Paths is a path, or a list of them.
type Paths []string

// UnmarshalJSON reads a string or a list of strings.
func (p *Paths) UnmarshalJSON(b []byte) error {
	var one string
	if err := json.Unmarshal(b, &one); err == nil {
		*p = Paths{one}
		return nil
	}
	var many []string
	if err := json.Unmarshal(b, &many); err != nil {
		return errors.New("a path, or a list of paths")
	}
	*p = many
	return nil
}

// CTargets are [c.prebuilt]'s keys: a target, or a platform whose one
// library is universal (a Mac's arm64 and x86_64; the simulator's), used
// for a target of its own that has no key.
var CTargets = []string{
	"ios-arm64", "ios-simulator-arm64", "ios-simulator-x64", "ios-simulator",
	"macos-arm64", "macos-x64", "macos",
	"android-arm64", "android-arm", "android-x64",
	"linux-x64", "linux-arm64",
	"windows-x64", "windows-arm64",
	"web",
}

// staticLib reports whether p names a static library (an archive).
func staticLib(p string) bool {
	switch strings.ToLower(filepath.Ext(p)) {
	case ".a", ".lib":
		return true
	}
	return false
}

// PrebuiltFor are [c.prebuilt]'s libraries for target (CTargets: its own
// key, else its platform's, universal), as paths in dir. None when the
// table is empty; an error when it has none for target, which the app
// can't then be built for.
func (c *CConfig) PrebuiltFor(dir, target string) ([]string, error) {
	if len(c.Prebuilt) == 0 {
		return nil, nil
	}
	ps, ok := c.Prebuilt[target]
	if !ok {
		if i := strings.LastIndexByte(target, '-'); i > 0 {
			ps, ok = c.Prebuilt[target[:i]]
		}
	}
	if !ok {
		return nil, fmt.Errorf("[c.prebuilt] has no library for %s", target)
	}
	var out []string
	for _, p := range ps {
		abs, d, err := c.Path(dir, p)
		if err != nil {
			return nil, fmt.Errorf("[c.prebuilt] %s: %v", target, err)
		}
		if d != nil {
			if err := d.Fetch(); err != nil {
				return nil, err
			}
		}
		if _, err := os.Stat(abs); err != nil {
			return nil, fmt.Errorf("[c.prebuilt] %s: %v", target, err)
		}
		out = append(out, abs)
	}
	return out, nil
}

// CSettings are a [c] table's keys.
type CSettings struct {
	// Sources are the C files, globs relative to the project (c/*.c;
	// ** matches any directories).
	Sources []string `json:"sources"`
	// Include are directories the sources (and only they) include from.
	Include []string `json:"include"`
	// Defines are macros, NAME or NAME=value.
	Defines []string `json:"defines"`
	// Flags are more of the compiler's flags (-Wno-…, -std=c99).
	Flags []string `json:"flags"`
	// Libraries are linked where the platform has them: sqlite3 is
	// -lsqlite3; a path (lib/libfoo.a, relative to the project) is that
	// file.
	Libraries []string `json:"libraries"`
	// Frameworks are Apple's (iOS, macOS): Security, CoreFoundation.
	Frameworks []string `json:"frameworks"`
}

// CPlatforms are the [c.<platform>] tables' names.
var CPlatforms = []string{"ios", "android", "macos", "windows", "linux", "web"}

// For is the settings for platform (CPlatforms), the top table's then
// the platform's.
func (c *CConfig) For(platform string) CSettings {
	var p CSettings
	switch platform {
	case "ios":
		p = c.IOS
	case "android":
		p = c.Android
	case "macos":
		p = c.MacOS
	case "windows":
		p = c.Windows
	case "linux":
		p = c.Linux
	case "web":
		p = c.Web
	}
	cat := func(a, b []string) []string { return append(append([]string{}, a...), b...) }
	return CSettings{
		Sources:    cat(c.Sources, p.Sources),
		Include:    cat(c.Include, p.Include),
		Defines:    cat(c.Defines, p.Defines),
		Flags:      cat(c.Flags, p.Flags),
		Libraries:  cat(c.Libraries, p.Libraries),
		Frameworks: cat(c.Frameworks, p.Frameworks),
	}
}

// Empty reports whether c says nothing.
func (c *CConfig) Empty() bool {
	if len(c.Prebuilt) > 0 {
		return false
	}
	for _, p := range append([]string{""}, CPlatforms...) {
		s := c.For(p)
		if len(s.Sources)+len(s.Include)+len(s.Defines)+len(s.Flags)+len(s.Libraries)+len(s.Frameworks) > 0 {
			return false
		}
	}
	return true
}

var (
	cDefine = regexp.MustCompile(`^[A-Za-z_][A-Za-z0-9_]*(=.*)?$`)
	cLib    = regexp.MustCompile(`^[A-Za-z0-9_.+-]+$`)
)

// Check reports what's wrong in c: a define that isn't one, a library
// or framework name that isn't.
func (c *CConfig) Check() error {
	tables := map[string]CSettings{"c": c.CSettings, "c.ios": c.IOS, "c.android": c.Android, "c.macos": c.MacOS, "c.windows": c.Windows, "c.linux": c.Linux, "c.web": c.Web}
	names := make([]string, 0, len(tables))
	for n := range tables {
		names = append(names, n)
	}
	sort.Strings(names)
	for _, n := range names {
		s := tables[n]
		for _, d := range s.Defines {
			if !cDefine.MatchString(d) {
				return fmt.Errorf("[%s] defines: %q isn't a macro (NAME or NAME=value)", n, d)
			}
		}
		for _, l := range s.Libraries {
			if !cLib.MatchString(l) && !strings.ContainsAny(l, `/\`) {
				return fmt.Errorf("[%s] libraries: %q is neither a library's name (sqlite3) nor a file's path", n, l)
			}
		}
		for _, f := range s.Frameworks {
			if !cLib.MatchString(f) {
				return fmt.Errorf("[%s] frameworks: %q isn't a framework's name", n, f)
			}
		}
		for _, p := range append(append([]string{}, s.Sources...), s.Include...) {
			if err := c.checkPath(p); err != nil {
				return fmt.Errorf("[%s]: %v", n, err)
			}
		}
		for _, l := range s.Libraries {
			if strings.HasPrefix(l, "$") {
				if err := c.checkPath(l); err != nil {
					return fmt.Errorf("[%s] libraries: %v", n, err)
				}
			}
		}
	}
	dnames := make([]string, 0, len(c.Download))
	for k := range c.Download {
		dnames = append(dnames, k)
	}
	sort.Strings(dnames)
	for _, k := range dnames {
		if err := c.Download[k].check(k); err != nil {
			return err
		}
	}
	keys := make([]string, 0, len(c.Prebuilt))
	for k := range c.Prebuilt {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	for _, k := range keys {
		if !slices.Contains(CTargets, k) {
			return fmt.Errorf("[c.prebuilt]: %q isn't a target (%s)", k, strings.Join(CTargets, ", "))
		}
		if len(c.Prebuilt[k]) == 0 {
			return fmt.Errorf("[c.prebuilt] %s: no library", k)
		}
		for _, p := range c.Prebuilt[k] {
			if err := c.checkPath(p); err != nil {
				return fmt.Errorf("[c.prebuilt] %s: %v", k, err)
			}
			if !staticLib(p) {
				return fmt.Errorf("[c.prebuilt] %s: %q isn't a static library (.a, or .lib on Windows): a dynamic one ships in a native package's Swift package or Gradle module (https://asuka.egoist.dev/docs/c#prebuilt-libraries)", k, p)
			}
		}
	}
	return nil
}

// checkPath reports what's wrong with p, a path in [c]: one outside the
// project, or in a download there's none of ($<name>/…).
func (c *CConfig) checkPath(p string) error {
	rest := p
	if strings.HasPrefix(p, "$") {
		name, r, _ := strings.Cut(p[1:], "/")
		if _, ok := c.Download[name]; !ok {
			return fmt.Errorf("%q: there's no [c.download.%s]", p, name)
		}
		if r == "" {
			return nil
		}
		rest = r
	}
	if filepath.IsAbs(rest) || strings.HasPrefix(filepath.ToSlash(filepath.Clean(rest)), "../") || filepath.Clean(rest) == ".." {
		if rest != p {
			return fmt.Errorf("%q is outside its download", p)
		}
		return fmt.Errorf("%q is outside the project (paths are relative to asuka.toml's directory, inside it)", p)
	}
	return nil
}

// ReadC reads [c] from dir's asuka.toml (nothing when there's none).
func ReadC(dir string) (*CConfig, error) {
	var m struct {
		C CConfig `json:"c"`
	}
	b, err := os.ReadFile(filepath.Join(dir, ManifestFile))
	if errors.Is(err, os.ErrNotExist) {
		return &m.C, nil
	}
	if err != nil {
		return nil, err
	}
	if err := toml.Unmarshal(b, &m); err != nil {
		return nil, fmt.Errorf("%s:%v", filepath.Join(dir, ManifestFile), err)
	}
	if err := m.C.Check(); err != nil {
		return nil, fmt.Errorf("%s: %v", filepath.Join(dir, ManifestFile), err)
	}
	return &m.C, nil
}

// CSources are the files s's source globs (c's s: CConfig.For) name in
// dir, sorted, each once. A glob that names none is an error: a typo, or
// a file missing. A file named in a download that isn't fetched yet is
// its path there, without fetching it (a build fetches it before it
// compiles: CConfig.FetchFor); a glob in one fetches it.
func (c *CConfig) CSources(dir string, s CSettings) ([]string, error) {
	seen := map[string]bool{}
	var out []string
	for _, g := range s.Sources {
		base, pattern := dir, filepath.FromSlash(g)
		if strings.HasPrefix(g, "$") {
			p, d, err := c.Path(dir, g)
			if err != nil {
				return nil, fmt.Errorf("[c] sources: %v", err)
			}
			if !strings.ContainsAny(g, "*?[") && !d.Fetched() {
				if !seen[p] {
					seen[p] = true
					out = append(out, p)
				}
				continue
			}
			if err := d.Fetch(); err != nil {
				return nil, err
			}
			base, pattern = d.Dir(), strings.TrimPrefix(p, d.Dir()+string(filepath.Separator))
		}
		ms, err := globAll(base, pattern)
		if err != nil {
			return nil, fmt.Errorf("[c] sources %q: %v", g, err)
		}
		if len(ms) == 0 {
			return nil, fmt.Errorf("[c] sources %q: no file in %s matches it", g, base)
		}
		for _, m := range ms {
			if !seen[m] {
				seen[m] = true
				out = append(out, m)
			}
		}
	}
	sort.Strings(out)
	return out, nil
}

// globAll is filepath.Glob in dir, where ** matches any directories
// (none included).
func globAll(dir, pattern string) ([]string, error) {
	if !strings.Contains(pattern, "**") {
		return filepath.Glob(filepath.Join(dir, pattern))
	}
	re, err := globRegexp(filepath.ToSlash(pattern))
	if err != nil {
		return nil, err
	}
	var out []string
	err = filepath.WalkDir(dir, func(p string, e fs.DirEntry, err error) error {
		if err != nil {
			return nil
		}
		if e.IsDir() {
			if p != dir && (strings.HasPrefix(e.Name(), ".") || e.Name() == "node_modules") {
				return filepath.SkipDir
			}
			return nil
		}
		rel, _ := filepath.Rel(dir, p)
		if re.MatchString(filepath.ToSlash(rel)) {
			out = append(out, p)
		}
		return nil
	})
	return out, err
}

// globRegexp turns a glob with ** into a regexp over slash paths.
func globRegexp(g string) (*regexp.Regexp, error) {
	var b strings.Builder
	b.WriteString("^")
	for i := 0; i < len(g); i++ {
		switch c := g[i]; {
		case strings.HasPrefix(g[i:], "**/"):
			b.WriteString("(?:.*/)?")
			i += 2
		case strings.HasPrefix(g[i:], "**"):
			b.WriteString(".*")
			i++
		case c == '*':
			b.WriteString("[^/]*")
		case c == '?':
			b.WriteString("[^/]")
		default:
			b.WriteString(regexp.QuoteMeta(string(c)))
		}
	}
	b.WriteString("$")
	return regexp.Compile(b.String())
}
