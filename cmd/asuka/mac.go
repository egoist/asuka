package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"image"
	"image/color"
	"math"
	"os"
	"os/exec"
	"path/filepath"
	"slices"
	"strconv"
	"strings"

	"github.com/egoist/asuka/internal/appconfig"
	"github.com/egoist/asuka/internal/cbuild"
	"github.com/egoist/asuka/internal/distpkg"
	iupdate "github.com/egoist/asuka/internal/update"
	"github.com/egoist/asuka/lang/target"
)

// macOS: the app's executable is the Swift host (platform/macos, AppKit)
// linked with the C core and the app's native packages' Swift
// (mac_swift.go), its settings in its Resources.
//
// asuka dev --platform mac runs the player here (desktop.go), from an
// .app like a release's (its Info.plist, icon and languages, signed ad
// hoc, answering for its own privacy prompts), with the core without an
// app, which loads the app's bytecode and each save's. asuka build
// --platform mac makes an .app for one architecture (--arch: arm64, the
// default, or amd64 for Intel Macs): an executable of the host and the
// core with the app compiled in, its
// Info.plist, icon and assets, signed (ad hoc unless --sign names an
// identity), with its provisioning profile
// (Contents/embedded.provisionprofile) and the entitlements the app's
// packages need of it (mac_profile.go), handed out as a .zip and a .dmg
// (mac_dmg.go); with --appstore, sandboxed, in a .pkg for the Mac App
// Store.

const macMin = "11.0"

// macArch is a Mac build's architecture: --arch's, arm64 (Apple
// silicon) unless amd64 (Intel) is asked for.
func macArch(arch string) (string, error) {
	switch arch {
	case "":
		return "arm64", nil
	case "arm64", "amd64":
		return arch, nil
	}
	return "", fmt.Errorf("--arch %q: arm64 or amd64", arch)
}

// macTarget is a Mac build's, for its updates: an architecture's own
// (an Intel install never takes Apple silicon's).
func macTarget(arch string) string { return "darwin-" + arch }

// macRunner runs a tool for a Mac build, its output shown only when it
// fails (or with -v).
func macRunner(verbose bool) func(dir string, env []string, cmd string, args ...string) error {
	return func(dir string, env []string, cmd string, args ...string) error {
		if verbose {
			fmt.Println("+", cmd, strings.Join(args, " "))
		}
		c := exec.Command(cmd, args...)
		c.Dir = dir
		// Apple's tools date no object they record (a debug map's, an
		// archive's members), as Xcode has them: a build's bytes are the
		// same whenever its objects were made (repro.go).
		c.Env = append(append(os.Environ(), "ZERO_AR_DATE=1"), env...)
		out, err := c.CombinedOutput()
		if err != nil {
			return fmt.Errorf("%s failed: %v\n%s", cmd, err, out)
		}
		return nil
	}
}

