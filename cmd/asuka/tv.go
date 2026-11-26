package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"image"
	"image/color"
	"image/draw"
	"math"
	"os"
	"os/exec"
	"path/filepath"
	"slices"
	"sort"
	"strings"
	"time"

	"github.com/egoist/asuka/internal/cbuild"
)

// TVs (docs/tv.md): Apple TV is the iOS host built with tvOS's SDK
// (--platform tvos: its own simulator, device builds and asset catalog,
// the same app's code: @platform is .iOS there, and environment().tv
// tells a TV); Android TV is the Android build, an app that declares
// asuka.toml's tv.android a TV app too (the TV launcher's intent, a
// banner, touch not required).

const tvosMin = cbuild.TVOSMin

// tvConfig is asuka.toml's [tv].
//
//	[tv]
//	android = true           # Android TV and Google TV too
//	banner = "banner.png"    # a landscape image: the TV launcher's
//	background = "#101114"   # behind the icon, when the banner's made from it
type tvConfig struct {
	// Android makes the Android build a TV app as well: the TV launcher's
	// (leanback) intent, a banner, and touch and the leanback library not
	// required, so Google Play offers it to TVs and phones alike.
	Android bool `json:"android"`
	// Banner is a landscape PNG (16:9, 1920×1080 or larger), the TV
	// launchers' image of the app: Android TV's banner, Apple TV's icon
	// and top shelf, each cut to its shape from the middle. Default: made
	// from icon, in the middle of Background.
	Banner string `json:"banner"`
	// Background is "#rrggbb", what a made banner has behind the icon
	// (default: the icon's corner's color).
	Background string `json:"background"`
}

// checkTV checks [tv] and makes its paths the app's.
func (c *appConfig) checkTV(dir string) error {
	if c.TV.Banner != "" && !filepath.IsAbs(c.TV.Banner) {
		c.TV.Banner = filepath.Join(dir, c.TV.Banner)
	}
	if b := c.TV.Background; b != "" && (!hexColor.MatchString(b) || len(strings.TrimPrefix(b, "#")) != 6) {
		return fmt.Errorf("asuka.toml: tv background %q (want #rrggbb)", b)
	}
	return nil
}

// tvHostFlags are the iOS host's optional parts tvOS has what they need
// of: the others (WebKit's web views, notifications, the photo library,
// contacts, calendars, mail, widgets and Live Activities, share and
// notification extensions) aren't built for it, and the packages that
// call them throw unsupported there.
var tvHostFlags = []string{"ASUKA_CRASH", "ASUKA_ICLOUD", "ASUKA_LIBRARY", "ASUKA_PERIODIC", "ASUKA_SHADER", "ASUKA_VIDEO"}

// hostFeatures are the host's flags a build of t has of features: all
// of them on iOS, tvOS's on a TV.
func (t *iosToolchain) hostFeatures(features []string) []string {
	if !t.tv() {
		return features
	}
	var out []string
	for _, f := range features {
		if slices.Contains(tvHostFlags, f) {
			out = append(out, f)
		}
	}
	return out
}

// dropTVExtensions leaves out of a tvOS build what tvOS has no
// extensions for: a share target, widgets and Live Activities, App
// Shortcuts, push's rewrites; each said once.
func (c *appConfig) dropTVExtensions() {
	var left []string
	if c.ShareTarget.on {
		c.ShareTarget.on = false
		left = append(left, "shareTarget")
	}
	if len(c.Widgets) > 0 || c.liveActivity {
		c.Widgets, c.liveActivity = nil, false
		left = append(left, "widgets")
	}
	if len(c.Shortcuts) > 0 {
		c.Shortcuts = nil
		left = append(left, "shortcuts")
	}
	if c.pushRewrite {
		c.pushRewrite = false
		left = append(left, "push's rewrite")
	}
	if c.carScene {
		c.carScene = false
		delete(c.entitlements, c.carEntitlement())
		left = append(left, "CarPlay")
	}
	if len(left) > 0 && !c.tvToldDropped {
		c.tvToldDropped = true
		fmt.Printf("asuka: tvOS has no %s: the Apple TV app goes without\n", strings.Join(left, ", "))
	}
}

