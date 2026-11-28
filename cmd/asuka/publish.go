package main

import (
	"archive/zip"
	"bytes"
	"encoding/xml"
	"errors"
	"fmt"
	"io"
	"maps"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"slices"
	"strconv"
	"strings"
	"unicode/utf8"

	"github.com/egoist/asuka/internal/appbuild"
)

// asuka publish: a release build, numbered after the store's last,
// uploaded, and handed to testers or review. The stores' APIs: App Store
// Connect's (appstore.go) and Google Play's (play.go).

type publishOptions struct {
	buildOptions
	file         string
	build        int
	notes        string
	noWait       bool
	groups       string
	review       bool
	release      string
	track        string
	rollout      float64
	draft        bool
	cancelReview bool
	repo         string
}

func publishFlags(o *publishOptions) *flagSet {
	fs := newFlagSet("publish", "[dir]")
	fs.about = "Makes a release build of the app and uploads it to App Store Connect or Google Play; a desktop app's, with its update, to asuka.toml's updates (GitHub's releases, an S3 bucket)."
	platformFlag(fs, &o.platform, []string{platformIOS, platformTVOS, platformWatchOS, platformAndroid, platformMac, platformWindows, platformLinux},
		"the `platform` whose store to upload to: ios, App Store Connect (TestFlight and the App Store); tvos, likewise, as the app's Apple TV platform; watchos, likewise, a watch-only app (its record's iOS platform); android, Google Play; mac, windows or linux, the update files where asuka.toml's updates say (installed apps update to it), with the packages to hand out (the Mac's .zip and .dmg; Windows' .zip, installer and .msix; Linux's .tar.gz, .deb, .rpm and AppImage)")
	fs.String(&o.sign, "sign", 0, "", "macOS: a signing `identity`, e.g. \"Developer ID Application: ...\" (default: ad hoc)")
	fs.String(&o.notarize, "notarize", 0, "", "macOS: notarize the app and staple the ticket: a notarytool keychain `profile`, or \"env\"")
	fs.Bool(&o.appStore, "appstore", 0, "macOS: upload to App Store Connect for the Mac App Store and TestFlight, as iOS does (--groups, --review, --notes), a .pkg asuka build --appstore makes, in place of the update files")
	fs.String(&o.arch, "arch", 0, "", "the Mac: arm64 or amd64 (Intel); Linux and Windows: amd64 or arm64")
	packageFlag(fs, &o.packages)
	fs.String(&o.version, "version", 0, "", "the `version` to build, in place of asuka.toml's (as CI takes it from the tag)")
	fs.String(&o.file, "file", 0, "", "upload this .ipa (tvOS's for --platform tvos, the watch-only app's for watchos), .aab or (macOS, --appstore) .pkg from asuka build instead of building one")
	fs.Int(&o.build, "build", 0, 0, "the build `number` (default: one more than the store's highest, at least asuka.toml's build)")
	fs.String(&o.notes, "notes", 0, "", "what's new: text in the store's default language, @file likewise, or @dir with a file per language as the store names it (en-US.txt); TestFlight's What to Test and, with --review, the App Store's What's New; Play's release notes")
	fs.String(&o.groups, "groups", 0, "", "iOS, tvOS, watchOS, Mac App Store: the TestFlight groups to give the build to, by name, comma separated (with an external group, the build goes to beta review)")
	fs.Bool(&o.review, "review", 0, "iOS, tvOS, watchOS, Mac App Store: submit the version, with this build, for App Store review")
	fs.String(&o.release, "release", 0, "", "iOS, tvOS, watchOS, Mac App Store, with --review: once approved, release it automatically (auto) or by hand (manual); default: as App Store Connect has it")
	fs.Bool(&o.noWait, "no-wait", 0, "iOS, tvOS, watchOS, Mac App Store: stop once App Store Connect has the upload, without waiting for Apple to process it")
	fs.String(&o.track, "track", 0, "internal", "Android: the track to release to: internal, alpha, beta, production, or a closed track's name")
	fs.Float(&o.rollout, "rollout", 0, 0, "Android: roll the release out to this `fraction` of users, as 0.1, raised later in the Play Console (default: all)")
	fs.Bool(&o.update, "update", 0, "iOS, tvOS, watchOS, Android: an over-the-air update of the app's code, not a store build: uploaded where asuka.toml's updates say, for the platform's installed builds whose native side it fits")
	fs.Bool(&o.draft, "draft", 0, "Android: a draft release, to roll out in the Play Console; mac, windows, linux on GitHub: upload to a draft release, which you publish once every platform is there (default: published)")
	fs.String(&o.repo, "repo", 0, "", "linux: write the .deb and the .rpm into an apt and a dnf repository, signed with the Linux key (ASUKA_LINUX_GPG_KEY), keeping the versions it has: a `directory`, or s3://bucket/prefix (default: asuka.toml's linux.repository)")
	fs.Bool(&o.cancelReview, "cancel-review", 0, "Android: if the app has changes in review, cancel that review and send them with this release (default: stop)")
	fs.String(&o.profile, "profile", 0, "", "iOS, tvOS, watchOS (the watch app's): a provisioning `profile`'s path, name or UUID (default: asuka.toml's, else the best match installed)")
	fs.String(&o.out, "out", 'o', "", "the output `directory` of the build (default: dist in the app's directory)")
	fs.Bool(&o.verbose, "verbose", 'v', "print the commands asuka runs and its requests to the store")
	variantFlags(fs)
	return fs
}

