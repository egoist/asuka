//go:build e2e

package e2e

import (
	"bytes"
	"crypto/ed25519"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/egoist/asuka/internal/distpkg"
	"github.com/egoist/asuka/internal/gpgsign"
	"github.com/egoist/asuka/internal/s3test"
	"github.com/egoist/asuka/internal/sigv4"
	iupdate "github.com/egoist/asuka/internal/update"
)

// TestPublishLinuxS3 publishes e2e/testdata/updater with asuka publish
// --platform linux to a real S3-compatible server: the bucket of
// ASUKA_E2E_S3 (its endpoint and bucket: http://127.0.0.1:9000/pkgs, whose
// objects anyone may read, as a CDN serves them, with AWS_ACCESS_KEY_ID and
// AWS_SECRET_ACCESS_KEY), else RustFS's, started in Docker
// (internal/s3test): 1.0.0, then 1.0.1, each its update (updates.s3) and
// its apt and dnf repository (linux.repository.s3), under
// ASUKA_E2E_S3_PREFIX (default a new one). The update's manifest has the
// AppImage's delta; apps installed from 1.0.0's tarball and AppImage update
// themselves from the bucket. With Docker, Debian and Fedora containers add
// the repository as asuka publish says to, signatures checked, and install
// the app. Then 1.0.2 and 1.0.3 are published to the repository at once,
// which the bucket's conditional writes keep both of, and the containers
// upgrade to 1.0.3. With credentials without s3:ListBucket (RustFS's, or
// ASUKA_E2E_S3_NOLIST_KEY and _SECRET), a publish to a new repository fails
// saying so (Amazon S3's 403), or goes on without pruning (MinIO's and
// RustFS's 404).
//
//	xvfb-run -a dbus-run-session go test -tags e2e ./e2e -run TestPublishLinuxS3
func TestPublishLinuxS3(t *testing.T) {
	if runtime.GOOS != "linux" {
		t.Skip("Linux")
	}
	srv := s3test.Start(t, "ASUKA_E2E_S3")
	endpoint, bucket := srv.Endpoint, srv.Bucket
	prefix := os.Getenv("ASUKA_E2E_S3_PREFIX")
	if prefix == "" {
		prefix = fmt.Sprintf("e2e-%d", time.Now().Unix())
	}
	t.Logf("publishing to s3://%s/%s (%s)", bucket, prefix, endpoint)
	arch := runtime.GOARCH

	// The update key, the Linux key, and the app copied with them.
	pub, priv, _ := ed25519.GenerateKey(nil)
	keys := t.TempDir()
	c := exec.Command("go", "run", "./cmd/asuka", "linux", "keygen", "--uid", "Updater <dev@example.com>", "--out", keys)
	c.Dir = root(t)
	if b, err := c.CombinedOutput(); err != nil {
		t.Fatalf("asuka linux keygen: %v\n%s", err, b)
	}
	gpgPub, err := os.ReadFile(filepath.Join(keys, "asuka-linux.asc"))
	if err != nil {
		t.Fatal(err)
	}
	updates := endpoint + "/" + bucket + "/" + prefix + "/updates"
	repo := endpoint + "/" + bucket + "/" + prefix + "/repo"
	app := func(withUpdates bool) string {
		dir := filepath.Join(t.TempDir(), "updater")
		run(t, "cp", "-R", filepath.Join(root(t), "e2e", "testdata", "updater"), dir)
		toml, _ := os.ReadFile(filepath.Join(dir, "asuka.toml"))
		if withUpdates {
			toml = fmt.Appendf(toml, "\n[updates]\npublicKey = %q\nurl = %q\ns3 = { bucket = %q, prefix = %q, endpoint = %q }\n",
				base64.StdEncoding.EncodeToString(pub), updates, bucket, prefix+"/updates", endpoint)
		}
		toml = fmt.Appendf(toml, "\n[linux.repository]\nurl = %q\ns3 = { bucket = %q, prefix = %q, endpoint = %q }\n", repo, bucket, prefix+"/repo", endpoint)
		if err := os.WriteFile(filepath.Join(dir, "asuka.toml"), toml, 0o644); err != nil {
			t.Fatal(err)
		}
		return dir
	}
	// The environment of a publish with the keys, uploading with key.
	withKeys := func(key sigv4.Key) []string {
		env := []string{"ASUKA_UPDATE_PRIVATE_KEY=" + base64.StdEncoding.EncodeToString(priv), "ASUKA_LINUX_GPG_KEY=" + filepath.Join(keys, "asuka-linux.key")}
		for _, e := range s3test.Env(key) {
			if !strings.HasPrefix(e, "ASUKA_UPDATE_") && !strings.HasPrefix(e, "ASUKA_LINUX_GPG_") {
				env = append(env, e)
			}
		}
		return env
	}
	env := withKeys(srv.Key)
	var logs sync.Map // version → what its publish printed
	publish := func(dir, version string, env []string, args ...string) (string, error) {
		out := filepath.Join(t.TempDir(), "dist")
		c := exec.Command("go", append([]string{"run", "./cmd/asuka", "publish", "--platform", "linux", "--version", version, "-o", out}, append(args, dir)...)...)
		c.Dir, c.Env = root(t), env
		b, err := c.CombinedOutput()
		t.Logf("asuka publish %s:\n%s", version, b)
		logs.Store(version, string(b))
		if err != nil {
			return out, fmt.Errorf("asuka publish %s: %v\n%s", version, err, b)
		}
		return out, nil
	}
	get := func(u string) []byte {
		resp, err := http.Get(u) // as users' systems do: unsigned
		if err != nil {
			t.Fatal(err)
		}
		defer resp.Body.Close()
		b, _ := io.ReadAll(resp.Body)
		if resp.StatusCode != http.StatusOK {
			t.Fatalf("GET %s: %s\n%s", u, resp.Status, b)
		}
		return b
	}

	withUpdates := app(true)
	v100, err := publish(withUpdates, "1.0.0", env)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := publish(withUpdates, "1.0.1", env); err != nil {
		t.Fatal(err)
	}

	// The manifest, served by the bucket: 1.0.1's, with the deltas of its
	// archive and of its AppImage from 1.0.0's.
	var m iupdate.Manifest
	if err := json.Unmarshal(get(updates+"/"+iupdate.ManifestName("linux-"+arch, "")), &m); err != nil {
		t.Fatal(err)
	}
	if m.Version != "1.0.1" || len(m.Deltas) != 1 || m.AppImage == nil || len(m.AppImage.Deltas) != 1 || m.AppImage.Deltas[0].From != "1.0.0" {
		t.Fatalf("the manifest: %+v", m)
	}
	if b := get(m.AppImage.Deltas[0].URL); int64(len(b)) != m.AppImage.Deltas[0].Size {
		t.Errorf("the AppImage's delta: %d bytes, the manifest says %d", len(b), m.AppImage.Deltas[0].Size)
	}
	// The repository: InRelease signed, both versions in it.
	checkRepo := func(versions int) {
		t.Helper()
		text, err := gpgsign.VerifyClearSigned(gpgPub, get(repo+"/deb/dists/stable/InRelease"))
		if err != nil {
			t.Fatalf("InRelease: %v", err)
		}
		rel := distpkg.ParseStanzas(string(text))[0]
		files := distpkg.ReleaseFiles(rel)
		dpkgArch := map[string]string{"amd64": "amd64", "arm64": "arm64"}[arch]
		p := "main/binary-" + dpkgArch + "/Packages"
		packages := get(repo + "/deb/dists/stable/" + distpkg.ByHashPath(p, files[p].SHA256))
		if n := len(distpkg.ParseStanzas(string(packages))); n != versions {
			t.Errorf("apt's Packages has %d versions, want %d:\n%s", n, versions, packages)
		}
		repomd := get(repo + "/rpm/stable/repodata/repomd.xml")
		if err := gpgsign.Verify(gpgPub, repomd, get(repo+"/rpm/stable/repodata/repomd.xml.asc")); err != nil {
			t.Errorf("repomd.xml.asc: %v", err)
		}
		ym, err := distpkg.ReadYumMetadata(repomd, func(href string) ([]byte, error) { return get(repo + "/rpm/stable/" + href), nil })
		if err != nil || len(ym.Packages) != versions {
			t.Errorf("dnf's repository: %d packages, want %d (%v)", len(ym.Packages), versions, err)
		}
	}
	checkRepo(2)

	// Debian's apt and Fedora's dnf, in containers, add the repository as
	// asuka publish said to, and install the app: 1.0.1.
	said, _ := logs.Load("1.0.0")
	distros := linuxDistros(t, srv, said.(string))
	for _, d := range distros {
		d.check(t, "1.0.1")
	}

	// Updated from the bucket: the tarball's prefix, and the AppImage
	// (extracted: the container has no FUSE).
	prefixDir := t.TempDir()
	run(t, "tar", "-xzf", filepath.Join(v100, "linux-"+arch, "updater-1.0.0-linux-"+arch+".tar.gz"), "--strip-components=1", "-C", prefixDir)
	runUpdater(t, filepath.Join(prefixDir, "bin", "updater"), nil)
	apps := t.TempDir()
	file := filepath.Join(apps, "Updater.AppImage")
	run(t, "cp", filepath.Join(v100, "linux-"+arch, "updater-1.0.0-linux-"+arch+".AppImage"), file)
	runUpdater(t, file, []string{"--appimage-extract-and-run"}, "TMPDIR="+t.TempDir())
	got, _ := os.ReadFile(file)
	if want := get(m.AppImage.URL); !bytes.Equal(got, want) {
		t.Errorf("the AppImage isn't 1.0.1's (%d bytes, want %d)", len(got), len(want))
	}

	// Two publishes to the repository at once (two CI jobs): neither's
	// version is lost.
	repoOnly := app(false)
	var wg sync.WaitGroup
	errs := make([]error, 2)
	for i, v := range []string{"1.0.2", "1.0.3"} {
		wg.Add(1)
		go func() {
			defer wg.Done()
			_, errs[i] = publish(repoOnly, v, env, "--package", "deb,rpm")
		}()
	}
	wg.Wait()
	for _, err := range errs {
		if err != nil {
			t.Fatal(err)
		}
	}
	checkRepo(4)
	// apt and dnf upgrade to the newest.
	for _, d := range distros {
		d.upgrade(t)
		d.check(t, "1.0.3")
	}

	// Credentials without s3:ListBucket: Amazon S3 answers 403 for what
	// isn't there yet, and publish stops saying why; MinIO and RustFS
	// answer 404, and the publish goes on, keeping old by-hash indexes it
	// can't list.
	if srv.NoList != nil {
		_, err := publish(repoOnly, "1.0.4", withKeys(*srv.NoList), "--package", "deb,rpm", "--repo", "s3://"+bucket+"/"+prefix+"/repo-nolist")
		out, _ := logs.Load("1.0.4")
		switch {
		case err != nil && strings.Contains(err.Error(), "403") && strings.Contains(err.Error(), "s3:ListBucket"):
			t.Log("without s3:ListBucket: refused, as Amazon S3 does")
		case err == nil && strings.Contains(out.(string), "keeping the old by-hash indexes") && strings.Contains(out.(string), "listing needs the s3:ListBucket permission"):
			t.Log("without s3:ListBucket: published, the old by-hash indexes kept, as MinIO and RustFS answer 404")
		default:
			t.Errorf("published without s3:ListBucket: %v\n%s", err, out)
		}
	}
}

