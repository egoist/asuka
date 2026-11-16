package main

import (
	"bytes"
	"debug/elf"
	"debug/macho"
	"debug/pe"
	"errors"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"

	"github.com/egoist/asuka/internal/appbuild"
	"github.com/egoist/asuka/internal/cbuild"
	"github.com/egoist/asuka/lang/target"
)

// asuka new --bin makes a program: its test passes, asuka run runs it
// with the arguments after --, and passes its exit code on; asuka build
// makes an executable for this computer that does the same, and with
// Zig, for Linux and Windows (the Linux one run in the asuka-linux-test
// image when Docker has it).
func TestProgramCLI(t *testing.T) {
	if _, err := cbuild.Find(); err != nil {
		t.Skip(err)
	}
	dir := filepath.Join(t.TempDir(), "greet")
	if err := cmdNew([]string{"--bin", dir}); err != nil {
		t.Fatal(err)
	}
	for _, f := range []string{"asuka.toml", "main.asuka", ".gitignore"} {
		if _, err := os.Stat(filepath.Join(dir, f)); err != nil {
			t.Fatal(err)
		}
	}
	if err := cmdTest([]string{dir}); err != nil {
		t.Fatalf("asuka test: %v", err)
	}
	p, err := appbuild.Load(dir)
	if err != nil {
		t.Fatal(err)
	}
	m, err := compileProject(p, target.Host())
	if err != nil {
		t.Fatal(err)
	}
	var out bytes.Buffer
	code, err := appbuild.RunProgram(m, appbuild.ProgramOptions{Name: p.ProgramName(), Args: []string{"Ada"}, Stdout: &out, Stderr: &out})
	if err != nil || code != 0 || out.String() != "Hello, Ada!\n" {
		t.Fatalf("asuka run -- Ada: %d %v %q", code, err, out.String())
	}
	if err := cmdRun([]string{dir, "--", "Ada"}); err != nil {
		t.Fatalf("cmdRun: %v", err)
	}

	// An exit code, and an error main throws.
	main := `import std/os

enum E: Error { bad }

fn main() throws {
  let a = os.args()
  if a.first() == "fail" {
    throw E.bad
  }
  if let f = a.first(), let c = f.toInt() {
    os.exit(c)
  }
  print("args: {a}")
}
`
	if err := os.WriteFile(filepath.Join(dir, "main.asuka"), []byte(main), 0o644); err != nil {
		t.Fatal(err)
	}
	var code3 exitCode
	if err := cmdRun([]string{dir, "--", "3"}); !errors.As(err, &code3) || code3 != 3 {
		t.Fatalf("asuka run -- 3: %v", err)
	}

	// The executable.
	o := buildOptions{out: filepath.Join(t.TempDir(), "dist")}
	if err := cmdBuildProgram(p, &o); err != nil {
		t.Fatal(err)
	}
	exe := filepath.Join(o.out, programExeName("greet", cbuild.Host()))
	check := func(name string, cmd *exec.Cmd, wantCode int, wantOut, wantErr string) {
		t.Helper()
		var so, se bytes.Buffer
		cmd.Stdout, cmd.Stderr = &so, &se
		err := cmd.Run()
		got := 0
		var ee *exec.ExitError
		if errors.As(err, &ee) {
			got = ee.ExitCode()
		} else if err != nil {
			t.Fatalf("%s: %v", name, err)
		}
		if got != wantCode || !strings.Contains(so.String(), wantOut) || !strings.Contains(se.String(), wantErr) {
			t.Errorf("%s: exit %d (want %d)\nstdout: %q (want %q)\nstderr: %q (want %q)", name, got, wantCode, so.String(), wantOut, se.String(), wantErr)
		}
	}
	check("a b", exec.Command(exe, "a", "b"), 0, `args: ["a","b"]`, "")
	check("exit 5", exec.Command(exe, "5"), 5, "", "")
	check("fail", exec.Command(exe, "fail"), 1, "", "error: E.bad\n    main (main.asuka:")

	// Cross-compiled.
	if _, err := cbuild.Zig(); err != nil {
		t.Log("no Zig: no cross-compiled builds")
		return
	}
	arch := runtime.GOARCH
	for _, target := range []string{"linux-" + arch, "windows-amd64"} {
		o := buildOptions{out: filepath.Join(t.TempDir(), "dist"), target: target}
		if err := cmdBuildProgram(p, &o); err != nil {
			t.Fatalf("--target %s: %v", target, err)
		}
		tg, _ := parseTarget(target)
		exe := filepath.Join(o.out, target, programExeName("greet", tg))
		if strings.HasPrefix(target, "linux") {
			f, err := elf.Open(exe)
			if err != nil {
				t.Fatal(err)
			}
			if f.Type != elf.ET_EXEC && f.Type != elf.ET_DYN {
				t.Errorf("%s: %v", target, f.Type)
			}
			f.Close()
			if dockerImage("asuka-linux-test") {
				check("linux", exec.Command("docker", "run", "--rm", "-v", filepath.Dir(exe)+":/w", "asuka-linux-test", "/w/greet", "x", "y"), 0, `args: ["x","y"]`, "")
				check("linux exit", exec.Command("docker", "run", "--rm", "-v", filepath.Dir(exe)+":/w", "asuka-linux-test", "/w/greet", "7"), 7, "", "")
			}
		} else {
			f, err := pe.Open(exe)
			if err != nil {
				t.Fatal(err)
			}
			if oh, ok := f.OptionalHeader.(*pe.OptionalHeader64); !ok || oh.Subsystem != pe.IMAGE_SUBSYSTEM_WINDOWS_CUI {
				t.Errorf("%s: not a console program", target)
			}
			f.Close()
		}
	}
}

