package main

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"strings"

	"github.com/egoist/asuka/internal/appbuild"
	"github.com/egoist/asuka/internal/cbuild"
	"github.com/egoist/asuka/lang/bytecode"
	"github.com/egoist/asuka/lang/target"
)

// Programs (docs/programs.md): a package whose fn main is its entry, a
// command-line tool with no UI. asuka build makes one an executable for
// this computer, or with --target for another desktop (Zig).

// programTargets are --target's values.
var programTargets = []string{"darwin-amd64", "darwin-arm64", "linux-amd64", "linux-arm64", "windows-amd64", "windows-arm64"}

// parseTarget reads --target (os-arch); "": this computer.
func parseTarget(s string) (cbuild.Target, error) {
	if s == "" {
		return cbuild.Host(), nil
	}
	for _, t := range programTargets {
		if s == t {
			os, arch, _ := strings.Cut(s, "-")
			return cbuild.Target{OS: os, Arch: arch}, nil
		}
	}
	return cbuild.Target{}, fmt.Errorf("--target %s: one of %s", s, strings.Join(programTargets, ", "))
}

// projectRoot is the directory of the project p is (or is a [bin] of).
func projectRoot(p *appbuild.Project) string {
	if p.Root != "" {
		return p.Root
	}
	return p.Dir
}

// programApp is the program in p (the project's own package, or one of
// its [bin]s) as the build sees an app: its directory, the project's
// asuka.toml (its version), what it uses on platform.
func programApp(p *appbuild.Project, platform target.Platform) (*asukaApp, error) {
	cfg, err := loadConfig(projectRoot(p))
	if err != nil {
		return nil, err
	}
	app := &asukaApp{dir: p.Dir, name: p.ProgramName(), bundleID: cfg.ID, cfg: cfg, proj: p, platform: platform}
	app.refresh()
	return app, nil
}

// programExeName is the executable's file name for t.
func programExeName(name string, t cbuild.Target) string {
	if t.OS == "windows" {
		return name + ".exe"
	}
	return name
}

// buildProgram builds the program in p for t into dir: its executable,
// whose path it returns.
func buildProgram(p *appbuild.Project, t cbuild.Target, dir, version string, verbose bool) (string, error) {
	return programExe(p, t, dir, version, verbose)
}

// programExe builds the program in p for t into dir.
func programExe(p *appbuild.Project, t cbuild.Target, dir, version string, verbose bool) (string, error) {
	app, err := programApp(p, osPlatform(t.OS))
	if err != nil {
		return "", err
	}
	if version != "" {
		app.cfg.Version = strings.TrimPrefix(version, "v")
	}
	m, err := compileProject(p, osPlatform(t.OS))
	if err != nil {
		return "", err
	}
	if m.Main < 0 {
		return "", fmt.Errorf("%s isn't a program: it has no fn main (asuka build --platform builds an app)", cwdOr(p.Dir))
	}
	if p.Root != "" && hasAppBlock(p.Root) {
		// An app's program: its data is the app's (a CLI over the app's
		// notes), where the app's host keeps it on each desktop.
		c := *m
		c.Data = appDataFolder(app.cfg, p.Root, t.OS)
		m = &c
	}
	exe := filepath.Join(dir, programExeName(app.name, t))
	if verbose {
		fmt.Printf("asuka: building %s for %s\n", app.name, t)
	}
	if t.OS == "darwin" {
		natives, err := nativeFor(app, "macos")
		if err != nil {
			return "", err
		}
		if len(natives) > 0 {
			if runtime.GOOS != "darwin" {
				return "", errors.New("a program whose packages have Swift (std/http's URLSession) is built for macOS on a Mac")
			}
			if err := macProgram(app, m, natives, t.Arch, exe, verbose); err != nil {
				return "", err
			}
			return exe, nil
		}
	}
	err = appbuild.ProgramExe(t, m, appbuild.ProgramBuild{
		Name: app.name, Version: app.cfg.Version, Natives: desktopNativeDirs(app), Root: projectRoot(p),
	}, exe)
	if err != nil {
		return "", err
	}
	return exe, nil
}