// writeMacBundle makes the .app at bundle, all but its executable: its
// Info.plist, icon and languages, and for a release its assets. It
// returns the executable's name, in Contents/MacOS.
func writeMacBundle(app *asukaApp, bundle, work string, release bool, run func(dir string, env []string, cmd string, args ...string) error) (string, error) {
	name := app.displayName()
	// The least macOS it runs on: the host's, or a native package's later
	// one (swiftMinimum).
	natives, err := nativeFor(app, "macos")
	if err != nil {
		return "", err
	}
	minimum, err := swiftMinimum(natives, "macos")
	if err != nil {
		return "", err
	}
	os.RemoveAll(bundle)
	contents := filepath.Join(bundle, "Contents")
	res := filepath.Join(contents, "Resources")
	for _, d := range []string{filepath.Join(contents, "MacOS"), res} {
		if err := os.MkdirAll(d, 0o755); err != nil {
			return "", err
		}
	}
	exe := sanitizeExe(name)
	info := map[string]any{
		"CFBundleExecutable":                   exe,
		"CFBundleIdentifier":                   app.bundleID,
		"CFBundleName":                         name,
		"CFBundleDisplayName":                  name,
		"CFBundlePackageType":                  "APPL",
		"CFBundleShortVersionString":           app.cfg.Version,
		"CFBundleVersion":                      strconv.Itoa(app.cfg.Build),
		"CFBundleInfoDictionaryVersion":        "6.0",
		"LSMinimumSystemVersion":               minimum,
		"NSHighResolutionCapable":              true,
		"NSPrincipalClass":                     "NSApplication",
		"NSSupportsAutomaticGraphicsSwitching": true,
		"LSApplicationCategoryType":            "public.app-category.utilities",
	}
	if t := app.cfg.urlTypes(false); len(t) > 0 {
		info["CFBundleURLTypes"] = t
	}
	if u := app.cfg.ubiquitousContainers(); u != nil {
		info["NSUbiquitousContainers"] = u
	}
	// In the background (a menu bar app): no Dock icon, from launch.
	if app.cfg.Background {
		info["LSUIElement"] = true
	}
	// What the app's plugins' Swift adds (pluginNeeds), where the app
	// doesn't set the key.
	for k, v := range app.cfg.nativePlist {
		if _, set := info[k]; !set {
			info[k] = v
		}
	}
	// Firebase's settings and file, for platform/analytics.
	for k, v := range app.cfg.analyticsPlist() {
		info[k] = v
	}
	if err := app.cfg.copyGoogleServices(res, app.bundleID); err != nil {
		return "", err
	}
	// The purpose strings are the same keys on macOS (camera, contacts...);
	// location's is NSLocationUsageDescription there.
	for k, v := range app.cfg.IOS.Usage {
		info[k] = v
	}
	if v := app.cfg.IOS.Usage["NSLocationWhenInUseUsageDescription"]; v != "" && info["NSLocationUsageDescription"] == nil {
		info["NSLocationUsageDescription"] = v
	}
	if app.cfg.Icon != "" {
		if err := macIcon(app.cfg.Icon, work, filepath.Join(res, "AppIcon.icns"), run); err != nil {
			return "", err
		}
		info["CFBundleIconFile"] = "AppIcon"
	}
	// The alternate icons (platform/appicon), shaped as the app's, which
	// the Dock shows while the app runs.
	for _, name := range app.cfg.alternateIconNames() {
		dir := filepath.Join(work, "AppIcons", name)
		if err := os.MkdirAll(dir, 0o755); err != nil {
			return "", err
		}
		if err := os.MkdirAll(filepath.Join(res, "AppIcons"), 0o755); err != nil {
			return "", err
		}
		if err := macIcon(app.cfg.AlternateIcons[name], dir, filepath.Join(res, "AppIcons", name+".icns"), run); err != nil {
			return "", err
		}
	}
	// asuka.toml's fileTypes, their icons as .icns files of their own.
	types := app.cfg.fileTypes()
	icons := map[int]string{}
	for i, t := range types {
		if t.icon == "" {
			continue
		}
		name := fmt.Sprintf("DocumentIcon%d", i+1)
		if err := macDocumentIcon(t.icon, work, filepath.Join(res, name+".icns"), run); err != nil {
			return "", fmt.Errorf("fileTypes[%d]'s icon: %w", i, err)
		}
		icons[i] = name
	}
	for k, v := range app.cfg.documentTypes(types, false, icons) {
		info[k] = v
	}
	app.cfg.plistLocalizations(info)
	if err := app.cfg.writeLprojs(res); err != nil {
		return "", err
	}
	if err := os.WriteFile(filepath.Join(contents, "Info.plist"), []byte(plist(info)), 0o644); err != nil {
		return "", err
	}
	if err := os.WriteFile(filepath.Join(contents, "PkgInfo"), []byte("APPL????"), 0o644); err != nil {
		return "", err
	}
	if release {
		if err := copyDir(filepath.Join(app.dir, "assets"), filepath.Join(res, "assets")); err != nil {
			return "", err
		}
	}
	return exe, nil
}

