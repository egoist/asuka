package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"io/fs"
	"maps"
	"math/rand/v2"
	"net/http"
	"net/url"
	"os"
	"path"
	"path/filepath"
	"slices"
	"strings"
	"time"

	"github.com/egoist/asuka/internal/ccache"
	"github.com/egoist/asuka/internal/distpkg"
	"github.com/egoist/asuka/internal/gpgsign"
	iupdate "github.com/egoist/asuka/internal/update"
)

// Linux's signed packages and repositories: asuka linux keygen makes the
// OpenPGP key; asuka build signs the .rpm with it (an RPM's own
// signature, as rpm --addsign makes it); asuka publish --platform linux
// --repo writes the .deb and the .rpm into an apt repository and a dnf
// one, signed with it, keeping the versions they had:
//
//	key.asc, key.gpg              the public key, armored and binary
//	deb/dists/<suite>/InRelease   (Release, Release.gpg, main/binary-<arch>/Packages)
//	deb/pool/main/n/notes/notes_1.2.0_amd64.deb
//	rpm/<suite>/repodata/repomd.xml (repomd.xml.asc, the files it names)
//	rpm/<suite>/Packages/n/notes-1.2.0-1.x86_64.rpm
//	rpm/notes.repo                dnf's .repo file (with the repository's url)
//
// The suite is stable for a release, a prerelease's channel for one
// (beta): users of the beta add both.

// linuxRepository is asuka.toml's linux.repository.
//
//	[linux.repository]
//	url = "https://packages.example.com/notes"
//	s3 = { bucket = "packages", prefix = "notes" }
type linuxRepository struct {
	// URL is where the repository is served (https): the .repo file and
	// the lines asuka prints name it.
	URL string `json:"url"`
	// S3 is the bucket that serves it, which asuka publish writes; or Dir,
	// a directory (relative to the app's).
	S3  *s3Config `json:"s3"`
	Dir string    `json:"dir"`
}

// linuxSigningKey is the OpenPGP key that signs the .rpm and the
// repository: ASUKA_LINUX_GPG_KEY (the armored key, or its file's path),
// else linux.signingKey's file, decrypted with ASUKA_LINUX_GPG_PASSPHRASE
// when it's encrypted; nil when there's none.
func linuxSigningKey(app *asukaApp) (*gpgsign.Key, error) {
	text := strings.TrimSpace(os.Getenv("ASUKA_LINUX_GPG_KEY"))
	file := ""
	switch {
	case strings.Contains(text, "-----BEGIN PGP"):
	case text != "":
		file, text = text, ""
	case app.cfg.Linux.SigningKey != "":
		file = app.cfg.Linux.SigningKey
	default:
		return nil, nil
	}
	if file != "" {
		if rest, ok := strings.CutPrefix(file, "~/"); ok {
			home, _ := os.UserHomeDir()
			file = filepath.Join(home, rest)
		} else if !filepath.IsAbs(file) && app.cfg.Linux.SigningKey == file {
			file = filepath.Join(app.dir, file)
		}
		b, err := os.ReadFile(file)
		if err != nil {
			return nil, fmt.Errorf("the Linux signing key: %w", err)
		}
		text = string(b)
	}
	k, err := gpgsign.Parse([]byte(text), []byte(os.Getenv("ASUKA_LINUX_GPG_PASSPHRASE")))
	if err != nil {
		return nil, fmt.Errorf("the Linux signing key (ASUKA_LINUX_GPG_KEY, linux.signingKey): %w", err)
	}
	return k, nil
}

