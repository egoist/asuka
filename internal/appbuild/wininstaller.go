package appbuild

import (
	"path/filepath"

	"github.com/egoist/asuka/internal/cbuild"
)

// WindowsInstaller builds an app's installer for t at out
// (platform/windows/installer: a GUI program, C on Win32, with the
// core's DEFLATE decoder, core/src/std/flate.c, and nothing else of the
// core's), with resources linked in: a COFF object with its icon, its
// manifest, its version, what it says of the app and the app itself
// (cmd/asuka's installerSyso). Without the app in the resources it's the
// uninstaller.
func WindowsInstaller(t cbuild.Target, resources, out string) error {
	tc, err := cbuild.ForTarget(t)
	if err != nil {
		return err
	}
	core, err := cbuild.CoreDir()
	if err != nil {
		return err
	}
	win, _, err := windowsTree.Find()
	if err != nil {
		return err
	}
	srcs := []string{filepath.Join(win, "installer", "installer.c"), filepath.Join(core, "src", "std", "flate.c")}
	flags := append(append([]string{}, cbuild.Flags...), "-O2", "-DNDEBUG", "-I"+filepath.Join(core, "src"), "-DUNICODE", "-D_UNICODE")
	objs, err := tc.Objects(srcs, flags)
	if err != nil {
		return err
	}
	ld := append(append([]string{}, tc.Flags...), "-s")
	if mingw(tc) {
		ld = append(ld, "-municode", subsystem(false))
	} else {
		ld = append(ld, "-Wl,/subsystem:windows", "-Wl,/entry:wWinMainCRTStartup")
	}
	ld = append(ld, "-lcomctl32", "-lshell32", "-lole32", "-luuid", "-ladvapi32", "-luser32")
	return tc.CC.Link(append(objs, resources), out, ld)
}