// tvInfo sets a tvOS app's own keys in its Info.plist m (t.infoPlist's,
// which has iOS's).
func (t *iosToolchain) tvInfo(m map[string]any) {
	for _, k := range []string{"CADisableMinimumFrameDurationOnPhone", "UISupportedInterfaceOrientations", "UISupportedInterfaceOrientations~ipad"} {
		delete(m, k)
	}
	m["UIDeviceFamily"] = []int{3}
	m["MinimumOSVersion"] = tvosMin
	// A TV app is light unless it says it follows the system's
	// appearance, which Asuka's apps do everywhere.
	m["UIUserInterfaceStyle"] = "Automatic"
}

// linkTV makes asuka dev's tvOS app (the player) in work: link's for a
// TV, without extensions.
func (t *iosToolchain) linkTV(app *asukaApp, work, hostObj, lib string) (string, error) {
	cfg := app.cfg
	cfg.dropTVExtensions()
	bundle := filepath.Join(work, app.name+".app")
	os.RemoveAll(bundle)
	if err := os.MkdirAll(bundle, 0o755); err != nil {
		return "", err
	}
	ents, err := simulatorEntitlements(work, cfg)
	if err != nil {
		return "", err
	}
	if _, err := t.linkApp(work, bundle, cfg, hostObj, lib, ents); err != nil {
		return "", err
	}
	if err := t.embedPackages(bundle); err != nil {
		return "", err
	}
	if err := copyDir(filepath.Join(app.dir, "assets"), filepath.Join(bundle, "assets")); err != nil {
		return "", err
	}
	if err := cfg.copyStoreKit(bundle); err != nil {
		return "", err
	}
	if err := cfg.copyGoogleServices(bundle, app.bundleID); err != nil {
		return "", err
	}
	info, err := t.infoPlist(cfg, true)
	if err != nil {
		return "", err
	}
	info["CFBundleIdentifier"] = app.bundleID
	info["CFBundleName"], info["CFBundleDisplayName"] = app.displayName(), app.displayName()
	cfg.plistLocalizations(info)
	infoPath := filepath.Join(bundle, "Info.plist")
	if err := os.WriteFile(infoPath, []byte(plist(info)), 0o644); err != nil {
		return "", err
	}
	if err := cfg.writeLprojs(bundle); err != nil {
		return "", err
	}
	if err := t.compileAssets(cfg, work, bundle, infoPath, cfg.Icon != "" || cfg.TV.Banner != ""); err != nil {
		return "", err
	}
	sign := simulatorIdentity(cfg.IOS.Team)
	if err := t.signFrameworks(work, bundle, sign); err != nil {
		return "", err
	}
	return bundle, t.run(work, nil, "codesign", "--force", "--sign", sign, bundle)
}

// The TV launchers' images: Android TV's banner (320×180, xhdpi), Apple
// TV's icon (a layered image, 400×240 at 1x and 2x, 1280×768 for the
// App Store) and top shelf images (1920×720, 2320×720, at 1x and 2x).

// tvLayers are a TV image of w×h: its back (the banner, or the color)
// and its front (the icon in the middle, as tall as frac of it; nothing
// over a banner, which has its own).
func (c *appConfig) tvLayers(w, h int, frac float64) (back, front *image.NRGBA, err error) {
	if c.TV.Banner != "" {
		b, err := loadImage(c.TV.Banner)
		if err != nil {
			return nil, nil, err
		}
		return cover(b, w, h), image.NewNRGBA(image.Rect(0, 0, w, h)), nil
	}
	if c.Icon == "" {
		return nil, nil, errors.New("asuka.toml has no icon (nor tv.banner) to make the TV's banner of")
	}
	icon, err := loadIcon(c.Icon)
	if err != nil {
		return nil, nil, err
	}
	bg := icon.NRGBAAt(0, 0)
	if c.TV.Background != "" {
		r, g, b := rgb(c.TV.Background)
		bg = color.NRGBA{r, g, b, 255}
	} else if bg.A < 255 {
		bg = color.NRGBA{255, 255, 255, 255}
	}
	back = image.NewNRGBA(image.Rect(0, 0, w, h))
	draw.Draw(back, back.Bounds(), &image.Uniform{bg}, image.Point{}, draw.Src)
	front = image.NewNRGBA(image.Rect(0, 0, w, h))
	size := int(math.Round(float64(h) * frac))
	in := resize(icon, size)
	off := image.Pt((w-size)/2, (h-size)/2)
	draw.Draw(front, in.Bounds().Add(off), in, image.Point{}, draw.Over)
	return back, front, nil
}

