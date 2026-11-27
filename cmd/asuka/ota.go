package main

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"

	"github.com/egoist/asuka/internal/appbuild"
	"github.com/egoist/asuka/internal/cbuild"
	iupdate "github.com/egoist/asuka/internal/update"
	"github.com/egoist/asuka/lang/bytecode"
	"github.com/egoist/asuka/lang/target"
	androidHost "github.com/egoist/asuka/platform/android"
	iosHost "github.com/egoist/asuka/platform/ios"
)

// Over-the-air updates of phone and Apple TV apps (RFC 0006): a release
// built with asuka.toml's [updates] carries its fingerprint, the feed and
// the public key (appbuild.Updates); asuka publish --update makes an
// update of the app's code, signed, for the builds of its fingerprint,
// and uploads it beside the desktop updates. Each platform (ios, tvos,
// android) has updates of its own: the platform is in the fingerprint
// and names the manifest (otaTarget), so an iPhone never takes an Apple
// TV's update, nor an Apple TV an iPhone's.

// otaPlatform reports whether platform's builds take over-the-air
// updates.
func otaPlatform(platform string) bool {
	return platform == platformIOS || platform == platformTVOS || platform == platformAndroid
}

// otaFingerprint is what a build's native side gives the app's code,
// hashed: an update made against another fingerprint isn't run. It
// covers the CLI (its version; a development CLI's core and host
// sources), the platform, the host's parts the build has (its flags: an
// Apple TV's are fewer than an iPhone's), the bytecode's format, the
// tables of builtins the build links (the module's), the app's externs,
// the native code of the packages it imports (their Package.swift too,
// whose platforms say which are built for tvOS) and the app's own (its
// ios/ and android/ folders), its assets, and asuka.toml but for its
// version and build.
func (a *asukaApp) otaFingerprint(platform string, m *bytecode.Module) (string, error) {
	h := sha256.New()
	put := func(k, v string) { io.WriteString(h, k+"\x00"+v+"\x00") }
	put("cli", version)
	if version == "dev" {
		if core, err := cbuild.CoreDir(); err == nil {
			for _, d := range []string{"src", "include", "link"} {
				if err := hashTree(h, filepath.Join(core, d), nil); err != nil {
					return "", err
				}
			}
		}
		// The host's sources, which a released CLI's version stands for.
		var host fs.FS = iosHost.Files
		if platform == platformAndroid {
			host = androidHost.Files
		}
		if err := hashFS(h, host); err != nil {
			return "", err
		}
	}
	put("platform", platform)
	// The host's parts the build has: those of the elements the app shows
	// (a Video's player, a WebView's WebKit) and of its packages' and
	// their members' manifests (ios.hostFlags), which an update that
	// starts to use one needs; on tvOS those it has (tvFeatures).
	features := usedFeatures(a, a.cfg)
	if platform == platformTVOS {
		features = tvFeatures(features)
	}
	put("host", strings.Join(features, ","))
	put("bytecode", fmtInt(bytecode.Version))
	// The tables of builtins the build links: whole, by their prefix
	// (link.c's AK_USES_FS for fs.join), so an update may call another of
	// a table's.
	tables := map[string]bool{}
	for _, b := range m.Builtins {
		if p, _, ok := strings.Cut(b, "."); ok {
			tables[p] = true
		}
	}
	names := make([]string, 0, len(tables))
	for p := range tables {
		names = append(names, p)
	}
	sort.Strings(names)
	put("builtins", strings.Join(names, ","))
	if m.C != nil {
		c, err := json.Marshal(m.C)
		if err != nil {
			return "", err
		}
		put("c", string(c))
	}
	// The packages' native code: everything but their Asuka, which is in
	// the module.
	dirs := a.packageDirs()
	ids := make([]string, 0, len(dirs))
	for id := range dirs {
		ids = append(ids, id)
	}
	sort.Strings(ids)
	for _, id := range ids {
		put("package", id)
		if err := hashTree(h, dirs[id], func(rel string) bool { return !strings.HasSuffix(rel, ".asuka") }); err != nil {
			return "", err
		}
	}
	for _, d := range []string{"assets", "ios", "android"} {
		put("app", d)
		if err := hashTree(h, filepath.Join(a.dir, d), nil); err != nil {
			return "", err
		}
	}
	cfg := *a.cfg
	cfg.Version, cfg.Build = "", 0
	if platform == platformTVOS {
		// What a tvOS build goes without (dropTVExtensions, which the
		// build has done by the time it asks, and an update hasn't).
		cfg.Widgets, cfg.Shortcuts = nil, nil
	}
	toml, err := json.Marshal(cfg)
	if err != nil {
		return "", err
	}
	put("asuka.toml", string(toml))
	return hex.EncodeToString(h.Sum(nil)), nil
}

func fmtInt(n int) string {
	b, _ := json.Marshal(n)
	return string(b)
}

