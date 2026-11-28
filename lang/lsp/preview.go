package lsp

import (
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"path/filepath"
	"strings"
	"time"

	"github.com/egoist/asuka/internal/devserver"
	"github.com/egoist/asuka/lang/syntax"
)

// Previews in the editor: a code lens over each preview block, ▶
// Preview, whose command (asuka.preview.show) has the asuka dev that runs
// the file's app show it on its devices, as its inspector's Previews tab
// does (devserver.Find: the asuka devs running, by their apps'
// directories); asuka.preview.showApp shows the app again. An editor's
// own commands (VS Code's Asuka: Show Preview, at the cursor's line)
// run them too.

const (
	cmdShowPreview = "asuka.preview.show"
	cmdShowApp     = "asuka.preview.showApp"
)

// previewArgs are a lens's command's arguments: the preview's file, name
// and line.
type previewArgs struct {
	File string `json:"file"`
	Name string `json:"name,omitempty"`
	Line int    `json:"line,omitempty"`
}

func (s *server) codeLens(p *CodeLensParams) ([]CodeLens, error) {
	path := uriToPath(p.TextDocument.URI)
	src, err := s.readFile(path)
	if err != nil {
		if src, err = s.source(path); err != nil {
			return nil, nil
		}
	}
	out := []CodeLens{}
	if !strings.Contains(string(src), "preview") {
		return out, nil
	}
	f, _ := syntax.ParseFile(syntax.NewFileSet(), path, src)
	if f == nil || f.File == nil {
		return out, nil
	}
	sb := &symbolBuilder{file: f.File, m: newMapper(src, s.utf8)}
	for _, d := range f.Decls {
		pd, ok := d.(*syntax.PreviewDecl)
		if !ok {
			continue
		}
		name := previewName(pd)
		r := sb.rng(syntax.Span{Start: pd.Start, End: pd.Start})
		args := previewArgs{File: path, Name: name, Line: f.File.Line(pd.Start)}
		out = append(out, CodeLens{Range: r, Command: &Command{Title: "▶ Preview", Command: cmdShowPreview, Arguments: []any{args}}})
	}
	return out, nil
}

func previewName(pd *syntax.PreviewDecl) string {
	if pd.Name == nil {
		return ""
	}
	return strings.Join(pd.Name.Segments, "")
}

// executeCommand runs the lenses' commands: it asks the file's asuka dev,
// off the main loop (it compiles the preview), and says what went wrong
// in a message.
func (s *server) executeCommand(p *ExecuteCommandParams) (any, error) {
	var a previewArgs
	if len(p.Arguments) > 0 {
		json.Unmarshal(p.Arguments[0], &a)
	}
	switch p.Command {
	case cmdShowPreview, cmdShowApp:
	default:
		return nil, &rpcError{Code: codeInvalidParams, Message: "no command " + p.Command}
	}
	if a.File == "" {
		return nil, &rpcError{Code: codeInvalidParams, Message: p.Command + " takes the file: [{\"file\": …}]"}
	}
	if p.Command == cmdShowApp {
		a.Name, a.Line = "", 0
	}
	go func() {
		if err := showPreview(a, p.Command == cmdShowApp); err != nil {
			s.conn.notify("window/showMessage", &ShowMessageParams{Type: 2, Message: err.Error()})
		}
	}()
	return nil, nil
}

// showPreview asks the asuka dev that runs a's file's app to show its
// preview (app: the app again).
func showPreview(a previewArgs, app bool) error {
	e, ok := devserver.Find(a.File)
	if !ok {
		dir := filepath.Dir(a.File)
		return fmt.Errorf("asuka dev isn't running this app: run asuka dev in %s, then show the preview again", dir)
	}
	q := url.Values{}
	if !app {
		q.Set("file", a.File)
		q.Set("name", a.Name)
		if a.Line > 0 {
			q.Set("line", fmt.Sprint(a.Line))
		}
	}
	c := http.Client{Timeout: 2 * time.Minute}
	resp, err := c.Post(e.URL+"preview?"+q.Encode(), "text/plain", nil)
	if err != nil {
		return fmt.Errorf("asuka dev (%s): %v", e.URL, err)
	}
	defer resp.Body.Close()
	if resp.StatusCode >= 300 {
		b, _ := io.ReadAll(resp.Body)
		return fmt.Errorf("asuka dev: %s", strings.TrimSpace(string(b)))
	}
	return nil
}