// cmdBuildProgram is asuka build for a program: dist/<name> for this
// computer, dist/<os>-<arch>/<name> for --target's.
func cmdBuildProgram(p *appbuild.Project, o *buildOptions) error {
	t, err := parseTarget(o.target)
	if err != nil {
		return err
	}
	if o.platform != "" && o.platform != thisComputer() {
		return fmt.Errorf("--platform %s builds an app: a program takes --target (%s)", o.platform, strings.Join(programTargets, ", "))
	}
	root := projectRoot(p)
	dir := o.out
	if dir == "" {
		dir = filepath.Join(root, "dist")
	}
	if o.target != "" {
		dir = filepath.Join(dir, t.String())
	}
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return err
	}
	exe, err := buildProgram(p, t, dir, o.version, o.verbose)
	if err != nil {
		return err
	}
	fmt.Printf("asuka: %s\n", relToCwd(exe))
	return nil
}

// hasAppBlock reports whether dir's package has an app block: a project
// that's an app.
func hasAppBlock(dir string) bool {
	files, _ := filepath.Glob(filepath.Join(dir, "*.asuka"))
	for _, f := range files {
		b, err := os.ReadFile(f)
		if err != nil {
			continue
		}
		for _, l := range strings.Split(string(b), "\n") {
			if strings.HasPrefix(l, "app {") || l == "app{" || strings.HasPrefix(l, "app{") {
				return true
			}
		}
	}
	return false
}

// appDataFolder is the folder an app's host keeps its data in on goos
// (the hosts' rules: macOS's and Linux's by its ID, Windows' by its
// name).
func appDataFolder(cfg *appConfig, root, goos string) string {
	if goos == "windows" {
		if cfg.Name != "" {
			return cfg.Name
		}
		return filepath.Base(root)
	}
	return cfg.ID
}

// isProgramDir reports whether dir is a program's directory (its package
// declares fn main).
func isProgramDir(dir string) bool {
	st, err := os.Stat(dir)
	if err != nil || !st.IsDir() {
		return false
	}
	abs, _ := filepath.Abs(dir)
	return isProgram(&appbuild.Project{Dir: abs})
}

// relToCwd is path relative to the working directory, when it's in it.
func relToCwd(path string) string {
	wd, err := os.Getwd()
	if err != nil {
		return path
	}
	if r, err := filepath.Rel(wd, path); err == nil && filepath.IsLocal(r) {
		return r
	}
	return path
}

// macProgram builds a program whose packages have Swift for macOS: the
// Mac host's Swift with theirs (the packages' runtime: NativeModules), no
// window, its entry a headless one (AsukaProgram), linked with the core
// and the program compiled to C.
func macProgram(app *asukaApp, m *bytecode.Module, natives []nativePackage, arch, exe string, verbose bool) error {
	_ = natives // nativeFor's again, in executable
	work, err := os.MkdirTemp("", "asuka-program-")
	if err != nil {
		return err
	}
	defer os.RemoveAll(work)
	b := &macBuild{app: app, work: work, release: true, program: true, run: macRunner(verbose)}
	tmp := filepath.Join(work, "program")
	if err := b.executable(tmp, []string{arch}, relativeFiles(appbuild.StripTests(m), projectRoot(app.proj))); err != nil {
		return err
	}
	if b.packages != nil && b.packages.bundled() {
		return errors.New("the program's packages bring frameworks or resources, which a program's one executable can't carry: build it as an app")
	}
	return copyFile(tmp, exe)
}

// relativeFiles is m with its functions' files relative to root (what an
// error's trace names).
func relativeFiles(m *bytecode.Module, root string) *bytecode.Module {
	c := *m
	c.Funcs = append([]bytecode.FuncDef{}, m.Funcs...)
	for i := range c.Funcs {
		if r, err := filepath.Rel(root, c.Funcs[i].File); err == nil && filepath.IsLocal(r) {
			c.Funcs[i].File = filepath.ToSlash(r)
		}
	}
	return &c
}

