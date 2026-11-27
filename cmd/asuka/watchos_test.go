package main

import (
	"os"
	"os/exec"
	"path/filepath"
	"slices"
	"strings"
	"testing"

	"github.com/egoist/asuka/lang/target"
)

// watchOS's toolchain: its SDKs' triples, platforms and minimum, its own
// host with notifications and pushes its only optional parts, and its
// Swift modules' targets (a watch's arm64_32 too).
func TestWatchOSToolchain(t *testing.T) {
	for _, c := range []struct {
		sdk, platform, bundle string
		device                bool
	}{
		{"watchsimulator", "watchos", "WatchSimulator", false},
		{"watchos", "watchos", "WatchOS", true},
	} {
		tc := &iosToolchain{sdkName: c.sdk}
		if !tc.watch() || tc.tv() || tc.swiftPlatform() != c.platform || tc.bundlePlatform() != c.bundle || tc.minOS() != watchosMin || tc.device() != c.device {
			t.Errorf("%s: %s %s %s device %v", c.sdk, tc.swiftPlatform(), tc.bundlePlatform(), tc.minOS(), tc.device())
		}
		if !slices.Equal(tc.devices(&appConfig{}), []int{4}) {
			t.Errorf("%s: UIDeviceFamily %v", c.sdk, tc.devices(&appConfig{}))
		}
		if got := tc.hostFeatures([]string{"ASUKA_CRASH", "ASUKA_NOTIFY", "ASUKA_PUSH", "ASUKA_VIDEO"}); !slices.Equal(got, []string{"ASUKA_NOTIFY", "ASUKA_PUSH"}) {
			t.Errorf("%s: the watch's host's optional parts are notifications and pushes, got %v", c.sdk, got)
		}
	}
	if (&iosToolchain{sdkName: "iphoneos"}).watch() {
		t.Error("iphoneos is a watch's")
	}
	b := &swiftBuild{platform: "watchos", sdkName: "watchsimulator", arch: "arm64"}
	if b.moduleTriple() != "arm64-apple-watchos-simulator" || b.target("10.0") != "arm64-apple-watchos10.0-simulator" {
		t.Errorf("watchOS simulator modules: %s, %s", b.moduleTriple(), b.target("10.0"))
	}
	b = &swiftBuild{platform: "watchos", sdkName: "watchos", arch: "arm64_32"}
	if b.moduleTriple() != "arm64_32-apple-watchos" || b.target("10.0") != "arm64_32-apple-watchos10.0" {
		t.Errorf("watchOS modules: %s, %s", b.moduleTriple(), b.target("10.0"))
	}
	if pre := b.prebuiltSwift(); pre.target != "watchos-arm64_32" {
		t.Errorf("watchOS's prebuilt modules' target: %s", pre.target)
	}
	if swiftPlatform("watchos") != `.watchOS("`+watchosMin+`")` {
		t.Errorf("Package.swift: %s", swiftPlatform("watchos"))
	}
	// A watch build compiles the app as iOS's: @platform is .iOS.
	if langPlatform(platformWatchOS) != target.IOS {
		t.Errorf("watchos compiles for %s", langPlatform(platformWatchOS))
	}
	if !slices.Equal(watchArchs(false), []string{"arm64_32", "arm64"}) || !slices.Equal(watchArchs(true), []string{"arm64"}) {
		t.Errorf("watch architectures: %v, %v", watchArchs(false), watchArchs(true))
	}
	if watchBundleID("dev.asuka.counter") != "dev.asuka.counter.watchkitapp" {
		t.Error(watchBundleID("dev.asuka.counter"))
	}
}

// A native package is built for watchOS when its Package.swift says so
// (or names no platform at all); one for iOS alone isn't, and throws
// unsupported there.
func TestSwiftPackageWatchOS(t *testing.T) {
	for _, c := range []struct {
		platforms []string
		want      bool
	}{
		{nil, true},
		{[]string{"ios", "macos"}, false},
		{[]string{"ios", "tvos", "watchos"}, true},
	} {
		if got := (swiftInfo{Platforms: c.platforms}).supports("watchos"); got != c.want {
			t.Errorf("%v supports watchOS: %v, want %v", c.platforms, got, c.want)
		}
	}
}

