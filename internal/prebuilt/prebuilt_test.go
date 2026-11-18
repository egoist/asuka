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
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"sync/atomic"
	"testing"
)

const asset = "asuka-prebuilt-test-arm64-1.0.0.tar.gz"

// archive is a release's archive of files, packed as a release packs
// them.
func archive(t *testing.T, files map[string]string) []byte {
	t.Helper()
	src := t.TempDir()
	for name, data := range files {
		p := filepath.Join(src, filepath.FromSlash(name))
		os.MkdirAll(filepath.Dir(p), 0o755)
		if err := os.WriteFile(p, []byte(data), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	var b bytes.Buffer
	if err := Pack(&b, src, strings.TrimSuffix(asset, ".tar.gz")); err != nil {
		t.Fatal(err)
	}
	return b.Bytes()
}

func sum(b []byte) string {
	s := sha256.Sum256(b)
	return hex.EncodeToString(s[:])
}

// release is a fake GitHub release: its downloads redirect to a CDN, as
// GitHub's do, which serves the archive.
type release struct {
	*httptest.Server
	data     []byte
	token    string // the Authorization a request must carry ("": none)
	requests atomic.Int32
	failures atomic.Int32 // the next requests answered 503
}

func newRelease(t *testing.T, data []byte) *release {
	r := &release{data: data}
	r.Server = httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, req *http.Request) {
		r.requests.Add(1)
		if r.failures.Load() > 0 {
			r.failures.Add(-1)
			http.Error(w, "busy", http.StatusServiceUnavailable)
			return
		}
		if r.token != "" && req.Header.Get("Authorization") != r.token {
			http.Error(w, "unauthorized", http.StatusUnauthorized)
			return
		}
		switch req.URL.Path {
		case "/download/v1.0.0/" + asset:
			http.Redirect(w, req, "/cdn/"+asset, http.StatusFound)
		case "/cdn/" + asset:
			w.Write(r.data)
		default:
			http.NotFound(w, req)
		}
	}))
	t.Cleanup(r.Close)
	return r
}

// setup writes a manifest naming the archive (its SHA-256 pinned) and its
// library (sha its SHA-256), and points the CLI at it.
func setup(t *testing.T, archiveSHA, sha string) {
	t.Helper()
	t.Setenv("ASUKA_CACHE", t.TempDir())
	m := Manifest{
		Version:  "1.0.0",
		Packages: map[string]Package{"test-arm64": {Asset: asset, Version: "1.0.0", SHA256: archiveSHA, Compiler: "zig"}},
		Archives: map[string]File{ArchiveKey("test-arm64", "release", "core"): {Package: "test-arm64", Path: "lib/release/libcore.a", SHA256: sha}},
	}
	b, _ := json.Marshal(m)
	p := filepath.Join(t.TempDir(), "manifest.json")
	if err := os.WriteFile(p, b, 0o644); err != nil {
		t.Fatal(err)
	}
	t.Setenv("ASUKA_PREBUILT_MANIFEST", p)
	t.Setenv("ASUKA_PREBUILT_URL", "")
	t.Setenv("ASUKA_PREBUILT_TOKEN", "")
	retryWait = 0
	reset()
	verified = map[string]bool{}
	t.Cleanup(reset)
}

const lib = "!<arch>\nthe core's objects\n"

func TestFetchFromRelease(t *testing.T) {
	data := archive(t, map[string]string{"lib/release/libcore.a": lib, "prebuilt.json": "{}"})
	rel := newRelease(t, data)
	setup(t, sum(data), sum([]byte(lib)))
	t.Setenv("ASUKA_PREBUILT_URL", rel.URL+"/download/v{version}")

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
	n := rel.requests.Load()
	if n != 2 {
		t.Fatalf("%d requests, want 2 (the download and its redirect)", n)
	}
	// Again: from the cache, not the release.
	if q, ok, err := Archive("test-arm64", "release", "core"); err != nil || !ok || q != p {
		t.Fatalf("Archive again: %s, %v, %v", q, ok, err)
	}
	if rel.requests.Load() != n {
		t.Fatal("the second lookup went to the release")
	}
	// What the manifest has no library for isn't fetched.
	if _, ok, err := Archive("test-arm64", "dev", "core"); ok || err != nil {
		t.Fatalf("a library the manifest hasn't: %v, %v", ok, err)
	}
	// Nothing is left beside it but the package.
	es, _ := os.ReadDir(filepath.Dir(Dir(Package{Asset: asset})))
	if len(es) != 1 {
		t.Fatalf("the cache's prebuilt/ has %d entries, want 1", len(es))
	}
}

