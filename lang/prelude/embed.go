package prelude

import _ "embed"

// The prelude's Asuka sources, which the checker reads before any
// package: the core types, the schema's enums and the elements.
var (
	//go:embed types.asuka
	typesSrc []byte
	//go:embed enums.asuka
	enumsSrc []byte
	//go:embed elements.asuka
	elementsSrc []byte
	//go:embed std_json.asuka
	stdJSONSrc []byte
)

// StdJSON is the Asuka of std/json, a package the compiler provides:
// its Value and parse, checked with the compiler's decode and encode in
// its scope (lang/check's stdJSON); the documentation reads it too.
func StdJSON() Source { return Source{"std/json/json.asuka", stdJSONSrc} }

// Source is a prelude file: its name and its text.
type Source struct {
	Name string
	Text []byte
}

// Sources are the prelude's files, in the order the checker reads them.
func Sources() []Source {
	return []Source{
		{"prelude/types.asuka", typesSrc},
		{"prelude/enums.asuka", enumsSrc},
		{"prelude/elements.asuka", elementsSrc},
	}
}
