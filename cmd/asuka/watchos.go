package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"maps"
	"os"
	"os/exec"
	"path/filepath"
	"slices"
	"sort"
	"strconv"
	"strings"
	"time"

	"github.com/egoist/asuka/internal/appbuild"
	"github.com/egoist/asuka/internal/cbuild"
	"github.com/egoist/asuka/lang/bytecode"
	"github.com/egoist/asuka/lang/target"
)

// Apple Watch (docs/watch.md): the watch's own host, SwiftUI's
// (platform/watchos: watchOS has no UIKit views), driven by the same core,
// built with watchOS's SDK. The app's code is iOS's, as an Apple TV's is
// (@platform is .iOS there; environment().watch tells a watch). asuka dev
// runs it on a watch simulator; asuka build makes a watch-only app (watchOS
// 10's independent apps): a universal executable (arm64_32 for Series 4
// to 8, SE and Ultra 1; arm64 for Series 9 and later), in an .ipa whose
// iOS app is Xcode's watch-only stub, as App Store Connect takes one.

const watchosMin = cbuild.WatchOSMin

// watchArchs are what a watch build is for: both of a watch's
// architectures, or the simulator's (Apple silicon's).
func watchArchs(simulator bool) []string {
	if simulator {
		return []string{"arm64"}
	}
	return []string{"arm64_32", "arm64"}
}

// watchBundleID is the watch app's bundle ID: the app's, with
// .watchkitapp, as Xcode names a watch app (the app's own ID is its
// container's, App Store Connect's record).
func watchBundleID(id string) string { return id + ".watchkitapp" }

// dropWatchExtensions leaves out what a watch app has none of: a share
// target, the widgets' home screen sizes (its widgets are complications:
// the circular, rectangular, inline and corner sizes), Live Activities,
// App Shortcuts, push's rewrites, CarPlay; each said once.
func (c *appConfig) dropWatchExtensions() {
	var left []string
	if c.ShareTarget.on {
		c.ShareTarget.on = false
		left = append(left, "shareTarget")
	}
	watch := c.watchWidgets()
	if widgetSizeCount(watch) < widgetSizeCount(c.Widgets) || c.liveActivity {
		left = append(left, "home screen widgets or Live Activities")
	}
	c.Widgets, c.liveActivity = watch, false
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
		fmt.Printf("asuka: watchOS has no %s: the watch app goes without\n", strings.Join(left, ", "))
	}
}

func widgetSizeCount(ws []widgetConfig) int {
	n := 0
	for _, w := range ws {
		n += len(w.Sizes)
	}
	return n
}

// clone is a copy of c that a build can change apart from c: the watch
// app's, built in the iPhone app's build, drops what it has none of and
// takes what its own packages ask for, which the iPhone app's mustn't.
func (c *appConfig) clone() *appConfig {
	d := *c
	d.entitlements = maps.Clone(c.entitlements)
	d.plistArrays = maps.Clone(c.plistArrays)
	d.nativePlist = maps.Clone(c.nativePlist)
	d.entitlementsFor = maps.Clone(c.entitlementsFor)
	d.IOS.Usage = maps.Clone(c.IOS.Usage)
	d.IOS.InfoPlist = maps.Clone(c.IOS.InfoPlist)
	d.iosTrue = slices.Clone(c.iosTrue)
	d.hostFlags = slices.Clone(c.hostFlags)
	d.derived = slices.Clone(c.derived)
	d.nativeModules = slices.Clone(c.nativeModules)
	d.Widgets = slices.Clone(c.Widgets)
	return &d
}

