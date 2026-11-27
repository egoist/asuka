package main

import (
	"crypto/ed25519"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"

	iupdate "github.com/egoist/asuka/internal/update"
	"github.com/egoist/asuka/lang/target"
)

// An over-the-air update (RFC 0006): its manifest names the build's
// fingerprint, which a change of the app's code keeps and a new asset or
// another platform changes; its module is signed with the update key.
func TestOTAUpdate(t *testing.T) {
	pub, priv := testKeys(t)
	t.Setenv("ASUKA_UPDATE_PUBLIC_KEY", "")
	t.Setenv("ASUKA_UPDATE_URL", "")
	t.Setenv("ASUKA_UPDATE_PRIVATE_KEY", priv)
	dir := t.TempDir()
	writeFiles(t, dir, map[string]string{
		"asuka.toml": "name = \"OTA\"\nid = \"dev.asuka.ota\"\nversion = \"1.0.0\"\n\n[updates]\npublicKey = \"" + pub + "\"\nurl = \"https://example.com/ota\"\n",
		"app.asuka":  "app { Text(\"A\") }\n",
	})
	fingerprint := func(platform string) string {
		t.Helper()
		app, err := loadApp(dir, platform)
		if err != nil {
			t.Fatal(err)
		}
		tp := target.IOS
		if platform == platformAndroid {
			tp = target.Android
		}
		m, err := app.compile(tp)
		if err != nil {
			t.Fatal(err)
		}
		u, err := app.otaUpdates(platform, m)
		if err != nil || u == nil {
			t.Fatalf("updates %v: %v", u, err)
		}
		if want := "https://example.com/ota/update-" + platform + "-" + u.Fingerprint[:16] + ".json"; u.Feed != want || u.Version != "1.0.0" || u.Built <= 0 {
			t.Errorf("feed %q (want %q), version %q, built %d", u.Feed, want, u.Version, u.Built)
		}
		return u.Fingerprint
	}
	built := fingerprint(platformIOS)

	app, err := loadApp(dir, platformIOS)
	if err != nil {
		t.Fatal(err)
	}
	out := t.TempDir()
	file, manifest, err := writeOTAUpdate(app, platformIOS, out)
	if err != nil {
		t.Fatal(err)
	}
	var m iupdate.Manifest
	b, _ := os.ReadFile(manifest)
	if err := json.Unmarshal(b, &m); err != nil {
		t.Fatal(err)
	}
	if m.Fingerprint != built || m.Update <= 0 || m.Version != "1.0.0" || m.URL != "https://example.com/ota/"+filepath.Base(file) {
		t.Errorf("manifest %+v: want the build's fingerprint %s", m, built)
	}
	if filepath.Base(manifest) != "update-ios-"+built[:16]+".json" {
		t.Errorf("manifest's name %s", filepath.Base(manifest))
	}
	data, _ := os.ReadFile(file)
	sum := sha256.Sum256(data)
	key, _ := base64.StdEncoding.DecodeString(pub)
	sig, _ := base64.StdEncoding.DecodeString(m.Signature)
	if !ed25519.Verify(ed25519.PublicKey(key), sum[:], sig) || len(data) == 0 || int64(len(data)) != m.Size {
		t.Errorf("the module isn't signed with the update key (%d bytes)", len(data))
	}

	// The app's code changed: the same builds take it.
	writeFiles(t, dir, map[string]string{"app.asuka": "app { Text(\"B\") }\n"})
	if got := fingerprint(platformIOS); got != built {
		t.Errorf("a change of code changed the fingerprint")
	}
	// Android's builds are others; a new asset needs a new build.
	if fingerprint(platformAndroid) == built {
		t.Errorf("Android's fingerprint is iOS's")
	}
	writeFiles(t, dir, map[string]string{"assets/logo.txt": "logo"})
	if fingerprint(platformIOS) == built {
		t.Errorf("a new asset kept the fingerprint")
	}
}

// otaFingerprintOf is the fingerprint of dir's builds for platform, and
// its feed.
func otaFingerprintOf(t *testing.T, dir, platform string) (fp, feed string) {
	t.Helper()
	app, err := loadApp(dir, platform)
	if err != nil {
		t.Fatal(err)
	}
	tp := target.IOS
	if platform == platformAndroid {
		tp = target.Android
	}
	m, err := app.compile(tp)
	if err != nil {
		t.Fatal(err)
	}
	u, err := app.otaUpdates(platform, m)
	if err != nil || u == nil {
		t.Fatalf("updates %v: %v", u, err)
	}
	return u.Fingerprint, u.Feed
}

