package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"maps"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/egoist/asuka/internal/distpkg"
	"github.com/egoist/asuka/internal/gpgsign"
)

var (
	linuxKeyOnce sync.Once
	linuxKeys    [2]*gpgsign.Key
)

// testLinuxKeys are two OpenPGP keys, made once (RSA 4096 takes a moment).
func testLinuxKeys(t *testing.T) [2]*gpgsign.Key {
	t.Helper()
	linuxKeyOnce.Do(func() {
		for i := range linuxKeys {
			linuxKeys[i], _ = gpgsign.Generate("Notes", "dev@example.com", time.Unix(1_700_000_000, 0))
		}
	})
	if linuxKeys[1] == nil {
		t.Fatal("no key")
	}
	return linuxKeys
}

// testLinuxPackages writes a .deb and a signed .rpm of version for arch.
func testLinuxPackages(t *testing.T, key *gpgsign.Key, version, arch string) (deb, rpm string) {
	t.Helper()
	dir := t.TempDir()
	p := &distpkg.Package{Name: "notes", Version: version, Arch: arch, Summary: "Write things down", License: "MIT",
		Files: []distpkg.File{{Path: "/usr/bin/notes", Mode: 0o755, Data: []byte("notes " + version)}}, Time: time.Unix(1_700_000_000, 0)}
	deb, rpm = filepath.Join(dir, "notes.deb"), filepath.Join(dir, "notes.rpm")
	if err := distpkg.WriteDeb(p, deb); err != nil {
		t.Fatal(err)
	}
	if key != nil {
		p.Signer = key
	}
	if err := distpkg.WriteRPM(p, nil, rpm); err != nil {
		t.Fatal(err)
	}
	return deb, rpm
}

