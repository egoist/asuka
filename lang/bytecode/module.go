package bytecode

import (
	"encoding/binary"
	"errors"
	"fmt"
	"math"
)

// Magic starts every module; Version changes with the format.
const (
	Magic   = "AKBC"
	Version = 9
)

// Kind is how a value is stored (core/src/rt/rt.h's ak_kind).
type Kind uint8

const (
	KVoid Kind = iota
	KInt
	KFloat
	KBool
	KRef
)

// KU64Text is TOSTR's kind for a u64, which it writes unsigned: never a
// register's kind (a u64 is a KInt).
const KU64Text Kind = 5

func (k Kind) String() string {
	if k > KRef {
		return "kind" + fmt.Sprint(int(k))
	}
	return [...]string{"void", "int", "float", "bool", "ref"}[k]
}

// None is the "no index" value of a 16-bit operand or index.
const None = 0xFFFF

// NoShape is NEWA's shape when its elements have none (a 32-bit
// operand: 0xFFFFFFFF).
const NoShape = -1

// Field is a struct's field or an enum case's payload field. Its Name
// may carry its type's JSON descriptor after a NUL ("title\x00s"; see
// core/src/rt/json.h), which the runtime reads to decode and encode it.
type Field struct {
	Name  string
	Kind  Kind
	Shape int32 // the shape of a struct or enum value, -1 when none
}

// Case is a case of an enum with payloads.
type Case struct {
	Name   string
	Fields []Field
}

// ShapeDef is a struct's, a tuple's or an enum's layout.
type ShapeDef struct {
	Name    string
	Fields  []Field // structs, tuples
	Cases   []Case  // enums with payloads
	IsError bool
	// Methods maps a method's name to its function, for interface calls
	// (CALLI).
	Methods map[string]uint32
}

// Handler is a range of code an error is caught in.
type Handler struct {
	Start, End uint32 // [Start, End) in words
	Target     uint32
}

// Line maps code from PC on to a source line.
type Line struct {
	PC, Line uint32
}

// Inline is a function's body the optimizer copied into another's in
// place of a call to it (lang/ir's inlining): its code is [Start, End)
// in words, and an error thrown there, or a call made there, is in the
// function Func, called at line Line of the function it's in. Its
// instructions' lines are Func's own.
type Inline struct {
	Start, End uint32
	Func       uint32
	Line       uint32
}

// Local is a name of the source's a function holds in a register, for
// the debugger (core/src/vm/debug.c): it shows the name's value while the
// function is at a pc in [From, To) (in words). Only code the optimizer
// left alone has them (lang/ir drops them as it optimizes: its passes
// move values between registers), which is asuka dev's.
type Local struct {
	Name string
	Reg  uint16
	Via  uint8 // how the register holds the value: LocalValue, LocalBox, LocalSignal
	// Desc is the value's type's descriptor (core/src/rt/json.h's), with
	// a duration "d" and a color "c" (test.show's): how the debugger
	// writes it. "-": none (a closure's, a view's), shown by what the
	// value says of itself.
	Desc     string
	From, To uint32
}

// How a Local's register holds its value.
const (
	LocalValue  = iota // the value itself
	LocalBox           // a var closures capture: a box (VBOX) holding it
	LocalSignal        // a state's cell, a let's memo, a parameter's accessor: what it holds now
)

// Function flags.
const (
	FAsync  = 1 << iota // may suspend
	FThrows             // may throw
	// FMut: a mut fn, whose first parameter is inout: a call moves it in
	// (not retained) from the caller's argument register, and the frame
	// moves it back there as it ends, returned or thrown (RFC 0011).
	FMut
	// FSized: its code has values of a sized integer type (i8 ... u64)
	// or arrays of them, its own or a function's inlined in it: an array
	// it reads an int from may be packed (an [u8]'s elements a byte
	// each). Without it, every int element it reads or writes is an
	// 8-byte value (lang/cgen asks no array how it stores them). The VM
	// doesn't read it.
	FSized
)

