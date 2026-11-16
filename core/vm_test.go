package core_test

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"

	"github.com/egoist/asuka/internal/cbuild"
	bc "github.com/egoist/asuka/lang/bytecode"
	"github.com/egoist/asuka/lang/cgen"
	"github.com/egoist/asuka/lang/prelude"
)

// These tests run hand-assembled modules on the VM (asuka-run, built
// with sanitizers) and compare what they print. The compiler's own tests
// (lang/...) cover the language; these cover the VM's instructions.

var runnerPath string

func runner(t *testing.T) string {
	t.Helper()
	if runnerPath != "" {
		return runnerPath
	}
	if _, err := cbuild.Find(); err != nil {
		t.Skip(err)
	}
	root, _ := filepath.Abs(".")
	exe, err := cbuild.Runner(root, true)
	if err != nil {
		t.Fatal(err)
	}
	runnerPath = exe
	return exe
}

// run runs m on the VM, then compiled to C (lang/cgen), and fails the
// test when the two don't print the same: both backends mean the same.
func run(t *testing.T, m *bc.Module, args ...string) (string, error) {
	t.Helper()
	dir := t.TempDir()
	p := filepath.Join(dir, "m.akb")
	if err := os.WriteFile(p, m.Encode(), 0o644); err != nil {
		t.Fatal(err)
	}
	out, err := exec.Command(runner(t), append(args, p)...).CombinedOutput()
	csrc, gerr := cgen.Generate(m)
	if gerr != nil {
		t.Fatal(gerr)
	}
	runC(t, "the compiled C", csrc, string(out), err, args...)
	return string(out), err
}

// runMixed runs m as run does, then twice more compiled to C with every
// other function left to the VM (the odd ones, then the even ones):
// compiled and interpreted frames calling, returning to, suspending under
// and throwing through each other print what the VM does.
func runMixed(t *testing.T, m *bc.Module, args ...string) (string, error) {
	t.Helper()
	out, err := run(t, m, args...)
	for _, odd := range []int{1, 0} {
		csrc, gerr := cgen.GenerateMixed(m, func(i int) bool { return i%2 == odd })
		if gerr != nil {
			t.Fatal(gerr)
		}
		runC(t, fmt.Sprintf("the C, with the functions i%%2 == %d interpreted,", odd), csrc, out, err, args...)
	}
	return out, err
}

// runC builds csrc (a module compiled to C) into a runner, runs it with
// args and fails the test unless it prints out and fails when the VM did
// (err).
func runC(t *testing.T, what string, csrc []byte, out string, err error, args ...string) {
	t.Helper()
	dir := t.TempDir()
	app := filepath.Join(dir, "app.c")
	if err := os.WriteFile(app, csrc, 0o644); err != nil {
		t.Fatal(err)
	}
	root, _ := filepath.Abs(".")
	exe, berr := cbuild.RunnerWith(root, []string{app}, true, dir)
	if berr != nil {
		t.Fatalf("%s doesn't build: %v", what, berr)
	}
	aout, aerr := exec.Command(exe, append(args, "-aot")...).CombinedOutput()
	if string(aout) != out || (aerr == nil) != (err == nil) {
		t.Fatalf("the VM and %s disagree:\nVM (%v):\n%s\nC (%v):\n%s", what, err, out, aerr, aout)
	}
}

// fn helps assemble a function: registers by kind, ops by name.
type fn struct {
	m   *bc.Module
	def bc.FuncDef
	a   bc.Asm
}

func newFn(m *bc.Module, name string, params ...bc.Kind) *fn {
	f := &fn{m: m, def: bc.FuncDef{Name: name, File: "test.asuka", NParams: uint16(len(params))}}
	f.def.Kinds = append(f.def.Kinds, params...)
	return f
}

func (f *fn) reg(k bc.Kind) int {
	f.def.Kinds = append(f.def.Kinds, k)
	return len(f.def.Kinds) - 1
}

func (f *fn) op(name string, args ...int) int {
	op, ok := bc.Byname[name]
	if !ok {
		panic("no op " + name)
	}
	return f.a.Emit(op, args...)
}

func (f *fn) str(s string) int  { return int(f.m.String(s)) }
func (f *fn) b(name string) int { return int(f.m.Builtin(name)) }
func (f *fn) pc() int           { return len(f.a.Code) }

// print emits a print of register r of kind k.
func (f *fn) print(r int, k bc.Kind) {
	s := r
	if k != bc.KRef {
		s = f.reg(bc.KRef)
		f.op("TOSTR", s, r, int(k))
	}
	f.op("CALLB", bc.None, f.b("print"), s, 1)
}

func (f *fn) done(ret bc.Kind, flags uint8) int {
	f.def.Code = f.a.Code
	f.def.Ret = ret
	f.def.Flags = flags
	f.m.Funcs = append(f.m.Funcs, f.def)
	return len(f.m.Funcs) - 1
}

func module() *bc.Module { return &bc.Module{App: -1, Main: -1} }

func addInit(m *bc.Module) {
	f := newFn(m, "init")
	f.op("RET0")
	m.Init = uint32(f.done(bc.KVoid, 0))
}

func test(m *bc.Module, name string, i int) {
	m.Tests = append(m.Tests, bc.Test{Name: name, Func: uint32(i)})
}

func expect(t *testing.T, got string, want ...string) {
	t.Helper()
	g := strings.Split(strings.TrimSpace(got), "\n")
	if strings.Join(g, "\n") != strings.Join(want, "\n") {
		t.Fatalf("got:\n%s\nwant:\n%s", strings.TrimSpace(got), strings.Join(want, "\n"))
	}
}