// asuka publish --repo dir: the packages in the pool, the indexes signed
// with the key, the versions the repository had kept, each architecture's
// own; a prerelease in its channel's suite; another key refused.
func TestPublishLinuxRepo(t *testing.T) {
	keys := testLinuxKeys(t)
	repo := t.TempDir()
	app := &asukaApp{dir: t.TempDir(), name: "notes", bundleID: "com.example.notes", cfg: &appConfig{Name: "Notes"}}
	app.cfg.Linux.Repository = &linuxRepository{URL: "https://packages.example.com/notes"}
	store, err := linuxRepoStore(app, repo)
	if err != nil {
		t.Fatal(err)
	}
	publish := func(key *gpgsign.Key, version, arch string) error {
		app.cfg.Version = version
		deb, rpm := testLinuxPackages(t, key, version, arch)
		return publishLinuxRepo(app, store, key, []string{deb}, []string{rpm})
	}
	for _, v := range [][2]string{{"1.0.0", "amd64"}, {"1.1.0", "amd64"}, {"1.1.0", "arm64"}, {"1.2.0-beta.1", "amd64"}} {
		if err := publish(keys[0], v[0], v[1]); err != nil {
			t.Fatal(err)
		}
	}
	read := func(p string) []byte {
		b, err := os.ReadFile(filepath.Join(repo, p))
		if err != nil {
			t.Fatal(err)
		}
		return b
	}
	pub := read("key.asc")
	if fp, err := gpgsign.ParsePublic(read("key.gpg")); err != nil || fp != keys[0].Fingerprint() {
		t.Errorf("key.gpg: %s %v", fp, err)
	}
	// apt: InRelease and Release.gpg are Release's signatures; Release
	// names both architectures' indexes; each has its versions.
	release := read("deb/dists/stable/Release")
	if err := gpgsign.Verify(pub, release, read("deb/dists/stable/Release.gpg")); err != nil {
		t.Errorf("Release.gpg: %v", err)
	}
	if text, err := gpgsign.VerifyClearSigned(pub, read("deb/dists/stable/InRelease")); err != nil || !bytes.Equal(bytes.TrimSpace(text), bytes.TrimSpace(release)) {
		t.Errorf("InRelease: %v", err)
	}
	r := distpkg.ParseStanzas(string(release))[0]
	if r.Get("Architectures") != "amd64 arm64" || !strings.Contains(r.Get("SHA256"), "main/binary-arm64/Packages.gz") {
		t.Errorf("Release:\n%s", release)
	}
	amd := distpkg.ParseStanzas(string(read("deb/dists/stable/main/binary-amd64/Packages")))
	if len(amd) != 2 || amd[0].Get("Version") != "1.0.0" || amd[1].Get("Version") != "1.1.0" {
		t.Errorf("amd64's Packages: %d", len(amd))
	}
	for _, s := range amd {
		if _, err := os.Stat(filepath.Join(repo, "deb", s.Get("Filename"))); err != nil {
			t.Error(err)
		}
	}
	if beta := distpkg.ParseStanzas(string(read("deb/dists/beta/main/binary-amd64/Packages"))); len(beta) != 1 || beta[0].Get("Version") != "1.2.0~beta.1" {
		t.Errorf("the beta suite: %+v", beta)
	}
	// dnf: repomd.xml signed, three packages in stable (two amd64, one
	// arm64), the .repo file.
	repomd := read("rpm/stable/repodata/repomd.xml")
	if err := gpgsign.Verify(pub, repomd, read("rpm/stable/repodata/repomd.xml.asc")); err != nil {
		t.Errorf("repomd.xml.asc: %v", err)
	}
	m, err := distpkg.ReadYumMetadata(repomd, func(href string) ([]byte, error) { return os.ReadFile(filepath.Join(repo, "rpm/stable", href)) })
	if err != nil || len(m.Packages) != 3 {
		t.Fatalf("the dnf repository: %v %v", m, err)
	}
	for _, p := range m.Packages {
		if _, err := os.Stat(filepath.Join(repo, "rpm/stable", p.Href)); err != nil {
			t.Error(err)
		}
	}
	// Only the metadata repomd.xml names: what it replaced is gone.
	if left, _ := filepath.Glob(filepath.Join(repo, "rpm/stable/repodata/*.xml.gz")); len(left) != 3 {
		t.Errorf("repodata: %v", left)
	}
	if got, want := string(read("rpm/notes.repo")), "[notes]\nname=Notes\nbaseurl=https://packages.example.com/notes/rpm/stable\nenabled=1\ngpgcheck=1\nrepo_gpgcheck=1\ngpgkey=https://packages.example.com/notes/key.asc\n"; got != want {
		t.Errorf("notes.repo:\n%s", got)
	}
	if b := read("rpm/notes-beta.repo"); !bytes.Contains(b, []byte("[notes-beta]\n")) || !bytes.Contains(b, []byte("/rpm/beta\n")) {
		t.Errorf("notes-beta.repo:\n%s", b)
	}
	// Another key: users' systems trust the first.
	if err := publish(keys[1], "1.3.0", "amd64"); err == nil || !strings.Contains(err.Error(), keys[0].Fingerprint()) {
		t.Errorf("another key: %v", err)
	}
	// An unsigned .rpm isn't published.
	deb, rpm := testLinuxPackages(t, nil, "1.3.0", "amd64")
	if err := publishLinuxRepo(app, store, keys[0], []string{deb}, []string{rpm}); err == nil || !strings.Contains(err.Error(), "isn't signed") {
		t.Errorf("unsigned: %v", err)
	}
}

func TestLinuxRepoStore(t *testing.T) {
	t.Setenv("AWS_ACCESS_KEY_ID", "id")
	t.Setenv("AWS_SECRET_ACCESS_KEY", "secret")
	app := &asukaApp{dir: "/app", cfg: &appConfig{}}
	if s, err := linuxRepoStore(app, ""); s != nil || err != nil {
		t.Errorf("none: %v %v", s, err)
	}
	app.cfg.Linux.Repository = &linuxRepository{Dir: "packages", S3: nil}
	if s, err := linuxRepoStore(app, ""); err != nil || s.String() != filepath.Join("/app", "packages") {
		t.Errorf("linux.repository.dir: %v %v", s, err)
	}
	app.cfg.Linux.Repository = &linuxRepository{S3: &s3Config{Bucket: "pkgs", Prefix: "notes", Endpoint: "https://r2.example.com"}}
	s, err := linuxRepoStore(app, "s3://other/linux")
	if err != nil || s.String() != "s3://other/linux" || s.(s3Store).c.endpoint.Host != "r2.example.com" {
		t.Errorf("--repo s3://: %v %v", s, err)
	}
	if s, err := linuxRepoStore(app, ""); err != nil || s.String() != "s3://pkgs/notes" {
		t.Errorf("linux.repository.s3: %v %v", s, err)
	}
	app.cfg.Linux.Repository = &linuxRepository{URL: "http://example.com"}
	if _, err := linuxRepoStore(app, ""); err == nil {
		t.Error("an http URL")
	}
}

