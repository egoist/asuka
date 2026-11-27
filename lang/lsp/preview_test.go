package lsp

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/egoist/asuka/internal/devserver"
)

const previewApp = `component Card {
  prop title: string
  Text(title)
}

app { Card { title: "The app" } }

preview "Card" { Card { title: "Hi" } }

preview "Dark, counting" (dark: true, fontScale: 1.5) {
  state n = 1
  Card { title: "n is {n}" }
}
`

// A ▶ Preview lens over each preview block, whose command has the asuka
// dev running the file's app show it (found in the registry); with none,
// the editor is told to start one.
func TestPreviewLenses(t *testing.T) {
	t.Setenv("ASUKA_DEV_REGISTRY", t.TempDir())
	root := t.TempDir()
	path := filepath.Join(root, "app.asuka")
	if err := os.WriteFile(path, []byte(previewApp), 0o644); err != nil {
		t.Fatal(err)
	}
	c := newClient(t, root)
	uri, _ := c.open(path)
	c.waitDiags(uri, noDiags)

	var lenses []CodeLens
	if err := c.call("textDocument/codeLens", &CodeLensParams{TextDocument: TextDocumentIdentifier{URI: uri}}, &lenses); err != nil {
		t.Fatal(err)
	}
	if len(lenses) != 2 {
		t.Fatalf("%d lenses: %+v", len(lenses), lenses)
	}
	var args []previewArgs
	for _, l := range lenses {
		if l.Command == nil || l.Command.Title != "▶ Preview" || l.Command.Command != cmdShowPreview || len(l.Command.Arguments) != 1 {
			t.Fatalf("lens %+v", l)
		}
		b, _ := json.Marshal(l.Command.Arguments[0])
		var a previewArgs
		json.Unmarshal(b, &a)
		args = append(args, a)
	}
	if args[0].Name != "Card" || args[0].Line != 8 || lenses[0].Range.Start.Line != 7 || args[1].Name != "Dark, counting" || args[1].Line != 10 || args[0].File != path {
		t.Errorf("lenses' arguments %+v, ranges %+v %+v", args, lenses[0].Range, lenses[1].Range)
	}

	// No asuka dev runs the app: the editor says so.
	raw, _ := json.Marshal(args[1])
	if err := c.call("workspace/executeCommand", &ExecuteCommandParams{Command: cmdShowPreview, Arguments: []json.RawMessage{raw}}, nil); err != nil {
		t.Fatal(err)
	}
	if m := c.waitNote("window/showMessage"); !strings.Contains(m, "asuka dev isn't running this app") {
		t.Errorf("message %s", m)
	}

	// One does: it's asked to show the preview, and the app.
	asked := make(chan string, 4)
	dev := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/previews":
			w.Write([]byte(`{"previews":[]}`))
		case "/preview":
			asked <- r.Method + " " + r.URL.RawQuery
			w.WriteHeader(http.StatusNoContent)
		}
	}))
	defer dev.Close()
	forget, err := devserver.Register(root, dev.URL+"/")
	if err != nil {
		t.Fatal(err)
	}
	defer forget()
	if err := c.call("workspace/executeCommand", &ExecuteCommandParams{Command: cmdShowPreview, Arguments: []json.RawMessage{raw}}, nil); err != nil {
		t.Fatal(err)
	}
	want := "POST " + url.Values{"file": {path}, "line": {"10"}, "name": {"Dark, counting"}}.Encode()
	if got := wait(t, asked); got != want {
		t.Errorf("asked %q, want %q", got, want)
	}
	app, _ := json.Marshal(previewArgs{File: path})
	if err := c.call("workspace/executeCommand", &ExecuteCommandParams{Command: cmdShowApp, Arguments: []json.RawMessage{app}}, nil); err != nil {
		t.Fatal(err)
	}
	if got := wait(t, asked); got != "POST " {
		t.Errorf("the app: asked %q", got)
	}
}

func wait(t *testing.T, c chan string) string {
	t.Helper()
	select {
	case s := <-c:
		return s
	case <-time.After(20 * time.Second):
		t.Fatal("asuka dev was never asked")
		return ""
	}
}

// waitNote waits for a notification of method, and returns its params.
func (c *client) waitNote(method string) string {
	c.t.Helper()
	timeout := time.After(20 * time.Second)
	for {
		select {
		case m, open := <-c.notes:
			if !open {
				c.t.Fatal("the server closed the connection")
			}
			if m.Method == method {
				return string(m.Params)
			}
		case <-timeout:
			c.t.Fatalf("no %s", method)
			return ""
		}
	}
}
