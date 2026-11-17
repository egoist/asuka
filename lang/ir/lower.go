package ir

import (
	"bytes"
	"fmt"
	"hash/fnv"
	"strings"

	bc "github.com/egoist/asuka/lang/bytecode"
	"github.com/egoist/asuka/lang/check"
	"github.com/egoist/asuka/lang/syntax"
)

// A Lowerer lowers a checked program.
type Lowerer struct {
	prog *check.Program
	info *check.Info
	fset *syntax.FileSet
	out  *Program
	mod  *bc.Module

	funcs     map[string]int // a function's instantiation → its index
	shapes    map[string]int // a type → its shape
	shapeType map[int]check.Type
	globals   map[*check.Global]int
	envShape  map[string]int
	memberIdx map[*check.Member]int
	enumNames map[string]int // an int enum → the global holding its case names
	errIDs    map[int]int    // an error's shape → its type id
	work      []func()
	errs      []check.Diagnostic

	opts           *Options
	pendingStrings []pendingStrings
	consts         map[string]int // a hoisted literal (and its type) → its global
	pendingConsts  []pendingConst
	pendingNative  []pendingNative
	pendingExtern  []pendingExtern
	stringsFn      *fb
	catalog        int // the app's catalog's global, or -1
	// styleMemo: a style's local (check.StyleLocal) whose register, in
	// the application being lowered, holds a memo (style.go).
	styleMemo map[*check.Var]bool
	// variantIDs are the declared variants' ids (style.go's variantID).
	variantIDs map[*check.VariantDef]int64
}

// Options say how to lower.
type Options struct {
	// Accent and AccentFlags are the app's accent color (the wire's
	// Color, 0: the platform's) and its flags, which the app's init gives
	// the core (ui.set_accent).
	Accent      uint64
	AccentFlags uint8
	// Lang and Direction force the language and the direction the app
	// shows for this run (asuka dev's --lang and --dir): the init says so
	// first (i18n.force), so that every platform's player follows.
	Lang, Direction string
	// Version is the project's (asuka.toml's version): std/flag's
	// --version.
	Version string
	// Platform is the platform the program is compiled for: what
	// @platform is, which decides the branches it folds (fold.go).
	// "": target.Default.
	Platform string
}

// Lower lowers prog (checked without errors) to a program.
func Lower(prog *check.Program, opts *Options) (*Program, []check.Diagnostic) {
	if opts == nil {
		opts = &Options{}
	}
	l := &Lowerer{
		prog: prog, info: prog.Info, fset: prog.Fset,
		mod:       &bc.Module{App: -1, Main: -1},
		funcs:     map[string]int{},
		shapes:    map[string]int{},
		shapeType: map[int]check.Type{},
		globals:   map[*check.Global]int{},
		envShape:  map[string]int{},
		memberIdx: map[*check.Member]int{},
		enumNames: map[string]int{},
		errIDs:    map[int]int{},
		catalog:   -1,
	}
	l.out = &Program{Mod: l.mod}
	l.opts = opts
	l.lower()
	return l.out, l.errs
}

// errorf reports what the lowering can't do (yet) with the program.
func (l *Lowerer) errorf(n syntax.Node, format string, args ...any) {
	var pos syntax.Position
	if n != nil {
		pos = l.fset.Position(n.Range().Start)
	}
	l.errs = append(l.errs, check.Diagnostic{Pos: pos, End: pos, Msg: fmt.Sprintf(format, args...)})
}

func (l *Lowerer) lower() {
	// The init function first: globals in dependency order.
	init := l.newFunc("init", "", 0, bc.KVoid)
	l.mod.Init = uint32(init.idx)
	l.initBody(init)
	root := l.prog.Root
	if root != nil && root.App != nil {
		app := l.newFunc("app", l.fileOf(root.App), 0, bc.KRef)
		l.mod.App = int32(app.idx)
		app.ctx = l.info.Ctxs[root.App]
		app.lowerApp(root.App)
	}
	if root != nil {
		for _, t := range root.Tests {
			l.lowerTest(t)
		}
		if m := l.prog.Main; m != nil {
			l.mod.Main = int32(l.funcIndex(m, nil, nil))
		}
	}
	// What the constants and the native errors' table use (an error's
	// message method) is lowered in turn.
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
	l.finishStrings()
}