func TestLinuxKeygen(t *testing.T) {
	dir := t.TempDir()
	t.Setenv("ASUKA_LINUX_GPG_PASSPHRASE", "")
	if err := cmdLinux([]string{"keygen", "--uid", "Example Inc. <dev@example.com>", "--out", dir}); err != nil {
		t.Fatal(err)
	}
	priv, _ := os.ReadFile(filepath.Join(dir, "asuka-linux.key"))
	pub, _ := os.ReadFile(filepath.Join(dir, "asuka-linux.asc"))
	if st, _ := os.Stat(filepath.Join(dir, "asuka-linux.key")); st == nil || st.Mode().Perm() != 0o600 {
		t.Error("the secret key isn't the user's alone")
	}
	app := &asukaApp{dir: t.TempDir(), cfg: &appConfig{}}
	t.Setenv("ASUKA_LINUX_GPG_KEY", string(priv))
	k, err := linuxSigningKey(app)
	if err != nil || k.UserID() != "Example Inc. <dev@example.com>" {
		t.Fatalf("the key: %v %v", k, err)
	}
	if fp, _ := gpgsign.ParsePublic(pub); fp != k.Fingerprint() {
		t.Error("the public key isn't the secret's")
	}
	// Its file's path, in the environment or asuka.toml.
	t.Setenv("ASUKA_LINUX_GPG_KEY", filepath.Join(dir, "asuka-linux.key"))
	if k, err := linuxSigningKey(app); err != nil || k == nil {
		t.Errorf("the key's path: %v", err)
	}
	t.Setenv("ASUKA_LINUX_GPG_KEY", "")
	app.cfg.Linux.SigningKey = filepath.Join(dir, "asuka-linux.key")
	if k, err := linuxSigningKey(app); err != nil || k == nil {
		t.Errorf("linux.signingKey: %v", err)
	}
	if err := cmdLinux([]string{"keygen", "--uid", "x", "--out", dir}); err == nil {
		t.Error("replaced the keys")
	}
	// Encrypted with ASUKA_LINUX_GPG_PASSPHRASE, read with it.
	t.Setenv("ASUKA_LINUX_GPG_PASSPHRASE", "secret")
	if err := cmdLinux([]string{"keygen", "--uid", "x", "--out", dir, "--force"}); err != nil {
		t.Fatal(err)
	}
	if k, err := linuxSigningKey(app); err != nil || k == nil {
		t.Errorf("with the passphrase: %v", err)
	}
	t.Setenv("ASUKA_LINUX_GPG_PASSPHRASE", "")
	if _, err := linuxSigningKey(app); err == nil {
		t.Error("an encrypted key read without its passphrase")
	}
}

// --repo is Linux's.
func TestPublishRepoFlag(t *testing.T) {
	for _, p := range []string{platformWindows, platformMac, platformIOS, platformAndroid} {
		o := publishOptions{}
		o.platform = p
		if err := o.check(map[string]bool{"repo": true}); err == nil || !strings.Contains(err.Error(), "--repo is for --platform linux") {
			t.Errorf("%s: %v", p, err)
		}
	}
	o := publishOptions{}
	o.platform = platformLinux
	if err := o.check(map[string]bool{"repo": true}); err != nil {
		t.Error(err)
	}
}

