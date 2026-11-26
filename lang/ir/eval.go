package ir

import (
	"errors"
	"fmt"
	"maps"
	"slices"

	bc "github.com/egoist/asuka/lang/bytecode"
	"github.com/egoist/asuka/lang/check"
)

// What asuka dev's debugger compiles into a program that's running: an
// expression checked to evaluate in a paused frame (check.Eval), lowered
// as a function of its own whose parameters are the frame's locals
// (LowerEval), after the program's functions. What it needs that the
// module doesn't have yet (functions instantiated for it, types,
// strings, the constants it hoists) is lowered after the module too:
// the module's extension (Extension), which grows with each expression,
// the module itself untouched. A player builds the module and the
// extension into one (core/src/vm/load.c's ak_module_extend) to run them.

// evalState is a program's lowering since its module: where the
// module's tables end, its shapes' methods as they were, and how many of
// the pending string tables init.strings made.
type evalState struct {
	nInts, nFloats, nStrings, nShapes, nFuncs, nBuiltins, nGlobals int
	methods                                                        []int
	pendingStrings                                                 int
	inits                                                          []bc.ExtInit
	broken                                                         error
}

// ErrBaseChanged is LowerEval's when the expression needs the module's
// own types to have more than they do (a type used as an interface its
// values are never converted to in the app's code: methods its shape
// would need). The program can't lower another expression after it: it
// must be lowered again (Lower) to.
var ErrBaseChanged = errors.New("ir: the expression changes the module's own types")

// evalInit readies p's lowering for the debugger's expressions: its
// module's tables are copied, so that what they add doesn't change the
// module (encoded and sent already).
func (p *Program) evalInit() *evalState {
	l := p.l
	if l.ev != nil {
		return l.ev
	}
	m := p.Mod
	mod := &bc.Module{App: m.App, Main: m.Main, Init: m.Init}
	mod.Ints = slices.Clone(m.Ints)
	mod.Floats = slices.Clone(m.Floats)
	mod.Strings = slices.Clone(m.Strings)
	mod.Builtins = slices.Clone(m.Builtins)
	mod.Globals = slices.Clone(m.Globals)
	mod.Shapes = make([]bc.ShapeDef, len(m.Shapes))
	ev := &evalState{nInts: len(m.Ints), nFloats: len(m.Floats), nStrings: len(m.Strings), nShapes: len(m.Shapes),
		nFuncs: len(p.Funcs), nBuiltins: len(m.Builtins), nGlobals: len(m.Globals), pendingStrings: len(l.pendingStrings)}
	for i, s := range m.Shapes {
		s.Fields = slices.Clone(s.Fields)
		s.Cases = slices.Clone(s.Cases)
		s.Methods = maps.Clone(s.Methods)
		mod.Shapes[i] = s
		ev.methods = append(ev.methods, len(s.Methods))
	}
	l.mod = mod
	l.ev = ev
	return ev
}

