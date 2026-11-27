package main

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"time"

	"github.com/egoist/asuka/internal/distpkg"
)

// The Mac's disk image: what a download of a Mac app usually is. It
// opens to a window of the app and the Applications folder side by side
// (its .DS_Store, written in Go: no Finder scripting), its icon the
// app's; compressed (LZFSE, macOS 10.11 and later), signed with the
// app's Developer ID and, with --notarize, notarized and stapled as the
// app is. hdiutil makes it: a Mac build is made on a Mac.

// dmgLayout is the window's: 600 by 380 points, the app on the left, the
// Applications folder on the right, 128-point icons.
func dmgLayout(app string) distpkg.DMGLayout {
	return distpkg.DMGLayout{
		Width: 600, Height: 380, IconSize: 128,
		Icons: map[string][2]int{app: {160, 180}, "Applications": {440, 180}},
	}
}

// macDMG writes the disk image of the signed (and notarized) bundle at
// dmg, signed with identity unless it's ad hoc ("-").
func macDMG(app *asukaApp, bundle, dmg, identity, work string, run func(dir string, env []string, cmd string, args ...string) error) error {
	stage := filepath.Join(work, "dmg")
	if err := os.RemoveAll(stage); err != nil {
		return err
	}
	if err := os.MkdirAll(stage, 0o755); err != nil {
		return err
	}
	name := filepath.Base(bundle)
	// ditto keeps the bundle as it's signed: its links, its extended
	// attributes, its stapled ticket.
	if err := run(work, nil, "ditto", bundle, filepath.Join(stage, name)); err != nil {
		return err
	}
	if err := os.Symlink("/Applications", filepath.Join(stage, "Applications")); err != nil {
		return err
	}
	if err := os.WriteFile(filepath.Join(stage, ".DS_Store"), distpkg.DSStore(dmgLayout(name)), 0o644); err != nil {
		return err
	}
	icon := filepath.Join(bundle, "Contents", "Resources", "AppIcon.icns")
	hasIcon := fileExists(icon)
	if hasIcon {
		data, err := os.ReadFile(icon)
		if err != nil {
			return err
		}
		if err := os.WriteFile(filepath.Join(stage, ".VolumeIcon.icns"), data, 0o644); err != nil {
			return err
		}
	}
	// A writable image of the folder first, where the volume is given its
	// icon, then compressed.
	rw := filepath.Join(work, "dmg-rw.dmg")
	os.Remove(rw)
	defer os.Remove(rw)
	vol := strings.TrimSuffix(name, ".app")
	if err := hdiutil(run, work, "create", "-ov", "-volname", vol, "-fs", "HFS+", "-format", "UDRW", "-srcfolder", stage, rw); err != nil {
		return err
	}
	if hasIcon {
		mnt := filepath.Join(work, "dmg-mount")
		os.RemoveAll(mnt)
		if err := hdiutil(run, work, "attach", rw, "-nobrowse", "-noverify", "-noautoopen", "-mountpoint", mnt); err != nil {
			return err
		}
		// The volume's Finder information says it has an icon of its own
		// (kHasCustomIcon, in its flags).
		info := "0000000000000000040000000000000000000000000000000000000000000000"
		err := run(work, nil, "xattr", "-wx", "com.apple.FinderInfo", info, mnt)
		// What mounting it made (the file system events' log) isn't the
		// app's.
		os.RemoveAll(filepath.Join(mnt, ".fseventsd"))
		if derr := detachImage(run, work, mnt); err == nil {
			err = derr
		}
		if err != nil {
			return err
		}
	}
	os.Remove(dmg)
	if err := hdiutil(run, work, "convert", rw, "-ov", "-format", "ULFO", "-o", dmg); err != nil {
		return err
	}
	if identity == "-" {
		return nil // ad hoc: a disk image isn't signed so
	}
	if err := run(work, nil, "codesign", "--force", "--sign", identity, "--timestamp", dmg); err != nil {
		return err
	}
	return run(work, nil, "codesign", "--verify", "--strict", dmg)
}

// hdiutil runs hdiutil, again when it fails at first: a volume Spotlight
// or another process still holds ("Resource busy").
func hdiutil(run func(dir string, env []string, cmd string, args ...string) error, dir string, args ...string) error {
	if _, err := exec.LookPath("hdiutil"); err != nil {
		return fmt.Errorf("a .dmg is made with macOS's hdiutil: build it on a Mac, or leave it out (--package zip)")
	}
	var err error
	for attempt := range 3 {
		if attempt > 0 {
			time.Sleep(time.Duration(attempt) * time.Second)
		}
		if err = run(dir, nil, "hdiutil", args...); err == nil {
			return nil
		}
	}
	return err
}

// detachImage unmounts the image at mnt, forcing it when asked nicely fails.
func detachImage(run func(dir string, env []string, cmd string, args ...string) error, dir, mnt string) error {
	if err := hdiutil(run, dir, "detach", mnt); err == nil {
		return nil
	}
	return run(dir, nil, "hdiutil", "detach", "-force", mnt)
}
