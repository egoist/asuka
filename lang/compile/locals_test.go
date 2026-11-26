package compile

import (
	"testing"

	"github.com/egoist/asuka/lang/bytecode"
)

// Code the optimizer leaves alone (asuka dev's) names its parameters
// and locals for the debugger: each a register over the pcs its name is
// in scope at, read as it's held (a var a closure captures is a box).
// Optimized code has none.
func TestLocals(t *testing.T) {
	src := `struct Point {
  x: int
  y: int
}

fn add(a: int, b: int) -> int {
  let sum = a + b
  return sum
}

fn run() -> int {
  var total = 0
  let p = Point(x: 1, y: 2)
  for i in 0..<3 {
    total = add(total, i)
  }
  let name = "Asuka"
  let f = () => total
  return total + p.x + f()
}

fn main() {
  print(run())
}
`
	compile := func(noOpt bool) *bytecode.Module {
		res := Files(map[string][]byte{"main.asuka": []byte(src)}, &Options{NoOptimize: noOpt, Debug: true, NoCatalogs: true})
		if err := res.Err(); err != nil {
			t.Fatal(err)
		}
		return res.Module
	}
	m := compile(true)
	fn := func(name string) *bytecode.FuncDef {
		for i := range m.Funcs {
			if m.Funcs[i].Name == name {
				return &m.Funcs[i]
			}
		}
		t.Fatalf("no function %s", name)
		return nil
	}
	local := func(f *bytecode.FuncDef, name string) bytecode.Local {
		t.Helper()
		var found []bytecode.Local
		for _, l := range f.Locals {
			if l.Name == name {
				found = append(found, l)
			}
		}
		if len(found) != 1 {
			t.Fatalf("%s: %d locals named %s: %+v", f.Name, len(found), name, f.Locals)
		}
		return found[0]
	}
	// line is the line the code at pc is on.
	line := func(f *bytecode.FuncDef, pc uint32) uint32 {
		var l uint32
		for _, e := range f.Lines {
			if e.PC <= pc {
				l = e.Line
			}
		}
		return l
	}

	add := fn("add")
	if len(add.Locals) != 3 {
		t.Fatalf("add's locals: %+v", add.Locals)
	}
	for i, name := range []string{"a", "b"} {
		l := local(add, name)
		if l.Reg != uint16(i) || l.From != 0 || l.To != uint32(len(add.Code)) || l.Desc != "i" || l.Via != bytecode.LocalValue {
			t.Errorf("parameter %s: %+v", name, l)
		}
	}
	// sum: from after the line that sets it, to the function's end.
	if sum := local(add, "sum"); line(add, sum.From) != 8 || sum.Desc != "i" || add.Kinds[sum.Reg] != bytecode.KInt {
		t.Errorf("sum: %+v (from line %d)", sum, line(add, sum.From))
	}

	run := fn("run")
	if total := local(run, "total"); total.Via != bytecode.LocalBox || total.Desc != "i" || run.Kinds[total.Reg] != bytecode.KRef {
		t.Errorf("total (a closure captures it: boxed): %+v", total)
	}
	if p := local(run, "p"); p.Desc != "SPoint;" || p.Via != bytecode.LocalValue {
		t.Errorf("p: %+v", p)
	}
	// name: from after the instruction that sets it, on line 17.
	if name := local(run, "name"); name.Desc != "s" || line(run, name.From) != 17 || run.Code[name.From-3] != uint16(bytecode.Byname["MOVR"]) {
		t.Errorf("name: %+v", name)
	}
	if f := local(run, "f"); f.Desc != "-" {
		t.Errorf("f (a closure: no descriptor): %+v", f)
	}
	// i: in the loop's body only.
	i := local(run, "i")
	if line(run, i.From) != 15 || line(run, i.To-1) != 15 {
		t.Errorf("i: %+v, lines %d to %d", i, line(run, i.From), line(run, i.To-1))
	}
	for _, l := range run.Locals {
		if l.From >= l.To || l.To > uint32(len(run.Code)) || int(l.Reg) >= len(run.Kinds) {
			t.Errorf("a bad range or register: %+v", l)
		}
	}
	// The closure's capture is the same box, by the same name.
	for i := range m.Funcs {
		if f := &m.Funcs[i]; len(f.CapKinds) == 1 && f.Name != "run" && len(f.Name) > 4 && f.Name[:4] == "run/" {
			if total := local(f, "total"); total.Via != bytecode.LocalBox {
				t.Errorf("the closure's total: %+v", total)
			}
		}
	}

	// It's in the module, as the VM reads it.
	back, err := bytecode.Decode(m.Encode())
	if err != nil {
		t.Fatal(err)
	}
	for i := range m.Funcs {
		if len(back.Funcs[i].Locals) != len(m.Funcs[i].Locals) {
			t.Fatalf("%s: %d locals decoded, %d encoded", m.Funcs[i].Name, len(back.Funcs[i].Locals), len(m.Funcs[i].Locals))
		}
	}

	for _, f := range compile(false).Funcs {
		if len(f.Locals) > 0 {
			t.Errorf("optimized %s has locals: %+v", f.Name, f.Locals)
		}
	}
}