func TestSource(t *testing.T) {
	p := Package{Asset: asset, Version: "1.0.0"}
	t.Setenv("ASUKA_PREBUILT_URL", "")
	if s, local := Source(p); s != "https://github.com/egoist/asuka/releases/download/v1.0.0/"+asset || local {
		t.Fatalf("Source = %s, %v", s, local)
	}
	t.Setenv("ASUKA_PREBUILT_URL", "https://mirror.example/asuka/")
	if s, _ := Source(p); s != "https://mirror.example/asuka/"+asset {
		t.Fatalf("a mirror's: %s", s)
	}
	dir := t.TempDir()
	t.Setenv("ASUKA_PREBUILT_URL", dir)
	if s, local := Source(p); s != filepath.Join(dir, asset) || !local {
		t.Fatalf("a directory's: %s, %v", s, local)
	}
	t.Setenv("ASUKA_PREBUILT_URL", (&url.URL{Scheme: "file", Path: filepath.ToSlash(dir)}).String())
	if s, local := Source(p); filepath.Clean(s) != filepath.Join(dir, asset) || !local {
		t.Fatalf("a file: URL's: %s, %v", s, local)
	}
}

func TestFetchFromDirectory(t *testing.T) {
	// Without a network: the release's archives copied into a directory.
	data := archive(t, map[string]string{"lib/release/libcore.a": lib})
	setup(t, sum(data), sum([]byte(lib)))
	dir := t.TempDir()
	os.WriteFile(filepath.Join(dir, asset), data, 0o644)
	t.Setenv("ASUKA_PREBUILT_URL", dir)
	if _, ok, err := Archive("test-arm64", "release", "core"); err != nil || !ok {
		t.Fatalf("Archive: %v, %v", ok, err)
	}
}

func TestMirrorToken(t *testing.T) {
	data := archive(t, map[string]string{"lib/release/libcore.a": lib})
	rel := newRelease(t, data)
	rel.token = "Bearer s3cret"
	setup(t, sum(data), sum([]byte(lib)))
	t.Setenv("ASUKA_PREBUILT_URL", rel.URL+"/download/v{version}")
	if _, _, err := Archive("test-arm64", "release", "core"); err == nil || !strings.Contains(err.Error(), "401") {
		t.Fatalf("Archive without the token = %v, want a 401", err)
	}
	reset()
	t.Setenv("ASUKA_PREBUILT_TOKEN", "s3cret")
	if _, ok, err := Archive("test-arm64", "release", "core"); err != nil || !ok {
		t.Fatalf("Archive with the token: %v, %v", ok, err)
	}
}

func TestRetried(t *testing.T) {
	data := archive(t, map[string]string{"lib/release/libcore.a": lib})
	rel := newRelease(t, data)
	rel.failures.Store(2)
	setup(t, sum(data), sum([]byte(lib)))
	t.Setenv("ASUKA_PREBUILT_URL", rel.URL+"/download/v{version}")
	if _, ok, err := Archive("test-arm64", "release", "core"); err != nil || !ok {
		t.Fatalf("Archive after two 503s: %v, %v", ok, err)
	}
	// A 404 isn't tried again.
	setup(t, sum(data), sum([]byte(lib)))
	t.Setenv("ASUKA_PREBUILT_URL", rel.URL+"/download/v9.9.9")
	n := rel.requests.Load()
	if _, _, err := Archive("test-arm64", "release", "core"); err == nil || !strings.Contains(err.Error(), "404") {
		t.Fatalf("Archive of a missing release = %v, want a 404", err)
	}
	if got := rel.requests.Load() - n; got != 1 {
		t.Fatalf("a 404 was asked %d times", got)
	}
}

func TestChecksumMismatchRefused(t *testing.T) {
	data := archive(t, map[string]string{"lib/release/libcore.a": lib})
	other := archive(t, map[string]string{"lib/release/libcore.a": "!<arch>\nsomething else\n"})

	// The release (or a mirror) serves another archive than the CLI pins.
	rel := newRelease(t, other)
	setup(t, sum(data), sum([]byte(lib)))
	t.Setenv("ASUKA_PREBUILT_URL", rel.URL+"/download/v{version}")
	n := rel.requests.Load()
	if _, _, err := Archive("test-arm64", "release", "core"); !errors.Is(err, ErrChecksum) {
		t.Fatalf("Archive = %v; want ErrChecksum", err)
	}
	if got := rel.requests.Load() - n; got != 2 {
		t.Fatalf("%d requests: another archive than the pinned one was fetched again", got)
	}
	d := Dir(Package{Asset: asset})
	if _, err := os.Stat(d); err == nil {
		t.Fatal("a refused archive is in the cache")
	}
	if es, _ := os.ReadDir(filepath.Dir(d)); len(es) != 0 {
		t.Fatalf("a refused download left %v", es)
	}
}

