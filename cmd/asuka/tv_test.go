package main

import (
	"encoding/json"
	"encoding/xml"
	"image"
	"image/color"
	"image/png"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"testing"

	"github.com/egoist/asuka/lang/target"
)

// writeTestIcon writes a square PNG icon: blue, a white square in its
// middle.
func writeTestIcon(t *testing.T, dir string) string {
	t.Helper()
	img := image.NewNRGBA(image.Rect(0, 0, 256, 256))
	for y := 0; y < 256; y++ {
		for x := 0; x < 256; x++ {
			c := color.NRGBA{30, 80, 220, 255}
			if x > 96 && x < 160 && y > 96 && y < 160 {
				c = color.NRGBA{255, 255, 255, 255}
			}
			img.SetNRGBA(x, y, c)
		}
	}
	p := filepath.Join(dir, "icon.png")
	f, err := os.Create(p)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	if err := png.Encode(f, img); err != nil {
		t.Fatal(err)
	}
	return p
}

// An Android build of an app that declares tv.android is a TV app too:
// the TV launcher's intent on its activity (or each alias), touch and
// leanback not required, and the banner; one that doesn't has none of it.
func TestAndroidTVManifest(t *testing.T) {
	c := &appConfig{Icon: "icon.png"}
	plain := c.androidManifest(false)
	for _, s := range []string{"LEANBACK_LAUNCHER", "android.software.leanback", "android:banner"} {
		if strings.Contains(plain, s) {
			t.Errorf("a phone app's manifest has %s", s)
		}
	}
	c.TV.Android = true
	m := c.androidManifest(false)
	if err := xml.Unmarshal([]byte(m), new(struct{})); err != nil {
		t.Fatalf("the manifest isn't XML: %v\n%s", err, m)
	}
	for _, s := range []string{
		`<category android:name="android.intent.category.LEANBACK_LAUNCHER" />`,
		`<uses-feature android:name="android.software.leanback" android:required="false" />`,
		`<uses-feature android:name="android.hardware.touchscreen" android:required="false" />`,
		`<application android:banner="@drawable/asuka_tv_banner"`,
	} {
		if !strings.Contains(m, s) {
			t.Errorf("the TV app's manifest lacks %s:\n%s", s, m)
		}
	}
	// The uses-features are the manifest's, before the application.
	if strings.Index(m, "android.software.leanback") > strings.Index(m, "<application") {
		t.Error("uses-feature after <application>")
	}
	// No icon nor banner: the launcher's intent, and no banner to name.
	c.Icon = ""
	if strings.Contains(c.androidManifest(false), "android:banner") {
		t.Error("a banner named without an icon or tv.banner to make it of")
	}
	// Alternate icons: each alias is the TV launcher's entry too.
	c.Icon = "icon.png"
	c.AlternateIcons = map[string]string{"Dark": "dark.png"}
	m = c.androidManifest(false)
	if n := strings.Count(m, "LEANBACK_LAUNCHER"); n != 2 {
		t.Errorf("%d TV launcher intents with two aliases, want 2:\n%s", n, m)
	}
}

// The banner Android TV's launcher shows: 320×180 at xhdpi, made from
// the icon on its corner's color, or from tv.banner.
func TestAndroidTVBanner(t *testing.T) {
	dir := t.TempDir()
	c := &appConfig{Icon: writeTestIcon(t, dir)}
	res := filepath.Join(dir, "res")
	if ok, err := androidTVRes(c, res); err != nil || ok {
		t.Fatalf("without tv.android: wrote %v, %v", ok, err)
	}
	c.TV.Android = true
	ok, err := androidTVRes(c, res)
	if err != nil || !ok {
		t.Fatalf("wrote %v, %v", ok, err)
	}
	img := decodePNG(t, filepath.Join(res, "drawable-xhdpi", "asuka_tv_banner.png"))
	if b := img.Bounds(); b.Dx() != 320 || b.Dy() != 180 {
		t.Fatalf("banner %v, want 320×180", b)
	}
	// The icon's corner color at the banner's edge, its white in the middle.
	if r, g, b, _ := img.At(4, 4).RGBA(); r>>8 != 30 || g>>8 != 80 || b>>8 != 220 {
		t.Errorf("edge %d %d %d, want the icon's corner (30 80 220)", r>>8, g>>8, b>>8)
	}
	if r, _, _, _ := img.At(160, 90).RGBA(); r>>8 != 255 {
		t.Errorf("middle red %d, want the icon's white", r>>8)
	}
	// tv.background takes the corner's place.
	c.TV.Background = "#102030"
	if _, err := androidTVRes(c, res); err != nil {
		t.Fatal(err)
	}
	img = decodePNG(t, filepath.Join(res, "drawable-xhdpi", "asuka_tv_banner.png"))
	if r, g, b, _ := img.At(4, 4).RGBA(); r>>8 != 0x10 || g>>8 != 0x20 || b>>8 != 0x30 {
		t.Errorf("edge %d %d %d, want tv.background", r>>8, g>>8, b>>8)
	}
}