// macResourceEntitlements are the hardened runtime's entitlements an
// app needs for what its purpose strings ask for: without them, an app
// signed with a Developer ID (the hardened runtime) is refused the
// camera, the microphone or the user's data, prompt or not.
var macResourceEntitlements = map[string]string{
	"NSCameraUsageDescription":              "com.apple.security.device.camera",
	"NSMicrophoneUsageDescription":          "com.apple.security.device.audio-input",
	"NSLocationWhenInUseUsageDescription":   "com.apple.security.personal-information.location",
	"NSContactsUsageDescription":            "com.apple.security.personal-information.addressbook",
	"NSCalendarsUsageDescription":           "com.apple.security.personal-information.calendars",
	"NSCalendarsFullAccessUsageDescription": "com.apple.security.personal-information.calendars",
	"NSPhotoLibraryUsageDescription":        "com.apple.security.personal-information.photos-library",
	"NSPhotoLibraryAddUsageDescription":     "com.apple.security.personal-information.photos-library",
	"NSBluetoothAlwaysUsageDescription":     "com.apple.security.device.bluetooth",
}

// writeMacEntitlements writes the app's entitlements for its signature
// without a provisioning profile into work, and returns the file ("" if
// it needs none). The ones only a profile grants (push's aps-environment,
// Sign in with Apple, App Attest) aren't among them: signed with those
// and no profile, macOS won't open the app.
func writeMacEntitlements(app *asukaApp, work string) (string, error) {
	ents, err := macEntitlements(app.cfg, nil, false)
	if err != nil {
		return "", err
	}
	return writeEntitlementsFile(ents, work)
}

// writeEntitlementsFile writes ents into work, and returns the file (""
// for none).
func writeEntitlementsFile(ents map[string]any, work string) (string, error) {
	if len(ents) == 0 {
		return "", nil
	}
	f := filepath.Join(work, "entitlements.plist")
	return f, os.WriteFile(f, []byte(plist(ents)), 0o644)
}

// signMac signs the bundle with identity ("-": ad hoc) and entitlements
// (a file, or ""), and checks it.
func signMac(bundle, identity, entitlements string, run func(dir string, env []string, cmd string, args ...string) error) error {
	args := []string{"--force", "--sign", identity}
	if identity != "-" {
		args = append(args, "--options", "runtime", "--timestamp")
	}
	dir := filepath.Dir(bundle)
	// What the bundle holds first (the app's Swift library and the
	// runtime it ships, its Swift packages' frameworks), each signed as
	// the app is,
	// without the entitlements, which are the executable's: with the
	// hardened runtime, the program loads only libraries signed by its
	// own team.
	libs, _ := filepath.Glob(filepath.Join(bundle, "Contents", "Frameworks", "*.dylib"))
	fws, _ := filepath.Glob(filepath.Join(bundle, "Contents", "Frameworks", "*.framework")) // its Swift packages'
	helpers, _ := filepath.Glob(filepath.Join(bundle, "Contents", "Helpers", "*"))          // its programs ([bin])
	for _, lib := range append(append(libs, fws...), helpers...) {
		if err := run(dir, nil, "codesign", append(slices.Clip(args), lib)...); err != nil {
			return err
		}
	}
	if entitlements != "" {
		args = append(args, "--entitlements", entitlements)
	}
	if err := run(dir, nil, "codesign", append(args, bundle)...); err != nil {
		return err
	}
	return run(dir, nil, "codesign", "--verify", "--strict", bundle)
}

