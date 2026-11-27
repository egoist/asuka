package main

import (
	"debug/elf"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"slices"
	"strings"

	"github.com/egoist/asuka/internal/cbuild"
	"github.com/egoist/asuka/internal/distpkg"
	iupdate "github.com/egoist/asuka/internal/update"
)

// The packages a desktop build hands the app out in, besides the app
// itself: on the Mac its .zip and a .dmg; on Windows the folder's .zip,
// an installer (windows_installer.go) and an MSIX (windows_msix.go); on
// Linux a .tar.gz of the prefix, a .deb, an .rpm, an AppImage and a
// Flatpak manifest, each written in Go (internal/distpkg) or built with
// Zig, from any system. --package picks some, else asuka.toml's
// mac.packages, windows.packages or linux.packages, else the platform's
// defaults: all but the MSIX, which is for the Store and sideloading,
// and wants the app's package identity.

// The packages, as --package and asuka.toml name them.
const (
	pkgZip       = "zip"
	pkgDMG       = "dmg"
	pkgTarGz     = "tar.gz"
	pkgDeb       = "deb"
	pkgRPM       = "rpm"
	pkgAppImage  = "appimage"
	pkgFlatpak   = "flatpak"
	pkgInstaller = "installer"
	pkgMSIX      = "msix"
)

var (
	macPackages     = []string{pkgZip, pkgDMG}
	linuxPackages   = []string{pkgTarGz, pkgDeb, pkgRPM, pkgAppImage, pkgFlatpak}
	windowsPackages = []string{pkgZip, pkgInstaller, pkgMSIX}
	// defaultPackages are a platform's packages when nothing picks them,
	// if not all.
	defaultPackages = map[string][]string{"windows": {pkgZip, pkgInstaller}}
)

// packagesFor is the set of packages a build makes, of all (a platform's):
// --package's (o.packages), else configured (asuka.toml's), else the
// platform's defaults (defaultPackages; all).
func packagesFor(o *buildOptions, configured, all []string, platform string) (map[string]bool, error) {
	from, where := o.packages, "--package"
	if len(from) == 0 {
		from, where = configured, "asuka.toml's "+platform+".packages"
	}
	if len(from) == 0 {
		from = all
		if d := defaultPackages[platform]; d != nil {
			from = d
		}
	}
	set := map[string]bool{}
	for _, p := range from {
		p = strings.ToLower(strings.TrimSpace(p))
		switch p {
		case "tgz", "tarball":
			p = pkgTarGz
		case "setup", "exe", "nsis":
			p = pkgInstaller
		case "appx":
			p = pkgMSIX
		}
		if !slices.Contains(all, p) {
			return nil, fmt.Errorf("%s: %q isn't a %s package (%s)", where, p, platform, strings.Join(all, ", "))
		}
		set[p] = true
	}
	return set, nil
}

// debName is the package's name as Debian and RPM take it: the
// executable's, lower case letters, digits, + - and . (two at least).
func debName(exe string) string {
	n := strings.ReplaceAll(exe, "_", "-")
	if len(n) < 2 {
		n += "-app"
	}
	return n
}

// debSections are Debian's sections of freedesktop.org's main categories.
var debSections = map[string]string{
	"AudioVideo": "video", "Audio": "sound", "Video": "video", "Development": "devel",
	"Education": "education", "Game": "games", "Graphics": "graphics", "Network": "net",
	"Office": "editors", "Science": "science", "Settings": "admin", "System": "admin", "Utility": "utils",
}

// linuxPackage is what the .deb and the .rpm say of the app, and the
// prefix's files as they install under /usr (not the update's file list:
// the system's package manager updates the app).
func linuxPackage(app *asukaApp, prefix, exe, arch string) (*distpkg.Package, error) {
	list := "share/" + exe + "/" + iupdate.FileList
	files, err := distpkg.FromDir(prefix, "/usr", func(rel string) bool { return rel == list })
	if err != nil {
		return nil, err
	}
	l := app.cfg.Linux
	maintainer := l.Maintainer
	if maintainer == "" {
		maintainer = l.Developer
	}
	if maintainer == "" {
		maintainer = app.displayName()
	}
	section := ""
	for _, c := range linuxCategories(app) {
		if s := debSections[c]; s != "" {
			section = s
			break
		}
	}
	summary := l.Summary
	if summary == "" {
		summary = app.displayName()
	}
	return &distpkg.Package{
		Name:        debName(exe),
		Version:     app.cfg.Version,
		Arch:        arch,
		Summary:     summary,
		Description: l.Description,
		Maintainer:  maintainer,
		Vendor:      l.Developer,
		License:     l.License,
		Homepage:    l.Homepage,
		Section:     section,
		Files:       files,
		Time:        distpkg.Epoch(),
	}, nil
}

