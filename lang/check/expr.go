package check

import (
	"errors"
	"fmt"
	"math"
	"strconv"
	"strings"

	"github.com/egoist/asuka/lang/diag"
	"github.com/egoist/asuka/lang/syntax"
)

// selfMember is a field or method of a method's receiver, in scope by
// its bare name.
type selfMember struct {
	object
	field  *Field
	method *Function
	owner  *TypeName
	// subst: the type's parameters, an extension's arguments for them
	// (extend<T> Box<T>: Box's T is the extension's).
	subst Subst
}

func (c *Checker) record(e syntax.Expr, t Type) Type {
	if t == nil {
		t = Invalid
	}
	c.info.Types[e] = t
	return t
}

// expr checks e, expecting a value of type want (nil: anything), and
// returns its type. Expectations steer literals (1 where a float is
// expected is a float), implicit members (.gray) and lambdas' parameter
// types; whether the value fits is assign's to say.
func (c *Checker) expr(e syntax.Expr, want Type) Type {
	return c.record(e, c.expr1(e, want))
}

func (c *Checker) expr1(e syntax.Expr, want Type) Type {
	switch e := e.(type) {
	case nil:
		return Invalid
	case *syntax.BadExpr:
		return Invalid
	case *syntax.Ident:
		return c.ident(e)
	case *syntax.BasicLit:
		return c.basicLit(e, want)
	case *syntax.StringLit:
		if _, err := Unescape(strings.Join(e.Segments, ""), e.Raw); err != nil {
			c.errorf(e, diag.InvalidLiteral, "%v", err)
		}
		for _, x := range e.Interps {
			t := c.expr(x, nil)
			c.interpolated(x, t)
		}
		return String
	case *syntax.BoolLit:
		return Bool
	case *syntax.NoneLit:
		if want != nil && IsOptional(want) {
			return want
		}
		return NoneType
	case *syntax.SelfExpr:
		if c.ctx == nil || c.ctx.Self == nil {
			// In a store: the store itself (its env).
			for cc := c.ctx; cc != nil; cc = cc.Parent {
				if cc.Comp != nil && cc.Comp.Kind == StoreType {
					c.envUse(cc.Comp, e, "self")
					c.info.StoreSelf[e] = cc.Comp
					return cc.Comp.Type
				}
			}
			if st := c.enclosingStatic(); st != nil {
				c.errorf(e, diag.UndefinedName, "a static fn has no self: it's %s's, not a value's; take one as a parameter, or make it a method", st.name)
				return Invalid
			}
			c.errorf(e, diag.UndefinedName, "self is only in a struct's, an enum's or a store's methods")
			return Invalid
		}
		c.useVar(c.ctx.Self, e)
		return c.ctx.Self.Type
	case *syntax.AtExpr:
		return c.atExpr(e)
	case *syntax.ImplicitMember:
		return c.implicitMember(e, want)
	case *syntax.MemberExpr:
		return c.member(e, want)
	case *syntax.CallExpr:
		return c.call(e, want)
	case *syntax.IndexExpr:
		return c.index(e)
	case *syntax.UnwrapExpr:
		// x! where a T is expected: x is a T? (.parse("19.99")! where a
		// Decimal is: Decimal.parse).
		t := c.expr(e.X, MakeOptionalOrNil(want))
		if IsInvalid(t) {
			return Invalid
		}
		if !IsOptional(t) {
			c.errorf(e, diag.NotOptional, "%s isn't optional: nothing to unwrap with !", typeString(t))
			return t
		}
		return Unopt(t)
	case *syntax.UnaryExpr:
		return c.unary(e, want)
	case *syntax.SpawnExpr:
		return c.spawn(e)
	case *syntax.BinaryExpr:
		return c.binary(e, want)
	case *syntax.LambdaExpr:
		return c.lambda(e, want)
	case *syntax.ParenExpr:
		return c.expr(e.X, want)
	case *syntax.TupleExpr:
		if len(e.Elems) == 0 {
			c.errorf(e, diag.NoValue, "() isn't a value")
			return Invalid
		}
		var wt *Tuple
		if w, ok := Unopt(want).(*Tuple); ok && len(w.Elems) == len(e.Elems) {
			wt = w
		}
		var es []Type
		for i, x := range e.Elems {
			if wt != nil {
				es = append(es, c.assign(x, wt.Elems[i], "a tuple's element"))
			} else {
				es = append(es, c.valueType(c.expr(x, nil), x))
			}
		}
		return &Tuple{es}
	case *syntax.ArrayExpr:
		return c.arrayLit(e, want)
	case *syntax.MapExpr:
		return c.mapLit(e, want)
	case *syntax.IfExpr:
		return c.ifExpr(e, want)
	case *syntax.MatchExpr:
		return c.matchExpr(e, want)
	case *syntax.LetCond:
		c.errorf(e, diag.MisplacedStatement, "let here is only an if's or a while's condition")
		return Invalid
	case *syntax.CondList:
		c.errorf(e, diag.MisplacedStatement, "a condition's clauses go in an if's or a while's head: write && between bools")
		return Invalid
	case *syntax.JumpExpr:
		return c.jumpExpr(e)
	case *syntax.CatchExpr:
		return c.catchExpr(e, want)
	}
	c.errorf(e, diag.InternalError, "unexpected expression")
	return Invalid
}

// interpolated checks a value put in a string.
func (c *Checker) interpolated(x syntax.Expr, t Type) {
	switch u := Unopt(t).(type) {
	case *Basic:
		switch u {
		case Void:
			c.errorf(x, diag.NoValue, "this has no value to put in a string")
		case View, TestApp:
			c.errorf(x, diag.TypeMismatch, "%s can't be put in a string", aType(u))
		}
	case *FuncType, *Task, *Stream, *TypeParam:
		c.errorf(x, diag.TypeMismatch, "%s can't be put in a string", aType(t))
	case *Named:
		if u.Obj.Kind == StoreType || u.Obj.Kind == ComponentType {
			c.errorf(x, diag.TypeMismatch, "%s can't be put in a string", aType(t))
		}
	}
}

// ---------------------------------------------------------------------
// Names

func (c *Checker) ident(e *syntax.Ident) Type {
	if e.Name == "_" {
		c.errorf(e, diag.NoValue, "_ isn't a value")
		return Invalid
	}
	if x := c.implicitSelf(e, false); x != nil {
		c.info.ImplicitSelf[e] = x
		return c.expr(x, nil)
	}
	o := c.lookup(e.Name)
	if o == nil && e.Name == "shader" {
		c.errorf(e, diag.InvalidShader, shaderMoved)
		return Invalid
	}
	if o == nil {
		c.unknownName(e)
		return Invalid
	}
	c.info.Uses[e] = o
	switch o := o.(type) {
	case *Var:
		c.useVar(o, e)
		if o.Kind == LocalFnVar {
			c.localUses = append(c.localUses, localUse{ctx: c.ctx, fn: o.Fn})
		}
		return o.Type
	case *Global:
		return c.useGlobal(o, e)
	case *Member:
		return c.useMember(o, e)
	case *selfMember:
		if c.ctx == nil || c.ctx.Self == nil {
			c.errorf(e, diag.NotAValue, "%s is a member of %s: use it in its methods", e.Name, o.owner.name)
			return Invalid
		}
		c.useVar(c.ctx.Self, e)
		if o.field != nil {
			return o.subst.Apply(o.field.Type)
		}
		c.errorf(e, diag.NotAValue, "%s is a method: call it, %s()", e.Name, e.Name)
		return Invalid
	case *Function:
		c.funcSig(o)
		if len(o.TypeParams) > 0 {
			c.errorf(e, diag.CannotInferType, "%s is generic: call it, or pass it where its types are known", e.Name)
			return Invalid
		}
		c.noteCall(o, e)
		return o.Sig.Type()
	case *TypeName:
		c.errorf(e, diag.NotAValue, "%s is a type, not a value%s", e.Name, typeValueHint(o))
		return Invalid
	case *Builtin:
		c.errorf(e, diag.NotAValue, "%s is a built-in function: call it, %s(…)", e.Name, e.Name)
		return Invalid
	case *PkgName:
		c.errorf(e, diag.NotAValue, "%s is a package: use its members, %s.name", e.Name, e.Name)
		return Invalid
	case *TrName:
		c.errorf(e, diag.NotAValue, "tr says the app's messages: call one, tr.key(…)")
		return Invalid
	}
	return Invalid
}

func typeValueHint(t *TypeName) string {
	switch t.Kind {
	case StructType, StoreType:
		return " (make one: " + t.name + "(…))"
	case EnumType:
		return " (a case is " + t.name + ".name)"
	}
	return ""
}

// unknownName reports a name that isn't in scope, saying why when it's
// a field's or a method's where there's no self to have it.
func (c *Checker) unknownName(e *syntax.Ident) {
	if st := c.enclosingStatic(); st != nil && (st.Field(e.Name) != nil || st.Methods[e.Name] != nil) {
		c.errorf(e, diag.UndefinedName, "%s is %s's, and a static fn has no self: take %s as a parameter, or make it a method", e.Name, st.name, aType(st.Type))
		return
	}
	if d := c.defaultOf; d != nil && d.fieldIndex != nil {
		if _, ok := d.fieldIndex[e.Name]; ok {
			c.errorf(e, diag.InvalidDeclaration, "a default can't read the struct's fields (%s): it's computed before there's a struct; a value made of others is a method, fn name() -> T = …", e.Name)
			return
		}
	}
	if m, ok := movedFuncs[e.Name]; ok {
		if m.pkg == "window" {
			c.errorf(e, diag.UndefinedName, "unknown name %s: it's window.%s() now", e.Name, m.name)
		} else {
			c.errorf(e, diag.UndefinedName, "unknown name %s: it's platform/%s's now, %s.%s(…) (import platform/%s)", e.Name, m.pkg, m.pkg, m.name, m.pkg)
		}
		return
	}
	c.errorf(e, diag.UndefinedName, "unknown name %s%s", e.Name, c.suggest(e.Name))
}

