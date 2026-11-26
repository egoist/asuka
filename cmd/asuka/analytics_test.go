package main

import (
	"encoding/json"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"testing"
)

// googleServicePlist is a GoogleService-Info.plist as the Firebase console
// gives it, for the Apple app id.
func googleServicePlist(id string) string {
	return `<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>API_KEY</key>
	<string>AIzaTest</string>
	<key>GCM_SENDER_ID</key>
	<string>123</string>
	<key>BUNDLE_ID</key>
	<string>` + id + `</string>
	<key>PROJECT_ID</key>
	<string>demo</string>
	<key>IS_ANALYTICS_ENABLED</key>
	<false/>
	<key>GOOGLE_APP_ID</key>
	<string>1:123:ios:abc</string>
</dict>
</plist>
`
}

// asuka.toml's [analytics] and ios.googleServices: checked as it's read,
// the file's path the app directory's.
func TestAnalyticsConfig(t *testing.T) {
	dir := t.TempDir()
	write := func(s string) {
		os.WriteFile(filepath.Join(dir, "asuka.toml"), []byte(s), 0o644)
	}
	write(`id = "com.example.app"
[ios]
googleServices = "GoogleService-Info.plist"
[analytics]
measurementId = "G-ABC123XYZ"
apiSecret = "s3cret"
enabled = false
consent = { adStorage = "denied", adUserData = "granted" }
`)
	c, err := loadConfig(dir)
	if err != nil {
		t.Fatal(err)
	}
	if c.IOS.GoogleServices != filepath.Join(dir, "GoogleService-Info.plist") {
		t.Errorf("ios.googleServices %q", c.IOS.GoogleServices)
	}
	a := c.Analytics
	if a.MeasurementID != "G-ABC123XYZ" || a.APISecret != "s3cret" || a.Enabled == nil || *a.Enabled || a.Consent.AdStorage != "denied" {
		t.Errorf("analytics %+v", a)
	}
	for bad, want := range map[string]string{
		`[analytics]
measurementId = "UA-1234-1"`: "measurementId",
		`[analytics]
apiSecret = "s3cret"`: "apiSecret",
		`[analytics]
measurementId = "G-ABC"
consent = { adStorage = "no" }`: "consent.adStorage",
	} {
		write(bad)
		if _, err := loadConfig(dir); err == nil || !strings.Contains(err.Error(), "asuka.toml: analytics: "+want) {
			t.Errorf("%s: %v", bad, err)
		}
	}
}

// An app of platform/analytics: Firebase from its launch on iOS, no
// screens of its own reported for the app's one view controller or
// activity, and asuka.toml's collection and consent as Firebase's
// defaults on iOS, the Mac and Android. An app without it has none.
func TestAnalyticsExtras(t *testing.T) {
	c := &appConfig{}
	off := false
	c.Analytics.Enabled = &off
	c.Analytics.Consent.AdStorage = "denied"
	c.Analytics.Consent.AnalyticsStorage = "granted"
	importStd(t, c, "ios", "analytics")
	if !c.analyticsOn {
		t.Fatal("not on")
	}
	x := c.plistExtras()
	for k, v := range map[string]any{
		"FirebaseAutomaticScreenReportingEnabled":          false,
		"FIREBASE_ANALYTICS_COLLECTION_ENABLED":            false,
		"GOOGLE_ANALYTICS_DEFAULT_ALLOW_AD_STORAGE":        false,
		"GOOGLE_ANALYTICS_DEFAULT_ALLOW_ANALYTICS_STORAGE": true,
	} {
		if x[k] != v {
			t.Errorf("Info.plist's %s: %v, want %v", k, x[k], v)
		}
	}
	if _, ok := x["GOOGLE_ANALYTICS_DEFAULT_ALLOW_AD_USER_DATA"]; ok {
		t.Error("a consent asuka.toml doesn't say")
	}
	if l, _ := x["AsukaLaunch"].([]string); !slices.Contains(l, "AsukaAnalyticsLaunch") {
		t.Errorf("launch classes %v", x["AsukaLaunch"])
	}
	m := c.androidManifest(false)
	for _, want := range []string{
		`<meta-data android:name="google_analytics_automatic_screen_reporting_enabled" android:value="false" />`,
		`<meta-data android:name="firebase_analytics_collection_enabled" android:value="false" />`,
		`<meta-data android:name="google_analytics_default_allow_ad_storage" android:value="false" />`,
		`<meta-data android:name="google_analytics_default_allow_analytics_storage" android:value="true" />`,
	} {
		if !strings.Contains(m, want) {
			t.Errorf("the manifest has no %s:\n%s", want, m)
		}
	}
	// Again (asuka dev's next save): once each.
	c.permissionsFor([]string{analyticsPkg})
	if m2 := c.androidManifest(false); m2 != m {
		t.Errorf("a second time:\n%s", m2)
	}

	none := &appConfig{}
	none.Analytics = c.Analytics
	importStd(t, none, "ios", "network")
	if none.analyticsOn || len(none.analyticsPlist()) > 0 || strings.Contains(none.androidManifest(false), "analytics") {
		t.Error("an app without the package has its settings")
	}
	if _, ok := none.plistExtras()["FirebaseAutomaticScreenReportingEnabled"]; ok {
		t.Error("an app without the package has Firebase's keys")
	}
}