// watchInfo is a watch app's Info.plist: id its bundle ID, companion the
// iOS app's ID it's paired with ("" for none), watchOnly whether it's a
// watch-only app (its iOS app the stub that carries it) or its iPhone
// app's ([watch] companion), which still runs without the iPhone app.
func (t *iosToolchain) watchInfo(cfg *appConfig, id, name string, companion string, watchOnly bool) (map[string]any, error) {
	sdkVersion, err := output("xcrun", "--sdk", t.sdkName, "--show-sdk-version")
	if err != nil {
		return nil, err
	}
	sdkBuild, _ := output("xcrun", "--sdk", t.sdkName, "--show-sdk-build-version")
	xcode, xcodeBuild := xcodeVersion()
	m := map[string]any{
		"CFBundleIdentifier":            id,
		"CFBundleExecutable":            appExe,
		"CFBundleName":                  name,
		"CFBundleDisplayName":           name,
		"CFBundlePackageType":           "APPL",
		"CFBundleShortVersionString":    firstOf(cfg.Version, "1.0"),
		"CFBundleVersion":               strconv.Itoa(max(cfg.Build, 1)),
		"CFBundleInfoDictionaryVersion": "6.0",
		"CFBundleDevelopmentRegion":     "en",
		"CFBundleSupportedPlatforms":    []string{t.bundlePlatform()},
		"MinimumOSVersion":              watchosMin,
		"UIDeviceFamily":                []int{4},
		// A SwiftUI app (its App's scene), not WatchKit's storyboard.
		"WKApplication":     true,
		"DTPlatformName":    t.sdkName,
		"DTPlatformVersion": sdkVersion,
		"DTSDKName":         t.sdkName + sdkVersion,
		"DTSDKBuild":        sdkBuild,
		"DTPlatformBuild":   sdkBuild,
		"DTXcode":           xcode,
		"DTXcodeBuild":      xcodeBuild,
		"DTCompiler":        "com.apple.compilers.llvm.clang.1_0",
	}
	if companion != "" {
		m["WKCompanionAppBundleIdentifier"] = companion
	}
	if watchOnly {
		// An independent app: it installs and runs without an iPhone app.
		m["WKWatchOnly"] = true
	} else {
		// Its iPhone app's, which installs on its own from the watch's
		// App Store too, and runs without the iPhone app near.
		m["WKRunsIndependentlyOfCompanionApp"] = true
	}
	if g := cfg.appGroup(); g != "" {
		// Where platform/widget leaves the complications' entries.
		m["AsukaAppGroup"] = g
	}
	if !t.device() {
		// asuka dev's player reaches the CLI on this Mac's loopback.
		m["NSAppTransportSecurity"] = map[string]any{"NSAllowsLocalNetworking": true}
	}
	switch cfg.IOS.NonExemptEncryption {
	case "yes":
		m["ITSAppUsesNonExemptEncryption"] = true
	case "ask":
	default:
		m["ITSAppUsesNonExemptEncryption"] = false
	}
	for k, v := range cfg.IOS.Usage {
		m[k] = v
	}
	for _, k := range cfg.iosTrue {
		m[k] = true
	}
	for k, v := range cfg.plistExtras() {
		m[k] = v
	}
	// iOS's window scenes and launch screen: a watch app is its SwiftUI
	// App's scene.
	delete(m, "UIApplicationSceneManifest")
	delete(m, "UILaunchScreen")
	for k, v := range cfg.IOS.InfoPlist {
		m[k] = v
	}
	cfg.plistLocalizations(m)
	return m, nil
}

// compileWatchAssets compiles the watch's icon (asuka.toml's icon: one
// 1024-point square, which watchOS masks to a circle) into bundle, its
// keys into Info.plist.
func (t *iosToolchain) compileWatchAssets(cfg *appConfig, work, bundle, infoPath string) error {
	if cfg.Icon == "" {
		return nil
	}
	cat := filepath.Join(work, "Watch.xcassets")
	os.RemoveAll(cat)
	img, err := loadIcon(cfg.Icon)
	if err != nil {
		return err
	}
	set := filepath.Join(cat, "AppIcon.appiconset")
	if err := writePNG(filepath.Join(set, "icon.png"), opaque(resize(img, 1024))); err != nil {
		return err
	}
	if err := writeCatalogJSON(cat, map[string]any{}); err != nil {
		return err
	}
	if err := writeCatalogJSON(set, map[string]any{"images": []any{
		map[string]any{"filename": "icon.png", "idiom": "universal", "platform": "watchos", "size": "1024x1024"},
	}}); err != nil {
		return err
	}
	partial := filepath.Join(work, "watch-assets-info.plist")
	if err := t.run(work, nil, "xcrun", "actool", "--compile", bundle, "--platform", t.sdkName,
		"--minimum-deployment-target", watchosMin, "--target-device", "watch", "--app-icon", "AppIcon",
		"--output-partial-info-plist", partial, "--output-format", "human-readable-text", cat); err != nil {
		return err
	}
	return t.run(work, nil, "/usr/libexec/PlistBuddy", "-c", "Merge "+partial, infoPath)
}

