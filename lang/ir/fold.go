package ir

import (
	"github.com/egoist/asuka/lang/bytecode"
	"github.com/egoist/asuka/lang/check"
	"github.com/egoist/asuka/lang/diag"
	"github.com/egoist/asuka/lang/syntax"
	"github.com/egoist/asuka/lang/target"
)

// Folding: @platform is a constant, the platform the program is
// compiled for (Options.Platform), @debug another, whether it's a
// development build (Options.Debug), and a bool @define a third, the
// build's (the checker's Info.Defines). A condition they decide is folded
// (@platform == .linux, @debug, !, && and || of such, true and false),
// and only the branch it takes is lowered: an if's, an else if's, a
// match's arm of @platform, a view's if. The other branches were
// checked, but nothing of them is in the module: no code, no strings,
// no builtins.

// folded is what a condition comes to: known, its value; effects are
// what's still evaluated first, for what it does (f() in f() && false).
type folded struct {
	known, val bool
	effects    []effect
}

// effect is an expression evaluated for what it does. With then, it's a
// bool that short-circuits as the && or || it came from: then runs only
// when x is when (f() && (g() && false): g() runs when f() is true).
type effect struct {
	x    syntax.Expr
	when bool
	then []effect
}

// platform is the case of the prelude's Platform @platform is.
func (l *Lowerer) platform() string { return target.Platform(l.opts.Platform).Or().Case() }

// platformCase is the case of Platform e is, when it's known as the
// program compiles: @platform, or a case written out (.linux,
// Platform.linux).
func (l *Lowerer) platformCase(e syntax.Expr) (string, bool) {
	e = unparen(e)
	if check.IsPlatformConst(e) {
		return l.platform(), true
	}
	switch e.(type) {
	case *syntax.ImplicitMember, *syntax.MemberExpr:
	default:
		return "", false
	}
	sel := l.info.Selections[e]
	if sel == nil || sel.Kind != check.SelCase || sel.Case == nil || !isPlatform(l.info.Types[e]) {
		return "", false
	}
	return sel.Case.Name, true
}

// isPlatform reports whether t is the prelude's Platform.
func isPlatform(t check.Type) bool {
	n, ok := t.(*check.Named)
	return ok && n.Obj.Pkg != nil && n.Obj.Pkg.Prelude && n.Obj.Name() == "Platform"
}

// fold folds the bool e, as far as @platform decides it.
func (l *Lowerer) fold(e syntax.Expr) folded {
	switch x := e.(type) {
	case *syntax.ParenExpr:
		return l.fold(x.X)
	case *syntax.BoolLit:
		return folded{known: true, val: x.Value}
	case *syntax.AtExpr:
		if check.IsDebugConst(x) {
			return folded{known: true, val: l.opts.Debug}
		}
		if v, ok := l.info.Defines[x].(bool); ok {
			return folded{known: true, val: v}
		}
	case *syntax.UnaryExpr:
		if x.Op == syntax.NOT {
			if f := l.fold(x.X); f.known {
				f.val = !f.val
				return f
			}
		}
	case *syntax.BinaryExpr:
		if l.info.Operators[x] != nil {
			break
		}
		switch x.Op {
		case syntax.EQL, syntax.NEQ:
			if !check.IsPlatformConst(x.X) && !check.IsPlatformConst(x.Y) {
				break
			}
			a, aok := l.platformCase(x.X)
			b, bok := l.platformCase(x.Y)
			if aok && bok {
				return folded{known: true, val: (a == b) == (x.Op == syntax.EQL)}
			}
		case syntax.LAND, syntax.LOR:
			decides := x.Op == syntax.LOR // the value of X that's the whole's: false for &&, true for ||
			fx := l.fold(x.X)
			if fx.known {
				if fx.val == decides {
					return fx
				}
				if fy := l.fold(x.Y); fy.known {
					fy.effects = append(append([]effect{}, fx.effects...), fy.effects...)
					return fy
				}
				return folded{}
			}
			if fy := l.fold(x.Y); fy.known && fy.val == decides {
				// f() && false: f() runs, and the whole is false; what
				// the right side still runs, only when f() doesn't decide.
				return folded{known: true, val: decides, effects: []effect{{x: x.X, when: !decides, then: fy.effects}}}
			}
		}
	}
	return folded{}
}

// foldCond folds an if's condition: a bool, or a chain of them and lets
// (if @platform == .linux, let x = y), as far as its clauses before the
// first let decide it.
func (l *Lowerer) foldCond(c syntax.Expr) folded {
	list, ok := c.(*syntax.CondList)
	if !ok {
		if _, let := c.(*syntax.LetCond); let {
			return folded{}
		}
		return l.fold(c)
	}
	all := folded{known: true, val: true}
	for _, x := range list.List {
		if _, let := x.(*syntax.LetCond); let {
			return folded{}
		}
		f := l.fold(x)
		if !f.known {
			return folded{}
		}
		all.effects = append(all.effects, f.effects...)
		if !f.val {
			all.val = false
			return all
		}
	}
	return all
}

