package windows_test

import (
	"os"
	"path/filepath"
	"testing"
)

// literalApp shows values as interpolation writes them (core/src/vm/
// literal.c): compound values as the language writes them, floats in
// their shortest form, durations as literals. The C library's printf
// differs on Windows (MinGW's, the UCRT's), so the app's compiled C and
// the core write them here as they do elsewhere.
const literalApp = `struct Point {
  x: int
  y: float
}

enum Shape {
  circle(radius: float)
  square
}

enum Tint { red, green }

fn lines() -> [string] {
  let p = Point(x: 1, y: 2.5)
  let big: float = 1e300
  return [
    "p {p}",
    "s {Shape.circle(radius: 1.5)} {[Shape.square]}",
    "a {[1, 2]} m {["a": 1]} t {(1, "a")} d {90s} {1500ms}",
    "f {0.1 + 0.2} {big * 10.0} {-0.0} {1.0 / 3.0} {123456789.0} {1e-7}",
    "c {'a'} q {["say \"hi\""]} e {[Tint.red]}",
    "set {Set([3])} i {-9007199254740993} u {[2147483648 * 4]}",
  ]
}

component Lines {
  Column {
    for l in lines() {
      Text(l)
    }
  }
}

app { Lines() }
`

// The app's compiled C finds the stack's limit (x28 on arm64, an
// argument on amd64: cbuild.AppFlags): recursing too deep is a stack
// overflow the app catches, on the main thread, through a builtin's
// closure, and on a spawn's thread, with its smaller stack; and a
// shallow recursion isn't one. lang/compile's stackoverflow.asuka, as
// an app.
func TestStackLimit(t *testing.T) {
	w := onWindows(t)
	local, guest := w.job(t)
	prog, err := os.ReadFile(filepath.Join("..", "..", "lang", "compile", "testdata", "run", "stackoverflow.asuka"))
	if err != nil {
		t.Fatal(err)
	}
	src := string(prog) + `
component Deep {
  state spawned = "not yet"
  Column {
    Text("shallow {deepWide(100)} {deepMapped(100)}")
    Text("wide: {deepWide(1000000)}")
    Text("mapped: {deepMapped(1000000)}")
    Text("spawned: {spawned}")
    Button("Spawn") {
      on press {
        let a = await spawn { deepWide(1000000) }
        let b = await spawn { deepMapped(1000000) }
        spawned = "{a}, {b}"
      }
    }
  }
}

app { Deep() }
`
	buildApp(t, w, src, local)
	d := script(t, w, local, guest, "app.exe", "tap:Spawn", "wait:5s", "snap:end2")
	shows(t, d["end2"], "the app", []string{"shallow 52 100", "wide: stack overflow", "mapped: stack overflow", "spawned: stack overflow, stack overflow"})
}

func TestValuesAsText(t *testing.T) {
	w := onWindows(t)
	local, guest := w.job(t)
	buildApp(t, w, literalApp, local)
	out, err := w.run(t, guest, guest+`\app.exe`, map[string]string{"ASUKA_SNAPSHOT": guest + `\v.png`, "ASUKA_DUMP": "1"})
	dump, rerr := os.ReadFile(filepath.Join(local, "v.png.txt"))
	if err != nil || rerr != nil {
		t.Fatalf("%v %v\n%s", err, rerr, out)
	}
	shows(t, string(dump), "the values", []string{
		"p Point(x: 1, y: 2.5)",
		"s circle(radius: 1.5) [.square]",
		`a [1, 2] m ["a": 1] t (1, "a") d 90s 1500ms`,
		"f 0.30000000000000004 1e+301 0 0.3333333333333333 123456789 1e-7",
		`c a q ["say \"hi\""] e [.red]`,
		"set Set([3]) i -9007199254740993 u [8589934592]",
	})
}