// linkWatch makes asuka dev's watch app (the player) in work: the watch's
// host and the core without an app, for the simulator.
func (t *iosToolchain) linkWatch(app *asukaApp, work, hostObj, lib string) (string, error) {
	cfg := app.cfg
	cfg.dropWatchExtensions()
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
	if err := t.finishWatch(app, work, bundle, app.bundleID, ""); err != nil {
		return "", err
	}
	sign := simulatorIdentity(cfg.IOS.Team)
	if err := t.signFrameworks(work, bundle, sign); err != nil {
		return "", err
	}
	return bundle, t.run(work, nil, "codesign", "--force", "--sign", sign, bundle)
}

// finishWatch puts what a watch app has besides its executable in bundle:
// its packages' frameworks and resources, its assets, Info.plist, its
// languages and its icon.
func (t *iosToolchain) finishWatch(app *asukaApp, work, bundle, id, companion string) error {
	cfg := app.cfg
	if err := t.embedPackages(bundle); err != nil {
		return err
	}
	if err := copyDir(filepath.Join(app.dir, "assets"), filepath.Join(bundle, "assets")); err != nil {
		return err
	}
	info, err := t.watchInfo(cfg, id, app.displayName(), companion, !cfg.Watch.Companion)
	if err != nil {
		return err
	}
	infoPath := filepath.Join(bundle, "Info.plist")
	if err := os.WriteFile(infoPath, []byte(plist(info)), 0o644); err != nil {
		return err
	}
	if err := cfg.writeLprojs(bundle); err != nil {
		return err
	}
	return t.compileWatchAssets(cfg, work, bundle, infoPath)
}

// buildWatch makes a release build of the watch app: with --simulator a
// release .app for the watch simulator; else the watch app, universal
// (arm64_32 and arm64) and signed with a watchOS profile for
// <id>.watchkitapp, in the watch-only stub app Xcode makes (signed with an
// iOS profile for <id>), packed as an .ipa for App Store Connect. An app
// whose watch app is its iPhone app's ([watch] companion) ships it in
// the iPhone app's .ipa instead (buildIOS).
func buildWatch(app *asukaApp, o *buildOptions) error {
	start := time.Now()
	cfg := app.cfg
	if cfg.Watch.Companion && !o.simulator {
		return errors.New("asuka.toml's [watch] companion: the watch app ships in the iPhone app (asuka build --platform ios makes its .ipa, with the watch app in it; asuka publish --platform ios uploads it)")
	}
	var prof, stubProf *profile
	if !o.simulator {
		var err error
		if prof, err = pickWatchProfile(cfg, cfg.IOS.Team, firstOf(o.profile, cfg.Watch.Profile), o.distribution); err != nil {
			return err
		}
		if stubProf, err = pickProfileOn(prof.team, cfg.ID, cfg.IOS.Profile, o.distribution, "", "iOS"); err != nil {
			return fmt.Errorf("the watch-only app's iOS stub (%s): %v", cfg.ID, err)
		}
		fmt.Printf("asuka: signing its iOS stub with %q\n", stubProf.name)
	}
	fmt.Printf("asuka: building %s %s (%d) for %s\n", cfg.Name, cfg.Version, cfg.Build, map[bool]string{true: "the watch simulator", false: "Apple Watch"}[o.simulator])
	w, err := makeWatchApp(app, o.simulator, prof, o.verbose)
	if err != nil {
		return err
	}
	tc, work, bundle := w.tc, w.work, w.bundle
	if cfg.Icon == "" {
		fmt.Println("asuka: no icon in asuka.toml; the App Store needs one (1024×1024 PNG)")
	}
	if o.simulator {
		out := filepath.Join(o.out, filepath.Base(bundle))
		os.RemoveAll(out)
		if err := tc.run(work, nil, "ditto", bundle, out); err != nil {
			return err
		}
		fmt.Printf("asuka: %s in %.0fs (install: xcrun simctl install <watch simulator> %s)\n", out, time.Since(start).Seconds(), out)
		return nil
	}
	stub, err := tc.watchStub(app, work, bundle)
	if err != nil {
		return err
	}
	stubTC, err := newAppleToolchain("iphoneos", o.verbose)
	if err != nil {
		return err
	}
	if err := stubProf.sign(stubTC, work, stub, cfg.ID, nil, "", "", nil); err != nil {
		return err
	}
	ipa := filepath.Join(o.out, safeName(cfg.Name)+".ipa")
	if err := packIPA(stubTC, work, stub, ipa); err != nil {
		return err
	}
	if dsyms, err := tc.writeSymbols(work, o.out, safeName(cfg.Name)); err != nil {
		return err
	} else if dsyms != "" {
		fmt.Println("asuka:", dsyms, "(its symbols: upload them to your crash service)")
	}
	fmt.Printf("asuka: %s in %.0fs\n", ipa, time.Since(start).Seconds())
	if prof.kind == "app-store" {
		fmt.Printf("asuka: asuka publish --platform watchos builds and uploads it to App Store Connect (or upload this one: asuka publish --platform watchos --file %s)\n", ipa)
	}
	return nil
}

