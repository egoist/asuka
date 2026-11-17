package prebuilt

import (
	"archive/tar"
	"bufio"
	"bytes"
	"compress/gzip"
	"crypto/sha256"
	"crypto/sha512"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"path"
	"path/filepath"
	"regexp"
	"strings"
	"sync"
	"time"

	"github.com/egoist/asuka/internal/ccache"
)

// DefaultRegistry is npm's.
const DefaultRegistry = "https://registry.npmjs.org/"

// Dir is where a package is kept in the cache: prebuilt/<name>@<version>
// (the scope's @ and / left out).
func Dir(p Package) string {
	name := strings.ReplaceAll(strings.TrimPrefix(p.Name, "@"), "/", "-")
	return filepath.Join(ccache.Dir("prebuilt"), name+"@"+p.Version)
}

var (
	fetchMu    sync.Mutex
	fetchLocks = map[string]*sync.Mutex{}
)

// Fetch makes sure p is in the cache and returns its directory: unpacked
// from its tarball, fetched from the npm registry (Registry's) and checked
// against p.Integrity, which the CLI pins: a registry can't give another.
func Fetch(p Package) (string, error) {
	dir := Dir(p)
	fetchMu.Lock()
	mu := fetchLocks[dir]
	if mu == nil {
		mu = &sync.Mutex{}
		fetchLocks[dir] = mu
	}
	fetchMu.Unlock()
	mu.Lock()
	defer mu.Unlock()
	if _, err := os.Stat(filepath.Join(dir, ".complete")); err == nil {
		ccache.Touch(dir)
		ccache.Hit("prebuilt")
		return dir, nil
	}
	ccache.Miss("prebuilt")
	if p.Integrity == "" || p.Version == "" {
		return "", fmt.Errorf("prebuilt: %s has no pinned version and integrity", p.Name)
	}
	cfg := LoadConfig()
	reg := cfg.Registry(p.Name)
	fmt.Fprintf(os.Stderr, "asuka: fetching %s@%s (once) from %s\n", p.Name, p.Version, reg)
	tgz, err := cfg.download(p)
	if err != nil {
		return "", fmt.Errorf("prebuilt: %s@%s from %s: %w", p.Name, p.Version, reg, err)
	}
	tmp := ccache.TmpName(dir)
	if err := unpack(tgz, tmp); err != nil {
		os.RemoveAll(tmp)
		return "", fmt.Errorf("prebuilt: %s@%s: %v", p.Name, p.Version, err)
	}
	if err := os.WriteFile(filepath.Join(tmp, ".complete"), nil, 0o644); err != nil {
		os.RemoveAll(tmp)
		return "", err
	}
	os.RemoveAll(dir) // a half-written one (no .complete)
	if err := ccache.Commit(tmp, dir); err != nil {
		return "", err
	}
	return dir, nil
}

// Check fetches p's tarball as Fetch does (the registry's version
// document, then its tarball, checked against p.Integrity), keeping
// nothing: a release waits for it to pass before it publishes the CLI
// that pins p, since a registry accepts a package before it serves it.
func Check(p Package) error {
	_, err := LoadConfig().download(p)
	return err
}

// ErrIntegrity is a download that isn't the package the CLI pins.
var ErrIntegrity = errors.New("the package isn't the one this asuka was released with (its integrity differs)")

// download fetches p's tarball: its version's metadata from the
// registry, then dist.tarball, both checked against p.Integrity.
func (c Config) download(p Package) ([]byte, error) {
	reg := c.Registry(p.Name)
	meta, err := c.get(reg, strings.TrimSuffix(reg, "/")+"/"+escapeName(p.Name)+"/"+url.PathEscape(p.Version), "application/json")
	var dist struct {
		Dist struct {
			Tarball   string `json:"tarball"`
			Integrity string `json:"integrity"`
		} `json:"dist"`
	}
	if err == nil {
		err = json.Unmarshal(meta, &dist)
	}
	if err != nil || dist.Dist.Tarball == "" {
		// A registry without the version's own document: the package's.
		all, aerr := c.get(reg, strings.TrimSuffix(reg, "/")+"/"+escapeName(p.Name), "application/vnd.npm.install-v1+json; q=1.0, application/json; q=0.8")
		if aerr != nil {
			if err != nil {
				return nil, err
			}
			return nil, aerr
		}
		var doc struct {
			Versions map[string]json.RawMessage `json:"versions"`
		}
		if err := json.Unmarshal(all, &doc); err != nil {
			return nil, err
		}
		v, ok := doc.Versions[p.Version]
		if !ok {
			return nil, fmt.Errorf("the registry has no version %s", p.Version)
		}
		if err := json.Unmarshal(v, &dist); err != nil {
			return nil, err
		}
	}
	if dist.Dist.Integrity != "" && dist.Dist.Integrity != p.Integrity {
		return nil, ErrIntegrity
	}
	if dist.Dist.Tarball == "" {
		return nil, errors.New("the registry names no tarball")
	}
	tgz, err := c.get(reg, dist.Dist.Tarball, "application/octet-stream")
	if err != nil {
		return nil, err
	}
	if !CheckIntegrity(tgz, p.Integrity) {
		return nil, ErrIntegrity
	}
	return tgz, nil
}