func decodePNG(t *testing.T, p string) image.Image {
	t.Helper()
	f, err := os.Open(p)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	img, err := png.Decode(f)
	if err != nil {
		t.Fatal(err)
	}
	return img
}

// [tv]'s checks: its banner is the app's path, its background a color.
func TestTVConfig(t *testing.T) {
	dir := t.TempDir()
	os.WriteFile(filepath.Join(dir, "asuka.toml"), []byte("[tv]\nandroid = true\nbanner = \"art/banner.png\"\nbackground = \"#000000\"\n"), 0o644)
	c, err := loadConfig(dir)
	if err != nil {
		t.Fatal(err)
	}
	if !c.TV.Android || c.TV.Banner != filepath.Join(dir, "art", "banner.png") || c.TV.Background != "#000000" {
		t.Errorf("tv: %+v", c.TV)
	}
	os.WriteFile(filepath.Join(dir, "asuka.toml"), []byte("[tv]\nbackground = \"navy\"\n"), 0o644)
	if _, err := loadConfig(dir); err == nil || !strings.Contains(err.Error(), "tv background") {
		t.Errorf("a background that isn't a color: %v", err)
	}
}

// tvOS's toolchain: its SDKs' triples, platforms and minimum, and the
// Info.plist keys of a TV app.
func TestTVOSToolchain(t *testing.T) {
	for _, c := range []struct {
		sdk, platform, bundle, min string
		device, tv                 bool
	}{
		{"appletvsimulator", "tvos", "AppleTVSimulator", tvosMin, false, true},
		{"appletvos", "tvos", "AppleTVOS", tvosMin, true, true},
		{"iphonesimulator", "ios", "iPhoneSimulator", iosMin, false, false},
		{"iphoneos", "ios", "iPhoneOS", iosMin, true, false},
	} {
		tc := &iosToolchain{sdkName: c.sdk}
		if tc.swiftPlatform() != c.platform || tc.bundlePlatform() != c.bundle || tc.minOS() != c.min || tc.device() != c.device || tc.tv() != c.tv {
			t.Errorf("%s: %s %s %s device %v tv %v", c.sdk, tc.swiftPlatform(), tc.bundlePlatform(), tc.minOS(), tc.device(), tc.tv())
		}
	}
	tc := &iosToolchain{sdkName: "appletvsimulator"}
	m := map[string]any{
		"UIDeviceFamily": []int{1, 2}, "MinimumOSVersion": iosMin, "CADisableMinimumFrameDurationOnPhone": true,
		"UISupportedInterfaceOrientations": []string{"UIInterfaceOrientationPortrait"}, "UISupportedInterfaceOrientations~ipad": []string{},
	}
	tc.tvInfo(m)
	if !slices.Equal(m["UIDeviceFamily"].([]int), []int{3}) || m["MinimumOSVersion"] != tvosMin || m["UIUserInterfaceStyle"] != "Automatic" {
		t.Errorf("tvOS Info.plist: %v", m)
	}
	for _, k := range []string{"CADisableMinimumFrameDurationOnPhone", "UISupportedInterfaceOrientations", "UISupportedInterfaceOrientations~ipad"} {
		if _, ok := m[k]; ok {
			t.Errorf("tvOS Info.plist has iOS's %s", k)
		}
	}
	// The host's parts tvOS has; iOS keeps them all.
	all := []string{"ASUKA_NOTIFY", "ASUKA_SHADER", "ASUKA_VIDEO", "ASUKA_WEBVIEW", "ASUKA_WIDGET"}
	if got := tc.hostFeatures(all); !slices.Equal(got, []string{"ASUKA_SHADER", "ASUKA_VIDEO"}) {
		t.Errorf("tvOS's host flags: %v", got)
	}
	if got := (&iosToolchain{sdkName: "iphoneos"}).hostFeatures(all); !slices.Equal(got, all) {
		t.Errorf("iOS's host flags: %v", got)
	}
	// Its Swift modules' triples and Package.swift platform.
	b := &swiftBuild{platform: "tvos", sdkName: "appletvsimulator", arch: "arm64"}
	if b.moduleTriple() != "arm64-apple-tvos-simulator" || b.target("17.0") != "arm64-apple-tvos17.0-simulator" {
		t.Errorf("tvOS simulator modules: %s, %s", b.moduleTriple(), b.target("17.0"))
	}
	b.sdkName = "appletvos"
	if b.moduleTriple() != "arm64-apple-tvos" || b.target("17.0") != "arm64-apple-tvos17.0" {
		t.Errorf("tvOS modules: %s, %s", b.moduleTriple(), b.target("17.0"))
	}
	if swiftPlatform("tvos") != `.tvOS("`+tvosMin+`")` {
		t.Errorf("Package.swift: %s", swiftPlatform("tvos"))
	}
	// A tvOS build compiles the app as iOS's: @platform is .iOS.
	if langPlatform(platformTVOS) != target.IOS {
		t.Errorf("tvos compiles for %s", langPlatform(platformTVOS))
	}
}

