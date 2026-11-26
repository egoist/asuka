package check

import (
	"testing"

	"github.com/egoist/asuka/lang/syntax"
)

func TestReactive(t *testing.T) {
	prog, diags := checkSrc(t, map[string]string{"a.asuka": `
state count = 0
let doubled = count * 2
let pi = 3
let viaFn = readsCount() + 1

fn readsCount() -> int = count
fn pure(x: int) -> int = x + 1
fn indirect() -> int = readsCount()
fn onDesktop() -> bool = environment().platform == .macOS

component C(title: string) {
  state n = 0
  let a = n + 1
  let b = 5
  let c = pure(3)
  let d = indirect()
  let e = [1, 2].map(x => x + n)
  let f = [1, 2].map(x => x + 1)
  // The environment is a signal (the host says it after mount), through a
  // function too.
  let g = environment().width
  let h = onDesktop()

  Column {
    Text("{b}")
    Text(title)
    Text("{pure(b)}")
  }
}
`})
	for _, d := range diags {
		t.Fatal(d)
	}
	root := prog.Root
	globals := map[string]GlobalKind{}
	for _, g := range root.Globals {
		globals[g.Name()] = g.Kind
	}
	for name, want := range map[string]GlobalKind{"count": StateGlobal, "doubled": MemoGlobal, "pi": ConstGlobal, "viaFn": MemoGlobal} {
		if globals[name] != want {
			t.Errorf("%s: kind %d, want %d", name, globals[name], want)
		}
	}
	var comp *TypeName
	for _, tn := range root.Types {
		if tn.Name() == "C" {
			comp = tn
		}
	}
	memo := map[string]bool{"a": true, "b": false, "c": false, "d": true, "e": true, "f": false, "g": true, "h": true}
	for name, want := range memo {
		if m := comp.Comp.Members[name]; m.Memo != want {
			t.Errorf("let %s: memo %v, want %v", name, m.Memo, want)
		}
	}
	// The texts' bindings: constant, the parameter's, constant.
	var texts []syntax.Expr
	syntax.Inspect(comp.Comp.Body, func(n syntax.Node) bool {
		if call, ok := n.(*syntax.CallExpr); ok {
			if id, ok := call.Fun.(*syntax.Ident); ok && id.Name == "Text" {
				texts = append(texts, call.Args.List[0].Value)
			}
		}
		return true
	})
	want := []bool{false, true, false}
	for i, x := range texts {
		if got := prog.Reactive(x); got != want[i] {
			t.Errorf("Text %d: reactive %v, want %v", i, got, want[i])
		}
	}
}