// fakeS3 is an S3 bucket of the test's: GET (with an ETag), PUT (If-Match
// and If-None-Match: 412 when the object isn't that one), DELETE, and
// ListObjectsV2; the requests signed.
type fakeS3 struct {
	mu        sync.Mutex
	objects   map[string][]byte
	cache     map[string]string // Cache-Control
	modified  map[string]time.Time
	conflicts int // PUTs refused by their condition
	srv       *httptest.Server
}

func newFakeS3(t *testing.T) *fakeS3 {
	f := &fakeS3{objects: map[string][]byte{}, cache: map[string]string{}, modified: map[string]time.Time{}}
	f.srv = httptest.NewServer(http.HandlerFunc(f.serve))
	t.Cleanup(f.srv.Close)
	return f
}

func etagOf(b []byte) string {
	sum := sha256.Sum256(b)
	return `"` + hex.EncodeToString(sum[:16]) + `"`
}

func (f *fakeS3) serve(w http.ResponseWriter, r *http.Request) {
	if !strings.HasPrefix(r.Header.Get("Authorization"), "AWS4-HMAC-SHA256 ") {
		w.WriteHeader(http.StatusForbidden)
		return
	}
	f.mu.Lock()
	defer f.mu.Unlock()
	noKey := func() {
		w.WriteHeader(http.StatusNotFound)
		io.WriteString(w, "<Error><Code>NoSuchKey</Code><Message>The specified key does not exist.</Message></Error>")
	}
	switch r.Method {
	case http.MethodPut:
		b, _ := io.ReadAll(r.Body)
		old, exists := f.objects[r.URL.Path]
		if m := r.Header.Get("If-Match"); m != "" && (!exists || m != etagOf(old)) {
			f.conflicts++
			if !exists {
				noKey()
				return
			}
			w.WriteHeader(http.StatusPreconditionFailed)
			io.WriteString(w, "<Error><Code>PreconditionFailed</Code><Message>At least one of the pre-conditions you specified did not hold</Message></Error>")
			return
		}
		if r.Header.Get("If-None-Match") == "*" && exists {
			f.conflicts++
			w.WriteHeader(http.StatusPreconditionFailed)
			io.WriteString(w, "<Error><Code>PreconditionFailed</Code><Message>At least one of the pre-conditions you specified did not hold</Message></Error>")
			return
		}
		f.objects[r.URL.Path], f.cache[r.URL.Path], f.modified[r.URL.Path] = b, r.Header.Get("Cache-Control"), time.Now()
		w.Header().Set("ETag", etagOf(b))
	case http.MethodGet:
		if r.URL.Query().Get("list-type") == "2" {
			prefix := strings.TrimSuffix(r.URL.Path, "/") + "/" + r.URL.Query().Get("prefix")
			var b strings.Builder
			b.WriteString("<ListBucketResult>")
			for _, k := range slices.Sorted(maps.Keys(f.objects)) {
				if strings.HasPrefix(k, prefix) {
					fmt.Fprintf(&b, "<Contents><Key>%s</Key><LastModified>%s</LastModified></Contents>", strings.TrimPrefix(k, strings.TrimSuffix(r.URL.Path, "/")+"/"), f.modified[k].UTC().Format("2006-01-02T15:04:05.000Z"))
				}
			}
			b.WriteString("<IsTruncated>false</IsTruncated></ListBucketResult>")
			io.WriteString(w, b.String())
			return
		}
		b, ok := f.objects[r.URL.Path]
		if !ok {
			noKey()
			return
		}
		w.Header().Set("ETag", etagOf(b))
		w.Write(b)
	case http.MethodDelete:
		delete(f.objects, r.URL.Path)
		w.WriteHeader(http.StatusNoContent)
	}
}

