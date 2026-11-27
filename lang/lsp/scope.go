package lsp

import (
	"github.com/egoist/asuka/lang/check"
	"github.com/egoist/asuka/lang/syntax"
)

// The names visible at a position, innermost first: what the checker's
// scopes held there, found again from the tree (the checker keeps its
// scopes only while it checks).

// A scoped name is a name in scope and how near it's declared.
type scoped struct {
	name  string
	obj   check.Object
	field *check.Field       // a struct's field, by its bare name in a method
	imeth *check.IfaceMethod // an interface's method, by its bare name in a provided one
	owner *check.TypeName
	depth int // 0: a local; 1: a member; 2: the package's; 3: the prelude's; 4: the universe's
	self  bool
}

// namesAt lists the names in scope at the end of path (pos).
func (fc *fileCtx) namesAt(path []syntax.Node, pos syntax.Pos) []scoped {
	var out []scoped
	seen := map[string]bool{}
	add := func(s scoped) {
		if s.name == "" || s.name == "_" || seen[s.name] {
			return
		}
		seen[s.name] = true
		out = append(out, s)
	}
	addIdent := func(id *syntax.Ident, depth int) {
		if id == nil {
			return
		}
		if o := fc.info.Defs[id]; o != nil {
			add(scoped{name: id.Name, obj: o, depth: depth})
		}
	}
	addPattern := func(p syntax.Node) {
		if p == nil || isNil(p) {
			return
		}
		syntax.Inspect(p, func(n syntax.Node) bool {
			if bp, ok := n.(*syntax.BindPat); ok {
				addIdent(bp.Name, 0)
			}
			return true
		})
	}
	within := func(n syntax.Node) bool {
		if n == nil || isNil(n) {
			return false
		}
		r := n.Range()
		return r.Start <= pos && pos <= r.End
	}
	// condNames adds what a condition's lets bind: all of them in its
	// body, and in a chain, those of the clauses before pos.
	condNames := func(cond syntax.Expr, inBody bool) {
		clauses := []syntax.Expr{cond}
		if l, ok := cond.(*syntax.CondList); ok {
			clauses = l.List
		}
		for i := len(clauses) - 1; i >= 0; i-- {
			lc, ok := clauses[i].(*syntax.LetCond)
			if ok && (inBody || lc.End < pos) {
				addPattern(lc.Pattern)
			}
		}
	}
	// Innermost first, so a local hides an outer name.
	for i := len(path) - 1; i >= 0; i-- {
		switch n := path[i].(type) {
		case *syntax.Block:
			// In a fn declared in the block, it and the fns declared
			// after it, next to it, are in scope too (they may call
			// each other).
			in := -1
			for j, it := range n.Items {
				if r := it.Range(); r.Start <= pos && pos <= r.End {
					if _, ok := it.(*syntax.FuncDecl); ok {
						in = j
					}
				}
			}
			for j := len(n.Items) - 1; j >= 0; j-- {
				it := n.Items[j]
				if d, ok := it.(*syntax.FuncDecl); ok && in >= 0 && j >= in {
					run := true
					for k := in; k <= j; k++ {
						if _, ok := n.Items[k].(*syntax.FuncDecl); !ok {
							run = false
						}
					}
					if run {
						addIdent(d.Name, 0)
					}
				}
				if it.Range().End > pos {
					continue
				}
				switch d := it.(type) {
				case *syntax.LetDecl:
					addPattern(d.Pattern)
				case *syntax.FuncDecl:
					addIdent(d.Name, 0)
				}
			}
		case *syntax.LambdaExpr:
			for _, p := range n.Params {
				addIdent(p.Name, 0)
			}
		case *syntax.FuncDecl:
			if n.Params != nil {
				for _, p := range n.Params.List {
					addIdent(p.Name, 0)
				}
			}
			for _, tp := range n.TypeParams {
				addIdent(tp.Name, 0)
			}
		case *syntax.OnHandler:
			for _, p := range n.Params {
				addIdent(p, 0)
			}
		case *syntax.ForStmt:
			if within(n.Body) || within(n.Key) {
				addPattern(n.Pattern)
			}
		case *syntax.MatchArm:
			if within(n.Body) || within(n.Guard) {
				addPattern(n.Pattern)
			}
		case *syntax.IfStmt:
			condNames(n.Cond, within(n.Then))
		case *syntax.IfExpr:
			condNames(n.Cond, within(n.Then))
		case *syntax.WhileStmt:
			condNames(n.Cond, within(n.Body))
		case *syntax.CatchClause:
			if within(n.Body) {
				addPattern(n.Pattern)
			}
		case *syntax.ComponentDecl, *syntax.ScreenDecl, *syntax.StoreDecl, *syntax.WorkerDecl, *syntax.PreviewDecl:
			if tn := fc.declType(n); tn != nil && tn.Comp != nil {
				for _, m := range tn.Comp.Order {
					add(scoped{name: m.Name(), obj: m, owner: tn, depth: 1})
				}
			}
			if d, ok := n.(*syntax.ComponentDecl); ok {
				for _, tp := range d.TypeParams {
					addIdent(tp.Name, 1)
				}
			}
		case *syntax.StructDecl, *syntax.EnumDecl:
			// In a method: the type's fields and methods by their bare
			// names, and self.
			inMethod := i+1 < len(path)
			if inMethod {
				if _, ok := path[i+1].(*syntax.FuncDecl); !ok {
					inMethod = false
				}
			}
			tn := fc.declType(n)
			if tn == nil || !inMethod {
				break
			}
			add(scoped{name: "self", owner: tn, depth: 1, self: true})
			for _, f := range tn.Fields {
				add(scoped{name: f.Name, field: f, owner: tn, depth: 1})
			}
			for name, f := range tn.Methods {
				add(scoped{name: name, obj: f, owner: tn, depth: 1})
			}
			if d, ok := n.(*syntax.StructDecl); ok {
				for _, tp := range d.TypeParams {
					addIdent(tp.Name, 1)
				}
			}
			if d, ok := n.(*syntax.EnumDecl); ok {
				for _, tp := range d.TypeParams {
					addIdent(tp.Name, 1)
				}
			}
		case *syntax.InterfaceDecl:
			// In a provided method: the interface's fields and methods
			// by their bare names, and self.
			if i+1 >= len(path) {
				break
			}
			if _, ok := path[i+1].(*syntax.FuncDecl); !ok {
				break
			}
			tn := fc.declType(n)
			if tn == nil || tn.Iface == nil {
				break
			}
			add(scoped{name: "self", owner: tn, depth: 1, self: true})
			for _, f := range tn.Iface.Fields {
				add(scoped{name: f.Name, field: f, owner: tn, depth: 1})
			}
			for _, m := range tn.Iface.Methods {
				add(scoped{name: m.Name, imeth: m, owner: tn, depth: 1})
			}
			for _, tp := range n.TypeParams {
				addIdent(tp.Name, 1)
			}
		}
	}
	// The file's imports, the package's names, the prelude's, the
	// universe's.
	if fc.pkg != nil {
		depth := 2
		var sc *check.Scope
		if fs := fc.pkg.FileScope(fc.file); fs != nil {
			sc = fs
		} else {
			sc = fc.pkg.Scope
		}
		for ; sc != nil; sc = sc.Parent() {
			if sc == fc.prog.Prelude.Scope {
				depth = 3
			} else if sc.Parent() == nil {
				depth = 4
			}
			for _, name := range sc.Names() {
				o := sc.LookupLocal(name)
				if tn, ok := o.(*check.TypeName); ok && tn.Comp != nil && tn.Comp.Style {
					continue // not an element: its props are every element's
				}
				add(scoped{name: name, obj: o, depth: depth})
			}
		}
	}
	return out
}
