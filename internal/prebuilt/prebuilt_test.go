package prebuilt

import (
	"archive/tar"
	"bytes"
	"compress/gzip"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync/atomic"
	"testing"
)

const pkgName = "@egoist/asuka-prebuilt-test-arm64"

// tarball is an npm package's: its files under package/.
func tarball(t *testing.T, files map[string]string) []byte {
	t.Helper()
	var b bytes.Buffer
	gz := gzip.NewWriter(&b)
	tw := tar.NewWriter(gz)
	for name, data := range files {
		if err := tw.WriteHeader(&tar.Header{Name: "package/" + name, Mode: 0o644, Size: int64(len(data)), Typeflag: tar.TypeReg}); err != nil {
			t.Fatal(err)
		}
		tw.Write([]byte(data))
	}
	tw.Close()
	gz.Close()
	return b.Bytes()
}

// registry is a fake npm registry with one package: its version's
// document and its tarball, served to requests with token's
// Authorization (any, with token "").
type registry struct {
	*httptest.Server
	tgz        []byte
	integrity  string // what the version's document says
	token      string
	versionDoc bool // serves /<name>/<version>; else only /<name>
	requests   atomic.Int32
	// unserved: the version listed, its tarball not served yet (404), as
	// npm's registry does for a while after a publish.
	unserved atomic.Bool
}

func newRegistry(t *testing.T, tgz []byte) *registry {
	r := &registry{tgz: tgz, integrity: Integrity(tgz), versionDoc: true}
	r.Server = httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, req *http.Request) {
		r.requests.Add(1)
		if r.token != "" && req.Header.Get("Authorization") != "Bearer "+r.token {
			http.Error(w, "unauthorized", http.StatusUnauthorized)
			return
		}
		dist := map[string]any{"dist": map[string]string{"tarball": r.URL + "/tarballs/x.tgz", "integrity": r.integrity}}
		switch req.URL.Path {
		case "/" + pkgName + "/1.0.0":
			if !r.versionDoc {
				http.NotFound(w, req)
				return
			}
			json.NewEncoder(w).Encode(dist)
		case "/" + pkgName:
			json.NewEncoder(w).Encode(map[string]any{"versions": map[string]any{"1.0.0": dist}})
		case "/tarballs/x.tgz":
			if r.unserved.Load() {
				http.NotFound(w, req)
				return
			}
			w.Write(r.tgz)
		default:
			http.NotFound(w, req)
		}
	}))
	t.Cleanup(r.Close)
	return r
}

// setup writes a manifest naming the package and its library (integrity
// pinned, sha256 the library's), points the CLI at it, and returns the
// library's bytes.
func setup(t *testing.T, integrity, sha string) {
	t.Helper()
	t.Setenv("ASUKA_CACHE", t.TempDir())
	m := Manifest{
		Version:  "1.0.0",
		Packages: map[string]Package{"test-arm64": {Name: pkgName, Version: "1.0.0", Integrity: integrity, Compiler: "zig"}},
		Archives: map[string]File{ArchiveKey("test-arm64", "release", "core"): {Package: "test-arm64", Path: "lib/release/libcore.a", SHA256: sha}},
	}
	b, _ := json.Marshal(m)
	p := filepath.Join(t.TempDir(), "manifest.json")
	if err := os.WriteFile(p, b, 0o644); err != nil {
		t.Fatal(err)
	}
	t.Setenv("ASUKA_PREBUILT_MANIFEST", p)
	// No user's or project's .npmrc but the test's.
	t.Setenv("NPM_CONFIG_USERCONFIG", filepath.Join(t.TempDir(), "none"))
	t.Setenv("NPM_CONFIG_REGISTRY", "")
	t.Setenv("npm_config_registry", "")
	t.Chdir(t.TempDir())
	reset()
	verified = map[string]bool{}
	t.Cleanup(reset)
}

const lib = "!<arch>\nthe core's objects\n"

func libSHA() string {
	s := sha256.Sum256([]byte(lib))
	return hex.EncodeToString(s[:])
}