// The distributions that install the app from the repository: Debian's
// apt, Fedora's dnf5.
const (
	debianImage = "debian:trixie-slim"
	fedoraImage = "fedora:45"
)

// A linuxDistro is a container of a Linux distribution that installs the
// app from the repository.
type linuxDistro struct {
	name, container string
	// version prints the app's version installed; upgrade upgrades it.
	version, upgradeCmd string
}

// linuxDistros starts Debian and Fedora containers that reach srv, and in
// each runs what asuka publish printed (out) to add the repository and
// install the app, signatures checked; none without Docker.
func linuxDistros(t *testing.T, srv *s3test.Server, out string) []*linuxDistro {
	t.Helper()
	network, ok := srv.DockerNetwork()
	if !ok {
		t.Log("no Docker: apt and dnf don't install the app")
		return nil
	}
	// The lines under a heading, without sudo (they run as root).
	said := func(heading string) string {
		_, rest, ok := strings.Cut(out, heading+"\n\n")
		if !ok {
			t.Fatalf("asuka publish didn't say %q:\n%s", heading, out)
		}
		var lines []string
		for _, l := range strings.Split(rest, "\n") {
			l, ok := strings.CutPrefix(l, "  ")
			if !ok {
				break
			}
			lines = append(lines, strings.ReplaceAll(l, "sudo ", ""))
		}
		return strings.Join(lines, "\n")
	}
	distros := []*linuxDistro{
		{name: "debian", version: "dpkg-query -W -f '${Version}' updater", upgradeCmd: "apt-get update && apt-get install -y --only-upgrade updater"},
		{name: "fedora", version: "rpm -q --qf '%{VERSION}' updater", upgradeCmd: "dnf upgrade -y --refresh updater"},
	}
	images := map[string]string{"debian": debianImage, "fedora": fedoraImage}
	scripts := map[string]string{
		// curl and gpg, which the instructions use; apt answering yes.
		"debian": "apt-get update && apt-get install -y --no-install-recommends curl gpg ca-certificates\n" +
			"mkdir -p /etc/apt/keyrings\necho 'APT::Get::Assume-Yes \"true\";' > /etc/apt/apt.conf.d/90yes\n" +
			said("Debian and Ubuntu, once:"),
		// dnf's config-manager; dnf answering yes, to the key's import too.
		"fedora": "echo assumeyes=True >> /etc/dnf/dnf.conf\ndnf install -y dnf5-plugins\n" + said("Fedora, once:"),
	}
	errs := make([]error, len(distros))
	var wg sync.WaitGroup
	for i, d := range distros {
		d.container = fmt.Sprintf("asuka-%s-%d", d.name, time.Now().UnixNano())
		args := append(append([]string{"run", "-d", "--rm", "--name", d.container}, network...), images[d.name], "sleep", "infinity")
		if b, err := exec.Command("docker", args...).CombinedOutput(); err != nil {
			t.Fatalf("docker run %s: %v\n%s", images[d.name], err, b)
		}
		t.Cleanup(func() { exec.Command("docker", "rm", "-f", d.container).Run() })
		wg.Add(1)
		go func() {
			defer wg.Done()
			_, errs[i] = d.exec(scripts[d.name])
		}()
	}
	wg.Wait()
	for i, err := range errs {
		if err != nil {
			t.Fatalf("%s: %v", distros[i].name, err)
		}
	}
	return distros
}

// exec runs script in the container (sh -e).
func (d *linuxDistro) exec(script string) (string, error) {
	b, err := exec.Command("docker", "exec", d.container, "sh", "-ec", script).CombinedOutput()
	if err != nil {
		return "", fmt.Errorf("%v\n%s\n%s", err, script, b)
	}
	return string(b), nil
}

// check fails t unless the app's version installed is version.
func (d *linuxDistro) check(t *testing.T, version string) {
	t.Helper()
	got, err := d.exec(d.version)
	if err != nil {
		t.Fatalf("%s: %v", d.name, err)
	}
	if got != version {
		t.Errorf("%s installed %q, want %s", d.name, got, version)
	}
}

// upgrade upgrades the app, the repository read again.
func (d *linuxDistro) upgrade(t *testing.T) {
	t.Helper()
	if _, err := d.exec(d.upgradeCmd); err != nil {
		t.Fatalf("%s: %v", d.name, err)
	}
}