// tvFlat is a TV image of w×h, its layers flattened.
func (c *appConfig) tvFlat(w, h int, frac float64) (*image.NRGBA, error) {
	back, front, err := c.tvLayers(w, h, frac)
	if err != nil {
		return nil, err
	}
	draw.Draw(back, back.Bounds(), front, image.Point{}, draw.Over)
	return back, nil
}

// loadImage reads a PNG of any shape.
func loadImage(path string) (*image.NRGBA, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	img, _, err := image.Decode(f)
	if err != nil {
		return nil, fmt.Errorf("%s: %v", path, err)
	}
	n := image.NewNRGBA(image.Rect(0, 0, img.Bounds().Dx(), img.Bounds().Dy()))
	draw.Draw(n, n.Bounds(), img, img.Bounds().Min, draw.Src)
	return n, nil
}

// cover scales src to cover w×h and cuts that from its middle.
func cover(src *image.NRGBA, w, h int) *image.NRGBA {
	sw, sh := src.Bounds().Dx(), src.Bounds().Dy()
	scale := math.Max(float64(w)/float64(sw), float64(h)/float64(sh))
	cw, ch := float64(w)/scale, float64(h)/scale
	x0, y0 := (float64(sw)-cw)/2, (float64(sh)-ch)/2
	dst := image.NewNRGBA(image.Rect(0, 0, w, h))
	for y := 0; y < h; y++ {
		sy0, sy1 := y0+float64(y)/scale, y0+float64(y+1)/scale
		for x := 0; x < w; x++ {
			sx0, sx1 := x0+float64(x)/scale, x0+float64(x+1)/scale
			var r, g, b, a, wsum float64
			for sy := int(sy0); sy < int(math.Ceil(sy1)) && sy < sh; sy++ {
				wy := math.Min(sy1, float64(sy+1)) - math.Max(sy0, float64(sy))
				for sx := int(sx0); sx < int(math.Ceil(sx1)) && sx < sw; sx++ {
					wx := math.Min(sx1, float64(sx+1)) - math.Max(sx0, float64(sx))
					wt := wx * wy
					c := src.NRGBAAt(sx, sy)
					al := float64(c.A) / 255
					r += float64(c.R) * al * wt
					g += float64(c.G) * al * wt
					b += float64(c.B) * al * wt
					a += al * wt
					wsum += wt
				}
			}
			if wsum == 0 || a == 0 {
				continue
			}
			dst.SetNRGBA(x, y, color.NRGBA{R: uint8(math.Round(r / a)), G: uint8(math.Round(g / a)), B: uint8(math.Round(b / a)), A: uint8(math.Round(a / wsum * 255))})
		}
	}
	return dst
}

