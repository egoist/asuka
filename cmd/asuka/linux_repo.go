package main

import (
	"bytes"
	"errors"
	"fmt"
	"io/fs"
	"maps"
	"net/http"
	"net/url"
	"os"
	"path"
	"path/filepath"
	"slices"
	"strings"
	"time"

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
// bucket.
type repoStore interface {
	// read reads a file (fs.ErrNotExist when it isn't there).
	read(name string) ([]byte, error)
	// write writes one; an index (mutable) is served uncached.
	write(name string, data []byte, mutable bool) error
	// upload copies the file at path there.
	upload(name, path string) error
	remove(name string) error
	String() string
}

type dirStore struct{ root string }

func (d dirStore) path(name string) string { return filepath.Join(d.root, filepath.FromSlash(name)) }

func (d dirStore) read(name string) ([]byte, error) { return os.ReadFile(d.path(name)) }

func (d dirStore) write(name string, data []byte, _ bool) error {
	p := d.path(name)
	if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
		return err
	}
	tmp := p + ".tmp"
	if err := os.WriteFile(tmp, data, 0o644); err != nil {
		return err
	}
	return os.Rename(tmp, p)
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
	b, err := s.c.get(s.c.key(name))
	if errors.Is(err, errNoObject) {
		return nil, fs.ErrNotExist
	}
	return b, err
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

func (s s3Store) upload(name, src string) error {
	return s.c.put(s.c.key(name), src, s.header(name, false))
}

func (s s3Store) remove(name string) error { return s.c.remove(s.c.key(name)) }

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
			// The bucket's region and endpoint, asuka.toml's.
			s.Region, s.Endpoint, s.PathStyle = r.S3.Region, r.S3.Endpoint, r.S3.PathStyle
		}
		if s.Endpoint == "" {
			s.Endpoint = cmpOr(os.Getenv("AWS_ENDPOINT_URL_S3"), os.Getenv("AWS_ENDPOINT_URL"))
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
// names the indexes (Release, InRelease; repomd.xml), so that no client
// reads an index whose packages aren't there.
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
	if old, err := store.read("key.asc"); err == nil {
		k, err := gpgsign.ParsePublic(old)
		if err != nil {
			return fmt.Errorf("%s's key.asc: %w", store, err)
		}
		if k != key.Fingerprint() {
			return fmt.Errorf("%s is signed with the key %s, not %s, which users' systems don't trust: sign with that key (ASUKA_LINUX_GPG_KEY), or publish to another repository", store, k, key.Fingerprint())
		}
	} else if !errors.Is(err, fs.ErrNotExist) {
		return err
	} else {
		if err := store.write("key.asc", pubArmored, true); err != nil {
			return err
		}
		if err := store.write("key.gpg", pub, true); err != nil {
			return err
		}
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

// publishApt adds the .deb packages to the suite of the apt repository in
// deb/.
func publishApt(app *asukaApp, store repoStore, key *gpgsign.Key, suite string, debs []string, now time.Time) error {
	dists := "deb/dists/" + suite + "/"
	indexes := map[string]string{} // arch → Packages
	for _, d := range debs {
		s, err := distpkg.DebStanza(d)
		if err != nil {
			return err
		}
		arch := s.Get("Architecture")
		if err := store.upload("deb/"+s.Get("Filename"), d); err != nil {
			return err
		}
		fmt.Printf("asuka: uploaded %s to %s/deb/%s\n", filepath.Base(d), store, s.Get("Filename"))
		if _, ok := indexes[arch]; !ok {
			b, err := store.read(dists + "main/binary-" + arch + "/Packages")
			if err != nil && !errors.Is(err, fs.ErrNotExist) {
				return err
			}
			indexes[arch] = string(b)
		}
		indexes[arch] = distpkg.AddPackage(indexes[arch], s)
	}
	// The suite's other architectures, as they are.
	if old, err := store.read(dists + "Release"); err == nil {
		for _, s := range distpkg.ParseStanzas(string(old)) {
			for _, a := range strings.Fields(s.Get("Architectures")) {
				if _, ok := indexes[a]; ok {
					continue
				}
				b, err := store.read(dists + "main/binary-" + a + "/Packages")
				if err != nil {
					return fmt.Errorf("%s/%s's Release lists %s, whose Packages is gone: %w", store, dists, a, err)
				}
				indexes[a] = string(b)
			}
		}
	} else if !errors.Is(err, fs.ErrNotExist) {
		return err
	}
	rel := &distpkg.AptRelease{
		Origin: app.displayName(), Label: app.displayName(), Suite: suite, Codename: suite,
		Description: app.displayName() + " for Debian and Ubuntu", Date: now, Components: []string{"main"},
		Files: map[string][]byte{},
	}
	for a, text := range indexes {
		p := "main/binary-" + a + "/Packages"
		rel.Files[p] = []byte(text)
		rel.Files[p+".gz"] = distpkg.Gzip([]byte(text))
	}
	for _, p := range slices.Sorted(maps.Keys(rel.Files)) {
		if err := store.write(dists+p, rel.Files[p], true); err != nil {
			return err
		}
	}
	release := rel.Text()
	detached, err := key.SignArmored(release)
	if err != nil {
		return err
	}
	inline, err := key.ClearSign(release)
	if err != nil {
		return err
	}
	for _, f := range []struct {
		name string
		data []byte
	}{{"Release", release}, {"Release.gpg", detached}, {"InRelease", inline}} {
		if err := store.write(dists+f.name, f.data, true); err != nil {
			return err
		}
	}
	fmt.Printf("asuka: %s/%s: the apt suite %s, signed (%s)\n", store, strings.TrimSuffix(dists, "/"), suite, strings.Join(rel.Architectures(), ", "))
	return nil
}

// publishDnf adds the .rpm packages, which must be signed, to the dnf
// repository rpm/<suite>.
func publishDnf(app *asukaApp, store repoStore, key *gpgsign.Key, suite string, rpms []string, now time.Time) error {
	root := "rpm/" + suite + "/"
	m := &distpkg.YumMetadata{}
	var before []string // the metadata files the repository had
	if repomd, err := store.read(root + "repodata/repomd.xml"); err == nil {
		if m, err = distpkg.ReadYumMetadata(repomd, func(href string) ([]byte, error) { return store.read(root + href) }); err != nil {
			return fmt.Errorf("%s/%srepodata: %w", store, root, err)
		}
		data, _ := distpkg.ParseRepomd(repomd)
		for _, d := range data {
			before = append(before, d.Location.Href)
		}
	} else if !errors.Is(err, fs.ErrNotExist) {
		return err
	}
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
		m.Add(distpkg.NewYumPackage(info, href, now))
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
	sig, err := key.SignArmored(files.Repomd)
	if err != nil {
		return err
	}
	if err := store.write(root+"repodata/repomd.xml.asc", sig, true); err != nil {
		return err
	}
	if err := store.write(root+"repodata/repomd.xml", files.Repomd, true); err != nil {
		return err
	}
	// What the metadata replaced goes, as createrepo_c removes it.
	for _, href := range before {
		if _, ok := files.Files[href]; !ok && strings.HasPrefix(href, "repodata/") && !strings.Contains(href, "..") {
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