func (l *Lowerer) fileOf(n syntax.Node) string {
	if n == nil {
		return ""
	}
	if f := l.fset.File(n.Range().Start); f != nil {
		return f.Name()
	}
	return ""
}

func (l *Lowerer) line(n syntax.Node) int32 {
	if n == nil {
		return 0
	}
	return int32(l.fset.Position(n.Range().Start).Line)
}

// newFunc adds a function and returns its builder.
func (l *Lowerer) newFunc(name, file string, nparams int, ret bc.Kind) *fb {
	f := &Func{Name: name, File: file, NParams: nparams, Ret: ret}
	l.out.Funcs = append(l.out.Funcs, f)
	return &fb{l: l, f: f, idx: len(l.out.Funcs) - 1, vars: map[*check.Var]int{}, unboxed: map[*check.Var]bool{}, free: map[bc.Kind][]int{},
		blockFree: map[string][]int{}, memberRegs: map[*check.Member]int{}, envReg: -1, nullReg: -1}
}

// ---------------------------------------------------------------------
// Types

// kind is how values of t are stored.
func (l *Lowerer) kind(t check.Type) bc.Kind {
	switch x := t.(type) {
	case *check.Basic:
		switch x {
		case check.Int, check.Duration, check.Color, check.Length, check.Pointer:
			return bc.KInt
		case check.Float:
			return bc.KFloat
		case check.Bool:
			return bc.KBool
		case check.Void, check.Never:
			return bc.KVoid
		}
		return bc.KRef
	case *check.Named:
		if x.Obj.Kind == check.EnumType && !x.Obj.RefEnum() {
			return bc.KInt
		}
	}
	return bc.KRef
}

// typeKey is a concrete type's name, unique in the program.
func (l *Lowerer) typeKey(t check.Type) string {
	switch x := t.(type) {
	case *check.Named:
		var b strings.Builder
		if p := x.Obj.Pkg; p != nil && p.Path != "" && !p.Prelude {
			b.WriteString(p.Path)
			b.WriteByte('.')
		}
		b.WriteString(x.Obj.Name())
		if len(x.Args) > 0 {
			b.WriteByte('<')
			for i, a := range x.Args {
				if i > 0 {
					b.WriteByte(',')
				}
				b.WriteString(l.typeKey(a))
			}
			b.WriteByte('>')
		}
		return b.String()
	case *check.Array:
		return "[" + l.typeKey(x.Elem) + "]"
	case *check.Map:
		return "[" + l.typeKey(x.Key) + ":" + l.typeKey(x.Elem) + "]"
	case *check.Set:
		return "Set<" + l.typeKey(x.Elem) + ">"
	case *check.Optional:
		return l.typeKey(x.Elem) + "?"
	case *check.Tuple:
		var parts []string
		for _, e := range x.Elems {
			parts = append(parts, l.typeKey(e))
		}
		return "(" + strings.Join(parts, ",") + ")"
	}
	if t == nil {
		return "?"
	}
	return t.String()
}

// isErrorStruct reports a struct declared : Error, which is stored as an
// enum of one case (see shape).
func isErrorStruct(t check.Type) bool {
	n, ok := t.(*check.Named)
	return ok && n.Obj.Kind == check.StructType && n.Obj.IsError
}

// caseBase is the index of an enum's first case in its shape: errors
// have a case 0 no value has, so that an error's ECASE is never 0 (a
// runtime error, a struct, has 0) and catch can tell them apart.
func caseBase(t check.Type) int {
	if n, ok := t.(*check.Named); ok && n.Obj.IsError {
		return 1
	}
	return 0
}

