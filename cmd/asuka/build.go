package main

import (
	"archive/zip"
	"errors"
	"fmt"
	"html"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"sort"
	"strings"

	"github.com/egoist/asuka/internal/appbuild"
	iupdate "github.com/egoist/asuka/internal/update"
)

type buildOptions struct {
	platform     string
	sign         string // macOS: a signing identity (default: ad hoc)
	notarize     string // macOS: notarytool's keychain profile ("env": ASUKA_APPLE_ID...)
	simulator    bool   // iOS: a release build for the simulator, unsigned
	distribution string // iOS: app-store, ad-hoc, development
	profile      string // iOS and macOS: a provisioning profile (path, name or UUID)
	appStore     bool   // macOS: a .pkg for the Mac App Store
	out          string
	xcframework  bool   // iOS: an .xcframework to embed in another app
	aar          bool   // Android: an .aar to embed in another app
	apk          bool   // Android: a release .apk to install directly
	update       bool   // iOS and Android: an over-the-air update of the app's code, not a build
	arch         string // the Mac, Linux and Windows: amd64 or arm64
	verbose      bool
	version      string // asuka.toml's version, for this build
	publishing   bool   // asuka publish: the update files are required
	base         string // web: the path the site is served at (asuka.toml's web.base)
	bin          string // a program of asuka.toml's [bin.<name>]
	target       string // a program's os-arch

	// dev, for iOS: asuka dev on a device (ios_device.go).
	dev *iosDevBuild
}

func buildFlags(o *buildOptions) *flagSet {
	fs := newFlagSet("build", "[dir]")
	fs.about = "Makes a release build of the app, in dist in its directory; of a program\n" +
		"(fn main, or --bin), an executable for this computer (or --target's)."
	fs.String(&o.bin, "bin", 0, "", "build the `program` asuka.toml's [bin.<name>] names, instead of the app")
	fs.String(&o.target, "target", 0, "", "a program's `os-arch`: darwin-amd64, darwin-arm64, linux-amd64, linux-arm64, windows-amd64 or windows-arm64 (default: this computer's; another needs Zig)")
	platformFlag(fs, &o.platform, []string{platformIOS, platformAndroid, platformMac, platformWindows, platformLinux, platformWeb},
		"the `platform` to build for: ios, an .ipa for the App Store (or --distribution); android, an .aab for Google Play; mac, an .app and its .zip in dist/darwin-<arch> (Apple silicon's; --arch amd64: Intel's); windows, a folder in dist/windows-<arch> with the .exe (its icon and version built in) and its assets, and its .zip; linux, a folder in dist/linux-<arch> laid out to install (the executable, its desktop file, icons, metainfo and assets), a .tar.gz of it and a Flatpak manifest; web, a static site (the core and the app as WebAssembly, and the web host) in dist/web. Default on macOS, Windows and Linux: this computer's")
	fs.String(&o.sign, "sign", 0, "", "macOS: a signing `identity`, e.g. \"Developer ID Application: ...\" (default: ad hoc)")
	fs.String(&o.notarize, "notarize", 0, "", "macOS: notarize the signed app and staple the ticket: a notarytool keychain `profile` (xcrun notarytool store-credentials), or \"env\" for ASUKA_APPLE_ID, ASUKA_APPLE_PASSWORD (app-specific) and ASUKA_TEAM_ID")
	fs.Bool(&o.simulator, "simulator", 0, "iOS: a release .app for the simulator instead (not signed for devices)")
	fs.Bool(&o.xcframework, "xcframework", 0, "iOS: AsukaApp.xcframework, the app to embed in an app made in Xcode (import AsukaApp; AsukaViewController())")
	fs.String(&o.base, "base", 0, "", "web: the `path` the site is served at, \"/app/\" for https://example.com/app/ (default: asuka.toml's web.base, else \"/\")")
	fs.String(&o.arch, "arch", 0, "", "the Mac: arm64 or amd64 (Intel; default: arm64); Linux: amd64 or arm64 (default: this machine's, else amd64); Windows: amd64 or arm64 (default: amd64, which runs on Windows on Arm too)")
	fs.Bool(&o.apk, "apk", 0, "Android: a signed release .apk with every ABI, to install with adb install (testing, sideloading), instead of the .aab for Google Play")
	fs.Bool(&o.update, "update", 0, "iOS, Android: an over-the-air update of the app's code instead of a build, in dist/updates: its module, signed, and its manifest, for the builds whose native side it fits (asuka.toml's updates)")
	fs.Bool(&o.aar, "aar", 0, "Android: an .aar, the app to embed in an app made in Android Studio (start dev.asuka.AsukaActivity)")
	fs.String(&o.distribution, "distribution", 0, "app-store", "iOS: app-store, ad-hoc or development")
	fs.String(&o.profile, "profile", 0, "", "iOS and macOS: a provisioning `profile`'s path, name or UUID (default: asuka.toml's (macOS: or ASUKA_MAC_PROFILE), else the best match installed; macOS: only for an app whose packages need one: push, Sign in with Apple, App Attest)")
	fs.Bool(&o.appStore, "appstore", 0, "macOS: a .pkg for the Mac App Store: the app sandboxed, signed with a Mac App Store profile's certificate (Apple Distribution), the .pkg with Mac Installer Distribution")
	fs.String(&o.out, "out", 'o', "", "the output `directory` (default: dist in the app's directory)")
	fs.String(&o.version, "version", 0, "", "the `version` to build, in place of asuka.toml's (as CI takes it from the tag: 1.2.0, 1.3.0-beta.1)")
	fs.Bool(&o.verbose, "verbose", 'v', "print the commands asuka runs")
	return fs
}