func TestWatchConfig(t *testing.T) {
	dir := t.TempDir()
	os.WriteFile(filepath.Join(dir, "asuka.toml"), []byte("[watch]\nprofile = \"Counter Watch\"\n"), 0o644)
	c, err := loadConfig(dir)
	if err != nil {
		t.Fatal(err)
	}
	if c.Watch.Profile != "Counter Watch" {
		t.Errorf("watch: %+v", c.Watch)
	}
}

// A watch app's Info.plist: a SwiftUI app (WKApplication), independent
// (WKWatchOnly), the watch's family and minimum, its companion's ID.
func TestWatchInfo(t *testing.T) {
	if _, err := exec.LookPath("xcrun"); err != nil {
		t.Skip("no Xcode")
	}
	tc := &iosToolchain{sdkName: "watchos"}
	if _, err := output("xcrun", "--sdk", "watchos", "--show-sdk-path"); err != nil {
		t.Skip("no watchOS SDK")
	}
	cfg := &appConfig{ID: "dev.asuka.counter", Version: "1.2.0", Build: 7}
	m, err := tc.watchInfo(cfg, watchBundleID(cfg.ID), "Counter", cfg.ID, true)
	if err != nil {
		t.Fatal(err)
	}
	want := map[string]any{
		"CFBundleIdentifier": "dev.asuka.counter.watchkitapp", "WKApplication": true, "WKWatchOnly": true,
		"WKCompanionAppBundleIdentifier": "dev.asuka.counter", "MinimumOSVersion": watchosMin, "CFBundleExecutable": appExe,
		"CFBundleShortVersionString": "1.2.0", "CFBundleVersion": "7", "DTPlatformName": "watchos",
	}
	for k, v := range want {
		if m[k] != v {
			t.Errorf("%s: %v, want %v", k, m[k], v)
		}
	}
	if !slices.Equal(m["UIDeviceFamily"].([]int), []int{4}) || !slices.Equal(m["CFBundleSupportedPlatforms"].([]string), []string{"WatchOS"}) {
		t.Errorf("family %v, platforms %v", m["UIDeviceFamily"], m["CFBundleSupportedPlatforms"])
	}
	if _, ok := m["NSAppTransportSecurity"]; ok {
		t.Error("a device's release reaches no asuka dev on the loopback")
	}
	for _, k := range []string{"UIApplicationSceneManifest", "UILaunchScreen"} {
		if _, ok := m[k]; ok {
			t.Errorf("a watch app's Info.plist has iOS's %s", k)
		}
	}
}

// A device build links for both of a watch's architectures, arm64_32
// (32-bit pointers) and arm64, the app compiled to C into the core, in
// one executable; its watch-only stub is Xcode's, with the watch app in
// Watch/. Not signed: that needs the developer's profiles.
func TestWatchDeviceBuild(t *testing.T) {
	if testing.Short() {
		t.Skip("builds the core and the host for a watch")
	}
	if _, err := output("xcrun", "--sdk", "watchos", "--show-sdk-path"); err != nil {
		t.Skip("no watchOS SDK")
	}
	app, err := loadApp(filepath.Join("..", "..", "examples", "counter"), platformWatchOS)
	if err != nil {
		t.Fatal(err)
	}
	m, err := app.compile(target.IOS)
	if err != nil {
		t.Fatal(err)
	}
	work := t.TempDir()
	bundle := filepath.Join(work, "Counter.app")
	exe := filepath.Join(bundle, appExe)
	tc, err := linkWatchExe(app, m, nil, work, "watchos", exe, false)
	if err != nil {
		t.Fatal(err)
	}
	info, err := output("xcrun", "lipo", "-archs", exe)
	if err != nil {
		t.Fatal(err)
	}
	if archs := strings.Fields(info); !slices.Contains(archs, "arm64_32") || !slices.Contains(archs, "arm64") {
		t.Errorf("the executable's architectures: %s", info)
	}
	stub, err := tc.watchStub(app, work, bundle)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(stub, "Watch", "Counter.app", appExe)); err != nil {
		t.Error("the stub has no watch app:", err)
	}
	b, _ := os.ReadFile(filepath.Join(stub, "Info.plist"))
	for _, k := range []string{"ITSWatchOnlyContainer", "LSApplicationLaunchProhibited"} {
		if !strings.Contains(string(b), "<key>"+k+"</key>") {
			t.Errorf("the stub's Info.plist has no %s", k)
		}
	}
}