func cmdPublish(args []string) error {
	var o publishOptions
	fs := publishFlags(&o)
	if err := fs.parse(args); err != nil {
		return err
	}
	appbuild.Debug = false // a release: @debug is false
	if err := useVariant(); err != nil {
		return err
	}
	dir := "."
	if fs.Arg(0) != "" {
		dir = fs.Arg(0)
	}
	if err := o.check(fs.set); err != nil {
		return err
	}
	if o.update {
		if !otaPlatform(o.platform) {
			return errors.New("--update is iOS's, tvOS's, watchOS's and Android's: a desktop app's update is published with each build")
		}
		app, err := loadRelease(dir, o.platform)
		if err != nil {
			return err
		}
		return publishOTA(app, &o)
	}
	if desktopPlatform(o.platform) && !(o.platform == platformMac && o.appStore) {
		app, err := loadRelease(dir, o.platform)
		if err != nil {
			return err
		}
		return publishDesktop(app, &o)
	}
	notes, err := readNotes(o.notes)
	if err != nil {
		return err
	}
	app, err := loadRelease(dir, o.platform)
	if err != nil {
		return err
	}
	if appStorePlatform(o.platform) {
		key, err := loadASCKey(app.cfg)
		if err != nil {
			return err
		}
		c := newASC(key, o.verbose)
		c.platform = ascPlatform(o.platform)
		return publishIOS(c, app, &o, notes, buildForStore)
	}
	sa, err := loadServiceAccount(app.cfg)
	if err != nil {
		return err
	}
	return publishAndroid(newPlay(sa, o.verbose), app, &o, notes, buildForStore)
}

// appStorePlatform reports whether --platform publishes to App Store
// Connect (the Mac's with --appstore).
func appStorePlatform(platform string) bool {
	return platform == platformIOS || platform == platformTVOS || platform == platformWatchOS || platform == platformMac
}

// ascPlatform is App Store Connect's platform of a --platform: its builds,
// versions and reviews are each platform's own. A watch-only app is an
// iOS app's record (its .ipa the iPhone's stub that carries the watch
// app, as Xcode exports one), so its platform is IOS.
func ascPlatform(platform string) string {
	switch platform {
	case platformTVOS:
		return "TV_OS"
	case platformMac:
		return "MAC_OS"
	}
	return "IOS"
}

