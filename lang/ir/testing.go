package ir

import (
	bc "github.com/egoist/asuka/lang/bytecode"
	"github.com/egoist/asuka/lang/check"
	"github.com/egoist/asuka/lang/syntax"
)

// What asuka test adds to test blocks (docs/testing.md): property tests'
// parameters, benches, keep and snapshots. The runner (core/runner)
// calls a property test's function with the values it generates, and a
// bench's with how many times to run what it measures.

// newTest makes t's function and its entry in the module's tests: a
// test's takes its parameters, a bench's the iterations (an int).
func (l *Lowerer) newTest(t *check.Test) (*fb, bc.Test) {
	name, n := "test "+t.Name, len(t.Params)
	if t.Bench != nil {
		name, n = "bench "+t.Name, 1
	}
	b := l.newFunc(name, l.fileOf(t.Node()), n, bc.KVoid)
	b.ctx = t.Ctx
	e := bc.Test{Name: t.Name, Func: uint32(b.idx), Bench: t.Bench != nil}
	for _, p := range t.Params {
		e.Params = append(e.Params, bc.Param{Name: p.Name(), Desc: l.desc(p.Type)})
	}
	return b, e
}

// testParams gives the test's parameters their registers, the function's
// first: a bench's iterations, a property test's values.
func (b *fb) testParams(t *check.Test) {
	if t.Bench != nil {
		b.bench, b.benchN = true, b.newReg(bc.KInt)
		return
	}
	regs := make([]int, len(t.Params))
	for i, p := range t.Params {
		regs[i] = b.newReg(b.kind(p.Type))
	}
	b.openScope()
	for i, p := range t.Params {
		if p.Boxed() {
			b.declare(p, regs[i])
		} else {
			b.setVar(p, regs[i])
		}
	}
}

// testBody lowers a test's statements; a bench's measured part runs in
// a loop of its iterations, between bench.start and bench.stop, which
// time it and count what it allocates.
func (b *fb) testBody(t *check.Test) {
	if t.Bench == nil || t.Measure != nil {
		b.stmts(t.Body().Items)
		return
	}
	b.measureLoop(func() { b.block(t.Body()) })
}

// measure lowers a bench's measure { … }.
func (b *fb) measure(s *syntax.MeasureStmt) {
	if !b.bench {
		b.block(s.Body)
		return
	}
	b.measureLoop(func() { b.block(s.Body) })
}

func (b *fb) measureLoop(body func()) {
	b.callBTo("bench.start", bc.None)
	i := b.local(bc.KInt)
	b.loadInt(i, 0)
	cond := b.local(bc.KBool)
	top, end := b.f.NewLabel(), b.f.NewLabel()
	b.f.Place(top)
	b.emit(op("LTI"), cond, i, b.benchN)
	b.emitJump(op("JF"), cond, end)
	mark := b.mark()
	body()
	b.release(mark)
	b.emit(op("ADDIK"), i, i, 1)
	b.jmp(top)
	b.f.Place(end)
	b.callBTo("bench.stop", bc.None)
}

// testingBuiltin lowers keep, expectSnapshot and expectInlineSnapshot.
func (b *fb) testingBuiltin(e *syntax.CallExpr, call *check.Call, dst int) int {
	switch call.Builtin.ID {
	case check.BKeep:
		// The value, through a builtin the C compiler can't see into: it
		// can't drop the work that computes it.
		t := b.typeOf(call.Args[0])
		k := b.kind(t)
		v := b.value(call.Args[0], -1)
		name := "keep"
		if k == bc.KRef {
			name = "keep.ref"
		}
		return b.copyTo(b.callB(name, k, v), dst)
	case check.BSnapshot:
		text := b.snapshotText(call.Args[0])
		b.callBTo("test.snapshot", bc.None, text, b.kstr(b.where(e)))
	case check.BInline:
		text := b.snapshotText(call.Args[0])
		want, has := b.kstr(""), b.konst(0)
		if len(call.Args) > 1 {
			want, has = b.value(call.Args[1], -1), b.konst(1)
		}
		p := b.l.fset.Position(e.Range().Start)
		b.callBTo("test.inline_snapshot", bc.None, text, want, has, b.kstr(p.Filename), b.konst(int64(p.Line)), b.konst(int64(p.Column)))
	}
	return -1
}

// snapshotText is what a snapshot of x says: a string as it is, any
// other value as test.show prints it (core/runner/show.c).
func (b *fb) snapshotText(x syntax.Expr) int {
	t := b.typeOf(x)
	v := b.value(x, -1)
	if t == check.String {
		return v
	}
	if r, ok := b.ownDescription(v, t); ok {
		return r // its own text
	}
	return b.callB("test.show", bc.KRef, v, b.kstr(b.l.showDesc(t)))
}