// The GoogleService-Info.plist goes into an iOS app's bundle and a Mac
// app's Resources, once it's checked to be the app's.
func TestCopyGoogleServices(t *testing.T) {
	dir, bundle := t.TempDir(), t.TempDir()
	path := filepath.Join(dir, "GoogleService-Info.plist")
	os.WriteFile(path, []byte(googleServicePlist("com.example.app")), 0o644)
	c := &appConfig{}
	c.IOS.GoogleServices = path
	// Without the package: nothing.
	if err := c.copyGoogleServices(bundle, "com.example.app"); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(bundle, googleServiceName)); err == nil {
		t.Fatal("copied for an app without platform/analytics")
	}
	c.analyticsOn = true
	if err := c.copyGoogleServices(bundle, "com.example.app"); err != nil {
		t.Fatal(err)
	}
	if b, _ := os.ReadFile(filepath.Join(bundle, googleServiceName)); string(b) != googleServicePlist("com.example.app") {
		t.Fatalf("copied %q", b)
	}
	res := filepath.Join(t.TempDir(), "Contents", "Resources")
	if err := c.copyGoogleServices(res, "com.example.app"); err != nil {
		t.Fatal(err)
	}
	if err := c.copyGoogleServices(bundle, "com.example.other"); err == nil || !strings.Contains(err.Error(), "is for the Apple app com.example.app") {
		t.Errorf("another app's: %v", err)
	}
	os.WriteFile(path, []byte(strings.Replace(googleServicePlist("com.example.app"), "GOOGLE_APP_ID", "OTHER", 1)), 0o644)
	if err := c.copyGoogleServices(bundle, "com.example.app"); err == nil || !strings.Contains(err.Error(), "GOOGLE_APP_ID") {
		t.Errorf("no GOOGLE_APP_ID: %v", err)
	}
	os.WriteFile(path, []byte("{}"), 0o644)
	if err := c.copyGoogleServices(bundle, "com.example.app"); err == nil {
		t.Error("not a property list: no error")
	}
	c.IOS.GoogleServices = filepath.Join(dir, "missing.plist")
	if err := c.copyGoogleServices(bundle, "com.example.app"); err == nil {
		t.Error("a missing file: no error")
	}
}

// google-services.json becomes Android's resources for an app of
// platform/analytics, as for one of platform/push.
func TestAnalyticsAndroidRes(t *testing.T) {
	path := filepath.Join(t.TempDir(), "google-services.json")
	os.WriteFile(path, []byte(`{"project_info": {"project_number": "123", "project_id": "demo"},
  "client": [{"client_info": {"mobilesdk_app_id": "1:123:android:abc", "android_client_info": {"package_name": "com.example.app"}}, "api_key": [{"current_key": "KEY"}]}]}`), 0o644)
	c := &appConfig{ID: "com.example.app"}
	c.Android.GoogleServices = path
	importStd(t, c, "", "network")
	res := t.TempDir()
	if _, err := androidRes(c, res); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(res, "values", "asuka_firebase.xml")); err == nil {
		t.Fatal("Firebase's resources for an app without Firebase")
	}
	importStd(t, c, "", "analytics")
	if wrote, err := androidRes(c, res); err != nil || !wrote {
		t.Fatal(wrote, err)
	}
	if b, _ := os.ReadFile(filepath.Join(res, "values", "asuka_firebase.xml")); !strings.Contains(string(b), "1:123:android:abc") {
		t.Errorf("asuka_firebase.xml: %s", b)
	}
}

// The web's page and the desktops' asuka-app.json carry the stream: the
// API secret only for the desktops (anyone reads a page),
// ASUKA_ANALYTICS_API_SECRET's over asuka.toml's; asuka dev's runs send
// to DebugView.
func TestAnalyticsSettings(t *testing.T) {
	c := &appConfig{}
	c.Analytics.MeasurementID = "G-ABC123"
	c.Analytics.APISecret = "toml"
	off := false
	c.Analytics.Enabled = &off
	c.Analytics.Consent.AdUserData = "denied"
	if s := c.analyticsSettings(platformLinux, false); s != nil {
		t.Errorf("without the package: %v", s)
	}
	c.analyticsOn = true
	web := c.analyticsSettings(platformWeb, false)
	if web["analytics.measurementId"] != "G-ABC123" || web["analytics.apiSecret"] != "" || web["analytics.enabled"] != "false" ||
		web["analytics.consent.adUserData"] != "denied" || web["analytics.debug"] != "" {
		t.Errorf("the web's: %v", web)
	}
	t.Setenv(analyticsSecretEnv, "env")
	desk := c.analyticsSettings(platformWindows, true)
	if desk["analytics.apiSecret"] != "env" || desk["analytics.debug"] != "true" {
		t.Errorf("the desktop's: %v", desk)
	}

	app := &asukaApp{name: "notes", cfg: c}
	b, _ := json.Marshal(app.hostConfig(platformLinux, "linux-amd64", false))
	if !strings.Contains(string(b), `"settings":{"analytics.apiSecret":"env",`) || strings.Contains(string(b), "analytics.debug") {
		t.Errorf("asuka-app.json: %s", b)
	}
	if b, _ := json.Marshal(app.hostConfig(platformMac, "darwin-arm64", false)); strings.Contains(string(b), "settings") {
		t.Errorf("the Mac's (Firebase's): %s", b)
	}
}
