package main

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"time"

	"github.com/egoist/asuka/internal/appbuild"
	"github.com/egoist/asuka/lang/bytecode"
	"github.com/egoist/asuka/lang/target"
)

// cmdTest is asuka test: an app's or a package's test blocks, compiled
// and run on the fake host (asuka-run: the VM, the core and the fake
// host built for this computer).
func cmdTest(args []string) error {
	var pattern, platform string
	fs := newFlagSet("test", "[dir]")
	fs.about = "Runs the test blocks of the app or package in dir (default: the working directory) on the\n" +
		"fake host: the app's views as a phone would show them, measured and laid out, driven by\n" +
		"tap, type and expect; and those of its programs (asuka.toml's [bin.<name>])."
	fs.String(&pattern, "run", 0, "", "run only the tests whose names contain `text`")
	platformFlag(fs, &platform, []string{platformIOS, platformAndroid, platformMac, platformWindows, platformLinux, platformWeb},
		"the `platform` the tests are compiled for (its packages' files for it, @platform) and the fake host says it is: ios (the default), android, mac, windows, linux or web")
	if err := fs.parse(args); err != nil {
		return err
	}
	dir := fs.Arg(0)
	if dir == "" {
		dir = "."
	}
	p, err := appbuild.Load(dir)
	if err != nil {
		return err
	}
	targets := []*appbuild.Project{p}
	for _, b := range p.Bins {
		bp, err := p.Bin(b.Name)
		if err != nil {
			return err
		}
		targets = append(targets, bp)
	}
	var failed error
	ran := 0
	for i, t := range targets {
		if i > 0 || len(targets) > 1 {
			if !hasSources(t.Dir) {
				continue
			}
			name := "the project"
			if i > 0 {
				name = "[bin." + p.Bins[i-1].Name + "]"
			}
			fmt.Printf("asuka: %s (%s)\n", name, relToCwd(t.Dir))
		}
		m, err := compileProject(t, langPlatform(platform))
		if err != nil {
			return err
		}
		if len(m.Tests) == 0 && len(targets) > 1 {
			continue
		}
		ran++
		if err := runTests(m, t, pattern, platform); err != nil && failed == nil {
			failed = err
		}
	}
	if ran == 0 && len(targets) > 1 {
		fmt.Println("asuka: no test blocks")
	}
	return failed
}

// hasSources reports whether dir has .asuka files: a package.
func hasSources(dir string) bool {
	m, _ := filepath.Glob(filepath.Join(dir, "*.asuka"))
	return len(m) > 0
}

// testPlatform is the fake host's name for a --platform.
func testPlatform(p string) string {
	if p == platformMac {
		return "macos"
	}
	return p
}

func runTests(m *bytecode.Module, p *appbuild.Project, pattern, platform string) error {
	if len(m.Tests) == 0 {
		fmt.Println("asuka: no test blocks")
		return nil
	}
	start := time.Now()
	s, err := appbuild.RunTests(m, appbuild.TestOptions{Pattern: pattern, Platform: testPlatform(platform), Dir: cwdOr(p.Dir)}, os.Stdout)
	if err != nil {
		return err
	}
	n, failed := len(s.Results), s.Failed()
	if n == 0 {
		return fmt.Errorf("no test's name contains %q", pattern)
	}
	if failed > 0 {
		return fmt.Errorf("%d of %d %s failed", failed, n, plural(n, "test", "tests"))
	}
	fmt.Printf("ok   %d %s passed in %.2fs\n", n, plural(n, "test", "tests"), time.Since(start).Seconds())
	return nil
}

// cwdOr is the working directory, what reports are relative to, when
// dir is in it; else dir.
func cwdOr(dir string) string {
	wd, err := os.Getwd()
	if err != nil {
		return dir
	}
	if r, err := filepath.Rel(wd, dir); err == nil && (r == "." || filepath.IsLocal(r)) {
		return wd
	}
	return dir
}

// compileProject compiles p for platform, printing its diagnostics
// relative to the working directory.
func compileProject(p *appbuild.Project, platform target.Platform) (*bytecode.Module, error) {
	m, diags, err := p.Compile(platform)
	base := cwdOr(p.Dir)
	for _, d := range diags {
		fmt.Fprintln(os.Stderr, relDiag(base, d))
	}
	if err != nil {
		return nil, err
	}
	if m == nil {
		return nil, fmt.Errorf("%d %s", len(diags), plural(len(diags), "error", "errors"))
	}
	return m, nil
}

