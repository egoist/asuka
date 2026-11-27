// Package lsp is the Asuka language server: diagnostics, hover,
// completion, go to definition, references, rename, formatting,
// symbols, signature help, semantic tokens and quick fixes for .asuka
// files, over LSP 3.17 (JSON-RPC 2.0 on stdio: asuka lsp).
//
// A package is a directory of .asuka files; the server checks one with
// lang/compile (CheckOnly), giving it the editor's open documents as an
// overlay, so what it says is about the text being edited. A change
// re-checks its package and the packages that import it, after a short
// pause (debounce); a request checks what it needs first, so its answer
// is never about stale text.
package lsp

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"runtime/debug"
	"sort"
	"strings"
	"time"

	"github.com/egoist/asuka/internal/pkgs"
)

// Options configure a server.
type Options struct {
	// Version is the server's version, reported to the client.
	Version string
	// Debounce is how long the server waits after a change before it
	// re-checks and publishes diagnostics (150ms when zero).
	Debounce time.Duration
	// Log, if set, receives the server's log lines.
	Log io.Writer
}

// Serve runs a language server reading requests from r and writing
// responses and notifications to w, until the client says exit, r ends
// or ctx is done.
func Serve(ctx context.Context, r io.Reader, w io.Writer, opts *Options) error {
	if opts == nil {
		opts = &Options{}
	}
	s := newServer(newConn(r, w), opts)
	return s.run(ctx)
}

type server struct {
	conn *conn
	opts Options

	// The main loop's: every field below is touched only on it.
	initialized bool
	shutdown    bool
	utf8        bool // positions in UTF-8 bytes, not UTF-16 units
	snippets    bool
	roots       []string // the workspace folders

	docs  map[string]*document // open documents, by path
	pkgs  map[string]*pkgState // checked packages, by directory
	gens  map[string]int       // each directory's edit count
	dirty map[string]bool      // directories changed since diagnostics were published
	timer *time.Timer

	// Directories with .asuka files under the roots (for references
	// across packages and workspace symbols); nil until needed.
	wsDirs []string

	// Dependency resolution, by project directory (asuka.toml's).
	imports map[string]*pkgs.Imports

	events chan func()
	done   chan struct{}
}

// A document is an open file: the editor's text, unsaved or not.
type document struct {
	uri     string
	path    string
	version int
	text    []byte
}

func newServer(c *conn, opts *Options) *server {
	s := &server{
		conn:    c,
		opts:    *opts,
		docs:    map[string]*document{},
		pkgs:    map[string]*pkgState{},
		gens:    map[string]int{},
		dirty:   map[string]bool{},
		events:  make(chan func(), 16),
		done:    make(chan struct{}),
		imports: map[string]*pkgs.Imports{},
	}
	if s.opts.Debounce == 0 {
		s.opts.Debounce = 150 * time.Millisecond
	}
	return s
}

func (s *server) logf(format string, args ...any) {
	if s.opts.Log != nil {
		fmt.Fprintf(s.opts.Log, "asuka lsp: "+format+"\n", args...)
	}
}

var errExit = errors.New("exit")

func (s *server) run(ctx context.Context) error {
	defer close(s.done)
	defer func() {
		if s.timer != nil {
			s.timer.Stop()
		}
	}()
	msgs := make(chan *message)
	readErr := make(chan error, 1)
	go func() {
		for {
			m, err := s.conn.read()
			if err != nil {
				var re *rpcError
				if errors.As(err, &re) {
					// A body that isn't JSON: say so, and go on.
					s.conn.reply(json.RawMessage("null"), nil, re)
					continue
				}
				readErr <- err
				return
			}
			select {
			case msgs <- m:
			case <-ctx.Done():
				return
			case <-s.done:
				return
			}
		}
	}()
	for {
		select {
		case <-ctx.Done():
			return ctx.Err()
		case err := <-readErr:
			if errors.Is(err, io.EOF) {
				return nil
			}
			return err
		case f := <-s.events:
			s.safely("event", f)
		case m := <-msgs:
			if err := s.handle(m); errors.Is(err, errExit) {
				return nil
			}
		}
	}
}

// safely runs f, logging a panic rather than dying of it: a server that
// crashes takes the editor's features with it.
func (s *server) safely(what string, f func()) (ok bool) {
	defer func() {
		if r := recover(); r != nil {
			s.logf("panic in %s: %v\n%s", what, r, debug.Stack())
			ok = false
		}
	}()
	f()
	return true
}

