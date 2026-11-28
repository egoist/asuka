package lsp

import (
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"

	"github.com/egoist/asuka/internal/pkgs"
	"github.com/egoist/asuka/lang/check"
	"github.com/egoist/asuka/lang/syntax"
	"github.com/egoist/asuka/std"
)

func (s *server) completion(p *CompletionParams) (*CompletionList, error) {
	list := &CompletionList{Items: []CompletionItem{}}
	fc, err := s.fileAt(p.TextDocument.URI)
	if err != nil {
		return list, nil
	}
	c := &completer{fc: fc, s: s, list: list, seen: map[string]bool{}}
	c.off = fc.m.offset(p.Position)
	c.run()
	return list, nil
}

type completer struct {
	fc   *fileCtx
	s    *server
	list *CompletionList
	seen map[string]bool

	off        int // the cursor
	start, end int // the word at the cursor
	prefix     string
}

// Sort groups: lower first.
const (
	prioMember = iota // what the context asks for: a prop, a field, a case
	prioLocal
	prioOwn // the component's or the type's members
	prioPkg
	prioPrelude
	prioUniverse
	prioKeyword
)

func (c *completer) add(it CompletionItem, prio int) {
	key := it.Label
	if c.seen[key] {
		return
	}
	c.seen[key] = true
	if it.SortText == "" {
		it.SortText = fmt.Sprintf("%d_%s", prio, it.Label)
	}
	if it.InsertTextFormat == 2 && !c.s.snippets {
		it.InsertText = plainSnippet(it.InsertText)
		it.InsertTextFormat = 1
	}
	c.list.Items = append(c.list.Items, it)
}

var placeholder = regexp.MustCompile(`\$\{\d+:([^}]*)\}|\$\{\d+\}|\$\d+`)

// plainSnippet is a snippet without its tab stops, for clients without
// snippets.
func plainSnippet(s string) string {
	return placeholder.ReplaceAllStringFunc(s, func(m string) string {
		if i := strings.IndexByte(m, ':'); i >= 0 {
			return m[i+1 : len(m)-1]
		}
		return ""
	})
}

var (
	importLine = regexp.MustCompile(`^\s*import\s+([\w@./-]*)$`)
	// import std/time.{Time, |: the names imported unqualified.
	importNamesLine = regexp.MustCompile(`^\s*import\s+[\w@./-]+\.\{[\w\s,]*$`)
	onLine          = regexp.MustCompile(`(^|[;{])\s*on\s+$`)
	bindLine        = regexp.MustCompile(`(^|[;{])\s*bind\s+$`)
	styleLine       = regexp.MustCompile(`(^|[;{])\s*style\s*:\s*(\[([^\]]*,)?\s*)?$`) // style: …, style: [a, …
	emitLine        = regexp.MustCompile(`(^|[;{])\s*emit\s+$`)
	propLine        = regexp.MustCompile(`(^|[;{])\s*([A-Za-z_]\w*)\s*:\s*$`)
)

func (c *completer) run() {
	fc := c.fc
	src := fc.m.src
	c.start, c.end = c.off, c.off
	for c.start > 0 && isWordByte(src[c.start-1]) {
		c.start--
	}
	c.end = wordEnd(src, c.off)
	c.prefix = string(src[c.start:c.off])
	pos := syntax.Pos(fc.file.File.Base() + c.off)
	if c.inCommentOrString(pos) {
		return
	}
	lineStart := c.start
	for lineStart > 0 && src[lineStart-1] != '\n' {
		lineStart--
	}
	line := string(src[lineStart:c.off])
	if m := importLine.FindStringSubmatch(line); m != nil {
		c.imports(m[1])
		return
	}
	if importNamesLine.MatchString(line) {
		c.importNames(syntax.Pos(fc.file.File.Base() + c.off))
		return
	}
	before := string(src[lineStart:c.start])
	if c.start > 0 && src[c.start-1] == '@' {
		// @platform, @debug and @define, the compile-time constants.
		if strings.HasPrefix("platform", c.prefix) {
			c.add(CompletionItem{Label: "platform", Kind: 21, Detail: "@platform: Platform",
				Documentation: &MarkupContent{Kind: "markdown", Value: platformDoc(fc.st.platform)}}, prioMember)
		}
		if strings.HasPrefix("debug", c.prefix) {
			c.add(CompletionItem{Label: "debug", Kind: 21, Detail: "@debug: bool",
				Documentation: &MarkupContent{Kind: "markdown", Value: debugDoc}}, prioMember)
		}
		if strings.HasPrefix("define", c.prefix) {
			c.add(CompletionItem{Label: "define", Kind: 21, Detail: "@define(\"KEY\")",
				Documentation: &MarkupContent{Kind: "markdown", Value: defineDoc}}, prioMember)
		}
		return
	}
	if c.start > 0 && src[c.start-1] == '.' {
		dot := c.start - 1
		if dot > 0 && src[dot-1] == '.' {
			// A range: a..b.
		} else if dot > 0 && receiverEnd(src[dot-1]) {
			c.members(dot)
			return
		} else {
			c.implicitMembers(dot)
			return
		}
	}
	path := pathTo(fc.file, pos)
	switch {
	case onLine.MatchString(before):
		c.events(path, false)
		return
	case bindLine.MatchString(before):
		c.binds(path, false)
		return
	case styleLine.MatchString(before):
		c.styles()
		return
	case emitLine.MatchString(before):
		c.emits(path)
		return
	}
	c.general(path, pos, before)
}

// receiverEnd reports whether b can end the value a member is taken of:
// x.name, f().name, xs[0].name, "s".name, x!.name, x?.name.
func receiverEnd(b byte) bool {
	return isWordByte(b) || b == ')' || b == ']' || b == '"' || b == '!' || b == '?' || b == '}'
}