func buildMac(app *asukaApp, o *buildOptions) error {
	if o.appStore && o.notarize != "" {
		return errors.New("asuka: --notarize is for an app distributed outside the Mac App Store, which reviews the app itself: leave it out with --appstore")
	}
	arch, err := macArch(o.arch)
	if err != nil {
		return err
	}
	work, err := workDir(app, "mac-release")
	if err != nil {
		return err
	}
	// What the native packages' Swift needs goes in the Info.plist and
	// signature.
	natives, err := nativeFor(app, "macos")
	if err != nil {
		return err
	}
	if err := app.cfg.nativeNeeds(natives, "macos"); err != nil {
		return err
	}
	// The profile first: no point building for one that isn't there.
	prof, identity, err := macSigning(app.cfg, o)
	if err != nil {
		return err
	}
	ents, err := macEntitlements(app.cfg, prof, o.appStore)
	if err != nil {
		return err
	}
	run := macRunner(o.verbose)
	dir := desktopDir(o, "darwin", arch)
	bundle := filepath.Join(dir, app.displayName()+".app")
	exe, err := writeMacBundle(app, bundle, work, true, run)
	if err != nil {
		return err
	}

	// The host and the core with the app compiled in, for its
	// architecture. Updates are the Mac App Store's for its builds.
	m, err := app.compile(target.MacOS)
	if err != nil {
		return err
	}
	exePath := filepath.Join(bundle, "Contents", "MacOS", exe)
	mb := &macBuild{app: app, work: work, release: true, run: run}
	if err := mb.executable(exePath, arch, m); err != nil {
		return err
	}
	if err := mb.embed(bundle, exePath); err != nil {
		return err
	}
	if err := writeHostConfig(app, platformMac, macTarget(arch), !o.appStore, filepath.Join(bundle, "Contents", "Resources", appconfig.File)); err != nil {
		return err
	}
	// The project's programs, in Helpers (a CLI the app ships, which a
	// user links onto their PATH): signed with the app too.
	if _, err := bundlePrograms(app, cbuild.Target{OS: "darwin", Arch: arch}, filepath.Join(bundle, "Contents", "Helpers"), o.verbose); err != nil {
		return err
	}

	if prof != nil {
		data, err := os.ReadFile(prof.path)
		if err != nil {
			return err
		}
		if err := os.WriteFile(filepath.Join(bundle, "Contents", "embedded.provisionprofile"), data, 0o644); err != nil {
			return err
		}
	}
	entsFile, err := writeEntitlementsFile(ents, work)
	if err != nil {
		return err
	}
	if err := signMac(bundle, identity, entsFile, run); err != nil {
		return err
	}
	if o.notarize != "" {
		if identity == "-" {
			return errors.New("asuka: --notarize needs --sign \"Developer ID Application: ...\": Apple notarizes only apps signed with a Developer ID")
		}
		if err := notarize(work, bundle, o.notarize, o.verbose); err != nil {
			return err
		}
	}
	fmt.Printf("asuka: %s\n", bundle)
	if !o.appStore {
		pkgs, err := packagesFor(o, app.cfg.Mac.Packages, macPackages, "mac")
		if err != nil {
			return err
		}
		removeUnmade(app, o, "darwin", arch, map[string]string{pkgZip: ".zip", pkgDMG: ".dmg"}, pkgs)
		if pkgs[pkgZip] {
			// The app to hand out: zipped as Apple's tools do (its
			// signature and stapled ticket kept), named for its version
			// and Macs.
			zip := releaseFile(app, o, "darwin", arch, ".zip")
			os.Remove(zip)
			// Its files dated by its sources and their times written in
			// UTC (a .zip's are local times): the same .zip from the same
			// sources, wherever and whenever it's made (repro.go).
			if err := stampTree(bundle, distpkg.Epoch()); err != nil {
				return err
			}
			if err := run(dir, []string{"TZ=UTC"}, "ditto", "-c", "-k", "--keepParent", bundle, zip); err != nil {
				return err
			}
			fmt.Printf("asuka: %s\n", zip)
		}
		if pkgs[pkgDMG] {
			dmg := releaseFile(app, o, "darwin", arch, ".dmg")
			if err := macDMG(app, bundle, dmg, identity, work, run); err != nil {
				return err
			}
			if o.notarize != "" {
				if err := notarize(work, dmg, o.notarize, o.verbose); err != nil {
					return err
				}
			}
			fmt.Printf("asuka: %s, a disk image: the app and the Applications folder, to drag it to\n", dmg)
		}
	}
	if !o.appStore {
		// The update files, of the app as notarized and stapled.
		b := updateBuild{target: macTarget(arch), kind: iupdate.Bundle, dir: dir, entries: []string{filepath.Base(bundle)}, root: bundle}
		if _, err := writeUpdates(app, o.out, b, o.publishing); err != nil {
			return err
		}
	}
	if o.appStore {
		pkg, err := macPackage(app, bundle, prof, identity, releaseFile(app, o, "darwin", arch, ".pkg"), run)
		if err != nil {
			return err
		}
		fmt.Printf("asuka: %s (upload it with Transporter, or xcrun altool --upload-package)\n", pkg)
		return nil
	}
	if identity == "-" {
		if prof != nil {
			fmt.Println("asuka: signed ad hoc, with the profile's entitlements: macOS opens it only signed with a certificate the profile allows (--sign)")
		} else {
			fmt.Println("asuka: signed ad hoc; to distribute it, sign with --sign \"Developer ID Application: ...\" and --notarize")
		}
	}
	return nil
}