func TestVMArith(t *testing.T) {
	m := module()
	addInit(m)
	f := newFn(m, "t")
	a, b, c := f.reg(bc.KInt), f.reg(bc.KInt), f.reg(bc.KInt)
	f.op("KSMALL", a, 40)
	f.op("KSMALL", b, 2)
	f.op("ADDI", c, a, b)
	f.print(c, bc.KInt)
	x, y := f.reg(bc.KFloat), f.reg(bc.KFloat)
	f.op("I2F", x, c)
	m.Floats = append(m.Floats, 0.5)
	f.op("KFLOAT", y, 0)
	f.op("MULF", x, x, y)
	f.print(x, bc.KFloat)
	// Interpolation: "n={c} half={x} ok={true}"
	s1, n1, s2, n2, s3, n3 := f.reg(bc.KRef), f.reg(bc.KInt), f.reg(bc.KRef), f.reg(bc.KFloat), f.reg(bc.KRef), f.reg(bc.KBool)
	f.op("KSTR", s1, f.str("n="))
	f.op("MOV", n1, c)
	f.op("KSTR", s2, f.str(" half="))
	f.op("MOV", n2, x)
	f.op("KSTR", s3, f.str(" ok="))
	f.op("KSMALL", n3, 1)
	out := f.reg(bc.KRef)
	f.op("INTERP", out, s1, 6, f.str(string([]byte{4, 1, 4, 2, 4, 3})))
	f.print(out, bc.KRef)
	// Division by zero throws.
	z := f.reg(bc.KInt)
	f.op("KSMALL", z, 0)
	f.op("DIVI", c, a, z)
	f.op("RET0")
	test(m, "arith", f.done(bc.KVoid, bc.FThrows))
	got, err := run(t, m, "-test")
	if err == nil {
		t.Fatal("want a failure")
	}
	expect(t, got, "42", "21", "n=42 half=21 ok=true", "FAIL arith: division by zero")
}

func TestVMCallsAndLoops(t *testing.T) {
	m := module()
	addInit(m)
	// fib(n) = n < 2 ? n : fib(n-1) + fib(n-2)
	fib := newFn(m, "fib", bc.KInt)
	two, c, r1, r2, n1 := fib.reg(bc.KInt), fib.reg(bc.KBool), fib.reg(bc.KInt), fib.reg(bc.KInt), fib.reg(bc.KInt)
	fib.op("KSMALL", two, 2)
	fib.op("LTI", c, 0, two)
	j := fib.op("JF", c, 0)
	fib.op("RET", 0)
	fib.a.PatchJump(j, fib.pc())
	fib.op("ADDIK", n1, 0, -1)
	fibIdx := len(m.Funcs) // this function's index once done
	fib.op("CALL", r1, fibIdx, n1, 1)
	fib.op("ADDIK", n1, 0, -2)
	fib.op("CALL", r2, fibIdx, n1, 1)
	fib.op("ADDI", r1, r1, r2)
	fib.op("RET", r1)
	fib.done(bc.KInt, 0)

	f := newFn(m, "t")
	n, res := f.reg(bc.KInt), f.reg(bc.KInt)
	f.op("KSMALL", n, 20)
	f.op("CALL", res, fibIdx, n, 1)
	f.print(res, bc.KInt)
	// A loop summing 1..100.
	i, sum, lim, cond := f.reg(bc.KInt), f.reg(bc.KInt), f.reg(bc.KInt), f.reg(bc.KBool)
	f.op("KSMALL", i, 1)
	f.op("KSMALL", sum, 0)
	f.op("KSMALL", lim, 100)
	top := f.pc()
	f.op("LEI", cond, i, lim)
	exit := f.op("JF", cond, 0)
	f.op("ADDI", sum, sum, i)
	f.op("ADDIK", i, i, 1)
	back := f.op("JMP", 0)
	f.a.PatchJump(back, top)
	f.a.PatchJump(exit, f.pc())
	f.print(sum, bc.KInt)
	f.op("RET0")
	test(m, "calls", f.done(bc.KVoid, 0))
	got, err := run(t, m, "-test")
	if err != nil {
		t.Fatalf("%v\n%s", err, got)
	}
	expect(t, got, "6765", "5050", "ok calls")
}

