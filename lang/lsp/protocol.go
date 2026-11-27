package lsp

import "encoding/json"

// The parts of LSP 3.17 the server speaks.

type Position struct {
	Line      int `json:"line"`
	Character int `json:"character"`
}

type Range struct {
	Start Position `json:"start"`
	End   Position `json:"end"`
}

type Location struct {
	URI   string `json:"uri"`
	Range Range  `json:"range"`
}

type TextDocumentIdentifier struct {
	URI string `json:"uri"`
}

type TextDocumentPositionParams struct {
	TextDocument TextDocumentIdentifier `json:"textDocument"`
	Position     Position               `json:"position"`
}

type TextEdit struct {
	Range   Range  `json:"range"`
	NewText string `json:"newText"`
}

// Initialization.

type InitializeParams struct {
	ProcessID        *int               `json:"processId"`
	RootURI          string             `json:"rootUri"`
	RootPath         string             `json:"rootPath"`
	WorkspaceFolders []WorkspaceFolder  `json:"workspaceFolders"`
	Capabilities     ClientCapabilities `json:"capabilities"`
}

type WorkspaceFolder struct {
	URI  string `json:"uri"`
	Name string `json:"name"`
}

type ClientCapabilities struct {
	General struct {
		PositionEncodings []string `json:"positionEncodings"`
	} `json:"general"`
	TextDocument struct {
		Completion struct {
			CompletionItem struct {
				SnippetSupport bool `json:"snippetSupport"`
			} `json:"completionItem"`
		} `json:"completion"`
		Hover struct {
			ContentFormat []string `json:"contentFormat"`
		} `json:"hover"`
		DocumentSymbol struct {
			HierarchicalDocumentSymbolSupport bool `json:"hierarchicalDocumentSymbolSupport"`
		} `json:"documentSymbol"`
		PublishDiagnostics struct {
			VersionSupport bool `json:"versionSupport"`
		} `json:"publishDiagnostics"`
	} `json:"textDocument"`
}

type InitializeResult struct {
	Capabilities ServerCapabilities `json:"capabilities"`
	ServerInfo   *ServerInfo        `json:"serverInfo,omitempty"`
}

type ServerInfo struct {
	Name    string `json:"name"`
	Version string `json:"version,omitempty"`
}

type ServerCapabilities struct {
	PositionEncoding           string                   `json:"positionEncoding,omitempty"`
	TextDocumentSync           TextDocumentSyncOptions  `json:"textDocumentSync"`
	HoverProvider              bool                     `json:"hoverProvider"`
	CompletionProvider         *CompletionOptions       `json:"completionProvider,omitempty"`
	SignatureHelpProvider      *SignatureHelpOptions    `json:"signatureHelpProvider,omitempty"`
	DefinitionProvider         bool                     `json:"definitionProvider"`
	TypeDefinitionProvider     bool                     `json:"typeDefinitionProvider"`
	ReferencesProvider         bool                     `json:"referencesProvider"`
	DocumentHighlightProvider  bool                     `json:"documentHighlightProvider"`
	DocumentSymbolProvider     bool                     `json:"documentSymbolProvider"`
	WorkspaceSymbolProvider    bool                     `json:"workspaceSymbolProvider"`
	DocumentFormattingProvider bool                     `json:"documentFormattingProvider"`
	RenameProvider             *RenameOptions           `json:"renameProvider,omitempty"`
	CodeActionProvider         *CodeActionOptions       `json:"codeActionProvider,omitempty"`
	SemanticTokensProvider     *SemanticTokensOptions   `json:"semanticTokensProvider,omitempty"`
	CodeLensProvider           *CodeLensOptions         `json:"codeLensProvider,omitempty"`
	ExecuteCommandProvider     *ExecuteCommandOptions   `json:"executeCommandProvider,omitempty"`
	Workspace                  *WorkspaceServerCapabity `json:"workspace,omitempty"`
}

type WorkspaceServerCapabity struct {
	WorkspaceFolders struct {
		Supported           bool `json:"supported"`
		ChangeNotifications bool `json:"changeNotifications"`
	} `json:"workspaceFolders"`
}

type TextDocumentSyncOptions struct {
	OpenClose bool         `json:"openClose"`
	Change    int          `json:"change"` // 1 full, 2 incremental
	Save      *SaveOptions `json:"save,omitempty"`
}

type SaveOptions struct {
	IncludeText bool `json:"includeText"`
}

type CompletionOptions struct {
	TriggerCharacters []string `json:"triggerCharacters,omitempty"`
	ResolveProvider   bool     `json:"resolveProvider"`
}

type SignatureHelpOptions struct {
	TriggerCharacters   []string `json:"triggerCharacters,omitempty"`
	RetriggerCharacters []string `json:"retriggerCharacters,omitempty"`
}