// macSigning is the profile (nil for none) and the identity ("-": ad
// hoc) a Mac build signs with: the profile --profile, ASUKA_MAC_PROFILE or
// asuka.toml names, else for an app whose packages need one (or for the
// Mac App Store) the best match installed; the identity --sign names, else
// the profile's, else ad hoc. Without a profile, it says what won't work.
func macSigning(cfg *appConfig, o *buildOptions) (*macProfile, string, error) {
	want := o.profile
	if want == "" {
		want = os.Getenv("ASUKA_MAC_PROFILE")
	}
	if want == "" {
		want = cfg.Mac.Profile
		if o.appStore {
			want = cfg.Mac.AppStoreProfile
		}
	}
	needs := macProfileNeeds(cfg)
	if want == "" && !o.appStore && len(needs) == 0 {
		identity := o.sign
		if identity == "" {
			identity = "-" // ad hoc: runs on this Mac; others need a Developer ID and notarization
		}
		return nil, identity, nil
	}
	ids := identities()
	prof, err := pickMacProfile(cfg.IOS.Team, cfg.ID, want, o.appStore, ids)
	if err != nil {
		if want != "" || o.appStore {
			return nil, "", err
		}
		// None installed: the app builds, without what needs one.
		fmt.Printf("asuka: %s won't work on the Mac without a provisioning profile: %v (or set ASUKA_MAC_PROFILE, or asuka.toml's mac.profile)\n",
			strings.Join(needs, ", "), err)
		identity := o.sign
		if identity == "" {
			identity = "-"
		}
		return nil, identity, nil
	}
	identity, name, err := macIdentity(o.sign, prof, ids)
	if err != nil {
		return nil, "", err
	}
	note := ""
	if !prof.signed {
		note = ", not signed by Apple"
	}
	fmt.Printf("asuka: provisioning profile %q (%s, team %s%s), signing as %s\n", prof.name, prof.kind, prof.team, note, name)
	return prof, identity, nil
}