// hashTree writes dir's files (those keep says, or all) into h: their
// paths and contents, in order; a directory that isn't there is empty.
// Build outputs (dist, build, .gradle) and hidden files are left out.
func hashTree(h io.Writer, dir string, keep func(rel string) bool) error {
	return filepath.WalkDir(dir, func(p string, d fs.DirEntry, err error) error {
		if err != nil {
			if os.IsNotExist(err) {
				return nil
			}
			return err
		}
		rel, _ := filepath.Rel(dir, p)
		name := d.Name()
		if d.IsDir() {
			if p != dir && (strings.HasPrefix(name, ".") || name == "dist" || name == "build" || name == "node_modules") {
				return filepath.SkipDir
			}
			return nil
		}
		if strings.HasPrefix(name, ".") || keep != nil && !keep(filepath.ToSlash(rel)) {
			return nil
		}
		f, err := os.Open(p)
		if err != nil {
			return err
		}
		defer f.Close()
		io.WriteString(h, filepath.ToSlash(rel)+"\x00")
		_, err = io.Copy(h, f)
		return err
	})
}

// hashFS writes fsys's files into h: their paths and contents, in order.
func hashFS(h io.Writer, fsys fs.FS) error {
	return fs.WalkDir(fsys, ".", func(p string, d fs.DirEntry, err error) error {
		if err != nil || d.IsDir() {
			return err
		}
		b, err := fs.ReadFile(fsys, p)
		if err != nil {
			return err
		}
		io.WriteString(h, p+"\x00")
		_, err = h.Write(b)
		return err
	})
}

// otaTarget names a platform's updates of a fingerprint in the feed:
// update-<platform>-<its first 16 digits>.json, the platform ios, tvos
// or android (platform/update's otaPlatform names the same at run time).
func otaTarget(platform, fingerprint string) string { return platform + "-" + fingerprint[:16] }

// otaUpdates are what a release of m for platform (ios, tvos, android)
// takes updates with, or nil when asuka.toml has no [updates].
func (a *asukaApp) otaUpdates(platform string, m *bytecode.Module) (*appbuild.Updates, error) {
	u, err := a.cfg.updates()
	if err != nil || u == nil {
		return nil, err
	}
	fp, err := a.otaFingerprint(platform, m)
	if err != nil {
		return nil, err
	}
	return &appbuild.Updates{Fingerprint: fp, Feed: u.feed(otaTarget(platform, fp)), Key: strings.TrimSpace(u.PublicKey),
		Version: a.cfg.Version, Built: time.Now().Unix()}, nil
}

// writeOTAUpdate writes, in out/updates, an over-the-air update of the
// app's code for the platform's builds of its fingerprint: the module,
// signed, and its manifest (update-<platform>-<fingerprint>.json). It
// returns the module's file and the manifest's. tvOS's is iOS's code
// (Apple TV runs a build of the iOS app) against tvOS's fingerprint.
func writeOTAUpdate(app *asukaApp, platform, out string) (file, manifest string, err error) {
	u, err := app.cfg.updates()
	if err != nil {
		return "", "", err
	}
	if u == nil {
		return "", "", errors.New("an update needs asuka.toml's updates (asuka update keygen prints them), in the build it updates too")
	}
	key, err := u.signingKey(app.dir)
	if err != nil {
		return "", "", err
	}
	if key == nil {
		return "", "", errors.New("an update needs the update signing key: set ASUKA_UPDATE_PRIVATE_KEY or updates.privateKey")
	}
	version := app.cfg.Version
	if !iupdate.ValidVersion(version) {
		return "", "", fmt.Errorf("asuka.toml: version %q isn't one updates compare (1.2.0, 1.3.0-beta.1)", version)
	}
	if !otaPlatform(platform) {
		return "", "", fmt.Errorf("over-the-air updates are iOS's, tvOS's and Android's, not %s's", platform)
	}
	tp := target.IOS // tvOS's too: the iOS app's code
	if platform == platformAndroid {
		tp = target.Android
	}
	m, err := app.compile(tp)
	if err != nil {
		return "", "", err
	}
	fp, err := app.otaFingerprint(platform, m)
	if err != nil {
		return "", "", err
	}
	notes, err := u.releaseNotes(app.dir, version)
	if err != nil {
		return "", "", err
	}
	dir := filepath.Join(out, "updates")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return "", "", err
	}
	tgt := otaTarget(platform, fp)
	number := time.Now().Unix()
	name := fmt.Sprintf("%s-%s-%s-%d.asukab", exeSlug(app.displayName(), app.bundleID), version, tgt, number)
	file = filepath.Join(dir, name)
	data := m.Encode()
	size, sig, err := writeSigned(file, key, func(w io.Writer) error {
		_, err := w.Write(data)
		return err
	})
	if err != nil {
		return "", "", err
	}
	mf := iupdate.Manifest{
		Version:     version,
		Notes:       notes,
		Date:        time.Now().UTC().Format(time.RFC3339),
		URL:         u.fileURL(version, name),
		Size:        size,
		Signature:   sig,
		Fingerprint: fp,
		Update:      number,
	}
	js, _ := json.MarshalIndent(mf, "", "  ")
	manifest = filepath.Join(dir, iupdate.ManifestName(tgt, iupdate.Channel(version)))
	if err := os.WriteFile(manifest, append(js, '\n'), 0o644); err != nil {
		return "", "", err
	}
	fmt.Printf("asuka: %s: an update of %s's code for its %s builds of fingerprint %s (%s), and its manifest %s\n",
		file, version, platform, fp[:16], formatBytes(size), filepath.Base(manifest))
	return file, manifest, nil
}