// check refuses flags that don't go together, before anything is built.
func (o *publishOptions) check(set map[string]bool) error {
	if o.platform == "" {
		return errors.New("usage: asuka publish --platform ios|tvos|watchos|android|mac|windows|linux [flags] [dir]")
	}
	// tvOS's, watchOS's and the Mac App Store's: App Store Connect's, as
	// iOS's.
	ios := o.platform == platformIOS || o.platform == platformTVOS || o.platform == platformWatchOS || o.platform == platformMac && o.appStore
	only := func(platform string, names ...string) error {
		for _, n := range names {
			if set[n] {
				return fmt.Errorf("--%s is for --platform %s", n, platform)
			}
		}
		return nil
	}
	if o.platform != platformLinux {
		if err := only("linux", "repo"); err != nil {
			return err
		}
	}
	if desktopPlatform(o.platform) && !ios {
		// The release notes are the changelog's; the rest is the stores'.
		if err := only("ios or android", "file", "build", "notes", "groups", "review", "release", "no-wait", "track", "rollout", "cancel-review", "profile"); err != nil {
			return err
		}
		if o.platform != platformMac {
			if err := only("mac", "sign", "notarize"); err != nil {
				return err
			}
		}
		return nil
	}
	if o.platform == platformMac {
		// What a Mac App Store build takes; it's signed by its profile.
		if err := only("mac without --appstore", "sign", "notarize", "draft", "package"); err != nil {
			return err
		}
	} else if err := only("mac, windows or linux", "sign", "notarize", "arch", "version", "package"); err != nil {
		return err
	}
	if ios {
		if err := only("android", "track", "rollout", "draft", "cancel-review"); err != nil {
			return err
		}
	} else if err := only("ios", "groups", "review", "release", "no-wait", "profile"); err != nil {
		return err
	}
	want := map[bool]string{true: ".ipa", false: ".aab"}[ios]
	if o.platform == platformMac {
		want = ".pkg"
	}
	if o.file != "" && !strings.EqualFold(filepath.Ext(o.file), want) {
		return fmt.Errorf("--file %s: a %s", o.file, want)
	}
	if o.file != "" && set["build"] {
		return errors.New("--build numbers a build asuka makes; the --file's number is its own")
	}
	if set["build"] && o.build <= 0 {
		return errors.New("--build: a number above 0")
	}
	switch o.release {
	case "", "auto", "manual":
	default:
		return fmt.Errorf("--release %q: auto or manual", o.release)
	}
	if o.release != "" && !o.review {
		return errors.New("--release goes with --review")
	}
	if o.noWait && (o.groups != "" || o.notes != "" || o.review) {
		return errors.New("--no-wait: --groups, --notes and --review need the build processed, so they wait")
	}
	if set["rollout"] && (o.rollout <= 0 || o.rollout >= 1) {
		return fmt.Errorf("--rollout %v: a fraction of users above 0 and below 1, as 0.1", o.rollout)
	}
	if o.draft && set["rollout"] {
		return errors.New("--draft or --rollout: a draft isn't rolled out")
	}
	return nil
}

// buildForStore makes the release build the store takes: its path.
func buildForStore(app *asukaApp, o *buildOptions) (string, error) {
	if o.out == "" {
		o.out = variantOut(filepath.Join(app.dir, "dist"))
		if o.platform == platformTVOS || o.platform == platformWatchOS {
			o.out = filepath.Join(o.out, o.platform) // as asuka build's: beside iOS's .ipa, of the same name
		}
	}
	if err := os.MkdirAll(o.out, 0o755); err != nil {
		return "", err
	}
	if o.platform == platformIOS || o.platform == platformTVOS {
		o.distribution = "app-store"
		return ipaPath(app.cfg, o), buildIOS(app, o)
	}
	if o.platform == platformWatchOS {
		o.distribution = "app-store"
		return ipaPath(app.cfg, o), buildWatch(app, o)
	}
	if o.platform == platformMac {
		// The Mac App Store's .pkg (buildMac writes it there).
		o.appStore = true
		arch, err := macArch(o.arch)
		if err != nil {
			return "", err
		}
		return releaseFile(app, o, "darwin", arch, ".pkg"), buildMac(app, o)
	}
	return aabPath(app.cfg, o), buildAndroid(app, o)
}

func ipaPath(cfg *appConfig, o *buildOptions) string {
	return filepath.Join(o.out, safeName(cfg.Name)+".ipa")
}

func aabPath(cfg *appConfig, o *buildOptions) string {
	return filepath.Join(o.out, safeName(cfg.Name)+".aab")
}

// nextBuild is the build number to publish: want if given, which must
// be above the store's highest; else one more than the store's highest,
// and at least asuka.toml's.
func nextBuild(highest, configured, want int) (int, error) {
	if want > 0 {
		if want <= highest {
			return 0, fmt.Errorf("--build %d: the store has build %d already; a build must be numbered higher", want, highest)
		}
		return want, nil
	}
	return max(highest+1, configured), nil
}