// The least GTK and libadwaita the host runs with (GNOME 46's), as each
// family's packages are named.
const (
	minGTK = "4.14"
	minAdw = "1.5"
)

// linuxDeps are the system packages the .deb (deb) or the .rpm needs:
// glibc (the newest symbol version the executables use), GTK 4,
// libadwaita and libsoup (HTTP: images, std/http, updates), GStreamer's
// for a Video, WebKitGTK for a WebView, the app's packages' (their
// manifests' linux.deb and linux.rpm), and asuka.toml's linux.debDepends
// or linux.rpmRequires.
func linuxDeps(app *asukaApp, glibc string, deb bool) (depends, recommends []string) {
	c := app.cfg
	video, web := app.refs("", "Video"), app.refs("", "WebView")
	if deb {
		depends = []string{"libc6 (>= " + glibc + ")", "libgtk-4-1 (>= " + minGTK + ")", "libadwaita-1-0 (>= " + minAdw + ")", "libsoup-3.0-0"}
		if video {
			// GtkMediaFile's GStreamer backend, and its codecs.
			depends = append(depends, "libgtk-4-media-gstreamer")
			recommends = append(recommends, "gstreamer1.0-plugins-good", "gstreamer1.0-libav")
		}
		if web {
			depends = append(depends, "libwebkitgtk-6.0-4")
		}
		depends = append(depends, c.debDepends...)
		depends = append(depends, c.Linux.DebDepends...)
		recommends = append(recommends, c.debRecommends...)
	} else {
		depends = []string{"libc.so.6(GLIBC_" + glibc + ")(64bit)", "gtk4 >= " + minGTK, "libadwaita >= " + minAdw, "libsoup3"}
		if video {
			recommends = append(recommends, "gstreamer1-plugins-good", "gstreamer1-plugin-libav")
		}
		if web {
			depends = append(depends, "webkitgtk6.0")
		}
		depends = append(depends, c.rpmRequires...)
		depends = append(depends, c.Linux.RPMRequires...)
		recommends = append(recommends, c.rpmRecommends...)
	}
	depends = dedupe(depends)
	// What one package needs and another only recommends, it depends on.
	recommends = slices.DeleteFunc(dedupe(recommends), func(r string) bool { return slices.Contains(depends, r) })
	return depends, recommends
}

func dedupe(xs []string) []string {
	var out []string
	for _, x := range xs {
		if x = strings.TrimSpace(x); x != "" && !slices.Contains(out, x) {
			out = append(out, x)
		}
	}
	return out
}

// elfGlibc is the newest glibc symbol version the ELF executables and
// libraries among files use (2.28: GLIBC_2.28), at least least: the glibc
// the .deb and .rpm need.
func elfGlibc(files []distpkg.File, least string) string {
	best := least
	for _, f := range files {
		if f.Src == "" || f.IsDir() || f.IsLink() {
			continue
		}
		e, err := elf.Open(f.Src)
		if err != nil {
			continue
		}
		syms, _ := e.ImportedSymbols()
		e.Close()
		for _, s := range syms {
			if v, ok := strings.CutPrefix(s.Version, "GLIBC_"); ok && versionLess(best, v) {
				best = v
			}
		}
	}
	return best
}

// rpmOwns reports whether the .rpm owns a directory: the app's own
// (share/<exe>, lib/<exe>), not the system's, which other packages own.
func rpmOwns(exe string) func(dir string) bool {
	roots := []string{"/usr/share/" + exe, "/usr/lib/" + exe, "/usr/libexec/" + exe}
	return func(d string) bool {
		for _, r := range roots {
			if d == r || strings.HasPrefix(d, r+"/") {
				return true
			}
		}
		return false
	}
}