// cmdLinux is asuka linux: keygen.
func cmdLinux(args []string) error {
	const use = "usage: asuka linux keygen [--uid \"Name <email>\"] [--out dir] [--force]"
	if len(args) == 0 || args[0] != "keygen" {
		if len(args) > 0 && (args[0] == "-h" || args[0] == "--help") {
			fmt.Println(use)
			return errHelp
		}
		return errors.New(use)
	}
	config, _ := os.UserConfigDir()
	var out, uid string
	var force bool
	fs := newFlagSet("linux keygen", "")
	fs.about = `Makes the OpenPGP key (RSA 4096) that signs a Linux app's .rpm and its apt
and dnf repositories: asuka-linux.key, the secret, and asuka-linux.asc, the
public key, which asuka publish puts in the repository for users to trust.
Keep the secret out of the repository: in a password manager, and as CI's
ASUKA_LINUX_GPG_KEY secret (ASUKA_LINUX_GPG_PASSPHRASE, when it's set as the key
is made, encrypts it). Users' systems trust the key once: losing it means
they add another.`
	fs.String(&out, "out", 'o', filepath.Join(config, "asuka", "linux-keys"), "the `directory` to write the keys in")
	fs.String(&uid, "uid", 0, "", "the key's user ID, \"Name <email>\" (default: asuka.toml's linux.maintainer, else developer, else the app's name)")
	fs.Bool(&force, "force", 0, "replace keys that are there")
	if err := fs.parse(args[1:]); err != nil {
		return err
	}
	if uid == "" {
		if app, err := loadApp(".", platformLinux); err == nil {
			uid = cmpOr(app.cfg.Linux.Maintainer, app.cfg.Linux.Developer, app.displayName()+" packages")
		}
	}
	if uid == "" {
		return errors.New("asuka linux keygen: --uid \"Name <email>\" names the key (or run it in an app's directory)")
	}
	name, email := uid, ""
	if i := strings.LastIndexByte(uid, '<'); i >= 0 && strings.HasSuffix(uid, ">") {
		name, email = strings.TrimSpace(uid[:i]), uid[i+1:len(uid)-1]
	}
	secret, public := filepath.Join(out, "asuka-linux.key"), filepath.Join(out, "asuka-linux.asc")
	if !force {
		for _, p := range []string{secret, public} {
			if _, err := os.Stat(p); err == nil {
				return fmt.Errorf("%s holds keys already; --force replaces them", out)
			}
		}
	}
	k, err := gpgsign.Generate(name, email, time.Now())
	if err != nil {
		return err
	}
	pub, err := k.ArmoredPublic()
	if err != nil {
		return err
	}
	if p := os.Getenv("ASUKA_LINUX_GPG_PASSPHRASE"); p != "" {
		if err := k.Encrypt([]byte(p)); err != nil {
			return err
		}
	}
	priv, err := k.ArmoredPrivate()
	if err != nil {
		return err
	}
	if err := os.MkdirAll(out, 0o700); err != nil {
		return err
	}
	if err := os.WriteFile(secret, priv, 0o600); err != nil {
		return err
	}
	if err := os.WriteFile(public, pub, 0o644); err != nil {
		return err
	}
	fmt.Printf("asuka: wrote %s (secret: keep it safe) and %s, the key %s of %s\n\nAdd to asuka.toml, for builds on this computer (CI: the key in ASUKA_LINUX_GPG_KEY):\n\n  [linux]\n  signingKey = %q\n",
		secret, public, k.Fingerprint(), k.UserID(), secret)
	return nil
}

func cmpOr(xs ...string) string {
	for _, x := range xs {
		if strings.TrimSpace(x) != "" {
			return x
		}
	}
	return ""
}

// A repoStore is where a repository's files are: a directory, or an S3
// bucket. Publishes may write it at once (each architecture's, from CI's
// jobs): what names the indexes (InRelease, repomd.xml) is written only
// if it's still the one read (writeIf), else read again, and the indexes
// are kept by their digests, so that no publish undoes another's.
type repoStore interface {
	// read reads a file (fs.ErrNotExist when it isn't there).
	read(name string) ([]byte, error)
	// readTagged reads a file and its version's tag (an ETag), which
	// writeIf takes.
	readTagged(name string) ([]byte, string, error)
	// write writes one; an index (mutable) is served uncached.
	write(name string, data []byte, mutable bool) error
	// writeIf writes a file if it's still at the version tag, or for "",
	// if it isn't there: errConflict when another publish wrote it since.
	writeIf(name string, data []byte, tag string, mutable bool) error
	// upload copies the file at path there.
	upload(name, path string) error
	remove(name string) error
	// list lists the files under dir ("deb/dists/stable/"), and when each
	// was written.
	list(dir string) ([]storedFile, error)
	String() string
}

// A storedFile is a file a repoStore lists.
type storedFile struct {
	name     string
	modified time.Time
}

type dirStore struct{ root string }

func (d dirStore) path(name string) string { return filepath.Join(d.root, filepath.FromSlash(name)) }

