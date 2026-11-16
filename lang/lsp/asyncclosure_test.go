package lsp

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// A closure marked async hovers as an async fn, whether or not its body
// awaits; an unmarked one as its body makes it.
func TestAsyncClosureHover(t *testing.T) {
	root := t.TempDir()
	src := `async fn run() {
  let marked = async (x: int) => x + 1
  let inferred = (x: int) => await later(x)
  let plain = (x: int) => x * 2
  let n = await marked(1) + await inferred(2) + plain(3)
}

async fn later(n: int) -> int = n
`
	if err := os.WriteFile(filepath.Join(root, "asuka.toml"), []byte("name = \"closures\"\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	main := filepath.Join(root, "main.asuka")
	if err := os.WriteFile(main, []byte(src), 0o644); err != nil {
		t.Fatal(err)
	}
	c := newClient(t, root)
	uri, text := c.open(main)
	c.waitDiags(uri, noDiags)
	for _, tc := range []struct{ at, want string }{
		{"let mar|ked", "let marked: async fn(int) -> int"},
		{"await mar|ked(1)", "async fn(int) -> int"},
		{"let infer|red", "let inferred: async fn(int) -> int"},
		{"let pla|in", "let plain: fn(int) -> int"},
	} {
		var h *Hover
		p := tdp(uri, posAt(t, text, tc.at))
		if err := c.call("textDocument/hover", &p, &h); err != nil {
			t.Fatal(err)
		}
		if h == nil || !strings.Contains(h.Contents.Value, tc.want) {
			t.Errorf("%s: hover %+v, want %q", tc.at, h, tc.want)
		}
	}
}