func (c *completer) inCommentOrString(pos syntax.Pos) bool {
	for _, cm := range c.fc.file.Comments {
		if cm.Pos < pos && pos <= cm.End() {
			if strings.HasPrefix(cm.Text, "//") || pos < cm.End() {
				return true
			}
		}
	}
	path := pathTo(c.fc.file, pos)
	for i := len(path) - 1; i >= 0; i-- {
		s, ok := path[i].(*syntax.StringLit)
		if !ok {
			continue
		}
		if pos <= s.Start || pos >= s.End {
			return false // at its quotes: outside
		}
		for _, x := range s.Interps {
			if r := x.Range(); r.Start <= pos && pos <= r.End {
				return false
			}
		}
		return true
	}
	return false
}

// ---------------------------------------------------------------------
// x.name

// members completes what follows x. (or x?.): fields, methods, cases.
func (c *completer) members(dot int) {
	fc := c.fc
	src := fc.m.src
	optional := src[dot-1] == '?'
	recvEnd := dot
	if optional {
		recvEnd = dot - 1
	}
	endPos := syntax.Pos(fc.file.File.Base() + recvEnd)
	// The outermost expression that ends right before the dot.
	var x syntax.Expr
	syntax.Inspect(fc.file, func(n syntax.Node) bool {
		if n == nil || isNil(n) {
			return false
		}
		r := n.Range()
		if r.Start > endPos || r.End < endPos {
			return false
		}
		if e, ok := n.(syntax.Expr); ok && r.End == endPos && r.Start < r.End {
			if x == nil || r.Start < x.Range().Start {
				x = e
			}
		}
		return true
	})
	if x == nil {
		return
	}
	// A package's names, a type's cases and constants.
	switch e := x.(type) {
	case *syntax.Ident:
		switch o := fc.info.Uses[e].(type) {
		case *check.PkgName:
			c.pkgMembers(o.Pkg)
			return
		case *check.TypeName:
			c.staticMembers(o)
			return
		}
		if o := fc.info.Uses[e]; o == nil {
			if pn, ok := fc.lookup(e.Name).(*check.PkgName); ok {
				c.pkgMembers(pn.Pkg)
				return
			}
			if tn, ok := fc.lookup(e.Name).(*check.TypeName); ok {
				c.staticMembers(tn)
				return
			}
		}
	case *syntax.MemberExpr:
		if sel := fc.info.Selections[e]; sel != nil && sel.Kind == check.SelPkg {
			if tn, ok := sel.Obj.(*check.TypeName); ok {
				c.staticMembers(tn)
				return
			}
		}
	}
	t := fc.info.Types[x]
	if t == nil || check.IsInvalid(t) {
		return
	}
	c.valueMembers(t, optional, dot)
}

// valueMembers completes the members of a value of type t.
func (c *completer) valueMembers(t check.Type, optional bool, dot int) {
	inner := t
	var edit func(name string) *TextEdit
	if opt, ok := t.(*check.Optional); ok {
		inner = opt.Elem
		if !optional {
			// x.isNone(), and x?.name for what's inside: the dot becomes ?.
			for _, m := range check.MethodsOf(t) {
				c.method(m, t)
			}
			rg := c.fc.m.rng(dot, c.end)
			edit = func(name string) *TextEdit {
				return &TextEdit{Range: rg, NewText: "?." + name}
			}
		}
	}
	addItem := func(it CompletionItem) {
		if edit != nil {
			te := edit(it.InsertText)
			if it.InsertText == "" {
				te = edit(it.Label)
			}
			it.TextEdit = te
			it.FilterText = "." + it.Label
			it.InsertText = ""
			if it.InsertTextFormat == 2 && !c.s.snippets {
				te.NewText = plainSnippet(te.NewText)
				it.InsertTextFormat = 1
			}
		}
		c.add(it, prioMember)
	}
	if p, ok := inner.(*check.TypeParam); ok {
		// A constrained type parameter: its interfaces' members.
		for _, i := range p.Ifaces {
			tn := i.Obj
			if tn.Iface == nil {
				continue
			}
			s := i.Subst()
			for _, f := range tn.Iface.Fields {
				addItem(CompletionItem{Label: f.Name, Kind: kindField, Detail: typeStr(s.Apply(f.Type))})
			}
			for _, m := range tn.Iface.Methods {
				addItem(c.callItem(m.Name, kindMethod, imethSig(m), "", m.Sig != nil && len(m.Sig.Params) > 0))
			}
		}
	}
	if tt, ok := inner.(*check.Tuple); ok {
		// Its elements, by place: t.0, t.1.
		for i, e := range tt.Elems {
			addItem(CompletionItem{Label: strconv.Itoa(i), Kind: kindField, Detail: typeStr(e), Documentation: c.doc(tupleElemDoc(i))})
		}
	}
	if n, ok := inner.(*check.Named); ok {
		s := n.Subst()
		tn := n.Obj
		switch tn.Kind {
		case check.StructType:
			for _, f := range tn.Fields {
				if f.Private && c.elsewhere(f.Pos) {
					continue
				}
				addItem(CompletionItem{Label: f.Name, Kind: kindField, Detail: typeStr(s.Apply(f.Type)), Documentation: c.doc(c.fc.declDoc(f.Pos))})
			}
			c.typeMethods(tn, addItem)
		case check.EnumType:
			c.typeMethods(tn, addItem)
			if check.IsLoadingType(tn) && len(n.Args) > 0 {
				addItem(CompletionItem{Label: "value", Kind: kindProperty, Detail: typeStr(check.MakeOptional(n.Args[0])), Documentation: c.doc(loadingValueDoc)})
				addItem(CompletionItem{Label: "loading", Kind: kindProperty, Detail: "bool", Documentation: c.doc(loadingLoadingDoc)})
			}
		case check.StoreType:
			if tn.Comp != nil {
				for _, m := range tn.Comp.Order {
					if m.Kind == check.EventMember || tn.Pkg != c.fc.pkg && !m.Pub {
						continue
					}
					it := c.objectItem(m, "")
					addItem(it)
				}
			}
		case check.WorkerType:
			// Its pub fns: what's called from outside it.
			if tn.Comp != nil {
				for _, m := range tn.Comp.Order {
					if m.Kind == check.FnMember && m.Pub {
						addItem(c.objectItem(m, ""))
					}
				}
			}
		case check.InterfaceType:
			if tn.Iface != nil {
				for _, f := range tn.Iface.Fields {
					addItem(CompletionItem{Label: f.Name, Kind: kindField, Detail: typeStr(f.Type)})
				}
				for _, m := range tn.Iface.Methods {
					addItem(c.callItem(m.Name, kindMethod, imethSig(m), "", m.Sig != nil && len(m.Sig.Params) > 0))
				}
			}
		}
		if tn.IsError || tn.Kind == check.InterfaceType && tn.Name() == "Error" && tn.Pkg == nil {
			addItem(CompletionItem{Label: "message", Kind: kindField, Detail: "string", Documentation: c.doc("The error's message.")})
		}
	}
	for _, m := range check.MethodsOf(inner) {
		if c.seen[m.Name] {
			continue
		}
		addItem(c.methodItem(m, inner))
	}
	// What the extensions the file's package sees add.
	c.extMembers(inner, "method", addItem)
}

