package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"time"

	"github.com/egoist/asuka/internal/appbuild"
	"github.com/egoist/asuka/internal/appconfig"
	"github.com/egoist/asuka/internal/cbuild"
	"github.com/egoist/asuka/internal/nativegen"
	"github.com/egoist/asuka/std"
)

// Desktop apps: on a Mac the executable is the Swift host (platform/macos,
// AppKit) with the core linked in (mac_swift.go). On Windows and Linux it
// is one program: the C host (platform/windows, platform/linux: RFC 0008),
// the core, the native registry, the native packages' C (each one's
// desktop/) and, in a release, the app compiled to C (windows.go; linux.go
// through appbuild.DesktopExe). Each has the app's settings beside it
// (internal/appconfig's File); asuka dev's player has no app compiled in,
// and loads the bytecode the CLI sends (ASUKA_DEV).

// writeHostConfig writes the app's settings for the desktop host at path:
// a release's for target, asuka dev's player's for "" (hostConfig).
func writeHostConfig(app *asukaApp, platform, target string, updates bool, path string) error {
	c := app.hostConfig(platform, target, updates)
	b, err := json.MarshalIndent(&c, "", "  ")
	if err != nil {
		return err
	}
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return err
	}
	return os.WriteFile(path, append(b, '\n'), 0o644)
}

// desktopProcess is a dev player running on this computer (or, for
// Linux from another system, on ASUKA_LINUX_RUN's machine).
type desktopProcess struct {
	cmd   *exec.Cmd
	proc  *os.Process
	stdin io.WriteCloser // Windows: closed, the app quits (ASUKA_STOP=stdin)
	done  chan struct{}
}

func (p *desktopProcess) stop() {
	if p == nil || p.proc == nil {
		return
	}
	select {
	case <-p.done:
		return
	default:
	}
	if p.stdin != nil {
		p.stdin.Close()
	} else {
		p.proc.Signal(os.Interrupt)
	}
	select {
	case <-p.done:
	case <-time.After(3 * time.Second):
		p.proc.Kill()
		<-p.done
	}
}

