// Package ir is the Asuka compiler's lowered program: functions of
// three-address code over typed registers, made from the checked program
// (lang/check) as lang/LOWERING.md says. Its instructions are the VM's
// (lang/bytecode's opcodes), with registers already allocated (per kind)
// and jumps to labels; Encode writes the bytecode module, resolving
// labels into offsets and handler ranges, and the C backend (lang/cgen)
// will read the same functions.
package ir

import (
	"github.com/egoist/asuka/lang/bytecode"
	"github.com/egoist/asuka/lang/check"
)

// A Label is a place in a function's code; jumps and handlers name it.
type Label int32

// An Instr is an instruction: an opcode and its operands, as many as the
// opcode takes (an Off operand holds a Label).
type Instr struct {
	Op   bytecode.Op
	Args [5]int32
	Line int32
}

// A Handler catches errors raised in [Start, End) at Target.
type Handler struct {
	Start, End, Target Label
	// Rethrows: its code always throws the error on (a defer's, a
	// cleanup's): bytecode.Handler's.
	Rethrows bool
}

// A Func is a lowered function.
type Func struct {
	Name     string
	File     string
	NParams  int
	Kinds    []bytecode.Kind // per register
	CapKinds []bytecode.Kind
	Ret      bytecode.Kind
	Flags    uint8
	Code     []Instr
	Handlers []Handler // innermost first
	// Inlines are the functions the optimizer inlined into it: [Start,
	// End) is Func's body, called at Line (bytecode.Inline).
	Inlines []Inline
	// Locals are the names the debugger shows (bytecode.Local): each a
	// register from one instruction to another. Recorded with Options'
	// Locals; the optimizer drops them (its passes move values between
	// registers and instructions).
	Locals []Local
	labels []int // per label, the instruction it's at (-1: not placed yet)
	// tempSpans: where reference registers were temps, by the lowered
	// code's instructions (the optimizer's first pass reads them, then
	// drops them: they don't follow the code as it changes).
	tempSpans []tempSpan
}

// A tempSpan is [start, end) of the lowered code, where reg was a
// statement's temp (no name's register).
type tempSpan struct{ reg, start, end int }

// An Inline is a function's body inlined in another's (opt_inline.go).
type Inline struct {
	Start, End Label
	Func       int
	Line       int32
}

// A Local is a name that's in a register over [Start, End) of the
// code's instructions (End -1: to the function's end).
type Local struct {
	Name       string
	Reg        int
	Via        uint8 // bytecode.LocalValue, LocalBox, LocalSignal
	Desc       string
	Start, End int
	// Type is the name's type (its function's type parameters
	// substituted), what an expression the debugger evaluates in the
	// frame sees it as; nil for one it can't name (a component's env).
	// Not encoded.
	Type check.Type
	// Env is the component or store whose env the local is (its
	// "self"), whose members an expression evaluated in the frame sees
	// by their names.
	Env *check.TypeName
}

// NewLabel makes a label, not placed yet.
func (f *Func) NewLabel() Label {
	f.labels = append(f.labels, -1)
	return Label(len(f.labels) - 1)
}

// Place puts l at the next instruction.
func (f *Func) Place(l Label) { f.labels[l] = len(f.Code) }

// Here makes a label at the next instruction.
func (f *Func) Here() Label {
	l := f.NewLabel()
	f.Place(l)
	return l
}

// Emit appends an instruction.
func (f *Func) Emit(line int32, op bytecode.Op, args ...int) {
	in := Instr{Op: op, Line: line}
	if len(args) != len(bytecode.Ops[op].Operands) {
		panic("ir: " + op.String() + " takes " + itoa(len(bytecode.Ops[op].Operands)) + " operands, got " + itoa(len(args)))
	}
	for i, a := range args {
		in.Args[i] = int32(a)
	}
	f.Code = append(f.Code, in)
}