func TestFetchFromRegistry(t *testing.T) {
	tgz := tarball(t, map[string]string{"lib/release/libcore.a": lib, "package.json": "{}"})
	reg := newRegistry(t, tgz)
	setup(t, Integrity(tgz), libSHA())
	t.Setenv("NPM_CONFIG_REGISTRY", reg.URL)

	if c := Compiler("test-arm64"); c != "zig" {
		t.Fatalf("Compiler = %q", c)
	}
	p, ok, err := Archive("test-arm64", "release", "core")
	if err != nil || !ok {
		t.Fatalf("Archive: %v, %v", ok, err)
	}
	if b, _ := os.ReadFile(p); string(b) != lib {
		t.Fatalf("the library is %q", b)
	}
	n := reg.requests.Load()
	// Again: from the cache, not the registry.
	if q, ok, err := Archive("test-arm64", "release", "core"); err != nil || !ok || q != p {
		t.Fatalf("Archive again: %s, %v, %v", q, ok, err)
	}
	if reg.requests.Load() != n {
		t.Fatal("the second lookup went to the registry")
	}
	// What the manifest has no library for isn't fetched.
	if _, ok, err := Archive("test-arm64", "dev", "core"); ok || err != nil {
		t.Fatalf("a library the manifest hasn't: %v, %v", ok, err)
	}
}

func TestCheck(t *testing.T) {
	// A release publishes the CLI only once its packages are served: a
	// version the registry lists, its tarball not served yet, isn't.
	tgz := tarball(t, map[string]string{"lib/release/libcore.a": lib})
	reg := newRegistry(t, tgz)
	setup(t, Integrity(tgz), libSHA())
	t.Setenv("NPM_CONFIG_REGISTRY", reg.URL)
	p := Package{Name: pkgName, Version: "1.0.0", Integrity: Integrity(tgz)}
	reg.unserved.Store(true)
	if err := Check(p); err == nil || !strings.Contains(err.Error(), "404") {
		t.Fatalf("Check before the tarball is served = %v, want a 404", err)
	}
	reg.unserved.Store(false)
	if err := Check(p); err != nil {
		t.Fatalf("Check: %v", err)
	}
	other := p
	other.Integrity = Integrity([]byte("another tarball"))
	if err := Check(other); !errors.Is(err, ErrIntegrity) {
		t.Fatalf("Check of another integrity = %v, want ErrIntegrity", err)
	}
	// It keeps nothing: a build still fetches it.
	if _, err := os.Stat(Dir(p)); err == nil {
		t.Fatal("Check left the package in the cache")
	}
}

func TestFetchPackageDocument(t *testing.T) {
	// A registry (a mirror) without the version's own document: the
	// package's lists it.
	tgz := tarball(t, map[string]string{"lib/release/libcore.a": lib})
	reg := newRegistry(t, tgz)
	reg.versionDoc = false
	setup(t, Integrity(tgz), libSHA())
	t.Setenv("npm_config_registry", reg.URL+"/")
	if _, ok, err := Archive("test-arm64", "release", "core"); err != nil || !ok {
		t.Fatalf("Archive: %v, %v", ok, err)
	}
}

func TestIntegrityMismatchRefused(t *testing.T) {
	tgz := tarball(t, map[string]string{"lib/release/libcore.a": lib})
	other := tarball(t, map[string]string{"lib/release/libcore.a": "!<arch>\nsomething else\n"})

	// The registry serves another package than the CLI pins, and says so.
	reg := newRegistry(t, other)
	setup(t, Integrity(tgz), libSHA())
	t.Setenv("NPM_CONFIG_REGISTRY", reg.URL)
	if _, _, err := Archive("test-arm64", "release", "core"); !errors.Is(err, ErrIntegrity) {
		t.Fatalf("Archive = %v; want ErrIntegrity", err)
	}
	// It says the pinned integrity, but serves another tarball.
	reg.integrity = Integrity(tgz)
	if _, _, err := Archive("test-arm64", "release", "core"); !errors.Is(err, ErrIntegrity) {
		t.Fatalf("Archive = %v; want ErrIntegrity", err)
	}
	if _, err := os.Stat(Dir(Package{Name: pkgName, Version: "1.0.0"})); err == nil {
		t.Fatal("a refused package is in the cache")
	}
}

