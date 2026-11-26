package main

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"slices"
	"strconv"
	"strings"
	"time"

	"github.com/egoist/asuka/internal/appbuild"
	"github.com/egoist/asuka/lang/bytecode"
	"github.com/egoist/asuka/lang/check"
	"github.com/egoist/asuka/lang/diag"
	"github.com/egoist/asuka/lang/ir"
	"github.com/egoist/asuka/lang/target"
)

// jsonOut: asuka check's and asuka test's --json, the diagnostics (and
// the tests' results) as JSON lines on stdout.
var jsonOut bool

// cmdTest is asuka test: an app's or a package's test blocks, compiled
// and run on the fake host (asuka-run: the VM, the core and the fake
// host built for this computer).
func cmdTest(args []string) error {
	var pattern, platform, benchtime, compare, seed, fuzztime string
	var release, gui bool
	var device, timeout string
	tr := &testRun{}
	jsonOut = false
	fs := newFlagSet("test", "[dir]")
	fs.Bool(&jsonOut, "json", 0, "write the diagnostics and each test's result as JSON, one object per line (a diagnostic: code, severity, file, range, message, help, url; a test: test, ok, message, pos, property; a bench: bench, nsPerOp, allocsPerOp, bytesPerOp, ...; coverage: cover, lines, branches)")
	fs.about = "Runs the test blocks of the app or package in dir (default: the working directory) on the\n" +
		"fake host: the app's views as a phone would show them, measured and laid out, driven by\n" +
		"tap, type and expect; and those of its programs (asuka.toml's [bin.<name>]). A test with\n" +
		"parameters is a property test, run with generated inputs; a bench runs once here, and is\n" +
		"timed with --bench. With --gui, it runs the app's flows (flows/*.flow: tap Add, see Count: 1)\n" +
		"on a platform's player instead: the simulator, an emulator, headless Chrome or this computer."
	fs.String(&pattern, "run", 0, "", "run only the tests (and benches) whose names contain `text`")
	platformFlag(fs, &platform, []string{platformIOS, platformAndroid, platformMac, platformWindows, platformLinux, platformWeb},
		"the `platform` the tests are compiled for (its packages' files for it, @platform) and the fake host says it is: ios (the default), android, mac, windows, linux or web")
	fs.Bool(&release, "release", 0, "compile the tests as a release build is: @debug is false (it's true by default, as under asuka dev)")
	fs.Bool(&tr.bench, "bench", 0, "time the benches (bench \"name\" { … }) instead of running the tests: compiled to C, as a release build is (@debug false); each's time, allocations and bytes per iteration")
	fs.String(&benchtime, "benchtime", 0, "1s", "how long each bench's samples take together (`time`: 500ms, 2s), or how many iterations each runs (100x)")
	fs.String(&compare, "compare", 0, "", "compare the benches with a run that asuka test --bench --json wrote to `file`")
	fs.Bool(&tr.vm, "vm", 0, "time the benches on the VM, not compiled to C")
	fs.String(&seed, "seed", 0, "", "the `seed` property tests' inputs are generated from (default 0, the same every run; random: a new one, said)")
	fs.Int(&tr.cases, "cases", 0, 100, "how many generated inputs (`n`) each property test tries")
	fs.Bool(&tr.fuzz, "fuzz", 0, "fuzz the property tests instead of running the tests: new inputs until --fuzztime, those that reach new code kept and changed; a failing one is shrunk and saved in testdata/fuzz, which every run then tries")
	fs.String(&fuzztime, "fuzztime", 0, "10s", "how long to fuzz each property test (`time`), or how many inputs (10000x)")
	fs.Bool(&tr.update, "update", 'u', "write the snapshots that differ from what the tests show, and remove those no test takes")
	fs.Bool(&tr.cover, "cover", 0, "measure the lines and branches of the package's code the tests run: a summary per file")
	fs.String(&tr.lcov, "cover-lcov", 0, "", "write the coverage to `file` in lcov's format, for editors and CI (--cover)")
	fs.String(&tr.html, "cover-html", 0, "", "write the coverage to `file` as a page: the source, what ran and what didn't (--cover)")
	fs.Bool(&gui, "gui", 0, "run the app's flows (flows/*.flow) on the --platform's player, as asuka dev runs it (default: this computer; ios: the simulator, android: an emulator, web: headless Chrome), instead of its test blocks")
	fs.String(&device, "device", 'd', "", "--gui: the simulator, emulator or device to run on, as asuka dev's --device")
	fs.String(&timeout, "timeout", 0, "10s", "--gui: how long a step is tried before its flow fails (`time`)")
	if err := fs.parse(args); err != nil {
		return err
	}
	if gui {
		return testGUI(fs.Arg(0), platform, device, pattern, timeout)
	}
	tr.pattern, tr.platform = pattern, platform
	// Tests run a development build, as asuka dev's; benches time what a
	// release runs.
	appbuild.Debug = !release && !tr.bench
	tr.ci = inCI()
	if tr.lcov != "" || tr.html != "" {
		tr.cover = true
	}
	var err error
	if tr.benchTime, tr.benchN, err = parseCount("benchtime", benchtime); err != nil {
		return err
	}
	if tr.fuzzTime, tr.fuzzCases, err = parseCount("fuzztime", fuzztime); err != nil {
		return err
	}
	switch {
	case tr.bench && tr.fuzz:
		return errors.New("--bench times the benches and --fuzz fuzzes the property tests: one at a time")
	case tr.bench && tr.cover:
		return errors.New("--cover measures what the tests run, and --bench times the benches: one at a time")
	case tr.vm && !tr.bench:
		return errors.New("--vm says where --bench runs the benches")
	case compare != "" && !tr.bench:
		return errors.New("--compare compares the benches: with --bench")
	}
	if compare != "" {
		if tr.baseline, err = appbuild.ReadBaseline(compare); err != nil {
			return err
		}
	}
	switch seed {
	case "":
	case "random":
		tr.seed = uint64(time.Now().UnixNano())
		if !jsonOut {
			fmt.Printf("asuka: seed %d\n", tr.seed)
		}
	default:
		if tr.seed, err = strconv.ParseUint(seed, 10, 64); err != nil {
			return fmt.Errorf("--seed %s: a whole number, or random", seed)
		}
	}
	if tr.fuzz && seed == "" {
		// Fuzzing looks for what the seeded runs didn't try.
		tr.seed = uint64(time.Now().UnixNano())
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
	var bins []appbuild.Bin // the Asuka programs: another toolchain tests its own
	for _, b := range p.Bins {
		if !b.Built() {
			bins = append(bins, b)
		}
	}
	for _, b := range bins {
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
				name = "[bin." + bins[i-1].Name + "]"
			}
			fmt.Printf("asuka: %s (%s)\n", name, relToCwd(t.Dir))
		}
		var m *bytecode.Module
		var cov *ir.Cover
		if tr.cover || tr.fuzz {
			// Statements count their runs: coverage's, and what tells the
			// fuzzer an input reached new code.
			m, cov, err = compileCover(t, langPlatform(platform))
		} else {
			m, err = compileTests(t, langPlatform(platform))
		}
		if err != nil {
			return err
		}
		if len(m.Tests) == 0 && len(targets) > 1 {
			continue
		}
		if len(targets) > 1 && (tr.bench && !hasBenches(m, pattern) || tr.fuzz && !hasProperties(m, pattern)) {
			continue
		}
		ran++
		if err := tr.runPackage(m, cov, t); err != nil && failed == nil {
			failed = err
		}
	}
	if ran == 0 && len(targets) > 1 {
		switch {
		case tr.bench:
			fmt.Println("asuka: no benches")
		case tr.fuzz:
			fmt.Println("asuka: no property tests")
		default:
			fmt.Println("asuka: no test blocks")
		}
	}
	if tr.coverage != nil {
		if err := tr.coverage.report(tr); err != nil && failed == nil {
			failed = err
		}
	}
	return failed
}

