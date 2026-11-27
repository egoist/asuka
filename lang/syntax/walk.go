package syntax

import "fmt"

// A Visitor's Visit is called for each node Walk meets. If it returns a
// Visitor w, Walk visits the node's children with w, then calls
// w.Visit(nil).
type Visitor interface {
	Visit(n Node) (w Visitor)
}

// Walk visits n and then, depth first and in source order, its children.
// Comments aren't nodes; TriviaOf has them.
func Walk(v Visitor, n Node) {
	if v = v.Visit(n); v == nil {
		return
	}
	walk := func(n Node) { Walk(v, n) }
	switch n := n.(type) {
	case *SourceFile:
		for _, d := range n.Imports {
			walk(d)
		}
		for _, d := range n.Decls {
			walk(d)
		}
	case *ImportDecl:
		walkIdents(v, n.Path)
		walkIdents(v, n.Names)
		if n.Alias != nil {
			walk(n.Alias)
		}

	// Declarations.
	case *TypeParam:
		walk(n.Name)
		for _, t := range n.Constraints {
			walk(t)
		}
	case *Param:
		walk(n.Name)
		if n.Type != nil {
			walk(n.Type)
		}
		if n.Default != nil {
			walk(n.Default)
		}
	case *ParamList:
		for _, p := range n.List {
			walk(p)
		}
	case *ComponentDecl:
		walk(n.Name)
		walkTypeParams(v, n.TypeParams)
		if n.Params != nil {
			walk(n.Params)
		}
		walk(n.Body)
	case *ScreenDecl:
		walk(n.Name)
		if n.Params != nil {
			walk(n.Params)
		}
		if n.At != nil {
			walk(n.At)
		}
		walk(n.Body)
	case *StoreDecl:
		walk(n.Name)
		walkTypeParams(v, n.TypeParams)
		if n.Params != nil {
			walk(n.Params)
		}
		walk(n.Body)
	case *WorkerDecl:
		walk(n.Name)
		if n.Params != nil {
			walk(n.Params)
		}
		walk(n.Body)
	case *ShaderDecl:
		walk(n.Name)
		if n.Params != nil {
			walk(n.Params)
		}
		for _, m := range n.Members {
			walk(m)
		}
	case *StructDecl:
		walk(n.Name)
		walkTypeParams(v, n.TypeParams)
		for _, t := range n.Conforms {
			walk(t)
		}
		for _, m := range n.Members {
			walk(m)
		}
	case *Field:
		walk(n.Name)
		walk(n.Type)
		if n.Default != nil {
			walk(n.Default)
		}
	case *EnumDecl:
		walk(n.Name)
		walkTypeParams(v, n.TypeParams)
		for _, t := range n.Conforms {
			walk(t)
		}
		for _, c := range n.Cases {
			walk(c)
		}
		for _, m := range n.Members {
			walk(m)
		}
	case *EnumCase:
		walk(n.Name)
		if n.Params != nil {
			walk(n.Params)
		}
	case *CaseParams:
		for _, p := range n.List {
			walk(p)
		}
	case *CaseParam:
		if n.Label != nil {
			walk(n.Label)
		}
		walk(n.Type)
	case *ExtendDecl:
		walkTypeParams(v, n.TypeParams)
		walk(n.Type)
		for _, t := range n.Conforms {
			walk(t)
		}
		for _, m := range n.Members {
			walk(m)
		}
	case *InterfaceDecl:
		walk(n.Name)
		walkTypeParams(v, n.TypeParams)
		for _, m := range n.Members {
			walk(m)
		}
	case *FuncDecl:
		if n.NativePrefix != nil {
			walk(n.NativePrefix)
		}
		walk(n.Name)
		walkTypeParams(v, n.TypeParams)
		walk(n.Params)
		if n.Result != nil {
			walk(n.Result)
		}
		if n.Body != nil {
			walk(n.Body)
		}
		if n.Value != nil {
			walk(n.Value)
		}
	case *ExternTypeDecl:
		walk(n.Name)
		if n.Release != nil {
			walk(n.Release)
		}
	case *TypeAliasDecl:
		walk(n.Name)
		walkTypeParams(v, n.TypeParams)
		walk(n.Type)
	case *StyleDecl:
		walk(n.Name)
		if n.Params != nil {
			walk(n.Params)
		}
		walk(n.Body)
	case *StateDecl:
		walk(n.Name)
		for _, d := range n.Deps {
			walk(d)
		}
		if n.Type != nil {
			walk(n.Type)
		}
		if n.Value != nil {
			walk(n.Value)
		}
	case *LetDecl:
		walk(n.Pattern)
		if n.Type != nil {
			walk(n.Type)
		}
		if n.Value != nil {
			walk(n.Value)
		}
		if n.Else != nil {
			walk(n.Else)
		}
	case *AppDecl:
		walk(n.Body)
	case *TestDecl:
		walk(n.Name)
		if n.Params != nil {
			walk(n.Params)
		}
		walk(n.Body)
	case *BenchDecl:
		walk(n.Name)
		walk(n.Body)
	case *PreviewDecl:
		walk(n.Name)
		if n.Settings != nil {
			walk(n.Settings)
		}
		walk(n.Body)
	case *MeasureStmt:
		walk(n.Body)
	case *BadDecl:

	// Blocks and items.
	case *Block:
		for _, it := range n.Items {
			walk(it)
		}
	case *PropDecl:
		walk(n.Name)
		if n.Type != nil {
			walk(n.Type)
		}
		if n.Default != nil {
			walk(n.Default)
		}
	case *EventDecl:
		walk(n.Name)
		if n.Params != nil {
			walk(n.Params)
		}
	case *VariantDecl:
		walk(n.Name)
		walk(n.Value)
	case *UseDecl:
		walk(n.Name)
		walk(n.Type)
	case *ProvideDecl:
		walk(n.Value)
	case *TaskDecl:
		for _, d := range n.Deps {
			walk(d)
		}
		walk(n.Body)
	case *EffectDecl:
		for _, d := range n.Deps {
			walk(d)
		}
		walk(n.Body)
	case *PropSet:
		walk(n.Name)
		walk(n.Value)
	case *OnHandler:
		walk(n.Name)
		walkIdents(v, n.Params)
		walk(n.Body)
	case *BindItem:
		walk(n.Name)
		walk(n.Target)
	case *StyleUse:
		walk(n.Name)
	case *SlotItem:
		walk(n.Name)
		walk(n.Body)
	case *IfStmt:
		walk(n.Cond)
		walk(n.Then)
		if n.Else != nil {
			walk(n.Else)
		}
	case *ForStmt:
		walk(n.Pattern)
		walk(n.Seq)
		if n.Key != nil {
			walk(n.Key)
		}
		if n.Label != nil {
			walk(n.Label)
		}
		walk(n.Body)
	case *WhileStmt:
		walk(n.Cond)
		if n.Label != nil {
			walk(n.Label)
		}
		walk(n.Body)
	case *MatchStmt:
		walk(n.Subject)
		for _, a := range n.Arms {
			walk(a)
		}
	case *MatchArm:
		walk(n.Pattern)
		if n.Guard != nil {
			walk(n.Guard)
		}
		walk(n.Body)
	case *DoStmt:
		walk(n.Body)
		for _, c := range n.Catches {
			walk(c)
		}
	case *CatchClause:
		if n.Pattern != nil {
			walk(n.Pattern)
		}
		if n.Type != nil {
			walk(n.Type)
		}
		walk(n.Body)
	case *ReturnStmt:
		if n.Value != nil {
			walk(n.Value)
		}
	case *ThrowStmt:
		walk(n.Value)
	case *BranchStmt:
		if n.Label != nil {
			walk(n.Label)
		}
	case *DeferStmt:
		walk(n.Body)
	case *CleanupStmt:
		walk(n.Body)
	case *EmitStmt:
		walk(n.Name)
		if n.Args != nil {
			walk(n.Args)
		}
	case *AssignStmt:
		walk(n.Target)
		walk(n.Value)
	case *ExprStmt:
		walk(n.X)
	case *BadItem:

	// Expressions.
	case *Ident, *BasicLit, *BoolLit, *NoneLit, *SelfExpr, *AtExpr, *BadExpr, *FormatSpec:
	case *StringLit:
		for i, x := range n.Interps {
			walk(x)
			if i < len(n.Specs) && n.Specs[i] != nil {
				walk(n.Specs[i])
			}
		}
	case *ImplicitMember:
		walk(n.Name)
	case *MemberExpr:
		walk(n.X)
		walk(n.Name)
	case *TypeApp:
		walk(n.X)
		for _, t := range n.TypeArgs {
			walk(t)
		}
	case *ArgList:
		for _, a := range n.List {
			walk(a)
		}
	case *Arg:
		if n.Label != nil {
			walk(n.Label)
		}
		walk(n.Value)
	case *CallExpr:
		walk(n.Fun)
		for _, t := range n.TypeArgs {
			walk(t)
		}
		if n.Args != nil {
			walk(n.Args)
		}
		if n.As != nil {
			walk(n.As)
		}
		if n.Block != nil {
			walk(n.Block)
		}
		for _, h := range n.Handlers {
			walk(h)
		}
	case *IndexExpr:
		walk(n.X)
		walk(n.Index)
		if n.Default != nil {
			walk(n.Default)
		}
	case *UnwrapExpr:
		walk(n.X)
	case *UnaryExpr:
		walk(n.X)
	case *SpawnExpr:
		walk(n.Body)
	case *BinaryExpr:
		walk(n.X)
		walk(n.Y)
	case *LambdaExpr:
		for _, p := range n.Params {
			walk(p)
		}
		walk(n.Body)
	case *ParenExpr:
		walk(n.X)
	case *TupleExpr:
		for _, x := range n.Elems {
			walk(x)
		}
	case *ArrayExpr:
		for _, x := range n.Elems {
			walk(x)
		}
	case *MapExpr:
		for _, e := range n.Entries {
			walk(e)
		}
	case *MapEntry:
		walk(n.Key)
		walk(n.Value)
	case *IfExpr:
		walk(n.Cond)
		walk(n.Then)
		if n.Else != nil {
			walk(n.Else)
		}
	case *MatchExpr:
		walk(n.Subject)
		for _, a := range n.Arms {
			walk(a)
		}
	case *LetCond:
		walk(n.Pattern)
		walk(n.Value)
	case *CondList:
		for _, x := range n.List {
			walk(x)
		}
	case *JumpExpr:
		if n.Value != nil {
			walk(n.Value)
		}
		if n.Label != nil {
			walk(n.Label)
		}
	case *CatchExpr:
		walk(n.X)
		for _, c := range n.Catches {
			walk(c)
		}

	// Types.
	case *NamedType:
		walkIdents(v, n.Path)
		for _, t := range n.Args {
			walk(t)
		}
	case *ArrayType:
		walk(n.Elem)
	case *MapType:
		walk(n.Key)
		walk(n.Value)
	case *TupleType:
		for _, t := range n.Elems {
			walk(t)
		}
	case *FuncType:
		for _, t := range n.Params {
			walk(t)
		}
		if n.Result != nil {
			walk(n.Result)
		}
	case *OptionalType:
		walk(n.Elem)
	case *BadType:

	// Patterns.
	case *WildcardPat, *BadPat:
	case *BindPat:
		walk(n.Name)
	case *LitPat:
		walk(n.Value)
	case *CasePat:
		walkIdents(v, n.Path)
		for i, a := range n.Args {
			if l := n.Label(i); l != nil {
				walk(l)
			}
			walk(a)
		}
	case *TypedPat:
		walk(n.Pat)
		walk(n.Type)
	case *TuplePat:
		for _, e := range n.Elems {
			walk(e)
		}
	case *OrPat:
		for _, a := range n.Alts {
			walk(a)
		}

	default:
		panic(fmt.Sprintf("syntax.Walk: unexpected node %T", n))
	}
	v.Visit(nil)
}

func walkIdents(v Visitor, list []*Ident) {
	for _, id := range list {
		Walk(v, id)
	}
}

func walkTypeParams(v Visitor, list []*TypeParam) {
	for _, tp := range list {
		Walk(v, tp)
	}
}

type inspector func(Node) bool

func (f inspector) Visit(n Node) Visitor {
	if f(n) {
		return f
	}
	return nil
}

// Inspect walks the tree from n, calling f for each node; f returning
// false skips the node's children. After a node's children (when f
// returned true), f is called with nil.
func Inspect(n Node, f func(Node) bool) { Walk(inspector(f), n) }