// The repository in an S3 bucket (a fake of the test's): read back to add
// the next version, the indexes uncached, kept by hash, what repomd.xml
// replaced deleted.
func TestPublishLinuxRepoS3(t *testing.T) {
	t.Setenv("AWS_ACCESS_KEY_ID", "id")
	t.Setenv("AWS_SECRET_ACCESS_KEY", "secret")
	keys := testLinuxKeys(t)
	f := newFakeS3(t)
	app := &asukaApp{dir: t.TempDir(), name: "notes", bundleID: "com.example.notes", cfg: &appConfig{Name: "Notes"}}
	app.cfg.Linux.Repository = &linuxRepository{URL: "https://packages.example.com/notes", S3: &s3Config{Bucket: "pkgs", Prefix: "notes", Endpoint: f.srv.URL}}
	store, err := linuxRepoStore(app, "")
	if err != nil {
		t.Fatal(err)
	}
	for _, v := range []string{"1.0.0", "1.1.0"} {
		app.cfg.Version = v
		deb, rpm := testLinuxPackages(t, keys[0], v, "amd64")
		if err := publishLinuxRepo(app, store, keys[0], []string{deb}, []string{rpm}); err != nil {
			t.Fatal(err)
		}
	}
	objects, cache := f.objects, f.cache
	const root = "/pkgs/notes/"
	if ss := distpkg.ParseStanzas(string(objects[root+"deb/dists/stable/main/binary-amd64/Packages"])); len(ss) != 2 {
		t.Errorf("Packages has %d", len(ss))
	}
	checkAptSuite(t, func(p string) []byte { return objects[root+p] }, keys[0], "stable", map[string]int{"amd64": 2})
	m, err := distpkg.ReadYumMetadata(objects[root+"rpm/stable/repodata/repomd.xml"], func(href string) ([]byte, error) {
		if b, ok := objects[root+"rpm/stable/"+href]; ok {
			return b, nil
		}
		return nil, os.ErrNotExist
	})
	if err != nil || len(m.Packages) != 2 {
		t.Fatalf("the dnf repository: %v %v", m, err)
	}
	var repodata int
	for k := range objects {
		if strings.HasPrefix(k, root+"rpm/stable/repodata/") && strings.HasSuffix(k, ".xml.gz") {
			repodata++
		}
	}
	if repodata != 3 {
		t.Errorf("%d metadata files: what repomd.xml replaced is left", repodata)
	}
	for _, k := range []string{"deb/dists/stable/InRelease", "deb/dists/stable/main/binary-amd64/Packages.gz", "rpm/stable/repodata/repomd.xml", "key.asc"} {
		if cache[root+k] != "no-cache" {
			t.Errorf("%s: Cache-Control %q", k, cache[root+k])
		}
	}
	if cache[root+"deb/pool/main/n/notes/notes_1.0.0_amd64.deb"] != "" {
		t.Error("a package is uncached")
	}
	// The first version's by-hash indexes stay a day: a client may have
	// read the Release that names them.
	var byHash int
	for k := range objects {
		if strings.Contains(k, "/by-hash/") {
			byHash++
		}
	}
	if byHash != 8 {
		t.Errorf("%d by-hash indexes, want both versions' Packages and Packages.gz, by SHA-256 and SHA-512", byHash)
	}
}

// checkAptSuite checks the apt suite read reads: InRelease signed with
// key, Release.gpg Release's signature, Release InRelease's text (with
// Acquire-By-Hash), each index at its name and in by-hash what Release
// says, and each architecture with its count of packages.
func checkAptSuite(t *testing.T, read func(path string) []byte, key *gpgsign.Key, suite string, want map[string]int) {
	t.Helper()
	pub, _ := key.ArmoredPublic()
	dists := "deb/dists/" + suite + "/"
	text, err := gpgsign.VerifyClearSigned(pub, read(dists+"InRelease"))
	if err != nil {
		t.Fatalf("InRelease: %v", err)
	}
	release := read(dists + "Release")
	if !bytes.Equal(bytes.TrimSpace(text), bytes.TrimSpace(release)) {
		t.Errorf("Release isn't InRelease's text:\n%s\n---\n%s", release, text)
	}
	if err := gpgsign.Verify(pub, release, read(dists+"Release.gpg")); err != nil {
		t.Errorf("Release.gpg: %v", err)
	}
	r := distpkg.ParseStanzas(string(text))[0]
	if r.Get("Acquire-By-Hash") != "yes" {
		t.Errorf("Release:\n%s", text)
	}
	files := distpkg.ReleaseFiles(r)
	for p, f := range files {
		if len(f.ByHash(p)) != 2 {
			t.Errorf("Release names %s without its SHA-256 and SHA-512", p)
		}
		for _, at := range append([]string{p}, f.ByHash(p)...) {
			b := read(dists + at)
			if sum := sha256.Sum256(b); hex.EncodeToString(sum[:]) != f.SHA256 || int64(len(b)) != f.Size {
				t.Errorf("%s isn't the %s Release names", at, p)
			}
		}
	}
	var archs []string
	for a, n := range want {
		archs = append(archs, a)
		if got := len(distpkg.ParseStanzas(string(read(dists + "main/binary-" + a + "/Packages")))); got != n {
			t.Errorf("%s has %d packages, want %d", a, got, n)
		}
	}
	slices.Sort(archs)
	if got := r.Get("Architectures"); got != strings.Join(archs, " ") {
		t.Errorf("Architectures: %q, want %v", got, archs)
	}
}