// writeTVBrandAssets writes Apple TV's icon and top shelf images into the
// asset catalog cat: "App Icon & Top Shelf Image", which actool compiles
// into the bundle's icons (CFBundleIcons) and TVTopShelfImage.
func (c *appConfig) writeTVBrandAssets(cat string) error {
	ba := filepath.Join(cat, tvBrandAssets+".brandassets")
	os.RemoveAll(ba)
	info := map[string]any{"author": "asuka", "version": 1}
	writeJSON := func(dir string, v map[string]any) error {
		if err := os.MkdirAll(dir, 0o755); err != nil {
			return err
		}
		v["info"] = info
		b, _ := json.MarshalIndent(v, "", "  ")
		return os.WriteFile(filepath.Join(dir, "Contents.json"), b, 0o644)
	}
	// An icon: a stack of two layers, the back and the front, which tvOS
	// moves apart as the focus tilts it (its parallax).
	stack := func(name string, w, h int, scales []int, idiom string) error {
		dir := filepath.Join(ba, name+".imagestack")
		var layers []any
		for _, l := range []string{"Front", "Back"} {
			ldir := filepath.Join(dir, l+".imagestacklayer")
			var images []any
			for _, s := range scales {
				back, front, err := c.tvLayers(w*s, h*s, 0.6)
				if err != nil {
					return err
				}
				img := front
				if l == "Back" {
					img = back
				}
				file := fmt.Sprintf("%s@%dx.png", strings.ToLower(l), s)
				if err := writePNG(filepath.Join(ldir, "Content.imageset", file), img); err != nil {
					return err
				}
				images = append(images, map[string]any{"filename": file, "idiom": idiom, "scale": fmt.Sprintf("%dx", s)})
			}
			if err := writeJSON(filepath.Join(ldir, "Content.imageset"), map[string]any{"images": images}); err != nil {
				return err
			}
			if err := writeJSON(ldir, map[string]any{}); err != nil {
				return err
			}
			layers = append(layers, map[string]any{"filename": l + ".imagestacklayer"})
		}
		return writeJSON(dir, map[string]any{"layers": layers})
	}
	if err := stack("App Icon", 400, 240, []int{1, 2}, "tv"); err != nil {
		return err
	}
	if err := stack("App Icon - App Store", 1280, 768, []int{1}, "tv"); err != nil {
		return err
	}
	// The top shelf's: the app's image, flat, wide.
	shelf := func(name string, w, h int) error {
		dir := filepath.Join(ba, name+".imageset")
		var images []any
		for _, s := range []int{1, 2} {
			img, err := c.tvFlat(w*s, h*s, 0.45)
			if err != nil {
				return err
			}
			file := fmt.Sprintf("shelf@%dx.png", s)
			if err := writePNG(filepath.Join(dir, file), img); err != nil {
				return err
			}
			images = append(images, map[string]any{"filename": file, "idiom": "tv", "scale": fmt.Sprintf("%dx", s)})
		}
		return writeJSON(dir, map[string]any{"images": images})
	}
	if err := shelf("Top Shelf Image", 1920, 720); err != nil {
		return err
	}
	if err := shelf("Top Shelf Image Wide", 2320, 720); err != nil {
		return err
	}
	return writeJSON(ba, map[string]any{"assets": []any{
		map[string]any{"filename": "App Icon - App Store.imagestack", "idiom": "tv", "role": "primary-app-icon", "size": "1280x768"},
		map[string]any{"filename": "App Icon.imagestack", "idiom": "tv", "role": "primary-app-icon", "size": "400x240"},
		map[string]any{"filename": "Top Shelf Image Wide.imageset", "idiom": "tv", "role": "top-shelf-image-wide", "size": "2320x720"},
		map[string]any{"filename": "Top Shelf Image.imageset", "idiom": "tv", "role": "top-shelf-image", "size": "1920x720"},
	}})
}

// tvBrandAssets is the catalog's app icon set on tvOS, as Xcode names it.
const tvBrandAssets = "App Icon & Top Shelf Image"

// Android TV

// androidTVRes writes the TV launcher's banner (drawable-xhdpi, 320×180),
// for an app that declares tv.android.
func androidTVRes(c *appConfig, res string) (bool, error) {
	if !c.TV.Android {
		return false, nil
	}
	if c.Icon == "" && c.TV.Banner == "" {
		fmt.Println("asuka: tv.android without an icon or tv.banner: the TV launcher shows the app's name alone, and Google Play wants a banner")
		return false, nil
	}
	img, err := c.tvFlat(320, 180, 0.6)
	if err != nil {
		return false, err
	}
	return true, writePNG(filepath.Join(res, "drawable-xhdpi", "asuka_tv_banner.png"), img)
}

// androidTVManifest is what the manifest adds for a TV app: touch and
// the leanback library not required (Google Play offers the app to TVs,
// whose remotes have neither), and the banner.
func (c *appConfig) androidTVFeatures() string {
	if !c.TV.Android {
		return ""
	}
	return `    <uses-feature android:name="android.hardware.touchscreen" android:required="false" />
    <uses-feature android:name="android.software.leanback" android:required="false" />
`
}

// androidTVBanner is the application's banner attribute, for a TV app
// with one (androidTVRes).
func (c *appConfig) androidTVBanner() string {
	if !c.TV.Android || (c.Icon == "" && c.TV.Banner == "") {
		return ""
	}
	return ` android:banner="@drawable/asuka_tv_banner"`
}

// androidTVLauncher is the TV launcher's intent filter, for a TV app.
const androidTVLauncher = `            <intent-filter>
                <action android:name="android.intent.action.MAIN" />
                <category android:name="android.intent.category.LEANBACK_LAUNCHER" />
            </intent-filter>
`

