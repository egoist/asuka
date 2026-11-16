package compile

import (
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"testing"

	"github.com/egoist/asuka/internal/cbuild"
	"github.com/egoist/asuka/lang/cgen"
)

// BenchmarkAsync times testdata/bench/async.asuka's async workloads, the
// program run on the VM and compiled to C as a release build compiles it
// (no sanitizers), each run a process: "startup" is a run that does no
// work. Async calls that don't wait (calls), work in an async function
// (compute), a stream read with for await (stream), and tasks (tasks).
func BenchmarkAsync(b *testing.B) {
	if _, err := cbuild.Find(); err != nil {
		b.Skip(err)
	}
	src, err := os.ReadFile("testdata/bench/async.asuka")
	if err != nil {
		b.Fatal(err)
	}
	res := Files(map[string][]byte{"async.asuka": src}, nil)
	if err := res.Err(); err != nil {
		b.Fatal(err)
	}
	dir := b.TempDir()
	akb := filepath.Join(dir, "module.akb")
	if err := os.WriteFile(akb, res.Module.Encode(), 0o644); err != nil {
		b.Fatal(err)
	}
	vm, err := Runner(false)
	if err != nil {
		b.Fatal(err)
	}
	csrc, err := cgen.Generate(res.Module)
	if err != nil {
		b.Fatal(err)
	}
	app := filepath.Join(dir, "app.c")
	if err := os.WriteFile(app, csrc, 0o644); err != nil {
		b.Fatal(err)
	}
	core, err := FindCore()
	if err != nil {
		b.Fatal(err)
	}
	c, err := cbuild.RunnerFor(core, cbuild.RunnerOptions{Extra: []string{app}, Dir: dir})
	if err != nil {
		b.Fatal(err)
	}
	backends := []struct {
		name string
		args []string
	}{
		{"vm", []string{vm, "-main", "-program", "bench", akb, "--"}},
		{"c", []string{c, "-main", "-program", "bench", "-aot", "--"}},
	}
	for _, w := range []struct {
		name string
		n    int
	}{
		{"startup", 0},
		{"calls", 2_000_000},
		{"compute", 5_000_000},
		{"stream", 200_000},
		{"tasks", 200_000},
	} {
		which := w.name
		if which == "startup" {
			which = "calls"
		}
		for _, be := range backends {
			b.Run(w.name+"/"+be.name, func(b *testing.B) {
				args := append(append([]string{}, be.args[1:]...), which, strconv.Itoa(w.n))
				for b.Loop() {
					if out, err := exec.Command(be.args[0], args...).CombinedOutput(); err != nil {
						b.Fatalf("%v\n%s", err, out)
					}
				}
			})
		}
	}
}