// readNotes reads --notes: text, or @file, for the store's default
// language (the key ""); or @dir, a file per language (<locale>.txt).
func readNotes(s string) (map[string]string, error) {
	if strings.TrimSpace(s) == "" {
		return nil, nil
	}
	path, ok := strings.CutPrefix(s, "@")
	if !ok {
		return map[string]string{"": strings.TrimSpace(s)}, nil
	}
	st, err := os.Stat(path)
	if err != nil {
		return nil, fmt.Errorf("--notes: %v", err)
	}
	if !st.IsDir() {
		b, err := os.ReadFile(path)
		if err != nil {
			return nil, err
		}
		if strings.TrimSpace(string(b)) == "" {
			return nil, fmt.Errorf("--notes: %s is empty", path)
		}
		return map[string]string{"": strings.TrimSpace(string(b))}, nil
	}
	entries, err := os.ReadDir(path)
	if err != nil {
		return nil, err
	}
	notes := map[string]string{}
	for _, e := range entries {
		locale, ok := strings.CutSuffix(e.Name(), ".txt")
		if !ok || e.IsDir() {
			continue
		}
		b, err := os.ReadFile(filepath.Join(path, e.Name()))
		if err != nil {
			return nil, err
		}
		if t := strings.TrimSpace(string(b)); t != "" {
			notes[locale] = t
		}
	}
	if len(notes) == 0 {
		return nil, fmt.Errorf("--notes: no <locale>.txt with text in %s (as en-US.txt)", path)
	}
	return notes, nil
}

// localizeNotes gives the default language's notes ("") the store's
// default language, and checks each fits the store's limit.
func localizeNotes(notes map[string]string, lang string, limit int, what string) (map[string]string, error) {
	if len(notes) == 0 {
		return nil, nil
	}
	out := map[string]string{}
	for k, v := range notes {
		if k == "" {
			k = lang
		}
		if n := utf8.RuneCountInString(v); n > limit {
			return nil, fmt.Errorf("--notes (%s): %d characters; %s takes at most %d", k, n, what, limit)
		}
		out[k] = v
	}
	return out, nil
}

// ipaInfo is what an .ipa's Info.plist says of it: its bundle ID,
// version, build, and the SDK it was built with (DTPlatformName:
// iphoneos, appletvos; "" where it doesn't say); and of the watch app in
// its Watch folder, if it has one, the SDK that was built with (watchos),
// and whether the iPhone app only carries it (ITSWatchOnlyContainer: a
// watch-only app's stub).
type ipaInfo struct {
	id, version, build, sdk string
	watch                   string
	watchOnly               bool
}

var (
	appPlist   = regexp.MustCompile(`^Payload/[^/]+\.app/Info\.plist$`)
	watchPlist = regexp.MustCompile(`^Payload/[^/]+\.app/Watch/[^/]+\.app/Info\.plist$`)
)

// readPlistStrings reads a property list's top-level strings (and bools,
// as "true" and "false"): an XML one, or a binary one through plutil.
func readPlistStrings(path string, data []byte) (map[string]string, error) {
	if bytes.HasPrefix(data, []byte("bplist")) {
		// A binary property list: plutil reads it (macOS).
		c := exec.Command("plutil", "-convert", "xml1", "-o", "-", "--", "-")
		c.Stdin = bytes.NewReader(data)
		var err error
		if data, err = c.Output(); err != nil {
			return nil, fmt.Errorf("%s: its Info.plist is binary, which plutil (macOS) reads: %v", path, err)
		}
	}
	m, err := plistStrings(data)
	if err != nil {
		return nil, fmt.Errorf("%s: Info.plist: %v", path, err)
	}
	return m, nil
}

func readIPAInfo(path string) (ipaInfo, error) {
	zr, err := zip.OpenReader(path)
	if err != nil {
		return ipaInfo{}, fmt.Errorf("%s: %v", path, err)
	}
	defer zr.Close()
	read := func(f *zip.File) (map[string]string, error) {
		rc, err := f.Open()
		if err != nil {
			return nil, err
		}
		data, err := io.ReadAll(rc)
		rc.Close()
		if err != nil {
			return nil, err
		}
		return readPlistStrings(path, data)
	}
	var info *ipaInfo
	watch := ""
	for _, f := range zr.File {
		switch {
		case appPlist.MatchString(f.Name):
			m, err := read(f)
			if err != nil {
				return ipaInfo{}, err
			}
			info = &ipaInfo{id: m["CFBundleIdentifier"], version: m["CFBundleShortVersionString"], build: m["CFBundleVersion"],
				sdk: m["DTPlatformName"], watchOnly: m["ITSWatchOnlyContainer"] == "true"}
			if info.id == "" || info.version == "" || info.build == "" {
				return ipaInfo{}, fmt.Errorf("%s: its Info.plist lacks the bundle ID, version or build", path)
			}
		case watchPlist.MatchString(f.Name):
			m, err := read(f)
			if err != nil {
				return ipaInfo{}, err
			}
			if watch = m["DTPlatformName"]; watch == "" {
				watch = "watchos" // a watch app's, whatever it says
			}
		}
	}
	if info == nil {
		return ipaInfo{}, fmt.Errorf("%s: no Payload/<app>.app/Info.plist: not an .ipa", path)
	}
	info.watch = watch
	return *info, nil
}