// shape returns the shape of a struct, a tuple, an enum stored as a
// reference, or a store; -1 for other types.
func (l *Lowerer) shape(t check.Type) int {
	key := l.typeKey(t)
	if i, ok := l.shapes[key]; ok {
		return i
	}
	switch x := t.(type) {
	case *check.Tuple:
		i := l.addShape(key, t)
		var fs []bc.Field
		for j, e := range x.Elems {
			fs = append(fs, l.field(itoa(j), e))
		}
		l.mod.Shapes[i].Fields = fs
		return i
	case *check.Named:
		s := x.Subst()
		switch x.Obj.Kind {
		case check.StructType:
			i := l.addShape(key, t)
			var fs []bc.Field
			for _, f := range x.Obj.Fields {
				fs = append(fs, l.field(f.Name, s.Apply(f.Type)))
			}
			if x.Obj.IsError {
				l.mod.Shapes[i].Cases = []bc.Case{{Name: "_"}, {Name: x.Obj.Name(), Fields: fs}}
				l.mod.Shapes[i].IsError = true
				l.errorMethods(i, x)
			} else {
				l.mod.Shapes[i].Fields = fs
			}
			return i
		case check.EnumType:
			if !x.Obj.RefEnum() {
				return -1
			}
			i := l.addShape(key, t)
			var cs []bc.Case
			if x.Obj.IsError {
				cs = append(cs, bc.Case{Name: "_"})
			}
			for _, k := range x.Obj.Cases {
				c := bc.Case{Name: k.Name}
				for j, p := range k.Params {
					name := k.Labels[j]
					if name == "" {
						name = itoa(j)
					}
					c.Fields = append(c.Fields, l.field(name, s.Apply(p)))
				}
				cs = append(cs, c)
			}
			l.mod.Shapes[i].Cases = cs
			l.mod.Shapes[i].IsError = x.Obj.IsError
			if x.Obj.IsError {
				l.errorMethods(i, x)
			}
			return i
		case check.StoreType:
			return l.envShapeOf(x.Obj, x)
		}
	}
	return -1
}

func (l *Lowerer) addShape(key string, t check.Type) int {
	l.mod.Shapes = append(l.mod.Shapes, bc.ShapeDef{Name: key})
	i := len(l.mod.Shapes) - 1
	l.shapes[key] = i
	l.shapeType[i] = t
	return i
}

// field is a shape's field: its name, with its JSON descriptor after a
// NUL, its kind and its shape.
func (l *Lowerer) field(name string, t check.Type) bc.Field {
	return bc.Field{Name: name + "\x00" + l.desc(t), Kind: l.kind(t), Shape: int32(l.shape(check.Unopt(t)))}
}

// desc is t's JSON descriptor (core/src/rt/json.h).
func (l *Lowerer) desc(t check.Type) string {
	switch x := t.(type) {
	case *check.Basic:
		switch x {
		case check.Int, check.Duration, check.Color, check.Length, check.Pointer:
			return "i"
		case check.Float:
			return "f"
		case check.Bool:
			return "b"
		case check.String:
			return "s"
		case check.Bytes:
			return "y"
		}
		return "-"
	case *check.Optional:
		return "?" + l.desc(x.Elem)
	case *check.Array:
		return "[" + l.desc(x.Elem)
	case *check.Map:
		return "{" + l.desc(x.Elem)
	case *check.Set:
		return "<" + l.desc(x.Elem)
	case *check.Named:
		switch x.Obj.Kind {
		case check.StructType:
			if x.Obj.IsError {
				return "-"
			}
			l.shape(t)
			return "S" + l.typeKey(t) + ";"
		case check.EnumType:
			if x.Obj.RefEnum() {
				l.shape(t)
				return "E" + l.typeKey(t) + ";"
			}
			l.intEnumShape(x)
			return "e" + l.typeKey(t) + ";"
		}
	}
	return "-"
}