func TestVMClosuresStructsArrays(t *testing.T) {
	m := module()
	addInit(m)
	m.Shapes = []bc.ShapeDef{{Name: "Todo", Fields: []bc.Field{{Name: "title", Kind: bc.KRef, Shape: -1}, {Name: "done", Kind: bc.KBool, Shape: -1}}}}
	// isDone(todo) = todo.done
	isDone := newFn(m, "isDone", bc.KRef)
	d := isDone.reg(bc.KBool)
	isDone.op("GETF", d, 0, 1)
	isDone.op("RET", d)
	isDoneIdx := isDone.done(bc.KBool, 0)
	// title(todo) = todo.title + suffix (a capture)
	title := newFn(m, "title", bc.KRef)
	title.def.CapKinds = []bc.Kind{bc.KRef}
	tr, suf := title.reg(bc.KRef), title.reg(bc.KRef)
	title.op("GETF", tr, 0, 0)
	title.op("CAP", suf, 0)
	title.op("CONCAT", tr, tr, suf)
	title.op("RET", tr)
	titleIdx := title.done(bc.KRef, 0)

	f := newFn(m, "t")
	// Three todos.
	fields := []int{f.reg(bc.KRef), f.reg(bc.KBool)}
	todos := []int{f.reg(bc.KRef), f.reg(bc.KRef), f.reg(bc.KRef)}
	for i, name := range []string{"milk", "eggs", "bread"} {
		f.op("KSTR", fields[0], f.str(name))
		f.op("KSMALL", fields[1], map[bool]int{true: 1, false: 0}[i == 1])
		f.op("NEWS", todos[i], 0, fields[0])
	}
	arr := f.reg(bc.KRef)
	f.op("NEWA", arr, int(bc.KRef), 0, todos[0], 3)
	// A copy, then a change to the copy: the original keeps its value.
	cp := f.reg(bc.KRef)
	f.op("MOVR", cp, arr)
	one, flag, el := f.reg(bc.KInt), f.reg(bc.KBool), f.reg(bc.KRef)
	f.op("KSMALL", one, 0)
	f.op("IDX", el, cp, one)
	f.op("KSMALL", flag, 1)
	f.op("SETF", el, 1, flag)
	f.op("SETIDX", cp, one, el)
	// filter(isDone) on both.
	pred, done1, done2, n := f.reg(bc.KRef), f.reg(bc.KRef), f.reg(bc.KRef), f.reg(bc.KInt)
	f.op("CLOS", pred, isDoneIdx, 0, 0)
	args := []int{f.reg(bc.KRef), f.reg(bc.KRef)}
	f.op("MOVR", args[0], arr)
	f.op("MOVR", args[1], pred)
	f.op("CALLB", done1, f.b("arr.filter"), args[0], 2)
	f.op("LEN", n, done1)
	f.print(n, bc.KInt)
	f.op("MOVR", args[0], cp)
	f.op("CALLB", done2, f.b("arr.filter"), args[0], 2)
	f.op("LEN", n, done2)
	f.print(n, bc.KInt)
	// map(title + "!") with a captured suffix.
	suf2, mapper, kind, mapped := f.reg(bc.KRef), f.reg(bc.KRef), f.reg(bc.KInt), f.reg(bc.KRef)
	f.op("KSTR", suf2, f.str("!"))
	f.op("CLOS", mapper, titleIdx, suf2, 1)
	margs := []int{f.reg(bc.KRef), f.reg(bc.KRef), f.reg(bc.KInt)}
	f.op("MOVR", margs[0], arr)
	f.op("MOVR", margs[1], mapper)
	f.op("KSMALL", margs[2], int(bc.KRef))
	_ = kind
	f.op("CALLB", mapped, f.b("arr.map"), margs[0], 3)
	sep, joined := f.reg(bc.KRef), f.reg(bc.KRef)
	f.op("KSTR", sep, f.str(", "))
	jargs := []int{f.reg(bc.KRef), f.reg(bc.KRef)}
	f.op("MOVR", jargs[0], mapped)
	f.op("MOVR", jargs[1], sep)
	f.op("CALLB", joined, f.b("str.join"), jargs[0], 2)
	f.print(joined, bc.KRef)
	// push in place.
	f.op("CALLB", bc.None, f.b("arr.push"), arr, 2) // arr.push(arr, pred?) no: push todos[0]
	f.op("RET0")
	// Fix the last push: arguments must be consecutive (arr, value).
	f.a.Code = f.a.Code[:len(f.a.Code)-(1+4)-1]
	pa := []int{f.reg(bc.KRef), f.reg(bc.KRef)}
	f.op("MOVR", pa[0], arr)
	f.op("MOVR", pa[1], todos[2])
	f.op("CALLB", bc.None, f.b("arr.push"), pa[0], 2)
	f.op("LEN", n, pa[0])
	f.print(n, bc.KInt)
	f.op("LEN", n, arr)
	f.print(n, bc.KInt)
	f.op("RET0")
	test(m, "values", f.done(bc.KVoid, 0))
	got, err := run(t, m, "-test")
	if err != nil {
		t.Fatalf("%v\n%s", err, got)
	}
	expect(t, got, "1", "2", "milk!, eggs!, bread!", "4", "3", "ok values")
}

func TestVMErrorsAndHandlers(t *testing.T) {
	m := module()
	addInit(m)
	// boom() throws error("boom")
	boom := newFn(m, "boom")
	msg, e := boom.reg(bc.KRef), boom.reg(bc.KRef)
	boom.op("KSTR", msg, boom.str("boom"))
	boom.op("CALLB", e, boom.b("error"), msg, 1)
	boom.op("THROW", e)
	boomIdx := boom.done(bc.KVoid, bc.FThrows)

	f := newFn(m, "t")
	start := f.pc()
	f.op("CALL", bc.None, boomIdx, 0, 0)
	end := f.pc()
	ok := f.reg(bc.KRef)
	f.op("KSTR", ok, f.str("not reached"))
	f.print(ok, bc.KRef)
	f.op("RET0")
	handler := f.pc()
	err := f.reg(bc.KRef)
	f.op("CATCH", err)
	m2, s := f.reg(bc.KRef), f.reg(bc.KRef)
	f.op("CALLB", m2, f.b("error.message"), err, 1)
	f.op("KSTR", s, f.str("caught: "))
	f.op("CONCAT", s, s, m2)
	f.print(s, bc.KRef)
	f.op("RET0")
	f.def.Handlers = []bc.Handler{{Start: uint32(start), End: uint32(end), Target: uint32(handler)}}
	test(m, "catch", f.done(bc.KVoid, 0))

	// Out-of-range index throws past a function with no handler.
	g := newFn(m, "u")
	arr, i, el := g.reg(bc.KRef), g.reg(bc.KInt), g.reg(bc.KInt)
	g.op("NEWA", arr, int(bc.KInt), bc.None, 0, 0)
	g.op("KSMALL", i, 3)
	g.op("IDX", el, arr, i)
	g.op("RET0")
	test(m, "range", g.done(bc.KVoid, bc.FThrows))
	got, runErr := run(t, m, "-test")
	if runErr == nil {
		t.Fatal("want a failure")
	}
	expect(t, got, "caught: boom", "ok catch", "FAIL range: index 3 out of range (length 0)")
}