// escapeName is a package's name in a registry's URL: @scope%2fname.
func escapeName(name string) string { return strings.Replace(name, "/", "%2f", 1) }

// Integrity is data's npm integrity (Subresource Integrity, SHA-512).
func Integrity(data []byte) string {
	s := sha512.Sum512(data)
	return "sha512-" + base64.StdEncoding.EncodeToString(s[:])
}

// CheckIntegrity reports whether data has integrity: its sha512 (the one
// npm publishes) among the space-separated hashes.
func CheckIntegrity(data []byte, integrity string) bool {
	want := Integrity(data)
	for _, h := range strings.Fields(integrity) {
		if h == want {
			return true
		}
	}
	return false
}

func (c Config) get(registry, u, accept string) ([]byte, error) {
	req, err := http.NewRequest("GET", u, nil)
	if err != nil {
		return nil, err
	}
	req.Header.Set("Accept", accept)
	req.Header.Set("User-Agent", "asuka-cli")
	if a := c.Auth(u); a != "" {
		req.Header.Set("Authorization", a)
	}
	client := &http.Client{Timeout: 10 * time.Minute}
	resp, err := client.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("GET %s: %s", u, resp.Status)
	}
	return io.ReadAll(io.LimitReader(resp.Body, 2<<30))
}

// unpack writes a package's tarball's files (under package/) into dir.
func unpack(tgz []byte, dir string) error {
	zr, err := gzip.NewReader(bytes.NewReader(tgz))
	if err != nil {
		return err
	}
	tr := tar.NewReader(zr)
	for {
		h, err := tr.Next()
		if err == io.EOF {
			return nil
		}
		if err != nil {
			return err
		}
		name := path.Clean(strings.TrimPrefix(filepath.ToSlash(h.Name), "./"))
		if name == ".." || strings.HasPrefix(name, "../") || path.IsAbs(name) {
			return fmt.Errorf("the tarball has a file outside it (%s)", h.Name)
		}
		// npm packs everything under package/ (some tools another top
		// directory): the first element goes.
		if i := strings.IndexByte(name, '/'); i >= 0 {
			name = name[i+1:]
		} else {
			continue
		}
		if name == "" || name == "." || strings.HasPrefix(name, "../") || name == ".." || path.IsAbs(name) || strings.Contains(name, ":") {
			return fmt.Errorf("the tarball has a file outside it (%s)", h.Name)
		}
		p := filepath.Join(dir, filepath.FromSlash(name))
		switch h.Typeflag {
		case tar.TypeDir:
			if err := os.MkdirAll(p, 0o755); err != nil {
				return err
			}
		case tar.TypeReg:
			if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
				return err
			}
			mode := os.FileMode(0o644)
			if h.Mode&0o111 != 0 {
				mode = 0o755
			}
			f, err := os.OpenFile(p, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, mode)
			if err != nil {
				return err
			}
			_, cerr := io.Copy(f, io.LimitReader(tr, h.Size))
			if err := f.Close(); cerr == nil {
				cerr = err
			}
			if cerr != nil {
				return cerr
			}
		default:
			// Links and the like: a package of libraries has none.
		}
	}
}

