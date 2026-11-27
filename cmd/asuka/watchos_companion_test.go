package main

import (
	"os"
	"os/exec"
	"path/filepath"
	"slices"
	"strings"
	"testing"
)

// [watch] companion: the iPhone app carries the watch app, which still
// runs on its own (WKRunsIndependentlyOfCompanionApp), paired with the
// iPhone app's ID; a watch-only app is WKWatchOnly.
func TestWatchCompanionInfo(t *testing.T) {
	dir := t.TempDir()
	os.WriteFile(filepath.Join(dir, "asuka.toml"), []byte("name = \"Pair\"\nid = \"dev.asuka.pair\"\n\n[watch]\ncompanion = true\nwidgetProfile = \"Pair Watch Widgets\"\n"), 0o644)
	cfg, err := loadConfig(dir)
	if err != nil {
		t.Fatal(err)
	}
	if !cfg.Watch.Companion || cfg.Watch.WidgetProfile != "Pair Watch Widgets" {
		t.Fatalf("watch: %+v", cfg.Watch)
	}
	if _, err := output("xcrun", "--sdk", "watchos", "--show-sdk-path"); err != nil {
		t.Skip("no watchOS SDK")
	}
	tc := &iosToolchain{sdkName: "watchos"}
	m, err := tc.watchInfo(cfg, watchBundleID(cfg.ID), "Pair", cfg.ID, false)
	if err != nil {
		t.Fatal(err)
	}
	if m["WKRunsIndependentlyOfCompanionApp"] != true || m["WKCompanionAppBundleIdentifier"] != "dev.asuka.pair" || m["CFBundleIdentifier"] != "dev.asuka.pair.watchkitapp" {
		t.Errorf("a companion's Info.plist: %v", m)
	}
	if _, ok := m["WKWatchOnly"]; ok {
		t.Error("a companion is WKWatchOnly")
	}
	if _, ok := m["AsukaAppGroup"]; ok {
		t.Error("an App Group without complications")
	}
	cfg.Widgets = []widgetConfig{{Kind: "steps", Sizes: []string{"circular"}}}
	if m, _ = tc.watchInfo(cfg, watchBundleID(cfg.ID), "Pair", cfg.ID, true); m["AsukaAppGroup"] != "group.dev.asuka.pair" || m["WKWatchOnly"] != true {
		t.Errorf("a watch-only app with complications: %v", m)
	}
}

// A companion's watch app ships in the iPhone app's .ipa: asuka build
// --platform watchos makes none of its own for watches (the simulator's
// still runs it alone).
func TestBuildWatchCompanionRefused(t *testing.T) {
	cfg := &appConfig{Name: "Pair", ID: "dev.asuka.pair"}
	cfg.Watch.Companion = true
	err := buildWatch(&asukaApp{dir: t.TempDir(), cfg: cfg}, &buildOptions{})
	if err == nil || !strings.Contains(err.Error(), "asuka build --platform ios") {
		t.Fatalf("a companion's watch-only build: %v", err)
	}
}

// The watch app's config is its own copy: what it drops and what its
// packages ask of it don't reach the iPhone app's.
func TestConfigClone(t *testing.T) {
	c := &appConfig{ID: "dev.asuka.pair", Widgets: []widgetConfig{{Kind: "a", Sizes: []string{"small", "circular"}}},
		Shortcuts: []shortcutConfig{{ID: "new"}}, entitlements: map[string]any{"x": true}}
	c.IOS.Usage = map[string]string{"NSCameraUsageDescription": "photos"}
	d := c.clone()
	d.tvToldDropped = true
	d.dropWatchExtensions()
	d.entitlements["y"] = true
	d.IOS.Usage["NSMicrophoneUsageDescription"] = "voice"
	if len(c.Shortcuts) != 1 || !slices.Equal(c.Widgets[0].Sizes, []string{"small", "circular"}) || len(c.entitlements) != 1 || len(c.IOS.Usage) != 1 {
		t.Errorf("the watch's changes reached the iPhone's config: %+v", c)
	}
	if !slices.Equal(d.Widgets[0].Sizes, []string{"circular"}) || d.Shortcuts != nil {
		t.Errorf("the watch's config: %+v", d)
	}
}