func TestVMStateAndEffects(t *testing.T) {
	m := module()
	addInit(m)
	// effect body: print("count=" + cell)
	body := newFn(m, "body")
	body.def.CapKinds = []bc.Kind{bc.KRef}
	c, v, s, out := body.reg(bc.KRef), body.reg(bc.KInt), body.reg(bc.KRef), body.reg(bc.KRef)
	body.op("CAP", c, 0)
	body.op("CGET", v, c)
	body.op("KSTR", s, body.str("count="))
	body.op("MOV", out, s) // placeholder, replaced below
	body.a.Code = body.a.Code[:len(body.a.Code)-3]
	parts := []int{body.reg(bc.KRef), body.reg(bc.KInt)}
	body.op("MOVR", parts[0], s)
	body.op("MOV", parts[1], v)
	body.op("INTERP", out, parts[0], 2, body.str(string([]byte{4, 1})))
	body.print(out, bc.KRef)
	body.op("RET0")
	bodyIdx := body.done(bc.KVoid, 0)
	// memo: cell * 2
	dbl := newFn(m, "double")
	dbl.def.CapKinds = []bc.Kind{bc.KRef}
	dc, dv := dbl.reg(bc.KRef), dbl.reg(bc.KInt)
	dbl.op("CAP", dc, 0)
	dbl.op("CGET", dv, dc)
	dbl.op("ADDI", dv, dv, dv)
	dbl.op("RET", dv)
	dblIdx := dbl.done(bc.KInt, 0)

	f := newFn(m, "t")
	zero, cell, eff, k := f.reg(bc.KInt), f.reg(bc.KRef), f.reg(bc.KRef), f.reg(bc.KInt)
	f.op("KSMALL", zero, 0)
	f.op("CELL", cell, zero, int(bc.KInt))
	f.op("CLOS", eff, bodyIdx, cell, 1)
	f.op("EFFECT", eff)
	f.op("KSMALL", k, 1)
	f.op("CSET", cell, k)
	f.op("CSET", cell, k) // equal: no effect
	// A batch: two writes, one run.
	f.op("BATCH", 1)
	f.op("KSMALL", k, 2)
	f.op("CSET", cell, k)
	f.op("KSMALL", k, 3)
	f.op("CSET", cell, k)
	f.op("BATCH", 0)
	memoClos, memo, mv := f.reg(bc.KRef), f.reg(bc.KRef), f.reg(bc.KInt)
	f.op("CLOS", memoClos, dblIdx, cell, 1)
	f.op("MEMO", memo, memoClos, int(bc.KInt))
	f.op("MREAD", mv, memo)
	f.print(mv, bc.KInt)
	f.op("RET0")
	test(m, "state", f.done(bc.KVoid, 0))
	got, err := run(t, m, "-test")
	if err != nil {
		t.Fatalf("%v\n%s", err, got)
	}
	expect(t, got, "count=0", "count=1", "count=3", "6", "ok state")
}

func TestVMTasks(t *testing.T) {
	m := module()
	addInit(m)
	// async worker(): sleep 1s; print "slept"; return 7
	w := newFn(m, "worker")
	ns, fut, seven := w.reg(bc.KInt), w.reg(bc.KRef), w.reg(bc.KInt)
	m.Ints = append(m.Ints, 1_000_000_000)
	w.op("KINT", ns, 0)
	w.op("CALLB", fut, w.b("time.sleep"), ns, 1)
	w.op("AWAIT", bc.None, fut)
	msg := w.reg(bc.KRef)
	w.op("KSTR", msg, w.str("slept"))
	w.print(msg, bc.KRef)
	w.op("KSMALL", seven, 7)
	w.op("RET", seven)
	wIdx := w.done(bc.KInt, bc.FAsync)

	f := newFn(m, "t")
	clos, task, res := f.reg(bc.KRef), f.reg(bc.KRef), f.reg(bc.KInt)
	f.op("CLOS", clos, wIdx, 0, 0)
	f.op("TASK", task, clos)
	before := f.reg(bc.KRef)
	f.op("KSTR", before, f.str("started"))
	f.print(before, bc.KRef)
	f.op("AWAIT", res, task)
	f.print(res, bc.KInt)
	// A direct call of an async function runs on this task.
	f.op("CALL", res, wIdx, 0, 0)
	f.print(res, bc.KInt)
	f.op("RET0")
	test(m, "tasks", f.done(bc.KVoid, bc.FAsync))
	got, err := run(t, m, "-test")
	if err != nil {
		t.Fatalf("%v\n%s", err, got)
	}
	expect(t, got, "started", "slept", "7", "slept", "7", "ok tasks")
}