// enclosingStatic is the type whose static fn is being checked, or nil.
func (c *Checker) enclosingStatic() *TypeName {
	for cc := c.ctx; cc != nil; cc = cc.Parent {
		if cc.Func != nil {
			return cc.Func.Static
		}
	}
	return nil
}

// useVar notes a use of a local: from a closure, it's a capture.
func (c *Checker) useVar(v *Var, at syntax.Node) {
	for cc := c.ctx; cc != nil && cc != v.Ctx; cc = cc.Parent {
		v.Captured = true
		cc.capture(v)
		if cc.Kind == CtxSpawn && !sendable(v.Type) {
			c.errorf(at, diag.SpawnCapturesState, "spawn can't use %s (%s): it runs on another thread and takes values only", v.name, aType(v.Type))
		}
	}
}

func (c *Checker) useGlobal(g *Global, at syntax.Node) Type {
	t := c.globalType(g)
	if c.curGlobal != nil && !contains(c.curGlobal.deps, g) && g != c.curGlobal {
		c.curGlobal.deps = append(c.curGlobal.deps, g)
	}
	if c.ctx != nil {
		if g.Kind == StateGlobal || g.Kind == MemoGlobal || g.Ctx != nil && g.Ctx.readsState {
			c.ctx.readsState = true
			c.spawnState(at, g.name)
		} else if !IsInvalid(t) && !sendable(t) {
			c.spawnNotValue(at, g.name, t)
		}
		c.ctx.globals = append(c.ctx.globals, g)
		c.noteRead(g, at)
	}
	return t
}

// spawnState reports a read of state inside spawn.
func (c *Checker) spawnState(at syntax.Node, name string) {
	if w := c.workerOf(); w != nil {
		c.errorf(at, diag.WorkerCapturesState, "worker %s can't use %s: it's state, and the worker runs on a thread of its own (read it on the main thread and pass its value to a pub fn)", w.name, name)
		return
	}
	for cc := c.ctx; cc != nil; cc = cc.Parent {
		if cc.Kind == CtxSpawn {
			c.errorf(at, diag.SpawnCapturesState, "spawn can't use %s: it's state, and spawn runs on another thread (read it into a let first)", name)
			return
		}
	}
}

// spawnNotValue reports a read inside spawn of a file's let that isn't a
// value (a database, a native object, a C pointer): it's the main
// thread's, and on the web a spawn's worker has none of it.
func (c *Checker) spawnNotValue(at syntax.Node, name string, t Type) {
	if w := c.workerOf(); w != nil {
		c.errorf(at, diag.WorkerCapturesState, "worker %s can't use %s (%s): it isn't a value, and the worker runs on a thread of its own; make what it needs in the worker (var x = …)", w.name, name, aType(t))
		return
	}
	for cc := c.ctx; cc != nil; cc = cc.Parent {
		if cc.Kind == CtxSpawn {
			c.errorf(at, diag.SpawnCapturesState, "spawn can't use %s (%s): it isn't a value, and spawn runs on another thread; use it outside spawn, with what spawn returns", name, aType(t))
			return
		}
	}
}

// useMember notes a use of a component's or store's member: closures
// reach it through the env.
func (c *Checker) useMember(m *Member, at syntax.Node) Type {
	c.envUse(m.Owner, at, m.name)
	switch m.Kind {
	case EventMember:
		// Read as a value, an event is whether the caller handles it.
		return Bool
	case FnMember:
		c.errorf(at, diag.NotAValue, "%s is a fn: call it, %s()", m.name, m.name)
		return Invalid
	case StateMember, LetMember, ParamMember, PropMember:
		if c.ctx != nil && !m.Owner.Comp.Worker {
			// (A worker's are its own, on its thread: not state.)
			c.ctx.readsState = true
		}
		c.noteRead(m, at)
	}
	return c.memberType(m)
}

// envUse marks the closures between here and the component's body as
// using its env.
func (c *Checker) envUse(owner *TypeName, at syntax.Node, name string) {
	for cc := c.ctx; cc != nil; cc = cc.Parent {
		if (cc.Kind == CtxComponent || cc.Kind == CtxStore) && cc.Comp == owner {
			return
		}
		if (cc.Kind == CtxMethod || cc.Kind == CtxWorker) && cc.Comp == owner {
			return
		}
		if (cc.Kind == CtxMethod || cc.Kind == CtxWorker) && cc.Comp != nil && cc.Comp.Kind == WorkerType {
			c.errorf(at, diag.WorkerCapturesState, "worker %s can't use %s: it belongs to %s, and the worker runs on a thread of its own (pass its value to a pub fn)", cc.Comp.name, name, owner.name)
			return
		}
		if cc.Kind == CtxSpawn {
			c.errorf(at, diag.SpawnCapturesState, "spawn can't use %s: it belongs to %s, and spawn runs on another thread (read it into a let first)", name, owner.name)
			return
		}
		cc.UsesEnv = true
	}
}

// noteCall records that the current function calls f (for the reactive
// analysis and spawn).
func (c *Checker) noteCall(f *Function, at syntax.Node) {
	if c.ctx != nil && !contains(c.ctx.calls, f) {
		c.ctx.calls = append(c.ctx.calls, f)
	}
	if r := c.workerRoot(); r != nil && r != c.ctx && !contains(r.calls, f) {
		// A closure's in a worker's fn: the worker calls it too.
		r.calls = append(r.calls, f)
	}
	if c.ctx != nil {
		u := callUse{f: f, at: at}
		if c.ctx.peeks == 0 {
			c.ctx.readCalls = append(c.ctx.readCalls, u)
		}
		c.ctx.writeCalls = append(c.ctx.writeCalls, u)
	}
}

// ---------------------------------------------------------------------
// Literals

func (c *Checker) basicLit(e *syntax.BasicLit, want Type) Type {
	switch e.Kind {
	case syntax.INT:
		if _, err := ParseInt(e.Value); errors.Is(err, strconv.ErrRange) {
			// (One that isn't a number at all, the scanner said so.)
			c.errorf(e, diag.IntOverflow, "%s doesn't fit in an int (64 bits)", e.Value)
		}
		if w := Unopt(want); w == Float || w == Length {
			return Float
		}
		return Int
	case syntax.FLOAT:
		return Float
	case syntax.DURATION:
		if _, err := ParseDuration(e.Value); err != nil {
			c.errorf(e, diag.InvalidLiteral, "%v", err)
		}
		return Duration
	case syntax.LENGTH:
		if strings.HasSuffix(e.Value, "fr") {
			c.errorf(e, diag.InvalidLiteral, "%s is a grid track's share: write it .fr(%s)", e.Value, strings.TrimSuffix(e.Value, "fr"))
			return Invalid
		}
		return Length
	case syntax.COLOR:
		if _, err := ParseColor(e.Value); err != nil {
			c.errorf(e, diag.InvalidLiteral, "%v", err)
		}
		return Color
	}
	return Invalid
}

// ParseInt reads an int literal: 42, 1_000, 0xff, 0b1010, 0o17. A
// hexadecimal, binary or octal one is its 64 bits (0xffff_ffff_ffff_ffff
// is -1); a decimal one must fit an int.
func ParseInt(s string) (int64, error) {
	s = strings.ReplaceAll(s, "_", "")
	if len(s) > 2 && s[0] == '0' {
		base := 0
		switch s[1] {
		case 'x', 'X':
			base = 16
		case 'b', 'B':
			base = 2
		case 'o', 'O':
			base = 8
		}
		if base != 0 {
			u, err := strconv.ParseUint(s[2:], base, 64)
			return int64(u), err
		}
	}
	return strconv.ParseInt(s, 10, 64)
}

// ParseFloat reads a float literal.
func ParseFloat(s string) (float64, error) {
	return strconv.ParseFloat(strings.ReplaceAll(s, "_", ""), 64)
}

// ParseDuration reads a duration literal in nanoseconds: 300ms, 1.5s,
// 2min, 1h, 500us, 1ns.
func ParseDuration(s string) (int64, error) {
	s = strings.ReplaceAll(s, "_", "")
	units := []struct {
		suffix string
		ns     int64
	}{{"ns", 1}, {"us", 1e3}, {"ms", 1e6}, {"min", 60e9}, {"s", 1e9}, {"h", 3600e9}}
	for _, u := range units {
		if strings.HasSuffix(s, u.suffix) {
			num := strings.TrimSuffix(s, u.suffix)
			outOfRange := errorString("the duration " + s + " is out of range")
			// A whole number exactly: a float64 can't hold the largest int.
			if n, err := strconv.ParseInt(num, 10, 64); err == nil {
				if n > math.MaxInt64/u.ns {
					return 0, outOfRange
				}
				return n * u.ns, nil
			} else if !strings.ContainsAny(num, ".eE") {
				return 0, outOfRange
			}
			// 2^63 is past the largest int: converting it to int64 gives
			// a different int on each architecture.
			n, err := strconv.ParseFloat(num, 64)
			if err != nil || n*float64(u.ns) >= 1<<63 {
				return 0, outOfRange
			}
			return int64(math.Round(n * float64(u.ns))), nil
		}
	}
	return 0, errorString("unknown duration " + s)
}