// macPackage makes the Mac App Store's .pkg of the signed app at pkg,
// signed with the team's Mac Installer Distribution identity (unsigned
// only for a profile that isn't Apple's, to look at).
func macPackage(app *asukaApp, bundle string, prof *macProfile, identity, pkg string, run func(dir string, env []string, cmd string, args ...string) error) (string, error) {
	os.Remove(pkg)
	args := []string{"--component", bundle, "/Applications"}
	if prof.signed || identity != "-" {
		installer := installerIdentity(prof.team)
		if installer == "" {
			return "", fmt.Errorf("no Mac Installer Distribution identity for team %s in the keychain, which signs the .pkg: make one in Xcode (Settings › Accounts › Manage Certificates) or at developer.apple.com", prof.team)
		}
		args = append(args, "--sign", installer)
	} else {
		fmt.Println("asuka: the .pkg isn't signed (the profile isn't Apple's): App Store Connect won't take it")
	}
	if err := run(filepath.Dir(bundle), nil, "productbuild", append(args, pkg)...); err != nil {
		return "", err
	}
	return pkg, nil
}

// notarize sends the signed app (a bundle, or a disk image of it) to
// Apple's notary service, waits for its verdict, and staples the ticket
// to it, so Gatekeeper opens it offline too. profile is notarytool's
// keychain profile, or "env" for an Apple ID's app-specific password from
// the environment (CI).
func notarize(work, bundle, profile string, verbose bool) error {
	submit := bundle
	if filepath.Ext(bundle) == ".app" {
		// A bundle goes zipped; a disk image as it is.
		submit = filepath.Join(work, "notarize.zip")
		os.Remove(submit)
		if out, err := exec.Command("ditto", "-c", "-k", "--keepParent", bundle, submit).CombinedOutput(); err != nil {
			return fmt.Errorf("ditto failed: %v\n%s", err, out)
		}
		defer os.Remove(submit)
	}
	creds, err := notaryCredentials(profile, os.Getenv)
	if err != nil {
		return err
	}
	fmt.Printf("asuka: notarizing %s (Apple's notary service; usually a few minutes)...\n", filepath.Base(bundle))
	args := append([]string{"notarytool", "submit", submit, "--wait", "--output-format", "json"}, creds...)
	if verbose {
		fmt.Println("+ xcrun", strings.Join(redacted(args), " "))
	}
	out, err := exec.Command("xcrun", args...).Output()
	id, status := notaryResult(out)
	if err != nil && status == "" {
		var stderr []byte
		if ee, ok := err.(*exec.ExitError); ok {
			stderr = ee.Stderr
		}
		return fmt.Errorf("xcrun notarytool submit failed: %v\n%s%s", err, out, stderr)
	}
	if status != "Accepted" {
		log, _ := exec.Command("xcrun", append([]string{"notarytool", "log", id}, creds...)...).CombinedOutput()
		return fmt.Errorf("asuka: notarization %s (submission %s):\n%s", strings.ToLower(status), id, log)
	}
	if out, err := exec.Command("xcrun", "stapler", "staple", bundle).CombinedOutput(); err != nil {
		return fmt.Errorf("xcrun stapler staple failed: %v\n%s", err, out)
	}
	fmt.Println("asuka: notarized, and the ticket stapled")
	return nil
}

// notaryCredentials are notarytool's credential flags for --notarize.
func notaryCredentials(profile string, getenv func(string) string) ([]string, error) {
	if profile != "env" {
		return []string{"--keychain-profile", profile}, nil
	}
	id, pw, team := getenv("ASUKA_APPLE_ID"), getenv("ASUKA_APPLE_PASSWORD"), getenv("ASUKA_TEAM_ID")
	if id == "" || pw == "" || team == "" {
		return nil, errors.New("asuka: --notarize env needs ASUKA_APPLE_ID, ASUKA_APPLE_PASSWORD (an app-specific password) and ASUKA_TEAM_ID")
	}
	return []string{"--apple-id", id, "--password", pw, "--team-id", team}, nil
}

// redacted is args with the password after --password hidden.
func redacted(args []string) []string {
	out := append([]string(nil), args...)
	for i := range out {
		if out[i] == "--password" && i+1 < len(out) {
			out[i+1] = "***"
		}
	}
	return out
}

