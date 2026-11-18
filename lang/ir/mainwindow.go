package ir

import (
	"github.com/egoist/asuka/lang/check"
	"github.com/egoist/asuka/lang/syntax"
)

// The app's main window (ui.main_kind, the core's AK_MAIN_*): the window
// its app block's content shows in, the app block's Window, or none.
const (
	mainContent = iota
	mainWindow
	mainNone
)

// nonViews are the prelude's elements that show nothing in the window
// they're declared in: a window of their own, the app's menu bar, an
// alert over the window in use, what a window's own keys and close say.
var nonViews = map[string]bool{
	"Window": true, "MenuBar": true, "MenuItem": true, "MenuSeparator": true,
	"Alert": true, "WindowCloseGuard": true, "WindowKeys": true,
}

// fragments are the prelude's elements that show their children and
// nothing of their own: views when their children are.
var fragments = map[string]bool{"Boundary": true, "Router": true, "Route": true}

// mainKind is what the app block's items make of its main window: its
// first Window's, the window its content shows in when anything of it
// may show a view, or none when nothing does (only windows, tray icons,
// menus, hotkeys and the like). It's decided as the app compiles, by
// what the code may show on the platform it compiles for (an if on
// @platform takes its branch), not by what its state shows at the time.
func (b *fb) mainKind(views []syntax.Item) int {
	if b.mainWindow != nil {
		return mainWindow
	}
	a := viewsAnalysis{l: b.l, info: b.l.info, active: map[*check.TypeName]bool{}}
	if a.items(views, nil) {
		return mainContent
	}
	return mainNone
}

// viewsAnalysis finds whether items may show a view in the window
// they're in.
type viewsAnalysis struct {
	l      *Lowerer
	info   *check.Info
	active map[*check.TypeName]bool // the components being looked into (a cycle shows nothing more)
}

// slotScope is a component call's: what its slots are given, and the
// scope its caller's items are in.
type slotScope struct {
	call *check.ElementCall
	up   *slotScope
}

func (a *viewsAnalysis) items(items []syntax.Item, sc *slotScope) bool {
	for _, it := range items {
		if a.item(it, sc) {
			return true
		}
	}
	return false
}

func (a *viewsAnalysis) item(it syntax.Item, sc *slotScope) bool {
	if a.info.BodyCalls[it] {
		return false // a registration: useKeys(…), onBack { … }
	}
	switch s := it.(type) {
	case *syntax.ExprStmt:
		return a.expr(s.X, sc)
	case *syntax.IfStmt:
		return a.ifStmt(s, sc)
	case *syntax.ForStmt:
		return a.items(s.Body.Items, sc)
	case *syntax.MatchStmt:
		for _, arm := range s.Arms {
			if a.node(arm.Body, sc) {
				return true
			}
		}
		return false
	}
	return false // a declaration, a prop, a handler: no view
}

func (a *viewsAnalysis) ifStmt(s *syntax.IfStmt, sc *slotScope) bool {
	// A condition known as the app compiles (@platform's): the branch the
	// platform has.
	if f := a.l.foldCond(s.Cond); f.known {
		if f.val {
			return a.items(s.Then.Items, sc)
		}
		return s.Else != nil && a.node(s.Else, sc)
	}
	if a.items(s.Then.Items, sc) {
		return true
	}
	return s.Else != nil && a.node(s.Else, sc)
}

func (a *viewsAnalysis) node(n syntax.Node, sc *slotScope) bool {
	switch n := n.(type) {
	case *syntax.Block:
		return a.items(n.Items, sc)
	case *syntax.IfStmt:
		return a.ifStmt(n, sc)
	case syntax.Item:
		return a.item(n, sc)
	}
	return true
}

// expr is a view item's expression: an element, a component, a slot.
func (a *viewsAnalysis) expr(x syntax.Expr, sc *slotScope) bool {
	x = unparen(x)
	if id, ok := x.(*syntax.Ident); ok {
		if m, ok := a.info.Uses[id].(*check.Member); ok && m.IsSlot() {
			return a.slot(m, sc)
		}
		return true
	}
	e, ok := x.(*syntax.CallExpr)
	if !ok {
		return true
	}
	ec := a.info.Elements[e]
	if ec == nil {
		return true // a function that returns a view
	}
	comp := ec.Comp.Comp
	if comp.Element != nil {
		name := ec.Comp.Name()
		if nonViews[name] {
			return false
		}
		if fragments[name] {
			return a.given(ec, sc)
		}
		return true
	}
	if comp.Native || comp.Body == nil {
		return true // a native component: a view of the platform's
	}
	if a.active[ec.Comp] {
		return false
	}
	a.active[ec.Comp] = true
	defer delete(a.active, ec.Comp)
	return a.items(comp.Body.Items, &slotScope{call: ec, up: sc})
}

// given is whether what a call's slots are given may show a view: its
// children, its slot blocks, its view props.
func (a *viewsAnalysis) given(ec *check.ElementCall, sc *slotScope) bool {
	if a.items(ec.Children, sc) {
		return true
	}
	for _, s := range ec.Slots {
		if s.Block != nil && a.items(s.Block.Items, sc) {
			return true
		}
	}
	for _, p := range ec.Props {
		if p.Member != nil && p.Member.IsSlot() && a.expr(p.Value, sc) {
			return true
		}
	}
	return false
}

// slot is whether the slot m, shown in a component's body, may show a
// view: what its call gives it (in its caller's scope), else its default.
func (a *viewsAnalysis) slot(m *check.Member, sc *slotScope) bool {
	if sc == nil {
		return true
	}
	ec := sc.call
	if m.Name() == "children" && len(ec.Children) > 0 {
		return a.items(ec.Children, sc.up)
	}
	for _, s := range ec.Slots {
		if s.Prop == m && s.Block != nil {
			return a.items(s.Block.Items, sc.up)
		}
	}
	for i := len(ec.Props) - 1; i >= 0; i-- {
		if p := ec.Props[i]; p.Member == m {
			return a.expr(p.Value, sc.up)
		}
	}
	if m.Default == nil {
		return false
	}
	if _, none := unparen(m.Default).(*syntax.NoneLit); none {
		return false
	}
	return a.expr(m.Default, sc)
}
