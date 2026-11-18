package check

import (
	"strings"
	"unicode/utf8"

	"github.com/egoist/asuka/lang/diag"
	"github.com/egoist/asuka/lang/syntax"
)

// An interpolation's format, "{price:>8.2}": Python's and Rust's
// mini-language, [[fill]align][sign][#][0][width][,|_][.precision][type].
// The checker reads it to say what's wrong where it's written; the
// runtime's str.fmt_int, fmt_float and fmt_text (core/src/rt/lib.c) read
// the same text as they format.
type FormatSpec struct {
	Fill      string // one character, " " unless it says
	Align     byte   // '<', '>', '^', or 0: the value's own (numbers right, text left)
	Sign      byte   // '+', ' ', or 0: a minus only
	Alt       bool   // #: 0x, 0o, 0b before the digits
	Zero      bool   // 0: zeros after the sign, up to the width
	Width     int    // -1: none
	Group     byte   // ',' or '_' between groups of digits, or 0
	Precision int    // -1: none
	Type      byte   // 'd', 'x', 'X', 'o', 'b', 'f', 'e', '%', or 0
}

// formatLimit is the largest width and precision a format says: more is
// a mistake, not a column.
const formatLimit = 1000

// ParseFormatSpec reads a format; the problem, "" when there's none.
func ParseFormatSpec(text string) (FormatSpec, string) {
	f := FormatSpec{Fill: " ", Width: -1, Precision: -1}
	s := text
	isAlign := func(c byte) bool { return c == '<' || c == '>' || c == '^' }
	if r, n := utf8.DecodeRuneInString(s); n > 0 && n < len(s) && isAlign(s[n]) {
		if r == '{' || r == '}' {
			return f, "a brace can't fill"
		}
		f.Fill, f.Align, s = s[:n], s[n], s[n+1:]
	} else if s != "" && isAlign(s[0]) {
		f.Align, s = s[0], s[1:]
	}
	if s != "" && (s[0] == '+' || s[0] == '-' || s[0] == ' ') {
		if s[0] != '-' {
			f.Sign = s[0]
		}
		s = s[1:]
	}
	if s != "" && s[0] == '#' {
		f.Alt, s = true, s[1:]
	}
	if s != "" && s[0] == '0' {
		f.Zero, s = true, s[1:]
	}
	num := func() (int, bool) {
		n, i := 0, 0
		for i < len(s) && s[i] >= '0' && s[i] <= '9' {
			if n <= formatLimit {
				n = n*10 + int(s[i]-'0')
			}
			i++
		}
		s = s[i:]
		return n, i > 0
	}
	if n, ok := num(); ok {
		f.Width = n
	}
	if s != "" && (s[0] == ',' || s[0] == '_') {
		f.Group, s = s[0], s[1:]
	}
	if s != "" && s[0] == '.' {
		s = s[1:]
		n, ok := num()
		if !ok {
			return f, "a . needs the precision after it: .2"
		}
		f.Precision = n
	}
	if s != "" && strings.IndexByte("dxXobfe%", s[0]) >= 0 {
		f.Type, s = s[0], s[1:]
	}
	if s != "" {
		r, _ := utf8.DecodeRuneInString(s)
		return f, "a format doesn't take " + string(r) + " here: it's [[fill]align][sign][#][0][width][,|_][.precision][type]"
	}
	if f.Width > formatLimit || f.Precision > formatLimit {
		return f, "a width or a precision past 1000"
	}
	return f, ""
}

// formatSpec checks the format of an interpolated value of type t.
func (c *Checker) formatSpec(spec *syntax.FormatSpec, t Type) {
	f, problem := ParseFormatSpec(spec.Text)
	if problem != "" {
		c.errorf(spec, diag.FormatSpec, "%s", problem)
		return
	}
	if IsInvalid(t) {
		return
	}
	bad := func(format string, args ...any) { c.errorf(spec, diag.FormatSpec, format, args...) }
	switch t {
	case Int:
		switch {
		case f.Precision >= 0 && (f.Type == 0 || !strings.ContainsRune("fe%", rune(f.Type))):
			bad("an int has no precision: give it a float's type to have one ({n:.2f}), or leave it out")
		case f.Alt && (f.Type == 0 || !strings.ContainsRune("xXob", rune(f.Type))):
			bad("# writes 0x, 0o or 0b: it goes with x, X, o or b ({n:#x})")
		case f.Group == ',' && f.Type != 0 && strings.ContainsRune("xXob", rune(f.Type)):
			bad(", groups decimal digits by three: hexadecimal, octal and binary group with _ ({n:_x})")
		}
	case Float:
		switch {
		case f.Type != 0 && !strings.ContainsRune("fe%", rune(f.Type)):
			bad("a float is written as f, e or %%, not %c: an int's digits come from int(x) ({int(x):%c})", f.Type, f.Type)
		case f.Alt:
			bad("# goes with an int's x, X, o or b, not a float")
		}
	default:
		what := aType(t)
		switch {
		case f.Type != 0:
			bad("%c is a number's type, and this is %s", f.Type, what)
		case f.Sign != 0 || f.Alt || f.Zero || f.Group != 0:
			bad("a sign, #, 0 and grouping are a number's, and this is %s: its text takes a fill, an alignment, a width and a precision (the most characters)", what)
		}
	}
}
