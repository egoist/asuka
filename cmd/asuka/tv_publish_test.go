package main

import (
	"os"
	"os/exec"
	"path/filepath"
	"slices"
	"strings"
	"testing"
)

// asuka publish --platform tvos: App Store Connect's flags, as iOS's;
// Google Play's and the desktops' refused.
func TestPublishTVOSFlags(t *testing.T) {
	for _, c := range []struct {
		args []string
		err  string
	}{
		{[]string{"-p", "tvos"}, ""},
		{[]string{"-p", "tvos", "--groups", "Friends", "--notes", "New", "--review", "--release", "manual", "--profile", "TV Store"}, ""},
		{[]string{"-p", "tvos", "--file", "dist/tvos/Notes.ipa"}, ""},
		{[]string{"-p", "tvos", "--no-wait", "--build", "12"}, ""},
		{[]string{"-p", "tvos", "--file", "Notes.aab"}, "a .ipa"},
		{[]string{"-p", "tvos", "--track", "beta"}, "--track is for --platform android"},
		{[]string{"-p", "tvos", "--rollout", "0.1"}, "--rollout is for --platform android"},
		{[]string{"-p", "tvos", "--sign", "Developer ID Application: X"}, "--sign is for --platform mac, windows or linux"},
		{[]string{"-p", "tvos", "--version", "2.0"}, "--version is for --platform mac, windows or linux"},
	} {
		var o publishOptions
		fs := publishFlags(&o)
		if err := fs.parse(c.args); err != nil {
			t.Fatalf("%q: %v", c.args, err)
		}
		err := o.check(fs.set)
		if c.err == "" && err != nil || c.err != "" && (err == nil || !strings.Contains(err.Error(), c.err)) {
			t.Errorf("%q: %v, want %q", c.args, err, c.err)
		}
	}
	if ascPlatform(platformTVOS) != "TV_OS" || ascPlatform(platformIOS) != "IOS" || ascPlatform(platformMac) != "MAC_OS" {
		t.Error("App Store Connect's platforms")
	}
}

// An Apple TV build goes up as an iPhone's does, as the app's tvOS
// platform: its upload, builds, versions and review App Store Connect's
// TV_OS ones, its TestFlight groups the app's.
func TestPublishTVOS(t *testing.T) {
	c, f := testASC(t)
	c.platform = ascPlatform(platformTVOS)
	f.versions = append(f.versions, res("appStoreVersions", "tv10", map[string]any{"versionString": "1.0", "appVersionState": "READY_FOR_DISTRIBUTION", "platform": "TV_OS"}))
	app := testApp(t)
	var built int
	build, _ := fakeBuild(t, 2500, ".ipa", &built)
	o := &publishOptions{groups: "friends", review: true}
	if err := publishIOS(c, app, o, map[string]string{"": "On the TV."}, build); err != nil {
		t.Fatalf("%v\nrequests: %v", err, f.log)
	}
	if built != 9 {
		t.Fatalf("built %d, want 9", built)
	}
	if a := f.attrs("POST /v1/buildUploads", 0); a["platform"] != "TV_OS" || a["cfBundleVersion"] != "9" {
		t.Fatalf("buildUploads %v", a)
	}
	if a := f.attrs("POST /v1/buildUploadFiles", 0); a["uti"] != "com.apple.ipa" {
		t.Fatalf("buildUploadFiles %v", a)
	}
	if a := f.attrs("POST /v1/appStoreVersions", 0); a["platform"] != "TV_OS" || a["versionString"] != "1.2" {
		t.Fatalf("version %v", a)
	}
	if a := f.attrs("POST /v1/reviewSubmissions", 0); a["platform"] != "TV_OS" {
		t.Fatalf("review submission %v", a)
	}
	if f.count("POST /v1/betaAppReviewSubmissions") != 1 {
		t.Fatal("no beta review for the external group")
	}
	var tv, ios int
	for _, q := range f.queries {
		if strings.Contains(q, "TV_OS") {
			tv++
		}
		if strings.Contains(q, "=IOS") {
			ios++
		}
	}
	if tv == 0 || ios != 0 {
		t.Fatalf("queries for tvOS %d, for iOS %d: %v", tv, ios, f.queries)
	}
}

// An app whose record has no tvOS platform: told to add it, before
// anything is built.
func TestPublishTVOSNoPlatform(t *testing.T) {
	c, f := testASC(t)
	c.platform = "TV_OS"
	for _, v := range f.versions {
		v["attributes"].(map[string]any)["platform"] = "IOS"
	}
	var built int
	build, _ := fakeBuild(t, 10, ".ipa", &built)
	err := publishIOS(c, testApp(t), &publishOptions{}, nil, build)
	if err == nil || !strings.Contains(err.Error(), "no tvOS platform on App Store Connect") || !strings.Contains(err.Error(), "Add Platform") || built != 0 {
		t.Fatalf("no tvOS platform: %v (built %d)", err, built)
	}
	if f.count("POST /v1/buildUploads") != 0 {
		t.Fatal("uploaded")
	}
	// iOS's record needs no look: it's an iPhone app's.
	c, f = testASC(t)
	for _, v := range f.versions {
		v["attributes"].(map[string]any)["platform"] = "IOS"
	}
	if err := publishIOS(c, testApp(t), &publishOptions{noWait: true}, nil, build); err != nil {
		t.Fatal(err)
	}
}