func (d dirStore) read(name string) ([]byte, error) { return os.ReadFile(d.path(name)) }

// A directory's tags are its files' digests.
func (d dirStore) readTagged(name string) ([]byte, string, error) {
	b, err := d.read(name)
	if err != nil {
		return nil, "", err
	}
	sum := sha256.Sum256(b)
	return b, hex.EncodeToString(sum[:]), nil
}

func (d dirStore) write(name string, data []byte, _ bool) error {
	p := d.path(name)
	if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
		return err
	}
	tmp := ccache.TmpName(p)
	if err := os.WriteFile(tmp, data, 0o644); err != nil {
		os.Remove(tmp)
		return err
	}
	if err := os.Rename(tmp, p); err != nil {
		os.Remove(tmp)
		return err
	}
	return nil
}

// writeIf compares and writes under a lock file beside the file, made
// with O_EXCL: publishes on this computer, or on others sharing the
// directory, take turns.
func (d dirStore) writeIf(name string, data []byte, tag string, mutable bool) error {
	p := d.path(name)
	if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
		return err
	}
	lock := p + ".lock"
	for i := 0; ; i++ {
		f, err := os.OpenFile(lock, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0o644)
		if err == nil {
			f.Close()
			break
		}
		if !errors.Is(err, fs.ErrExist) {
			return err
		}
		// A lock older than a minute is a publish's that died.
		if st, err := os.Stat(lock); err == nil && time.Since(st.ModTime()) > time.Minute {
			os.Remove(lock)
			continue
		}
		if i >= 600 {
			return fmt.Errorf("%s: locked for a minute (remove it if no publish is running)", lock)
		}
		time.Sleep(100 * time.Millisecond)
	}
	defer os.Remove(lock)
	_, cur, err := d.readTagged(name)
	switch {
	case errors.Is(err, fs.ErrNotExist):
		cur = ""
	case err != nil:
		return err
	}
	if cur != tag {
		return fmt.Errorf("%s: %w", p, errConflict)
	}
	return d.write(name, data, mutable)
}

func (d dirStore) list(dir string) ([]storedFile, error) {
	var out []storedFile
	err := filepath.WalkDir(d.path(dir), func(p string, e fs.DirEntry, err error) error {
		if err != nil {
			if errors.Is(err, fs.ErrNotExist) {
				return nil
			}
			return err
		}
		if !e.Type().IsRegular() {
			return nil
		}
		info, err := e.Info()
		if err != nil {
			return err
		}
		rel, _ := filepath.Rel(d.root, p)
		out = append(out, storedFile{filepath.ToSlash(rel), info.ModTime()})
		return nil
	})
	return out, err
}

func (d dirStore) upload(name, src string) error {
	b, err := os.ReadFile(src)
	if err != nil {
		return err
	}
	return d.write(name, b, false)
}

func (d dirStore) remove(name string) error {
	err := os.Remove(d.path(name))
	if errors.Is(err, fs.ErrNotExist) {
		return nil
	}
	return err
}

func (d dirStore) String() string { return d.root }

type s3Store struct{ c *s3Client }

func (s s3Store) read(name string) ([]byte, error) {
	b, _, err := s.readTagged(name)
	return b, err
}

func (s s3Store) readTagged(name string) ([]byte, string, error) {
	b, tag, err := s.c.getTagged(s.c.key(name))
	if errors.Is(err, errNoObject) {
		return nil, "", fs.ErrNotExist
	}
	return b, tag, err
}

func (s s3Store) header(name string, mutable bool) http.Header {
	h := http.Header{}
	h.Set("Content-Type", repoFileType(name))
	if mutable {
		h.Set("Cache-Control", "no-cache")
	}
	return h
}

func (s s3Store) write(name string, data []byte, mutable bool) error {
	return s.c.putData(s.c.key(name), data, s.header(name, mutable))
}

func (s s3Store) writeIf(name string, data []byte, tag string, mutable bool) error {
	return s.c.putDataIf(s.c.key(name), data, tag, s.header(name, mutable))
}

func (s s3Store) upload(name, src string) error {
	return s.c.put(s.c.key(name), src, s.header(name, false))
}

func (s s3Store) remove(name string) error { return s.c.remove(s.c.key(name)) }