func TestVMSpawn(t *testing.T) {
	m := module()
	addInit(m)
	// heavy() sums 1..1e6 on a worker; captured n.
	heavy := newFn(m, "heavy")
	heavy.def.CapKinds = []bc.Kind{bc.KInt}
	i, sum, n, c := heavy.reg(bc.KInt), heavy.reg(bc.KInt), heavy.reg(bc.KInt), heavy.reg(bc.KBool)
	heavy.op("CAP", n, 0)
	heavy.op("KSMALL", i, 1)
	heavy.op("KSMALL", sum, 0)
	top := heavy.pc()
	heavy.op("LEI", c, i, n)
	exit := heavy.op("JF", c, 0)
	heavy.op("ADDI", sum, sum, i)
	heavy.op("ADDIK", i, i, 1)
	back := heavy.op("JMP", 0)
	heavy.a.PatchJump(back, top)
	heavy.a.PatchJump(exit, heavy.pc())
	heavy.op("RET", sum)
	hIdx := heavy.done(bc.KInt, 0)

	f := newFn(m, "t")
	nn, clos, kind, fut, res := f.reg(bc.KInt), f.reg(bc.KRef), f.reg(bc.KInt), f.reg(bc.KRef), f.reg(bc.KInt)
	m.Ints = append(m.Ints, 1000000)
	f.op("KINT", nn, 0)
	f.op("CLOS", clos, hIdx, nn, 1)
	f.op("KSMALL", kind, int(bc.KInt))
	f.op("CALLB", fut, f.b("async.spawn"), clos, 2)
	f.op("AWAIT", res, fut)
	f.print(res, bc.KInt)
	f.op("RET0")
	test(m, "spawn", f.done(bc.KVoid, bc.FAsync))
	got, err := run(t, m, "-test")
	if err != nil {
		t.Fatalf("%v\n%s", err, got)
	}
	expect(t, got, "500000500000", "ok spawn")
}

// sleep emits an await of time.sleep(1ms).
func (f *fn) sleep() {
	ns, fut := f.reg(bc.KInt), f.reg(bc.KRef)
	f.m.Ints = append(f.m.Ints, 1_000_000)
	f.op("KINT", ns, len(f.m.Ints)-1)
	f.op("CALLB", fut, f.b("time.sleep"), ns, 1)
	f.op("AWAIT", bc.None, fut)
}

// say emits a print of the string s.
func (f *fn) say(s string) {
	r := f.reg(bc.KRef)
	f.op("KSTR", r, f.str(s))
	f.print(r, bc.KRef)
}

// Async functions calling async functions: results through chains deeper
// than compiled code calls directly (AK_VM_ADEPTH_MAX), suspended at the
// bottom; a loop of calls that don't suspend; results discarded and
// kept; an error thrown after a suspension, through a hundred frames, to
// a handler.
func TestVMAsyncCalls(t *testing.T) {
	m := module()
	addInit(m)

	// async leaf(n) throws -> int: sleep; throw when n == 13; n + n.
	leaf := newFn(m, "leaf", bc.KInt)
	leaf.sleep()
	k13, c := leaf.reg(bc.KInt), leaf.reg(bc.KBool)
	leaf.op("KSMALL", k13, 13)
	leaf.op("EQI", c, 0, k13)
	jok := leaf.op("JF", c, 0)
	msg, e := leaf.reg(bc.KRef), leaf.reg(bc.KRef)
	leaf.op("KSTR", msg, leaf.str("thirteen"))
	leaf.op("CALLB", e, leaf.b("error"), msg, 1)
	leaf.op("THROW", e)
	leaf.a.PatchJump(jok, leaf.pc())
	r := leaf.reg(bc.KInt)
	leaf.op("ADDI", r, 0, 0)
	leaf.op("RET", r)
	leafIdx := leaf.done(bc.KInt, bc.FAsync|bc.FThrows)

	// async chain(d, n) throws -> int: d == 0 ? leaf(n) : chain(d-1, n) + 1.
	chainIdx := len(m.Funcs)
	ch := newFn(m, "chain", bc.KInt, bc.KInt)
	zero, cz, res := ch.reg(bc.KInt), ch.reg(bc.KBool), ch.reg(bc.KInt)
	ch.op("KSMALL", zero, 0)
	ch.op("EQI", cz, 0, zero)
	jrec := ch.op("JF", cz, 0)
	ch.op("CALL", res, leafIdx, 1, 1)
	ch.op("RET", res)
	ch.a.PatchJump(jrec, ch.pc())
	a0, a1 := ch.reg(bc.KInt), ch.reg(bc.KInt)
	ch.op("ADDIK", a0, 0, 0xFFFF) // d - 1
	ch.op("MOV", a1, 1)
	ch.op("CALL", res, chainIdx, a0, 2)
	ch.op("ADDIK", res, res, 1)
	ch.op("RET", res)
	if ch.done(bc.KInt, bc.FAsync|bc.FThrows) != chainIdx {
		t.Fatal("chain's index")
	}

	// async inc(n) -> int: n + 1, without waiting.
	inc := newFn(m, "inc", bc.KInt)
	ir := inc.reg(bc.KInt)
	inc.op("ADDIK", ir, 0, 1)
	inc.op("RET", ir)
	incIdx := inc.done(bc.KInt, bc.FAsync)

	// async text(n) -> string: sleep; "{n}".
	text := newFn(m, "text", bc.KInt)
	text.sleep()
	ts := text.reg(bc.KRef)
	text.op("TOSTR", ts, 0, int(bc.KInt))
	text.op("RET", ts)
	textIdx := text.done(bc.KRef, bc.FAsync)

	f := newFn(m, "t")
	d, n, out := f.reg(bc.KInt), f.reg(bc.KInt), f.reg(bc.KInt)
	f.op("KSMALL", d, 200)
	f.op("KSMALL", n, 5)
	f.op("CALL", out, chainIdx, d, 2)
	f.print(out, bc.KInt)
	acc, i, lim, lc := f.reg(bc.KInt), f.reg(bc.KInt), f.reg(bc.KInt), f.reg(bc.KBool)
	f.op("KSMALL", acc, 0)
	f.op("KSMALL", i, 0)
	f.op("KSMALL", lim, 1000)
	top := f.pc()
	f.op("LTI", lc, i, lim)
	jdone := f.op("JF", lc, 0)
	f.op("CALL", acc, incIdx, acc, 1)
	f.op("ADDIK", i, i, 1)
	back := f.op("JMP", 0)
	f.a.PatchJump(back, top)
	f.a.PatchJump(jdone, f.pc())
	f.print(acc, bc.KInt)
	f.op("CALL", bc.None, textIdx, n, 1)
	s := f.reg(bc.KRef)
	f.op("CALL", s, textIdx, n, 1)
	f.print(s, bc.KRef)
	d2, n2 := f.reg(bc.KInt), f.reg(bc.KInt)
	f.op("KSMALL", d2, 100)
	f.op("KSMALL", n2, 13)
	start := f.pc()
	f.op("CALL", out, chainIdx, d2, 2)
	end := f.pc()
	f.say("not reached")
	f.op("RET0")
	handler := f.pc()
	er, em, cs := f.reg(bc.KRef), f.reg(bc.KRef), f.reg(bc.KRef)
	f.op("CATCH", er)
	f.op("CALLB", em, f.b("error.message"), er, 1)
	f.op("KSTR", cs, f.str("caught: "))
	f.op("CONCAT", cs, cs, em)
	f.print(cs, bc.KRef)
	f.op("RET0")
	f.def.Handlers = []bc.Handler{{Start: uint32(start), End: uint32(end), Target: uint32(handler)}}
	test(m, "async calls", f.done(bc.KVoid, bc.FAsync))
	got, err := runMixed(t, m, "-leaks", "-test")
	if err != nil {
		t.Fatalf("%v\n%s", err, got)
	}
	expect(t, got, "210", "1000", "5", "caught: thirteen", "ok async calls")
}