// bundlePrograms builds the project's [bin] programs into a desktop
// app's build for t (docs/programs.md): each executable in dir (the
// Mac's Contents/Helpers, Linux's bin, beside Windows' .exe). macOS's
// are universal, as the app is. It returns the executables.
func bundlePrograms(app *asukaApp, t cbuild.Target, archs []string, dir string, verbose bool) ([]string, error) {
	if app.proj == nil || len(app.proj.Bins) == 0 {
		return nil, nil
	}
	var out []string
	for _, b := range app.proj.Bins {
		p, err := app.proj.Bin(b.Name)
		if err != nil {
			return nil, err
		}
		fmt.Printf("asuka: building %s, the app's program\n", b.Name)
		if len(archs) == 0 {
			archs = []string{t.Arch}
		}
		var parts []string
		work, err := os.MkdirTemp("", "asuka-bin-")
		if err != nil {
			return nil, err
		}
		defer os.RemoveAll(work)
		for _, arch := range archs {
			at := cbuild.Target{OS: t.OS, Arch: arch}
			d := filepath.Join(work, arch)
			if err := os.MkdirAll(d, 0o755); err != nil {
				return nil, err
			}
			exe, err := programExe(p, at, d, app.cfg.Version, verbose)
			if err != nil {
				return nil, fmt.Errorf("[bin.%s]: %w", b.Name, err)
			}
			parts = append(parts, exe)
		}
		if err := os.MkdirAll(dir, 0o755); err != nil {
			return nil, err
		}
		dst := filepath.Join(dir, programExeName(b.Name, t))
		if len(parts) == 1 {
			if err := copyFile(parts[0], dst); err != nil {
				return nil, err
			}
		} else {
			os.Remove(dst)
			if err := macRunner(verbose)(work, nil, "lipo", append([]string{"-create", "-output", dst}, parts...)...); err != nil {
				return nil, err
			}
		}
		out = append(out, dst)
	}
	return out, nil
}

// devProgram is asuka dev for a program: it runs main on the VM with
// args, and with each change saved, stops the run (if it's still going)
// and runs it again, as cargo watch -x run does.
func devProgram(dir, bin string, args []string, stop chan os.Signal) error {
	root, err := appbuild.Load(dir)
	if err != nil {
		return err
	}
	p := root
	if bin != "" {
		if p, err = root.Bin(bin); err != nil {
			return err
		}
	}
	roots := p.WatchRoots()
	if p.Root != "" {
		roots = append(roots, p.Root)
	}
	w := newWatcher(roots...)
	ended := make(chan int, 1)
	var run *appbuild.Run
	start := func() {
		p.Forget()
		m, err := compileProject(p, target.Host())
		watchEmbeds(w, p.Uses(target.Host()))
		if err != nil {
			fmt.Fprintln(os.Stderr, "asuka:", err)
			return
		}
		r, err := appbuild.StartProgram(m, appbuild.ProgramOptions{Name: p.ProgramName(), Args: args, Trim: cwdOr(p.Dir), Stdin: os.Stdin, Stdout: os.Stdout, Stderr: os.Stderr})
		if err != nil {
			fmt.Fprintln(os.Stderr, "asuka:", err)
			return
		}
		run = r
		go func() {
			code, err := r.Wait()
			if err != nil {
				fmt.Fprintln(os.Stderr, "asuka:", err)
			}
			ended <- code
		}()
	}
	halt := func() {
		if run == nil {
			return
		}
		run.Cmd.Process.Kill()
		<-ended
		run = nil
	}
	fmt.Printf("asuka: running %s; again with each change (Ctrl-C ends it)\n", p.ProgramName())
	start()
	for {
		select {
		case <-stop:
			halt()
			return nil
		case code := <-ended:
			run = nil
			fmt.Printf("asuka: %s exited with %d; waiting for a change\n", p.ProgramName(), code)
		case <-w.wake:
			if src, proj := w.changed(); !src && !proj {
				continue
			}
			halt()
			fmt.Println("asuka: changed: running again")
			start()
		}
	}
}