// Complications are the widgets' watch sizes: the iPhone's extension
// has its sizes (a corner is the watch's alone), the watch's its own.
func TestWatchWidgets(t *testing.T) {
	c := &appConfig{ID: "dev.asuka.rings", Widgets: []widgetConfig{
		{Kind: "steps", Name: "Steps", Sizes: []string{"small", "circular", "corner"}},
		{Kind: "home", Name: "Home", Sizes: []string{"medium"}},
		{Kind: "dial", Name: "Dial", Sizes: []string{"corner"}},
	}}
	if err := c.checkWidgets(); err != nil {
		t.Fatal(err)
	}
	ios, watch := c.iosWidgets(), c.watchWidgets()
	if len(ios) != 2 || !slices.Equal(ios[0].Sizes, []string{"small", "circular"}) || ios[1].Kind != "home" {
		t.Errorf("the iPhone's: %+v", ios)
	}
	if len(watch) != 2 || !slices.Equal(watch[0].Sizes, []string{"circular", "corner"}) || watch[1].Kind != "dial" {
		t.Errorf("the watch's: %+v", watch)
	}
	phone := c.widgetKindsSwift(false)
	if strings.Contains(phone, "accessoryCorner") || strings.Contains(phone, `"dial"`) || !strings.Contains(phone, ".systemSmall, .accessoryCircular") {
		t.Errorf("the iPhone's kinds:\n%s", phone)
	}
	c.liveActivity = true
	wrist := c.widgetKindsSwift(true)
	if !strings.Contains(wrist, ".accessoryCircular, .accessoryCorner") || strings.Contains(wrist, "systemSmall") || strings.Contains(wrist, `"home"`) || strings.Contains(wrist, "LiveActivity") {
		t.Errorf("the watch's kinds:\n%s", wrist)
	}
	// An app whose widgets are all the watch's has no iPhone extension.
	only := &appConfig{Widgets: []widgetConfig{{Kind: "dial", Sizes: []string{"corner"}}}}
	if only.widgetExtension() {
		t.Error("an iPhone widget extension for a corner alone")
	}
	d := c.clone()
	d.tvToldDropped = true
	d.dropWatchExtensions()
	if len(d.Widgets) != 2 || d.liveActivity {
		t.Errorf("the watch build's widgets %+v, Live Activities %v", d.Widgets, d.liveActivity)
	}
	bad := &appConfig{Widgets: []widgetConfig{{Kind: "x", Sizes: []string{"edge"}}}}
	if err := bad.checkWidgets(); err == nil || !strings.Contains(err.Error(), "corner") {
		t.Errorf("an unknown size: %v", err)
	}
}

// An iPhone app with [watch] companion carries its watch app in Watch/,
// paired with it, and the watch app its complications' widget extension:
// built for the simulators, as a release is (signed for them).
func TestWatchCompanionSimulatorBuild(t *testing.T) {
	if testing.Short() {
		t.Skip("builds the iPhone app and the watch app")
	}
	if _, err := output("xcrun", "--sdk", "watchsimulator", "--show-sdk-path"); err != nil {
		t.Skip("no watchOS simulator SDK")
	}
	dir := t.TempDir()
	writeFiles(t, dir, map[string]string{
		"asuka.toml": "name = \"Pair\"\nid = \"dev.asuka.pairtest\"\nversion = \"1.0.0\"\n\n[watch]\ncompanion = true\n\n[[widgets]]\nkind = \"steps\"\nsizes = [\"small\", \"circular\", \"corner\"]\n",
		"app.asuka":  "import platform/companion\nimport platform/widget\n\napp {\n  Text(\"{companion.available()}\")\n  Button(\"Set\") { on press { widget.set(\"steps\", widget.Entry(value: \"1\")) } }\n}\n",
	})
	out := t.TempDir()
	if err := cmdBuild([]string{"--platform", "ios", "--simulator", "-o", out, dir}); err != nil {
		t.Fatal(err)
	}
	app := filepath.Join(out, "Pair.app")
	watch := filepath.Join(app, "Watch", "Pair.app")
	plistOf := func(p string) string {
		b, err := exec.Command("plutil", "-convert", "xml1", "-o", "-", p).Output()
		if err != nil {
			t.Fatalf("%s: %v", p, err)
		}
		return string(b)
	}
	w := plistOf(filepath.Join(watch, "Info.plist"))
	for _, want := range []string{"<string>dev.asuka.pairtest.watchkitapp</string>", "<key>WKCompanionAppBundleIdentifier</key>", "<key>WKRunsIndependentlyOfCompanionApp</key>", "<string>watchsimulator</string>", "<string>group.dev.asuka.pairtest</string>"} {
		if !strings.Contains(w, want) {
			t.Errorf("the watch app's Info.plist has no %s", want)
		}
	}
	// Each its own widget extension: the iPhone's home screen and lock
	// screen sizes, the watch's complications.
	if !exists(filepath.Join(app, "PlugIns", widgetExe+".appex")) {
		t.Error("the iPhone app has no widget extension")
	}
	x := plistOf(filepath.Join(watch, "PlugIns", widgetExe+".appex", "Info.plist"))
	if !strings.Contains(x, "<string>dev.asuka.pairtest.watchkitapp.widgets</string>") || !strings.Contains(x, "com.apple.widgetkit-extension") || !strings.Contains(x, "WatchSimulator") {
		t.Errorf("the watch's widget extension's Info.plist:\n%s", x)
	}
	if err := exec.Command("codesign", "--verify", "--deep", app).Run(); err != nil {
		t.Errorf("the iPhone app's signature doesn't seal the watch app: %v", err)
	}
	// A watch's complications build for its arm64_32 too.
	cfg := &appConfig{ID: "dev.asuka.pairtest", Widgets: []widgetConfig{{Kind: "steps", Name: "Steps", Sizes: []string{"circular", "corner"}}}}
	tc, err := newAppleToolchainFor("watchos", "arm64_32", false)
	if err != nil {
		t.Fatal(err)
	}
	work := filepath.Join(t.TempDir(), "a", "b")
	os.MkdirAll(work, 0o755)
	exe, err := tc.buildWidgets(work, cfg)
	if err != nil {
		t.Fatal(err)
	}
	if archs, _ := output("xcrun", "lipo", "-archs", exe); strings.TrimSpace(archs) != "arm64_32" {
		t.Errorf("the watch's widget extension for a device: %s", archs)
	}
}
