package pkgs

import (
	"bytes"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"

	"github.com/egoist/asuka/internal/toml"
)

// ManifestFile is a project's and a package's manifest.
const ManifestFile = "asuka.toml"

// LockFile records the resolved dependencies.
const LockFile = "asuka.lock"

// Manifest is what the package manager reads of asuka.toml (the CLI
// reads the rest).
type Manifest struct {
	Name         string            `json:"name"`
	Version      string            `json:"version"`
	Description  string            `json:"description"`
	License      string            `json:"license"`
	Repository   string            `json:"repository"`
	Dependencies map[string]string `json:"dependencies"`
	// Bin are the project's programs ([bin.<name>]): command-line tools
	// in directories of their own, the table's key the executable's name.
	Bin map[string]BinTarget `json:"bin"`
}

// BinTarget is a program of a project's: [bin.<name>] path = "cli", or
// one another toolchain builds: build = ["go", "build", "-o", "{out}", "."].
type BinTarget struct {
	// Path is its package's directory, relative to the project's.
	Path string `json:"path"`
	// Build is the command that builds it, run in the project's directory,
	// with {out}, {os} and {arch} in its arguments and Env's values.
	Build []string `json:"build"`
	// Env is what Build runs with over the CLI's environment.
	Env map[string]string `json:"env"`
}

// ReadManifest reads dir's asuka.toml (an empty manifest when there's
// none).
func ReadManifest(dir string) (*Manifest, error) {
	m := &Manifest{}
	b, err := os.ReadFile(filepath.Join(dir, ManifestFile))
	if errors.Is(err, os.ErrNotExist) {
		return m, nil
	}
	if err != nil {
		return nil, err
	}
	if err := toml.Unmarshal(b, m); err != nil {
		return nil, fmt.Errorf("%s:%v", ManifestFile, err)
	}
	return m, nil
}

// CheckName says why a package's or a dependency's name can't be one: std/
// and platform/ are the standard library's and the platform SDK's (the
// CLI carries them, an import of either never reaches a dependency), and
// asuka/ was theirs.
func CheckName(name string) error {
	for _, p := range []string{"std/", "platform/", "asuka/"} {
		if strings.HasPrefix(name, p) {
			return fmt.Errorf("%s: %s is the standard library's and the platform SDK's namespace (std/<name>, platform/<name>), not a package's: name it @you/%s", name, p, strings.TrimPrefix(name, p))
		}
	}
	return nil
}

var tableHeader = regexp.MustCompile(`^\s*\[`)

// SetDependency writes key = spec in the [dependencies] table of dir's
// asuka.toml, keeping the rest of the file as it is (its comments and
// order); spec "" removes the dependency.
func SetDependency(dir, key, spec string) error {
	p := filepath.Join(dir, ManifestFile)
	b, err := os.ReadFile(p)
	if err != nil && !errors.Is(err, os.ErrNotExist) {
		return err
	}
	out := setDependency(b, key, spec)
	if err := toml.Unmarshal(out, &Manifest{}); err != nil {
		return fmt.Errorf("editing %s: %v", ManifestFile, err)
	}
	return os.WriteFile(p, out, 0o644)
}

func quoteKey(k string) string {
	if regexp.MustCompile(`^[A-Za-z0-9_-]+$`).MatchString(k) {
		return k
	}
	return strconv.Quote(k)
}