// An Apple TV build's updates are its own: its fingerprint is of tvOS's
// native side (the host's parts tvOS has), not the iPhone build's, and
// its manifest is named for tvOS, so neither takes the other's.
func TestOTAFingerprintTVOS(t *testing.T) {
	pub, _ := testKeys(t)
	t.Setenv("ASUKA_UPDATE_PUBLIC_KEY", "")
	t.Setenv("ASUKA_UPDATE_URL", "")
	dir := t.TempDir()
	toml := "name = \"TV\"\nid = \"dev.asuka.tv\"\nversion = \"1.0.0\"\n\n[updates]\npublicKey = \"" + pub + "\"\nurl = \"https://example.com/ota\"\n"
	writeFiles(t, dir, map[string]string{"asuka.toml": toml, "app.asuka": "app { Text(\"A\") }\n"})
	phone, _ := otaFingerprintOf(t, dir, platformIOS)
	tv, feed := otaFingerprintOf(t, dir, platformTVOS)
	if tv == phone {
		t.Fatal("tvOS's fingerprint is iOS's")
	}
	if want := "https://example.com/ota/update-tvos-" + tv[:16] + ".json"; feed != want {
		t.Errorf("tvOS's feed %q, want %q", feed, want)
	}
	// A change of code: the same builds of each take it.
	writeFiles(t, dir, map[string]string{"app.asuka": "app { Text(\"B\") }\n"})
	if p, _ := otaFingerprintOf(t, dir, platformIOS); p != phone {
		t.Error("a change of code changed iOS's fingerprint")
	}
	if p, _ := otaFingerprintOf(t, dir, platformTVOS); p != tv {
		t.Error("a change of code changed tvOS's fingerprint")
	}
	// A Video needs the host's player (ASUKA_VIDEO), which both builds
	// have only when the app shows one: an update that starts to is for
	// the next build of each.
	writeFiles(t, dir, map[string]string{"app.asuka": "app { Video(\"https://example.com/a.mp4\") }\n"})
	if p, _ := otaFingerprintOf(t, dir, platformIOS); p == phone {
		t.Error("a Video kept iOS's fingerprint")
	}
	if p, _ := otaFingerprintOf(t, dir, platformTVOS); p == tv {
		t.Error("a Video kept tvOS's fingerprint")
	}
	// A WebView's WebKit is iOS's alone: tvOS's host has none, with or
	// without one.
	writeFiles(t, dir, map[string]string{"app.asuka": "app { WebView(\"https://example.com\") }\n"})
	if p, _ := otaFingerprintOf(t, dir, platformIOS); p == phone {
		t.Error("a WebView kept iOS's fingerprint")
	}
	if p, _ := otaFingerprintOf(t, dir, platformTVOS); p != tv {
		t.Error("a WebView changed tvOS's fingerprint, whose host has none")
	}

	// What a tvOS build goes without (widgets, shortcuts) is out of its
	// fingerprint: the build drops it before it asks, the update doesn't.
	writeFiles(t, dir, map[string]string{
		"asuka.toml": toml + "\n[[widgets]]\nkind = \"today\"\n\n[[shortcuts]]\nid = \"new\"\ntitle = \"New\"\nurl = \"tv://new\"\n",
		"app.asuka":  "app { Text(\"A\") }\n",
	})
	app, err := loadApp(dir, platformTVOS)
	if err != nil {
		t.Fatal(err)
	}
	m, err := app.compile(target.IOS)
	if err != nil {
		t.Fatal(err)
	}
	before, err := app.otaFingerprint(platformTVOS, m)
	if err != nil {
		t.Fatal(err)
	}
	app.cfg.dropTVExtensions()
	after, err := app.otaFingerprint(platformTVOS, m)
	if err != nil {
		t.Fatal(err)
	}
	if before != after || before != tv {
		t.Errorf("tvOS's fingerprint with widgets %s, without %s, want %s", before[:16], after[:16], tv[:16])
	}
}

