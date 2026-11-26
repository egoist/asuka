package main

import (
	"crypto/ed25519"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"os"
	"path/filepath"
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