// A task cancelled while it waits two async calls deep: each frame's
// handler (a defer's) runs as the cancellation unwinds it.
func TestVMAsyncCancel(t *testing.T) {
	m := module()
	addInit(m)

	// async inner(): await never(), its handler printing and rethrowing.
	inner := newFn(m, "inner")
	fut := inner.reg(bc.KRef)
	start := inner.pc()
	inner.op("CALLB", fut, inner.b("async.never"), 0, 0)
	inner.op("AWAIT", bc.None, fut)
	end := inner.pc()
	inner.op("RET0")
	h := inner.pc()
	ie := inner.reg(bc.KRef)
	inner.op("CATCH", ie)
	inner.say("inner stopped")
	inner.op("RETHROW", ie)
	inner.def.Handlers = []bc.Handler{{Start: uint32(start), End: uint32(end), Target: uint32(h)}}
	innerIdx := inner.done(bc.KVoid, bc.FAsync)

	// async outer() -> int: inner(); 1, likewise.
	outer := newFn(m, "outer")
	one := outer.reg(bc.KInt)
	start = outer.pc()
	outer.op("CALL", bc.None, innerIdx, 0, 0)
	end = outer.pc()
	outer.op("KSMALL", one, 1)
	outer.op("RET", one)
	h = outer.pc()
	oe := outer.reg(bc.KRef)
	outer.op("CATCH", oe)
	outer.say("outer stopped")
	outer.op("RETHROW", oe)
	outer.def.Handlers = []bc.Handler{{Start: uint32(start), End: uint32(end), Target: uint32(h)}}
	outerIdx := outer.done(bc.KInt, bc.FAsync)

	// async quick() -> int: sleep; 2.
	quick := newFn(m, "quick")
	quick.sleep()
	two := quick.reg(bc.KInt)
	quick.op("KSMALL", two, 2)
	quick.op("RET", two)
	quickIdx := quick.done(bc.KInt, bc.FAsync)

	// race(outer(), quick())
	f := newFn(m, "t")
	c1, c2, t1, t2 := f.reg(bc.KRef), f.reg(bc.KRef), f.reg(bc.KRef), f.reg(bc.KRef)
	f.op("CLOS", c1, outerIdx, 0, 0)
	f.op("TASK", t1, c1)
	f.op("CLOS", c2, quickIdx, 0, 0)
	f.op("TASK", t2, c2)
	arr, won, r := f.reg(bc.KRef), f.reg(bc.KRef), f.reg(bc.KInt)
	f.op("NEWA", arr, int(bc.KRef), bc.None, t1, 2)
	f.op("CALLB", won, f.b("async.race"), arr, 1)
	f.op("AWAIT", r, won)
	f.print(r, bc.KInt)
	f.op("RET0")
	test(m, "cancel", f.done(bc.KVoid, bc.FAsync))
	got, err := runMixed(t, m, "-leaks", "-test")
	if err != nil {
		t.Fatalf("%v\n%s", err, got)
	}
	expect(t, got, "inner stopped", "outer stopped", "2", "ok cancel")
}