// notaryResult reads notarytool's JSON: the submission's id and status
// (Accepted, Invalid, Rejected...).
func notaryResult(out []byte) (id, status string) {
	var r struct {
		ID     string `json:"id"`
		Status string `json:"status"`
	}
	if i := strings.IndexByte(string(out), '{'); i >= 0 {
		json.Unmarshal(out[i:], &r)
	}
	return r.ID, r.Status
}

// sanitizeExe is the executable's name: the app's, without slashes.
func sanitizeExe(name string) string {
	s := strings.Map(func(r rune) rune {
		if r == '/' || r == ':' {
			return '-'
		}
		return r
	}, name)
	if s == "" {
		return "app"
	}
	return s
}

// macIcon makes the .icns from a square icon: macOS's shape, a rounded
// square inset in the canvas (824 of 1024 points), at each size iconutil
// wants.
func macIcon(src, work, out string, run func(dir string, env []string, cmd string, args ...string) error) error {
	img, err := loadIcon(src)
	if err != nil {
		return err
	}
	shaped := macShape(resize(img, 824))
	canvas := newNRGBA(1024)
	for y := range 824 {
		for x := range 824 {
			canvas.SetNRGBA(x+100, y+100, shaped.NRGBAAt(x, y))
		}
	}
	set := filepath.Join(work, "AppIcon.iconset")
	os.RemoveAll(set)
	for _, s := range []int{16, 32, 128, 256, 512} {
		if err := writePNG(filepath.Join(set, fmt.Sprintf("icon_%dx%d.png", s, s)), resize(canvas, s)); err != nil {
			return err
		}
		if err := writePNG(filepath.Join(set, fmt.Sprintf("icon_%dx%d@2x.png", s, s)), resize(canvas, s*2)); err != nil {
			return err
		}
	}
	return run(work, nil, "iconutil", "-c", "icns", "-o", out, set)
}

// macDocumentIcon makes a file type's .icns from a PNG, as it is: a
// document's icon has its own shape (a page, say), which the PNG draws.
func macDocumentIcon(src, work, out string, run func(dir string, env []string, cmd string, args ...string) error) error {
	img, err := loadIcon(src)
	if err != nil {
		return err
	}
	set := filepath.Join(work, strings.TrimSuffix(filepath.Base(out), ".icns")+".iconset")
	os.RemoveAll(set)
	for _, s := range []int{16, 32, 128, 256, 512} {
		if err := writePNG(filepath.Join(set, fmt.Sprintf("icon_%dx%d.png", s, s)), resize(img, s)); err != nil {
			return err
		}
		if err := writePNG(filepath.Join(set, fmt.Sprintf("icon_%dx%d@2x.png", s, s)), resize(img, s*2)); err != nil {
			return err
		}
	}
	return run(work, nil, "iconutil", "-c", "icns", "-o", out, set)
}

// macShape masks an icon to a rounded square (radius 22.5% of its side),
// antialiased.
func macShape(src *image.NRGBA) *image.NRGBA {
	n := src.Bounds().Dx()
	r := float64(n) * 0.225
	dst := image.NewNRGBA(src.Bounds())
	for y := range n {
		for x := range n {
			// Coverage of the pixel's center by the rounded square.
			cx, cy := float64(x)+0.5, float64(y)+0.5
			dx := math.Max(math.Max(r-cx, cx-(float64(n)-r)), 0)
			dy := math.Max(math.Max(r-cy, cy-(float64(n)-r)), 0)
			d := math.Hypot(dx, dy) - r
			cov := math.Min(math.Max(0.5-d, 0), 1)
			if dx == 0 || dy == 0 {
				cov = 1
			}
			c := src.NRGBAAt(x, y)
			c.A = uint8(float64(c.A) * cov)
			dst.SetNRGBA(x, y, color.NRGBA{c.R, c.G, c.B, c.A})
		}
	}
	return dst
}