func sha256File(p string) (string, error) {
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

// ---- The npm configuration ----

// Config is the npm configuration the CLI reads to reach a registry:
// npm_config_registry (NPM_CONFIG_REGISTRY), then the project's .npmrc
// (beside the package.json or asuka.toml nearest above the working
// directory), then the user's
// (~/.npmrc, or NPM_CONFIG_USERCONFIG): registry=, @scope:registry=, and
// //host/path/:_authToken= (or :_auth=, user:password in base64) for a
// private registry or mirror, ${VAR} expanded from the environment.
type Config struct {
	values map[string]string
}

// LoadConfig reads the configuration.
func LoadConfig() Config {
	c := Config{values: map[string]string{}}
	var files []string
	user := os.Getenv("NPM_CONFIG_USERCONFIG")
	if user == "" {
		user = os.Getenv("npm_config_userconfig")
	}
	if user == "" {
		if home, err := os.UserHomeDir(); err == nil {
			user = filepath.Join(home, ".npmrc")
		}
	}
	if user != "" {
		files = append(files, user)
	}
	if wd, err := os.Getwd(); err == nil {
		// The project's: beside the nearest package.json or asuka.toml
		// above the working directory, as npm's is beside its project's
		// package.json.
		for d := wd; ; d = filepath.Dir(d) {
			if exists(filepath.Join(d, "package.json")) || exists(filepath.Join(d, "asuka.toml")) {
				if p := filepath.Join(d, ".npmrc"); exists(p) && p != user {
					files = append(files, p)
				}
				break
			}
			if filepath.Dir(d) == d {
				break
			}
		}
	}
	for _, f := range files {
		if b, err := os.ReadFile(f); err == nil {
			for k, v := range parseNpmrc(b) {
				c.values[k] = v // the project's over the user's
			}
		}
	}
	for _, k := range []string{"NPM_CONFIG_REGISTRY", "npm_config_registry"} {
		if v := os.Getenv(k); v != "" {
			c.values["registry"] = v
		}
	}
	return c
}

func exists(p string) bool { _, err := os.Stat(p); return err == nil }

var envRef = regexp.MustCompile(`\$\{([^}]+)\}`)

// parseNpmrc reads an ini file's key = value lines (npm's: no sections).
func parseNpmrc(b []byte) map[string]string {
	out := map[string]string{}
	sc := bufio.NewScanner(bytes.NewReader(b))
	for sc.Scan() {
		l := strings.TrimSpace(sc.Text())
		if l == "" || l[0] == '#' || l[0] == ';' {
			continue
		}
		k, v, ok := strings.Cut(l, "=")
		if !ok {
			continue
		}
		k, v = strings.TrimSpace(k), strings.TrimSpace(v)
		if len(v) >= 2 && (v[0] == '"' && v[len(v)-1] == '"' || v[0] == '\'' && v[len(v)-1] == '\'') {
			v = v[1 : len(v)-1]
		}
		v = envRef.ReplaceAllStringFunc(v, func(m string) string { return os.Getenv(m[2 : len(m)-1]) })
		out[k] = v
	}
	return out
}

// Registry is the registry a package is fetched from: its scope's
// (@scope:registry), else the registry, else npm's; ending in /.
func (c Config) Registry(name string) string {
	r := ""
	if scope, _, ok := strings.Cut(name, "/"); ok && strings.HasPrefix(scope, "@") {
		r = c.values[scope+":registry"]
	}
	if r == "" {
		r = c.values["registry"]
	}
	if r == "" {
		r = DefaultRegistry
	}
	if !strings.HasSuffix(r, "/") {
		r += "/"
	}
	return r
}

// Auth is the Authorization header for u: the credentials of the
// longest //host/path/ of the configuration's that u is under (npm's
// "nerf dart"), or "".
func (c Config) Auth(u string) string {
	pu, err := url.Parse(u)
	if err != nil {
		return ""
	}
	target := "//" + pu.Host + pu.Path
	best, auth := -1, ""
	for k, v := range c.values {
		var prefix, kind string
		for _, s := range []string{":_authToken", ":_auth"} {
			if strings.HasSuffix(k, s) && strings.HasPrefix(k, "//") {
				prefix, kind = strings.TrimSuffix(k, s), s
			}
		}
		if prefix == "" || v == "" {
			continue
		}
		if !strings.HasSuffix(prefix, "/") {
			prefix += "/"
		}
		if !strings.HasPrefix(target, prefix) && target+"/" != prefix {
			continue
		}
		if len(prefix) > best {
			best = len(prefix)
			if kind == ":_authToken" {
				auth = "Bearer " + v
			} else {
				auth = "Basic " + v
			}
		}
	}
	if auth == "" {
		// The configuration's own _authToken, for its registry.
		if t := c.values["_authToken"]; t != "" {
			if reg, err := url.Parse(c.values["registry"]); err == nil && reg.Host == pu.Host {
				auth = "Bearer " + t
			}
		}
	}
	return auth
}