// setVersion makes --version the app's version for the build.
func (o *buildOptions) setVersion(app *asukaApp) error {
	if o.version == "" {
		return nil
	}
	v := strings.TrimPrefix(o.version, "v")
	if !iupdate.ValidVersion(v) {
		return fmt.Errorf("--version %q isn't a version (1.2.0, 1.3.0-beta.1)", o.version)
	}
	app.cfg.Version = v
	return nil
}

func cmdBuild(args []string) error {
	var o buildOptions
	fs := buildFlags(&o)
	if err := fs.parse(args); err != nil {
		return err
	}
	appbuild.Debug = false // a release: @debug is false
	dir := "."
	if fs.Arg(0) != "" {
		dir = fs.Arg(0)
	}
	if o.bin != "" || o.target != "" || (o.platform == "" && isProgramDir(dir)) {
		proj, err := appbuild.Load(dir)
		if err != nil {
			return err
		}
		if o.bin != "" {
			if proj, err = proj.Bin(o.bin); err != nil {
				return err
			}
		}
		return cmdBuildProgram(proj, &o)
	}
	if o.platform == "" {
		if o.platform = thisComputer(); o.platform == "" {
			return errors.New("usage: asuka build --platform ios|android|mac|windows|linux|web [flags] [dir]")
		}
	}
	app, err := loadApp(dir, o.platform)
	if err != nil {
		return err
	}
	if err := o.setVersion(app); err != nil {
		return err
	}
	if o.out == "" {
		o.out = filepath.Join(app.dir, "dist")
	}
	if err := os.MkdirAll(o.out, 0o755); err != nil {
		return err
	}
	if o.update {
		if o.platform != platformIOS && o.platform != platformAndroid {
			return errors.New("--update is iOS's and Android's: a desktop build writes its update itself")
		}
		_, _, err := writeOTAUpdate(app, o.platform, o.out)
		return err
	}
	switch o.platform {
	case platformIOS:
		if o.xcframework {
			return buildIOSFramework(app, &o)
		}
		return buildIOS(app, &o)
	case platformMac:
		return buildMac(app, &o)
	case platformWindows:
		return buildWindows(app, &o)
	case platformLinux:
		return buildLinux(app, &o)
	case platformWeb:
		return buildWeb(app, &o)
	}
	if o.aar {
		return buildAndroidAAR(app, &o)
	}
	return buildAndroid(app, &o)
}