// pickWatchProfile is the watchOS profile for the watch app
// (<id>.watchkitapp) of team, want naming one (or "": the best match).
func pickWatchProfile(cfg *appConfig, team, want, distribution string) (*profile, error) {
	prof, err := pickProfileOn(team, watchBundleID(cfg.ID), want, distribution, "", "watchOS")
	if err != nil {
		return nil, fmt.Errorf("the watch app (%s): %v", watchBundleID(cfg.ID), err)
	}
	fmt.Printf("asuka: signing the watch app with %q (%s, team %s) as %s\n", prof.name, prof.kind, prof.team, prof.identityName)
	return prof, nil
}

// watchBuilt is a release build of the watch app: its bundle, signed, in
// work, and the toolchain that built it.
type watchBuilt struct {
	tc           *iosToolchain
	work, bundle string
}

// makeWatchApp makes a release build of the watch app, signed: for the
// watch simulator, or (prof its watchOS profile) for watches. Its config
// is a copy of the app's, so what it goes without and what its packages
// ask of it stay the watch's. A watch-only app's ID is <id>.watchkitapp
// on a watch (the record's, <id>, its stub's) and <id> on the simulator,
// where it has no stub; a companion's ([watch] companion) is
// <id>.watchkitapp on both, paired with the iPhone app's.
func makeWatchApp(app *asukaApp, simulator bool, prof *profile, verbose bool) (*watchBuilt, error) {
	wapp := *app
	wapp.cfg = app.cfg.clone()
	app = &wapp
	cfg := app.cfg
	cfg.dropWatchExtensions()
	sdkName := "watchos"
	if simulator {
		sdkName = "watchsimulator"
	}
	if err := generateNatives(app); err != nil {
		return nil, err
	}
	m, err := app.compile(target.IOS)
	if err != nil {
		return nil, err
	}
	work, err := workDir(app, "watchos-release-"+sdkName)
	if err != nil {
		return nil, err
	}
	bundle := filepath.Join(work, safeName(cfg.Name)+".app")
	os.RemoveAll(bundle)
	if err := os.MkdirAll(bundle, 0o755); err != nil {
		return nil, err
	}
	exe := filepath.Join(bundle, appExe)
	// The watch's updates are its own (otaFingerprint): another
	// fingerprint and feed than the iPhone's and the Apple TV's.
	updates, err := app.otaUpdates(platformWatchOS, m)
	if err != nil {
		return nil, err
	}
	tc, err := linkWatchExe(app, m, updates, work, sdkName, exe, verbose)
	if err != nil {
		return nil, err
	}
	if err := tc.dsym(work, exe, filepath.Base(bundle)); err != nil {
		return nil, err
	}
	manifest, err := privacyManifest(app.dir, exe, cfg.IOS.Collected...)
	if err != nil {
		return nil, err
	}
	if err := os.WriteFile(filepath.Join(bundle, "PrivacyInfo.xcprivacy"), manifest, 0o644); err != nil {
		return nil, err
	}
	id, companion := watchBundleID(cfg.ID), cfg.ID
	if simulator && !cfg.Watch.Companion {
		id, companion = cfg.ID, ""
	}
	if err := tc.finishWatch(app, work, bundle, id, companion); err != nil {
		return nil, err
	}
	identity := simulatorIdentity(cfg.IOS.Team)
	if !simulator {
		identity = prof.identity
	}
	// Its complications: the widget extension, built for the watch,
	// signed before the app, whose signature seals it.
	if len(cfg.Widgets) > 0 {
		if err := embedWatchWidgets(app, sdkName, work, bundle, id, prof, verbose); err != nil {
			return nil, err
		}
	}
	if err := tc.signFrameworks(work, bundle, identity); err != nil {
		return nil, err
	}
	if simulator {
		err = tc.run(work, nil, "codesign", "--force", "--sign", identity, bundle)
	} else {
		err = prof.sign(tc, work, bundle, id, nil, cfg.appGroup(), "", cfg)
	}
	if err != nil {
		return nil, err
	}
	return &watchBuilt{tc: tc, work: work, bundle: bundle}, nil
}