func (s *server) handle(m *message) error {
	if m.isNotification() {
		if m.Method == "exit" {
			return errExit
		}
		s.safely(m.Method, func() { s.notification(m.Method, m.Params) })
		return nil
	}
	if !m.isRequest() {
		return nil // a response to a request of ours
	}
	var result any
	var err error
	if !s.safely(m.Method, func() { result, err = s.request(m.Method, m.Params) }) {
		err = &rpcError{Code: codeInternalError, Message: "the server failed on " + m.Method}
	}
	s.conn.reply(m.ID, result, err)
	return nil
}

func decode[T any](raw json.RawMessage) (*T, error) {
	v := new(T)
	if len(raw) == 0 {
		return v, nil
	}
	if err := json.Unmarshal(raw, v); err != nil {
		return nil, &rpcError{Code: codeInvalidParams, Message: err.Error()}
	}
	return v, nil
}

func (s *server) request(method string, raw json.RawMessage) (any, error) {
	if method == "initialize" {
		p, err := decode[InitializeParams](raw)
		if err != nil {
			return nil, err
		}
		return s.initialize(p), nil
	}
	if !s.initialized {
		return nil, &rpcError{Code: codeServerNotInit, Message: "initialize first"}
	}
	if s.shutdown && method != "shutdown" {
		return nil, &rpcError{Code: codeInvalidRequest, Message: "the server is shutting down"}
	}
	switch method {
	case "shutdown":
		s.shutdown = true
		return nil, nil
	case "textDocument/hover":
		return withParams(raw, s.hover)
	case "textDocument/completion":
		return withParams(raw, s.completion)
	case "textDocument/signatureHelp":
		return withParams(raw, s.signatureHelp)
	case "textDocument/definition":
		return withParams(raw, s.definition)
	case "textDocument/typeDefinition":
		return withParams(raw, s.typeDefinition)
	case "textDocument/references":
		return withParams(raw, s.references)
	case "textDocument/documentHighlight":
		return withParams(raw, s.documentHighlight)
	case "textDocument/prepareRename":
		return withParams(raw, s.prepareRename)
	case "textDocument/rename":
		return withParams(raw, s.rename)
	case "textDocument/formatting":
		return withParams(raw, s.formatting)
	case "textDocument/codeAction":
		return withParams(raw, s.codeAction)
	case "textDocument/documentSymbol":
		return withParams(raw, s.documentSymbol)
	case "workspace/symbol":
		return withParams(raw, s.workspaceSymbol)
	case "textDocument/semanticTokens/full":
		return withParams(raw, s.semanticTokens)
	case "textDocument/codeLens":
		return withParams(raw, s.codeLens)
	case "workspace/executeCommand":
		return withParams(raw, s.executeCommand)
	}
	return nil, &rpcError{Code: codeMethodNotFound, Message: "no method " + method}
}

func withParams[P, R any](raw json.RawMessage, f func(*P) (R, error)) (any, error) {
	p, err := decode[P](raw)
	if err != nil {
		return nil, err
	}
	return f(p)
}

func (s *server) notification(method string, raw json.RawMessage) {
	if !s.initialized && method != "initialized" {
		return
	}
	switch method {
	case "initialized":
	case "textDocument/didOpen":
		if p, err := decode[DidOpenTextDocumentParams](raw); err == nil {
			s.didOpen(p)
		}
	case "textDocument/didChange":
		if p, err := decode[DidChangeTextDocumentParams](raw); err == nil {
			s.didChange(p)
		}
	case "textDocument/didClose":
		if p, err := decode[DidCloseTextDocumentParams](raw); err == nil {
			s.didClose(p)
		}
	case "textDocument/didSave":
		if p, err := decode[DidSaveTextDocumentParams](raw); err == nil {
			s.didSave(p)
		}
	case "workspace/didChangeWatchedFiles":
		if p, err := decode[DidChangeWatchedFilesParams](raw); err == nil {
			s.didChangeWatchedFiles(p)
		}
	case "workspace/didChangeWorkspaceFolders":
		if p, err := decode[DidChangeWorkspaceFoldersParams](raw); err == nil {
			for _, f := range p.Event.Removed {
				s.roots = remove(s.roots, uriToPath(f.URI))
			}
			for _, f := range p.Event.Added {
				if d := uriToPath(f.URI); d != "" {
					s.roots = append(s.roots, d)
				}
			}
			s.wsDirs = nil
		}
	}
}