// intEnumShape is the shape JSON names a payloadless enum's values by.
func (l *Lowerer) intEnumShape(x *check.Named) int {
	key := l.typeKey(x)
	if i, ok := l.shapes[key]; ok {
		return i
	}
	i := l.addShape(key, x)
	for _, k := range x.Obj.Cases {
		l.mod.Shapes[i].Cases = append(l.mod.Shapes[i].Cases, bc.Case{Name: k.Name})
	}
	return i
}

// errorMethods gives an error's shape the methods catch and .message
// call through CALLI: __etype (its type's id) and message.
func (l *Lowerer) errorMethods(shape int, t *check.Named) {
	id := len(l.errIDs) + 1
	l.errIDs[shape] = id
	if l.mod.Shapes[shape].Methods == nil {
		l.mod.Shapes[shape].Methods = map[string]uint32{}
	}
	et := l.newFunc(l.typeKey(t)+".__etype", "", 1, bc.KInt)
	et.f.Kinds = []bc.Kind{bc.KRef}
	r := et.newReg(bc.KInt)
	et.emit(bc.Byname["KSMALL"], r, id)
	et.emit(op("RET"), r)
	l.mod.Shapes[shape].Methods["__etype"] = uint32(et.idx)
	l.mod.Shapes[shape].Methods["message"] = uint32(l.messageFunc(t))
}

// messageFunc is an error type's message: its message field, its
// message method, or its type's (and case's) name.
func (l *Lowerer) messageFunc(t *check.Named) int {
	key := "message|" + l.typeKey(t)
	if i, ok := l.funcs[key]; ok {
		return i
	}
	if m := t.Obj.Methods["message"]; m != nil && m.Sig != nil && len(m.Sig.Params) == 0 && m.Sig.Result == check.String {
		i := l.funcIndex(m, nil, t)
		l.funcs[key] = i
		return i
	}
	b := l.newFunc(l.typeKey(t)+".message", "", 1, bc.KRef)
	l.funcs[key] = b.idx
	b.f.Kinds = []bc.Kind{bc.KRef}
	out := b.newReg(bc.KRef)
	if t.Obj.Kind == check.StructType {
		if f := t.Obj.Field("message"); f != nil && f.Type == check.String {
			b.emit(op("GETP"), out, 0, f.Index)
			b.emit(op("RET"), out)
			return b.idx
		}
		b.emit(op("KSTR"), out, l.str(t.Obj.Name()))
		b.emit(op("RET"), out)
		return b.idx
	}
	// An enum: Type.case.
	c := b.newReg(bc.KInt)
	b.emit(op("ECASE"), c, 0)
	names := l.frozenStrings(func() []string {
		ns := []string{t.Obj.Name()}
		for _, k := range t.Obj.Cases {
			ns = append(ns, t.Obj.Name()+"."+k.Name)
		}
		return ns
	}(), "message."+l.typeKey(t))
	arr := b.newReg(bc.KRef)
	b.emit(op("GGET"), arr, names, int(bc.KRef))
	b.emit(op("IDX"), out, arr, c)
	b.emit(op("RET"), out)
	return b.idx
}

// frozenStrings is a global holding an array of strings, built in init
// and frozen.
func (l *Lowerer) frozenStrings(ss []string, key string) int {
	if g, ok := l.enumNames[key]; ok {
		return g
	}
	g := len(l.mod.Globals)
	l.mod.Globals = append(l.mod.Globals, bc.KRef)
	l.enumNames[key] = g
	l.pendingStrings = append(l.pendingStrings, pendingStrings{g, ss})
	return g
}

type pendingConst struct {
	global int
	e      syntax.Expr
	subst  check.Subst
}

type pendingStrings struct {
	global int
	ss     []string
}

func (l *Lowerer) str(s string) int        { return int(l.mod.String(s)) }
func (l *Lowerer) builtin(name string) int { return int(l.mod.Builtin(name)) }