// embedWatchWidgets builds the watch app's widget extension (its
// complications: asuka.toml's widgets in the watch's sizes, drawn by the
// iPhone's template, platform/ios's Widget, built for watchOS) for each of
// the SDK's architectures, and puts it in the watch app's PlugIns, signed
// (prof the watch app's profile, nil on the simulator), with the App Group
// the watch app leaves the entries in. Its ID is the watch app's (id),
// with .widgets.
func embedWatchWidgets(app *asukaApp, sdkName, work, bundle, id string, prof *profile, verbose bool) error {
	cfg := app.cfg
	simulator := sdkName == "watchsimulator"
	var exes []string
	var tc *iosToolchain
	for _, arch := range watchArchs(simulator) {
		var err error
		if tc, err = newAppleToolchainFor(sdkName, arch, verbose); err != nil {
			return err
		}
		tc.release = true
		w := filepath.Join(work, arch)
		if err := os.MkdirAll(w, 0o755); err != nil {
			return err
		}
		exe, err := tc.buildWidgets(w, cfg)
		if err != nil {
			return fmt.Errorf("the watch's widget extension: %w", err)
		}
		exes = append(exes, exe)
	}
	exe := filepath.Join(work, "widgets", widgetExe)
	if err := os.MkdirAll(filepath.Dir(exe), 0o755); err != nil {
		return err
	}
	if err := tc.run(work, nil, "xcrun", append([]string{"lipo", "-create", "-output", exe}, exes...)...); err != nil {
		return err
	}
	appInfo, err := tc.watchInfo(cfg, id, app.displayName(), "", true)
	if err != nil {
		return err
	}
	info := cfg.widgetInfo(appInfo)
	wid := id + ".widgets"
	info["CFBundleIdentifier"] = wid
	appex, err := embedWidgets(bundle, exe, info)
	if err != nil {
		return err
	}
	if err := tc.keepSymbols(work, exe, filepath.Base(appex)); err != nil {
		return err
	}
	if err := cfg.writeLprojs(appex); err != nil {
		return err
	}
	if simulator {
		return tc.run(work, nil, "codesign", "--force", "--sign", simulatorIdentity(cfg.IOS.Team), appex)
	}
	wprof, err := pickProfileOn(prof.team, wid, cfg.Watch.WidgetProfile, prof.kind, "", "watchOS")
	if err != nil {
		return fmt.Errorf("the watch's widget extension (%s): %v", wid, err)
	}
	fmt.Printf("asuka: signing the watch's widget extension with %q\n", wprof.name)
	return wprof.sign(tc, work, appex, wid, nil, cfg.appGroup(), "", nil)
}