// YIELD lets the other tasks run: two tasks take turns.
func TestVMAsyncYield(t *testing.T) {
	m := module()
	addInit(m)
	// async turns() (captures tag): tag + i for i in 0..<3, yielding after each.
	y := newFn(m, "turns")
	y.def.CapKinds = []bc.Kind{bc.KRef}
	tag, i, lim, c, s, is := y.reg(bc.KRef), y.reg(bc.KInt), y.reg(bc.KInt), y.reg(bc.KBool), y.reg(bc.KRef), y.reg(bc.KRef)
	y.op("CAP", tag, 0)
	y.op("KSMALL", i, 0)
	y.op("KSMALL", lim, 3)
	top := y.pc()
	y.op("LTI", c, i, lim)
	jdone := y.op("JF", c, 0)
	y.op("TOSTR", is, i, int(bc.KInt))
	y.op("CONCAT", s, tag, is)
	y.print(s, bc.KRef)
	y.op("YIELD")
	y.op("ADDIK", i, i, 1)
	back := y.op("JMP", 0)
	y.a.PatchJump(back, top)
	y.a.PatchJump(jdone, y.pc())
	y.op("RET0")
	yIdx := y.done(bc.KVoid, bc.FAsync)

	f := newFn(m, "t")
	a, b, ca, cb, ta, tb := f.reg(bc.KRef), f.reg(bc.KRef), f.reg(bc.KRef), f.reg(bc.KRef), f.reg(bc.KRef), f.reg(bc.KRef)
	f.op("KSTR", a, f.str("a"))
	f.op("KSTR", b, f.str("b"))
	f.op("CLOS", ca, yIdx, a, 1)
	f.op("CLOS", cb, yIdx, b, 1)
	f.op("TASK", ta, ca)
	f.op("TASK", tb, cb)
	f.op("AWAIT", bc.None, ta)
	f.op("AWAIT", bc.None, tb)
	f.op("RET0")
	test(m, "yield", f.done(bc.KVoid, bc.FAsync))
	got, err := runMixed(t, m, "-leaks", "-test")
	if err != nil {
		t.Fatalf("%v\n%s", err, got)
	}
	expect(t, got, "a0", "b0", "a1", "b1", "a2", "b2", "ok yield")
}

// The UI runtime's numbers (core/src/ui/elements_gen.h), from the
// prelude's tables they're generated from.

func elID(t *testing.T, name string) int {
	for i, e := range prelude.Elements {
		if e.Name == name {
			return i + 1
		}
	}
	t.Fatalf("no element %s", name)
	return 0
}

func keyID(t *testing.T, el, name string) int {
	n := 0
	if el == "" {
		for _, k := range prelude.Style {
			n++
			if k.Name == name {
				return n
			}
		}
		t.Fatalf("no style key %s", name)
	}
	n = len(prelude.Style)
	for _, e := range prelude.Elements {
		for _, k := range e.Keys {
			n++
			if e.Name == el && k.Name == name {
				return n
			}
		}
		n += len(e.Slots)
	}
	t.Fatalf("no key %s.%s", el, name)
	return 0
}

func evID(t *testing.T, el, name string) int {
	n := 0
	for _, e := range prelude.Elements {
		for _, ev := range e.Events {
			n++
			if e.Name == el && ev.Name == name {
				return n
			}
		}
	}
	t.Fatalf("no event %s.%s", el, name)
	return 0
}

// callb emits a builtin's call with args copied into consecutive
// registers of their kinds (CALLB takes its arguments from a run of
// registers).
func (f *fn) callb(dst int, name string, args ...[2]int) {
	first := -1
	for i, a := range args {
		r := f.reg(bc.Kind(a[1]))
		if i == 0 {
			first = r
		}
		if bc.Kind(a[1]) == bc.KRef {
			f.op("MOVR", r, a[0])
		} else {
			f.op("MOV", r, a[0])
		}
	}
	if first < 0 {
		first = 0
	}
	f.op("CALLB", dst, f.b(name), first, len(args))
}

func ref(r int) [2]int { return [2]int{r, int(bc.KRef)} }
func num(r int) [2]int { return [2]int{r, int(bc.KInt)} }

// A Counter, as the compiler lowers
//
//	component Counter {
//	  state count = 0
//	  Column {
//	    gap: 8
//	    Text("Count: {count}")
//	    Button("Add") { on press { count += 1 } }
//	  }
//	}
//	test "counter" { let app = mount(Counter()); app.tap("Add"); app.expect("Count: 1") }
//
// mounted on the fake host by the test builtins.
func TestVMCounterOnTheFakeHost(t *testing.T) {
	m, bodyIdx := counterModule(t)
	counterTests(t, m, bodyIdx)
}

