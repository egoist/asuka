package signal

import (
	"runtime"
	"testing"
)

func TestClosureDepth(t *testing.T) {
	for _, c := range []struct {
		rest       string
		depth, num int
	}{
		{".func2", 1, 2},
		{".func2.func1", 2, 1},
		{".1", 1, 1}, // inlined
		{"-range1.func3", 1, 3},
		{"-range1-range1.1", 1, 1},
		{".func1-range1.func2", 2, 2},
		{"-range1", 0, 0},         // a loop's body is no literal
		{".func1.Screen.1", 0, 0}, // belongs to an inlined declaration
		{".func1.Screen", 0, 0},   // a declared function, not a literal
		{".func1.gowrap1", 0, 0},
		{".func1-rangeX", 0, 0},
		{"1", 0, 0}, // the end of another declaration's name

	} {
		if d, n := closureDepth(c.rest); d != c.depth || n != c.num {
			t.Errorf("%s: %d, %d; want %d, %d", c.rest, d, n, c.depth, c.num)
		}
	}
}

func TestPlaceLookups(t *testing.T) {
	defer func() { places = nil }()
	KeepPlaces("F\t/app/main.go\n" +
		"D\tmain.Screen\nD\tmain.ScreenList\nD\tmain.(*T[...]).M\nD\tmain.Generic[...]\nD\tmain.main\n" +
		"L\t10\t20\t1\t1\tmain.Screen>body\n" +
		"L\t12\t12\t2\t1\tmain.Screen>body>ui.Show#1\n" +
		"L\t30\t30\t1\t2\tmain.Screen>run#0\n" +
		"L\t30\t30\t1\t3\tmain.Screen>run#1\n" +
		"L\t40\t41\t1\t1\tmain.(*T[...]).M>onTap\n" +
		"L\t50\t51\t1\t1\tmain.Generic[...]>body\n" +
		"L\t60\t60\t1\t4\tmain.Screen>left\n" +
		"L\t60\t60\t2\t1\tmain.Screen>left>inner\n" +
		"L\t60\t60\t1\t5\tmain.Screen>right\n" +
		"L\t60\t60\t2\t1\tmain.Screen>right>inner\n" +
		"S\t11\t11\tcount\nS\t13\t13\ta\nS\t13\t13\tb\n" +
		"F\t/app/reset.go\nR\tabc\nS\t5\t5\tx\n")
	frame := func(fn, file string, line int) runtime.Frame {
		return runtime.Frame{Function: fn, File: file, Line: line}
	}
	for _, c := range []struct {
		fr   runtime.Frame
		want string
	}{
		{frame("main.Screen.func1", "/app/main.go", 11), "main.Screen>body"},
		{frame("main.Screen.1", "/app/main.go", 15), "main.Screen>body"},                     // inlined
		{frame("main.Screen.func1.func1", "/app/main.go", 12), "main.Screen>body>ui.Show#1"}, // the inner, on a shared line
		{frame("main.Screen.func1", "/app/main.go", 12), "main.Screen>body"},                 // the outer, likewise
		{frame("main.Screen.func2", "/app/main.go", 30), "main.Screen>run#0"},                // siblings on a line
		{frame("main.Screen.func3", "/app/main.go", 30), "main.Screen>run#1"},
		{frame("main.Screen.func9", "/app/main.go", 30), ""}, // can't tell
		{frame("main.(*T[...]).M.func1", "/app/main.go", 40), "main.(*T[...]).M>onTap"},
		{frame("main.ScreenList", "/app/main.go", 11), ""}, // a declared function
		{frame("main.Screen.func1", "/elsewhere.go", 11), ""},
		// Inlining prefixes the caller's name and can renumber the literal.
		{frame("main.main.func1.Screen.1", "/app/main.go", 11), "main.Screen>body"},
		{frame("main.main.func1.Screen.1", "/app/main.go", 12), "main.Screen>body"},
		{frame("main.main.Screen.func3", "/app/main.go", 11), "main.Screen>body"},
		{frame("main.main.Screen.func3.func1", "/app/main.go", 12), "main.Screen>body>ui.Show#1"},
		{frame("main.main.Screen.Screen.func3", "/app/main.go", 11), "main.Screen>body"},
		{frame("main.main.(*T[...]).M.func2", "/app/main.go", 40), "main.(*T[...]).M>onTap"},
		{frame("main.main.Generic[...].func2", "/app/main.go", 50), "main.Generic[...]>body"},
		{frame("main.main.func1.Screen.1-range1", "/app/main.go", 11), "main.Screen>body"},
		{frame("main.main.func1.Screen-range1.func1", "/app/main.go", 11), "main.Screen>body"},
		{frame("main.main.func1.Screen-range1", "/app/main.go", 11), ""},
		// A generated ordinal can't disambiguate siblings after inlining.
		{frame("main.main.func1.Screen.2", "/app/main.go", 30), ""},
		{frame("main.Screen.func4", "/app/main.go", 60), "main.Screen>left"},
		{frame("main.Screen.func5", "/app/main.go", 60), "main.Screen>right"},
		{frame("main.Screen.func4.func1", "/app/main.go", 60), ""}, // two inner literals numbered 1

		// Neither a matching suffix nor a matching line alone is enough.
		{frame("main.main.func1", "/app/main.go", 11), ""},
		{frame("main.Screen.func1", "/app/main.go", 40), ""},
		{frame("main.ScreenList.func1", "/app/main.go", 11), ""},
		{frame("main.Screen1", "/app/main.go", 11), ""},
		{frame("main.main.func1.Screen1.func1", "/app/main.go", 12), ""},
		{frame("main.Screen.func1.NotScreen.func1", "/app/main.go", 12), ""},
		{frame("main.main.func1.Screen", "/app/main.go", 11), ""},
	} {
		if got := places.closure(c.fr); got != c.want {
			t.Errorf("%s:%d: %q, want %q", c.fr.Function, c.fr.Line, got, c.want)
		}
	}
	if name, reset := places.site(frame("main.Screen.func1", "/app/main.go", 11)); name != "count" || reset != "" {
		t.Error(name, reset)
	}
	if name, _ := places.site(frame("main.Screen.func1", "/app/main.go", 13)); name != "" {
		t.Error("two on a line:", name)
	}
	if name, reset := places.site(frame("main.F", "/app/reset.go", 5)); name != "x" || reset != "abc" {
		t.Error(name, reset)
	}
}
