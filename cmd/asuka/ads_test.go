package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// asuka.toml's [ads] gives an app of platform/ads its AdMob app IDs:
// Info.plist's GADApplicationIdentifier with Google's SKAdNetwork
// identifiers (and the app's more), and the manifest's meta-data.
func TestAdsConfig(t *testing.T) {
	dir := t.TempDir()
	os.WriteFile(filepath.Join(dir, "asuka.toml"), []byte(`[ads]
ios = "ca-app-pub-3940256099942544~1458002511"
android = "ca-app-pub-3940256099942544~3347511713"
skAdNetworks = ["abcd1234.skadnetwork", "cstr6suwn9.skadnetwork"]
`), 0o644)
	c, err := loadConfig(dir)
	if err != nil {
		t.Fatal(err)
	}
	importStd(t, c, "", "ads")

	m := c.androidManifest(false)
	want := `<meta-data android:name="com.google.android.gms.ads.APPLICATION_ID" android:value="ca-app-pub-3940256099942544~3347511713" />`
	if strings.Count(m, want) != 1 {
		t.Errorf("the manifest hasn't the app ID once:\n%s", m)
	}
	// Again (asuka dev's next save): once.
	c.permissionsFor([]string{adsPkg})
	if m2 := c.androidManifest(false); m2 != m {
		t.Errorf("a second time:\n%s", m2)
	}

	plist := plistKeys(c.plistExtras())
	for _, want := range []string{
		"<key>GADApplicationIdentifier</key>\n<string>ca-app-pub-3940256099942544~1458002511</string>",
		"<key>SKAdNetworkItems</key>\n<array>\n\t<dict>\n\t\t<key>SKAdNetworkIdentifier</key>\n\t\t<string>cstr6suwn9.skadnetwork</string>",
		"<string>abcd1234.skadnetwork</string>",
	} {
		if !strings.Contains(plist, want) {
			t.Errorf("Info.plist has no %s:\n%s", want, plist)
		}
	}
	if n := strings.Count(plist, "<key>SKAdNetworkIdentifier</key>"); n != len(adsNetworks)+1 {
		t.Errorf("%d SKAdNetwork identifiers, want Google's %d and the app's one", n, len(adsNetworks))
	}
	for _, p := range []string{"ios", "android"} {
		if err := c.adsCheck(p); err != nil {
			t.Errorf("%s: %v", p, err)
		}
	}

	// Without the package: nothing, and builds need no ID.
	none, err := loadConfig(dir)
	if err != nil {
		t.Fatal(err)
	}
	importStd(t, none, "")
	if m := none.androidManifest(false); strings.Contains(m, "gms.ads") {
		t.Errorf("without the package:\n%s", m)
	}
	if p := plistKeys(none.plistExtras()); strings.Contains(p, "GADApplicationIdentifier") || strings.Contains(p, "SKAdNetwork") {
		t.Errorf("without the package:\n%s", p)
	}
	if err := none.adsCheck("ios"); err != nil {
		t.Error(err)
	}

	// The package without [ads]: a build fails, saying what to write.
	var bare appConfig
	importStd(t, &bare, "", "ads")
	if m := bare.androidManifest(false); strings.Contains(m, "gms.ads") {
		t.Errorf("without [ads]:\n%s", m)
	}
	for p, sample := range adsSampleApp {
		err := bare.adsCheck(p)
		if err == nil || !strings.Contains(err.Error(), "ads."+p) || !strings.Contains(err.Error(), sample) {
			t.Errorf("%s without [ads]: %v, want an error naming ads.%s and Google's sample app ID", p, err, p)
		}
	}

	for toml, want := range map[string]string{
		"[ads]\nios = \"ca-app-pub-3940256099942544/2435281174\"": `ios "ca-app-pub-3940256099942544/2435281174" isn't an AdMob app ID`,
		"[ads]\nandroid = \"pub-123\"":                            `android "pub-123"`,
		"[ads]\nskAdNetworks = [\"Google\"]":                      `skAdNetworks: "Google"`,
	} {
		os.WriteFile(filepath.Join(dir, "asuka.toml"), []byte(toml), 0o644)
		if _, err := loadConfig(dir); err == nil || !strings.Contains(err.Error(), want) {
			t.Errorf("%s: %v, want an error saying %s", toml, err, want)
		}
	}
}
