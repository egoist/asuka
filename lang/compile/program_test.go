package compile

import (
	"os"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"testing"
)

// Programs (docs/programs.md): each testdata/programs/*.asuka is a
// program, its fn main run on the VM and compiled to C, with the
// arguments, stdin and environment its first lines say (// args:,
// // stdin: with \n and \r escapes, // env: KEY=value); it must exit with
// // exit:'s code and write what its // Stdout: and // Stderr: blocks
// say (a line … stands for the rest: an error's trace, which the two
// backends tell apart). A first line // unix: it runs Unix's programs.
func TestPrograms(t *testing.T) {
	files, _ := filepath.Glob("testdata/programs/*.asuka")
	if len(files) == 0 {
		t.Fatal("no programs")
	}
	runner := testRunner(t)
	for _, path := range files {
		path := path
		t.Run(strings.TrimSuffix(filepath.Base(path), ".asuka"), func(t *testing.T) {
			t.Parallel()
			b, err := os.ReadFile(path)
			if err != nil {
				t.Fatal(err)
			}
			src := string(b)
			if strings.HasPrefix(src, "// unix\n") && runtime.GOOS == "windows" {
				t.Skip("runs Unix's programs")
			}
			var args, env []string
			var sig os.Signal
			stdin, code := "", 0
			for _, l := range strings.Split(src, "\n") {
				switch {
				case strings.HasPrefix(l, "// args: "):
					args = strings.Fields(strings.TrimPrefix(l, "// args: "))
				case strings.HasPrefix(l, "// stdin: "):
					stdin = strings.NewReplacer(`\n`, "\n", `\r`, "\r").Replace(strings.TrimPrefix(l, "// stdin: "))
				case strings.HasPrefix(l, "// env: "):
					env = append(env, strings.TrimPrefix(l, "// env: "))
				case l == "// signal: interrupt":
					if runtime.GOOS == "windows" {
						t.Skip("sends a POSIX signal")
					}
					sig = os.Interrupt
				case strings.HasPrefix(l, "// exit: "):
					code, _ = strconv.Atoi(strings.TrimPrefix(l, "// exit: "))
				}
			}
			i := strings.Index(src, "\n// Stdout:\n")
			j := strings.Index(src, "\n// Stderr:\n")
			if i < 0 || j < i {
				t.Fatal("no // Stdout: and // Stderr: blocks")
			}
			block := func(s string) string {
				var out []string
				for _, l := range strings.Split(strings.TrimRight(s, "\n"), "\n") {
					out = append(out, strings.TrimPrefix(strings.TrimPrefix(l, "//"), " "))
				}
				return strings.Join(out, "\n")
			}
			wantOut := block(src[i+len("\n// Stdout:\n") : j+1])
			wantErr := block(src[j+len("\n// Stderr:\n"):])
			res := Files(map[string][]byte{path: b}, nil)
			if err := res.Err(); err != nil {
				t.Fatalf("compile:\n%v", err)
			}
			if res.Module.Main < 0 {
				t.Fatal("no Main")
			}
			check := func(backend string, r MainRun, err error) {
				t.Helper()
				if err != nil {
					t.Fatalf("%s: %v\n%s%s", backend, err, r.Stdout, r.Stderr)
				}
				if r.Code != code {
					t.Errorf("%s: exit %d, want %d\nstdout:\n%s\nstderr:\n%s", backend, r.Code, code, r.Stdout, r.Stderr)
				}
				if got := strings.TrimRight(r.Stdout, "\n"); got != strings.TrimRight(wantOut, "\n") {
					t.Errorf("%s: stdout:\n%s\nwant:\n%s", backend, got, wantOut)
				}
				got := strings.TrimRight(r.Stderr, "\n")
				want := strings.TrimRight(wantErr, "\n")
				if pre, ok := strings.CutSuffix(want, "\n…"); ok {
					if !strings.HasPrefix(got, pre) {
						t.Errorf("%s: stderr:\n%s\nwant it to start:\n%s", backend, got, pre)
					}
				} else if got != want {
					t.Errorf("%s: stderr:\n%s\nwant:\n%s", backend, got, want)
				}
			}
			o := MainOptions{Args: args, Stdin: stdin, Env: env, Signal: sig}
			r, err := RunMain(runner, res.Module, o)
			check("VM", r, err)
			r, err = RunMainC(res.Module, true, o)
			check("C", r, err)
		})
	}
}