func (s s3Store) list(dir string) ([]storedFile, error) {
	objs, err := s.c.list(s.c.key(dir))
	if err != nil {
		return nil, err
	}
	var out []storedFile
	for _, o := range objs {
		name := o.Key
		if s.c.prefix != "" {
			name = strings.TrimPrefix(name, s.c.prefix+"/")
		}
		out = append(out, storedFile{name, o.LastModified})
	}
	return out, nil
}

func (s s3Store) String() string { return "s3://" + s.c.bucket + "/" + s.c.prefix }

// repoFileType is a repository file's media type.
func repoFileType(name string) string {
	switch {
	case strings.HasSuffix(name, ".gz"):
		return "application/gzip"
	case strings.HasSuffix(name, ".xml"):
		return "application/xml"
	case strings.HasSuffix(name, ".asc"), strings.HasSuffix(name, "InRelease"), strings.HasSuffix(name, ".gpg") && !strings.HasSuffix(name, "key.gpg"):
		return "application/pgp-signature"
	case strings.HasSuffix(name, "key.gpg"):
		return "application/pgp-keys"
	case strings.HasSuffix(name, ".deb"), strings.HasSuffix(name, ".rpm"):
		return assetType(name)
	}
	return "text/plain; charset=utf-8"
}

// linuxRepoStore is where asuka publish writes the repository: --repo's
// (a directory, or s3://bucket/prefix), else linux.repository's; nil for
// none.
func linuxRepoStore(app *asukaApp, flag string) (repoStore, error) {
	r := app.cfg.Linux.Repository
	if r != nil && r.URL != "" && !httpsURL(r.URL) {
		return nil, fmt.Errorf("asuka.toml: linux.repository.url %q is not an https URL", r.URL)
	}
	switch {
	case strings.HasPrefix(flag, "s3://"):
		bucket, prefix, _ := strings.Cut(strings.TrimPrefix(flag, "s3://"), "/")
		s := &s3Config{Bucket: bucket, Prefix: prefix}
		if r != nil && r.S3 != nil {
			// The bucket's region and endpoint, asuka.toml's (else the
			// environment's: AWS_ENDPOINT_URL).
			s.Region, s.Endpoint, s.PathStyle = r.S3.Region, r.S3.Endpoint, r.S3.PathStyle
		}
		if err := s.validate("--repo"); err != nil {
			return nil, err
		}
		c, err := newS3Client(s)
		if err != nil {
			return nil, err
		}
		return s3Store{c}, nil
	case flag != "":
		abs, err := filepath.Abs(flag)
		if err != nil {
			return nil, err
		}
		return dirStore{abs}, nil
	case r == nil:
		return nil, nil
	case r.S3 != nil:
		if err := r.S3.validate("linux.repository.s3"); err != nil {
			return nil, err
		}
		c, err := newS3Client(r.S3)
		if err != nil {
			return nil, err
		}
		return s3Store{c}, nil
	case r.Dir != "":
		d := r.Dir
		if !filepath.IsAbs(d) {
			d = filepath.Join(app.dir, d)
		}
		return dirStore{d}, nil
	}
	return nil, nil
}

// repoSuite is the suite of a version: stable for a release, a
// prerelease's channel for one.
func repoSuite(version string) string {
	if c := iupdate.Channel(version); c != "" {
		return c
	}
	return "stable"
}

