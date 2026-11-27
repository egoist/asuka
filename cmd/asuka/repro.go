package main

import (
	"io/fs"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"github.com/egoist/asuka/internal/cbuild"
)

// A build is reproducible (docs/shipping.md): the same sources make the
// same bytes, wherever and whenever they're built. What it dates (its
// packages' files, a Linux app's metainfo, the web's version.json, a
// phone release's updates' "built") is dated by SOURCE_DATE_EPOCH, as
// reproducible-builds.org has it; without it, by the app's last commit,
// else by the newest of its files: never by the time the build ran.

// setSourceDateEpoch sets SOURCE_DATE_EPOCH for this process and what it
// runs, unless it's set: the time of the last commit of the git
// repository dir is in, else the newest modification time of dir's
// files (its sources, asuka.toml, its assets).
func setSourceDateEpoch(dir string) {
	if os.Getenv("SOURCE_DATE_EPOCH") != "" {
		return
	}
	if t, ok := sourceDate(dir); ok {
		os.Setenv("SOURCE_DATE_EPOCH", strconv.FormatInt(t.Unix(), 10))
	}
}

// sourceDate is what setSourceDateEpoch dates a build of dir by.
func sourceDate(dir string) (time.Time, bool) {
	if _, err := exec.LookPath("git"); err == nil {
		cmd := exec.Command("git", "-C", dir, "log", "-1", "--format=%ct")
		if out, err := cmd.Output(); err == nil {
			if n, err := strconv.ParseInt(strings.TrimSpace(string(out)), 10, 64); err == nil && n > 0 {
				return time.Unix(n, 0).UTC(), true
			}
		}
	}
	var newest time.Time
	filepath.WalkDir(dir, func(p string, d fs.DirEntry, err error) error {
		if err != nil {
			return nil
		}
		if d.IsDir() {
			if p != dir && (strings.HasPrefix(d.Name(), ".") || d.Name() == "dist" || d.Name() == "build" || d.Name() == "node_modules") {
				return filepath.SkipDir
			}
			return nil
		}
		if info, err := d.Info(); err == nil && info.Mode().IsRegular() && info.ModTime().After(newest) {
			newest = info.ModTime()
		}
		return nil
	})
	if newest.IsZero() {
		return time.Time{}, false
	}
	return newest.Truncate(time.Second).UTC(), true
}

// stampTree dates every file, directory and link under dir t: an archive
// a tool makes of it (ditto's .zip of a Mac app) records their times.
func stampTree(dir string, t time.Time) error {
	return filepath.WalkDir(dir, func(p string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if d.Type()&fs.ModeSymlink != 0 {
			return lchtimes(p, t)
		}
		return os.Chtimes(p, t, t)
	})
}

// loadRelease is loadApp for a release (asuka build, asuka publish): what
// it makes is dated by the app's sources (setSourceDateEpoch).
func loadRelease(dir, platform string) (*asukaApp, error) {
	app, err := loadApp(dir, platform)
	if err != nil {
		return nil, err
	}
	setSourceDateEpoch(app.dir)
	return app, nil
}

// appleLinkRepro are what a link of an Apple platform's binary by swiftc
// adds for the same objects to link to the same bytes, where this
// Xcode's linker has them: -reproducible, and the small Objective-C
// stubs (each loads its selector and branches to objc_msgSend). The fast
// ones each load objc_msgSend from a GOT slot that Xcode 27's linker
// orders differently from one link of the same objects to the next (an
// iOS app's took one of two layouts at random, -reproducible or not);
// the small ones cost a branch more per message sent from Swift.
func appleLinkRepro() []string {
	var out []string
	for _, f := range []string{"-reproducible", "-objc_stubs_small"} {
		if cbuild.AppleLinkerTakes(f) {
			out = append(out, "-Xlinker", f)
		}
	}
	return out
}