// leaves reports whether control never gets past it, as it's lowered:
// a return, a break, a continue, a throw, what has no value (panic(…)),
// and an if or a match whose every branch that may run leaves, an if
// @debug's only the one it takes. What comes after it in its block is
// never lowered: a release has nothing of what follows if !@debug
// { return … }, its strings and builtins included, as it has nothing of
// a branch it doesn't take.
func (l *Lowerer) leaves(it syntax.Item) bool {
	switch s := it.(type) {
	case *syntax.ReturnStmt, *syntax.ThrowStmt, *syntax.BranchStmt:
		return true
	case *syntax.ExprStmt:
		return l.info.Types[s.X] == check.Never
	case *syntax.IfStmt:
		if f := l.foldCond(s.Cond); f.known {
			if f.val {
				return l.blockLeaves(s.Then)
			}
			return l.elseLeaves(s.Else)
		}
		return s.Else != nil && l.blockLeaves(s.Then) && l.elseLeaves(s.Else)
	case *syntax.MatchStmt:
		arms := l.platformArms(s.Subject, s.Arms)
		if len(arms) == 0 {
			return false
		}
		mi := l.info.Matches[s]
		if !(mi != nil && mi.Exhaustive) && !(check.IsPlatformConst(s.Subject) && arms[len(arms)-1].Guard == nil) {
			return false
		}
		for _, a := range arms {
			switch body := a.Body.(type) {
			case *syntax.Block:
				if !l.blockLeaves(body) {
					return false
				}
			case syntax.Item:
				if !l.leaves(body) {
					return false
				}
			default:
				return false
			}
		}
		return true
	case *syntax.DoStmt:
		if !l.blockLeaves(s.Body) {
			return false
		}
		for _, c := range s.Catches {
			if !l.blockLeaves(c.Body) {
				return false
			}
		}
		return true
	}
	return false
}

// blockLeaves reports whether control never gets to a block's end: one
// of its statements leaves.
func (l *Lowerer) blockLeaves(bl *syntax.Block) bool {
	if bl == nil {
		return false
	}
	for _, it := range bl.Items {
		if l.leaves(it) {
			return true
		}
	}
	return false
}

// elseLeaves is blockLeaves of an if's else: another if, or a block.
func (l *Lowerer) elseLeaves(el syntax.Node) bool {
	switch el := el.(type) {
	case *syntax.IfStmt:
		return l.leaves(el)
	case *syntax.Block:
		return l.blockLeaves(el)
	}
	return false
}

// effects evaluates what a folded condition still runs.
func (b *fb) effects(f folded) { b.runEffects(f.effects) }

func (b *fb) runEffects(es []effect) {
	for _, e := range es {
		if len(e.then) == 0 {
			b.value(e.x, -1)
			continue
		}
		c := b.value(e.x, -1)
		skip := b.f.NewLabel()
		if e.when {
			b.emitJump(op("JF"), c, skip)
		} else {
			b.emitJump(op("JT"), c, skip)
		}
		b.runEffects(e.then)
		b.f.Place(skip)
	}
}

// constBool is a bool constant in dst.
func (b *fb) constBool(v bool, dst int) int {
	r := b.out(dst, bytecode.KBool)
	k := 0
	if v {
		k = 1
	}
	b.emit(op("KSMALL"), r, k)
	return r
}

// platformArms are the arms of a match of @platform that may run: those
// up to the first that matches it without a guard, without those that
// can't. Another match's arms are all.
func (l *Lowerer) platformArms(subject syntax.Expr, arms []*syntax.MatchArm) []*syntax.MatchArm {
	if !check.IsPlatformConst(subject) {
		return arms
	}
	p := l.platform()
	var out []*syntax.MatchArm
	for _, a := range arms {
		if !l.mayMatchCase(a.Pattern, p) {
			continue
		}
		out = append(out, a)
		if a.Guard == nil {
			break
		}
	}
	return out
}

// mayMatchCase reports whether pattern p may match the payloadless case
// named name.
func (l *Lowerer) mayMatchCase(p syntax.Pattern, name string) bool {
	switch p := p.(type) {
	case *syntax.CasePat:
		if k := l.info.Cases[p]; k != nil && len(p.Args) == 0 {
			return k.Name == name
		}
	case *syntax.OrPat:
		for _, a := range p.Alts {
			if l.mayMatchCase(a, name) {
				return true
			}
		}
		return false
	}
	return true
}

// atValue is a compile-time constant as a value: @platform's case,
// @debug's bool, or a @define's value (the checker's: the build's, or
// its default).
func (b *fb) atValue(e *syntax.AtExpr, dst int) int {
	if check.IsDebugConst(e) {
		return b.constBool(b.l.opts.Debug, dst)
	}
	if e.Name == "define" {
		switch v := b.l.info.Defines[e].(type) {
		case bool:
			return b.constBool(v, dst)
		case string:
			r := b.out(dst, bytecode.KRef)
			b.emit(op("KSTR"), r, b.l.str(v))
			return r
		case int64:
			r := b.out(dst, bytecode.KInt)
			b.loadInt(r, v)
			return r
		case float64:
			r := b.out(dst, bytecode.KFloat)
			b.emit(op("KFLOAT"), r, b.l.floatConst(v))
			return r
		}
		b.l.errorf(e, diag.InternalError, "internal: @define has no value")
		return b.valueReg(-1, bytecode.KInt)
	}
	return b.platformValue(e, dst)
}

// platformValue is @platform as a value: the case of Platform the
// program is compiled for.
func (b *fb) platformValue(e *syntax.AtExpr, dst int) int {
	t := b.l.info.Types[e]
	if n, ok := t.(*check.Named); ok {
		for _, k := range n.Obj.Cases {
			if k.Name == b.l.platform() {
				return b.caseValue(t, k, dst)
			}
		}
	}
	b.l.errorf(e, diag.InternalError, "internal: no Platform case %s", b.l.platform())
	return b.valueReg(-1, bytecode.KInt)
}