// publishLinuxRepo writes the .deb and .rpm packages into the repository
// of store, signed with key: the packages first, their indexes, then what
// names the indexes (InRelease; repomd.xml), so that no client reads an
// index whose packages aren't there. Publishes may run at once (each
// architecture's): each adds to what the others wrote.
func publishLinuxRepo(app *asukaApp, store repoStore, key *gpgsign.Key, debs, rpms []string) error {
	now := time.Now().UTC()
	suite := repoSuite(app.cfg.Version)
	// The key users trust: the repository's must be this one.
	pubArmored, err := key.ArmoredPublic()
	if err != nil {
		return err
	}
	pub, err := key.Public()
	if err != nil {
		return err
	}
	for {
		old, err := store.read("key.asc")
		if err == nil {
			k, err := gpgsign.ParsePublic(old)
			if err != nil {
				return fmt.Errorf("%s's key.asc: %w", store, err)
			}
			if k != key.Fingerprint() {
				return fmt.Errorf("%s is signed with the key %s, not %s, which users' systems don't trust: sign with that key (ASUKA_LINUX_GPG_KEY), or publish to another repository", store, k, key.Fingerprint())
			}
			break
		}
		if !errors.Is(err, fs.ErrNotExist) {
			return err
		}
		// A new repository: its key, once (another publish's at once is
		// checked as above).
		if err := store.writeIf("key.asc", pubArmored, "", true); errors.Is(err, errConflict) {
			continue
		} else if err != nil {
			return err
		}
		if err := store.write("key.gpg", pub, true); err != nil {
			return err
		}
		break
	}
	if len(debs) > 0 {
		if err := publishApt(app, store, key, suite, debs, now); err != nil {
			return err
		}
	}
	if len(rpms) > 0 {
		if err := publishDnf(app, store, key, suite, rpms, now); err != nil {
			return err
		}
	}
	printRepoLines(app, store, suite, len(debs) > 0, len(rpms) > 0)
	return nil
}

// repoAttempts is how many times a publish reads the repository again
// when another changed it as it wrote.
const repoAttempts = 30

// repoRetry runs try until it doesn't conflict with another publish
// (errConflict), waiting a moment more each time; what changed is named
// in the message.
func repoRetry(what string, try func() error) error {
	for attempt := 1; ; attempt++ {
		err := try()
		if !errors.Is(err, errConflict) {
			return err
		}
		if attempt == repoAttempts {
			return fmt.Errorf("%s kept changing as this publish wrote it (%d tries): %w", what, attempt, err)
		}
		fmt.Printf("asuka: %s changed as this publish wrote it (another publish?): reading it again\n", what)
		time.Sleep(time.Duration(min(attempt, 10)) * time.Duration(100+rand.IntN(200)) * time.Millisecond)
	}
}

// byHashAge is how long the indexes no Release names any more stay in
// by-hash, for clients that read the Release before.
const byHashAge = 24 * time.Hour

// publishApt adds the .deb packages to the suite of the apt repository in
// deb/: their indexes kept by their digests (Acquire-By-Hash), which the
// suite's InRelease, written only if it's still the one read, names;
// then Release, Release.gpg and the indexes at their names, as of the
// latest InRelease.
func publishApt(app *asukaApp, store repoStore, key *gpgsign.Key, suite string, debs []string, now time.Time) error {
	dists := "deb/dists/" + suite + "/"
	pub, err := key.ArmoredPublic()
	if err != nil {
		return err
	}
	var stanzas []*distpkg.Stanza
	for _, d := range debs {
		s, err := distpkg.DebStanza(d)
		if err != nil {
			return err
		}
		if err := store.upload("deb/"+s.Get("Filename"), d); err != nil {
			return err
		}
		fmt.Printf("asuka: uploaded %s to %s/deb/%s\n", filepath.Base(d), store, s.Get("Filename"))
		stanzas = append(stanzas, s)
	}
	var rel *distpkg.AptRelease
	var replaced []byte // the Release this one replaced
	err = repoRetry(fmt.Sprintf("%s/%sInRelease", store, dists), func() error {
		signed, tag, err := store.readTagged(dists + "InRelease")
		indexes := map[string]string{} // arch → Packages
		replaced = nil
		switch {
		case err == nil:
			text, err := gpgsign.VerifyClearSigned(pub, signed)
			if err != nil {
				return fmt.Errorf("%s/%sInRelease isn't signed with the key: %w", store, dists, err)
			}
			replaced = text
			old := distpkg.ParseStanzas(string(text))
			if len(old) == 0 {
				return fmt.Errorf("%s/%sInRelease is empty", store, dists)
			}
			files := distpkg.ReleaseFiles(old[0])
			// The suite's architectures, their indexes as this Release
			// has them.
			for _, a := range strings.Fields(old[0].Get("Architectures")) {
				b, err := readAptIndex(store, dists, "main/binary-"+a+"/Packages", files)
				if err != nil {
					return err
				}
				indexes[a] = string(b)
			}
		case errors.Is(err, fs.ErrNotExist):
			tag = ""
		default:
			return err
		}
		for _, s := range stanzas {
			a := s.Get("Architecture")
			indexes[a] = distpkg.AddPackage(indexes[a], s)
		}
		rel = &distpkg.AptRelease{
			Origin: app.displayName(), Label: app.displayName(), Suite: suite, Codename: suite,
			Description: app.displayName() + " for Debian and Ubuntu", Date: now, Components: []string{"main"},
			Files: map[string][]byte{}, AcquireByHash: true,
		}
		for a, text := range indexes {
			p := "main/binary-" + a + "/Packages"
			rel.Files[p] = []byte(text)
			rel.Files[p+".gz"] = distpkg.Gzip([]byte(text))
		}
		// The indexes by their digests: each Release's stay as they are.
		for _, p := range slices.Sorted(maps.Keys(rel.Files)) {
			for _, at := range distpkg.ByHashPaths(p, rel.Files[p]) {
				if err := store.write(dists+at, rel.Files[p], false); err != nil {
					return err
				}
			}
		}
		inline, err := key.ClearSign(rel.Text())
		if err != nil {
			return err
		}
		return store.writeIf(dists+"InRelease", inline, tag, true)
	})
	if err != nil {
		return err
	}
	current, err := syncApt(store, key, pub, dists)
	if err != nil {
		return err
	}
	pruneByHash(store, dists, now, current, replaced)
	fmt.Printf("asuka: %s/%s: the apt suite %s, signed (%s)\n", store, strings.TrimSuffix(dists, "/"), suite, strings.Join(rel.Architectures(), ", "))
	return nil
}

