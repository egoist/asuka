package lsp

import (
	"path/filepath"
	"strings"
	"testing"
)

// Signature help while a call is typed: the signature of what's called
// (a function, a component, an emit's event, a style, a built-in method
// before its first argument), and the parameter the argument at the
// cursor goes to, up to the comma after it.
func TestSignatureHelpWhileTyping(t *testing.T) {
	root := t.TempDir()
	c := newClient(t, root)
	const base = `fn area(width: float, height: float = 1) -> float = width * height

style chip(tint: Color = .blue) { color: tint }

store Box<T>(initial: T) {
  pub state value: T = initial
}

component Row(title: string) {
  event archive(id: string)

  CURSOR
}
`
	uri := c.openText(filepath.Join(root, "a.asuka"), "")
	version := 1
	for _, tc := range []struct{ at, label, active string }{
		{"let q = area(1 |)", "fn area(width: float, height: float = 1) -> float", "width: float"},
		{"let q = area(1, height: |)", "fn area(width: float, height: float = 1) -> float", "height: float = 1"},
		{"View { Text(|) }", "native component Text(text: string)", "text: string"},
		{"View { Row(|) }", "component Row(title: string)", "title: string"},
		{"let b = Box(|)", "store Box<T>(initial: T)", "initial: T"},
		{"View { on press { emit archive(id: |) } }", "event archive(id: string)", "id: string"},
		{"View { Text(title) { style: [chip(tint: |)] } }", "style chip(tint: Color = .blue)", "tint: Color = .blue"},
		{"let r = [1, 2].map(|)", "fn [int].map(transform: fn(int) throws -> U) -> [U]", "transform: fn(int) throws -> U"},
	} {
		text := strings.Replace(base, "CURSOR", tc.at, 1)
		i := strings.Index(text, "|")
		text = text[:i] + text[i+1:]
		version++
		c.change(uri, version, text)
		var sh *SignatureHelp
		p := tdp(uri, newMapper([]byte(text), false).position(i))
		if err := c.call("textDocument/signatureHelp", &p, &sh); err != nil {
			t.Fatal(err)
		}
		if sh == nil || len(sh.Signatures) != 1 {
			t.Errorf("%s: %+v", tc.at, sh)
			continue
		}
		s := sh.Signatures[0]
		if s.Label != tc.label {
			t.Errorf("%s: label %q, want %q", tc.at, s.Label, tc.label)
			continue
		}
		active := ""
		if sh.ActiveParameter < len(s.Parameters) {
			r := s.Parameters[sh.ActiveParameter].Label
			active = string([]rune(s.Label)[r[0]:r[1]])
		}
		if active != tc.active {
			t.Errorf("%s: active parameter %q, want %q", tc.at, active, tc.active)
		}
	}
}

// A style's hover says its parameters.
func TestStyleHover(t *testing.T) {
	root := t.TempDir()
	c := newClient(t, root)
	text := "pub style chip(tint: Color, size: float = 13) { color: tint }\n\ncomponent A { Text(\"a\") { style: chip(.red) } }\n"
	uri := c.openText(filepath.Join(root, "a.asuka"), text)
	c.waitDiags(uri, noDiags)
	if got := hoverAt(t, c, uri, text, "style: ch|ip(.red)"); !strings.Contains(got, "pub style chip(tint: Color, size: float = 13)") {
		t.Errorf("hover: %q", got)
	}
}