// A native package is built for tvOS when its Package.swift says so (or
// names no platform at all); one for iOS alone isn't, and throws
// unsupported there.
func TestSwiftPackageTVOS(t *testing.T) {
	for _, c := range []struct {
		platforms []string
		want      bool
	}{
		{nil, true},
		{[]string{"ios", "macos"}, false},
		{[]string{"ios", "tvos"}, true},
	} {
		if got := (swiftInfo{Platforms: c.platforms}).supports("tvos"); got != c.want {
			t.Errorf("%v supports tvOS: %v, want %v", c.platforms, got, c.want)
		}
	}
}

// A provisioning profile is for its platforms: one that doesn't say is
// iOS's.
func TestProfilePlatform(t *testing.T) {
	ios := &profile{}
	tv := &profile{platforms: []string{"tvOS"}}
	both := &profile{platforms: []string{"iOS", "xrOS"}}
	if !ios.forPlatform("iOS") || ios.forPlatform("tvOS") || !tv.forPlatform("tvOS") || tv.forPlatform("iOS") || !both.forPlatform("iOS") {
		t.Error("profiles' platforms")
	}
}

// Apple TV's icon and top shelf, made of the icon: layered image stacks
// (a back and a front, at 1x and 2x) and the shelf's images, as actool
// takes them.
func TestTVBrandAssets(t *testing.T) {
	dir := t.TempDir()
	c := &appConfig{Icon: writeTestIcon(t, dir)}
	cat := filepath.Join(dir, "Assets.xcassets")
	if err := c.writeTVBrandAssets(cat); err != nil {
		t.Fatal(err)
	}
	ba := filepath.Join(cat, tvBrandAssets+".brandassets")
	var contents struct {
		Assets []struct{ Filename, Role, Size string }
	}
	b, err := os.ReadFile(filepath.Join(ba, "Contents.json"))
	if err != nil {
		t.Fatal(err)
	}
	json.Unmarshal(b, &contents)
	var roles []string
	for _, a := range contents.Assets {
		roles = append(roles, a.Role+" "+a.Size)
		if _, err := os.Stat(filepath.Join(ba, a.Filename, "Contents.json")); err != nil {
			t.Errorf("%s: %v", a.Filename, err)
		}
	}
	slices.Sort(roles)
	want := []string{"primary-app-icon 1280x768", "primary-app-icon 400x240", "top-shelf-image 1920x720", "top-shelf-image-wide 2320x720"}
	if !slices.Equal(roles, want) {
		t.Errorf("roles %v, want %v", roles, want)
	}
	for _, f := range []struct {
		path string
		w, h int
	}{
		{"App Icon.imagestack/Back.imagestacklayer/Content.imageset/back@2x.png", 800, 480},
		{"App Icon.imagestack/Front.imagestacklayer/Content.imageset/front@1x.png", 400, 240},
		{"App Icon - App Store.imagestack/Back.imagestacklayer/Content.imageset/back@1x.png", 1280, 768},
		{"Top Shelf Image Wide.imageset/shelf@2x.png", 4640, 1440},
	} {
		img := decodePNG(t, filepath.Join(ba, filepath.FromSlash(f.path)))
		if b := img.Bounds(); b.Dx() != f.w || b.Dy() != f.h {
			t.Errorf("%s: %v, want %d×%d", f.path, b, f.w, f.h)
		}
	}
	// The front layer is the icon alone: clear around it.
	front := decodePNG(t, filepath.Join(ba, "App Icon.imagestack", "Front.imagestacklayer", "Content.imageset", "front@1x.png"))
	if _, _, _, a := front.At(2, 2).RGBA(); a != 0 {
		t.Error("the front layer isn't clear at its edge")
	}
}