// readAptIndex is the index at path of the suite in dists, as its Release
// names it in files: from by-hash, else (a suite written before by-hash)
// at its name. errConflict when that's another's: a publish is writing.
func readAptIndex(store repoStore, dists, path string, files map[string]distpkg.ReleaseFile) ([]byte, error) {
	f, ok := files[path]
	if !ok {
		return nil, fmt.Errorf("%s/%sInRelease names no %s", store, dists, path)
	}
	for _, p := range []string{distpkg.ByHashPath(path, f.SHA256), path} {
		b, err := store.read(dists + p)
		if errors.Is(err, fs.ErrNotExist) {
			continue
		}
		if err != nil {
			return nil, err
		}
		if sum := sha256.Sum256(b); hex.EncodeToString(sum[:]) == f.SHA256 {
			return b, nil
		}
	}
	return nil, fmt.Errorf("%s/%s%s isn't the one InRelease names: %w", store, dists, path, errConflict)
}

// syncApt writes the suite's Release, Release.gpg and indexes at their
// names (for clients that don't fetch by hash) as its InRelease has them,
// until InRelease stays the one they were written from: another publish
// that wrote it since writes them too. It returns that Release.
func syncApt(store repoStore, key *gpgsign.Key, pub []byte, dists string) ([]byte, error) {
	for range repoAttempts {
		signed, tag, err := store.readTagged(dists + "InRelease")
		if err != nil {
			return nil, err
		}
		text, err := gpgsign.VerifyClearSigned(pub, signed)
		if err != nil {
			return nil, fmt.Errorf("%s/%sInRelease isn't signed with the key: %w", store, dists, err)
		}
		if !bytes.HasSuffix(text, []byte("\n")) {
			text = append(text, '\n')
		}
		st := distpkg.ParseStanzas(string(text))
		if len(st) == 0 {
			return nil, fmt.Errorf("%s/%sInRelease is empty", store, dists)
		}
		files := distpkg.ReleaseFiles(st[0])
		for _, p := range slices.Sorted(maps.Keys(files)) {
			b, err := store.read(dists + distpkg.ByHashPath(p, files[p].SHA256))
			if err != nil {
				return nil, fmt.Errorf("%s/%sInRelease names %s, which by-hash doesn't have: %w", store, dists, p, err)
			}
			if err := store.write(dists+p, b, true); err != nil {
				return nil, err
			}
		}
		detached, err := key.SignArmored(text)
		if err != nil {
			return nil, err
		}
		if err := store.write(dists+"Release", text, true); err != nil {
			return nil, err
		}
		if err := store.write(dists+"Release.gpg", detached, true); err != nil {
			return nil, err
		}
		if _, latest, err := store.readTagged(dists + "InRelease"); err != nil || latest == tag {
			return text, err
		}
	}
	return nil, fmt.Errorf("%s/%sInRelease kept changing as this publish wrote its Release", store, dists)
}