// extMembers adds what the extensions the file's package sees give type
// t: its methods, or its static fns and constants (kind "static").
func (c *completer) extMembers(t check.Type, kind string, add func(CompletionItem)) {
	for _, e := range check.VisibleExtensions(c.fc.pkg) {
		if !e.Extends(t) {
			continue
		}
		if kind == "method" {
			for _, f := range e.Order {
				if !e.SeenFrom(c.fc.pkg, f.Decl.Pub.IsValid()) {
					continue
				}
				add(c.callItem(f.Name(), kindMethod, funcSig(f), docOf(f.Decl), f.Sig != nil && len(f.Sig.Params) > 0))
			}
			continue
		}
		for _, f := range e.Statics {
			if e.SeenFrom(c.fc.pkg, f.Decl.Pub.IsValid()) {
				add(c.callItem(f.Name(), kindFunction, funcSig(f), docOf(f.Decl), f.Sig != nil && len(f.Sig.Params) > 0))
			}
		}
		for _, g := range e.Consts {
			if e.SeenFrom(c.fc.pkg, g.Pub) {
				add(CompletionItem{Label: g.Name(), Kind: kindConstant, Detail: typeStr(g.Type), Documentation: c.doc(docOf(g.Decl))})
			}
		}
	}
}

// method adds a built-in method.
func (c *completer) method(m *check.Method, recv check.Type) {
	c.add(c.methodItem(m, recv), prioMember)
}

// methodItem is a built-in method's item: a call, or for one read as a
// property (g.width, card.hovered) its name.
func (c *completer) methodItem(m *check.Method, recv check.Type) CompletionItem {
	if m.Field {
		return CompletionItem{Label: m.Name, Kind: kindProperty, Detail: methodSig(m, recv), Documentation: c.doc(methodDoc(m))}
	}
	ps, _ := m.Instantiate(recv)
	return c.callItem(m.Name, kindMethod, methodSig(m, recv), methodDoc(m), len(ps) > 0)
}

// typeMethods adds a struct's or an enum's methods, sorted.
func (c *completer) typeMethods(tn *check.TypeName, add func(CompletionItem)) {
	names := make([]string, 0, len(tn.Methods))
	for n := range tn.Methods {
		names = append(names, n)
	}
	sort.Strings(names)
	for _, n := range names {
		f := tn.Methods[n]
		if tn.Pkg != c.fc.pkg && (f.Decl == nil || !f.Decl.Pub.IsValid()) && !tn.Pkg.Prelude {
			continue
		}
		if f.Decl != nil && f.Decl.Private.IsValid() && c.elsewhere(f.Pos()) {
			continue
		}
		hasParams := f.Sig != nil && len(f.Sig.Params) > 0
		add(c.callItem(n, kindMethod, funcSig(f), docOf(f.Decl), hasParams))
	}
}

// elsewhere reports whether pos, a private declaration's, is in another
// file than the one completed: its file's only.
func (c *completer) elsewhere(pos syntax.Pos) bool {
	f := c.fc.fset.File(pos)
	return f == nil || f.Name() != c.fc.name
}

// callItem is a function's or a method's item: name(), with the cursor
// in the parentheses when it takes arguments.
func (c *completer) callItem(name string, kind int, detail, doc string, hasParams bool) CompletionItem {
	ins := name + "()$0"
	if hasParams {
		ins = name + "($0)"
	}
	return CompletionItem{Label: name, Kind: kind, Detail: detail, Documentation: c.doc(doc), InsertText: ins, InsertTextFormat: 2}
}

func (c *completer) doc(s string) *MarkupContent {
	if s == "" {
		return nil
	}
	return &MarkupContent{Kind: "markdown", Value: s}
}

// staticMembers completes Type.: an enum's cases, a type's constants.
func (c *completer) staticMembers(tn *check.TypeName) {
	for _, k := range tn.Cases {
		c.caseItem(k, tn)
	}
	c.consts(tn)
	if tn.Type != nil {
		c.extMembers(tn.Type, "static", func(it CompletionItem) { c.add(it, prioMember) })
	}
}

