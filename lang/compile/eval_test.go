package compile

import (
	"strings"
	"testing"

	"github.com/egoist/asuka/lang/bytecode"
)

const evalSrc = `import std/semver

struct Point {
  x: int
  y: int

  fn sum() -> int {
    return x + y
  }
}

enum Light {
  red
  green
}

fn add(a: int, b: int) -> int {
  let sum = a + b
  return sum
}

fn label(p: Point, names: [string], light: Light) -> string {
  return "{p.x}"
}

component Main {
  state count = 0
  let n = add(1, 2)
  let l = label(Point(x: 1, y: 2), ["a"], Light.green)
  let s = Point(x: 1, y: 2).sum()
  Column {
    Text("{n} {l} {s}")
    Button("Add") {
      on press {
        let next = count + 1
        count = next
      }
    }
  }
}

app { Main() }
`

// funcAt is the module's function named name, and the pc of its line's
// first instruction.
func funcAt(t *testing.T, m *bytecode.Module, name string, line uint32) (uint32, uint32) {
	t.Helper()
	for i, f := range m.Funcs {
		if f.Name != name {
			continue
		}
		for _, l := range f.Lines {
			if l.Line == line {
				return uint32(i), l.PC
			}
		}
	}
	t.Fatalf("no %s at line %d", name, line)
	return 0, 0
}

// An expression compiled to evaluate in a frame: a function of the
// module's extension, taking the frame's locals it sees; one that
// doesn't check says why; the module itself is as it was.
func TestEvaluator(t *testing.T) {
	res := Files(map[string][]byte{"app.asuka": []byte(evalSrc)}, &Options{NoOptimize: true, Debug: true, NoCatalogs: true, NoTests: true})
	if err := res.Err(); err != nil {
		t.Fatal(err)
	}
	m := res.Module
	before := m.Encode()
	e, ok := m.Evaluator.(*Evaluator)
	if !ok {
		t.Fatal("no evaluator")
	}
	fn, pc := funcAt(t, m, "add", 18)
	fr, err := e.Compile(fn, pc, "a * 10 + b", EvalValue)
	if err != nil {
		t.Fatal(err)
	}
	if fr.Base != bytecode.Hash(before) || fr.Desc != "i" || len(fr.Params) != 2 {
		t.Fatalf("fragment: %+v", fr)
	}
	x, err := bytecode.DecodeExtension(fr.Ext)
	if err != nil {
		t.Fatal(err)
	}
	if int(x.NFuncs) != len(m.Funcs) || fr.Func < x.NFuncs || int(fr.Func-x.NFuncs) >= len(x.Funcs) {
		t.Fatalf("extension: %d funcs after %d, func %d", len(x.Funcs), x.NFuncs, fr.Func)
	}
	if got := x.Funcs[fr.Func-x.NFuncs]; got.NParams != 2 || got.Ret != bytecode.KInt {
		t.Fatalf("eval func: %+v", got)
	}
	// The same again: the same function.
	if fr2, err := e.Compile(fn, pc, "a * 10 + b", EvalValue); err != nil || fr2.Func != fr.Func {
		t.Fatalf("again: %v %v", fr2, err)
	}
	// What doesn't check.
	if _, err := e.Compile(fn, pc, "a + nope", EvalValue); err == nil || !strings.Contains(err.Error(), "nope") {
		t.Fatalf("an unknown name: %v", err)
	}
	if _, err := e.Compile(fn, pc, "a", EvalCond); err == nil {
		t.Fatal("a condition that isn't a bool")
	}
	// A condition, a log message, a method's self by its fields' names,
	// what the file imports.
	if fr, err := e.Compile(fn, pc, "a > b", EvalCond); err != nil || fr.Desc != "b" {
		t.Fatalf("condition: %v %v", fr, err)
	}
	if fr, err := e.Compile(fn, pc, `a is {a}, "b" {b * 2}`, EvalLog); err != nil || fr.Desc != "s" {
		t.Fatalf("log: %v %v", fr, err)
	}
	sfn, spc := funcAt(t, m, "Point.sum", 8)
	if _, err := e.Compile(sfn, spc, "x * y + self.y + sum()", EvalValue); err != nil {
		t.Fatalf("in a method: %v", err)
	}
	lfn, lpc := funcAt(t, m, "label", 23)
	if fr, err := e.Compile(lfn, lpc, `names.map(n => n + "!").count() + (if semver.satisfies("1.2.0", "^1") { p.x } else { 0 })`, EvalValue); err != nil {
		t.Fatalf("a closure, an import: %v", err)
	} else if len(fr.Params) != 3 {
		t.Fatalf("params: %v", fr.Params)
	}
	if fr, err := e.Compile(lfn, lpc, `if light == .green { [p] } else { [] }`, EvalValue); err != nil || !strings.HasPrefix(fr.Desc, "[") {
		t.Fatalf("an array: %v %v", fr, err)
	}
	// In a component's handler: its members by their names, through its
	// env (the frame's self).
	line := uint32(strings.Count(evalSrc[:strings.Index(evalSrc, "let next")], "\n") + 1)
	hfn, hpc := funcAt(t, m, "Main/on press", line)
	if fr, err := e.Compile(hfn, hpc, "count * 10 + n + add(count, 1)", EvalValue); err != nil || fr.Desc != "i" || len(fr.Params) != 1 {
		t.Fatalf("in a handler: %+v %v", fr, err)
	}
	if got := m.Encode(); string(got) != string(before) {
		t.Fatal("the module changed")
	}
}
