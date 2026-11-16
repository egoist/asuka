package lsp

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// A package with platforms' files: its diagnostics are every platform's,
// once each, saying which platforms when not every one; a platform's
// file is read with that platform's files (hover, definition), a shared
// one with iOS's.
func TestPlatformFiles(t *testing.T) {
	root := t.TempDir()
	write := func(name, src string) string {
		p := filepath.Join(root, name)
		if err := os.WriteFile(p, []byte(src), 0o644); err != nil {
			t.Fatal(err)
		}
		return p
	}
	main := write("main.asuka", "app { Text(label()) }\n\nfn shown() -> bool = @platform == .linux\n")
	write("label_mobile.asuka", "fn label() -> string = \"phone\"\n")
	write("label_desktop.asuka", "fn label() -> string = \"computer\"\n")
	linux := write("tray_linux.asuka", "fn tray() -> string = label()\n")
	c := newClient(t, root)
	uri, text := c.open(main)
	// The web has no label: said for the web alone.
	ds := c.waitDiags(uri, hasDiag("label"))
	if len(ds) != 1 || !strings.HasSuffix(ds[0].Message, "(for the web)") {
		t.Fatalf("got %+v", ds)
	}
	write("label_web.asuka", "fn label() -> string = \"page\"\n")
	c.change(uri, 2, text+"\n")
	c.waitDiags(uri, noDiags)

	// A mistake in Linux's file, open: for Linux.
	luri, ltext := c.open(linux)
	c.waitDiags(luri, noDiags)
	c.change(luri, 2, strings.Replace(ltext, "label()", "lable()", 1))
	ds = c.waitDiags(luri, hasDiag("lable"))
	if len(ds) != 1 || !strings.HasSuffix(ds[0].Message, "(for Linux)") {
		t.Fatalf("got %+v", ds)
	}
	// A mistake in a branch every platform has: every platform's, said
	// plainly, once.
	c.change(luri, 3, ltext)
	c.waitDiags(luri, noDiags)
	broken := strings.Replace(text, "@platform == .linux", "@platform == .linuks", 1)
	c.change(uri, 4, broken)
	ds = c.waitDiags(uri, hasDiag("linuks"))
	if len(ds) != 1 || strings.Contains(ds[0].Message, "(for") {
		t.Fatalf("got %+v", ds)
	}
	c.change(uri, 5, text)
	c.waitDiags(uri, noDiags)

	// Linux's file: its label is the desktop's.
	var locs []Location
	p := tdp(luri, posAt(t, ltext, "= la|bel()"))
	if err := c.call("textDocument/definition", &p, &locs); err != nil {
		t.Fatal(err)
	}
	if len(locs) != 1 || !strings.HasSuffix(locs[0].URI, "label_desktop.asuka") {
		t.Errorf("definition: %+v", locs)
	}
	// main's: iOS's.
	p = tdp(uri, posAt(t, text, "Text(la|bel())"))
	if err := c.call("textDocument/definition", &p, &locs); err != nil {
		t.Fatal(err)
	}
	if len(locs) != 1 || !strings.HasSuffix(locs[0].URI, "label_mobile.asuka") {
		t.Errorf("definition: %+v", locs)
	}
	// @platform's hover, and its completion.
	var h *Hover
	p = tdp(uri, posAt(t, text, "@plat|form =="))
	if err := c.call("textDocument/hover", &p, &h); err != nil || h == nil || !strings.Contains(h.Contents.Value, "@platform: Platform") || !strings.Contains(h.Contents.Value, "iOS's files") {
		t.Errorf("hover: %v %+v", err, h)
	}
	version := 5
	items := complete(t, c, uri, &version, text+"let x = @pl\n", text+"let x = @pl|")
	if l := labels(items); len(l) != 1 || l[0] != "platform" {
		t.Errorf("completion: %q", l)
	}
}