func (c *completer) caseItem(k *check.Case, tn *check.TypeName) {
	it := CompletionItem{Label: k.Name, Kind: kindEnumMember, Detail: tn.Name() + "." + caseString(k), Documentation: c.doc(c.fc.declDoc(k.Pos))}
	if len(k.Params) > 0 {
		var ps []string
		for i, t := range k.Params {
			name := typeStr(t)
			if i < len(k.Labels) && k.Labels[i] != "" {
				name = k.Labels[i] + ": " + "${" + fmt.Sprint(i+1) + "}"
			} else {
				name = "${" + fmt.Sprint(i+1) + ":" + name + "}"
			}
			ps = append(ps, name)
		}
		it.InsertText = k.Name + "(" + strings.Join(ps, ", ") + ")$0"
		it.InsertTextFormat = 2
	}
	c.add(it, prioMember)
}

func (c *completer) consts(tn *check.TypeName) {
	names := make([]string, 0, len(tn.Consts))
	for n := range tn.Consts {
		names = append(names, n)
	}
	sort.Strings(names)
	for _, n := range names {
		g := tn.Consts[n]
		if pos, ok := check.IsPrivate(g); ok && c.elsewhere(pos) {
			continue
		}
		c.add(CompletionItem{Label: n, Kind: kindConstant, Detail: typeStr(g.Type), Documentation: c.doc(docOf(g.Decl))}, prioMember)
	}
}

// pkgMembers completes pkg.: what the package exports.
func (c *completer) pkgMembers(p *check.Package) {
	names := p.Scope.Names()
	sort.Strings(names)
	for _, n := range names {
		o := p.Scope.LookupLocal(n)
		if !check.Exported(p, o) {
			continue
		}
		c.add(c.objectItem(o, ""), prioMember)
	}
}

// ---------------------------------------------------------------------
// .name where a type is expected

func (c *completer) implicitMembers(dot int) {
	fc := c.fc
	dotPos := syntax.Pos(fc.file.File.Base() + dot)
	var want check.Type
	found := false
	var pat *syntax.CasePat
	syntax.Inspect(fc.file, func(n syntax.Node) bool {
		if n == nil || isNil(n) || found {
			return false
		}
		r := n.Range()
		if r.Start > dotPos || r.End < dotPos {
			return false
		}
		switch n := n.(type) {
		case *syntax.ImplicitMember:
			if r.Start == dotPos {
				want, found = fc.info.Expected[n], true
			}
		case *syntax.CasePat:
			if n.Implicit && r.Start == dotPos {
				pat = n
			}
		}
		return true
	})
	if !found && pat != nil {
		want = c.patternType(pat)
	}
	if want == nil {
		return
	}
	c.expectedMembers(want)
}

// patternType is the type a case pattern matches: its match's subject.
func (c *completer) patternType(pat *syntax.CasePat) check.Type {
	path := pathTo(c.fc.file, pat.Start)
	for i := len(path) - 1; i >= 0; i-- {
		switch n := path[i].(type) {
		case *syntax.MatchStmt, *syntax.MatchExpr:
			if mi := c.fc.info.Matches[n]; mi != nil {
				return mi.Subject
			}
			var subj syntax.Expr
			if m, ok := n.(*syntax.MatchStmt); ok {
				subj = m.Subject
			} else {
				subj = n.(*syntax.MatchExpr).Subject
			}
			return c.fc.info.Types[subj]
		case *syntax.CasePat:
			// A pattern inside a case's payload: .ok(.some): unknown.
			if n != pat {
				return nil
			}
		}
	}
	return nil
}

// expectedMembers completes .name for a value of type t.
func (c *completer) expectedMembers(t check.Type) {
	u := check.Unopt(t)
	if _, named := u.(*check.Named); !named && u != nil && !check.IsInvalid(u) {
		// A built-in type's (a named one's are staticMembers').
		c.extMembers(u, "static", func(it CompletionItem) { c.add(it, prioMember) })
	}
	switch u {
	case check.Color:
		for _, n := range check.SemanticColors() {
			c.add(CompletionItem{Label: n, Kind: kindColor, Detail: "Color"}, prioMember)
		}
		return
	case check.Length:
		c.add(CompletionItem{Label: "auto", Kind: kindEnumMember, Detail: "Length", Documentation: c.doc("The size its content and its parent's layout give it.")}, prioMember)
		return
	}
	if n, ok := u.(*check.Named); ok {
		switch n.Obj.Kind {
		case check.EnumType, check.StructType:
			c.staticMembers(n.Obj)
		}
	}
}

// dotted adds .name for a value of type t, where the . isn't typed yet.
func (c *completer) dotted(t check.Type) {
	n := len(c.list.Items)
	c.expectedMembers(t)
	for i := n; i < len(c.list.Items); i++ {
		it := &c.list.Items[i]
		ins := it.InsertText
		if ins == "" {
			ins = it.Label
		}
		it.FilterText = it.Label
		it.Label = "." + it.Label
		it.InsertText = "." + ins
		it.SortText = fmt.Sprintf("%d_%s", prioMember, it.Label)
	}
}

// ---------------------------------------------------------------------
// In a block: props, events, slots, and names

// blockKind is what a block holds.
type blockKind int

const (
	topLevel blockKind = iota
	elementBlock
	componentBody
	storeBody
	workerBody
	viewBlock // views: an app's body, a slot's, an if's in a view
	stmtBlock // statements: a function's, a handler's
	typeBody  // a struct's, an enum's, an interface's
)