func setDependency(b []byte, key, spec string) []byte {
	lines := strings.Split(string(b), "\n")
	start, end := -1, len(lines)
	for i, l := range lines {
		t := strings.TrimSpace(l)
		if t == "[dependencies]" {
			start = i
			continue
		}
		if start >= 0 && i > start && tableHeader.MatchString(l) {
			end = i
			break
		}
	}
	line := quoteKey(key) + " = " + strconv.Quote(spec)
	if start < 0 {
		if spec == "" {
			return b
		}
		s := strings.TrimRight(string(b), "\n")
		if s != "" {
			s += "\n\n"
		}
		return []byte(s + "[dependencies]\n" + line + "\n")
	}
	// The table's entries: replace or remove the key's, else add it,
	// keeping them sorted when they were.
	for i := start + 1; i < end; i++ {
		t := strings.TrimSpace(lines[i])
		if t == "" || strings.HasPrefix(t, "#") {
			continue
		}
		k, _, ok := strings.Cut(t, "=")
		if !ok {
			continue
		}
		k = strings.TrimSpace(k)
		if uq, err := strconv.Unquote(k); err == nil {
			k = uq
		}
		if k == key {
			if spec == "" {
				lines = append(lines[:i], lines[i+1:]...)
			} else {
				lines[i] = line
			}
			return []byte(strings.Join(lines, "\n"))
		}
	}
	if spec == "" {
		return b
	}
	// After the table's last entry.
	at := end
	for at > start+1 && strings.TrimSpace(lines[at-1]) == "" {
		at--
	}
	lines = append(lines[:at], append([]string{line}, lines[at:]...)...)
	return []byte(strings.Join(lines, "\n"))
}

// ReadLock reads dir's asuka.lock (nil when there's none).
func ReadLock(dir string) (*Lock, error) {
	b, err := os.ReadFile(filepath.Join(dir, LockFile))
	if errors.Is(err, os.ErrNotExist) {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	var l Lock
	if err := toml.Unmarshal(b, &l); err != nil {
		return nil, fmt.Errorf("%s:%v", LockFile, err)
	}
	return &l, nil
}

// WriteLock writes dir's asuka.lock.
func WriteLock(dir string, l *Lock) error {
	sort.Slice(l.Packages, func(i, j int) bool { return l.Packages[i].ID < l.Packages[j].ID })
	b, err := toml.Marshal(l)
	if err != nil {
		return err
	}
	var buf bytes.Buffer
	buf.WriteString("# Written by asuka (asuka add, asuka install): the versions of the app's\n# dependencies, and their hashes. Commit it; don't edit it.\n\n")
	buf.Write(b)
	return os.WriteFile(filepath.Join(dir, LockFile), buf.Bytes(), 0o644)
}

// Current reports whether l still answers m's dependencies.
func (l *Lock) Current(m *Manifest) bool {
	if l == nil || len(l.Root) != len(m.Dependencies) {
		return false
	}
	for k, spec := range m.Dependencies {
		id, ok := l.Root[k]
		if !ok {
			return false
		}
		sp, err := ParseSpec(k, spec)
		if err != nil {
			return false
		}
		p := l.Find(id)
		if p == nil {
			return false
		}
		switch sp.Kind {
		case "registry":
			v, err := ParseVersion(p.Version)
			if err != nil || !sp.Range.Match(v) || p.Name != sp.Name {
				return false
			}
		case "github":
			if id != "github:"+sp.Repo+"#"+sp.Ref {
				return false
			}
		}
	}
	return true
}

// Install makes dir's dependencies ready: resolved (again, when update,
// or when asuka.lock doesn't answer asuka.toml any more), recorded in
// asuka.lock, and fetched into the cache. It returns the lock.
func Install(c *Client, dir string, update bool) (*Lock, error) {
	m, err := ReadManifest(dir)
	if err != nil {
		return nil, err
	}
	l, err := ReadLock(dir)
	if err != nil {
		return nil, err
	}
	if update || !l.Current(m) {
		l, err = Resolve(c, m.Dependencies, dir)
		if err != nil {
			return nil, err
		}
	}
	for i := range l.Packages {
		// A GitHub package's hash is learned on its first fetch.
		if _, err := Fetch(c, &l.Packages[i]); err != nil {
			return nil, err
		}
	}
	if len(m.Dependencies) > 0 || l.Packages != nil {
		if err := WriteLock(dir, l); err != nil {
			return nil, err
		}
	}
	return l, nil
}

// Dirs maps every package of the lock to its directory, fetching what
// isn't in the cache; a file: package's is resolved from the project's
// directory.
func Dirs(c *Client, l *Lock, projectDir string) (map[string]string, error) {
	out := map[string]string{}
	for i := range l.Packages {
		d, err := Fetch(c, &l.Packages[i])
		if err != nil {
			return nil, err
		}
		if !filepath.IsAbs(d) {
			d = filepath.Join(projectDir, d)
		}
		out[l.Packages[i].ID] = d
	}
	return out, nil
}