// cmdRun is asuka run: a program's main on the VM, with the arguments
// after --; or one file (or a directory) without one, compiled and run on
// the fake host: its test blocks, and what its app shows.
func cmdRun(args []string) error {
	var pattern, bin string
	var progArgs []string
	if i := slices.Index(args, "--"); i >= 0 {
		args, progArgs = args[:i], args[i+1:]
	}
	fs := newFlagSet("run", "[dir | file.asuka] [-- args...]")
	fs.about = "Runs a program (a package with fn main, or asuka.toml's [bin.<name>]) on the VM,\n" +
		"with the arguments after --, and exits with its code. A file or a package that isn't a\n" +
		"program runs on the fake host: its test blocks, if it has any, and its app, whose screen\n" +
		"it prints."
	fs.String(&bin, "bin", 0, "", "the `program` to run: one of asuka.toml's [bin.<name>] (needed when there are several)")
	fs.String(&pattern, "run", 0, "", "run only the tests whose names contain `text`")
	if err := fs.parse(args); err != nil {
		return err
	}
	path := fs.Arg(0)
	if path == "" {
		path = "."
	}
	p, err := appbuild.Load(path)
	if err != nil {
		return err
	}
	if p.File == "" {
		if p, err = programOf(p, bin); err != nil {
			return err
		}
	}
	// A program runs here: it's compiled for this computer. What isn't
	// runs on the fake host, which is an iPhone as asuka test's is.
	m, err := compileProject(p, target.Host())
	if err != nil {
		return err
	}
	if m.Main < 0 && target.Host() != target.Default {
		if m, err = compileProject(p, target.Default); err != nil {
			return err
		}
	}
	if m.Main >= 0 {
		var env []string
		if p.Root != "" && hasAppBlock(p.Root) && os.Getenv("ASUKA_DATA") == "" {
			// An app's program shares the app's data: under development,
			// what asuka dev's run on this computer keeps.
			if w, err := workDir(&asukaApp{dir: p.Root, name: filepath.Base(p.Root)}, thisComputer()); err == nil {
				data := filepath.Join(w, "data")
				os.MkdirAll(data, 0o755)
				env = append(env, "ASUKA_DATA="+data)
			}
		}
		code, err := appbuild.RunProgram(m, appbuild.ProgramOptions{Name: p.ProgramName(), Args: progArgs, Env: env, Trim: cwdOr(p.Dir), Stdin: os.Stdin, Stdout: os.Stdout, Stderr: os.Stderr})
		if err != nil {
			return err
		}
		if code != 0 {
			return exitCode(code)
		}
		return nil
	}
	if len(m.Tests) == 0 && m.App < 0 {
		return errors.New("nothing to run: no fn main, no test blocks, and no app block")
	}
	var testErr error
	if len(m.Tests) > 0 {
		testErr = runTests(m, p, pattern, "")
	}
	if m.App >= 0 {
		if err := appbuild.ShowApp(m, os.Stdout); err != nil {
			return err
		}
	}
	return testErr
}

// exitCode is an error that ends the CLI with a program's exit code,
// saying nothing more: the program said what went wrong.
type exitCode int

func (c exitCode) Error() string { return fmt.Sprintf("exit status %d", int(c)) }

// programOf is the project's program to run or build: --bin's, else the
// project itself (an app's or a library's, or a program), unless it has
// exactly one [bin] and isn't a program itself, which is then the one.
func programOf(p *appbuild.Project, bin string) (*appbuild.Project, error) {
	if bin != "" {
		return p.Bin(bin)
	}
	if len(p.Bins) == 0 || isProgram(p) {
		return p, nil
	}
	if len(p.Bins) == 1 {
		return p.Bin(p.Bins[0].Name)
	}
	var names []string
	for _, b := range p.Bins {
		names = append(names, b.Name)
	}
	return nil, fmt.Errorf("which program? --bin %s", strings.Join(names, ", --bin "))
}

// isProgram reports whether the project's own package declares fn main
// (a quick look: the compiler decides).
func isProgram(p *appbuild.Project) bool {
	files, _ := filepath.Glob(filepath.Join(p.Dir, "*.asuka"))
	for _, f := range files {
		b, err := os.ReadFile(f)
		if err != nil {
			continue
		}
		for _, l := range strings.Split(string(b), "\n") {
			l = strings.TrimSpace(l)
			l = strings.TrimPrefix(l, "pub ")
			l = strings.TrimPrefix(l, "async ")
			if strings.HasPrefix(l, "fn main(") {
				return true
			}
		}
	}
	return false
}