func op(name string) bc.Op {
	o, ok := bc.Byname[name]
	if !ok {
		panic("ir: no opcode " + name)
	}
	return o
}

// ---------------------------------------------------------------------
// Functions

// funcKey names an instantiation of f.
func (l *Lowerer) funcKey(f *check.Function, targs []check.Type, recv check.Type) string {
	var b strings.Builder
	fmt.Fprintf(&b, "%p", f)
	if recv != nil {
		b.WriteString("|" + l.typeKey(recv))
	}
	for _, t := range targs {
		b.WriteString("|" + l.typeKey(t))
	}
	return b.String()
}

// funcIndex returns the index of f's instantiation with targs (and its
// receiver's type), lowering it once.
func (l *Lowerer) funcIndex(f *check.Function, targs []check.Type, recv check.Type) int {
	key := l.funcKey(f, targs, recv)
	if i, ok := l.funcs[key]; ok {
		return i
	}
	name := f.Name()
	if f.Recv != nil {
		if recv != nil {
			name = l.typeKey(recv) + "." + name
		} else {
			name = f.Recv.Name() + "." + name
		}
	}
	if len(targs) > 0 {
		var ts []string
		for _, t := range targs {
			ts = append(ts, l.typeKey(t))
		}
		name += "<" + strings.Join(ts, ",") + ">"
	}
	s := check.Subst{}
	if n, ok := recv.(*check.Named); ok {
		for k, v := range n.Subst() {
			s[k] = v
		}
	}
	for i, tp := range f.TypeParams {
		if i < len(targs) {
			s[tp] = targs[i]
		}
	}
	nparams := len(f.Sig.Params)
	if f.Recv != nil {
		nparams++
	}
	b := l.newFunc(name, l.fileOf(f.Decl), nparams, l.kind(s.Apply(f.Sig.Result)))
	b.subst = s
	l.funcs[key] = b.idx
	l.work = append(l.work, func() { b.lowerFunc(f, recv) })
	return b.idx
}

// ---------------------------------------------------------------------
// Globals and init

// global returns a package-level value's slot.
func (l *Lowerer) global(g *check.Global) int {
	if i, ok := l.globals[g]; ok {
		return i
	}
	i := len(l.mod.Globals)
	k := bc.KRef
	if g.Kind == check.ConstGlobal {
		k = l.kind(g.Type)
	}
	l.mod.Globals = append(l.mod.Globals, k)
	l.globals[g] = i
	return i
}

// initBody makes the module's init: each package's state, memos and
// constants, dependencies first.
func (l *Lowerer) initBody(b *fb) {
	// The case-name tables the lowering asks for as it goes are made by
	// a function init calls first; its body is written at the end.
	l.stringsFn = l.newFunc("init.strings", "", 0, bc.KVoid)
	// The catalog first: what init's memos say may read the language.
	if l.opts.Lang != "" || l.opts.Direction != "" {
		mark := b.mark()
		b.callBTo("i18n.force", bc.None, b.kstr(l.opts.Lang), b.kstr(l.opts.Direction))
		b.release(mark)
	}
	l.initCatalog(b)
	b.emit(op("CALL"), bc.None, l.stringsFn.idx, 0, 0)
	done := map[*check.Global]bool{}
	var visit func(g *check.Global)
	// In a spawn's worker (the web's: an instance of the core that runs
	// spawn's work and nothing else), only the file's values a spawn can
	// use are made: not state, memos, or what isn't a value (a database, a
	// native object), which the checker keeps out of spawn, nor what's
	// made from them. A file's database isn't opened again in each worker.
	mainOnly := map[*check.Global]bool{}
	var isMainOnly func(g *check.Global) bool
	isMainOnly = func(g *check.Global) bool {
		if v, ok := mainOnly[g]; ok {
			return v
		}
		mainOnly[g] = false // a cycle is the checker's error
		v := g.Kind != check.ConstGlobal || !check.Sendable(g.Type)
		for _, d := range g.Deps() {
			v = v || isMainOnly(d)
		}
		mainOnly[g] = v
		return v
	}
	worker := -1
	visit = func(g *check.Global) {
		if done[g] {
			return
		}
		done[g] = true
		for _, d := range g.Deps() {
			visit(d)
		}
		if g.Value == nil || !isMainOnly(g) {
			l.initGlobal(b, g)
			return
		}
		if worker < 0 {
			worker = b.local(bc.KBool)
			b.callBTo("async.in_worker", worker)
		}
		skip := b.f.NewLabel()
		b.emitJump(op("JT"), worker, skip)
		l.initGlobal(b, g)
		b.f.Place(skip)
	}
	// An app sets its accent first, always (0 when it has none), so a
	// player that ran another app drops that one's.
	if root := l.prog.Root; root != nil && root.App != nil {
		c, f := b.konst(int64(l.opts.Accent)), b.konst(int64(l.opts.AccentFlags))
		b.callB("ui.set_accent", bc.KVoid, c, f)
	}
	for _, p := range l.prog.Packages {
		for _, g := range p.Globals {
			visit(g)
		}
	}
	b.emit(op("RET0"))
}