// context is what the innermost block around the cursor is, and the
// element whose block it is.
func (c *completer) context(path []syntax.Node) (blockKind, *check.TypeName) {
	for i := len(path) - 1; i >= 0; i-- {
		switch n := path[i].(type) {
		case *syntax.CallExpr:
			if n.Block != nil && i+1 < len(path) && path[i+1] == syntax.Node(n.Block) {
				if tn, _ := c.fc.enclosingElement(path[:i+1]); tn != nil {
					return elementBlock, tn
				}
				return stmtBlock, nil // withTimeout(5s) { … }
			}
		case *syntax.ComponentDecl, *syntax.ScreenDecl, *syntax.PreviewDecl:
			return componentBody, nil
		case *syntax.StoreDecl:
			return storeBody, nil
		case *syntax.WorkerDecl:
			return workerBody, nil
		case *syntax.AppDecl, *syntax.SlotItem:
			return viewBlock, nil
		case *syntax.StyleDecl:
			return elementBlock, c.fc.styleType()
		case *syntax.FuncDecl, *syntax.LambdaExpr, *syntax.OnHandler, *syntax.TaskDecl, *syntax.EffectDecl,
			*syntax.TestDecl, *syntax.BenchDecl, *syntax.MeasureStmt, *syntax.DeferStmt, *syntax.CleanupStmt, *syntax.SpawnExpr, *syntax.DoStmt:
			return stmtBlock, nil
		case *syntax.StructDecl, *syntax.EnumDecl, *syntax.InterfaceDecl:
			return typeBody, nil
		}
	}
	return topLevel, nil
}

// inEffect reports whether the innermost body around the cursor is an
// effect's, where a cleanup { } may go (rfcs/0016).
func inEffect(path []syntax.Node) bool {
	for i := len(path) - 1; i >= 0; i-- {
		switch path[i].(type) {
		case *syntax.EffectDecl:
			return true
		case *syntax.FuncDecl, *syntax.LambdaExpr, *syntax.OnHandler, *syntax.TaskDecl, *syntax.TestDecl, *syntax.BenchDecl,
			*syntax.DeferStmt, *syntax.CleanupStmt, *syntax.SpawnExpr, *syntax.ComponentDecl, *syntax.ScreenDecl, *syntax.StoreDecl, *syntax.AppDecl, *syntax.PreviewDecl:
			return false
		}
	}
	return false
}

// atItemStart reports whether the word is where an item of a block
// starts: first on its line, or after a ; or a {.
func atItemStart(before string) bool {
	t := strings.TrimRight(before, " \t")
	return t == "" || strings.TrimSpace(t) == "" || strings.HasSuffix(t, ";") || strings.HasSuffix(t, "{")
}

var (
	topKeywords       = []string{"component", "screen", "store", "worker", "struct", "enum", "interface", "fn", "async", "type", "style", "state", "let", "app", "test", "bench", "preview", "import", "pub", "native", "extern", "extend"}
	componentKeywords = []string{"prop", "event", "state", "let", "use", "provide", "fn", "async", "task", "effect", "cleanup", "pub"}
	viewKeywords      = []string{"if", "for", "match"}
	stmtKeywords      = []string{"let", "var", "if", "for", "while", "match", "return", "throw", "break", "continue", "defer", "do", "emit", "try", "await"}
	exprKeywords      = []string{"true", "false", "none", "if", "match", "await", "try", "load", "reload", "spawn", "throw", "return"}
)

func (c *completer) keywords(ks []string) {
	for _, k := range ks {
		c.add(CompletionItem{Label: k, Kind: kindKeyword}, prioKeyword)
	}
}

func (c *completer) general(path []syntax.Node, pos syntax.Pos, before string) {
	kind, el := c.context(path)
	item := atItemStart(before)
	if kind == topLevel {
		if item {
			c.keywords(topKeywords)
		}
		return
	}
	if kind == typeBody {
		if item {
			c.keywords([]string{"fn", "mut", "async", "let", "pub"})
		}
		return
	}
	// An argument's label: f(name: …).
	if call, ok := c.argStart(path, before); ok {
		c.labels(call)
	}
	// A prop's value, before its . is typed: variant: .filled.
	if kind == elementBlock && el != nil {
		if m := propLine.FindStringSubmatch(before); m != nil {
			if mem := c.fc.componentMember(el, m[2]); mem != nil && mem.Kind == check.PropMember {
				c.dotted(mem.Type)
			}
		}
	}
	views := false
	switch kind {
	case elementBlock:
		views = true
		if item && el != nil {
			c.elementMembers(path, el)
		}
	case componentBody:
		views = true
		if item {
			c.keywords(componentKeywords)
		}
	case storeBody:
		if item {
			c.keywords([]string{"state", "let", "use", "fn", "async", "task", "effect", "cleanup", "pub"})
		}
	case workerBody:
		if item {
			c.keywords([]string{"var", "let", "fn", "pub"})
		}
	case viewBlock:
		views = true
	}
	if item && views {
		c.keywords(viewKeywords)
		if kind == elementBlock || kind == componentBody {
			c.keywords([]string{"style"})
		}
	}
	if item && kind == stmtBlock {
		c.keywords(stmtKeywords)
		if inEffect(path) {
			c.keywords([]string{"cleanup"})
		}
	}
	c.names(path, pos, item && views)
	c.keywords(exprKeywords)
	if c.selfInScope(path) {
		c.add(CompletionItem{Label: "self", Kind: kindKeyword}, prioKeyword)
	}
}