// embedCompanionWatch builds the iPhone app's watch app ([watch]
// companion) and puts it in bundle's Watch folder, where an iPhone app
// carries its watch app (Xcode's layout since watchOS 7's single-target
// watch apps), its symbols beside the iPhone app's in work: before the
// iPhone app is signed, whose signature seals it.
func embedCompanionWatch(app *asukaApp, simulator bool, prof *profile, verbose bool, work, bundle string) error {
	fmt.Println("asuka: building its watch app")
	w, err := makeWatchApp(app, simulator, prof, verbose)
	if err != nil {
		return fmt.Errorf("the watch app: %w", err)
	}
	dst := filepath.Join(bundle, "Watch", filepath.Base(w.bundle))
	os.RemoveAll(filepath.Dir(dst))
	if err := os.MkdirAll(filepath.Dir(dst), 0o755); err != nil {
		return err
	}
	if err := w.tc.run(w.work, nil, "ditto", w.bundle, dst); err != nil {
		return err
	}
	dsyms, _ := os.ReadDir(filepath.Join(w.work, "dSYMs"))
	for _, d := range dsyms {
		// Named apart from the iPhone app's, whose bundle's name is the same.
		name := "Watch-" + d.Name()
		if err := w.tc.run(w.work, nil, "ditto", filepath.Join(w.work, "dSYMs", d.Name()), filepath.Join(work, "dSYMs", name)); err != nil {
			return err
		}
	}
	return nil
}

// linkWatchExe links the watch app's executable, with m compiled in (and
// its over-the-air updates', u, nil for none), into exe: each of the SDK's
// architectures' (watchArchs), then one of them all (lipo). It returns the
// last architecture's toolchain.
func linkWatchExe(app *asukaApp, m *bytecode.Module, u *appbuild.Updates, work, sdkName, exe string, verbose bool) (*iosToolchain, error) {
	cfg := app.cfg
	simulator := sdkName == "watchsimulator"
	var exes []string
	var tc *iosToolchain
	for _, arch := range watchArchs(simulator) {
		var err error
		if tc, err = newAppleToolchainFor(sdkName, arch, verbose); err != nil {
			return nil, err
		}
		tc.release = true
		tc.updates = u
		w := filepath.Join(work, arch)
		if err := os.MkdirAll(w, 0o755); err != nil {
			return nil, err
		}
		lib, err := tc.buildCore(w, m, nil)
		if err != nil {
			return nil, err
		}
		hostObj, err := tc.buildHost(w, app, usedFeatures(app, cfg))
		if err != nil {
			return nil, err
		}
		one := filepath.Join(w, "exe.app")
		os.RemoveAll(one)
		if err := os.MkdirAll(one, 0o755); err != nil {
			return nil, err
		}
		var ents []string
		if simulator {
			if ents, err = simulatorEntitlements(w, cfg); err != nil {
				return nil, err
			}
		}
		e, err := tc.linkApp(w, one, cfg, hostObj, lib, ents)
		if err != nil {
			return nil, err
		}
		exes = append(exes, e)
	}
	if err := os.MkdirAll(filepath.Dir(exe), 0o755); err != nil {
		return nil, err
	}
	if len(exes) == 1 {
		return tc, tc.run(work, nil, "cp", exes[0], exe)
	}
	return tc, tc.run(work, nil, "xcrun", append([]string{"lipo", "-create", "-output", exe}, exes...)...)
}