// finishStrings writes init.strings: the frozen string tables (enums'
// case names) the lowering asked for.
// finishConsts makes the hoisted literals, in init.strings.
func (l *Lowerer) finishConsts() {
	b := l.stringsFn
	for len(l.pendingConsts) > 0 {
		p := l.pendingConsts[0]
		l.pendingConsts = l.pendingConsts[1:]
		mark := b.mark()
		b.subst = p.subst
		b.hoisting = true
		v := b.expr(p.e, -1)
		b.hoisting = false
		v = b.freeze(v, bc.KRef)
		b.emit(op("GSET"), p.global, v, int(bc.KRef))
		b.release(mark)
	}
}

func (l *Lowerer) finishStrings() {
	b := l.stringsFn
	for _, p := range l.pendingStrings {
		mark := b.mark()
		blk := b.regs(repeatKind(bc.KRef, len(p.ss)))
		for i, s := range p.ss {
			b.emit(op("KSTR"), blk+i, l.str(s))
		}
		arr := b.temp(bc.KRef)
		b.emit(op("NEWA"), arr, int(bc.KRef), bc.None, blk, len(p.ss))
		b.releaseRegs(blk, repeatKind(bc.KRef, len(p.ss)))
		arr = b.freeze(arr, bc.KRef)
		b.emit(op("GSET"), p.global, arr, int(bc.KRef))
		b.release(mark)
	}
	b.emit(op("RET0"))
}

func repeatKind(k bc.Kind, n int) []bc.Kind {
	ks := make([]bc.Kind, n)
	for i := range ks {
		ks[i] = k
	}
	return ks
}

func (l *Lowerer) initGlobal(b *fb, g *check.Global) {
	if g.Value == nil {
		return
	}
	slot := l.global(g)
	b.ctx = g.Ctx
	b.line = l.line(g.Value)
	mark := b.mark()
	switch g.Kind {
	case check.StateGlobal:
		v := b.value(g.Value, -1)
		k := l.kind(g.Type)
		cell := b.temp(bc.KRef)
		b.emitKind(op("CELLK"), cell, b.valueReg(v, k), int(k), l.str(l.stateKey(g)))
		b.emit(op("GSET"), slot, cell, int(bc.KRef))
	case check.MemoGlobal:
		clos := b.thunk(g.Ctx, g.Value, g.Type)
		m := b.temp(bc.KRef)
		b.emit(op("MEMO"), m, clos, int(l.kind(g.Type)))
		b.emit(op("GSET"), slot, m, int(bc.KRef))
	default:
		k := l.kind(g.Type)
		v := b.valueReg(b.value(g.Value, -1), k)
		if k == bc.KRef && !isObject(g.Type) {
			v = b.freeze(v, k)
		}
		b.emit(op("GSET"), slot, v, int(k))
	}
	b.release(mark)
}