// compileCover compiles p as compileProject does, its statements
// counting their runs.
func compileCover(p *appbuild.Project, platform target.Platform) (*bytecode.Module, *ir.Cover, error) {
	m, cov, diags, err := p.CompileCover(platform)
	base := cwdOr(p.Dir)
	for _, d := range diags {
		d = relDiag(base, d)
		if jsonOut {
			fmt.Println(d.Record(diag.Error).JSON())
		} else {
			fmt.Fprintln(os.Stderr, d.Format(diag.Error, ""))
		}
	}
	if err != nil {
		return nil, nil, err
	}
	if m == nil {
		return nil, nil, fmt.Errorf("%d %s", len(diags), plural(len(diags), "error", "errors"))
	}
	return m, cov, nil
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

// runTests runs m's tests, of the package p (asuka run's).
func runTests(m *bytecode.Module, p *appbuild.Project, pattern, platform string) error {
	tr := &testRun{pattern: pattern, platform: platform, ci: inCI()}
	return tr.runPackage(m, nil, p)
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

// compileProject builds p for platform (no tests), printing its
// diagnostics relative to the working directory.
func compileProject(p *appbuild.Project, platform target.Platform) (*bytecode.Module, error) {
	m, diags, err := p.Compile(platform)
	return reportCompile(p, m, diags, err)
}

// compileTests is compileProject's test build: its test files and test
// blocks are part of it.
func compileTests(p *appbuild.Project, platform target.Platform) (*bytecode.Module, error) {
	m, diags, err := p.CompileTests(platform)
	return reportCompile(p, m, diags, err)
}

func reportCompile(p *appbuild.Project, m *bytecode.Module, diags []check.Diagnostic, err error) (*bytecode.Module, error) {
	base := cwdOr(p.Dir)
	for _, d := range diags {
		d = relDiag(base, d)
		if jsonOut {
			fmt.Println(d.Record(diag.Error).JSON())
		} else {
			fmt.Fprintln(os.Stderr, d.Format(diag.Error, ""))
		}
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
	var release bool
	fs.String(&bin, "bin", 0, "", "the `program` to run: one of asuka.toml's [bin.<name>] (needed when there are several)")
	fs.String(&pattern, "run", 0, "", "run only the tests whose names contain `text`")
	fs.Bool(&release, "release", 0, "compile it as a release build is: @debug is false (it's true by default, as under asuka dev)")
	if err := fs.parse(args); err != nil {
		return err
	}
	appbuild.Debug = !release
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
				env = append(env, devDirs(w)...)
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
	// Not a program: its tests, then its app, from a test build.
	if m, err = compileTests(p, target.Default); err != nil {
		return err
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
	var names []string // the Asuka programs: one another toolchain builds isn't run here
	for _, b := range p.Bins {
		if !b.Built() {
			names = append(names, b.Name)
		}
	}
	if len(names) == 0 || isProgram(p) {
		return p, nil
	}
	if len(names) == 1 {
		return p.Bin(names[0])
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
