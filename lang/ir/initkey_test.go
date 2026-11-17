package ir

import (
	"testing"

	"github.com/egoist/asuka/lang/syntax"
)

// A state's key changes with its initializer, so a reload starts it
// again from the new one, and not with how its initializer is laid out.
func TestInitKey(t *testing.T) {
	key := func(src string) string {
		t.Helper()
		e, errs := syntax.ParseExpr(src)
		if len(errs) > 0 {
			t.Fatal(errs)
		}
		return initKey(e)
	}
	same := [][2]string{
		{"0", "0"},
		{"[1, 2, 3]", "[1,2,   3]"},
		{"[\n  1,\n  2,\n]", "[1, 2]"},
		{`Todo(title: "a", done: false)`, "Todo(\n  title: \"a\",\n  done: false,\n)"},
	}
	for _, c := range same {
		if a, b := key(c[0]), key(c[1]); a != b {
			t.Errorf("%q and %q: keys %s and %s, want the same", c[0], c[1], a, b)
		}
	}
	differ := [][2]string{
		{"0", "10"},
		{`"a b"`, `"ab"`},
		{"[1, 2]", "[1, 2, 3]"},
		{"START", "START + 1"},
	}
	for _, c := range differ {
		if a, b := key(c[0]), key(c[1]); a == b {
			t.Errorf("%q and %q: both %s, want different keys", c[0], c[1], a)
		}
	}
	if k := initKey(nil); k != "" {
		t.Errorf("no initializer: %q", k)
	}
}