func TestArchiveHashMismatchRefused(t *testing.T) {
	tgz := tarball(t, map[string]string{"lib/release/libcore.a": lib})
	reg := newRegistry(t, tgz)
	setup(t, Integrity(tgz), strings.Repeat("0", 64))
	t.Setenv("NPM_CONFIG_REGISTRY", reg.URL)
	_, _, err := Archive("test-arm64", "release", "core")
	if err == nil || !strings.Contains(err.Error(), "SHA-256") {
		t.Fatalf("Archive = %v; want a SHA-256 mismatch", err)
	}
	if _, err := os.Stat(Dir(Package{Name: pkgName, Version: "1.0.0"})); err == nil {
		t.Fatal("a package whose library didn't match stayed in the cache")
	}
}

func TestScopedRegistryAndToken(t *testing.T) {
	tgz := tarball(t, map[string]string{"lib/release/libcore.a": lib})
	reg := newRegistry(t, tgz)
	reg.token = "s3cret"
	setup(t, Integrity(tgz), libSHA())
	// The registry the scope's packages come from, and its token, from
	// the environment as npm expands it, in the project's .npmrc (an
	// app's: beside its asuka.toml); the default registry is elsewhere.
	t.Setenv("PRIVATE_TOKEN", "s3cret")
	host := strings.TrimPrefix(reg.URL, "http:")
	proj := t.TempDir()
	os.WriteFile(filepath.Join(proj, "asuka.toml"), nil, 0o644)
	os.WriteFile(filepath.Join(proj, ".npmrc"), []byte("registry=http://127.0.0.1:9/\n@egoist:registry="+reg.URL+"/\n"+host+"/:_authToken=${PRIVATE_TOKEN}\n"), 0o644)
	sub := filepath.Join(proj, "src")
	os.MkdirAll(sub, 0o755)
	t.Chdir(sub)

	cfg := LoadConfig()
	if r := cfg.Registry(pkgName); r != reg.URL+"/" {
		t.Fatalf("Registry(%s) = %s", pkgName, r)
	}
	if r := cfg.Registry("left-pad"); r != "http://127.0.0.1:9/" {
		t.Fatalf("Registry(left-pad) = %s", r)
	}
	if a := cfg.Auth(reg.URL + "/tarballs/x.tgz"); a != "Bearer s3cret" {
		t.Fatalf("Auth = %q", a)
	}
	if a := cfg.Auth("https://registry.npmjs.org/x"); a != "" {
		t.Fatalf("another registry got the token: %q", a)
	}
	if _, ok, err := Archive("test-arm64", "release", "core"); err != nil || !ok {
		t.Fatalf("Archive: %v, %v", ok, err)
	}

	// The user's .npmrc, the project's over it.
	user := filepath.Join(t.TempDir(), ".npmrc")
	os.WriteFile(user, []byte("registry=https://user.example/\n//user.example/:_auth=dTpw\n"), 0o644)
	t.Setenv("NPM_CONFIG_USERCONFIG", user)
	t.Chdir(t.TempDir())
	cfg = LoadConfig()
	if r := cfg.Registry(pkgName); r != "https://user.example/" {
		t.Fatalf("the user's registry: %s", r)
	}
	if a := cfg.Auth("https://user.example/@egoist%2fx/-/x.tgz"); a != "Basic dTpw" {
		t.Fatalf("the user's _auth: %q", a)
	}
	// The environment's registry over both.
	t.Setenv("NPM_CONFIG_REGISTRY", "https://env.example")
	if r := LoadConfig().Registry(pkgName); r != "https://env.example/" {
		t.Fatalf("the environment's registry: %s", r)
	}
}

func TestUnpackRefusesEscapes(t *testing.T) {
	var b bytes.Buffer
	gz := gzip.NewWriter(&b)
	tw := tar.NewWriter(gz)
	tw.WriteHeader(&tar.Header{Name: "package/../../evil", Mode: 0o644, Size: 1, Typeflag: tar.TypeReg})
	tw.Write([]byte("x"))
	tw.Close()
	gz.Close()
	if err := unpack(b.Bytes(), t.TempDir()); err == nil {
		t.Fatal("a tarball's file outside it was written")
	}
}