// racingStore is a store whose first write of what names a repository's
// indexes (InRelease, repomd.xml) another publish precedes: it runs race,
// a whole publish, first.
type racingStore struct {
	repoStore
	race    func()
	started map[string]bool
}

func (s *racingStore) writeIf(name string, data []byte, tag string, mutable bool) error {
	if (strings.HasSuffix(name, "/InRelease") || strings.HasSuffix(name, "/repomd.xml")) && !s.started[name] {
		s.started[name] = true
		s.race()
	}
	return s.repoStore.writeIf(name, data, tag, mutable)
}

// Two architectures published at once, into a directory and into an S3
// bucket: one's InRelease and repomd.xml are written by the other after
// it read them, so its own write is refused (412: If-Match), and it reads
// them again and adds to them: both architectures' packages are in the
// indexes, every index is the one the signed InRelease names, and
// repomd.xml.asc signs the repomd.xml that has both. Then four publishes
// at once, in goroutines: none is lost.
func TestPublishLinuxRepoAtOnce(t *testing.T) {
	t.Setenv("AWS_ACCESS_KEY_ID", "id")
	t.Setenv("AWS_SECRET_ACCESS_KEY", "secret")
	keys := testLinuxKeys(t)
	for _, kind := range []string{"dir", "s3"} {
		t.Run(kind, func(t *testing.T) {
			app := &asukaApp{dir: t.TempDir(), name: "notes", bundleID: "com.example.notes", cfg: &appConfig{Name: "Notes", Version: "1.0.0"}}
			app.cfg.Linux.Repository = &linuxRepository{URL: "https://packages.example.com/notes"}
			var read func(string) []byte
			var conflicts func() int
			var store repoStore
			if kind == "dir" {
				dir := t.TempDir()
				store = dirStore{dir}
				read = func(p string) []byte { b, _ := os.ReadFile(filepath.Join(dir, p)); return b }
				conflicts = func() int { return -1 }
			} else {
				f := newFakeS3(t)
				c, err := newS3Client(&s3Config{Bucket: "pkgs", Prefix: "notes", Endpoint: f.srv.URL})
				if err != nil {
					t.Fatal(err)
				}
				store = s3Store{c}
				read = func(p string) []byte { f.mu.Lock(); defer f.mu.Unlock(); return f.objects["/pkgs/notes/"+p] }
				conflicts = func() int { f.mu.Lock(); defer f.mu.Unlock(); return f.conflicts }
			}
			armDeb, armRPM := testLinuxPackages(t, keys[0], "1.0.0", "arm64")
			amdDeb, amdRPM := testLinuxPackages(t, keys[0], "1.0.0", "amd64")
			racing := &racingStore{repoStore: store, started: map[string]bool{}, race: func() {
				// The other architecture's publish, whole, between this
				// one's read and its write.
				if err := publishLinuxRepo(app, store, keys[0], []string{armDeb}, []string{armRPM}); err != nil {
					t.Error(err)
				}
			}}
			if err := publishLinuxRepo(app, racing, keys[0], []string{amdDeb}, []string{amdRPM}); err != nil {
				t.Fatal(err)
			}
			if n := conflicts(); n == 0 {
				t.Error("no write was refused: the race didn't happen")
			}
			checkAptSuite(t, read, keys[0], "stable", map[string]int{"amd64": 1, "arm64": 1})
			pub, _ := keys[0].ArmoredPublic()
			repomd := read("rpm/stable/repodata/repomd.xml")
			if err := gpgsign.Verify(pub, repomd, read("rpm/stable/repodata/repomd.xml.asc")); err != nil {
				t.Errorf("repomd.xml.asc: %v", err)
			}
			m, err := distpkg.ReadYumMetadata(repomd, func(href string) ([]byte, error) {
				if b := read("rpm/stable/" + href); b != nil {
					return b, nil
				}
				return nil, os.ErrNotExist
			})
			if err != nil || len(m.Packages) != 2 {
				t.Fatalf("the dnf repository: %v %v", m, err)
			}
			// And at once for real: four publishes in goroutines, each
			// architecture's next version and a beta.
			var wg sync.WaitGroup
			for _, v := range [][2]string{{"1.1.0", "amd64"}, {"1.1.0", "arm64"}, {"1.2.0", "amd64"}, {"1.2.0", "arm64"}} {
				deb, rpm := testLinuxPackages(t, keys[0], v[0], v[1])
				wg.Add(1)
				go func() {
					defer wg.Done()
					a := *app
					cfg := *app.cfg
					cfg.Version = v[0]
					a.cfg = &cfg
					if err := publishLinuxRepo(&a, store, keys[0], []string{deb}, []string{rpm}); err != nil {
						t.Error(err)
					}
				}()
			}
			wg.Wait()
			checkAptSuite(t, read, keys[0], "stable", map[string]int{"amd64": 3, "arm64": 3})
			repomd = read("rpm/stable/repodata/repomd.xml")
			if err := gpgsign.Verify(pub, repomd, read("rpm/stable/repodata/repomd.xml.asc")); err != nil {
				t.Errorf("repomd.xml.asc: %v", err)
			}
			if m, err := distpkg.ReadYumMetadata(repomd, func(href string) ([]byte, error) { return read("rpm/stable/" + href), nil }); err != nil || len(m.Packages) != 6 {
				t.Errorf("the dnf repository after: %v %v", m, err)
			}
		})
	}
}