func (l *Lowerer) stateKey(g *check.Global) string {
	p := g.Pkg.Path
	if p == "" {
		p = "main"
	}
	return p + "." + g.Name() + initKey(g.Value)
}

// initKey is what a state's key (CELLK's) says of its initial value: a
// hash of its expression as the formatter prints it ("#1a2b3c4d"), so
// that a reload starts a state whose initializer was edited again from
// the new one (React Fast Refresh does likewise for useState's), and
// keeps one whose initializer was only laid out differently.
func initKey(e syntax.Expr) string {
	if e == nil {
		return ""
	}
	var b bytes.Buffer
	syntax.Fprint(&b, nil, e)
	h := fnv.New32a()
	h.Write(b.Bytes())
	return fmt.Sprintf("#%08x", h.Sum32())
}

// ---------------------------------------------------------------------
// Tests

func (l *Lowerer) lowerTest(t *check.Test) {
	b := l.newFunc("test "+t.Name, l.fileOf(t.Decl), 0, bc.KVoid)
	b.ctx = t.Ctx
	l.mod.Tests = append(l.mod.Tests, bc.Test{Name: t.Name, Func: uint32(b.idx)})
	l.work = append(l.work, func() {
		b.f.Flags = bc.FThrows
		if t.Ctx.Awaits {
			b.f.Flags |= bc.FAsync
		}
		// A user error that leaves the test fails it with its message:
		// rethrown as a runtime error, whose message the runner reads.
		start, end, target := b.f.NewLabel(), b.f.NewLabel(), b.f.NewLabel()
		b.f.Place(start)
		b.line = l.line(t.Decl)
		b.stmts(t.Decl.Body.Items)
		b.f.Place(end)
		b.emit(op("RET0"))
		b.f.Place(target)
		e := b.newReg(bc.KRef)
		b.emit(op("CATCH"), e)
		user := b.f.NewLabel()
		c := b.newReg(bc.KInt)
		b.emit(op("ECASE"), c, e)
		b.emitJump(op("JT"), c, user)
		b.emit(op("THROW"), e)
		b.f.Place(user)
		m := b.newReg(bc.KRef)
		b.emit(op("CALLI"), m, l.str("message"), e, 1)
		blk := b.newReg(bc.KRef)
		b.emit(op("MOVR"), blk, m)
		b.emit(op("CALLB"), e, l.builtin("error"), blk, 1)
		b.emit(op("THROW"), e)
		b.f.Handlers = append(b.f.Handlers, Handler{start, end, target})
	})
}

// implement fills the method table of t's shape with what interface
// iface calls through CALLI: its methods, and getters of its fields.
func (l *Lowerer) implement(t, iface check.Type) {
	n, ok := t.(*check.Named)
	in, ok2 := iface.(*check.Named)
	if !ok || !ok2 || in.Obj.Iface == nil {
		return
	}
	shape := l.shape(n)
	if shape < 0 {
		return
	}
	sd := &l.mod.Shapes[shape]
	if sd.Methods == nil {
		sd.Methods = map[string]uint32{}
	}
	for _, m := range in.Obj.Iface.Methods {
		if _, done := sd.Methods[m.Name]; done {
			continue
		}
		if f := n.Obj.Methods[m.Name]; f != nil {
			sd.Methods[m.Name] = uint32(l.funcIndex(f, nil, n))
		}
	}
	for _, fl := range in.Obj.Iface.Fields {
		if _, done := sd.Methods[fl.Name]; done {
			continue
		}
		if f := n.Obj.Methods[fl.Name]; f != nil && n.Obj.Kind == check.EnumType {
			sd.Methods[fl.Name] = uint32(l.funcIndex(f, nil, n))
			continue
		}
		f := n.Obj.Field(fl.Name)
		if f == nil {
			continue
		}
		ft := n.Subst().Apply(f.Type)
		g := l.newFunc(l.typeKey(n)+"."+fl.Name, "", 1, l.kind(ft))
		g.f.Kinds = []bc.Kind{bc.KRef}
		r := g.getField(0, n, f, -1)
		g.emit(op("RET"), r)
		sd.Methods[fl.Name] = uint32(g.idx)
	}
}