// pruneByHash removes the suite's by-hash indexes that neither its
// Release nor the one before (replaced) names, once byHashAge has passed
// since they were written: what a publish at once just wrote, and what a
// client that read the Release before fetches, stay. Without a listing
// (S3 without s3:ListBucket) they all stay.
func pruneByHash(store repoStore, dists string, now time.Time, current, replaced []byte) {
	named := map[string]bool{}
	for _, text := range [][]byte{current, replaced} {
		if st := distpkg.ParseStanzas(string(text)); len(st) > 0 {
			for p, f := range distpkg.ReleaseFiles(st[0]) {
				for _, at := range f.ByHash(p) {
					named[dists+at] = true
				}
			}
		}
	}
	files, err := store.list(dists + "main/")
	if err != nil {
		fmt.Printf("asuka: keeping the old by-hash indexes of %s/%s: %v\n", store, strings.TrimSuffix(dists, "/"), err)
		return
	}
	for _, f := range files {
		if !strings.Contains(f.name, "/by-hash/") || named[f.name] || now.Sub(f.modified) < byHashAge {
			continue
		}
		if err := store.remove(f.name); err != nil {
			fmt.Printf("asuka: leaving %s: %v\n", f.name, err)
		}
	}
}

// publishDnf adds the .rpm packages, which must be signed, to the dnf
// repository rpm/<suite>: its metadata files (named by their digests),
// then repomd.xml, written only if it's still the one read, then its
// signature, as of the latest repomd.xml.
func publishDnf(app *asukaApp, store repoStore, key *gpgsign.Key, suite string, rpms []string, now time.Time) error {
	root := "rpm/" + suite + "/"
	var pkgs []*distpkg.YumPackage
	for _, r := range rpms {
		info, err := distpkg.ReadRPM(r)
		if err != nil {
			return err
		}
		if info.Name == "" || info.Version == "" || info.Release == "" || info.Arch == "" {
			return fmt.Errorf("%s: its header has no name, version, release or architecture", r)
		}
		if !info.Signed {
			return fmt.Errorf("%s isn't signed: build it with the Linux signing key (ASUKA_LINUX_GPG_KEY)", r)
		}
		href := "Packages/" + info.Name[:1] + "/" + info.Name + "-" + info.Version + "-" + info.Release + "." + info.Arch + ".rpm"
		if err := store.upload(root+href, r); err != nil {
			return err
		}
		fmt.Printf("asuka: uploaded %s to %s/%s%s\n", filepath.Base(r), store, root, href)
		pkgs = append(pkgs, distpkg.NewYumPackage(info, href, now))
	}
	var m *distpkg.YumMetadata
	var before []string // the metadata files the repomd.xml replaced names
	err := repoRetry(fmt.Sprintf("%s/%srepodata/repomd.xml", store, root), func() error {
		m, before = &distpkg.YumMetadata{}, nil
		repomd, tag, err := store.readTagged(root + "repodata/repomd.xml")
		switch {
		case err == nil:
			m, err = distpkg.ReadYumMetadata(repomd, func(href string) ([]byte, error) {
				b, err := store.read(root + href)
				if errors.Is(err, fs.ErrNotExist) {
					// Removed as another publish replaced this repomd.xml.
					return nil, fmt.Errorf("%s/%s%s: %w", store, root, href, errConflict)
				}
				return b, err
			})
			if err != nil {
				if errors.Is(err, errConflict) {
					return err
				}
				return fmt.Errorf("%s/%srepodata: %w", store, root, err)
			}
			data, _ := distpkg.ParseRepomd(repomd)
			for _, d := range data {
				before = append(before, d.Location.Href)
			}
		case errors.Is(err, fs.ErrNotExist):
			tag = ""
		default:
			return err
		}
		for _, p := range pkgs {
			m.Add(p)
		}
		files, err := m.Files(now)
		if err != nil {
			return err
		}
		for _, p := range slices.Sorted(maps.Keys(files.Files)) {
			if err := store.write(root+p, files.Files[p], false); err != nil {
				return err
			}
		}
		return store.writeIf(root+"repodata/repomd.xml", files.Repomd, tag, true)
	})
	if err != nil {
		return err
	}
	// Its signature, of the latest repomd.xml: another publish that wrote
	// it since signs it too.
	var current []string
	for i := 0; ; i++ {
		repomd, tag, err := store.readTagged(root + "repodata/repomd.xml")
		if err != nil {
			return err
		}
		sig, err := key.SignArmored(repomd)
		if err != nil {
			return err
		}
		if err := store.write(root+"repodata/repomd.xml.asc", sig, true); err != nil {
			return err
		}
		if _, latest, err := store.readTagged(root + "repodata/repomd.xml"); err != nil {
			return err
		} else if latest == tag {
			data, _ := distpkg.ParseRepomd(repomd)
			for _, d := range data {
				current = append(current, d.Location.Href)
			}
			break
		}
		if i == repoAttempts {
			return fmt.Errorf("%s/%srepodata/repomd.xml kept changing as this publish signed it", store, root)
		}
	}
	// What the metadata replaced goes, as createrepo_c removes it.
	for _, href := range before {
		if !slices.Contains(current, href) && strings.HasPrefix(href, "repodata/") && !strings.Contains(href, "..") {
			if err := store.remove(root + href); err != nil {
				fmt.Printf("asuka: leaving %s%s: %v\n", root, href, err)
			}
		}
	}
	if u := repoURL(app); u != "" {
		if err := store.write("rpm/"+repoFileName(app, suite), []byte(dnfRepoFile(app, u, suite)), true); err != nil {
			return err
		}
	}
	fmt.Printf("asuka: %s/%s: the dnf repository %s, signed (%d packages)\n", store, strings.TrimSuffix(root, "/"), suite, len(m.Packages))
	return nil
}

