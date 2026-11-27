package compile

import (
	"slices"
	"testing"

	"github.com/egoist/asuka/lang/bytecode"
)

// A default (a field's, a parameter's) a call leaves out is evaluated in
// the caller, at the call's line: stepping over the call doesn't stop at
// the default's declaration, and a default from another file doesn't
// put its line numbers in the caller's file (where a breakpoint on that
// line would stop).
func TestDefaultLines(t *testing.T) {
	files := map[string][]byte{
		"main.asuka": []byte(`struct Stats {
  total: int
  tags: [string] = ["a", "{1 + 1}"]
}

fn run() -> int {
  let s = Stats(total: 1)
  let n = scaled(2)
  return s.total + n + s.tags.count()
}

fn main() {
  print(run())
}
`),
		// scaled's default is on line 11: run has no code on its line 11.
		"util.asuka": []byte(`fn factor() -> int = 3







// The default is on line 11.
fn scaled(n: int,
  by: int = factor() * 2) -> int = n * by
`),
	}
	res := Files(files, &Options{NoOptimize: true, Debug: true, NoCatalogs: true})
	if err := res.Err(); err != nil {
		t.Fatal(err)
	}
	var run *bytecode.FuncDef
	for i := range res.Module.Funcs {
		if res.Module.Funcs[i].Name == "run" {
			run = &res.Module.Funcs[i]
		}
	}
	if run == nil {
		t.Fatal("no function run")
	}
	var lines []uint32
	for _, l := range run.Lines {
		if l.Line != 0 && !slices.Contains(lines, l.Line) {
			lines = append(lines, l.Line)
		}
	}
	slices.Sort(lines)
	if want := []uint32{7, 8, 9}; !slices.Equal(lines, want) {
		t.Fatalf("run's lines are %v, want %v", lines, want)
	}
}
