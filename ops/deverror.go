package ops

// The error overlay asuka dev shows (the DevError command, to the root):
// an error nothing in the app caught, a module that didn't load, or a
// build that failed. Its arguments are u8 DevErrorFlags bits, then the
// title, the message, the source (the lines around where it went wrong,
// numbered) and the details (the stack, or the build's other errors),
// u32-length each. The host shows them in its own views over the main
// window, with Dismiss and, with DevErrorReload, Reload: DevErrorAction
// to the root.

// DevErrorFlags are a DevError's flags.
const (
	DevErrorReload = 1 // the overlay offers Reload: the app starts again
	DevErrorBuild  = 2 // a build that failed, not an error of the app's
)

// DevError is a DevError command's arguments.
type DevError struct {
	Flags   uint8
	Title   string
	Message string
	Source  string
	Details string
}

// AppendDevError appends a DevError command's arguments.
func AppendDevError(b []byte, e DevError) []byte {
	b = append(b, e.Flags)
	b = appendStr(b, e.Title)
	b = appendStr(b, e.Message)
	b = appendStr(b, e.Source)
	return appendStr(b, e.Details)
}

// DecodeDevError reads what AppendDevError writes.
func DecodeDevError(b []byte) (DevError, bool) {
	r := &reader{b: b}
	var e DevError
	e.Flags = r.u8()
	e.Title = string(r.bytes())
	e.Message = string(r.bytes())
	e.Source = string(r.bytes())
	e.Details = string(r.bytes())
	return e, r.err == nil
}