// hoist returns a constant literal (an array, a map, a tuple, a struct or
// a case made of literals) from the global init builds it in once,
// frozen; ok false when e isn't one.
func (b *fb) hoist(e syntax.Expr, dst int) (int, bool) {
	l := b.l
	if b.hoisting || b == l.stringsFn || b.idx == int(l.mod.Init) || !l.constLiteral(e, true) {
		return 0, false
	}
	t := b.rawType(e)
	if check.HasTypeParams(t) {
		return 0, false
	}
	key := fmt.Sprintf("%p|%s", e, l.typeKey(t))
	if l.consts == nil {
		l.consts = map[string]int{}
	}
	g, ok := l.consts[key]
	if !ok {
		g = len(l.mod.Globals)
		l.mod.Globals = append(l.mod.Globals, bc.KRef)
		l.consts[key] = g
		l.pendingConsts = append(l.pendingConsts, pendingConst{g, e, b.subst})
	}
	r := b.out(dst, bc.KRef)
	b.emit(op("GGET"), r, g, int(bc.KRef))
	return r, true
}

// constLiteral reports a value made of literals only (top: a container
// of them, which is worth hoisting).
func (l *Lowerer) constLiteral(e syntax.Expr, top bool) bool {
	switch x := e.(type) {
	case *syntax.ParenExpr:
		return l.constLiteral(x.X, top)
	case *syntax.BasicLit, *syntax.BoolLit, *syntax.NoneLit:
		return !top
	case *syntax.StringLit:
		return !top && len(x.Interps) == 0
	case *syntax.UnaryExpr:
		return !top && x.Op == syntax.SUB && l.constLiteral(x.X, false)
	case *syntax.ImplicitMember, *syntax.MemberExpr:
		if top {
			return false
		}
		sel := l.info.Selections[e]
		return sel != nil && (sel.Kind == check.SelCase || sel.Kind == check.SelColor || sel.Kind == check.SelLengthAuto)
	case *syntax.ArrayExpr:
		if len(x.Elems) == 0 {
			return false
		}
		for _, el := range x.Elems {
			if !l.constLiteral(el, false) {
				return false
			}
		}
		return true
	case *syntax.MapExpr:
		if len(x.Entries) == 0 {
			return false
		}
		for _, en := range x.Entries {
			if !l.constLiteral(en.Key, false) || !l.constLiteral(en.Value, false) {
				return false
			}
		}
		return true
	case *syntax.TupleExpr:
		for _, el := range x.Elems {
			if !l.constLiteral(el, false) {
				return false
			}
		}
		return len(x.Elems) > 0
	case *syntax.CallExpr:
		call := l.info.Calls[x]
		if call == nil || call.Kind != check.CallStruct && call.Kind != check.CallCase {
			return false
		}
		if n, ok := call.Type.Obj.Type.(*check.Named); ok && n.Obj.IsError {
			return false
		}
		for i, a := range call.Args {
			if a == nil {
				if d := call.Params[i].Default; d != nil && !l.constLiteral(d, false) {
					return false
				}
				continue
			}
			if !l.constLiteral(a, false) {
				return false
			}
		}
		return len(call.Args) > 0
	}
	return false
}

// isObject reports a type whose values are the runtime's objects, not
// values: a Ref (a cell), a Path (a builder), a Graphics. A constant of
// one isn't frozen: it changes.
func isObject(t check.Type) bool {
	switch check.Unopt(t) {
	case check.Ref, check.Path, check.Graphics:
		return true
	}
	return false
}
