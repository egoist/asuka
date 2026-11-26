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
)

// Over-the-air updates of phone apps (RFC 0006): a release built with
// asuka.toml's [updates] carries its fingerprint, the feed and the public
// key (appbuild.Updates); asuka publish --update makes an update of the
// app's code, signed, for the builds of its fingerprint, and uploads it
// beside the desktop updates.

// otaFingerprint is what a phone build's native side gives the app's
// code, hashed: an update made against another fingerprint isn't run. It
// covers the CLI (its version; a development CLI's core sources), the
// platform, the bytecode's format, the tables of builtins the build links
// (the module's), the app's externs, the native code of the packages it
// imports and the app's own (its ios/ and android/ folders), its assets,
// and asuka.toml but for its version and build.
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
	}
	put("platform", platform)
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

// otaTarget names a phone's updates of a fingerprint in the feed:
// update-<platform>-<its first 16 digits>.json.
func otaTarget(platform, fingerprint string) string { return platform + "-" + fingerprint[:16] }

// otaUpdates are what a phone release of m takes updates with, or nil
// when asuka.toml has no [updates].
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
// returns the module's file and the manifest's.
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
	tp := target.IOS
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