func (c *completer) selfInScope(path []syntax.Node) bool {
	for i := len(path) - 1; i > 0; i-- {
		if _, ok := path[i].(*syntax.FuncDecl); ok {
			switch path[i-1].(type) {
			case *syntax.StructDecl, *syntax.EnumDecl:
				return true
			}
		}
	}
	return false
}

// names adds what's in scope. In a view, elements and components come
// as views: Text("…"), Column { … }.
func (c *completer) names(path []syntax.Node, pos syntax.Pos, view bool) {
	for _, sc := range c.fc.namesAt(path, pos) {
		if sc.self {
			continue
		}
		prio := prioLocal + sc.depth
		if sc.field != nil {
			c.add(CompletionItem{Label: sc.name, Kind: kindField, Detail: typeStr(sc.field.Type), Documentation: c.doc(c.fc.declDoc(sc.field.Pos))}, prio)
			continue
		}
		if m := sc.imeth; m != nil {
			c.add(c.callItem(m.Name, kindMethod, imethSig(m), "", m.Sig != nil && len(m.Sig.Params) > 0), prio)
			continue
		}
		if m, ok := sc.obj.(*check.Member); ok && m.Kind == check.EventMember {
			continue // not a value: emit it
		}
		if p, ok := check.IsPrivate(sc.obj); ok && c.elsewhere(p) {
			continue // another file's
		}
		if tn, ok := sc.obj.(*check.TypeName); ok && view {
			// A view starts with an element or a component; a type
			// that isn't one starts no view.
			if tn.Comp != nil && !tn.Comp.Store {
				c.add(c.viewItem(tn), prio)
			}
			continue
		}
		if b, ok := sc.obj.(*check.Builtin); ok && view && b.Name() != "push" && b.Name() != "present" {
			continue
		}
		c.add(c.objectItem(sc.obj, ""), prio)
	}
}

// viewItem is a component's item in a view: Text($1), Column {…}.
func (c *completer) viewItem(tn *check.TypeName) CompletionItem {
	it := c.objectItem(tn, "")
	comp := tn.Comp
	var req []string
	for i, m := range comp.Params {
		if m.Default == nil {
			req = append(req, fmt.Sprintf("${%d}", i+1))
		}
	}
	children := false
	if m := comp.Members["children"]; m != nil && m.Kind == check.PropMember {
		children = true
	}
	ins := tn.Name()
	switch {
	case len(req) > 0:
		ins += "(" + strings.Join(req, ", ") + ")"
	case !children:
		ins += "()"
	}
	if children {
		ins += " {\n\t$0\n}"
	} else {
		ins += "$0"
	}
	it.InsertText, it.InsertTextFormat = ins, 2
	return it
}

// objectItem is an object's item.
func (c *completer) objectItem(o check.Object, label string) CompletionItem {
	if label == "" {
		label = o.Name()
	}
	it := CompletionItem{Label: label}
	code, doc := c.fc.describe(c.fc.objRef(o))
	it.Detail = code
	it.Documentation = c.doc(doc)
	switch o := o.(type) {
	case *check.Var:
		it.Kind = kindVariable
		it.Detail = typeStr(o.Type)
	case *check.Global:
		it.Kind = kindVariable
		if o.Kind == check.ConstGlobal {
			it.Kind = kindConstant
		}
		it.Detail = typeStr(o.Type)
	case *check.Member:
		switch o.Kind {
		case check.ParamMember, check.PropMember:
			it.Kind = kindProperty
			it.Detail = typeStr(o.Type)
		case check.FnMember:
			return c.callItem(label, kindMethod, code, doc, o.Fn != nil && o.Fn.Sig != nil && len(o.Fn.Sig.Params) > 0)
		case check.EventMember:
			it.Kind = kindEvent
		default:
			it.Kind = kindVariable
			it.Detail = typeStr(o.Type)
		}
	case *check.Function:
		return c.callItem(label, kindFunction, code, doc, o.Sig == nil || len(o.Sig.Params) > 0)
	case *check.Builtin:
		return c.callItem(label, kindFunction, code, doc, label != "idle" && label != "pop" && label != "dismiss")
	case *check.PkgName:
		it.Kind = kindModule
		it.Detail = "package " + pkgPath(o.Pkg)
	case *check.TypeName:
		switch o.Kind {
		case check.StructType:
			it.Kind = kindStruct
		case check.EnumType:
			it.Kind = kindEnum
		case check.InterfaceType:
			it.Kind = kindInterface
		case check.ParamType:
			it.Kind = kindTypeParameter
		case check.ComponentType, check.StoreType, check.WorkerType:
			it.Kind = kindClass
		default:
			it.Kind = kindClass
		}
		if i := strings.IndexByte(it.Detail, '\n'); i >= 0 {
			it.Detail = it.Detail[:i]
		}
		it.Detail = strings.TrimSuffix(it.Detail, " {")
	}
	return it
}