// devDesktop runs the app on this computer (mac, windows, linux): the
// player (the host and the core without an app) in the app's work
// directory, connected to asuka dev, which sends it the app.
func devDesktop(dir string, o *devOptions, stop <-chan os.Signal, platform string) error {
	goos := map[string]string{platformMac: "darwin", platformWindows: "windows", platformLinux: "linux"}[platform]
	var wrap []string
	if goos != runtime.GOOS {
		if platform != platformLinux {
			return fmt.Errorf("asuka dev --platform %s runs the app on this computer: run it on %s (asuka build --platform %s builds from anywhere)", platform, map[string]string{platformMac: "a Mac", platformWindows: "Windows"}[platform], platform)
		}
		if wrap = strings.Fields(os.Getenv("ASUKA_LINUX_RUN")); len(wrap) == 0 {
			return errors.New("a Linux app runs only on Linux: run asuka dev there, or set ASUKA_LINUX_RUN to a command that runs a program on a Linux machine with a display that sees this computer's files at the same paths (orb -m ubuntu, limactl shell default). asuka build --platform linux builds a release from here")
		}
	}
	app, err := loadApp(dir, platform)
	if err != nil {
		return err
	}
	work, err := workDir(app, platform)
	if err != nil {
		return err
	}
	t := cbuild.Target{OS: goos, Arch: runtime.GOARCH}
	where := "this computer"
	listen, devHost := "127.0.0.1:0", "127.0.0.1"
	if wrap != nil {
		out, err := exec.Command(wrap[0], append(wrap[1:len(wrap):len(wrap)], "uname", "-m")...).Output()
		if err != nil {
			return fmt.Errorf("ASUKA_LINUX_RUN=%q: uname -m: %v", os.Getenv("ASUKA_LINUX_RUN"), err)
		}
		switch m := strings.TrimSpace(string(out)); m {
		case "aarch64", "arm64":
			t.Arch = "arm64"
		case "x86_64", "amd64":
			t.Arch = "amd64"
		default:
			return fmt.Errorf("ASUKA_LINUX_RUN: a %s machine; Asuka runs on amd64 and arm64", m)
		}
		where = wrap[0]
		// The machine reaches this one by the network.
		listen = ":0"
		if devHost = os.Getenv("ASUKA_DEV_HOST"); devHost == "" {
			if hs := lanHosts(); len(hs) > 0 {
				devHost = hs[0]
			}
		}
	}
	if platform == platformLinux && !gAppIDValid(app.bundleID) {
		fmt.Printf("asuka: %q isn't a valid application ID on Linux (letters, digits, _ and -, in two parts or more, none starting with a digit): the app runs under another\n", app.bundleID)
	}
	fmt.Printf("asuka: %s on %s\n", app.name, where)
	loop, err := newDevLoop(app, listen)
	if err != nil {
		return err
	}
	dirs := devDirs(work)
	// The app's programs ([bin]), built once, where command.bundled finds
	// them (ASUKA_BIN_DIR), as a release has them beside the app.
	bins := filepath.Join(work, "bin")
	var binsBuilt bool
	var proc *desktopProcess
	launch := func() error {
		start := time.Now()
		if !binsBuilt {
			if _, err := bundlePrograms(app, t, bins, o.verbose); err != nil {
				return err
			}
			binsBuilt = true
		}
		exe, err := buildDesktopPlayer(app, platform, t, work, o.verbose)
		if err != nil {
			return err
		}
		proc.stop()
		env := append(append(os.Environ(), dirs...),
			fmt.Sprintf("ASUKA_DEV=%s:%d", devHost, loop.port()),
			// Read where they are, so changing one needs no rebuild.
			"ASUKA_ASSETS="+filepath.Join(app.dir, "assets"),
			"ASUKA_BIN_DIR="+bins,
		)
		if platform != platformMac {
			env = append(env, "ASUKA_APP_CONFIG="+filepath.Join(filepath.Dir(exe), appconfig.File))
		}
		if platform == platformWindows {
			env = append(env, "ASUKA_STOP=stdin")
		}
		p := &desktopProcess{done: make(chan struct{})}
		if platform == platformMac {
			// It answers for itself, as when it's opened from the Finder:
			// its privacy prompts (camera, location...) are its own, with
			// its purpose strings, not the terminal's.
			p.proc, err = spawnOwn(exe, app.dir, env)
		} else {
			name, args := exe, []string(nil)
			if wrap != nil {
				name, args = wrap[0], append(append(wrap[1:len(wrap):len(wrap)], "env", "ASUKA_DEV="+fmt.Sprintf("%s:%d", devHost, loop.port())), dirs...)
				args = append(args, "ASUKA_ASSETS="+filepath.Join(app.dir, "assets"), "ASUKA_BIN_DIR="+bins,
					"ASUKA_APP_CONFIG="+filepath.Join(filepath.Dir(exe), appconfig.File))
				args = append(args, exe)
			}
			c := exec.Command(name, args...)
			c.Dir = app.dir
			c.Env = env
			c.Stdout, c.Stderr = os.Stdout, os.Stderr
			if platform == platformWindows {
				if p.stdin, err = c.StdinPipe(); err != nil {
					return err
				}
			}
			if err = c.Start(); err == nil {
				p.cmd, p.proc = c, c.Process
			}
		}
		if err != nil {
			return err
		}
		go func() {
			if p.cmd != nil {
				p.cmd.Wait()
			} else {
				p.proc.Wait()
			}
			close(p.done)
		}()
		proc = p
		fmt.Printf("asuka: the player built and started in %.1fs\n", time.Since(start).Seconds())
		return nil
	}
	loop.flows = o.flows
	return loop.run(devPlayer{
		launch: launch,
		stop:   func() { proc.stop() },
		done: func() <-chan struct{} {
			if proc == nil {
				return nil
			}
			return proc.done
		},
	}, stop, o.once)
}