// --file: an .ipa of the platform asked for; an iPhone's isn't an Apple
// TV's, nor the other way.
func TestPublishTVOSFile(t *testing.T) {
	dir := t.TempDir()
	tvIPA, phoneIPA := filepath.Join(dir, "tv.ipa"), filepath.Join(dir, "phone.ipa")
	writeIPA(t, tvIPA, map[string]any{"CFBundleIdentifier": "com.example.notes", "CFBundleShortVersionString": "1.2", "CFBundleVersion": "42",
		"UIDeviceFamily": []int{3}, "DTPlatformName": "appletvos", "CFBundleSupportedPlatforms": []string{"AppleTVOS"}})
	writeIPA(t, phoneIPA, map[string]any{"CFBundleIdentifier": "com.example.notes", "CFBundleShortVersionString": "1.2", "CFBundleVersion": "42",
		"UIDeviceFamily": []int{1, 2}, "DTPlatformName": "iphoneos"})
	if info, err := readIPAInfo(tvIPA); err != nil || info.sdk != "appletvos" {
		t.Fatalf("%+v %v", info, err)
	}
	never := func(*asukaApp, *buildOptions) (string, error) { t.Fatal("built"); return "", nil }

	c, f := testASC(t)
	c.platform = "TV_OS"
	if err := publishIOS(c, testApp(t), &publishOptions{file: tvIPA, noWait: true}, nil, never); err != nil {
		t.Fatal(err)
	}
	if a := f.attrs("POST /v1/buildUploads", 0); a["platform"] != "TV_OS" || a["cfBundleVersion"] != "42" {
		t.Fatalf("%v", a)
	}
	err := publishIOS(c, testApp(t), &publishOptions{file: phoneIPA, noWait: true}, nil, never)
	if err == nil || !strings.Contains(err.Error(), "an iPhone and iPad app") || !strings.Contains(err.Error(), "asuka build --platform tvos") {
		t.Fatalf("an iPhone's .ipa for tvOS: %v", err)
	}
	c, _ = testASC(t)
	err = publishIOS(c, testApp(t), &publishOptions{file: tvIPA, noWait: true}, nil, never)
	if err == nil || !strings.Contains(err.Error(), "an Apple TV app") || !strings.Contains(err.Error(), "--platform ios") {
		t.Fatalf("an Apple TV's .ipa for iOS: %v", err)
	}
	// One that doesn't say what it was built with goes up as given.
	if err := checkIPAPlatform("x.ipa", "", "TV_OS"); err != nil {
		t.Fatal(err)
	}
}

// A device build's Info.plist is an Apple TV app's, with the keys App
// Store validation checks: tvOS's SDK and platform, the TV's device
// family and minimum, and no iPhone's orientations.
func TestTVOSReleaseInfoPlist(t *testing.T) {
	if err := exec.Command("xcrun", "--sdk", "appletvos", "--show-sdk-version").Run(); err != nil {
		t.Skip("no tvOS SDK (Xcode's tvOS platform)")
	}
	dir := t.TempDir()
	os.WriteFile(filepath.Join(dir, "asuka.toml"), []byte("name = \"Notes\"\nid = \"com.example.notes\"\nversion = \"1.2\"\n"), 0o644)
	cfg, err := loadConfig(dir)
	if err != nil {
		t.Fatal(err)
	}
	cfg.Build = 9
	m, err := (&iosToolchain{sdkName: "appletvos"}).infoPlist(cfg, false)
	if err != nil {
		t.Fatal(err)
	}
	if m["DTPlatformName"] != "appletvos" || !strings.HasPrefix(m["DTSDKName"].(string), "appletvos") ||
		!slices.Equal(m["CFBundleSupportedPlatforms"].([]string), []string{"AppleTVOS"}) {
		t.Errorf("SDK and platform: %v %v %v", m["DTPlatformName"], m["DTSDKName"], m["CFBundleSupportedPlatforms"])
	}
	if !slices.Equal(m["UIDeviceFamily"].([]int), []int{3}) || m["MinimumOSVersion"] != tvosMin || m["CFBundleVersion"] != "9" {
		t.Errorf("devices, minimum, build: %v %v %v", m["UIDeviceFamily"], m["MinimumOSVersion"], m["CFBundleVersion"])
	}
	for _, k := range []string{"UISupportedInterfaceOrientations", "UISupportedInterfaceOrientations~ipad", "CADisableMinimumFrameDurationOnPhone"} {
		if _, ok := m[k]; ok {
			t.Errorf("an iPhone's %s", k)
		}
	}
	if m["ITSAppUsesNonExemptEncryption"] != false {
		t.Errorf("export compliance: %v", m["ITSAppUsesNonExemptEncryption"])
	}
}