// elementMembers adds an element's props (name: …), events (on name {
// … }), slots (name { … }) and two-way bindings (bind name: …), less
// those its block sets already.
func (c *completer) elementMembers(path []syntax.Node, tn *check.TypeName) {
	set := c.setInBlock(path)
	comp := tn.Comp
	var members []*check.Member
	members = append(members, comp.Order...)
	if comp.Native && (comp.Element == nil || !comp.Element.NoStyle) && !comp.Style {
		if st := c.fc.styleType(); st != nil && st.Comp != nil {
			members = append(members, st.Comp.Order...)
		}
	}
	for i, m := range members {
		name := m.Name()
		doc := joinDoc(docOf(m.Decl), "")
		order := fmt.Sprintf("%d_%03d", prioMember, i)
		if comp.Style || m.Owner != tn {
			order = fmt.Sprintf("%d_%03d", prioMember+1, i) // style props after the element's own
		}
		switch m.Kind {
		case check.PropMember:
			if name == "children" || set[name] {
				continue
			}
			if m.IsSlot() {
				c.add(CompletionItem{Label: name, Kind: kindProperty, Detail: "slot", Documentation: c.doc(doc), InsertText: name + " {\n\t$0\n}", InsertTextFormat: 2, SortText: order}, prioMember)
				continue
			}
			c.add(CompletionItem{Label: name, Kind: kindProperty, Detail: typeStr(m.Type), Documentation: c.doc(doc), InsertText: name + ": $0", InsertTextFormat: 2, SortText: order}, prioMember)
		case check.EventMember:
			c.add(c.eventItem(m, true, order), prioMember)
		}
	}
	if comp.Element != nil {
		for _, k := range comp.Element.Keys {
			if k.Bind && !set["bind "+k.Name] {
				c.add(CompletionItem{Label: "bind " + k.Name, Kind: kindProperty, Detail: k.Type, Documentation: c.doc("Two-way: the control writes what's typed or picked back to the target."), InsertText: "bind " + k.Name + ": $0", InsertTextFormat: 2, SortText: fmt.Sprintf("%d_bind", prioMember)}, prioMember)
			}
		}
	}
}

// setInBlock is what the element's block sets already.
func (c *completer) setInBlock(path []syntax.Node) map[string]bool {
	set := map[string]bool{}
	for i := len(path) - 1; i >= 0; i-- {
		b, ok := path[i].(*syntax.Block)
		if !ok {
			continue
		}
		for _, it := range b.Items {
			r := it.Range()
			if r.Start <= syntax.Pos(c.fc.file.File.Base()+c.off) && syntax.Pos(c.fc.file.File.Base()+c.off) <= r.End {
				continue // the one being written
			}
			switch it := it.(type) {
			case *syntax.PropSet:
				set[it.Name.Name] = true
			case *syntax.BindItem:
				set["bind "+it.Name.Name] = true
				set[it.Name.Name] = true
			case *syntax.SlotItem:
				set[it.Name.Name] = true
			}
		}
		break
	}
	return set
}

// eventItem is on name { … } (withOn), or name { … } after an on.
func (c *completer) eventItem(m *check.Member, withOn bool, order string) CompletionItem {
	var params []string
	for i, p := range m.Params {
		params = append(params, fmt.Sprintf("${%d:%s}", i+1, p.Name))
	}
	head := m.Name()
	if len(params) > 0 {
		head += "(" + strings.Join(params, ", ") + ")"
	}
	label := m.Name()
	if withOn {
		label = "on " + label
		head = "on " + head
	}
	return CompletionItem{Label: label, Kind: kindEvent, Detail: "event " + eventSig(m), Documentation: c.doc(docOf(m.Decl)), InsertText: head + " {\n\t$0\n}", InsertTextFormat: 2, SortText: order, FilterText: label}
}

// events completes on name.
func (c *completer) events(path []syntax.Node, withOn bool) {
	el, _ := c.fc.enclosingElement(path)
	if el == nil || el.Comp == nil {
		return
	}
	for i, m := range el.Comp.Order {
		if m.Kind == check.EventMember {
			c.add(c.eventItem(m, withOn, fmt.Sprintf("%d_%03d", prioMember, i)), prioMember)
		}
	}
}

// binds completes bind name.
func (c *completer) binds(path []syntax.Node, _ bool) {
	el, _ := c.fc.enclosingElement(path)
	if el == nil || el.Comp == nil || el.Comp.Element == nil {
		return
	}
	for _, k := range el.Comp.Element.Keys {
		if k.Bind {
			c.add(CompletionItem{Label: k.Name, Kind: kindProperty, Detail: k.Type, Documentation: c.doc(k.Doc), InsertText: k.Name + ": $0", InsertTextFormat: 2}, prioMember)
		}
	}
}

// emits completes emit name: the component's events.
func (c *completer) emits(path []syntax.Node) {
	for i := len(path) - 1; i >= 0; i-- {
		switch path[i].(type) {
		case *syntax.ComponentDecl, *syntax.ScreenDecl:
			tn := c.fc.declType(path[i])
			if tn == nil || tn.Comp == nil {
				return
			}
			for _, m := range tn.Comp.Events {
				ins := m.Name()
				if len(m.Params) > 0 {
					ins += "($0)"
				}
				c.add(CompletionItem{Label: m.Name(), Kind: kindEvent, Detail: "event " + eventSig(m), Documentation: c.doc(docOf(m.Decl)), InsertText: ins, InsertTextFormat: 2}, prioMember)
			}
			return
		}
	}
}

// styles completes an entry of a style: list: the package's styles (a
// style with parameters, as its call).
func (c *completer) styles() {
	if c.fc.pkg == nil {
		return
	}
	for _, f := range c.fc.pkg.Files {
		for _, d := range f.Decls {
			if s, ok := d.(*syntax.StyleDecl); ok {
				item := CompletionItem{Label: s.Name.Name, Kind: kindValue, Detail: "style", Documentation: c.doc(docOf(s))}
				if s.Params != nil {
					item.Kind, item.InsertText, item.InsertTextFormat = kindFunction, s.Name.Name+"($0)", 2
				}
				c.add(item, prioMember)
			}
		}
	}
}

// argStart is the call whose argument starts at the word, if one does.
func (c *completer) argStart(path []syntax.Node, before string) (*syntax.CallExpr, bool) {
	t := strings.TrimRight(before, " \t")
	if !strings.HasSuffix(t, "(") && !strings.HasSuffix(t, ",") && strings.TrimSpace(t) != "" {
		return nil, false
	}
	pos := syntax.Pos(c.fc.file.File.Base() + c.start)
	for i := len(path) - 1; i >= 0; i-- {
		if call, ok := path[i].(*syntax.CallExpr); ok && call.Args != nil && call.Args.Start < pos && pos <= call.Args.End {
			return call, true
		}
	}
	return nil, false
}