// ParseColor reads #rgb, #rgba, #rrggbb or #rrggbbaa as an ops.Color
// (RGBA: 2<<32 | r<<24 | g<<16 | b<<8 | a).
func ParseColor(s string) (int64, error) {
	h := strings.TrimPrefix(s, "#")
	switch len(h) {
	case 3, 4:
		var b strings.Builder
		for _, ch := range h {
			b.WriteRune(ch)
			b.WriteRune(ch)
		}
		h = b.String()
	}
	if len(h) == 6 {
		h += "ff"
	}
	if len(h) != 8 {
		return 0, errorString("a color is #rgb, #rgba, #rrggbb or #rrggbbaa")
	}
	v, err := strconv.ParseUint(h, 16, 32)
	if err != nil {
		return 0, errorString("a color's digits are hexadecimal")
	}
	return int64(2<<32 | v), nil
}

// ParseLength reads 50% as a packed Length.
func ParseLength(s string) (int64, error) {
	if strings.HasSuffix(s, "%") {
		f, err := strconv.ParseFloat(strings.TrimSuffix(s, "%"), 64)
		if err != nil {
			return 0, err
		}
		return PackLength(2, f), nil
	}
	return 0, errorString("unknown length " + s)
}

// PackLength packs a Length: its unit (0 auto, 1 points, 2 percent) in
// bits 32-39 and its value as a float32 in bits 0-31.
func PackLength(unit int64, v float64) int64 {
	return unit<<32 | int64(math.Float32bits(float32(v)))
}

type errorString string

func (e errorString) Error() string { return string(e) }