func remove(list []string, x string) []string {
	out := list[:0]
	for _, y := range list {
		if y != x {
			out = append(out, y)
		}
	}
	return out
}

// The token types and modifiers of semantic tokens, by index.
var (
	tokenTypes     = []string{"namespace", "type", "struct", "enum", "interface", "typeParameter", "parameter", "variable", "property", "enumMember", "event", "function", "method", "class", "macro"}
	tokenModifiers = []string{"declaration", "readonly", "defaultLibrary", "modification"}
)

func (s *server) initialize(p *InitializeParams) *InitializeResult {
	s.initialized = true
	for _, e := range p.Capabilities.General.PositionEncodings {
		if e == "utf-8" {
			s.utf8 = true
		}
	}
	s.snippets = p.Capabilities.TextDocument.Completion.CompletionItem.SnippetSupport
	for _, f := range p.WorkspaceFolders {
		if d := uriToPath(f.URI); d != "" {
			s.roots = append(s.roots, d)
		}
	}
	if len(s.roots) == 0 {
		if d := uriToPath(p.RootURI); d != "" {
			s.roots = append(s.roots, d)
		} else if p.RootPath != "" {
			s.roots = append(s.roots, filepath.Clean(p.RootPath))
		}
	}
	enc := "utf-16"
	if s.utf8 {
		enc = "utf-8"
	}
	caps := ServerCapabilities{
		PositionEncoding: enc,
		TextDocumentSync: TextDocumentSyncOptions{OpenClose: true, Change: 2, Save: &SaveOptions{}},
		HoverProvider:    true,
		CompletionProvider: &CompletionOptions{
			TriggerCharacters: []string{".", "@", "/"},
		},
		SignatureHelpProvider: &SignatureHelpOptions{
			TriggerCharacters:   []string{"(", ","},
			RetriggerCharacters: []string{":"},
		},
		DefinitionProvider:         true,
		TypeDefinitionProvider:     true,
		ReferencesProvider:         true,
		DocumentHighlightProvider:  true,
		DocumentSymbolProvider:     true,
		WorkspaceSymbolProvider:    true,
		DocumentFormattingProvider: true,
		RenameProvider:             &RenameOptions{PrepareProvider: true},
		CodeActionProvider:         &CodeActionOptions{CodeActionKinds: []string{"quickfix"}},
		SemanticTokensProvider: &SemanticTokensOptions{
			Legend: SemanticTokensLegend{TokenTypes: tokenTypes, TokenModifiers: tokenModifiers},
			Full:   true,
		},
		CodeLensProvider:       &CodeLensOptions{},
		ExecuteCommandProvider: &ExecuteCommandOptions{Commands: []string{cmdShowPreview, cmdShowApp}},
		Workspace:              &WorkspaceServerCapabity{},
	}
	caps.Workspace.WorkspaceFolders.Supported = true
	caps.Workspace.WorkspaceFolders.ChangeNotifications = true
	v := s.opts.Version
	if v == "" {
		v = "dev"
	}
	return &InitializeResult{Capabilities: caps, ServerInfo: &ServerInfo{Name: "asuka", Version: v}}
}

// ---------------------------------------------------------------------
// Documents

func (s *server) didOpen(p *DidOpenTextDocumentParams) {
	path := uriToPath(p.TextDocument.URI)
	if path == "" {
		return
	}
	s.docs[path] = &document{uri: p.TextDocument.URI, path: path, version: p.TextDocument.Version, text: []byte(p.TextDocument.Text)}
	s.changed(filepath.Dir(path))
	// A file just opened gets its diagnostics now, not after the pause.
	s.flush()
}

func (s *server) didChange(p *DidChangeTextDocumentParams) {
	path := uriToPath(p.TextDocument.URI)
	d := s.docs[path]
	if d == nil {
		return
	}
	for _, c := range p.ContentChanges {
		if c.Range == nil {
			d.text = []byte(c.Text)
			continue
		}
		m := newMapper(d.text, s.utf8)
		start, end := m.offset(c.Range.Start), m.offset(c.Range.End)
		if end < start {
			start, end = end, start
		}
		text := make([]byte, 0, len(d.text)-(end-start)+len(c.Text))
		text = append(text, d.text[:start]...)
		text = append(text, c.Text...)
		text = append(text, d.text[end:]...)
		d.text = text
	}
	d.version = p.TextDocument.Version
	s.changed(filepath.Dir(path))
	s.schedule()
}

