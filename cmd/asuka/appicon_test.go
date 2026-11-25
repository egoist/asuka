package main

import (
	"image"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

const alternateIconsConfig = `id = "com.example.notes"
icon = "icon.png"
scheme = "notes"

[[shortcuts]]
id = "new"
title = "New Note"
url = "notes://new"

[alternateIcons]
dark = "icons/dark.png"
Classic = "icons/classic.png"
`

// alternateIconsApp is an app with two alternate icons, their PNGs made.
func alternateIconsApp(t *testing.T) *asukaApp {
	t.Helper()
	app := linuxTestApp(t, alternateIconsConfig)
	img := image.NewNRGBA(image.Rect(0, 0, 64, 64))
	for i := range img.Pix {
		img.Pix[i] = 200
	}
	os.MkdirAll(filepath.Join(app.dir, "icons"), 0o755)
	for _, p := range []string{app.cfg.Icon, app.cfg.AlternateIcons["dark"], app.cfg.AlternateIcons["Classic"]} {
		if err := writePNG(p, img); err != nil {
			t.Fatal(err)
		}
	}
	return app
}

// asuka.toml's alternate icons: names an Android alias, an iOS icon set
// and a file can have, paths from the app's directory, the app's own icon
// too.
func TestAlternateIconsConfig(t *testing.T) {
	app := alternateIconsApp(t)
	if got := app.cfg.alternateIconNames(); strings.Join(got, ",") != "Classic,dark" {
		t.Errorf("names: %v", got)
	}
	if p := app.cfg.AlternateIcons["dark"]; p != filepath.Join(app.dir, "icons", "dark.png") {
		t.Errorf("dark's path: %s", p)
	}
	for _, bad := range []string{
		"icon = \"icon.png\"\n[alternateIcons]\n\"dark-mode\" = \"x.png\"",
		"icon = \"icon.png\"\n[alternateIcons]\n\"2024\" = \"x.png\"",
		"[alternateIcons]\ndark = \"x.png\"",
	} {
		dir := t.TempDir()
		os.WriteFile(filepath.Join(dir, "asuka.toml"), []byte(bad), 0o644)
		if _, err := loadConfig(dir); err == nil || !strings.Contains(err.Error(), "alternateIcons") {
			t.Errorf("%q: %v", bad, err)
		}
	}
}

// Android's launcher entries are activity aliases, one an icon, the
// app's own enabled; the activity's own entry goes, and the shortcuts
// are the aliases'.
func TestAndroidAlternateIcons(t *testing.T) {
	app := alternateIconsApp(t)
	m := app.cfg.androidManifest(false)
	for _, want := range []string{
		`xmlns:tools="http://schemas.android.com/tools"`,
		`<intent-filter tools:node="remove">`,
		`<activity-alias android:name="dev.asuka.AppIcon" android:targetActivity="dev.asuka.AsukaActivity" android:exported="true" android:enabled="true" android:icon="@mipmap/ic_launcher">`,
		`<activity-alias android:name="dev.asuka.AppIcon.Classic" android:targetActivity="dev.asuka.AsukaActivity" android:exported="true" android:enabled="false" android:icon="@mipmap/ic_launcher_classic">`,
		`<activity-alias android:name="dev.asuka.AppIcon.dark" android:targetActivity="dev.asuka.AsukaActivity" android:exported="true" android:enabled="false" android:icon="@mipmap/ic_launcher_dark">`,
	} {
		if !strings.Contains(m, want) {
			t.Errorf("the manifest has no %s:\n%s", want, m)
		}
	}
	activity := m[strings.Index(m, "<activity "):strings.Index(m, "</activity>")]
	if strings.Contains(activity, "android.app.shortcuts") {
		t.Error("the activity has the shortcuts, which are the launcher's entries'")
	}
	if n := strings.Count(m, `android:resource="@xml/asuka_shortcuts"`); n != 3 {
		t.Errorf("%d aliases have the shortcuts, want 3", n)
	}
	// Without alternates the activity is the launcher's entry, as before.
	plain := linuxTestApp(t, "icon = \"icon.png\"").cfg.androidManifest(false)
	if strings.Contains(plain, "activity-alias") || strings.Contains(plain, "tools:") {
		t.Errorf("an app without alternate icons has aliases:\n%s", plain)
	}

	res := t.TempDir()
	if err := androidIcons(app.cfg, res); err != nil {
		t.Fatal(err)
	}
	for _, f := range []string{"mipmap-xxxhdpi/ic_launcher.png", "mipmap-xxxhdpi/ic_launcher_dark_foreground.png",
		"mipmap-mdpi/ic_launcher_classic.png", "mipmap-anydpi-v26/ic_launcher_dark.xml", "values/asuka_ic_launcher_dark.xml"} {
		if !exists(filepath.Join(res, f)) {
			t.Errorf("no %s", f)
		}
	}
	b, _ := os.ReadFile(filepath.Join(res, "mipmap-anydpi-v26", "ic_launcher_dark.xml"))
	if !strings.Contains(string(b), "@color/asuka_ic_launcher_dark_background") || !strings.Contains(string(b), "@mipmap/ic_launcher_dark_foreground") {
		t.Errorf("dark's adaptive icon:\n%s", b)
	}
}

// Windows: the alternates are icon groups 201 and on, in their names'
// order, which a release's settings list; a player's has none.
func TestWindowsAlternateIcons(t *testing.T) {
	app := alternateIconsApp(t)
	if c := app.hostConfig(platformWindows, "windows-amd64", true); c.AlternateIcons != "Classic,dark" {
		t.Errorf("a release's settings: %q", c.AlternateIcons)
	}
	if c := app.hostConfig(platformWindows, "", false); c.AlternateIcons != "" {
		t.Errorf("the player's settings name icons it hasn't: %q", c.AlternateIcons)
	}
	b, err := winresSyso(app, "amd64")
	if err != nil {
		t.Fatal(err)
	}
	// Groups 201 (0xc9) and 202 (0xca): their directory entries' IDs.
	for _, id := range []string{"\xc9\x00\x00\x00", "\xca\x00\x00\x00"} {
		if !strings.Contains(string(b), id) {
			t.Errorf("no group icon %d in the resources", id[0])
		}
	}
}

// The web page gets each alternate as a small PNG, by name.
func TestWebAlternateIcons(t *testing.T) {
	app := alternateIconsApp(t)
	icons, err := webAlternateIcons(app.cfg)
	if err != nil {
		t.Fatal(err)
	}
	if len(icons) != 2 || !strings.HasPrefix(icons["dark"], "data:image/png;base64,") || !strings.HasPrefix(icons["Classic"], "data:image/png;base64,") {
		t.Errorf("icons: %v", icons)
	}
	if none, _ := webAlternateIcons(linuxTestApp(t, "").cfg); none != nil {
		t.Errorf("an app without alternates: %v", none)
	}
}

// iOS's asset catalog has an icon set for each alternate, actool's
// --alternate-app-icon names.
func TestIOSAlternateIconSets(t *testing.T) {
	app := alternateIconsApp(t)
	cat, err := writeAssetCatalog(app.cfg, t.TempDir(), true)
	if err != nil {
		t.Fatal(err)
	}
	for _, set := range []string{"AppIcon", "dark", "Classic"} {
		if !exists(filepath.Join(cat, set+".appiconset", "icon.png")) || !exists(filepath.Join(cat, set+".appiconset", "Contents.json")) {
			t.Errorf("no %s icon set", set)
		}
	}
}