// Unescape is a string literal's text with its escapes (\n \t \r \0 \"
// \' \\ \{ \} \u{…}) replaced; a raw string's as it is.
func Unescape(s string, raw bool) (string, error) {
	if raw || !strings.Contains(s, `\`) {
		return s, nil
	}
	var b strings.Builder
	for i := 0; i < len(s); i++ {
		ch := s[i]
		if ch != '\\' || i+1 >= len(s) {
			b.WriteByte(ch)
			continue
		}
		i++
		switch s[i] {
		case 'n':
			b.WriteByte('\n')
		case 't':
			b.WriteByte('\t')
		case 'r':
			b.WriteByte('\r')
		case '0':
			b.WriteByte(0)
		case '"', '\'', '\\', '{', '}':
			b.WriteByte(s[i])
		case 'u':
			end := strings.IndexByte(s[i:], '}')
			if i+1 >= len(s) || s[i+1] != '{' || end < 0 {
				return "", errorString(`a \u escape is \u{hex digits}`)
			}
			n, err := strconv.ParseUint(s[i+2:i+end], 16, 32)
			if err != nil {
				return "", errorString(`a \u escape is \u{hex digits}`)
			}
			b.WriteRune(rune(n))
			i += end
		default:
			b.WriteByte('\\')
			b.WriteByte(s[i])
		}
	}
	return b.String(), nil
}

// Dedent is a multi-line string's text: its first line (after the
// opening quotes) dropped when it's blank, the indentation of its
// closing quotes' line taken off every line, and that last line
// dropped.
func Dedent(s string) string {
	if i := strings.IndexByte(s, '\n'); i >= 0 && strings.TrimSpace(s[:i]) == "" {
		s = s[i+1:]
	}
	indent := ""
	if i := strings.LastIndexByte(s, '\n'); i >= 0 && strings.TrimSpace(s[i+1:]) == "" {
		indent = s[i+1:]
		s = s[:i]
	}
	if indent == "" {
		return s
	}
	lines := strings.Split(s, "\n")
	for i, l := range lines {
		lines[i] = strings.TrimPrefix(l, indent)
	}
	return strings.Join(lines, "\n")
}

// setOf is Set<elem>, its elements Hashable (at says where it's
// written).
func (c *Checker) setOf(elem Type, at syntax.Node, written bool) Type {
	c.checkHashable(elem, at, "a Set's element", written)
	return &Set{elem}
}

// setLit is an array literal where a Set is expected: its elements.
func (c *Checker) setLit(e *syntax.ArrayExpr, st *Set) Type {
	for _, x := range e.Elems {
		c.assign(x, st.Elem, "an element of "+typeString(st))
	}
	return st
}

func (c *Checker) arrayLit(e *syntax.ArrayExpr, want Type) Type {
	if st, ok := Unopt(want).(*Set); ok {
		return c.setLit(e, st)
	}
	var elem Type
	if a, ok := Unopt(want).(*Array); ok {
		elem = a.Elem
	}
	if len(e.Elems) == 0 {
		if elem == nil {
			if want != Invalid { // an invalid type said why already
				c.errorf(e, diag.CannotInferType, "[] says nothing of its elements' type: write it, let xs: [T] = []")
			}
			return Invalid
		}
		return &Array{elem}
	}
	if elem != nil {
		for _, x := range e.Elems {
			c.assign(x, elem, "an element of "+typeString(&Array{elem}))
		}
		return &Array{elem}
	}
	elem = c.valueType(c.expr(e.Elems[0], nil), e.Elems[0])
	if elem == Int {
		// [1, 2.5] is [float].
		for _, x := range e.Elems[1:] {
			if c.literalFloat(x) {
				elem = Float
				c.expr(e.Elems[0], Float)
				break
			}
		}
	}
	for _, x := range e.Elems[1:] {
		c.assign(x, elem, "an element of "+typeString(&Array{elem}))
	}
	return &Array{elem}
}

// literalFloat reports a float literal (maybe negated).
func (c *Checker) literalFloat(x syntax.Expr) bool {
	switch x := x.(type) {
	case *syntax.BasicLit:
		return x.Kind == syntax.FLOAT
	case *syntax.UnaryExpr:
		return x.Op == syntax.SUB && c.literalFloat(x.X)
	case *syntax.ParenExpr:
		return c.literalFloat(x.X)
	}
	return false
}

// literalInt reports an int literal (maybe negated): it can be a float.
func literalInt(x syntax.Expr) bool {
	switch x := x.(type) {
	case *syntax.BasicLit:
		return x.Kind == syntax.INT
	case *syntax.UnaryExpr:
		return x.Op == syntax.SUB && literalInt(x.X)
	case *syntax.ParenExpr:
		return literalInt(x.X)
	case *syntax.BinaryExpr:
		switch x.Op {
		case syntax.ADD, syntax.SUB, syntax.MUL, syntax.QUO, syntax.REM:
			return literalInt(x.X) && literalInt(x.Y)
		}
	}
	return false
}

func (c *Checker) mapLit(e *syntax.MapExpr, want Type) Type {
	var key, val Type
	if m, ok := Unopt(want).(*Map); ok {
		key, val = m.Key, m.Elem
	}
	if len(e.Entries) == 0 {
		if key == nil {
			if want != Invalid { // an invalid type said why already
				c.errorf(e, diag.CannotInferType, "[:] says nothing of its keys' and values' types: write them, let m: [K: V] = [:]")
			}
			return Invalid
		}
		return &Map{key, val}
	}
	if key == nil {
		key = c.valueType(c.expr(e.Entries[0].Key, nil), e.Entries[0].Key)
		val = c.valueType(c.expr(e.Entries[0].Value, nil), e.Entries[0].Value)
		c.checkHashable(key, e.Entries[0].Key, "a map's key", false)
	}
	for _, en := range e.Entries {
		c.assign(en.Key, key, "a key of "+typeString(&Map{key, val}))
		c.assign(en.Value, val, "a value of "+typeString(&Map{key, val}))
	}
	return &Map{key, val}
}

// ---------------------------------------------------------------------
// Assignability

// assign checks e where a want is expected, records the conversion it
// needs, and returns want (or e's type when want is nil).
func (c *Checker) assign(e syntax.Expr, want Type, what string) Type {
	if want == nil {
		return c.expr(e, nil)
	}
	if IsInvalid(want) {
		// What's expected is invalid (and said why): the value is
		// checked, but what it'd need from the type isn't asked again.
		c.expr(e, Invalid)
		return Invalid
	}
	// Edges from a number or [v, h] / [t, r, b, l].
	if c.isEdges(want) {
		if a, ok := e.(*syntax.ArrayExpr); ok && (len(a.Elems) == 2 || len(a.Elems) == 4) {
			for _, x := range a.Elems {
				c.assign(x, Float, "a side of Edges")
			}
			c.record(e, &Array{Float})
			c.info.Convs[e] = want
			return want
		}
	}
	return c.assigned(e, c.expr(e, want), want, what)
}

// assigned is assign once e is checked (its type got): got converted to
// want, or an error.
func (c *Checker) assigned(e syntax.Expr, got, want Type, what string) Type {
	if c.convert(e, got, want) {
		return want
	}
	c.errorf(e, diag.TypeMismatch, "%s is %s, not %s%s", describe(what), aType(got), aType(want), c.convHint(got, want))
	return want
}

func describe(what string) string {
	if what == "" {
		return "the value"
	}
	return what
}

func (c *Checker) convHint(got, want Type) string {
	if o, ok := want.(*Optional); ok && got != NoneType && !IsOptional(got) {
		// What isn't an optional would be put in one: it's what the
		// optional holds that it doesn't fit.
		want = o.Elem
	}
	if got == Int && want == Float {
		return " (make it one: float(x))"
	}
	if got == Float && want == Int {
		return " (make it one: int(x), truncating)"
	}
	if IsOptional(got) && Identical(Unopt(got), want) {
		return " (it may be none: unwrap it with if let, ?? or !)"
	}
	if want == String && got != String {
		return " (put it in a string: \"{x}\")"
	}
	return ""
}

// convert reports whether a value of type got may be used where want is
// expected, and records the conversion on e.
func (c *Checker) convert(e syntax.Expr, got, want Type) bool {
	if got == Never && want != nil && !IsInvalid(want) && want != Void && want != Never {
		// What leaves (return, throw, a call of a fn that never returns)
		// fits any type; the lowering gives it a register of the one
		// expected, which the code after it, never reached, is
		// compiled with.
		c.info.Convs[e] = want
		return true
	}
	if IsInvalid(got) || IsInvalid(want) || got == Never {
		return true
	}
	if Identical(got, want) {
		return true
	}
	if got == NoneType {
		return IsOptional(want)
	}
	if o, ok := want.(*Optional); ok {
		if c.convert(e, got, o.Elem) {
			c.info.Convs[e] = want
			return true
		}
		return false
	}
	switch want {
	case Length:
		if (got == Int || got == Float) && constNumber(e) {
			c.info.Convs[e] = Length
			return true
		}
		if got == Int || got == Float {
			c.info.Convs[e] = Length // points, packed when it runs
			return true
		}
	case Float:
		return false
	}
	if c.isEdges(want) && (got == Int || got == Float) {
		c.info.Convs[e] = want
		return true
	}
	switch w := want.(type) {
	case *Named:
		if w.Obj.Kind == InterfaceType {
			if p, ok := got.(*TypeParam); ok && c.hasIface(p, w) == "" {
				// A type parameter's value, constrained to the
				// interface (a provided method's self): the type it's
				// instantiated with converts.
				c.info.Convs[e] = want
				return true
			}
			if why := c.implements(got, w); why == "" {
				c.info.Convs[e] = want
				return true
			} else if _, ok := got.(*Named); ok {
				c.errorf(e, diag.TypeMismatch, "%s isn't %s: %s", typeString(got), aType(want), why)
				return true
			}
		}
	case *FuncType:
		g, ok := got.(*FuncType)
		if !ok || len(g.Params) != len(w.Params) {
			return false
		}
		for i := range g.Params {
			if !Identical(g.Params[i], w.Params[i]) {
				return false
			}
		}
		if g.Throws && !w.Throws || g.Async && !w.Async {
			return false
		}
		return Identical(g.Result, w.Result) || w.Result == Void
	}
	return false
}

// isEdges reports the prelude's Edges.
func (c *Checker) isEdges(t Type) bool {
	n, ok := t.(*Named)
	return ok && n.Obj.Pkg != nil && n.Obj.Pkg.Prelude && n.Obj.name == "Edges"
}

// constNumber reports a number known when compiling: a literal, maybe
// negated, or arithmetic on them.
func constNumber(e syntax.Expr) bool {
	switch x := e.(type) {
	case *syntax.BasicLit:
		return x.Kind == syntax.INT || x.Kind == syntax.FLOAT
	case *syntax.UnaryExpr:
		return x.Op == syntax.SUB && constNumber(x.X)
	case *syntax.ParenExpr:
		return constNumber(x.X)
	case *syntax.BinaryExpr:
		switch x.Op {
		case syntax.ADD, syntax.SUB, syntax.MUL, syntax.QUO:
			return constNumber(x.X) && constNumber(x.Y)
		}
	}
	return false
}

// ConstNumber evaluates a constant number (constNumber's).
func ConstNumber(e syntax.Expr) float64 {
	switch x := e.(type) {
	case *syntax.BasicLit:
		if x.Kind == syntax.INT {
			v, _ := ParseInt(x.Value)
			return float64(v)
		}
		v, _ := ParseFloat(x.Value)
		return v
	case *syntax.UnaryExpr:
		return -ConstNumber(x.X)
	case *syntax.ParenExpr:
		return ConstNumber(x.X)
	case *syntax.BinaryExpr:
		a, b := ConstNumber(x.X), ConstNumber(x.Y)
		switch x.Op {
		case syntax.ADD:
			return a + b
		case syntax.SUB:
			return a - b
		case syntax.MUL:
			return a * b
		case syntax.QUO:
			if b != 0 {
				return a / b
			}
		}
	}
	return 0
}

// join is the type of two branches' values: one of them, when the other
// converts to it.
func (c *Checker) join(a, b Type) Type {
	switch {
	case IsInvalid(a) || a == Never:
		return b
	case IsInvalid(b) || b == Never:
		return a
	case Identical(a, b):
		return a
	case a == NoneType:
		return MakeOptional(b)
	case b == NoneType:
		return MakeOptional(a)
	case IsOptional(a) && Identical(Unopt(a), b):
		return a
	case IsOptional(b) && Identical(Unopt(b), a):
		return b
	}
	return nil
}

// ---------------------------------------------------------------------
// Members

// staticType resolves x (an identifier or pkg.Name) as a type, without
// reporting anything when it isn't one.
func (c *Checker) staticType(x syntax.Expr) *TypeName {
	switch x := x.(type) {
	case *syntax.Ident:
		if tn, ok := c.lookup(x.Name).(*TypeName); ok {
			c.info.Uses[x] = tn
			return tn
		}
	case *syntax.MemberExpr:
		if id, ok := x.X.(*syntax.Ident); ok {
			if pn, ok := c.lookup(id.Name).(*PkgName); ok {
				if tn, ok := pn.Pkg.Scope.LookupLocal(x.Name.Name).(*TypeName); ok {
					c.info.Uses[id] = pn
					c.info.Selections[x] = &Selection{Kind: SelPkg, Obj: tn}
					return tn
				}
			}
		}
	}
	return nil
}

func (c *Checker) member(e *syntax.MemberExpr, want Type) Type {
	name := e.Name.Name
	// A package's member.
	if id, ok := e.X.(*syntax.Ident); ok {
		if pn, ok := c.lookup(id.Name).(*PkgName); ok {
			c.info.Uses[id] = pn
			o := pn.Pkg.Scope.LookupLocal(name)
			if o == nil {
				if !pn.Pkg.Broken {
					c.errorf(e.Name, diag.UnknownMember, "package %s has no %s", pn.Pkg.Path, name)
				}
				return Invalid
			}
			if !c.exported(pn.Pkg, o) {
				c.errorf(e.Name, diag.NotPub, "%s isn't pub in package %s", name, pn.Pkg.Path)
			}
			c.info.Selections[e] = &Selection{Kind: SelPkg, Obj: o}
			c.info.Uses[e.Name] = o
			switch o := o.(type) {
			case *Global:
				return c.useGlobal(o, e)
			case *Function:
				c.funcSig(o)
				c.noteCall(o, e)
				return o.Sig.Type()
			case *TypeName:
				c.errorf(e, diag.NotAValue, "%s is a type, not a value%s", name, typeValueHint(o))
			case *Builtin:
				c.errorf(e, diag.NotAValue, "%s.%s is a function: call it", id.Name, name)
			}
			return Invalid
		}
	}
	// A type's case or constant.
	if tn := c.staticType(e.X); tn != nil {
		c.record(e.X, Invalid)
		return c.staticMember(e, tn, name, want)
	}
	var xt Type
	if implicitRoot(e.X) {
		xt = c.expr(e.X, want)
	} else {
		xt = c.expr(e.X, nil)
	}
	if IsInvalid(xt) {
		return Invalid
	}
	if e.Optional || IsOptional(xt) && c.chained[unparen(e.X)] {
		// x?.name, or .name after one in a chain (a?.b.c): none when
		// what's before is.
		c.chained[e] = true
		if !IsOptional(xt) {
			c.errorf(e, diag.NotOptional, "%s isn't optional: use . (?. is for optionals)", typeString(xt))
		}
		t := c.valueMember(e, Unopt(xt), name)
		if sel := c.info.Selections[e]; sel != nil {
			sel.Optional = true
			sel.MemberType = t
		}
		if IsInvalid(t) {
			return t
		}
		return MakeOptional(t)
	}
	if IsOptional(xt) {
		c.errorf(e, diag.MaybeNone, "%s may be none: use ?. (x?.%s), or unwrap it first", typeString(xt), name)
		return Invalid
	}
	return c.valueMember(e, xt, name)
}

// staticMember is Type.name: a case or a constant.
func (c *Checker) staticMember(e syntax.Expr, tn *TypeName, name string, want Type) Type {
	c.resolveType(tn)
	t := tn.Type
	if len(tn.TypeParams) > 0 {
		// Enum<T>.case: the type's arguments come from what's expected.
		if n, ok := Unopt(want).(*Named); ok && n.Obj == tn {
			t = n
		} else {
			c.errorf(e, diag.CannotInferType, "%s is generic: write the type where it's expected (let x: %s<…> = .%s)", tn.name, tn.name, name)
			return Invalid
		}
	}
	if k := tn.Case(name); k != nil {
		c.info.Selections[e] = &Selection{Kind: SelCase, Case: k, Recv: t}
		if len(k.Params) > 0 {
			c.errorf(e, diag.ArgumentCount, "case %s of %s has a payload: give it, %s.%s(…)", name, tn.name, tn.name, name)
		}
		return t
	}
	if g := tn.Consts[name]; g != nil {
		c.info.Selections[e] = &Selection{Kind: SelConst, Global: g, Recv: t}
		return c.useGlobal(g, e)
	}
	if hit, _ := c.extLookup(t, name, "const", e); hit.g != nil {
		c.info.Selections[e] = &Selection{Kind: SelConst, Global: hit.g, Recv: t}
		if m, ok := e.(*syntax.MemberExpr); ok {
			c.info.Uses[m.Name] = hit.g
		}
		return c.useGlobal(hit.g, e)
	}
	f := tn.Statics[name]
	if f == nil {
		hit, _ := c.extLookup(t, name, "static", e)
		f = hit.f
	}
	if f != nil {
		// A static fn as a value: Money.of.
		c.info.Selections[e] = &Selection{Kind: SelPkg, Obj: f}
		if m, ok := e.(*syntax.MemberExpr); ok {
			c.info.Uses[m.Name] = f
		}
		c.funcSig(f)
		c.noteCall(f, e)
		return f.Sig.Type()
	}
	if tn.Type == Color {
		// Color.accent: the semantic color, as .accent where a Color is
		// expected.
		if v, ok := SemanticColor(name); ok {
			c.info.Selections[e] = &Selection{Kind: SelColor, Value: v, Recv: Color}
			return Color
		}
	}
	if tn.Kind == EnumType || tn.Kind == StructType {
		c.errorf(e, diag.UnknownMember, "%s has no case or constant %s", tn.name, name)
	} else {
		c.errorf(e, diag.UnknownMember, "%s is a type: it has no member %s", tn.name, name)
	}
	return Invalid
}

// valueMember is x.name for a value x of type t.
func (c *Checker) valueMember(e *syntax.MemberExpr, t Type, name string) Type {
	if isIndexName(name) {
		return c.tupleElem(e, t, name)
	}
	switch u := t.(type) {
	case *TypeParam:
		if f, i := paramField(u, name); f != nil {
			if i.Obj.isErrorIface {
				c.info.Selections[e] = &Selection{Kind: SelErrorMessage, Recv: t}
			} else {
				c.info.Selections[e] = &Selection{Kind: SelIfaceField, Field: f, Recv: t, Name: name}
			}
			return f.Type
		}
		if m, _ := paramMethod(u, name); m != nil {
			c.errorf(e.Name, diag.NotAValue, "%s is a method: call it, %s()", name, name)
			return Invalid
		}
		c.errorf(e.Name, diag.UnknownMember, "%s has no %s%s", u.Name, name, paramHint(u))
		return Invalid
	case *Named:
		s := u.Subst()
		switch u.Obj.Kind {
		case StructType:
			if f := u.Obj.Field(name); f != nil {
				c.info.Selections[e] = &Selection{Kind: SelField, Field: f, Recv: t}
				return s.Apply(f.Type)
			}
			if u.Obj.Methods[name] != nil && !(u.Obj.IsError && name == "message") {
				c.errorf(e.Name, diag.NotAValue, "%s is a method of %s: call it, %s()", name, u.Obj.name, name)
				return Invalid
			}
		case EnumType:
			if u.Obj.IsError && name == "message" {
				c.info.Selections[e] = &Selection{Kind: SelErrorMessage, Recv: t}
				return String
			}
			if u.Obj.Methods[name] != nil {
				c.errorf(e.Name, diag.NotAValue, "%s is a method of %s: call it, %s()", name, u.Obj.name, name)
				return Invalid
			}
			if u.Obj == loadingT && (name == "value" || name == "loading") {
				c.info.Selections[e] = &Selection{Kind: SelLoading, Name: name, Recv: t}
				if c.ctx != nil {
					c.ctx.readsState = true
				}
				if name == "loading" {
					return Bool
				}
				return MakeOptional(u.Args[0])
			}
			if u.Obj.IsError && name == "message" {
				c.info.Selections[e] = &Selection{Kind: SelErrorMessage, Recv: t}
				return String
			}
		case WorkerType:
			if m := u.Obj.Comp.Members[name]; m != nil {
				if m.Kind == FnMember {
					c.errorf(e.Name, diag.NotAValue, "%s is a fn of %s: call it, %s()", name, u.Obj.name, name)
				} else {
					c.errorf(e.Name, diag.WorkerMisuse, "%s is worker %s's own: it's read on its thread, by its fns (return it from a pub fn)", name, u.Obj.name)
				}
				return Invalid
			}
		case StoreType:
			if m := u.Obj.Comp.Members[name]; m != nil {
				if u.Obj.Pkg != c.pkg && !m.Pub {
					c.errorf(e.Name, diag.NotPub, "%s isn't pub in store %s", name, u.Obj.name)
				}
				switch m.Kind {
				case FnMember:
					c.errorf(e.Name, diag.NotAValue, "%s is a fn of %s: call it, %s()", name, u.Obj.name, name)
					return Invalid
				case EventMember:
					c.errorf(e.Name, diag.UnknownMember, "a store has no events")
					return Invalid
				}
				c.info.Selections[e] = &Selection{Kind: SelStoreMember, Member: m, Recv: t}
				if c.ctx != nil {
					c.ctx.readsState = true
				}
				c.noteRead(m, e)
				c.spawnState(e, name)
				return s.Apply(c.memberType(m))
			}
		case InterfaceType:
			if u.Obj.isErrorIface && name == "message" {
				c.info.Selections[e] = &Selection{Kind: SelErrorMessage, Recv: t}
				return String
			}
			for _, f := range u.Obj.Iface.Fields {
				if f.Name == name {
					c.info.Selections[e] = &Selection{Kind: SelIfaceField, Field: f, Recv: t, Name: name}
					return f.Type
				}
			}
			for _, m := range u.Obj.Iface.Methods {
				if m.Name == name {
					c.errorf(e.Name, diag.NotAValue, "%s is a method: call it, %s()", name, name)
					return Invalid
				}
			}
		}
		if u.Obj.IsError && name == "message" {
			c.info.Selections[e] = &Selection{Kind: SelErrorMessage, Recv: t}
			return String
		}
		c.errorf(e.Name, diag.UnknownMember, "%s has no %s%s", typeString(t), name, c.memberSuggest(t, name))
		return Invalid
	}
	if id, ok := unparen(e.X).(*syntax.Ident); ok && t == Ref {
		if v, ok := c.info.Uses[id].(*Var); ok && v.RefComp != nil {
			if d := c.FindVariant(v.RefComp, name); d != nil && (d.Pub || d.Owner.Pkg == c.pkg) {
				// Component as name, then name.checked: its variant, tracked.
				c.info.Selections[e] = &Selection{Kind: SelVariant, Recv: t, Name: name, Obj: d}
				if c.ctx != nil {
					c.ctx.readsState = true
				}
				return Bool
			}
		}
	}
	if m := lookupField(t, name); m != nil {
		_, res := c.methodSig(m)
		c.info.Selections[e] = &Selection{Kind: SelBuiltinField, Recv: t, Name: name, Method: m}
		if m.Recv == "ref" && c.ctx != nil {
			c.ctx.readsState = true // a view's state: tracked
		}
		return res
	}
	if lookupMethod(t, name) != nil {
		c.errorf(e.Name, diag.NotAValue, "%s is a method of %s: call it, %s()", name, typeString(t), name)
		return Invalid
	}
	c.errorf(e.Name, diag.UnknownMember, "%s has no %s%s", typeString(t), name, c.memberSuggest(t, name))
	return Invalid
}

// isIndexName reports a member named by digits: a tuple's element, t.0.
func isIndexName(name string) bool {
	if name == "" {
		return false
	}
	for _, r := range name {
		if r < '0' || r > '9' {
			return false
		}
	}
	return true
}

// tupleElem is t.0, t.1: a tuple's element, by its place.
func (c *Checker) tupleElem(e *syntax.MemberExpr, t Type, name string) Type {
	tt, ok := t.(*Tuple)
	if !ok {
		c.errorf(e.Name, diag.UnknownMember, "%s isn't a tuple: .%s reads a tuple's element", typeString(t), name)
		return Invalid
	}
	i, err := strconv.Atoi(name)
	if err != nil || i >= len(tt.Elems) || len(name) > 1 && name[0] == '0' {
		c.errorf(e.Name, diag.UnknownMember, "%s has no element %s: its elements are %s", typeString(t), name, tupleElemNames(len(tt.Elems)))
		return Invalid
	}
	f := &Field{Name: name, Type: tt.Elems[i], Index: i}
	c.info.Selections[e] = &Selection{Kind: SelTupleElem, Field: f, Recv: t}
	return tt.Elems[i]
}

// tupleElemNames is how a tuple of n's elements are read: .0 and .1.
func tupleElemNames(n int) string {
	var b strings.Builder
	for i := 0; i < n; i++ {
		switch {
		case i == 0:
		case i == n-1:
			b.WriteString(" and ")
		default:
			b.WriteString(", ")
		}
		b.WriteString("." + strconv.Itoa(i))
	}
	return b.String()
}

func (c *Checker) memberSuggest(t Type, name string) string {
	var names []string
	if n, ok := t.(*Named); ok {
		for _, f := range n.Obj.Fields {
			names = append(names, f.Name)
		}
		for m := range n.Obj.Methods {
			names = append(names, m)
		}
		if n.Obj.Comp != nil {
			for m := range n.Obj.Comp.Members {
				names = append(names, m)
			}
		}
	} else {
		names = methodNames(t)
	}
	loading := IsLoading(t)
	if loading {
		names = append(names, "value", "loading", "reload", "optimistic")
	}
	best, bestD := "", 3
	for _, n := range names {
		if d := editDistance(name, n); d < bestD && d < len(name) {
			best, bestD = n, d
		}
	}
	if best != "" {
		return " (did you mean " + best + "?)"
	}
	if loading {
		return ": it has value (what it shows), loading (a run is in flight), reload() and optimistic(change); match it for its case"
	}
	return ""
}

// implicitMember is .name where a type is expected.
func (c *Checker) implicitMember(e *syntax.ImplicitMember, want Type) Type {
	name := e.Name.Name
	c.info.Expected[e] = want
	if want == Invalid {
		return Invalid // what's expected is invalid, and said why
	}
	w := Unopt(want)
	if w != nil && !IsInvalid(w) {
		// An extension's constant: .brand where a Color is expected.
		if hit, _ := c.extLookup(w, name, "const", e); hit.g != nil {
			c.info.Selections[e] = &Selection{Kind: SelConst, Global: hit.g, Recv: w}
			c.info.Uses[e.Name] = hit.g
			return c.useGlobal(hit.g, e)
		}
	}
	switch w {
	case nil:
		c.errorf(e, diag.CannotInferType, "can't tell what .%s is here: write its type, Type.%s", name, name)
		return Invalid
	case Color:
		if v, ok := SemanticColor(name); ok {
			c.info.Selections[e] = &Selection{Kind: SelColor, Value: v, Recv: Color}
			return Color
		}
		c.errorf(e, diag.UnknownMember, "%s isn't a semantic color (they're %s)", name, strings.Join(semanticColorNames(), ", "))
		return Invalid
	case Length:
		if name == "auto" {
			c.info.Selections[e] = &Selection{Kind: SelLengthAuto, Recv: Length}
			return Length
		}
		c.errorf(e, diag.UnknownMember, "a Length is a number of points, a percentage (50%%) or .auto, not .%s", name)
		return Invalid
	}
	if IsInvalid(w) {
		return Invalid
	}
	if n, ok := w.(*Named); ok && (n.Obj.Kind == EnumType || n.Obj.Kind == StructType) {
		if k := n.Obj.Case(name); k != nil {
			c.info.Selections[e] = &Selection{Kind: SelCase, Case: k, Recv: n}
			if len(k.Params) > 0 {
				c.errorf(e, diag.ArgumentCount, "case %s has a payload: give it, .%s(…)", name, name)
			}
			return n
		}
		if g := n.Obj.Consts[name]; g != nil {
			c.info.Selections[e] = &Selection{Kind: SelConst, Global: g, Recv: n}
			return c.useGlobal(g, e)
		}
		c.errorf(e, diag.UnknownMember, "%s has no case or constant %s", typeString(w), name)
		return Invalid
	}
	c.errorf(e, diag.UnknownMember, ".%s: %s has no implicit members (they're an enum's cases or a type's constants)", name, aType(w))
	return Invalid
}

// ---------------------------------------------------------------------
// Operators

func (c *Checker) index(e *syntax.IndexExpr) Type {
	xt := c.expr(e.X, nil)
	if IsOptional(xt) && c.chained[unparen(e.X)] {
		// a?.items[0], in a ?. chain: none when what's before is.
		c.chained[e] = true
		return MakeOptional(c.index1(e, Unopt(xt)))
	}
	return c.index1(e, xt)
}

func (c *Checker) index1(e *syntax.IndexExpr, xt Type) Type {
	if _, isMap := xt.(*Map); !isMap && e.Default != nil {
		c.expr(e.Default, nil)
		if !IsInvalid(xt) {
			c.errorf(e.Default, diag.BadOperand, "default: is a map's, for a key it may not have: %s has no such entries", aType(xt))
		}
	}
	switch t := xt.(type) {
	case *Array:
		c.assign(e.Index, Int, "an index")
		return t.Elem
	case *Map:
		c.assign(e.Index, t.Key, "a key")
		if e.Default != nil {
			// m[k, default: v]: the entry, or v when there's none.
			c.assign(e.Default, t.Elem, "the default")
			return t.Elem
		}
		return MakeOptional(t.Elem)
	}
	if xt == Bytes {
		c.assign(e.Index, Int, "an index")
		return Int
	}
	c.expr(e.Index, nil)
	if !IsInvalid(xt) {
		c.errorf(e, diag.BadOperand, "%s can't be indexed: only arrays, maps and bytes can", aType(xt))
	}
	return Invalid
}

// intOperand checks an operand of a bitwise operator, which takes ints:
// whether it's one (an invalid one, said already, isn't).
func (c *Checker) intOperand(x syntax.Expr, op syntax.Token) bool {
	t := c.expr(x, Int)
	switch {
	case t == Int:
		return true
	case IsInvalid(t):
	case t == Bool && (op == syntax.AMP || op == syntax.PIPE):
		c.errorf(x, diag.BadOperand, "%s takes ints, not a bool: write %s%s for bools", op, op, op)
	default:
		c.errorf(x, diag.BadOperand, "%s takes ints, not %s", op, aType(t))
	}
	return false
}

func (c *Checker) unary(e *syntax.UnaryExpr, want Type) Type {
	switch e.Op {
	case syntax.TILDE:
		if !c.intOperand(e.X, e.Op) {
			return Invalid
		}
		return Int
	case syntax.NOT:
		c.assign(e.X, Bool, "the operand of !")
		return Bool
	case syntax.SUB:
		t := c.expr(e.X, want)
		if o, res := c.negation(t); o != nil {
			c.info.Negations[e] = o
			return res
		}
		if !IsInvalid(t) && !IsNumeric(t) && t != Duration {
			c.errorf(e, diag.BadOperand, "- negates numbers, not %s", aType(t))
			return Invalid
		}
		if lit, ok := unparen(e.X).(*syntax.BasicLit); ok && lit.Kind == syntax.INT && t == Int {
			// -0x8000_0000_0000_0000: the smallest int's bits, negated.
			if v, err := ParseInt(lit.Value); err == nil && v == math.MinInt64 {
				c.errorf(e, diag.IntOverflow, "-%s overflows: %s is the smallest int, which has no positive", lit.Value, lit.Value)
			}
		}
		return t
	case syntax.AWAIT:
		if c.ctx == nil || !c.ctx.Async {
			c.errorf(e, c.awaitCode(), "await only where code may wait: %s", c.asyncHint())
		} else if c.ctx.peeks > 0 {
			c.errorf(e, diag.AwaitOutsideAsync, "peek reads at once, and can't wait: await first, into a let, and peek that")
		} else {
			c.ctx.awaited(e.Start)
		}
		c.inAwait++
		t := c.expr(e.X, want)
		c.inAwait--
		if ce, ok := unparen(e.X).(*syntax.CallExpr); ok {
			if call := c.info.Calls[ce]; call != nil && call.Kind == CallBuiltin && call.Builtin.ID == BAnimate {
				call.Awaited = true
			}
		}
		if tk, ok := t.(*Task); ok {
			return tk.Elem
		}
		return t
	case syntax.TRY:
		if !e.Optional {
			c.mayThrow(e)
		}
		c.inTry++
		saved := c.handled
		if e.Optional {
			c.handled++
		}
		// The operand is given what an expected optional holds: try? makes
		// the optional of it. try is what its operand is, so a call there
		// whose result is a type parameter is told of the optional, which
		// its arguments may be (inferArgs).
		tried, triedOpt := c.tried, c.triedOpt
		if ce, ok := tryCall(e.X); ok && !e.Optional && IsOptional(want) {
			c.tried, c.triedOpt = ce, want
		}
		t := c.expr(e.X, Unopt(want))
		c.tried, c.triedOpt = tried, triedOpt
		c.handled = saved
		c.inTry--
		if e.Optional {
			if t == Void {
				return Void
			}
			return MakeOptional(t)
		}
		return t
	case syntax.LOAD:
		return c.load(e)
	case syntax.RELOAD:
		return c.reload(e)
	}
	c.errorf(e, diag.InternalError, "unexpected operator %s", e.Op)
	return Invalid
}

// tryCall returns the call a try is of: try f(), try await f().
func tryCall(x syntax.Expr) (*syntax.CallExpr, bool) {
	for {
		x = unparen(x)
		u, ok := x.(*syntax.UnaryExpr)
		if !ok || u.Op != syntax.AWAIT {
			break
		}
		x = u.X
	}
	ce, ok := x.(*syntax.CallExpr)
	return ce, ok
}

// mayThrow reports a throw (or try) where nothing catches it.
func (c *Checker) mayThrow(at syntax.Node) {
	if msg := c.throwHere(at); msg != "" {
		c.errorf(at, diag.UnhandledError, "%s", msg)
	}
}

// throwHere notes that an error may leave at at, and says why it can't
// ("" when it can).
func (c *Checker) throwHere(at syntax.Node) string {
	if c.handled > 0 || c.ctx == nil {
		return ""
	}
	if !c.ctx.threw {
		c.ctx.threwAt = at
	}
	c.ctx.threw = true
	if c.ctx.Throws {
		return ""
	}
	if c.partial != nil && c.partial.ctx == c.ctx {
		// Inside a do or a catch whose clauses don't take every error.
		return fmt.Sprintf("the catch%s here take%s %s only, and %s: add a catch e { … } for the rest%s",
			map[bool]string{true: "es", false: ""}[c.partial.n > 1], map[bool]string{true: "", false: "s"}[c.partial.n > 1],
			c.partial.what, c.cantThrow(), c.throwsHint())
	}
	switch c.ctx.Kind {
	case CtxFunc, CtxMethod, CtxLocalFn:
		return c.ctx.Name + " doesn't throw: mark it throws, or catch the error (try … catch { }, do { } catch { }, try?)"
	case CtxLambda:
		return "this closure doesn't throw where it's passed: catch the error (try … catch { }, do { } catch { }, try?)"
	case CtxGlobal:
		return "a value here can't throw: catch the error (try … catch { fallback }, try?)"
	case CtxSpawn:
		// spawn's error is its task's.
		return ""
	case CtxBinding, CtxComponent, CtxStore, CtxBuilder:
		return "what a component's body declares can't throw: give a fallback (try … catch { fallback }), or none on an error (try?)"
	case CtxCleanup:
		return "a cleanup can't throw: it runs as its owner goes, and the cleanups after it still have to; catch the error (try?, do { } catch { })"
	}
	return "this can't throw here: catch the error (try … catch { }, do { } catch { }, try?)"
}

// awaitCode is the code of an await where nothing may wait: in an
// effect, async-in-effect (its work belongs in a task); elsewhere,
// await-outside-async.
func (c *Checker) awaitCode() *diag.Code {
	if c.ctx != nil && c.ctx.Kind == CtxEffect {
		return diag.AsyncInEffect
	}
	return diag.AwaitOutsideAsync
}

func (c *Checker) asyncHint() string {
	if c.ctx == nil {
		return "in an async fn, a handler, a task or a test"
	}
	switch c.ctx.Kind {
	case CtxFunc, CtxMethod, CtxLocalFn:
		return "mark " + c.ctx.Name + " async (async fn …)"
	case CtxBinding, CtxComponent, CtxStore, CtxBuilder:
		return "a component's body is declarations; load data with let x = load f(), or in a task { }"
	case CtxEffect:
		return "an effect runs at once; start async work in a task { }"
	case CtxCleanup:
		return "a cleanup runs at once, as its owner goes; async teardown is a task's defer"
	case CtxGlobal:
		return "a value here is computed at once"
	case CtxSpawn:
		return "spawn's block runs on another thread, synchronously"
	}
	if w := c.workerOf(); w != nil {
		return "worker " + w.name + "'s fns run on its thread, one call at a time, and don't wait"
	}
	return "in an async fn, a handler, a task or a test"
}

func (c *Checker) binary(e *syntax.BinaryExpr, want Type) Type {
	switch e.Op {
	case syntax.LAND, syntax.LOR:
		c.assign(e.X, Bool, "the operand of "+e.Op.String())
		c.assign(e.Y, Bool, "the operand of "+e.Op.String())
		return Bool
	case syntax.COALESCE:
		xt := c.expr(e.X, MakeOptionalOrNil(want))
		if IsInvalid(xt) {
			c.expr(e.Y, nil)
			return Invalid
		}
		if !IsOptional(xt) {
			c.errorf(e.X, diag.NotOptional, "%s isn't optional: ?? has nothing to do", typeString(xt))
			c.expr(e.Y, nil)
			return xt
		}
		el := Unopt(xt)
		yt := c.expr(e.Y, el)
		if Identical(yt, xt) || yt == NoneType {
			return xt
		}
		if !c.convert(e.Y, yt, el) {
			c.errorf(e.Y, diag.TypeMismatch, "the default is %s, not %s", aType(yt), aType(el))
		}
		return el
	case syntax.RANGE, syntax.RANGELT:
		c.assign(e.X, Int, "a range's start")
		c.assign(e.Y, Int, "a range's end")
		return &Array{Int}
	case syntax.AMP, syntax.PIPE, syntax.CARET, syntax.SHL, syntax.SHR, syntax.WADD, syntax.WSUB, syntax.WMUL:
		// Not an int when an operand isn't one: what it's used as
		// isn't asked again.
		okX := c.intOperand(e.X, e.Op)
		if !c.intOperand(e.Y, e.Op) || !okX {
			return Invalid
		}
		return Int
	}
	// Arithmetic and comparisons: an int literal on one side takes the
	// other side's type.
	var xt, yt Type
	switch {
	case literalInt(e.X) && !literalInt(e.Y):
		yt = c.expr(e.Y, numWant(want, e.Op))
		xt = c.expr(e.X, numOnly(yt))
	case literalInt(e.Y) && !literalInt(e.X):
		xt = c.expr(e.X, numWant(want, e.Op))
		yt = c.expr(e.Y, numOnly(xt))
	default:
		w := numWant(want, e.Op)
		xt = c.expr(e.X, w)
		yt = c.expr(e.Y, xt)
	}
	if IsInvalid(xt) || IsInvalid(yt) {
		switch e.Op {
		case syntax.EQL, syntax.NEQ, syntax.LSS, syntax.LEQ, syntax.GTR, syntax.GEQ:
			return Bool
		}
		return Invalid
	}
	if t, ok := c.operatorMethod(e, xt, yt); ok {
		return t
	}
	switch e.Op {
	case syntax.EQL, syntax.NEQ:
		if xt == NoneType && IsOptional(yt) || yt == NoneType && IsOptional(xt) {
			return Bool
		}
		if !Identical(xt, yt) && !c.convert(e.Y, yt, xt) && !c.convert(e.X, xt, yt) {
			c.errorf(e, diag.BadOperand, "can't compare %s with %s", aType(xt), aType(yt))
			return Bool
		}
		t := xt
		if IsOptional(yt) {
			t = yt
		}
		if !IsEquatable(t) {
			c.errorf(e, diag.ConstraintNotMet, "%s can't be compared with %s: %s", typeString(t), e.Op, whyNotEquatable(t))
		}
		return Bool
	case syntax.LSS, syntax.LEQ, syntax.GTR, syntax.GEQ:
		if !Identical(xt, yt) {
			c.errorf(e, diag.BadOperand, "can't compare %s with %s", aType(xt), aType(yt))
			return Bool
		}
		if why := c.whyNotComparable(xt); why != "" {
			c.errorf(e, diag.ConstraintNotMet, "%s can't be ordered with %s: %s", typeString(xt), e.Op, why)
		}
		return Bool
	case syntax.ADD:
		if xt == String && yt == String {
			return String
		}
		if a, ok := xt.(*Array); ok && Identical(xt, yt) {
			return a
		}
		fallthrough
	case syntax.SUB:
		if xt == Duration && yt == Duration {
			return Duration
		}
		fallthrough
	case syntax.MUL, syntax.QUO, syntax.REM:
		if xt == Duration && yt == Int && (e.Op == syntax.MUL || e.Op == syntax.QUO) {
			return Duration
		}
		// A Duration scaled by a float (0.5 * d), and a Duration's ratio to
		// another (elapsed / total), a float.
		if (xt == Duration && yt == Float && (e.Op == syntax.MUL || e.Op == syntax.QUO)) || (xt == Float && yt == Duration && e.Op == syntax.MUL) {
			c.info.DurationFloats[e] = true
			return Duration
		}
		// A Length scaled by a number: filled * 1% is a percent.
		if e.Op == syntax.MUL && (xt == Length && IsNumeric(yt) || IsNumeric(xt) && yt == Length) {
			c.info.LengthScales[e] = true
			return Length
		}
		if xt == Duration && yt == Duration && e.Op == syntax.QUO {
			c.info.DurationFloats[e] = true
			return Float
		}
		if xt == Int && yt == Duration && e.Op == syntax.MUL {
			return Duration
		}
		if IsNumeric(xt) && Identical(xt, yt) {
			return xt
		}
		if IsNumeric(xt) && IsNumeric(yt) {
			c.errorf(e, diag.BadOperand, "%s on an int and a float: make them the same, float(x) or int(x)", e.Op)
			return Invalid
		}
		c.errorf(e, diag.BadOperand, "%s doesn't take %s and %s", e.Op, aType(xt), aType(yt))
		return Invalid
	}
	c.errorf(e, diag.InternalError, "unexpected operator %s", e.Op)
	return Invalid
}

// An Operator is an operator a struct's or an enum's method does: + its
// add(y), - its sub(y) or minus(y) (the one whose parameter takes y), *
// its mul(y) or times(y), / its div(y) or quo(y), % its rem(y), <, <=, >
// and >= its compare(y) -> int against 0, and -x its neg().
type Operator struct {
	Func    *Function
	Recv    Type
	Compare bool
}

// operatorMethod finds the method of x's struct or enum an operator
// calls (std/time's Time: t + 1h, b - a, a < b), and its result.
func (c *Checker) operatorMethod(e *syntax.BinaryExpr, xt, yt Type) (Type, bool) {
	o, res := c.findOperator(e.Op, xt, yt, nil)
	if o == nil {
		return nil, false
	}
	c.info.Operators[e] = o
	return res, true
}

// operatorNames are the methods an operator may call, and whether it
// compares. An operator with two names takes two kinds of operand, one
// each (Time - Time and Time - Duration: sub and minus; Decimal *
// Decimal and Decimal * int: mul and times).
func operatorNames(op syntax.Token) (names []string, compare bool) {
	switch op {
	case syntax.ADD:
		return []string{"add"}, false
	case syntax.SUB:
		return []string{"sub", "minus"}, false
	case syntax.MUL:
		return []string{"mul", "times"}, false
	case syntax.QUO:
		return []string{"div", "quo"}, false
	case syntax.REM:
		return []string{"rem"}, false
	case syntax.LSS, syntax.LEQ, syntax.GTR, syntax.GEQ:
		return []string{"compare"}, true
	}
	return nil, false
}

// negation is the neg() of t's struct or enum that -x calls, and its
// result: a method without parameters, neither async nor throwing.
func (c *Checker) negation(t Type) (*Operator, Type) {
	n, ok := t.(*Named)
	if !ok || n.Obj.Kind != StructType && n.Obj.Kind != EnumType {
		return nil, nil
	}
	f := n.Obj.Methods["neg"]
	if f == nil {
		return nil, nil
	}
	c.funcSig(f)
	if f.Sig == nil || len(f.Sig.Params) != 0 || f.Sig.Async || f.Sig.Throws || f.Sig.Result == nil || f.Sig.Result == Void {
		return nil, nil
	}
	return &Operator{Func: f, Recv: t}, n.Subst().Apply(f.Sig.Result)
}

// operatorCandidates are the methods of xt's struct or enum that op may
// call, their signatures read: one parameter, neither async nor
// throwing.
func (c *Checker) operatorCandidates(op syntax.Token, xt Type) (fs []*Function, n *Named, compare bool) {
	n, ok := xt.(*Named)
	if !ok || n.Obj.Kind != StructType && n.Obj.Kind != EnumType {
		return nil, nil, false
	}
	names, compare := operatorNames(op)
	for _, name := range names {
		f := n.Obj.Methods[name]
		if f == nil {
			continue
		}
		c.funcSig(f)
		if f.Sig == nil || len(f.Sig.Params) != 1 || f.Sig.Async || f.Sig.Throws {
			continue
		}
		fs = append(fs, f)
	}
	return fs, n, compare
}

// findOperator is the method op calls on an xt and a yt, and its result
// (a comparison's: a bool); want, when set, is what the result must be
// (x += y's: x's type).
func (c *Checker) findOperator(op syntax.Token, xt, yt, want Type) (*Operator, Type) {
	fs, n, compare := c.operatorCandidates(op, xt)
	for _, f := range fs {
		s := n.Subst()
		pt, res := s.Apply(f.Sig.Params[0].Type), s.Apply(f.Sig.Result)
		if !Identical(pt, yt) {
			continue
		}
		if compare {
			if res != Int {
				continue
			}
			res = Bool
		}
		if want != nil && !Identical(res, want) {
			continue
		}
		return &Operator{Func: f, Recv: xt, Compare: compare}, res
	}
	return nil, nil
}

// operatorParam is what x op= y's y is expected to be: the parameter of
// the one method op calls on an xt that gives an xt back (nil: none, or
// more than one).
func (c *Checker) operatorParam(op syntax.Token, xt Type) Type {
	fs, n, compare := c.operatorCandidates(op, xt)
	if compare {
		return nil
	}
	var param Type
	for _, f := range fs {
		s := n.Subst()
		if !Identical(s.Apply(f.Sig.Result), xt) {
			continue
		}
		if param != nil {
			return nil
		}
		param = s.Apply(f.Sig.Params[0].Type)
	}
	return param
}

// MakeOptionalOrNil is t? (nil stays nil).
func MakeOptionalOrNil(t Type) Type {
	if t == nil {
		return nil
	}
	return MakeOptional(t)
}

func numWant(want Type, op syntax.Token) Type {
	switch op {
	case syntax.EQL, syntax.NEQ, syntax.LSS, syntax.LEQ, syntax.GTR, syntax.GEQ:
		return nil
	}
	if w := Unopt(want); w == Float || w == Length {
		return Float
	}
	return nil
}

func numOnly(t Type) Type {
	if t == Float {
		return Float
	}
	return nil
}

// ---------------------------------------------------------------------
// if and match as values

// valueBlock checks a block whose value is its last expression.
func (c *Checker) valueBlock(b *syntax.Block, want Type) Type {
	if b == nil {
		return Invalid
	}
	c.openScope()
	defer c.closeScope()
	n := len(b.Items)
	if n == 0 {
		c.errorPos(b.Lbrace, diag.NoValue, "this block has no value: end it with one")
		return Invalid
	}
	c.stmts(b.Items[:n-1])
	last := b.Items[n-1]
	if es, ok := last.(*syntax.ExprStmt); ok {
		if want != nil {
			return c.assign(es.X, want, "the value")
		}
		return c.expr(es.X, nil)
	}
	if t, ok := c.valueItem(last, want); ok {
		return t
	}
	c.stmt(last)
	if c.terminates(b.Items) {
		return Never
	}
	if s, ok := last.(*syntax.IfStmt); ok {
		c.errorf(lastIf(s), diag.NoValue, "an if used as a value needs an else")
		return Invalid
	}
	c.errorf(last, diag.NoValue, "this block has no value: end it with an expression")
	return Invalid
}

// lastIf is the last if of s's else-if chain.
func lastIf(s *syntax.IfStmt) *syntax.IfStmt {
	for {
		el, ok := s.Else.(*syntax.IfStmt)
		if !ok {
			return s
		}
		s = el
	}
}

// valueItem checks the last item of a block whose value is its last
// expression when it's an if (each in its chain with an else) or a
// match: a value, as if it were one written as an expression. ok is
// false for any other item.
func (c *Checker) valueItem(last syntax.Item, want Type) (t Type, ok bool) {
	switch s := last.(type) {
	case *syntax.IfStmt:
		if lastIf(s).Else == nil {
			return nil, false
		}
		t = c.ifValue(s, s.Cond, s.Then, s.Else, want)
	case *syntax.MatchStmt:
		t = c.matchValue(s, s.Subject, s.Arms, want)
	default:
		return nil, false
	}
	c.info.ValueItems[last] = true
	return t, true
}

func (c *Checker) ifExpr(e *syntax.IfExpr, want Type) Type {
	if e.Else == nil {
		c.openScope()
		c.cond(e.Cond)
		c.valueBlock(e.Then, want)
		c.closeScope()
		if c.styleValue > 0 {
			// Elsewhere the parser said so.
			c.errorf(e, diag.NoValue, "an if used as a value needs an else")
		}
		return Invalid
	}
	return c.ifValue(e, e.Cond, e.Then, e.Else, want)
}

// ifValue checks an if as a value (an IfExpr, or an IfStmt that's its
// block's value): its condition, then each branch's block as a value;
// els is an *IfExpr, an *IfStmt or a *Block.
func (c *Checker) ifValue(n syntax.Node, cond syntax.Expr, then *syntax.Block, els syntax.Node, want Type) Type {
	c.openScope()
	c.cond(cond)
	a := c.valueBlock(then, want)
	c.closeScope()
	var b Type
	switch el := els.(type) {
	case *syntax.IfExpr:
		b = c.expr(el, want)
	case *syntax.IfStmt:
		b = c.ifValue(el, el.Cond, el.Then, el.Else, want)
	case *syntax.Block:
		b = c.valueBlock(el, want)
	}
	if want != nil {
		return want
	}
	t := c.join(a, b)
	if t == nil {
		c.errorf(n, diag.TypeMismatch, "the branches are %s and %s: make them the same type", aType(a), aType(b))
		return Invalid
	}
	return t
}

// implicitRoot reports a chain that starts with an implicit member
// (.largeTitle.bold()): what's expected of the chain names its type.
func implicitRoot(x syntax.Expr) bool {
	for {
		switch y := x.(type) {
		case *syntax.ImplicitMember:
			return true
		case *syntax.MemberExpr:
			x = y.X
		case *syntax.CallExpr:
			if _, ok := y.Fun.(*syntax.ImplicitMember); ok {
				return true
			}
			x = y.Fun
		default:
			return false
		}
	}
}

// IsConstNumber reports a number the compiler computes (a literal, and
// arithmetic on literals): a Length of it is packed when compiling.
func IsConstNumber(e syntax.Expr) bool { return constNumber(e) }

// atExpr checks a compile-time constant: @platform, the platform the
// program is compiled for (a Platform). Its value isn't the checker's:
// every branch is checked whatever it is, and lowering folds it.
func (c *Checker) atExpr(e *syntax.AtExpr) Type {
	if e.Name != "platform" {
		c.errorf(e, diag.UndefinedName, "no compile-time constant @%s: there's @platform, the platform the app is compiled for", e.Name)
		return Invalid
	}
	pt := c.prelude.Scope.LookupLocal("Platform").(*TypeName)
	c.resolveType(pt)
	return pt.Type
}

// IsPlatformConst reports whether e is @platform (in parentheses or not).
func IsPlatformConst(e syntax.Expr) bool {
	for {
		p, ok := e.(*syntax.ParenExpr)
		if !ok {
			break
		}
		e = p.X
	}
	a, ok := e.(*syntax.AtExpr)
	return ok && a.Name == "platform"
}