// labels completes an argument's label: name: ….
func (c *completer) labels(call *syntax.CallExpr) {
	given := map[string]bool{}
	for _, a := range call.Args.List {
		if a.Label != nil {
			given[a.Label.Name] = true
		}
	}
	addLabel := func(name, typ, doc string) {
		if given[name] || name == "" || name == "_" {
			return
		}
		c.add(CompletionItem{Label: name + ":", Kind: kindProperty, Detail: typ, Documentation: c.doc(doc), InsertText: name + ": $0", InsertTextFormat: 2, FilterText: name}, prioMember)
	}
	cl := c.fc.info.Calls[call]
	if cl == nil {
		if tn := c.fc.calleeComponent(call); tn != nil {
			for _, m := range tn.Comp.Params {
				addLabel(m.Name(), typeStr(m.Type), docOf(m.Decl))
			}
		}
		return
	}
	switch cl.Kind {
	case check.CallComponent, check.CallStore, check.CallWorkerNew:
		if cl.Type != nil && cl.Type.Obj.Comp != nil {
			for _, m := range cl.Type.Obj.Comp.Params {
				addLabel(m.Name(), typeStr(m.Type), docOf(m.Decl))
			}
		}
	case check.CallStruct:
		if cl.Type != nil {
			for _, f := range cl.Type.Obj.Fields {
				addLabel(f.Name, typeStr(f.Type), c.fc.declDoc(f.Pos))
			}
		}
	case check.CallFunc, check.CallMethod, check.CallBuiltinMethod, check.CallIface, check.CallNative, check.CallWorker:
		for _, p := range cl.Params {
			addLabel(p.Name, typeStr(p.Type), "")
		}
		if cl.Params == nil && cl.Func != nil && cl.Func.Sig != nil {
			for _, p := range cl.Func.Sig.Params {
				addLabel(p.Name, typeStr(p.Type), "")
			}
		}
	case check.CallCase:
		if cl.Case != nil {
			for i, l := range cl.Case.Labels {
				if l != "" {
					addLabel(l, typeStr(cl.Case.Params[i]), "")
				}
			}
		}
	}
}

// ---------------------------------------------------------------------
// import paths

func (c *completer) imports(typed string) {
	fc := c.fc
	start := c.off - len(typed)
	rg := fc.m.rng(start, c.off)
	add := func(path, detail string, kind int) {
		if !strings.HasPrefix(path, typed) && !strings.Contains(path, typed) {
			return
		}
		c.add(CompletionItem{Label: path, Kind: kind, Detail: detail, TextEdit: &TextEdit{Range: rg, NewText: path}, FilterText: path}, prioMember)
	}
	// The standard library and the platform SDK.
	for _, p := range []string{"std/embed", "std/json", "std/math"} {
		add(p, "standard library", kindModule)
	}
	if ps, err := std.Packages(); err == nil {
		paths := make([]string, 0, len(ps))
		for p := range ps {
			paths = append(paths, p)
		}
		sort.Strings(paths)
		for _, p := range paths {
			detail := "standard library"
			if strings.HasPrefix(p, std.PlatformPrefix) {
				detail = "platform SDK"
			}
			add(p, detail, kindModule)
		}
	}
	// Packages beside this one.
	dir := filepath.Dir(fc.path)
	if entries, err := os.ReadDir(dir); err == nil {
		for _, e := range entries {
			if e.IsDir() && !strings.HasPrefix(e.Name(), ".") && hasAsuka(filepath.Join(dir, e.Name())) {
				add("./"+e.Name(), "package", kindFolder)
			}
		}
	}
	// The project's dependencies.
	if root := projectRoot(dir); root != "" {
		if m, err := pkgs.ReadManifest(root); err == nil {
			for name := range m.Dependencies {
				add(name, "dependency", kindModule)
			}
		}
	}
}

// importNames completes the names in an import's braces (import
// std/time.{Time, now}): what the package exports that the braces don't
// name yet.
func (c *completer) importNames(pos syntax.Pos) {
	fc := c.fc
	for _, d := range fc.file.Imports {
		if pos < d.Start || pos > d.End {
			continue
		}
		p := fc.info.Imports[d]
		if p == nil {
			return
		}
		have := map[string]bool{}
		for _, n := range d.Names {
			if n.End != pos {
				have[n.Name] = true
			}
		}
		names := p.Scope.Names()
		sort.Strings(names)
		for _, n := range names {
			o := p.Scope.LookupLocal(n)
			if have[n] || !check.Exported(p, o) {
				continue
			}
			it := c.objectItem(o, "")
			// A name, not a call: the braces list names.
			it.InsertText, it.InsertTextFormat = "", 0
			c.add(it, prioMember)
		}
		return
	}
}

func hasAsuka(dir string) bool {
	entries, err := os.ReadDir(dir)
	if err != nil {
		return false
	}
	for _, e := range entries {
		if !e.IsDir() && strings.HasSuffix(e.Name(), ".asuka") {
			return true
		}
	}
	return false
}

// projectRoot is the directory of the asuka.toml dir is in (or under).
func projectRoot(dir string) string {
	for d := dir; ; {
		if _, err := os.Stat(filepath.Join(d, "asuka.toml")); err == nil {
			return d
		}
		parent := filepath.Dir(d)
		if parent == d {
			return ""
		}
		d = parent
	}
}