// buildDesktopPlayer builds the dev player for platform on target t in
// work/player: the host, the core without an app, the app's settings
// (on macOS, an .app with the app's Info.plist, icon and Swift, signed
// ad hoc). It returns the executable.
func buildDesktopPlayer(app *asukaApp, platform string, t cbuild.Target, work string, verbose bool) (string, error) {
	dir := filepath.Join(work, "player")
	if platform == platformMac {
		run := macRunner(verbose)
		bundle := filepath.Join(dir, app.displayName()+".app")
		name, err := writeMacBundle(app, bundle, work, false, run)
		if err != nil {
			return "", err
		}
		exe := filepath.Join(bundle, "Contents", "MacOS", name)
		// The Swift host and the core without an app, which loads the
		// bytecode asuka dev sends.
		mb := &macBuild{app: app, work: work, run: run}
		if err := mb.executable(exe, t.Arch, nil); err != nil {
			return "", err
		}
		if err := mb.embed(bundle, exe); err != nil {
			return "", err
		}
		if err := writeHostConfig(app, platform, "", false, filepath.Join(bundle, "Contents", "Resources", appconfig.File)); err != nil {
			return "", err
		}
		ents, err := writeMacEntitlements(app, work)
		if err != nil {
			return "", err
		}
		return exe, signMac(bundle, "-", ents, run)
	}
	if t.OS == "windows" {
		// One console program: the host, the core, every standard
		// package's C and the app's own, and the C it calls (its
		// externs', std/sqlite's SQLite); no app.
		exe := filepath.Join(dir, "asuka-player.exe")
		natives, err := appbuild.DesktopNatives(t, append(stdNativeDirs(), desktopNativeDirs(app)...))
		if err != nil {
			return "", err
		}
		if err := appbuild.WindowsHost(t, appbuild.WindowsOptions{C: app.c, Native: natives, Console: true}, exe); err != nil {
			return "", err
		}
		return exe, writeHostConfig(app, platform, "", false, filepath.Join(dir, appconfig.File))
	}
	if t.OS == "linux" {
		// One program likewise; no app.
		exe := filepath.Join(dir, "asuka-host")
		if err := appbuild.LinuxPlayer(t, app.c, append(stdNativeDirs(), desktopNativeDirs(app)...), exe); err != nil {
			return "", err
		}
		return exe, writeHostConfig(app, platform, "", false, filepath.Join(dir, appconfig.File))
	}
	return "", fmt.Errorf("no desktop player for %s", t)
}

// desktopNativeDirs are the directories of the app's native packages
// (its own, its dependencies', the std and platform packages it imports) with C
// for the desktop (a desktop/), which a C host's executable is built
// with.
func desktopNativeDirs(app *asukaApp) []string {
	ps, err := appNatives(app)
	if err != nil {
		return nil
	}
	var out []string
	for _, p := range ps {
		if nativegen.HasPlatform(p.dir, "desktop") {
			out = append(out, p.dir)
		}
	}
	return out
}

// stdNativeDirs are the standard library's and the platform SDK's
// packages with C for the desktop: a dev player has them all, as the
// phones' players have their native code.
func stdNativeDirs() []string {
	ds, err := std.PackageDirs()
	if err != nil {
		return nil
	}
	var out []string
	for _, d := range ds {
		if st, err := os.Stat(filepath.Join(d, nativegen.DesktopDir)); err == nil && st.IsDir() {
			out = append(out, d)
		}
	}
	return out
}

// devDirs is the environment that puts an app's directories (std/fs's
// appDir, configDir, cacheDir, logDir) in its work directory under
// development, not the user's, the data directory made.
func devDirs(work string) []string {
	data := filepath.Join(work, "data")
	os.MkdirAll(data, 0o755)
	return []string{"ASUKA_DATA=" + data, "ASUKA_CONFIG=" + filepath.Join(work, "config"),
		"ASUKA_CACHE=" + filepath.Join(work, "cache"), "ASUKA_LOGS=" + filepath.Join(work, "logs")}
}