// counterModule is the Counter component (above) in a module.
func counterModule(t *testing.T) (*bc.Module, int) {
	m := module()
	addInit(m)

	// The Text's binding: "Count: {count}".
	label := newFn(m, "Counter.label")
	label.def.CapKinds = []bc.Kind{bc.KRef}
	c, v, s, out := label.reg(bc.KRef), label.reg(bc.KInt), label.reg(bc.KRef), label.reg(bc.KRef)
	label.op("CAP", c, 0)
	label.op("CGET", v, c)
	label.op("KSTR", s, label.str("Count: "))
	parts := []int{label.reg(bc.KRef), label.reg(bc.KInt)}
	label.op("MOVR", parts[0], s)
	label.op("MOV", parts[1], v)
	label.op("INTERP", out, parts[0], 2, label.str(string([]byte{byte(bc.KRef), byte(bc.KInt)})))
	label.op("RET", out)
	labelIdx := label.done(bc.KRef, 0)

	// on press { count += 1 }
	press := newFn(m, "Counter.press")
	press.def.CapKinds = []bc.Kind{bc.KRef}
	pc, pv := press.reg(bc.KRef), press.reg(bc.KInt)
	press.op("CAP", pc, 0)
	press.op("CGET", pv, pc)
	press.op("ADDIK", pv, pv, 1)
	press.op("CSET", pc, pv)
	press.op("RET0")
	pressIdx := press.done(bc.KVoid, 0)

	// The component's body.
	body := newFn(m, "Counter")
	name := body.reg(bc.KRef)
	body.op("KSTR", name, body.str("Counter"))
	body.callb(bc.None, "ui.enter", ref(name))
	zero, cell := body.reg(bc.KInt), body.reg(bc.KRef)
	body.op("KSMALL", zero, 0)
	body.op("CELLK", cell, zero, int(bc.KInt), body.str("Counter.count"))
	k := body.reg(bc.KInt)
	col := body.reg(bc.KRef)
	body.op("KSMALL", k, elID(t, "Column"))
	body.callb(col, "ui.new", num(k))
	gap := body.reg(bc.KFloat)
	m.Floats = append(m.Floats, 8)
	body.op("KFLOAT", gap, len(m.Floats)-1)
	body.op("KSMALL", k, keyID(t, "", "gap"))
	body.callb(bc.None, "ui.set", ref(col), num(k), [2]int{gap, int(bc.KFloat)})
	// Text("Count: {count}")
	txt, thunk := body.reg(bc.KRef), body.reg(bc.KRef)
	body.op("KSMALL", k, elID(t, "Text"))
	body.callb(txt, "ui.new", num(k))
	body.op("CLOS", thunk, labelIdx, cell, 1)
	body.op("KSMALL", k, keyID(t, "Text", "text"))
	body.callb(bc.None, "ui.bind", ref(txt), num(k), ref(thunk))
	body.callb(bc.None, "ui.add", ref(col), ref(txt))
	// Button("Add") { on press { ... } }
	btn, title, handler := body.reg(bc.KRef), body.reg(bc.KRef), body.reg(bc.KRef)
	body.op("KSMALL", k, elID(t, "Button"))
	body.callb(btn, "ui.new", num(k))
	body.op("KSTR", title, body.str("Add"))
	body.op("KSMALL", k, keyID(t, "Button", "title"))
	body.callb(bc.None, "ui.set", ref(btn), num(k), ref(title))
	body.op("CLOS", handler, pressIdx, cell, 1)
	body.op("KSMALL", k, evID(t, "Button", "press"))
	body.callb(bc.None, "ui.on", ref(btn), num(k), ref(handler))
	body.callb(bc.None, "ui.add", ref(col), ref(btn))
	body.callb(bc.None, "ui.leave")
	body.op("RET", col)
	bodyIdx := body.done(bc.KRef, 0)
	return m, bodyIdx
}

func counterTests(t *testing.T, m *bc.Module, bodyIdx int) {
	// The test block.
	f := newFn(m, "t")
	build, app, s1 := f.reg(bc.KRef), f.reg(bc.KRef), f.reg(bc.KRef)
	f.op("CLOS", build, bodyIdx, 0, 0)
	f.callb(app, "test.mount", ref(build))
	f.op("KSTR", s1, f.str("Add"))
	f.callb(bc.None, "test.tap", ref(app), ref(s1))
	f.op("KSTR", s1, f.str("Count: 1"))
	f.callb(bc.None, "test.expect", ref(app), ref(s1))
	f.op("KSTR", s1, f.str("Count: 2"))
	f.callb(bc.None, "test.expect_no", ref(app), ref(s1))
	// What the screen shows, as test.dump has it.
	dump := f.reg(bc.KRef)
	f.callb(dump, "test.dump", ref(app))
	f.print(dump, bc.KRef)
	f.op("RET0")
	test(m, "counter", f.done(bc.KVoid, bc.FThrows))

	// A failing expectation says what's missing and what's shown.
	g := newFn(m, "u")
	gb, ga, gs := g.reg(bc.KRef), g.reg(bc.KRef), g.reg(bc.KRef)
	g.op("CLOS", gb, bodyIdx, 0, 0)
	g.callb(ga, "test.mount", ref(gb))
	g.op("KSTR", gs, g.str("Count: 7"))
	g.callb(bc.None, "test.expect", ref(ga), ref(gs))
	g.op("RET0")
	test(m, "missing", g.done(bc.KVoid, bc.FThrows))

	got, err := run(t, m, "-test")
	if err == nil {
		t.Fatalf("want the second test to fail\n%s", got)
	}
	var lines []string
	for _, l := range strings.Split(got, "\n") {
		if l != "" {
			lines = append(lines, l)
		}
	}
	want := []string{
		"View 0,0 390x844",
		"  Text 0,0 390x20 \"Count: 1\"",
		"  Button 0,28 390x44 \"Add\"",
		"ok counter",
		"FAIL missing: the screen doesn't show \"Count: 7\"",
	}
	for i, w := range want {
		if i >= len(lines) || lines[i] != w {
			t.Fatalf("got:\n%s\nwant (first lines):\n%s", got, strings.Join(want, "\n"))
		}
	}
	if !strings.Contains(got, "Text 0,0 390x20 \"Count: 0\"") {
		t.Fatalf("the failure doesn't show the screen:\n%s", got)
	}
}
