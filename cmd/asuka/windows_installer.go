package main

import (
	"archive/zip"
	"bytes"
	"compress/flate"
	"crypto/sha1"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"strings"

	"github.com/egoist/asuka/internal/appbuild"
	"github.com/egoist/asuka/internal/cbuild"
	"github.com/egoist/asuka/internal/filetype"
	iupdate "github.com/egoist/asuka/internal/update"
)

// A Windows app's installer (platform/windows/installer, built with Zig
// or this PC's clang, as the app is): one .exe of the user's to run,
// <name>-<version>-windows-<arch>-setup.exe, which installs the app for
// them alone, with no administrator's rights, into
// %LOCALAPPDATA%\Programs\<Name> (where the app updates itself), with a
// Start menu shortcut and an entry in Settings' Installed apps, and
// opens it: electron-builder's one-click NSIS installer, made without
// NSIS. Its uninstaller, Uninstall <Name>.exe, is the same program
// without the app in it, in the app's folder. Both are signed as the app
// is (ASUKA_WINDOWS_CERT).

// windowsAUMID is the app's AppUserModelID, as the host makes it
// (platform/windows/src/modules.c's wh_app_user_model_id): its ID, else
// Asuka. and its name's letters, digits and dots.
func windowsAUMID(app *asukaApp) string {
	if app.cfg != nil && app.cfg.ID != "" {
		return app.cfg.ID
	}
	id := strings.Map(func(r rune) rune {
		if r < 0x80 && (r == '.' || r >= '0' && r <= '9' || r >= 'a' && r <= 'z' || r >= 'A' && r <= 'Z') {
			return r
		}
		return -1
	}, app.displayName())
	if id == "" {
		id = "App"
	}
	return "Asuka." + id
}

// toastActivatorCLSID is the CLSID of the app's toasts' COM activator, as
// the host makes it (platform/windows/src/notify.c's toast_clsid): a
// name-based (SHA-1) UUID of "asuka.toast-activator:" and the
// AppUserModelID, "{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}".
func toastActivatorCLSID(aumid string) string {
	s := sha1.Sum([]byte("asuka.toast-activator:" + aumid))
	s[6] = s[6]&0x0f | 0x50
	s[8] = s[8]&0x3f | 0x80
	return fmt.Sprintf("{%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
		s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9], s[10], s[11], s[12], s[13], s[14], s[15])
}

// windowsPublisher is who the installer and the MSIX say makes the app:
// asuka.toml's windows.publisher, else its name.
func windowsPublisher(app *asukaApp) string {
	if p := strings.TrimSpace(app.cfg.Windows.Publisher); p != "" {
		return p
	}
	return app.displayName()
}

// uninstallerName is the uninstaller's file name in the app's folder.
func uninstallerName(app *asukaApp) string {
	return "Uninstall " + sanitizeExe(app.displayName()) + ".exe"
}

// installerConfig is what the installer and the uninstaller say of the
// app (their CONFIG resource): lines of key=value, and name.<tag>= its
// name in each language of its catalogs' that names it.
func installerConfig(app *asukaApp, exe string) []byte {
	aumid := windowsAUMID(app)
	id := app.bundleID
	if id == "" {
		id = aumid
	}
	var progids, exts []string
	for _, t := range windowsFileTypes(app.cfg.fileTypes(), nil) {
		progids = append(progids, filetype.ProgID(app.cfg.ID, t))
		for _, e := range t.Extensions {
			exts = append(exts, "."+strings.ToLower(strings.TrimPrefix(e, ".")))
		}
	}
	desktop := "0"
	if app.cfg.Windows.DesktopShortcut {
		desktop = "1"
	}
	clean := func(s string) string { return strings.NewReplacer("\r", " ", "\n", " ").Replace(s) }
	var b strings.Builder
	for _, kv := range [][2]string{
		{"name", app.displayName()},
		{"version", app.cfg.Version},
		{"publisher", windowsPublisher(app)},
		{"id", id},
		{"aumid", aumid},
		{"exe", exe},
		{"folder", sanitizeExe(app.displayName())},
		{"uninstaller", uninstallerName(app)},
		{"homepage", app.cfg.Windows.Homepage},
		{"scheme", app.cfg.Scheme},
		{"progids", strings.Join(progids, ";")},
		{"exts", strings.Join(dedupe(exts), ";")},
		{"activator", toastActivatorCLSID(aumid)},
		{"desktop", desktop},
	} {
		b.WriteString(kv[0] + "=" + clean(kv[1]) + "\n")
	}
	// The app's name in its catalogs' languages (asuka.appName), which
	// the dialogs say it as in the user's.
	langs, _, byLang := app.cfg.catalogStrings()
	for _, tag := range langs {
		if n := strings.TrimSpace(byLang[tag]["appName"]); n != "" {
			b.WriteString("name." + tag + "=" + clean(n) + "\n")
		}
	}
	return []byte(b.String())
}