// FuncDef is a function: its code and what the VM needs to run it.
type FuncDef struct {
	Name     string
	File     string
	NParams  uint16
	Kinds    []Kind // per register: its kind (refs own their value)
	CapKinds []Kind // a closure's captures
	Ret      Kind   // the result's kind
	Flags    uint8
	Code     []uint16
	Handlers []Handler
	Lines    []Line
	Inlines  []Inline // the functions inlined into it, none nested
	Locals   []Local  // its parameters' and locals' names, for the debugger
}

// Test is a test block, or a bench.
type Test struct {
	Name string
	Func uint32
	// Bench: a benchmark, whose function takes how many times to run
	// what it measures (an int).
	Bench bool
	// Params are a property test's parameters, which its function takes
	// in order: values the runner generates, of their descriptors' types
	// (core/src/rt/json.h).
	Params []Param
}

// Param is a property test's parameter: its name and its type's JSON
// descriptor.
type Param struct {
	Name, Desc string
}

// Module is a compiled program.
type Module struct {
	Ints     []int64
	Floats   []float64
	Strings  []string
	Shapes   []ShapeDef
	Funcs    []FuncDef
	Builtins []string // the names of the builtins CALLB calls, resolved by the VM as it loads
	Globals  []Kind
	Init     uint32 // runs first: module-level state, stores
	App      int32  // the app's root view function, -1 when none
	Main     int32  // a program's fn main, -1 when none
	Tests    []Test

	// Data, for a program, is the name of the folder its data goes in,
	// in the platform's place for an app's (std/fs's appDir): a
	// program an app ships ([bin] in its asuka.toml) shares the app's.
	// "": the program's own name. Not encoded: lang/cgen compiles it in.
	Data string

	// C is the C the module's extern declarations call (RFC 0008), nil
	// when it has none. It isn't encoded: the CLI compiles it into the
	// core's build, which the module's ffi.* builtins are then in.
	C *C
}

// C is a module's C: the glue lang/cffi generated for its extern
// declarations, and the projects whose asuka.toml [c] sections build
// what it calls.
type C struct {
	Glue []byte
	// Dirs are the directories of the projects (the app, the
	// dependencies) whose packages declare externs, the app's first.
	Dirs []string
	// Std are the directories of the std and sdk packages the program
	// imports that have C of their own (their asuka.toml's [c]: std/sqlite's
	// SQLite), which a build links.
	Std []string
}

// String interns s and returns its index (a linear search: a compiler
// keeps an index of its own, as lang/ir does).
func (m *Module) String(s string) uint32 {
	for i, x := range m.Strings {
		if x == s {
			return uint32(i)
		}
	}
	m.Strings = append(m.Strings, s)
	return uint32(len(m.Strings) - 1)
}

// Builtin returns the index of the builtin named name: a 16-bit
// operand, as the builtins are the runtime's, not the program's.
func (m *Module) Builtin(name string) uint16 {
	for i, x := range m.Builtins {
		if x == name {
			return uint16(i)
		}
	}
	if len(m.Builtins) >= None {
		panic("bytecode: more than 65535 builtins")
	}
	m.Builtins = append(m.Builtins, name)
	return uint16(len(m.Builtins) - 1)
}