// writeLinuxPackages writes the .deb, the .rpm and the AppImage of the
// prefix that pkgs asks for, beside it, and returns the files.
func writeLinuxPackages(app *asukaApp, o *buildOptions, pkgs map[string]bool, prefix, exe, arch, updateInfo string) ([]string, error) {
	var written []string
	// The Linux key signs the .rpm and the AppImage, when there's one (a
	// repository's packages must be signed: dnf checks them).
	key, err := linuxSigningKey(app)
	if err != nil {
		return nil, err
	}
	if pkgs[pkgDeb] || pkgs[pkgRPM] {
		p, err := linuxPackage(app, prefix, exe, arch)
		if err != nil {
			return nil, err
		}
		glibc := elfGlibc(p.Files, cbuild.LinuxGlibc)
		if pkgs[pkgDeb] {
			p.Depends, p.Recommends = linuxDeps(app, glibc, true)
			f := releaseFile(app, o, "linux", arch, ".deb")
			if err := distpkg.WriteDeb(p, f); err != nil {
				return nil, err
			}
			written = append(written, f)
		}
		if pkgs[pkgRPM] {
			p.Depends, p.Recommends = linuxDeps(app, glibc, false)
			if key != nil {
				p.Signer = key
			}
			f := releaseFile(app, o, "linux", arch, ".rpm")
			if err := distpkg.WriteRPM(p, rpmOwns(exe), f); err != nil {
				return nil, err
			}
			if key != nil {
				fmt.Printf("asuka: %s: signed with the key %s (%s)\n", filepath.Base(f), key.Fingerprint(), key.UserID())
			}
			written = append(written, f)
		}
	}
	if pkgs[pkgAppImage] {
		a := &distpkg.AppImage{UpdateInfo: updateInfo}
		if key != nil {
			a.Signer = key
		}
		f, err := writeAppImage(app, o, a, prefix, exe, arch)
		if err != nil {
			return nil, err
		}
		if key != nil {
			fmt.Printf("asuka: %s: signed with the key %s (%s)\n", filepath.Base(f), key.Fingerprint(), key.UserID())
		}
		written = append(written, f)
	}
	return written, nil
}

// appRun is the AppImage's AppRun: the app, run from where the runtime
// mounted it, its icons and MIME types where GTK looks (XDG_DATA_DIRS).
func appRun(exe string) string {
	return `#!/bin/sh
HERE="$(dirname "$(readlink -f "$0")")"
export XDG_DATA_DIRS="$HERE/usr/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
exec "$HERE/usr/bin/` + exe + `" "$@"
`
}

// writeAppImage writes the AppImage a (its update information and
// signer set) of the prefix: its AppDir is the prefix in usr/, with
// AppRun, the desktop file and the icon at its root (where the AppImage
// tools and desktops' integrations find them).
func writeAppImage(app *asukaApp, o *buildOptions, a *distpkg.AppImage, prefix, exe, arch string) (string, error) {
	rt, err := distpkg.Runtime(arch)
	if err != nil {
		return "", err
	}
	list := "share/" + exe + "/" + iupdate.FileList
	files, err := distpkg.FromDir(prefix, "/usr", func(rel string) bool { return rel == list })
	if err != nil {
		return "", err
	}
	id := app.bundleID
	desktop, err := os.ReadFile(filepath.Join(prefix, "share", "applications", id+".desktop"))
	if err != nil {
		return "", err
	}
	desktop = append(desktop, []byte("X-AppImage-Version="+app.cfg.Version+"\n")...)
	files = append(files,
		distpkg.File{Path: "/AppRun", Mode: 0o755, Data: []byte(appRun(exe))},
		distpkg.File{Path: "/" + id + ".desktop", Mode: 0o644, Data: desktop},
	)
	icon := filepath.Join(prefix, "share", "icons", "hicolor", "256x256", "apps", id+".png")
	if _, err := os.Stat(icon); err == nil {
		files = append(files,
			distpkg.File{Path: "/" + id + ".png", Mode: 0o644, Src: icon},
			distpkg.File{Path: "/.DirIcon", Mode: fs.ModeSymlink | 0o777, Link: id + ".png"},
		)
	}
	f := releaseFile(app, o, "linux", arch, ".AppImage")
	a.Runtime, a.Files = rt, files
	if err := distpkg.WriteAppImage(a, f); err != nil {
		return "", err
	}
	return f, nil
}

// releaseAssets are the files of a desktop build to hand out beside its
// update (the zip and the .dmg; the zip, installer and .msix; the .deb,
// .rpm and AppImage), which asuka publish uploads with it: those the
// build made.
func releaseAssets(app *asukaApp, o *buildOptions, platform, arch string) []string {
	var exts []string
	switch platform {
	case "darwin":
		exts = []string{".zip", ".dmg"}
	case "windows":
		exts = []string{".zip", "-setup.exe", ".msix"}
	case "linux":
		// Its .tar.gz is the update's archive, which goes anyway.
		exts = []string{".deb", ".rpm", ".AppImage"}
	}
	var out []string
	for _, e := range exts {
		if f := releaseFile(app, o, platform, arch, e); fileExists(f) {
			out = append(out, f)
		}
	}
	return out
}

func fileExists(p string) bool {
	_, err := os.Stat(p)
	return err == nil
}

// removeUnmade removes the release files of packages a build doesn't
// make (a build before's), so that none is handed out with this one.
func removeUnmade(app *asukaApp, o *buildOptions, platform, arch string, ext map[string]string, pkgs map[string]bool) {
	for p, e := range ext {
		if !pkgs[p] {
			os.Remove(releaseFile(app, o, platform, arch, e))
		}
	}
}