type RenameOptions struct {
	PrepareProvider bool `json:"prepareProvider"`
}

type SemanticTokensOptions struct {
	Legend SemanticTokensLegend `json:"legend"`
	Full   bool                 `json:"full"`
	Range  bool                 `json:"range"`
}

type SemanticTokensLegend struct {
	TokenTypes     []string `json:"tokenTypes"`
	TokenModifiers []string `json:"tokenModifiers"`
}

// Documents.

type TextDocumentItem struct {
	URI        string `json:"uri"`
	LanguageID string `json:"languageId"`
	Version    int    `json:"version"`
	Text       string `json:"text"`
}

type DidOpenTextDocumentParams struct {
	TextDocument TextDocumentItem `json:"textDocument"`
}

type VersionedTextDocumentIdentifier struct {
	URI     string `json:"uri"`
	Version int    `json:"version"`
}

type TextDocumentContentChangeEvent struct {
	Range *Range `json:"range,omitempty"`
	Text  string `json:"text"`
}

type DidChangeTextDocumentParams struct {
	TextDocument   VersionedTextDocumentIdentifier  `json:"textDocument"`
	ContentChanges []TextDocumentContentChangeEvent `json:"contentChanges"`
}

type DidCloseTextDocumentParams struct {
	TextDocument TextDocumentIdentifier `json:"textDocument"`
}

type DidSaveTextDocumentParams struct {
	TextDocument TextDocumentIdentifier `json:"textDocument"`
	Text         *string                `json:"text,omitempty"`
}

type DidChangeWatchedFilesParams struct {
	Changes []FileEvent `json:"changes"`
}

type FileEvent struct {
	URI  string `json:"uri"`
	Type int    `json:"type"` // 1 created, 2 changed, 3 deleted
}

type DidChangeWorkspaceFoldersParams struct {
	Event struct {
		Added   []WorkspaceFolder `json:"added"`
		Removed []WorkspaceFolder `json:"removed"`
	} `json:"event"`
}

// Diagnostics.

type PublishDiagnosticsParams struct {
	URI         string       `json:"uri"`
	Version     *int         `json:"version,omitempty"`
	Diagnostics []Diagnostic `json:"diagnostics"`
}

type Diagnostic struct {
	Range    Range `json:"range"`
	Severity int   `json:"severity"` // 1 error, 2 warning, 3 information, 4 hint
	// Code is the diagnostic's code (lang/diag), and CodeDescription its
	// page (LSP 3.16).
	Code            string           `json:"code,omitempty"`
	CodeDescription *CodeDescription `json:"codeDescription,omitempty"`
	Source          string           `json:"source"`
	Message         string           `json:"message"`
}

type CodeDescription struct {
	Href string `json:"href"`
}

// Hover.

type Hover struct {
	Contents MarkupContent `json:"contents"`
	Range    *Range        `json:"range,omitempty"`
}

type MarkupContent struct {
	Kind  string `json:"kind"` // plaintext or markdown
	Value string `json:"value"`
}

// Completion.

type CompletionParams struct {
	TextDocumentPositionParams
	Context *struct {
		TriggerKind      int    `json:"triggerKind"`
		TriggerCharacter string `json:"triggerCharacter"`
	} `json:"context,omitempty"`
}

type CompletionList struct {
	IsIncomplete bool             `json:"isIncomplete"`
	Items        []CompletionItem `json:"items"`
}

type CompletionItem struct {
	Label            string                      `json:"label"`
	LabelDetails     *CompletionItemLabelDetails `json:"labelDetails,omitempty"`
	Kind             int                         `json:"kind,omitempty"`
	Detail           string                      `json:"detail,omitempty"`
	Documentation    *MarkupContent              `json:"documentation,omitempty"`
	SortText         string                      `json:"sortText,omitempty"`
	FilterText       string                      `json:"filterText,omitempty"`
	InsertText       string                      `json:"insertText,omitempty"`
	InsertTextFormat int                         `json:"insertTextFormat,omitempty"` // 1 plain, 2 snippet
	TextEdit         *TextEdit                   `json:"textEdit,omitempty"`
	Command          *Command                    `json:"command,omitempty"`
	Preselect        bool                        `json:"preselect,omitempty"`
}

type CompletionItemLabelDetails struct {
	Detail      string `json:"detail,omitempty"`
	Description string `json:"description,omitempty"`
}

type Command struct {
	Title     string `json:"title"`
	Command   string `json:"command"`
	Arguments []any  `json:"arguments,omitempty"`
}

type CodeLensParams struct {
	TextDocument TextDocumentIdentifier `json:"textDocument"`
}

type CodeLens struct {
	Range   Range    `json:"range"`
	Command *Command `json:"command,omitempty"`
}

type CodeLensOptions struct {
	ResolveProvider bool `json:"resolveProvider"`
}

