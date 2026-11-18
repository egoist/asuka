package compile

import (
	"bufio"
	"bytes"
	"errors"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"slices"
	"strings"

	"github.com/egoist/asuka/internal/cbuild"
	"github.com/egoist/asuka/lang/bytecode"
	"github.com/egoist/asuka/lang/cgen"
	"github.com/egoist/asuka/std"
)

// FindCore finds the C core's directory (core/, with core/runner): from
// ASUKA_CORE, beside this source file (go run, go test), or up from the
// working directory.
func FindCore() (string, error) {
	ok := func(d string) bool {
		_, err := os.Stat(filepath.Join(d, "runner", "main.c"))
		return err == nil
	}
	if d := os.Getenv("ASUKA_CORE"); d != "" {
		if ok(d) {
			return d, nil
		}
		return "", errors.New("ASUKA_CORE (" + d + ") isn't the core's directory: it has no runner/main.c")
	}
	if _, file, _, found := runtime.Caller(0); found {
		d := filepath.Join(filepath.Dir(file), "..", "..", "core")
		if ok(d) {
			return filepath.Clean(d), nil
		}
	}
	wd, _ := os.Getwd()
	for d := wd; ; {
		if ok(filepath.Join(d, "core")) {
			return filepath.Join(d, "core"), nil
		}
		parent := filepath.Dir(d)
		if parent == d {
			break
		}
		d = parent
	}
	return "", errors.New("can't find the C core (core/runner/main.c): set ASUKA_CORE to its directory")
}

// Runner builds (once, cached) the VM runner, asuka-run.
func Runner(sanitize bool) (string, error) {
	core, err := FindCore()
	if err != nil {
		return "", err
	}
	return cbuild.Runner(core, sanitize)
}

// Run runs a module on the VM with runner: its tests matching pattern
// with test set (pattern "" for all), else its app. It returns what the
// runner printed, and an error when it failed (a test failed, or the
// module didn't load). flags go to the runner first (-leaks).
func Run(runner string, m *bytecode.Module, test bool, pattern string, flags ...string) (string, error) {
	dir, err := os.MkdirTemp("", "asuka-run")
	if err != nil {
		return "", err
	}
	defer os.RemoveAll(dir)
	p := filepath.Join(dir, "module.akb")
	if err := os.WriteFile(p, m.Encode(), 0o644); err != nil {
		return "", err
	}
	args := append([]string{}, flags...)
	if test {
		args = append(args, "-test")
		if pattern != "" {
			args = append(args, pattern)
		}
	}
	args = append(args, p)
	cmd := exec.Command(runner, args...)
	cmd.Env = runnerEnv(flags)
	var out bytes.Buffer
	cmd.Stdout, cmd.Stderr = &out, &out
	err = cmd.Run()
	return out.String(), err
}

// Foreign is the C m calls, for a runner: its externs' glue and units,
// the C they call (asuka.toml's [c], read by the CLI; a test's own), with
// link's flags, and the C of the std packages it imports (std/sqlite's
// SQLite). Nil when m has none.
func Foreign(m *bytecode.Module, units []cbuild.CUnit, link []string) (*cbuild.Foreign, error) {
	if m.C == nil {
		return nil, nil
	}
	f := &cbuild.Foreign{Units: units, Link: link}
	if m.C.Glue != nil {
		glue, err := cbuild.CacheFile("ffi", "ffi.c", m.C.Glue)
		if err != nil {
			return nil, err
		}
		f.Glue = glue
	}
	for _, d := range m.C.Std {
		u, l, err := cbuild.StdC(d, cbuild.Host(), std.Embedded())
		if err != nil {
			return nil, err
		}
		f.Libs = append(f.Libs, u)
		f.Link = append(f.Link, l...)
	}
	return f, nil
}

// RunnerFor builds asuka-run with the C m's externs call (f, from
// Foreign) in it, linked in dir (the caller's, which it removes).
func RunnerFor(f *cbuild.Foreign, sanitize bool, dir string) (string, error) {
	core, err := FindCore()
	if err != nil {
		return "", err
	}
	return cbuild.RunnerFor(core, cbuild.RunnerOptions{Foreign: f, Sanitize: sanitize, Dir: dir})
}

// RunC runs a module compiled to C (lang/cgen), as a release build runs
// it: the C is built with the core into a runner (its objects cached),
// which runs it as Run's runner runs bytecode.
func RunC(m *bytecode.Module, sanitize, test bool, pattern string) (string, error) {
	return RunCWith(m, nil, sanitize, test, pattern)
}