// watchStub makes the iOS app a watch-only app ships in (Xcode's
// "Watch-Only Application Stub"): the iPhone platform's stub executable,
// which never launches (LSApplicationLaunchProhibited), and the watch app
// in its Watch folder. App Store Connect reads ITSWatchOnlyContainer and
// offers the app on the watch's App Store alone.
func (t *iosToolchain) watchStub(app *asukaApp, work, watchApp string) (string, error) {
	cfg := app.cfg
	dir, err := output("xcrun", "--sdk", "iphoneos", "--show-sdk-platform-path")
	if err != nil {
		return "", err
	}
	src := filepath.Join(dir, "Library", "Application Support", "MessagesApplicationStub")
	stub := filepath.Join(work, "stub", safeName(cfg.Name)+".app")
	os.RemoveAll(filepath.Dir(stub))
	if err := os.MkdirAll(filepath.Join(stub, "Watch"), 0o755); err != nil {
		return "", err
	}
	exe := safeName(cfg.Name)
	if err := t.run(work, nil, "xcrun", "lipo", filepath.Join(src, "MessagesApplicationStub"), "-thin", "arm64", "-output", filepath.Join(stub, exe)); err != nil {
		return "", err
	}
	if err := t.run(work, nil, "ditto", watchApp, filepath.Join(stub, "Watch", filepath.Base(watchApp))); err != nil {
		return "", err
	}
	sdkVersion, _ := output("xcrun", "--sdk", "iphoneos", "--show-sdk-version")
	sdkBuild, _ := output("xcrun", "--sdk", "iphoneos", "--show-sdk-build-version")
	xcode, xcodeBuild := xcodeVersion()
	info := map[string]any{
		"CFBundleIdentifier":            cfg.ID,
		"CFBundleExecutable":            exe,
		"CFBundleName":                  app.displayName(),
		"CFBundleDisplayName":           app.displayName(),
		"CFBundlePackageType":           "APPL",
		"CFBundleShortVersionString":    firstOf(cfg.Version, "1.0"),
		"CFBundleVersion":               strconv.Itoa(max(cfg.Build, 1)),
		"CFBundleInfoDictionaryVersion": "6.0",
		"CFBundleDevelopmentRegion":     "en",
		"CFBundleSupportedPlatforms":    []string{"iPhoneOS"},
		"MinimumOSVersion":              "15.0",
		"UIDeviceFamily":                []int{1},
		"LSRequiresIPhoneOS":            true,
		"LSApplicationLaunchProhibited": true,
		"ITSWatchOnlyContainer":         true,
		"DTPlatformName":                "iphoneos",
		"DTPlatformVersion":             sdkVersion,
		"DTSDKName":                     "iphoneos" + sdkVersion,
		"DTSDKBuild":                    sdkBuild,
		"DTPlatformBuild":               sdkBuild,
		"DTXcode":                       xcode,
		"DTXcodeBuild":                  xcodeBuild,
		"DTCompiler":                    "com.apple.compilers.llvm.clang.1_0",
	}
	infoPath := filepath.Join(stub, "Info.plist")
	if err := os.WriteFile(infoPath, []byte(plist(info)), 0o644); err != nil {
		return "", err
	}
	// The stub's icon, Xcode's (the iPhone shows none: it never launches).
	partial := filepath.Join(work, "stub-assets-info.plist")
	if err := t.run(work, nil, "xcrun", "actool", "--compile", stub, "--platform", "iphoneos", "--minimum-deployment-target", "15.0",
		"--target-device", "iphone", "--app-icon", "MessagesApplicationStub", "--output-partial-info-plist", partial,
		"--output-format", "human-readable-text", filepath.Join(src, "MessagesApplicationStub.xcassets")); err != nil {
		return "", err
	}
	return stub, t.run(work, nil, "/usr/libexec/PlistBuddy", "-c", "Merge "+partial, infoPath)
}