// readPkgInfo is a Mac App Store .pkg's app's bundle ID, version and
// build: its PackageInfo's bundle (pkgutil expands it; macOS).
func readPkgInfo(path string) (ipaInfo, error) {
	dir, err := os.MkdirTemp("", "asuka-pkg-*")
	if err != nil {
		return ipaInfo{}, err
	}
	defer os.RemoveAll(dir)
	out := filepath.Join(dir, "x")
	if b, err := exec.Command("pkgutil", "--expand-full", path, out).CombinedOutput(); err != nil {
		return ipaInfo{}, fmt.Errorf("%s: pkgutil (macOS) can't expand it: %v\n%s", path, err, b)
	}
	plists, _ := filepath.Glob(filepath.Join(out, "*", "Payload", "*.app", "Contents", "Info.plist"))
	if len(plists) == 0 {
		plists, _ = filepath.Glob(filepath.Join(out, "Payload", "*.app", "Contents", "Info.plist"))
	}
	if len(plists) == 0 {
		return ipaInfo{}, fmt.Errorf("%s: no <app>.app/Contents/Info.plist in its payload: not an app's .pkg", path)
	}
	data, err := exec.Command("plutil", "-convert", "xml1", "-o", "-", plists[0]).Output()
	if err != nil {
		return ipaInfo{}, fmt.Errorf("%s: its Info.plist: %v", path, err)
	}
	m, err := plistStrings(data)
	if err != nil {
		return ipaInfo{}, fmt.Errorf("%s: Info.plist: %v", path, err)
	}
	info := ipaInfo{id: m["CFBundleIdentifier"], version: m["CFBundleShortVersionString"], build: m["CFBundleVersion"]}
	if info.id == "" || info.version == "" || info.build == "" {
		return ipaInfo{}, fmt.Errorf("%s: its Info.plist lacks the bundle ID, version or build", path)
	}
	return info, nil
}

// checkIPAPlatform fails if an .ipa isn't one for --platform flag (ios,
// tvos, watchos), which App Store Connect would refuse only once it's
// uploaded, or would take as another app: an iPhone's .ipa given to
// --platform tvos, an Apple TV's to --platform ios; a watch-only app's
// (the iPhone stub that only carries it) given to ios, or an iPhone app
// to watchos.
func checkIPAPlatform(path string, info ipaInfo, flag string) error {
	sdk := info.sdk
	want := map[string]string{"ios": "iphoneos", "tvos": "appletvos", "watchos": "iphoneos"}[flag]
	if sdk != "" && want != "" && sdk != want {
		what := map[string]string{"iphoneos": "an iPhone and iPad app", "appletvos": "an Apple TV app",
			"iphonesimulator": "a simulator's app", "appletvsimulator": "a simulator's app", "watchsimulator": "a simulator's app"}[sdk]
		if what == "" {
			what = "an app for " + sdk
		}
		return fmt.Errorf("%s is %s (built with %s's SDK), not one for --platform %s: asuka build --platform %s makes that", path, what, sdk, flag, flag)
	}
	switch {
	case flag == platformIOS && info.watchOnly:
		return fmt.Errorf("%s is a watch-only app (an iPhone app that only carries its watch app), not one for --platform ios: publish it with --platform watchos", path)
	case flag == platformWatchOS && !info.watchOnly:
		if info.watch != "" {
			return fmt.Errorf("%s is an iPhone app with its watch app in it, which goes up as the iPhone app does: publish it with --platform ios", path)
		}
		return fmt.Errorf("%s isn't a watch-only app (it has no watch app): asuka build --platform watchos makes one", path)
	case flag == platformWatchOS && info.watch != "watchos":
		return fmt.Errorf("%s's watch app is built with %s's SDK, not a watch's: asuka build --platform watchos makes one for the App Store", path, info.watch)
	}
	return nil
}

