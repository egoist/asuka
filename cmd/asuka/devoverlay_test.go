package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/egoist/asuka/lang/target"
)

// A build that failed shows its first error with its file relative to
// the app's, the lines at it with a caret under it, and the others.
func TestDevOverlayBuildFailure(t *testing.T) {
	dir := counterDir(t)
	src := "component Counter {\n  state count = 0\n  Button(\"Add\") {\n    on press { count += \"x\" }\n  }\n}\n\napp { Counter(); Text(nope) }\n"
	os.WriteFile(filepath.Join(dir, "app.asuka"), []byte(src), 0o644)
	app := projectApp(t, dir, nil)
	_, diags, err := app.compileShown(target.IOS, map[string]bool{})
	if err == nil {
		t.Fatal("it compiled")
	}
	f := buildFailure(dir, diags, err)
	if f.Message != "app.asuka:4:16: += doesn't take an int and a string" {
		t.Errorf("message %q", f.Message)
	}
	want := "  2 |   state count = 0\n" +
		"  3 |   Button(\"Add\") {\n" +
		"> 4 |     on press { count += \"x\" }\n" +
		"    |                ^^^^^^^^^^^^\n" +
		"  5 |   }\n" +
		"  6 | }"
	if f.Source != want {
		t.Errorf("source:\n%s\nwant:\n%s", f.Source, want)
	}
	if !strings.Contains(f.Details, "see https://asuka.egoist.dev/docs/errors/bad-operand") || !strings.Contains(f.Details, "1 more error:\napp.asuka:8:") {
		t.Errorf("details:\n%s", f.Details)
	}
	// No diagnostics: the error says what went wrong.
	if f := buildFailure(dir, nil, os.ErrNotExist); f.Message != os.ErrNotExist.Error() || f.Source != "" {
		t.Errorf("%+v", f)
	}
}

// An error the app didn't catch: the lines where its innermost frame in
// the app's files was, and the stack, the app's files relative to it, a
// package's as they were.
func TestDevOverlayUncaught(t *testing.T) {
	dir := counterDir(t)
	src := "fn parse(s: string) throws -> int {\n  throw Error(\"can't parse {s}\")\n}\n"
	os.WriteFile(filepath.Join(dir, "parse.asuka"), []byte(src), 0o644)
	trace := "std.thing (std/thing/thing.asuka:9)\n" +
		"parse (" + filepath.Join(dir, "parse.asuka") + ":2)\n" +
		"Counter.press (" + filepath.Join(dir, "app.asuka") + ":1)\n" +
		"a line that isn't a frame\n"
	source, details := uncaughtSource(dir, trace)
	if want := "  1 | fn parse(s: string) throws -> int {\n> 2 |   throw Error(\"can't parse {s}\")\n  3 | }"; source != want {
		t.Errorf("source:\n%q\nwant:\n%q", source, want)
	}
	want := "std.thing (std/thing/thing.asuka:9)\nparse (parse.asuka:2)\nCounter.press (app.asuka:1)\na line that isn't a frame"
	if details != want {
		t.Errorf("details:\n%s\nwant:\n%s", details, want)
	}
	// No frame of the app's: no source.
	if source, _ := uncaughtSource(dir, "std.thing (std/thing/thing.asuka:9)"); source != "" {
		t.Errorf("source %q", source)
	}
}

// A code frame's numbers are as wide as its last's, tabs show as two
// spaces (the caret too), and the file's edges cut it short.
func TestCodeFrame(t *testing.T) {
	p := filepath.Join(t.TempDir(), "a.asuka")
	os.WriteFile(p, []byte("a\nb\nc\nd\ne\nf\ng\nh\ni\n\tlet x = 1\nk\n"), 0o644) // its last line ends
	want := "   8 | h\n   9 | i\n> 10 |   let x = 1\n     |       ^\n  11 | k"
	if got := codeFrame(p, 10, 6, 0); got != want {
		t.Errorf("got:\n%s\nwant:\n%s", got, want)
	}
	if got := codeFrame(p, 1, 0, 0); got != "> 1 | a\n  2 | b\n  3 | c" {
		t.Errorf("got:\n%s", got)
	}
	if codeFrame(p, 99, 0, 0) != "" || codeFrame(p+"x", 1, 0, 0) != "" {
		t.Error("a line or file that isn't there has a frame")
	}
}