// The suite's by-hash indexes no Release names go a day after they were
// written; those its Release or the one before names, and younger ones
// (a publish at once may be about to name them), stay.
func TestPruneByHash(t *testing.T) {
	dir := t.TempDir()
	store := dirStore{dir}
	dists := "deb/dists/stable/"
	rel := func(sums ...string) []byte {
		text := "Suite: stable\nSHA256:\n"
		for i, s := range sums {
			text += fmt.Sprintf(" %s %8d main/binary-amd64/Packages%s\n", s, 10, []string{"", ".gz"}[i%2])
		}
		return []byte(text)
	}
	old := time.Now().Add(-48 * time.Hour)
	files := map[string]time.Time{"aaaa": old, "bbbb": old, "cccc": old, "dddd": time.Now()}
	for sum, mtime := range files {
		p := filepath.Join(dir, filepath.FromSlash(dists+distpkg.ByHashPath("main/binary-amd64/Packages", sum)))
		os.MkdirAll(filepath.Dir(p), 0o755)
		os.WriteFile(p, []byte(sum), 0o644)
		os.Chtimes(p, mtime, mtime)
	}
	pruneByHash(store, dists, time.Now(), rel("aaaa"), rel("bbbb"))
	for sum, want := range map[string]bool{"aaaa": true, "bbbb": true, "cccc": false, "dddd": true} {
		_, err := os.Stat(filepath.Join(dir, filepath.FromSlash(dists+distpkg.ByHashPath("main/binary-amd64/Packages", sum))))
		if (err == nil) != want {
			t.Errorf("%s: kept %v, want %v", sum, err == nil, want)
		}
	}
}
