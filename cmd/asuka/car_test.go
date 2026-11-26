package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// carApp is an app of platform/car whose asuka.toml has toml.
func carApp(t *testing.T, toml string, imports ...string) *appConfig {
	t.Helper()
	dir := t.TempDir()
	os.WriteFile(filepath.Join(dir, "asuka.toml"), []byte(toml), 0o644)
	c, err := loadConfig(dir)
	if err != nil {
		t.Fatal(err)
	}
	importStd(t, c, "ios", imports...)
	return c
}

// An audio app: CarPlay's audio entitlement and its scene, whose delegate
// is the package's, in Info.plist (a release's and asuka dev's); on
// Android, the package's media browser and automotive_app_desc.xml's
// media.
func TestCarAudio(t *testing.T) {
	c := carApp(t, "[car]\ncategory = \"audio\"\n", "car")
	if c.entitlements["com.apple.developer.carplay-audio"] != true {
		t.Errorf("entitlements: %v", c.entitlements)
	}
	if !strings.Contains(c.entitlementsFor["com.apple.developer.carplay-audio"], "platform/car") {
		t.Errorf("what the entitlement is for: %q", c.entitlementsFor["com.apple.developer.carplay-audio"])
	}
	plist := plistKeys(c.plistExtras())
	for _, want := range []string{
		"<key>UIApplicationSceneManifest</key>",
		"<key>UIApplicationSupportsMultipleScenes</key>",
		"<key>CPTemplateApplicationSceneSessionRoleApplication</key>",
		"<string>CPTemplateApplicationScene</string>",
		"<string>AsukaCarSceneDelegate</string>",
	} {
		if !strings.Contains(plist, want) {
			t.Errorf("Info.plist has no %s:\n%s", want, plist)
		}
	}
	m := c.androidManifest(false)
	for _, want := range []string{
		`<service android:name="platform.car.AsukaCarMediaService" android:exported="true">`,
		`<action android:name="android.media.browse.MediaBrowserService" />`,
		`<meta-data android:name="com.google.android.gms.car.application" android:resource="@xml/asuka_automotive_app_desc" />`,
	} {
		if !strings.Contains(m, want) {
			t.Errorf("the manifest has no %s:\n%s", want, m)
		}
	}
	if strings.Contains(m, "androidx.car.app.CarAppService") || strings.Contains(m, "minCarApiLevel") {
		t.Errorf("an audio app has no templated service:\n%s", m)
	}
	res := t.TempDir()
	if ok, err := c.androidCarRes(res); err != nil || !ok {
		t.Fatal(ok, err)
	}
	desc, _ := os.ReadFile(filepath.Join(res, "xml", "asuka_automotive_app_desc.xml"))
	if !strings.Contains(string(desc), `<uses name="media" />`) {
		t.Errorf("automotive_app_desc.xml:\n%s", desc)
	}
	// Again (asuka dev's next save): once each.
	c.permissionsFor([]string{carPkg})
	if m2 := c.androidManifest(false); m2 != m {
		t.Errorf("a second time:\n%s", m2)
	}
	// The profile lacks it: a signed build says what it's for.
	ent := filepath.Join(t.TempDir(), "profile.plist")
	os.WriteFile(ent, []byte("<plist><dict></dict></plist>"), 0o644)
	if missing := strings.Join(c.missingEntitlements(ent), "; "); !strings.Contains(missing, "CarPlay's audio apps") {
		t.Errorf("missing: %s", missing)
	}
	// tvOS has no CarPlay.
	c.tvToldDropped = true
	c.dropTVExtensions()
	if c.carScene || c.entitlements["com.apple.developer.carplay-audio"] != nil {
		t.Errorf("a tvOS build keeps CarPlay: %v", c.entitlements)
	}
	if strings.Contains(plistKeys(c.plistExtras()), "CPTemplateApplicationScene") {
		t.Error("a tvOS build has the CarPlay scene")
	}
}