// dockerImage reports whether Docker runs here and has the image.
func dockerImage(name string) bool {
	if os.Getenv("ASUKA_NO_DOCKER") != "" {
		return false
	}
	out, err := exec.Command("docker", "images", "-q", name).Output()
	return err == nil && len(bytes.TrimSpace(out)) > 0
}

// A project's [bin] programs: asuka run picks the one there is, --bin
// names one; asuka test runs every target's tests; a program imports
// the project's packages (../model); an app's program keeps its data
// where the app does.
func TestBinTargets(t *testing.T) {
	if _, err := cbuild.Find(); err != nil {
		t.Skip(err)
	}
	dir := filepath.Join(t.TempDir(), "proj")
	write := func(name, src string) {
		p := filepath.Join(dir, filepath.FromSlash(name))
		os.MkdirAll(filepath.Dir(p), 0o755)
		if err := os.WriteFile(p, []byte(src), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	write("asuka.toml", "name = \"Proj\"\nid = \"com.example.proj\"\nversion = \"1.0.0\"\n\n[bin.tool]\npath = \"tool\"\n\n[bin.other]\npath = \"other\"\n")
	write("model/model.asuka", "pub fn shout(s: string) -> string = s.upper()\n")
	write("app.asuka", "import ./model\n\napp { Text(model.shout(\"hi\")) }\n\ntest \"app\" { expect(model.shout(\"a\") == \"A\") }\n")
	write("tool/main.asuka", "import ../model\nimport std/fs\nimport std/os\n\nfn main() {\n  print(model.shout(os.args().first() ?? \"none\"))\n  print(fs.appDir())\n}\n\ntest \"tool\" {\n  let o = os.fake(args: [\"x\"])\n  expect(await o.run(main) == 0)\n  expect(o.stdout().hasPrefix(\"X\\n\"))\n}\n")
	write("other/main.asuka", "fn main() { print(\"other\") }\n")
	p, err := appbuild.Load(dir)
	if err != nil {
		t.Fatal(err)
	}
	if len(p.Bins) != 2 || p.Bins[0].Name != "other" || p.Bins[1].Name != "tool" {
		t.Fatalf("bins %+v", p.Bins)
	}
	if _, err := programOf(p, ""); err == nil || !strings.Contains(err.Error(), "--bin other, --bin tool") {
		t.Fatalf("two programs, none named: %v", err)
	}
	if _, err := p.Bin("nope"); err == nil {
		t.Fatal("--bin nope")
	}
	if err := cmdTest([]string{dir}); err != nil {
		t.Fatalf("asuka test: %v", err)
	}
	tool, err := p.Bin("tool")
	if err != nil {
		t.Fatal(err)
	}
	if tool.ProgramName() != "tool" {
		t.Fatal(tool.ProgramName())
	}
	if runtime.GOOS == "windows" {
		return
	}
	// Built, it keeps its data where the app does: Application Support
	// (macOS), the XDG data directory (Linux), by the app's ID.
	o := buildOptions{out: filepath.Join(t.TempDir(), "dist")}
	if err := cmdBuildProgram(tool, &o); err != nil {
		t.Fatal(err)
	}
	home := t.TempDir()
	cmd := exec.Command(filepath.Join(o.out, "tool"), "hey")
	cmd.Env = append(os.Environ(), "HOME="+home, "XDG_DATA_HOME="+filepath.Join(home, "data"), "ASUKA_DATA=")
	b, err := cmd.Output()
	if err != nil {
		t.Fatal(err)
	}
	lines := strings.Split(strings.TrimSpace(string(b)), "\n")
	want := filepath.Join(home, "Library", "Application Support", "com.example.proj")
	if runtime.GOOS == "linux" {
		want = filepath.Join(home, "data", "com.example.proj")
	}
	if len(lines) != 2 || lines[0] != "HEY" || lines[1] != want {
		t.Fatalf("got %q, want HEY and %s", lines, want)
	}
	if _, err := os.Stat(want); err != nil {
		t.Fatalf("the data directory isn't made: %v", err)
	}
}

// A desktop app's build bundles its [bin] programs: the Mac's
// Contents/Helpers (universal, as the app is), Linux's bin, beside
// Windows' .exe.
func TestBundlePrograms(t *testing.T) {
	if _, err := cbuild.Zig(); err != nil {
		t.Skip("cross-compiling needs Zig:", err)
	}
	dir := filepath.Join(t.TempDir(), "proj")
	os.MkdirAll(filepath.Join(dir, "cli"), 0o755)
	os.WriteFile(filepath.Join(dir, "asuka.toml"), []byte("name = \"Proj\"\n\n[bin.proj-cli]\npath = \"cli\"\n"), 0o644)
	os.WriteFile(filepath.Join(dir, "app.asuka"), []byte("app { Text(\"hi\") }\n"), 0o644)
	os.WriteFile(filepath.Join(dir, "cli", "main.asuka"), []byte("fn main() { print(\"cli\") }\n"), 0o644)
	app, err := loadApp(dir, thisComputer())
	if err != nil {
		t.Fatal(err)
	}
	for _, c := range []struct {
		t     cbuild.Target
		archs []string
		file  string
	}{
		{cbuild.Target{OS: "darwin"}, appbuild.MacArchs, "proj-cli"},
		{cbuild.Target{OS: "linux", Arch: "amd64"}, nil, "proj-cli"},
		{cbuild.Target{OS: "windows", Arch: "arm64"}, nil, "proj-cli.exe"},
	} {
		if c.t.OS == "darwin" && runtime.GOOS != "darwin" {
			continue
		}
		out := filepath.Join(t.TempDir(), "bundle")
		got, err := bundlePrograms(app, c.t, c.archs, out, false)
		if err != nil {
			t.Fatalf("%s: %v", c.t.OS, err)
		}
		if len(got) != 1 || got[0] != filepath.Join(out, c.file) {
			t.Fatalf("%s: %v", c.t.OS, got)
		}
		switch c.t.OS {
		case "darwin":
			f, err := macho.OpenFat(got[0])
			if err != nil || len(f.Arches) != 2 {
				t.Fatalf("not universal: %v", err)
			}
			f.Close()
		case "linux":
			f, err := elf.Open(got[0])
			if err != nil || f.Machine != elf.EM_X86_64 {
				t.Fatalf("linux: %v", err)
			}
			f.Close()
		case "windows":
			f, err := pe.Open(got[0])
			if err != nil || f.Machine != pe.IMAGE_FILE_MACHINE_ARM64 {
				t.Fatalf("windows: %v", err)
			}
			f.Close()
		}
	}
}

// A [bin] another toolchain builds: its command runs for each target in
// the project's directory, {out}, {os} and {arch} replaced, and what it
// writes goes in the app as an Asuka program's executable does.
func TestBundleBuiltPrograms(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("the command is sh's")
	}
	dir := filepath.Join(t.TempDir(), "proj")
	os.MkdirAll(dir, 0o755)
	os.WriteFile(filepath.Join(dir, "asuka.toml"), []byte(`name = "Proj"

[bin.gate]
build = ["sh", "-c", "printf '%s %s %s' {os} {arch} \"$WHERE\" > {out}"]
env = { WHERE = "{os}/{arch}" }
`), 0o644)
	os.WriteFile(filepath.Join(dir, "app.asuka"), []byte("app { Text(\"hi\") }\n"), 0o644)
	app, err := loadApp(dir, thisComputer())
	if err != nil {
		t.Fatal(err)
	}
	if b := app.proj.Bins; len(b) != 1 || !b[0].Built() {
		t.Fatalf("bins: %+v", b)
	}
	if _, err := app.proj.Bin("gate"); err == nil || !strings.Contains(err.Error(), "its own command") {
		t.Fatalf("Bin(gate): %v", err)
	}
	for _, c := range []struct {
		t    cbuild.Target
		file string
		want string
	}{
		{cbuild.Target{OS: "linux", Arch: "amd64"}, "gate", "linux amd64 linux/amd64"},
		{cbuild.Target{OS: "windows", Arch: "arm64"}, "gate.exe", "windows arm64 windows/arm64"},
	} {
		out := filepath.Join(t.TempDir(), "bundle")
		got, err := bundlePrograms(app, c.t, nil, out, false)
		if err != nil {
			t.Fatalf("%s: %v", c.t.OS, err)
		}
		if len(got) != 1 || got[0] != filepath.Join(out, c.file) {
			t.Fatalf("%s: %v", c.t.OS, got)
		}
		if b, _ := os.ReadFile(got[0]); string(b) != c.want {
			t.Fatalf("%s: %q, want %q", c.t.OS, b, c.want)
		}
	}
}

// A [bin] is an Asuka program's directory or a command, not both.
func TestBinPathOrBuild(t *testing.T) {
	for _, c := range []struct{ toml, err string }{
		{"[bin.x]\npath = \"cli\"\nbuild = [\"true\"]\n", "not both"},
		{"[bin.x]\npath = \"cli\"\nenv = { A = \"b\" }\n", "env is for build"},
		{"[bin.x]\n", "or build"},
	} {
		dir := t.TempDir()
		os.WriteFile(filepath.Join(dir, "asuka.toml"), []byte(c.toml), 0o644)
		os.WriteFile(filepath.Join(dir, "app.asuka"), []byte("app { Text(\"hi\") }\n"), 0o644)
		if _, err := appbuild.Load(dir); err == nil || !strings.Contains(err.Error(), c.err) {
			t.Fatalf("%q: %v, want %q", c.toml, err, c.err)
		}
	}
}