// publishFlag is the --platform an App Store Connect publish is for: o's,
// or the one of App Store Connect's platform.
func publishFlag(o *publishOptions, c *ascClient) string {
	if o.platform != "" {
		return o.platform
	}
	return map[string]string{"IOS": platformIOS, "TV_OS": platformTVOS, "MAC_OS": platformMac}[c.plat()]
}

// plistStrings reads an XML property list's top-level string values.
func plistStrings(data []byte) (map[string]string, error) {
	d := xml.NewDecoder(bytes.NewReader(data))
	out := map[string]string{}
	depth := 0
	key := ""
	for {
		tok, err := d.Token()
		if err == io.EOF {
			return out, nil
		}
		if err != nil {
			return nil, err
		}
		switch t := tok.(type) {
		case xml.StartElement:
			depth++
			// plist › dict › key, value
			if depth != 3 {
				continue
			}
			switch t.Name.Local {
			case "key", "string":
				var s string
				if err := d.DecodeElement(&s, &t); err != nil {
					return nil, err
				}
				depth--
				if t.Name.Local == "key" {
					key = s
				} else if key != "" {
					out[key], key = s, ""
				}
			case "true", "false":
				if key != "" {
					out[key], key = t.Name.Local, ""
				}
			default:
				key = "" // a value of another type
			}
		case xml.EndElement:
			depth--
		}
	}
}

// publishIOS builds the app (or takes --file), uploads it to App Store
// Connect, waits for Apple to process it, and hands it to TestFlight's
// groups and to App Review as asked.
func publishIOS(c *ascClient, app *asukaApp, o *publishOptions, notes map[string]string, build func(*asukaApp, *buildOptions) (string, error)) error {
	return ascHint(publishIOSSteps(c, app, o, notes, build), c.key)
}

func publishIOSSteps(c *ascClient, app *asukaApp, o *publishOptions, notes map[string]string, build func(*asukaApp, *buildOptions) (string, error)) error {
	cfg := app.cfg
	id, version := cfg.ID, cfg.Version
	var info ipaInfo
	if o.file != "" {
		var err error
		read := readIPAInfo
		if strings.EqualFold(filepath.Ext(o.file), ".pkg") {
			read = readPkgInfo
		}
		if info, err = read(o.file); err != nil {
			return err
		}
		if info.id != cfg.ID {
			return fmt.Errorf("%s is %s, but asuka.toml's id is %s", o.file, info.id, cfg.ID)
		}
		if err := checkIPAPlatform(o.file, info, publishFlag(o, c)); err != nil {
			return err
		}
		version = info.version
	}
	// Everything the store can refuse, before the build.
	a, err := c.findApp(id)
	if err != nil {
		return err
	}
	fmt.Printf("asuka: %s (%s) on App Store Connect\n", a.Attributes.Name, id)
	if err := c.checkPlatform(a.ID); err != nil {
		return err
	}
	if err := c.checkVersionOpen(a.ID, version); err != nil {
		return err
	}
	groups, err := c.betaGroups(a.ID, splitList(o.groups))
	if err != nil {
		return err
	}
	if notes, err = localizeNotes(notes, a.Attributes.PrimaryLocale, 4000, "App Store Connect"); err != nil {
		return err
	}
	path := o.file
	if path == "" {
		high, err := c.highestBuild(a.ID)
		if err != nil {
			return err
		}
		n, err := nextBuild(high, cfg.Build, o.build)
		if err != nil {
			return err
		}
		cfg.Build = n
		if high > 0 {
			fmt.Printf("asuka: build %d (App Store Connect's highest is %d)\n", n, high)
		}
		if path, err = build(app, &o.buildOptions); err != nil {
			return err
		}
		info = ipaInfo{id: id, version: version, build: strconv.Itoa(n)}
	}
	uploadID, err := c.upload(a.ID, path, info.version, info.build)
	if err != nil {
		return err
	}
	if o.noWait {
		fmt.Printf("asuka: App Store Connect has %s (%s); Apple is processing it, and emails when it's done\n", info.version, info.build)
		return nil
	}
	buildID, err := c.processed(a.ID, uploadID, info.version, info.build)
	if err != nil {
		return err
	}
	if len(notes) > 0 {
		if err := c.betaNotes(buildID, notes); err != nil {
			return err
		}
	}
	if err := c.testFlight(buildID, groups); err != nil {
		return err
	}
	if o.review {
		release := map[string]string{"auto": "AFTER_APPROVAL", "manual": "MANUAL"}[o.release]
		if err := c.submitReview(a.ID, buildID, info.version, notes, release); err != nil {
			return err
		}
	}
	fmt.Printf("asuka: %s %s (%s) is on App Store Connect, in TestFlight\n", a.Attributes.Name, info.version, info.build)
	return nil
}

