package appbuild

import (
	"fmt"
	"strconv"
	"strings"

	"github.com/egoist/asuka/internal/cbuild"
	"github.com/egoist/asuka/lang/bytecode"
	"github.com/egoist/asuka/lang/cgen"
	"github.com/egoist/asuka/platform/desktop"
)

// AppC writes m compiled to C (lang/cgen) into the build cache, at a
// path of its content's (the object cache compiles it from there), and
// returns the path.
func AppC(m *bytecode.Module) (string, error) {
	if m == nil || m.App < 0 && m.Main < 0 {
		return "", fmt.Errorf("appbuild: the module has no app (an app block) and no fn main")
	}
	src, err := cgen.Generate(m)
	if err != nil {
		return "", err
	}
	return cbuild.CacheFile("app", "app.c", src)
}

// Lib builds the core for t into out (kind: cbuild.Static for iOS,
// Shared for Android and the desktops, Module for the web): with m
// compiled into it, a release's (asuka_run starts it), or with m nil, a
// dev player's, which loads the bytecode asuka dev sends
// (asuka_dev_connect). A player is built for debugging (assertions on,
// optimized less); a release isn't.
func Lib(t cbuild.Target, kind cbuild.LibKind, m *bytecode.Module, out string) error {
	return LibWithUpdates(t, kind, m, nil, out)
}

// Updates are what a phone release takes over-the-air updates with (RFC
// 0006): its fingerprint, which an update must have been made against,
// the feed it checks and the public key updates are signed with.
type Updates struct {
	Fingerprint, Feed, Key string
	// Version is the app's, Built when the build was made (seconds since
	// 1970): an update older is older code.
	Version string
	Built   int64
}

// defines are core/link/link.c's macros of u.
func (u *Updates) defines() []string {
	if u == nil || u.Fingerprint == "" {
		return nil
	}
	q := func(s string) string { return strconv.Quote(s) }
	return []string{"-DAK_FINGERPRINT=" + q(u.Fingerprint), "-DAK_UPDATE_FEED=" + q(u.Feed), "-DAK_UPDATE_KEY=" + q(u.Key),
		"-DAK_UPDATE_VERSION=" + q(u.Version), "-DAK_UPDATE_BUILT=" + strconv.FormatInt(u.Built, 10)}
}

// LibWithUpdates is Lib for a release that takes over-the-air updates
// (u nil: none).
func LibWithUpdates(t cbuild.Target, kind cbuild.LibKind, m *bytecode.Module, u *Updates, out string) error {
	var c *bytecode.C
	if m != nil {
		c = m.C
	}
	return lib(t, kind, m, c, u.defines(), out)
}

// PlayerLib builds a dev player's core for t into out, as Lib with no
// module does, with the C c calls (the app's externs', from its last
// compile; nil for none) compiled in, which the module the player loads
// calls through its ffi.* builtins.
func PlayerLib(t cbuild.Target, kind cbuild.LibKind, c *bytecode.C, out string) error {
	return lib(t, kind, nil, c, nil, out)
}

func lib(t cbuild.Target, kind cbuild.LibKind, m *bytecode.Module, c *bytecode.C, defines []string, out string) error {
	core, err := cbuild.CoreDir()
	if err != nil {
		return err
	}
	tc, err := cbuild.ForTarget(t)
	if err != nil {
		return err
	}
	var extra, builtins []string
	if m != nil {
		builtins = m.Builtins
		src, err := AppC(m)
		if err != nil {
			return err
		}
		extra = append(extra, src)
	}
	ffi, err := Foreign(c, t)
	if err != nil {
		return err
	}
	return tc.LibWith(core, kind, cbuild.LibOptions{Extra: extra, Foreign: ffi, Debug: m == nil, Builtins: builtins, Defines: defines}, out)
}

// LinuxExe builds a Linux app's executable for t at out: the C host
// (platform/linux, with platform/desktop's shared C), the native registry
// and the C of the native packages in dirs (DesktopNatives), and the core,
// linked into one executable, with m compiled to C (lang/cgen) for a
// release, or with m nil asuka dev's player, which loads the bytecode
// asuka dev sends (built for debugging, as Lib's player is).
func LinuxExe(t cbuild.Target, m *bytecode.Module, dirs []string, out string) error {
	var c *bytecode.C
	if m != nil {
		c = m.C
	}
	return linuxExe(t, m, c, dirs, out)
}

// LinuxPlayer builds asuka dev's player for Linux, as LinuxExe does with
// no module, with the C c calls (the app's last compile's: its externs',
// the std packages' it imports) compiled in.
func LinuxPlayer(t cbuild.Target, c *bytecode.C, dirs []string, out string) error {
	return linuxExe(t, nil, c, dirs, out)
}

func linuxExe(t cbuild.Target, m *bytecode.Module, c *bytecode.C, dirs []string, out string) error {
	host, err := linuxHost()
	if err != nil {
		return err
	}
	natives, err := DesktopNatives(t, dirs)
	if err != nil {
		return err
	}
	return desktopExe(t, m, c, host, natives, out)
}

// linuxHost is the Linux host's C (cbuild.LinuxHost: the checkout's, or
// the CLI's own copy), a unit.
func linuxHost() (*DesktopC, error) {
	core, err := cbuild.CoreDir()
	if err != nil {
		return nil, err
	}
	u, err := cbuild.LinuxHost(core)
	if err != nil {
		return nil, err
	}
	return &DesktopC{Sources: u.Sources, Units: []cbuild.Unit{u}}, nil
}

// MacShared is the C the desktop hosts share that the Mac's host links
// too (desktop.MacShared: the route state, flyouts' places, Asuka's own
// words) for arch, a unit: its library (prebuilt, or built into the
// cache), which the host's link searches for what it calls.
func MacShared(arch string, debug bool) ([]string, error) {
	tc, err := cbuild.ForTarget(cbuild.Target{OS: "darwin", Arch: arch})
	if err != nil {
		return nil, err
	}
	u, err := MacSharedUnit()
	if err != nil {
		return nil, err
	}
	lib, err := tc.UnitArchive(u, debug)
	if err != nil {
		return nil, err
	}
	return []string{lib}, nil
}

// MacSharedUnit is desktop.MacShared's C as a unit.
func MacSharedUnit() (cbuild.Unit, error) {
	d, err := desktop.Dir()
	if err != nil {
		return cbuild.Unit{}, err
	}
	var srcs []string
	for _, f := range desktop.MacShared(d) {
		if strings.HasSuffix(f, ".c") {
			srcs = append(srcs, f)
		}
	}
	return cbuild.Unit{Name: "mac-shared", Sources: srcs, Flags: []string{"-I" + desktop.Include(d)}, Embedded: desktop.Embedded()}, nil
}