// A templated category: Android Auto's CarAppService with the category's
// intent filter, and the Car App API level its templates need.
func TestCarTemplates(t *testing.T) {
	c := carApp(t, "[car]\ncategory = \"charging\"\nminCarApiLevel = 6\n", "car")
	if c.entitlements["com.apple.developer.carplay-charging"] != true {
		t.Errorf("entitlements: %v", c.entitlements)
	}
	m := c.androidManifest(false)
	for _, want := range []string{
		`<service android:name="platform.car.AsukaCarService" android:exported="true">`,
		`<action android:name="androidx.car.app.CarAppService" />`,
		`<category android:name="androidx.car.app.category.POI" />`,
		`<meta-data android:name="androidx.car.app.minCarApiLevel" android:value="6" />`,
		`android:resource="@xml/asuka_automotive_app_desc"`,
	} {
		if !strings.Contains(m, want) {
			t.Errorf("the manifest has no %s:\n%s", want, m)
		}
	}
	if !strings.Contains(c.automotiveAppDesc(), `<uses name="template" />`) {
		t.Error(c.automotiveAppDesc())
	}

	// The level's default.
	c = carApp(t, "[car]\ncategory = \"communication\"\n", "car")
	m = c.androidManifest(false)
	for _, want := range []string{
		`<category android:name="androidx.car.app.category.MESSAGING" />`,
		`<meta-data android:name="androidx.car.app.minCarApiLevel" android:value="1" />`,
	} {
		if !strings.Contains(m, want) {
			t.Errorf("the manifest has no %s:\n%s", want, m)
		}
	}
	if c.entitlements["com.apple.developer.carplay-communication"] != true {
		t.Errorf("entitlements: %v", c.entitlements)
	}
}

// A category one platform has no apps of: the other gets nothing.
func TestCarOnePlatform(t *testing.T) {
	c := carApp(t, "[car]\ncategory = \"quickFood\"\n", "car")
	if c.entitlements["com.apple.developer.carplay-quick-ordering"] != true {
		t.Errorf("entitlements: %v", c.entitlements)
	}
	if m := c.androidManifest(false); strings.Contains(m, "platform.car.") || strings.Contains(m, "gms.car") {
		t.Errorf("Android Auto has no quick food apps:\n%s", m)
	}
	if ok, _ := c.androidCarRes(t.TempDir()); ok {
		t.Error("automotive_app_desc.xml for no category")
	}

	c = carApp(t, "[car]\ncategory = \"weather\"\n", "car")
	if c.carScene || len(c.entitlements) > 0 && c.carEntitlement() != "" {
		t.Errorf("CarPlay has no weather apps: %v", c.entitlements)
	}
	if strings.Contains(plistKeys(c.plistExtras()), "CPTemplateApplicationScene") {
		t.Error("a CarPlay scene without CarPlay")
	}
	if m := c.androidManifest(false); !strings.Contains(m, `<category android:name="androidx.car.app.category.WEATHER" />`) {
		t.Errorf("the manifest:\n%s", m)
	}
}

// [car] without the package, or the package without [car]: nothing.
func TestCarOff(t *testing.T) {
	for name, c := range map[string]*appConfig{
		"no import": carApp(t, "[car]\ncategory = \"audio\"\n"),
		"no [car]":  carApp(t, "", "car"),
	} {
		if c.carScene || c.carAndroid != "" || c.entitlements["com.apple.developer.carplay-audio"] != nil {
			t.Errorf("%s: %v %q %v", name, c.carScene, c.carAndroid, c.entitlements)
		}
		if strings.Contains(plistKeys(c.plistExtras()), "UISceneConfigurations") {
			t.Errorf("%s: Info.plist has scene configurations", name)
		}
		if m := c.androidManifest(false); strings.Contains(m, "platform.car.AsukaCar") {
			t.Errorf("%s: the manifest has the car's service:\n%s", name, m)
		}
	}
}

func TestCarConfigErrors(t *testing.T) {
	for toml, want := range map[string]string{
		"[car]\ncategory = \"music\"\n":                     `category "music" (want one of audio, charging,`,
		"[car]\ncategory = \"navigation\"\n":                "draws its own map",
		"[car]\ncategory = \"audio\"\nminCarApiLevel = 9\n": "minCarApiLevel 9 (want 1 to 7)",
	} {
		dir := t.TempDir()
		os.WriteFile(filepath.Join(dir, "asuka.toml"), []byte(toml), 0o644)
		_, err := loadConfig(dir)
		if err == nil || !strings.Contains(err.Error(), want) || !strings.Contains(err.Error(), "asuka.toml: car:") {
			t.Errorf("%q: %v, want %s", toml, err, want)
		}
	}
}