// ascHint says what to do about App Store Connect's refusals of the key.
func ascHint(err error, key *ascKey) error {
	var ae *ascError
	if !errors.As(err, &ae) {
		return err
	}
	switch ae.status {
	case http.StatusUnauthorized:
		return fmt.Errorf("%v\nApp Store Connect doesn't know the key: check its ID (%s), its issuer's ID and its .p8, and that it isn't revoked (Users and Access › Integrations)", err, key.id)
	case http.StatusForbidden:
		return fmt.Errorf("%v\nthe key %s may lack the role for this (App Manager or Admin), or an agreement awaits the account holder in App Store Connect (Business)", err, key.id)
	}
	return err
}

// publishAndroid builds the app (or takes --file), uploads it to Google
// Play and releases it on the track.
func publishAndroid(p *playClient, app *asukaApp, o *publishOptions, notes map[string]string, build func(*asukaApp, *buildOptions) (string, error)) error {
	cfg := app.cfg
	pkg := androidPackage(cfg.ID)
	// A look first: the highest version code and the listing's language,
	// and whether Play takes this app from this account at all.
	edit, err := p.insertEdit(pkg)
	if err != nil {
		return playHint(err, p.sa, pkg)
	}
	high, err := p.highestVersionCode(pkg, edit)
	lang := ""
	if err == nil && len(notes) > 0 {
		lang, err = p.defaultLanguage(pkg, edit)
	}
	p.deleteEdit(pkg, edit)
	if err != nil {
		return playHint(err, p.sa, pkg)
	}
	if notes, err = localizeNotes(notes, lang, 500, "Google Play"); err != nil {
		return err
	}
	fmt.Printf("asuka: %s on Google Play\n", pkg)
	path := o.file
	name := ""
	if path == "" {
		n, err := nextBuild(int(high), cfg.Build, o.build)
		if err != nil {
			return err
		}
		cfg.Build = n
		if high > 0 {
			fmt.Printf("asuka: version code %d (Google Play's highest is %d)\n", n, high)
		}
		if path, err = build(app, &o.buildOptions); err != nil {
			return err
		}
		name = fmt.Sprintf("%s (%d)", cfg.Version, n)
	}
	if edit, err = p.insertEdit(pkg); err != nil {
		return playHint(err, p.sa, pkg)
	}
	committed := false
	defer func() {
		if !committed {
			p.deleteEdit(pkg, edit)
		}
	}()
	vc, err := p.uploadBundle(pkg, edit, path)
	if err != nil {
		return playHint(err, p.sa, pkg)
	}
	rel := playRelease{Name: name, VersionCodes: []string{strconv.FormatInt(vc, 10)}, Status: "completed"}
	switch {
	case o.draft:
		rel.Status = "draft"
	case o.rollout > 0:
		rel.Status, rel.UserFraction = "inProgress", o.rollout
	}
	for _, lang := range slices.Sorted(maps.Keys(notes)) {
		rel.ReleaseNotes = append(rel.ReleaseNotes, playText{Language: lang, Text: notes[lang]})
	}
	status, err := p.updateTrack(pkg, edit, o.track, rel)
	if err != nil {
		return playHint(fmt.Errorf("releasing to the %s track: %w", o.track, err), p.sa, pkg)
	}
	sent, err := p.commit(pkg, edit, o.cancelReview)
	if err != nil {
		return playHint(err, p.sa, pkg)
	}
	committed = true
	what := map[string]string{"completed": "released", "inProgress": fmt.Sprintf("rolling out to %g%% of users", o.rollout*100), "draft": "a draft"}[status]
	fmt.Printf("asuka: version code %d is on Google Play's %s track, %s\n", vc, o.track, what)
	if !sent {
		fmt.Println("asuka: Play holds the app's changes for you to send for review: send them in the Play Console (Publishing overview)")
	}
	return nil
}

func splitList(s string) []string {
	var out []string
	for _, x := range strings.Split(s, ",") {
		if x = strings.TrimSpace(x); x != "" {
			out = append(out, x)
		}
	}
	return out
}