// watchConfig is asuka.toml's [watch].
//
//	[watch]
//	companion = true            # the iPhone app carries the watch app (default: a watch-only app)
//	profile = "Counter Watch"   # the watch app's provisioning profile (default: the best match)
type watchConfig struct {
	// Companion makes the watch app the iPhone app's: asuka build
	// --platform ios puts it in the iPhone app's Watch folder, the two
	// share the App Store record and talk with platform/companion.
	// Without it, asuka build --platform watchos makes a watch-only app.
	Companion bool `json:"companion"`
	// Profile is the watch app's provisioning profile (its name, UUID or
	// path) for <id>.watchkitapp; --profile overrides it for a watch-only
	// app.
	Profile string `json:"profile"`
	// WidgetProfile is the watch's widget extension's (its
	// complications'), for <id>.watchkitapp.widgets (default: the best
	// match).
	WidgetProfile string `json:"widgetProfile"`
}

// Apple Watch's simulator

// pickWatchSimulator returns the watch simulator named want, or the one
// booted, or the newest Apple Watch's, made if there's none, booting it if
// needed. It boots one simulator, never another iPhone's.
func pickWatchSimulator(want string) (*simDevice, error) {
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
		if strings.Contains(rt, "watchOS") {
			runtimes = append(runtimes, rt)
		}
	}
	if len(runtimes) == 0 {
		return nil, errors.New("no watchOS simulator runtime: install it with xcodebuild -downloadPlatform watchOS (or Xcode › Settings › Components)")
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
			if strings.HasPrefix(d.Name, "Apple Watch Series") {
				pick = d
				break
			}
		}
		if pick == nil && len(all) > 0 {
			pick = all[0]
		}
	}
	if pick == nil && want == "" {
		typ, name, err := newestWatchType()
		if err != nil {
			return nil, err
		}
		udid, err := output("xcrun", "simctl", "create", name, typ, runtimes[0])
		if err != nil {
			return nil, fmt.Errorf("no Apple Watch simulator, and making one failed: %v", err)
		}
		pick = &simDevice{Name: name, UDID: udid, State: "Shutdown", runtime: runtimes[0]}
	}
	if pick == nil {
		return nil, fmt.Errorf("no Apple Watch simulator %q (see xcrun simctl list devices)", want)
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

// newestWatchType is the newest Apple Watch Series' device type, its
// larger size, as Xcode makes one: its identifier and name.
func newestWatchType() (string, string, error) {
	out, err := exec.Command("xcrun", "simctl", "list", "devicetypes", "-j").Output()
	if err != nil {
		return "", "", err
	}
	var list struct {
		DeviceTypes []struct {
			Name       string `json:"name"`
			Identifier string `json:"identifier"`
		} `json:"devicetypes"`
	}
	if err := json.Unmarshal(out, &list); err != nil {
		return "", "", err
	}
	best, bestName, bestN := "", "", -1
	for _, t := range list.DeviceTypes {
		rest, ok := strings.CutPrefix(t.Name, "Apple Watch Series ")
		if !ok {
			continue
		}
		num, size, _ := strings.Cut(rest, " ")
		n, err := strconv.Atoi(num)
		if err != nil {
			continue
		}
		// The larger of a series' two sizes: 46mm over 42mm.
		mm, _ := strconv.Atoi(strings.Trim(strings.TrimSuffix(strings.Trim(size, "()"), "mm"), " "))
		if n*1000+mm > bestN {
			best, bestName, bestN = t.Identifier, t.Name, n*1000+mm
		}
	}
	if best == "" {
		return "", "", errors.New("no Apple Watch simulator device type: install Xcode's watchOS platform (xcodebuild -downloadPlatform watchOS)")
	}
	return best, bestName, nil
}

// devWatchOS runs the app on an Apple Watch simulator, as devIOS does on
// an iPhone's: the player (the watch's host and the core without an app),
// which loads the bytecode asuka dev sends. The watch simulator shares
// this Mac's network, so it reaches asuka dev on the loopback.
func devWatchOS(dir string, o *devOptions, stop <-chan os.Signal) error {
	app, err := loadApp(dir, platformWatchOS)
	if err != nil {
		return err
	}
	tc, err := newAppleToolchain("watchsimulator", o.verbose)
	if err != nil {
		return err
	}
	work, err := workDir(app, "watchos")
	if err != nil {
		return err
	}
	sim, err := pickWatchSimulator(o.device)
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