// RunCWith is RunC with the C m's externs call (f, from Foreign) built
// in; flags go to the runner first, as Run's.
func RunCWith(m *bytecode.Module, f *cbuild.Foreign, sanitize, test bool, pattern string, flags ...string) (string, error) {
	core, err := FindCore()
	if err != nil {
		return "", err
	}
	src, err := cgen.Generate(m)
	if err != nil {
		return "", err
	}
	dir, err := os.MkdirTemp("", "asuka-runc")
	if err != nil {
		return "", err
	}
	defer os.RemoveAll(dir)
	app := filepath.Join(dir, "app.c")
	if err := os.WriteFile(app, src, 0o644); err != nil {
		return "", err
	}
	exe, err := cbuild.RunnerFor(core, cbuild.RunnerOptions{Extra: []string{app}, Foreign: f, Sanitize: sanitize, Dir: dir})
	if err != nil {
		return "", errors.New("the C the module compiles to doesn't build: " + err.Error())
	}
	args := append([]string{}, flags...)
	if test {
		args = append(args, "-test")
		if pattern != "" {
			args = append(args, pattern)
		}
	}
	args = append(args, "-aot")
	cmd := exec.Command(exe, args...)
	cmd.Env = runnerEnv(flags)
	var out bytes.Buffer
	cmd.Stdout, cmd.Stderr = &out, &out
	err = cmd.Run()
	return out.String(), err
}

// runnerEnv is a runner's environment for flags: with -leaks, which
// counts what each test leaves itself (and runs programs that leak on
// purpose, to see it said), without LeakSanitizer's check at exit.
func runnerEnv(flags []string) []string {
	if !slices.Contains(flags, "-leaks") {
		return nil
	}
	opts := "detect_leaks=0"
	if v := os.Getenv("ASAN_OPTIONS"); v != "" {
		opts = v + ":" + opts
	}
	return append(os.Environ(), "ASAN_OPTIONS="+opts)
}

// MainRun is how a program ran (RunMain, RunMainC): what it wrote and its
// exit code.
type MainRun struct {
	Stdout, Stderr string
	Code           int
}

// MainOptions are what RunMain and RunMainC run a program with.
type MainOptions struct {
	Args  []string // its arguments
	Stdin string   // its input
	Env   []string // more of its environment (KEY=value)
	// Signal, when set, is sent to it once it has written its first line
	// to stdout ("ready"): as a person's Ctrl-C would be.
	Signal os.Signal
}

// RunMain runs m's fn main on the VM with runner (asuka-run -main), as o
// says.
func RunMain(runner string, m *bytecode.Module, o MainOptions) (MainRun, error) {
	dir, err := os.MkdirTemp("", "asuka-run")
	if err != nil {
		return MainRun{}, err
	}
	defer os.RemoveAll(dir)
	p := filepath.Join(dir, "module.akb")
	if err := os.WriteFile(p, m.Encode(), 0o644); err != nil {
		return MainRun{}, err
	}
	return runMain(runner, append([]string{"-main", "-program", "prog", p, "--"}, o.Args...), o)
}

// RunMainC runs m's fn main compiled to C (lang/cgen), as RunMain does on
// the VM.
func RunMainC(m *bytecode.Module, sanitize bool, o MainOptions) (MainRun, error) {
	core, err := FindCore()
	if err != nil {
		return MainRun{}, err
	}
	src, err := cgen.Generate(m)
	if err != nil {
		return MainRun{}, err
	}
	dir, err := os.MkdirTemp("", "asuka-runc")
	if err != nil {
		return MainRun{}, err
	}
	defer os.RemoveAll(dir)
	app := filepath.Join(dir, "app.c")
	if err := os.WriteFile(app, src, 0o644); err != nil {
		return MainRun{}, err
	}
	var f *cbuild.Foreign
	if m.C != nil && len(m.C.Std) > 0 {
		// The C of the std packages it imports (std/sqlite's SQLite).
		if f, err = Foreign(&bytecode.Module{C: &bytecode.C{Std: m.C.Std}}, nil, nil); err != nil {
			return MainRun{}, err
		}
	}
	exe, err := cbuild.RunnerFor(core, cbuild.RunnerOptions{Extra: []string{app}, Foreign: f, Sanitize: sanitize, Dir: dir})
	if err != nil {
		return MainRun{}, errors.New("the C the module compiles to doesn't build: " + err.Error())
	}
	return runMain(exe, append([]string{"-main", "-program", "prog", "-aot", "--"}, o.Args...), o)
}

func runMain(exe string, args []string, o MainOptions) (MainRun, error) {
	cmd := exec.Command(exe, args...)
	cmd.Env = append(os.Environ(), o.Env...)
	var out, errb bytes.Buffer
	cmd.Stdin = strings.NewReader(o.Stdin)
	cmd.Stderr = &errb
	var err error
	if o.Signal == nil {
		cmd.Stdout = &out
		err = cmd.Run()
	} else {
		// Its first line, then the signal, then the rest.
		pipe, perr := cmd.StdoutPipe()
		if perr != nil {
			return MainRun{}, perr
		}
		if err = cmd.Start(); err != nil {
			return MainRun{}, err
		}
		rd := bufio.NewReader(pipe)
		first, _ := rd.ReadString('\n')
		out.WriteString(first)
		cmd.Process.Signal(o.Signal)
		io.Copy(&out, rd)
		err = cmd.Wait()
	}
	r := MainRun{Stdout: out.String(), Stderr: errb.String()}
	var exit *exec.ExitError
	if errors.As(err, &exit) && exit.ExitCode() >= 0 {
		r.Code = exit.ExitCode()
		return r, nil
	}
	return r, err
}
