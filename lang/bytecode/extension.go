package bytecode

import (
	"errors"
	"fmt"
	"hash/fnv"
	"math"
)

// ExtMagic starts every extension.
const ExtMagic = "AKBX"

// Hash is a module's identity as a player that loaded it knows it: the
// 64-bit FNV-1a of its encoding (core/src/vm/load.c computes the same as
// it loads one, in a development build). An Extension names its base by
// it.
func Hash(encoded []byte) uint64 {
	h := fnv.New64a()
	h.Write(encoded)
	return h.Sum64()
}

// An Extension is what's been compiled against a module since it was
// sent: asuka dev's debugger's expressions (a breakpoint's condition, a
// log message, what an editor evaluates), each a function, and what they
// need that the module doesn't have (functions, types, constants,
// builtins, globals). Its tables continue the module's: an index the
// module has is the module's, the ones after it the extension's. A
// player builds a module of the two (core/src/vm/load.c's
// ak_module_extend) to run them, sharing the base's globals and types.
type Extension struct {
	Base uint64 // the base module's Hash
	// The base's sizes: where each table's part here starts.
	NInts, NFloats, NStrings, NShapes, NFuncs, NBuiltins, NGlobals uint32

	Ints     []int64
	Floats   []float64
	Strings  []string
	Shapes   []ShapeDef
	Funcs    []FuncDef
	Builtins []string
	Globals  []Kind
	// Inits make the globals the extension adds: each a function run
	// once, by the first module built with it, after the globals from
	// First on (the hoisted constants of an expression) are there.
	Inits []ExtInit
}

// ExtInit is an Extension's init: Func makes the globals from First on
// that its expression added.
type ExtInit struct {
	First, Func uint32
}

// Encode writes the extension: ExtMagic, Version, then as Module's
// encoding writes its parts.
func (x *Extension) Encode() []byte {
	w := &writer{}
	w.raw([]byte(ExtMagic))
	w.u32(Version)
	w.u64(x.Base)
	for _, n := range []uint32{x.NInts, x.NFloats, x.NStrings, x.NShapes, x.NFuncs, x.NBuiltins, x.NGlobals} {
		w.u32(n)
	}
	w.u32(uint32(len(x.Ints)))
	for _, v := range x.Ints {
		w.u64(uint64(v))
	}
	w.u32(uint32(len(x.Floats)))
	for _, v := range x.Floats {
		w.u64(math.Float64bits(v))
	}
	w.u32(uint32(len(x.Strings)))
	for _, s := range x.Strings {
		w.str(s)
	}
	w.u32(uint32(len(x.Shapes)))
	for i := range x.Shapes {
		w.shape(&x.Shapes[i])
	}
	w.u32(uint32(len(x.Funcs)))
	for i := range x.Funcs {
		w.funcDef(&x.Funcs[i])
	}
	w.u32(uint32(len(x.Builtins)))
	for _, b := range x.Builtins {
		w.str(b)
	}
	w.kinds(x.Globals)
	w.u32(uint32(len(x.Inits)))
	for _, in := range x.Inits {
		w.u32(in.First)
		w.u32(in.Func)
	}
	return w.b
}

// DecodeExtension reads an extension Encode wrote.
func DecodeExtension(b []byte) (*Extension, error) {
	r := &reader{b: b}
	if string(r.raw(4)) != ExtMagic {
		return nil, errors.New("bytecode: not an extension")
	}
	if v := r.u32(); v != Version {
		return nil, fmt.Errorf("bytecode: version %d, want %d", v, Version)
	}
	x := &Extension{Base: r.u64()}
	for _, n := range []*uint32{&x.NInts, &x.NFloats, &x.NStrings, &x.NShapes, &x.NFuncs, &x.NBuiltins, &x.NGlobals} {
		*n = r.u32()
	}
	for n := r.u32(); n > 0 && r.err == nil; n-- {
		x.Ints = append(x.Ints, int64(r.u64()))
	}
	for n := r.u32(); n > 0 && r.err == nil; n-- {
		x.Floats = append(x.Floats, math.Float64frombits(r.u64()))
	}
	for n := r.u32(); n > 0 && r.err == nil; n-- {
		x.Strings = append(x.Strings, r.str())
	}
	for n := r.u32(); n > 0 && r.err == nil; n-- {
		x.Shapes = append(x.Shapes, r.shape())
	}
	for n := r.u32(); n > 0 && r.err == nil; n-- {
		x.Funcs = append(x.Funcs, r.funcDef())
	}
	for n := r.u32(); n > 0 && r.err == nil; n-- {
		x.Builtins = append(x.Builtins, r.str())
	}
	x.Globals = r.kinds()
	for n := r.u32(); n > 0 && r.err == nil; n-- {
		x.Inits = append(x.Inits, ExtInit{r.u32(), r.u32()})
	}
	if r.err != nil {
		return nil, r.err
	}
	return x, nil
}