func TestLibraryHashMismatchRefused(t *testing.T) {
	data := archive(t, map[string]string{"lib/release/libcore.a": lib})
	rel := newRelease(t, data)
	setup(t, sum(data), strings.Repeat("0", 64))
	t.Setenv("ASUKA_PREBUILT_URL", rel.URL+"/download/v{version}")
	_, _, err := Archive("test-arm64", "release", "core")
	if err == nil || !strings.Contains(err.Error(), "SHA-256") {
		t.Fatalf("Archive = %v; want a SHA-256 mismatch", err)
	}
	if _, err := os.Stat(Dir(Package{Asset: asset})); err == nil {
		t.Fatal("a package whose library didn't match stayed in the cache")
	}
}

func TestPackIsReproducible(t *testing.T) {
	files := map[string]string{"lib/release/libcore.a": lib, "lib/dev/libcore.a": "dev", "prebuilt.json": "{}"}
	a := archive(t, files)
	b := archive(t, files)
	if !bytes.Equal(a, b) {
		t.Fatal("the same files packed twice differ")
	}
	dir := t.TempDir()
	if err := unpack(bytes.NewReader(a), dir); err != nil {
		t.Fatal(err)
	}
	for name, data := range files {
		if got, _ := os.ReadFile(filepath.Join(dir, filepath.FromSlash(name))); string(got) != data {
			t.Fatalf("%s unpacked is %q", name, got)
		}
	}
}

func TestUnpackRefusesEscapes(t *testing.T) {
	var b bytes.Buffer
	gz := gzip.NewWriter(&b)
	tw := tar.NewWriter(gz)
	tw.WriteHeader(&tar.Header{Name: "top/../../evil", Mode: 0o644, Size: 1, Typeflag: tar.TypeReg})
	tw.Write([]byte("x"))
	tw.Close()
	gz.Close()
	if err := unpack(bytes.NewReader(b.Bytes()), t.TempDir()); err == nil {
		t.Fatal("an archive's file outside it was written")
	}
}

func TestHostTarget(t *testing.T) {
	for _, c := range []struct{ goos, goarch, want string }{
		{"darwin", "arm64", "darwin-arm64"},
		{"linux", "amd64", "linux-x64"},
		{"linux", "arm64", "linux-arm64"},
		{"windows", "amd64", "windows-x64"},
		{"windows", "arm64", "windows-arm64"},
	} {
		if got := HostTarget(c.goos, c.goarch); got != c.want {
			t.Errorf("HostTarget(%s, %s) = %s, want %s", c.goos, c.goarch, got, c.want)
		}
	}
}

func TestFetchTarget(t *testing.T) {
	data := archive(t, map[string]string{"lib/release/libcore.a": lib, "prebuilt.json": "{}"})
	rel := newRelease(t, data)
	setup(t, sum(data), sum([]byte(lib)))
	t.Setenv("ASUKA_PREBUILT_URL", rel.URL+"/download/v{version}")

	dir, pkg, ok, err := FetchTarget("test-arm64")
	if err != nil || !ok || pkg.Asset != asset {
		t.Fatalf("FetchTarget: %v, %v, %+v", ok, err, pkg)
	}
	if b, _ := os.ReadFile(filepath.Join(dir, "lib", "release", "libcore.a")); string(b) != lib {
		t.Fatalf("the library is %q", b)
	}
	n := rel.requests.Load()
	// A build then finds it in the cache.
	if _, ok, err := Archive("test-arm64", "release", "core"); err != nil || !ok {
		t.Fatalf("Archive: %v, %v", ok, err)
	}
	if rel.requests.Load() != n {
		t.Fatal("the build went to the release")
	}
	if _, _, ok, err := FetchTarget("linux-x64"); ok || err != nil {
		t.Fatalf("a target the manifest hasn't: %v, %v", ok, err)
	}
}

func TestFetchTargetDamaged(t *testing.T) {
	data := archive(t, map[string]string{"lib/release/libcore.a": lib})
	rel := newRelease(t, data)
	setup(t, sum([]byte("another archive")), sum([]byte(lib)))
	t.Setenv("ASUKA_PREBUILT_URL", rel.URL+"/download/v{version}")
	if _, _, ok, err := FetchTarget("test-arm64"); ok || !errors.Is(err, ErrChecksum) {
		t.Fatalf("FetchTarget of an archive that isn't the pinned one: %v, %v", ok, err)
	}
}