// writeWindowsInstaller writes the installer of the app's folder (dir,
// with exe in it) for goarch beside it, signed if a certificate is set,
// and returns it.
func writeWindowsInstaller(app *asukaApp, o *buildOptions, dir, exe, goarch string) (string, error) {
	t := cbuild.Target{OS: "windows", Arch: goarch}
	tmp, err := os.MkdirTemp("", "asuka-installer")
	if err != nil {
		return "", err
	}
	defer os.RemoveAll(tmp)
	config := installerConfig(app, exe)
	name := app.displayName()

	// The uninstaller: the program without the app.
	un := filepath.Join(tmp, uninstallerName(app))
	if err := buildInstallerExe(app, t, "Uninstall "+name, filepath.Base(un), config, nil, filepath.Join(tmp, "uninstall.o"), un); err != nil {
		return "", err
	}
	if _, err := signWindows(un, name, o.verbose); err != nil {
		return "", err
	}
	// The app's folder and the uninstaller, zipped: the payload.
	payload, err := installerPayload(dir, un)
	if err != nil {
		return "", err
	}
	setup := releaseFile(app, o, "windows", goarch, "-setup.exe")
	os.Remove(setup)
	if err := buildInstallerExe(app, t, name+" Setup", filepath.Base(setup), config, payload, filepath.Join(tmp, "setup.o"), setup); err != nil {
		return "", err
	}
	if _, err := signWindows(setup, name+" Setup", o.verbose); err != nil {
		os.Remove(setup)
		return "", err
	}
	return setup, nil
}

func buildInstallerExe(app *asukaApp, t cbuild.Target, description, file string, config, payload []byte, obj, out string) error {
	res, err := installerSyso(app, t.Arch, description, file, config, payload)
	if err != nil {
		return err
	}
	if err := os.WriteFile(obj, res, 0o644); err != nil {
		return err
	}
	return appbuild.WindowsInstaller(t, obj, out)
}

// installerPayload is a zip of dir's entries (the app's folder, its
// names relative to it, the file list left out: the installer writes
// its own) and the uninstaller at its top, compressed hard: the
// installer's size is the download's.
func installerPayload(dir, uninstaller string) ([]byte, error) {
	var buf bytes.Buffer
	zw := zip.NewWriter(&buf)
	zw.RegisterCompressor(zip.Deflate, func(w io.Writer) (io.WriteCloser, error) {
		return flate.NewWriter(w, flate.BestCompression)
	})
	add := func(name, src string) error {
		f, err := os.Open(src)
		if err != nil {
			return err
		}
		defer f.Close()
		w, err := zw.CreateHeader(&zip.FileHeader{Name: name, Method: zip.Deflate})
		if err != nil {
			return err
		}
		_, err = io.Copy(w, f)
		return err
	}
	err := filepath.WalkDir(dir, func(p string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		rel, _ := filepath.Rel(dir, p)
		if rel == "." || rel == iupdate.FileList {
			return nil
		}
		rel = filepath.ToSlash(rel)
		if d.IsDir() {
			_, err := zw.CreateHeader(&zip.FileHeader{Name: rel + "/"})
			return err
		}
		return add(rel, p)
	})
	if err == nil {
		err = add(filepath.Base(uninstaller), uninstaller)
	}
	if err == nil {
		err = zw.Close()
	}
	if err != nil {
		return nil, err
	}
	if buf.Len() > 0x7fff0000 {
		return nil, fmt.Errorf("the app's folder is too big for an installer (%d MB compressed; 2 GB at most)", buf.Len()>>20)
	}
	return buf.Bytes(), nil
}
