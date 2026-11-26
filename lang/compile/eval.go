package compile

import (
	"errors"
	"fmt"
	"strings"
	"sync"

	"github.com/egoist/asuka/lang/bytecode"
	"github.com/egoist/asuka/lang/check"
	"github.com/egoist/asuka/lang/ir"
)

// An Evaluator compiles expressions to evaluate in the frames of a
// module that's running (asuka dev's debugger: a breakpoint's condition,
// a log message, an editor's watch, hover or console): each a function
// of the module's extension (bytecode.Extension), taking the frame's
// locals it sees. A module compiled for development (not optimized:
// its locals named) has one (bytecode.Module.Evaluator).
type Evaluator struct {
	res *Result

	mu     sync.Mutex
	prog   *ir.Program // the lowering expressions continue (res.IR, or one lowered again)
	hash   uint64
	hashed bool
	cache  map[evalKey]*Fragment
	ext    []byte // the extension encoded, as of extFns functions
	extFns int
}

type evalKey struct {
	fn     uint32
	params string
	src    string
	mode   EvalMode
}

// EvalMode is what an expression is for.
type EvalMode int

const (
	// EvalValue: any value, written as the debugger writes a local's.
	EvalValue EvalMode = iota
	// EvalCond: a breakpoint's condition, a bool.
	EvalCond
	// EvalLog: a log message: text with expressions in braces ("count
	// is {count}"), as a string literal's.
	EvalLog
)

// Fragment is an expression compiled to evaluate in a frame: Func, a
// function of Ext (the module's extension, with every expression
// compiled for it so far), takes the frame's locals Params (indexes in
// its function's bytecode.FuncDef.Locals), in order, and returns the
// value, whose descriptor is Desc (bytecode.Local's).
type Fragment struct {
	Base   uint64 // the module's bytecode.Hash, as the extension names it
	Ext    []byte
	Func   uint32
	Params []uint32
	Desc   string
}

// Hash is the module's bytecode.Hash.
func (e *Evaluator) Hash() uint64 {
	e.mu.Lock()
	defer e.mu.Unlock()
	return e.baseHash()
}

func (e *Evaluator) baseHash() uint64 {
	if !e.hashed {
		e.hash, e.hashed = bytecode.Hash(e.res.Module.Encode()), true
	}
	return e.hash
}

// Compile compiles src (an expression, or a log message's text) to
// evaluate in function fn of the module at pc (in words), with the
// locals in scope there.
func (e *Evaluator) Compile(fn, pc uint32, src string, mode EvalMode) (*Fragment, error) {
	e.mu.Lock()
	defer e.mu.Unlock()
	m := e.res.Module
	if int(fn) >= len(m.Funcs) {
		return nil, errors.New("not a function of the app's")
	}
	if e.prog == nil {
		e.prog = e.res.IR
		e.cache = map[evalKey]*Fragment{}
	}
	f := &m.Funcs[fn]
	locals := e.prog.EvalLocals(int(fn))
	var vars []check.EvalVar
	var params []uint32
	var key strings.Builder
	for _, i := range visibleLocals(f, pc) {
		if i >= len(locals) || locals[i].Type == nil && locals[i].Env == nil {
			continue
		}
		vars = append(vars, locals[i])
		params = append(params, uint32(i))
		fmt.Fprintf(&key, "%d,", i)
	}
	k := evalKey{fn, key.String(), src, mode}
	if fr := e.cache[k]; fr != nil {
		out := *fr
		out.Ext = e.extension()
		return &out, nil
	}
	expr := src
	var want check.Type
	switch mode {
	case EvalCond:
		want = check.Bool
	case EvalLog:
		expr, want = logLiteral(src), check.String
	}
	ev, diags := e.res.Program.CheckEval(f.File, vars, expr, want)
	if len(diags) > 0 {
		return nil, errors.New(diags[0].Msg)
	}
	idx, err := e.prog.LowerEval(ev)
	if err != nil {
		// Its lowering can't go on: the next expression starts again
		// from the module.
		if again, ds := e.prog.Again(); again != nil && len(ds) == 0 {
			if bytecode.Hash(again.Encode().Encode()) == e.baseHash() {
				e.prog = again
				e.cache = map[evalKey]*Fragment{}
				e.ext, e.extFns = nil, 0
			}
		}
		if errors.Is(err, ir.ErrBaseChanged) {
			return nil, errors.New("this needs more of a type of the app's than the app's code makes (it used as an interface, compared or hashed as the app never does): write that in the app to evaluate it")
		}
		return nil, err
	}
	fr := &Fragment{Base: e.baseHash(), Func: uint32(idx), Params: params, Desc: e.prog.EvalDesc(ev.Type)}
	e.cache[k] = fr
	out := *fr
	out.Ext = e.extension()
	return &out, nil
}

// extension is the extension encoded as it is now (e.mu held).
func (e *Evaluator) extension() []byte {
	if n := len(e.prog.Funcs); e.ext == nil || n != e.extFns {
		e.ext, e.extFns = e.prog.Extension(e.baseHash()).Encode(), n
	}
	return e.ext
}

// visibleLocals are the locals of f in scope at pc, as the debugger
// lists a frame's (core/src/vm/debug.c's frame_locals): one per name, the
// innermost.
func visibleLocals(f *bytecode.FuncDef, pc uint32) []int {
	var out []int
	for i, l := range f.Locals {
		if pc < l.From || pc >= l.To {
			continue
		}
		shadowed := false
		for _, o := range f.Locals[i+1:] {
			if pc >= o.From && pc < o.To && o.Name == l.Name {
				shadowed = true
				break
			}
		}
		if !shadowed {
			out = append(out, i)
		}
	}
	return out
}

// logLiteral is a log message as a string literal: its text as it is,
// its {expressions} interpolated.
func logLiteral(msg string) string {
	var b strings.Builder
	b.WriteByte('"')
	depth := 0
	for _, r := range msg {
		switch {
		case depth > 0:
			if r == '{' {
				depth++
			} else if r == '}' {
				depth--
			}
			b.WriteRune(r)
		case r == '{':
			depth++
			b.WriteRune(r)
		case r == '"' || r == '\\':
			b.WriteByte('\\')
			b.WriteRune(r)
		case r == '\n':
			b.WriteString(`\n`)
		case r == '\t':
			b.WriteString(`\t`)
		default:
			b.WriteRune(r)
		}
	}
	b.WriteByte('"')
	return b.String()
}