// Encode writes the module in the binary format the VM reads:
// little-endian; strings as u32 length and bytes; lists as u32 count and
// items.
func (m *Module) Encode() []byte {
	w := &writer{}
	w.raw([]byte(Magic))
	w.u32(Version)
	w.u32(uint32(len(m.Ints)))
	for _, v := range m.Ints {
		w.u64(uint64(v))
	}
	w.u32(uint32(len(m.Floats)))
	for _, v := range m.Floats {
		w.u64(math.Float64bits(v))
	}
	w.u32(uint32(len(m.Strings)))
	for _, s := range m.Strings {
		w.str(s)
	}
	w.u32(uint32(len(m.Shapes)))
	for _, s := range m.Shapes {
		w.str(s.Name)
		w.fields(s.Fields)
		w.u32(uint32(len(s.Cases)))
		for _, c := range s.Cases {
			w.str(c.Name)
			w.fields(c.Fields)
		}
		if s.IsError {
			w.u8(1)
		} else {
			w.u8(0)
		}
		names := sortedKeys(s.Methods)
		w.u32(uint32(len(names)))
		for _, n := range names {
			w.str(n)
			w.u32(s.Methods[n])
		}
	}
	w.u32(uint32(len(m.Funcs)))
	for _, f := range m.Funcs {
		w.str(f.Name)
		w.str(f.File)
		w.u16(f.NParams)
		w.kinds(f.Kinds)
		w.kinds(f.CapKinds)
		w.u8(uint8(f.Ret))
		w.u8(f.Flags)
		w.u32(uint32(len(f.Code)))
		for _, c := range f.Code {
			w.u16(c)
		}
		w.u32(uint32(len(f.Handlers)))
		for _, h := range f.Handlers {
			w.u32(h.Start)
			w.u32(h.End)
			w.u32(h.Target)
		}
		w.u32(uint32(len(f.Lines)))
		for _, l := range f.Lines {
			w.u32(l.PC)
			w.u32(l.Line)
		}
		w.u32(uint32(len(f.Inlines)))
		for _, in := range f.Inlines {
			w.u32(in.Start)
			w.u32(in.End)
			w.u32(in.Func)
			w.u32(in.Line)
		}
		w.u32(uint32(len(f.Locals)))
		for _, l := range f.Locals {
			w.str(l.Name)
			w.u16(l.Reg)
			w.u8(l.Via)
			w.str(l.Desc)
			w.u32(l.From)
			w.u32(l.To)
		}
	}
	w.u32(uint32(len(m.Builtins)))
	for _, b := range m.Builtins {
		w.str(b)
	}
	w.kinds(m.Globals)
	w.u32(m.Init)
	w.u32(uint32(m.App))
	w.u32(uint32(m.Main))
	w.u32(uint32(len(m.Tests)))
	for _, t := range m.Tests {
		w.str(t.Name)
		w.u32(t.Func)
		var flags uint8
		if t.Bench {
			flags |= 1
		}
		w.u8(flags)
		w.u32(uint32(len(t.Params)))
		for _, p := range t.Params {
			w.str(p.Name)
			w.str(p.Desc)
		}
	}
	return w.b
}

// Decode reads a module Encode wrote.
func Decode(b []byte) (*Module, error) {
	r := &reader{b: b}
	if string(r.raw(4)) != Magic {
		return nil, errors.New("bytecode: not a module")
	}
	if v := r.u32(); v != Version {
		return nil, fmt.Errorf("bytecode: version %d, want %d", v, Version)
	}
	m := &Module{}
	for n := r.u32(); n > 0 && r.err == nil; n-- {
		m.Ints = append(m.Ints, int64(r.u64()))
	}
	for n := r.u32(); n > 0 && r.err == nil; n-- {
		m.Floats = append(m.Floats, math.Float64frombits(r.u64()))
	}
	for n := r.u32(); n > 0 && r.err == nil; n-- {
		m.Strings = append(m.Strings, r.str())
	}
	for n := r.u32(); n > 0 && r.err == nil; n-- {
		s := ShapeDef{Name: r.str(), Fields: r.fields()}
		for c := r.u32(); c > 0 && r.err == nil; c-- {
			s.Cases = append(s.Cases, Case{Name: r.str(), Fields: r.fields()})
		}
		s.IsError = r.u8() == 1
		if k := r.u32(); k > 0 {
			s.Methods = map[string]uint32{}
			for ; k > 0 && r.err == nil; k-- {
				name := r.str()
				s.Methods[name] = r.u32()
			}
		}
		m.Shapes = append(m.Shapes, s)
	}
	for n := r.u32(); n > 0 && r.err == nil; n-- {
		f := FuncDef{Name: r.str(), File: r.str(), NParams: r.u16(), Kinds: r.kinds(), CapKinds: r.kinds(), Ret: Kind(r.u8()), Flags: r.u8()}
		for c := r.u32(); c > 0 && r.err == nil; c-- {
			f.Code = append(f.Code, r.u16())
		}
		for c := r.u32(); c > 0 && r.err == nil; c-- {
			f.Handlers = append(f.Handlers, Handler{r.u32(), r.u32(), r.u32()})
		}
		for c := r.u32(); c > 0 && r.err == nil; c-- {
			f.Lines = append(f.Lines, Line{r.u32(), r.u32()})
		}
		for c := r.u32(); c > 0 && r.err == nil; c-- {
			f.Inlines = append(f.Inlines, Inline{r.u32(), r.u32(), r.u32(), r.u32()})
		}
		for c := r.u32(); c > 0 && r.err == nil; c-- {
			f.Locals = append(f.Locals, Local{Name: r.str(), Reg: r.u16(), Via: r.u8(), Desc: r.str(), From: r.u32(), To: r.u32()})
		}
		m.Funcs = append(m.Funcs, f)
	}
	for n := r.u32(); n > 0 && r.err == nil; n-- {
		m.Builtins = append(m.Builtins, r.str())
	}
	m.Globals = r.kinds()
	m.Init = r.u32()
	m.App = int32(r.u32())
	m.Main = int32(r.u32())
	for n := r.u32(); n > 0 && r.err == nil; n-- {
		t := Test{Name: r.str(), Func: r.u32()}
		t.Bench = r.u8()&1 != 0
		for k := r.u32(); k > 0 && r.err == nil; k-- {
			t.Params = append(t.Params, Param{r.str(), r.str()})
		}
		m.Tests = append(m.Tests, t)
	}
	if r.err != nil {
		return nil, r.err
	}
	return m, nil
}