// asuka build --platform tvos --update writes tvOS's update, in
// dist/tvos/updates, for tvOS's builds; iOS's goes in dist/updates.
func TestBuildOTAUpdateTVOS(t *testing.T) {
	pub, priv := testKeys(t)
	t.Setenv("ASUKA_UPDATE_PUBLIC_KEY", "")
	t.Setenv("ASUKA_UPDATE_URL", "")
	t.Setenv("ASUKA_UPDATE_PRIVATE_KEY", priv)
	dir := t.TempDir()
	writeFiles(t, dir, map[string]string{
		"asuka.toml": "name = \"TV\"\nid = \"dev.asuka.tv\"\nversion = \"1.0.0\"\n\n[updates]\npublicKey = \"" + pub + "\"\nurl = \"https://example.com/ota\"\n",
		"app.asuka":  "app { Text(\"A\") }\n",
	})
	for _, c := range []struct{ platform, dir string }{{platformTVOS, "dist/tvos/updates"}, {platformIOS, "dist/updates"}} {
		if err := cmdBuild([]string{"--platform", c.platform, "--update", dir}); err != nil {
			t.Fatalf("%s: %v", c.platform, err)
		}
		fp, _ := otaFingerprintOf(t, dir, c.platform)
		manifest := filepath.Join(dir, filepath.FromSlash(c.dir), "update-"+c.platform+"-"+fp[:16]+".json")
		b, err := os.ReadFile(manifest)
		if err != nil {
			t.Fatalf("%s: %v", c.platform, err)
		}
		var m iupdate.Manifest
		if err := json.Unmarshal(b, &m); err != nil || m.Fingerprint != fp {
			t.Errorf("%s: manifest %+v (%v): want the fingerprint %s", c.platform, m, err, fp)
		}
		if !strings.Contains(m.URL, "-"+c.platform+"-"+fp[:16]+"-") {
			t.Errorf("%s: the module %s isn't named for its platform", c.platform, m.URL)
		}
	}
	if err := cmdBuild([]string{"--platform", platformTVOS, "--xcframework", dir}); err == nil || !strings.Contains(err.Error(), "--xcframework is iOS's") {
		t.Errorf("tvOS's --xcframework: %v", err)
	}
}

// asuka publish --platform tvos --update signs tvOS's update and uploads
// it as iOS's is: the module, then its manifest, update-tvos-<fp>.json,
// to the version's release.
func TestPublishOTAUpdateTVOS(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("a shell script as gh")
	}
	pub, priv := testKeys(t)
	t.Setenv("ASUKA_UPDATE_PUBLIC_KEY", "")
	t.Setenv("ASUKA_UPDATE_URL", "")
	t.Setenv("ASUKA_UPDATE_PRIVATE_KEY", priv)
	dir := t.TempDir()
	writeFiles(t, dir, map[string]string{
		"asuka.toml": "name = \"TV\"\nid = \"dev.asuka.tv\"\nversion = \"1.2.0\"\n\n[updates]\npublicKey = \"" + pub + "\"\ngithub = \"me/tv\"\n",
		"app.asuka":  "app { Text(\"A\") }\n",
	})
	bin := t.TempDir()
	log := filepath.Join(bin, "log")
	script := "#!/bin/sh\necho \"$*\" >> " + log + "\ncase \"$1 $2\" in \"release view\") exit 1;; esac\n"
	if err := os.WriteFile(filepath.Join(bin, "gh"), []byte(script), 0o755); err != nil {
		t.Fatal(err)
	}
	defer func(c string) { ghCommand = c }(ghCommand)
	ghCommand = filepath.Join(bin, "gh")
	if err := cmdPublish([]string{"--platform", platformTVOS, "--update", dir}); err != nil {
		t.Fatal(err)
	}
	fp, feed := otaFingerprintOf(t, dir, platformTVOS)
	if want := "https://github.com/me/tv/releases/latest/download/update-tvos-" + fp[:16] + ".json"; feed != want {
		t.Errorf("the build's feed %q, want %q", feed, want)
	}
	updates := filepath.Join(dir, "dist", "tvos", "updates")
	modules, _ := filepath.Glob(filepath.Join(updates, "*-tvos-"+fp[:16]+"-*.asukab"))
	manifest := filepath.Join(updates, "update-tvos-"+fp[:16]+".json")
	if len(modules) != 1 {
		t.Fatalf("modules %q in %s", modules, updates)
	}
	b, _ := os.ReadFile(log)
	want := "release view v1.2.0 --repo me/tv\n" +
		"release create v1.2.0 --repo me/tv --draft --title v1.2.0 --notes \n" +
		"release upload v1.2.0 --repo me/tv --clobber " + modules[0] + "\n" +
		"release upload v1.2.0 --repo me/tv --clobber " + manifest + "\n" +
		"release edit v1.2.0 --repo me/tv --draft=false\n"
	if string(b) != want {
		t.Errorf("gh ran:\n%s\nwant\n%s", b, want)
	}
	var m iupdate.Manifest
	mb, _ := os.ReadFile(manifest)
	if err := json.Unmarshal(mb, &m); err != nil || m.Fingerprint != fp || m.URL != "https://github.com/me/tv/releases/download/v1.2.0/"+filepath.Base(modules[0]) {
		t.Errorf("manifest %+v (%v)", m, err)
	}
	if phone, _ := otaFingerprintOf(t, dir, platformIOS); phone == fp {
		t.Error("the update's fingerprint is the iPhone build's")
	}
}