// repoURL is where the repository is served, "" when asuka.toml doesn't
// say.
func repoURL(app *asukaApp) string {
	if r := app.cfg.Linux.Repository; r != nil {
		return strings.TrimSuffix(r.URL, "/")
	}
	return ""
}

// repoID is the repository's name in apt's and dnf's configuration: the
// app's executable's, with the suite for a channel's (notes-beta).
func repoID(app *asukaApp, suite string) string {
	id := linuxExe(app)
	if suite != "stable" {
		id += "-" + suite
	}
	return id
}

func repoFileName(app *asukaApp, suite string) string { return repoID(app, suite) + ".repo" }

// dnfRepoFile is the .repo file of the suite's dnf repository, served at u.
func dnfRepoFile(app *asukaApp, u, suite string) string {
	name := app.displayName()
	if suite != "stable" {
		name += " (" + suite + ")"
	}
	return "[" + repoID(app, suite) + "]\n" +
		"name=" + name + "\n" +
		"baseurl=" + u + "/rpm/" + url.PathEscape(suite) + "\n" +
		"enabled=1\n" +
		"gpgcheck=1\n" +
		"repo_gpgcheck=1\n" +
		"gpgkey=" + u + "/key.asc\n"
}

// printRepoLines prints what users run, once, to install from the
// repository.
func printRepoLines(app *asukaApp, store repoStore, suite string, deb, rpm bool) {
	u := repoURL(app)
	if u == "" {
		fmt.Printf("asuka: serve %s over HTTPS, and set asuka.toml's linux.repository.url to its URL for the lines users run\n", store)
		u = "https://…"
	}
	id, exe := repoID(app, suite), linuxExe(app)
	var b bytes.Buffer
	if deb {
		fmt.Fprintf(&b, "\nDebian and Ubuntu, once:\n\n  curl -fsSL %s/key.asc | sudo gpg --dearmor -o /etc/apt/keyrings/%s.gpg\n", u, linuxExe(app))
		fmt.Fprintf(&b, "  echo \"deb [signed-by=/etc/apt/keyrings/%s.gpg] %s/deb %s main\" | sudo tee /etc/apt/sources.list.d/%s.list\n", exe, u, suite, id)
		fmt.Fprintf(&b, "  sudo apt update && sudo apt install %s\n", debName(exe))
	}
	if rpm {
		fmt.Fprintf(&b, "\nFedora, once:\n\n  sudo dnf config-manager addrepo --from-repofile=%s/rpm/%s\n  sudo dnf install %s\n", u, path.Base(repoFileName(app, suite)), debName(exe))
	}
	fmt.Print(b.String())
}