type ExecuteCommandParams struct {
	Command   string            `json:"command"`
	Arguments []json.RawMessage `json:"arguments,omitempty"`
}

type ExecuteCommandOptions struct {
	Commands []string `json:"commands"`
}

type ShowMessageParams struct {
	Type    int    `json:"type"` // 1 error, 2 warning, 3 info, 4 log
	Message string `json:"message"`
}

// The CompletionItemKinds the server uses.
const (
	kindText          = 1
	kindMethod        = 2
	kindFunction      = 3
	kindConstructor   = 4
	kindField         = 5
	kindVariable      = 6
	kindClass         = 7
	kindInterface     = 8
	kindModule        = 9
	kindProperty      = 10
	kindValue         = 12
	kindEnum          = 13
	kindKeyword       = 14
	kindSnippet       = 15
	kindColor         = 16
	kindFile          = 17
	kindFolder        = 19
	kindEnumMember    = 20
	kindConstant      = 21
	kindStruct        = 22
	kindEvent         = 23
	kindTypeParameter = 25
)

// Signature help.

type SignatureHelp struct {
	Signatures      []SignatureInformation `json:"signatures"`
	ActiveSignature int                    `json:"activeSignature"`
	ActiveParameter int                    `json:"activeParameter"`
}

type SignatureInformation struct {
	Label           string                 `json:"label"`
	Documentation   *MarkupContent         `json:"documentation,omitempty"`
	Parameters      []ParameterInformation `json:"parameters"`
	ActiveParameter *int                   `json:"activeParameter,omitempty"`
}

type ParameterInformation struct {
	Label         [2]int         `json:"label"` // offsets into the signature's label (UTF-16)
	Documentation *MarkupContent `json:"documentation,omitempty"`
}

// References and rename.

type ReferenceParams struct {
	TextDocumentPositionParams
	Context struct {
		IncludeDeclaration bool `json:"includeDeclaration"`
	} `json:"context"`
}

type RenameParams struct {
	TextDocumentPositionParams
	NewName string `json:"newName"`
}

type WorkspaceEdit struct {
	Changes map[string][]TextEdit `json:"changes"`
}

type PrepareRenameResult struct {
	Range       Range  `json:"range"`
	Placeholder string `json:"placeholder"`
}

type DocumentHighlight struct {
	Range Range `json:"range"`
	Kind  int   `json:"kind"` // 1 text, 2 read, 3 write
}

// Code actions.

type CodeActionParams struct {
	TextDocument TextDocumentIdentifier `json:"textDocument"`
	Range        Range                  `json:"range"`
	Context      struct {
		Diagnostics []Diagnostic `json:"diagnostics"`
		Only        []string     `json:"only,omitempty"`
	} `json:"context"`
}

type CodeAction struct {
	Title       string         `json:"title"`
	Kind        string         `json:"kind,omitempty"`
	Diagnostics []Diagnostic   `json:"diagnostics,omitempty"`
	IsPreferred bool           `json:"isPreferred,omitempty"`
	Edit        *WorkspaceEdit `json:"edit,omitempty"`
}

type CodeActionOptions struct {
	CodeActionKinds []string `json:"codeActionKinds,omitempty"`
}

// Formatting.

type DocumentFormattingParams struct {
	TextDocument TextDocumentIdentifier `json:"textDocument"`
}

// Symbols.

type DocumentSymbolParams struct {
	TextDocument TextDocumentIdentifier `json:"textDocument"`
}

type DocumentSymbol struct {
	Name           string           `json:"name"`
	Detail         string           `json:"detail,omitempty"`
	Kind           int              `json:"kind"`
	Range          Range            `json:"range"`
	SelectionRange Range            `json:"selectionRange"`
	Children       []DocumentSymbol `json:"children,omitempty"`
}

type WorkspaceSymbolParams struct {
	Query string `json:"query"`
}

type SymbolInformation struct {
	Name          string   `json:"name"`
	Kind          int      `json:"kind"`
	Location      Location `json:"location"`
	ContainerName string   `json:"containerName,omitempty"`
}

// The SymbolKinds the server uses.
const (
	symFile          = 1
	symModule        = 2
	symNamespace     = 3
	symPackage       = 4
	symClass         = 5
	symMethod        = 6
	symProperty      = 7
	symField         = 8
	symConstructor   = 9
	symEnum          = 10
	symInterface     = 11
	symFunction      = 12
	symVariable      = 13
	symConstant      = 14
	symString        = 15
	symObject        = 19
	symEnumMember    = 22
	symStruct        = 23
	symEvent         = 24
	symTypeParameter = 26
)

// Semantic tokens.

type SemanticTokensParams struct {
	TextDocument TextDocumentIdentifier `json:"textDocument"`
}

type SemanticTokens struct {
	Data []uint32 `json:"data"`
}

type cancelParams struct {
	ID json.RawMessage `json:"id"`
}
