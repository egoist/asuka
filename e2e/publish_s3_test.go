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
	iupdate "github.com/egoist/asuka/internal/update"
)

// TestPublishLinuxS3 publishes e2e/testdata/updater with asuka publish
// --platform linux to a real S3-compatible server, the bucket of
// ASUKA_E2E_S3 (its endpoint and bucket: http://127.0.0.1:9000/pkgs, a
// MinIO's, whose objects anyone may read, as a CDN serves them), with
// AWS_ACCESS_KEY_ID and AWS_SECRET_ACCESS_KEY: 1.0.0, then 1.0.1, each
// its update (updates.s3) and its apt and dnf repository
// (linux.repository.s3), under ASUKA_E2E_S3_PREFIX (default a new one).
// The update's manifest has the AppImage's delta; apps installed from
// 1.0.0's tarball and AppImage update themselves from the bucket. Then
// 1.0.2 and 1.0.3 are published to the repository at once, which the
// bucket's conditional writes keep both of. With ASUKA_E2E_S3_NOLIST_KEY
// and _SECRET, credentials without s3:ListBucket, a publish to a new
// repository fails saying so (Amazon S3's 403), or goes on without
// pruning (MinIO's 404).
//
//	ASUKA_E2E_S3=http://127.0.0.1:9000/pkgs xvfb-run -a dbus-run-session go test -tags e2e ./e2e -run TestPublishLinuxS3
func TestPublishLinuxS3(t *testing.T) {
	if runtime.GOOS != "linux" {
		t.Skip("Linux")
	}
	s3 := os.Getenv("ASUKA_E2E_S3")
	if s3 == "" {
		t.Skip("set ASUKA_E2E_S3 to an S3 endpoint and bucket (http://127.0.0.1:9000/pkgs), and its credentials")
	}
	endpoint, bucket := s3[:strings.LastIndex(s3, "/")], s3[strings.LastIndex(s3, "/")+1:]
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
	env := []string{"ASUKA_UPDATE_PRIVATE_KEY=" + base64.StdEncoding.EncodeToString(priv), "ASUKA_LINUX_GPG_KEY=" + filepath.Join(keys, "asuka-linux.key")}
	for _, e := range os.Environ() {
		if !strings.HasPrefix(e, "ASUKA_UPDATE_") && !strings.HasPrefix(e, "ASUKA_LINUX_GPG_") {
			env = append(env, e)
		}
	}
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

	// Credentials without s3:ListBucket: Amazon S3 answers 403 for what
	// isn't there yet, and publish stops saying why; MinIO answers 404, and
	// the publish goes on, keeping old by-hash indexes it can't list.
	if key := os.Getenv("ASUKA_E2E_S3_NOLIST_KEY"); key != "" {
		var nolist []string
		for _, e := range env {
			if !strings.HasPrefix(e, "AWS_ACCESS_KEY_ID=") && !strings.HasPrefix(e, "AWS_SECRET_ACCESS_KEY=") {
				nolist = append(nolist, e)
			}
		}
		nolist = append(nolist, "AWS_ACCESS_KEY_ID="+key, "AWS_SECRET_ACCESS_KEY="+os.Getenv("ASUKA_E2E_S3_NOLIST_SECRET"))
		_, err := publish(repoOnly, "1.0.4", nolist, "--package", "deb,rpm", "--repo", "s3://"+bucket+"/"+prefix+"/repo-nolist")
		out, _ := logs.Load("1.0.4")
		switch {
		case err != nil && strings.Contains(err.Error(), "403") && strings.Contains(err.Error(), "s3:ListBucket"):
			t.Log("without s3:ListBucket: refused, as Amazon S3 does")
		case err == nil && strings.Contains(out.(string), "keeping the old by-hash indexes") && strings.Contains(out.(string), "listing needs the s3:ListBucket permission"):
			t.Log("without s3:ListBucket: published, the old by-hash indexes kept, as MinIO answers 404")
		default:
			t.Errorf("published without s3:ListBucket: %v\n%s", err, out)
		}
	}
}