func (s *server) didClose(p *DidCloseTextDocumentParams) {
	path := uriToPath(p.TextDocument.URI)
	if _, ok := s.docs[path]; !ok {
		return
	}
	delete(s.docs, path)
	dir := filepath.Dir(path)
	s.changed(dir)
	if !s.active(dir) {
		// Nothing of its package is open: forget its diagnostics, unless
		// an open package still imports it.
		if st := s.pkgs[dir]; st != nil {
			s.publishNone(st)
			delete(s.pkgs, dir)
		}
		delete(s.dirty, dir)
	}
	s.schedule()
}

func (s *server) didSave(p *DidSaveTextDocumentParams) {
	path := uriToPath(p.TextDocument.URI)
	if d := s.docs[path]; d != nil && p.Text != nil {
		d.text = []byte(*p.Text)
	}
	// The overlay already had the text; what's on disk changed for
	// packages that read it from there (none while it's open).
}

func (s *server) didChangeWatchedFiles(p *DidChangeWatchedFilesParams) {
	for _, c := range p.Changes {
		path := uriToPath(c.URI)
		if path == "" {
			continue
		}
		if strings.HasSuffix(path, ".asuka") {
			if _, open := s.docs[path]; !open {
				s.changed(filepath.Dir(path))
			}
			if c.Type != 2 {
				s.wsDirs = nil // a package may have come or gone
			}
		} else {
			// asuka.toml, asuka.lock, a directory: anything may resolve
			// differently.
			s.imports = map[string]*pkgs.Imports{}
			for dir := range s.pkgs {
				s.changed(dir)
			}
		}
	}
	s.schedule()
}

// changed notes that dir's package changed: what checked it is stale.
func (s *server) changed(dir string) {
	s.gens[dir]++
	s.dirty[dir] = true
}

// schedule re-checks, after the pause, what changed.
func (s *server) schedule() {
	if s.timer != nil {
		s.timer.Stop()
	}
	s.timer = time.AfterFunc(s.opts.Debounce, func() {
		select {
		case s.events <- s.flush:
		case <-s.done:
		}
	})
}

// flush re-checks the packages that changed and the open packages that
// import them, and publishes their diagnostics.
func (s *server) flush() {
	if s.timer != nil {
		s.timer.Stop()
		s.timer = nil
	}
	if len(s.dirty) == 0 {
		return
	}
	dirty := s.dirty
	s.dirty = map[string]bool{}
	todo := map[string]bool{}
	for dir := range dirty {
		if s.active(dir) && !isPreludeCopy(dir) {
			todo[dir] = true
		}
	}
	// The packages that import one that changed.
	for dir, st := range s.pkgs {
		for d := range dirty {
			if _, ok := st.gens[d]; ok && dir != d && s.active(dir) {
				todo[dir] = true
			}
		}
	}
	dirs := make([]string, 0, len(todo))
	for d := range todo {
		dirs = append(dirs, d)
	}
	sort.Strings(dirs)
	for _, dir := range dirs {
		if st := s.check(dir); st != nil {
			s.publish(st)
		}
	}
}

// active reports whether a package's diagnostics are wanted: a file of
// it is open.
func (s *server) active(dir string) bool {
	for p := range s.docs {
		if filepath.Dir(p) == dir {
			return true
		}
	}
	return false
}

// overlay is the open documents' text, by path.
func (s *server) overlay() map[string][]byte {
	m := make(map[string][]byte, len(s.docs))
	for p, d := range s.docs {
		if strings.HasSuffix(p, ".asuka") {
			m[p] = d.text
		}
	}
	return m
}

// source is a file's text: the editor's when it's open, else the disk's.
func (s *server) source(path string) ([]byte, error) {
	if d := s.docs[path]; d != nil {
		return d.text, nil
	}
	if strings.HasPrefix(path, preludePrefix) {
		return preludeSource(path)
	}
	return os.ReadFile(path)
}

// uri is the URI the client knows a file by.
func (s *server) uri(path string) string {
	if d := s.docs[path]; d != nil {
		return d.uri
	}
	if strings.HasPrefix(path, preludePrefix) {
		if p, err := preludeFile(path); err == nil {
			return pathToURI(p)
		}
	}
	return pathToURI(path)
}