// Reg adds a register of kind k. A function encodes with fewer than
// bytecode.None (an instruction names a register in 16 bits): the
// lowering says when one would need more.
func (f *Func) Reg(k bytecode.Kind) int {
	f.Kinds = append(f.Kinds, k)
	return len(f.Kinds) - 1
}

// A Program is the lowered program: the module's pools and tables, and
// its functions.
type Program struct {
	Mod   *bytecode.Module // Funcs filled by Encode
	Funcs []*Func
	// Cover is what the program counts, with Options.Cover (cover.go).
	Cover *Cover
	// ints and floats index Mod's pools by value, for the constants the
	// optimizer adds (opt.go).
	ints   map[int64]int
	floats map[uint64]int
	// mutNames: the names of the types' mut fns (CALLI of one may write
	// its receiver's register back).
	mutNames map[string]bool
	// l is the lowering that made it, kept for what the debugger
	// compiles into the program later (eval.go).
	l *Lowerer
}

func itoa(i int) string {
	if i == 0 {
		return "0"
	}
	var b []byte
	neg := i < 0
	if neg {
		i = -i
	}
	for i > 0 {
		b = append([]byte{byte('0' + i%10)}, b...)
		i /= 10
	}
	if neg {
		b = append([]byte{'-'}, b...)
	}
	return string(b)
}

// Encode writes the program's functions into its module.
func (p *Program) Encode() *bytecode.Module {
	m := p.Mod
	m.Funcs = make([]bytecode.FuncDef, len(p.Funcs))
	for i, f := range p.Funcs {
		m.Funcs[i] = f.encode()
	}
	return m
}

func (f *Func) encode() bytecode.FuncDef {
	// Word offsets of each instruction.
	pcs := make([]int, len(f.Code)+1)
	pc := 0
	for i, in := range f.Code {
		pcs[i] = pc
		pc += in.Op.Width()
	}
	pcs[len(f.Code)] = pc
	at := func(l Label) int {
		i := f.labels[l]
		if i < 0 {
			panic("ir: label not placed in " + f.Name)
		}
		return pcs[i]
	}
	var a bytecode.Asm
	var lines []bytecode.Line
	last := int32(-1)
	for i, in := range f.Code {
		if in.Line != last && in.Line > 0 {
			lines = append(lines, bytecode.Line{PC: uint32(pcs[i]), Line: uint32(in.Line)})
			last = in.Line
		}
		ops := bytecode.Ops[in.Op].Operands
		args := make([]int, len(ops))
		for j, o := range ops {
			if o == bytecode.Off {
				args[j] = at(Label(in.Args[j])) - pcs[i+1]
			} else {
				args[j] = int(in.Args[j])
			}
		}
		a.Emit(in.Op, args...)
	}
	def := bytecode.FuncDef{
		Name: f.Name, File: f.File, NParams: uint16(f.NParams), Kinds: f.Kinds, CapKinds: f.CapKinds,
		Ret: f.Ret, Flags: f.Flags, Code: a.Code, Lines: lines,
	}
	for _, h := range f.Handlers {
		def.Handlers = append(def.Handlers, bytecode.Handler{Start: uint32(at(h.Start)), End: uint32(at(h.End)), Target: uint32(at(h.Target)), Rethrows: h.Rethrows})
	}
	for _, in := range f.Inlines {
		if s, e := at(in.Start), at(in.End); s < e {
			def.Inlines = append(def.Inlines, bytecode.Inline{Start: uint32(s), End: uint32(e), Func: uint32(in.Func), Line: uint32(in.Line)})
		}
	}
	for _, l := range f.Locals {
		end := l.End
		if end < 0 || end > len(f.Code) {
			end = len(f.Code)
		}
		if l.Start >= end {
			continue
		}
		def.Locals = append(def.Locals, bytecode.Local{Name: l.Name, Reg: uint16(l.Reg), Via: l.Via, Desc: l.Desc, From: uint32(pcs[l.Start]), To: uint32(pcs[end])})
	}
	return def
}