type writer struct{ b []byte }

func (w *writer) raw(p []byte) { w.b = append(w.b, p...) }
func (w *writer) u8(v uint8)   { w.b = append(w.b, v) }
func (w *writer) u16(v uint16) { w.b = binary.LittleEndian.AppendUint16(w.b, v) }
func (w *writer) u32(v uint32) { w.b = binary.LittleEndian.AppendUint32(w.b, v) }
func (w *writer) u64(v uint64) { w.b = binary.LittleEndian.AppendUint64(w.b, v) }
func (w *writer) str(s string) { w.u32(uint32(len(s))); w.b = append(w.b, s...) }

func (w *writer) kinds(ks []Kind) {
	w.u32(uint32(len(ks)))
	for _, k := range ks {
		w.u8(uint8(k))
	}
}

func (w *writer) fields(fs []Field) {
	w.u32(uint32(len(fs)))
	for _, f := range fs {
		w.str(f.Name)
		w.u8(uint8(f.Kind))
		w.u32(uint32(f.Shape))
	}
}

type reader struct {
	b   []byte
	off int
	err error
}

func (r *reader) raw(n int) []byte {
	if r.err != nil || len(r.b)-r.off < n {
		r.err = errors.New("bytecode: truncated module")
		return make([]byte, n)
	}
	p := r.b[r.off : r.off+n]
	r.off += n
	return p
}

func (r *reader) u8() uint8   { return r.raw(1)[0] }
func (r *reader) u16() uint16 { return binary.LittleEndian.Uint16(r.raw(2)) }
func (r *reader) u32() uint32 { return binary.LittleEndian.Uint32(r.raw(4)) }
func (r *reader) u64() uint64 { return binary.LittleEndian.Uint64(r.raw(8)) }
func (r *reader) str() string { return string(r.raw(int(r.u32()))) }

func (r *reader) kinds() []Kind {
	n := r.u32()
	if r.err != nil || int(n) > len(r.b) {
		r.err = errors.New("bytecode: truncated module")
		return nil
	}
	if n == 0 {
		return nil
	}
	ks := make([]Kind, n)
	for i := range ks {
		ks[i] = Kind(r.u8())
	}
	return ks
}

func (r *reader) fields() []Field {
	n := r.u32()
	var fs []Field
	for ; n > 0 && r.err == nil; n-- {
		fs = append(fs, Field{Name: r.str(), Kind: Kind(r.u8()), Shape: int32(r.u32())})
	}
	return fs
}

func sortedKeys(m map[string]uint32) []string {
	ks := make([]string, 0, len(m))
	for k := range m {
		ks = append(ks, k)
	}
	for i := 1; i < len(ks); i++ {
		for j := i; j > 0 && ks[j] < ks[j-1]; j-- {
			ks[j], ks[j-1] = ks[j-1], ks[j]
		}
	}
	return ks
}