// LowerEval lowers ev, checked in p's program (check.Program.CheckEval),
// as a function after the module's, taking ev's parameters in order and
// returning the expression's value (bytecode.FThrows: its error is the
// evaluation's). It returns the function's index, which Extension's
// functions have. An expression the lowering can't do is an error, as
// is one that would add to the module's own types (ErrBaseChanged),
// after which p lowers no more.
func (p *Program) LowerEval(ev *check.Eval) (int, error) {
	if p.l == nil {
		return -1, errors.New("ir: this program can't evaluate expressions")
	}
	st := p.evalInit()
	if st.broken != nil {
		return -1, st.broken
	}
	l := p.l
	nerrs := len(l.errs)
	globals := len(l.mod.Globals)
	k := l.kind(ev.Type)
	// Its file is the expression's own: no breakpoint of the app's
	// lines is in it.
	file := l.fileOf(ev.Expr)
	b := l.newFunc("eval", file, len(ev.Params), k)
	b.ctx = ev.Ctx
	b.at(ev.Expr)
	b.f.Flags |= bc.FThrows
	regs := make([]int, len(ev.Params))
	for i, v := range ev.Params {
		if i == ev.EnvParam {
			// The component's env: its members, by their names.
			regs[i] = b.newReg(bc.KRef)
			b.envReg, b.comp = regs[i], ev.Env
			continue
		}
		regs[i] = b.newReg(b.kind(v.Type))
	}
	b.openScope()
	for i, v := range ev.Params {
		if i != ev.EnvParam {
			b.setVar(v, regs[i])
		}
	}
	if k == bc.KVoid {
		b.exprStmt(ev.Expr)
		b.emit(op("RET0"))
	} else {
		mark := b.mark()
		r := b.value(ev.Expr, -1)
		b.ret(r, k)
		b.release(mark)
	}
	b.closeScope()
	if ev.Ctx.Awaits {
		b.f.Flags |= bc.FAsync
	}

	// What it asked for: the functions it calls, made in turn; the
	// constants it hoists and the tables it reads, made by an init of
	// its own (the module's ran already).
	init := l.newFunc("eval.init", file, 0, bc.KVoid)
	saved := l.stringsFn
	l.stringsFn = init
	for {
		for len(l.work) > 0 {
			w := l.work[0]
			l.work = l.work[1:]
			w()
		}
		if len(l.pendingConsts) > 0 {
			l.finishConsts()
			continue
		}
		if len(l.pendingNative) > 0 {
			l.finishNativeErrors()
			continue
		}
		break
	}
	l.finishExternTemplates()
	l.writeStrings(l.pendingStrings[st.pendingStrings:])
	st.pendingStrings = len(l.pendingStrings)
	init.emit(op("RET0"))
	l.stringsFn = saved
	if len(l.mod.Globals) > globals {
		st.inits = append(st.inits, bc.ExtInit{First: uint32(globals), Func: uint32(init.idx)})
	}

	if len(l.errs) > nerrs {
		err := l.errs[nerrs]
		l.errs = l.errs[:nerrs]
		st.broken = fmt.Errorf("%s", err.Msg)
		return -1, err
	}
	for i := 0; i < st.nShapes; i++ {
		if len(l.mod.Shapes[i].Methods) != st.methods[i] {
			st.broken = ErrBaseChanged
			return -1, ErrBaseChanged
		}
	}
	return b.idx, nil
}

// EvalDesc is the descriptor the debugger writes a value of t by
// (bytecode.Local's Desc).
func (p *Program) EvalDesc(t check.Type) string { return p.l.debugDesc(t) }

// Extension is what LowerEval lowered after p's module, base its Hash:
// its functions and what they added to the module's tables.
func (p *Program) Extension(base uint64) *bc.Extension {
	st := p.evalInit()
	m := p.l.mod
	x := &bc.Extension{Base: base,
		NInts: uint32(st.nInts), NFloats: uint32(st.nFloats), NStrings: uint32(st.nStrings), NShapes: uint32(st.nShapes),
		NFuncs: uint32(st.nFuncs), NBuiltins: uint32(st.nBuiltins), NGlobals: uint32(st.nGlobals),
		Ints: m.Ints[st.nInts:], Floats: m.Floats[st.nFloats:], Strings: m.Strings[st.nStrings:], Shapes: m.Shapes[st.nShapes:],
		Builtins: m.Builtins[st.nBuiltins:], Globals: m.Globals[st.nGlobals:], Inits: st.inits}
	for _, f := range p.Funcs[st.nFuncs:] {
		x.Funcs = append(x.Funcs, f.encode())
	}
	return x
}

// EvalLocals are what an expression evaluated in a frame of the encoded
// function fn sees of each of its Locals (bytecode.FuncDef's, in order):
// its name as a check.EvalVar, its Type nil (and Env too) for one it
// can't see.
func (p *Program) EvalLocals(fn int) []check.EvalVar {
	if fn < 0 || fn >= len(p.Funcs) {
		return nil
	}
	f := p.Funcs[fn]
	var vs []check.EvalVar
	for _, l := range f.Locals {
		end := l.End
		if end < 0 || end > len(f.Code) {
			end = len(f.Code)
		}
		if l.Start >= end {
			continue // as encode leaves it out
		}
		vs = append(vs, check.EvalVar{Name: l.Name, Type: l.Type, Env: l.Env})
	}
	return vs
}

// Again lowers p's program again, as it was lowered: a program whose
// lowering an expression left unable to lower more (LowerEval's
// ErrBaseChanged) is replaced by it. Its module is the same as p's.
func (p *Program) Again() (*Program, []check.Diagnostic) {
	if p.l == nil {
		return nil, nil
	}
	return Lower(p.l.prog, p.l.opts)
}