// Apple TV's simulator

// pickTVSimulator returns the Apple TV simulator named want, or the one
// booted, or the newest Apple TV 4K, booting it if needed.
func pickTVSimulator(want string) (*simDevice, error) {
	out, err := exec.Command("xcrun", "simctl", "list", "devices", "available", "-j").Output()
	if err != nil {
		return nil, fmt.Errorf("simctl: %v (install Xcode)", err)
	}
	var list struct {
		Devices map[string][]simDevice `json:"devices"`
	}
	if err := json.Unmarshal(out, &list); err != nil {
		return nil, err
	}
	var runtimes []string
	for rt := range list.Devices {
		if strings.Contains(rt, "tvOS") {
			runtimes = append(runtimes, rt)
		}
	}
	if len(runtimes) == 0 {
		return nil, errors.New("no tvOS simulator runtime: install it with xcodebuild -downloadPlatform tvOS (or Xcode › Settings › Components)")
	}
	sort.Sort(sort.Reverse(sort.StringSlice(runtimes)))
	var all []*simDevice
	for _, rt := range runtimes {
		for i := range list.Devices[rt] {
			d := list.Devices[rt][i]
			d.runtime = rt
			if d.IsAvailable {
				all = append(all, &d)
			}
		}
	}
	var pick *simDevice
	for _, d := range all {
		switch {
		case want != "" && (d.UDID == want || d.Name == want):
			pick = d
		case want == "" && pick == nil && (d.State == "Booted" || d.State == "Booting"):
			pick = d
		}
	}
	if pick == nil && want == "" {
		for _, d := range all {
			if strings.HasPrefix(d.Name, "Apple TV 4K") && !strings.Contains(d.Name, "1080p") {
				pick = d
				break
			}
		}
		if pick == nil && len(all) > 0 {
			pick = all[0]
		}
	}
	if pick == nil && want == "" {
		// A runtime without a device: one made, as Xcode makes them.
		udid, err := output("xcrun", "simctl", "create", "Apple TV 4K", "com.apple.CoreSimulator.SimDeviceType.Apple-TV-4K-3rd-generation-4K", runtimes[0])
		if err != nil {
			return nil, fmt.Errorf("no Apple TV simulator, and making one failed: %v", err)
		}
		pick = &simDevice{Name: "Apple TV 4K", UDID: udid, State: "Shutdown", runtime: runtimes[0]}
	}
	if pick == nil {
		return nil, fmt.Errorf("no Apple TV simulator %q (see xcrun simctl list devices)", want)
	}
	if pick.State != "Booted" {
		fmt.Printf("asuka: booting %s\n", pick.Name)
		start := time.Now()
		if pick.State != "Booting" {
			if out, err := exec.Command("xcrun", "simctl", "boot", pick.UDID).CombinedOutput(); err != nil && !strings.Contains(string(out), "current state: Boot") {
				return nil, fmt.Errorf("simctl boot: %s", strings.TrimSpace(string(out)))
			}
		}
		exec.Command("xcrun", "simctl", "bootstatus", pick.UDID, "-b").Run()
		showSimulator(pick.UDID)
		fmt.Printf("asuka: %s booted in %.1fs\n", pick.Name, time.Since(start).Seconds())
	}
	return pick, nil
}

// devTVOS runs the app on an Apple TV simulator, as devIOS does on an
// iPhone's: the player, which loads the bytecode asuka dev sends.
func devTVOS(dir string, o *devOptions, stop <-chan os.Signal) error {
	app, err := loadApp(dir, platformTVOS)
	if err != nil {
		return err
	}
	tc, err := newAppleToolchain("appletvsimulator", o.verbose)
	if err != nil {
		return err
	}
	work, err := workDir(app, "tvos")
	if err != nil {
		return err
	}
	sim, err := pickTVSimulator(o.device)
	if err != nil {
		return err
	}
	fmt.Printf("asuka: %s on %s (%s)\n", app.name, sim.Name, sim.UDID)
	loop, err := newDevLoop(app, "127.0.0.1:0")
	if err != nil {
		return err
	}
	loop.flows = o.flows
	return loop.run(simulatorPlayer(app, tc, work, sim, loop), stop, o.once)
}