// plist writes an XML property list of strings, bools, ints, string
// lists and nested dicts.
func plist(m map[string]any) string {
	var b strings.Builder
	b.WriteString(`<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
`)
	writePlist(&b, m, 0)
	b.WriteString("</plist>\n")
	return b.String()
}

func writePlist(b *strings.Builder, v any, depth int) {
	ind := strings.Repeat("\t", depth)
	switch x := v.(type) {
	case map[string]any:
		b.WriteString(ind + "<dict>\n")
		keys := make([]string, 0, len(x))
		for k := range x {
			keys = append(keys, k)
		}
		sort.Strings(keys)
		for _, k := range keys {
			fmt.Fprintf(b, "%s\t<key>%s</key>\n", ind, html.EscapeString(k))
			writePlist(b, x[k], depth+1)
		}
		b.WriteString(ind + "</dict>\n")
	case []string:
		b.WriteString(ind + "<array>\n")
		for _, s := range x {
			fmt.Fprintf(b, "%s\t<string>%s</string>\n", ind, html.EscapeString(s))
		}
		b.WriteString(ind + "</array>\n")
	case []any:
		b.WriteString(ind + "<array>\n")
		for _, e := range x {
			writePlist(b, e, depth+1)
		}
		b.WriteString(ind + "</array>\n")
	case []int:
		b.WriteString(ind + "<array>\n")
		for _, n := range x {
			fmt.Fprintf(b, "%s\t<integer>%d</integer>\n", ind, n)
		}
		b.WriteString(ind + "</array>\n")
	case bool:
		if x {
			b.WriteString(ind + "<true/>\n")
		} else {
			b.WriteString(ind + "<false/>\n")
		}
	case int:
		fmt.Fprintf(b, "%s<integer>%d</integer>\n", ind, x)
	default:
		fmt.Fprintf(b, "%s<string>%s</string>\n", ind, html.EscapeString(fmt.Sprint(x)))
	}
}

// desktopDir is where a desktop build for platform (darwin, windows,
// linux: as its updates' target names it) and arch goes: a directory of its own in the output, so that another
// architecture's build beside it (Apple silicon's and Intel's) doesn't
// write over it.
func desktopDir(o *buildOptions, platform, arch string) string {
	return filepath.Join(o.out, platform+"-"+arch)
}

// releaseFile is the path of a desktop build's file to hand out, in its
// directory: named for the app, its version, the platform and the
// architecture (notes-1.2.0-darwin-arm64.zip), as its update files are, so
// that every build's can go in one flat list (a GitHub release's).
func releaseFile(app *asukaApp, o *buildOptions, platform, arch, ext string) string {
	name := exeSlug(app.displayName(), app.bundleID) + "-" + app.cfg.Version + "-" + platform + "-" + arch + ext
	return filepath.Join(desktopDir(o, platform, arch), name)
}

// zipDir writes a .zip of dir at out, its entries under dir's name (a
// Windows build's folder, unzipped as it was).
func zipDir(dir, out string) error {
	tmp := out + ".tmp"
	f, err := os.Create(tmp)
	if err != nil {
		return err
	}
	zw := zip.NewWriter(f)
	base := filepath.Dir(dir)
	err = filepath.WalkDir(dir, func(p string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		rel, err := filepath.Rel(base, p)
		if err != nil {
			return err
		}
		info, err := d.Info()
		if err != nil {
			return err
		}
		h, err := zip.FileInfoHeader(info)
		if err != nil {
			return err
		}
		h.Name = filepath.ToSlash(rel)
		if d.IsDir() {
			h.Name += "/"
			_, err = zw.CreateHeader(h)
			return err
		}
		h.Method = zip.Deflate
		w, err := zw.CreateHeader(h)
		if err != nil {
			return err
		}
		src, err := os.Open(p)
		if err != nil {
			return err
		}
		defer src.Close()
		_, err = io.Copy(w, src)
		return err
	})
	if cerr := zw.Close(); err == nil {
		err = cerr
	}
	if cerr := f.Close(); err == nil {
		err = cerr
	}
	if err != nil {
		os.Remove(tmp)
		return err
	}
	return os.Rename(tmp, out)
}
