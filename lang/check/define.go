package check

import (
	"regexp"
	"strconv"

	"github.com/egoist/asuka/lang/diag"
	"github.com/egoist/asuka/lang/syntax"
)

// A Define is a value the build gives the program (@define): one of
// asuka.toml's [defines] (a variant's over them), or --define's.
type Define struct {
	// Value is a string, an int64, a float64 or a bool: asuka.toml's,
	// of its type. With Text, it's a string --define gave, which takes
	// the type the program reads it as (a default's).
	Value any
	Text  bool
}

// SetDefines gives the checker the build's values, by key, before any
// package is checked: what @define reads.
func (c *Checker) SetDefines(m map[string]Define) { c.defines = m }

// DefineKey is what a @define's key may be: letters, digits and _, a
// letter or _ first.
var DefineKey = regexp.MustCompile(`^[A-Za-z_][A-Za-z0-9_]*$`)

// defineExpr checks @define("KEY") and @define("KEY", default): the
// build's value of the key, of its type, or (with a default, when the
// build gives none) the default, whose type is the expression's.
// Info.Defines has the value.
func (c *Checker) defineExpr(e *syntax.AtExpr, want Type) Type {
	if e.Args == nil {
		c.errorf(e, diag.InvalidDefine, `@define takes the key of a value the build gives: @define("API_URL"), or with a default, @define("API_URL", "https://example.com")`)
		return Invalid
	}
	args := e.Args.List
	for _, a := range args {
		if a.Label != nil {
			c.errorf(a.Label, diag.InvalidDefine, "@define's arguments have no labels: @define(\"KEY\") or @define(\"KEY\", default)")
			return Invalid
		}
	}
	if len(args) == 0 || len(args) > 2 {
		c.errorf(e, diag.InvalidDefine, "@define takes a key and maybe a default: @define(\"KEY\") or @define(\"KEY\", default)")
		for _, a := range args {
			c.expr(a.Value, nil)
		}
		return Invalid
	}
	lit, ok := args[0].Value.(*syntax.StringLit)
	if !ok || len(lit.Interps) > 0 {
		c.expr(args[0].Value, nil)
		c.errorf(args[0].Value, diag.InvalidDefine, "@define's key is a string literal, without {…}: the build's value is known as the program compiles")
		return Invalid
	}
	c.record(lit, String)
	key, err := Unescape(lit.Segments[0], lit.Raw)
	if err != nil || !DefineKey.MatchString(key) {
		c.errorf(lit, diag.InvalidDefine, "@define's key %q isn't one: letters, digits and _, a letter or _ first (API_URL)", key)
		return Invalid
	}
	d, given := c.defines[key]
	if len(args) == 1 {
		if !given {
			c.errorf(e, diag.InvalidDefine, "the build defines no %s: give it a value in asuka.toml's [defines] (or with --define %s=…), or a default: @define(%q, …)", key, key, key)
			return Invalid
		}
		t, v := defineOf(d, Unopt(want) == Float)
		c.info.Defines[e] = v
		return t
	}
	def := args[1].Value
	t, dv, ok := c.defineDefault(def, want)
	if !ok {
		return Invalid
	}
	if !given {
		c.info.Defines[e] = dv
		return t
	}
	v, msg := defineAs(d, t)
	if msg != "" {
		c.errorf(e, diag.InvalidDefine, "the build's %s is %s, and its default says it's %s", key, msg, typeArticle(t))
		return Invalid
	}
	c.info.Defines[e] = v
	return t
}

// defineOf is a define's type and value with no default to say it: its
// own (a --define's text is a string), an integer a float where one is
// expected (asFloat).
func defineOf(d Define, asFloat bool) (Type, any) {
	switch v := d.Value.(type) {
	case int64:
		if asFloat && !d.Text {
			return Float, float64(v)
		}
		return Int, v
	case float64:
		return Float, v
	case bool:
		return Bool, v
	case string:
		return String, v
	}
	return Invalid, nil
}

// defineAs is the build's value d as type t (a default's): a --define's
// text converted, asuka.toml's of that type (an integer for a float),
// or what d is instead ("a string", "\"abc\", not an int").
func defineAs(d Define, t Type) (any, string) {
	if d.Text {
		s, _ := d.Value.(string)
		switch t {
		case String:
			return s, ""
		case Int:
			if n, err := strconv.ParseInt(s, 0, 64); err == nil {
				return n, ""
			}
		case Float:
			if f, err := strconv.ParseFloat(s, 64); err == nil {
				return f, ""
			}
		case Bool:
			if b, err := strconv.ParseBool(s); err == nil {
				return b, ""
			}
		}
		return nil, strconv.Quote(s) + ", not " + typeArticle(t)
	}
	switch v := d.Value.(type) {
	case string:
		if t == String {
			return v, ""
		}
		return nil, "a string"
	case int64:
		switch t {
		case Int:
			return v, ""
		case Float:
			return float64(v), ""
		}
		return nil, "an int"
	case float64:
		if t == Float {
			return v, ""
		}
		return nil, "a float"
	case bool:
		if t == Bool {
			return v, ""
		}
		return nil, "a bool"
	}
	return nil, "of no type a @define has"
}

func typeArticle(t Type) string {
	switch t {
	case Int:
		return "an int"
	case Float:
		return "a float"
	case Bool:
		return "a bool"
	case String:
		return "a string"
	}
	return t.String()
}

// defineDefault checks a @define's default, a literal (a string, an
// int, a float, a bool; a negative number), and is its type and value.
func (c *Checker) defineDefault(e syntax.Expr, want Type) (Type, any, bool) {
	w := Type(nil)
	if Unopt(want) == Float {
		w = Float // 3 where a float is expected is 3.0
	}
	t := c.expr(e, w)
	bad := func() (Type, any, bool) {
		c.errorf(e, diag.InvalidDefine, "@define's default is a literal: a string, an int, a float or a bool (\"https://example.com\", 8080, 0.5, false)")
		return Invalid, nil, false
	}
	neg := false
	x := e
	if u, ok := x.(*syntax.UnaryExpr); ok && u.Op == syntax.SUB {
		neg, x = true, u.X
	}
	switch l := x.(type) {
	case *syntax.StringLit:
		if neg || len(l.Interps) > 0 || t != String {
			return bad()
		}
		seg := l.Segments[0]
		if l.Multiline {
			seg = Dedent(seg)
		}
		s, _ := Unescape(seg, l.Raw)
		return String, s, true
	case *syntax.BoolLit:
		if neg || t != Bool {
			return bad()
		}
		return Bool, l.Value, true
	case *syntax.BasicLit:
		switch {
		case l.Kind == syntax.INT && t == Float:
			u, _ := ParseUint(l.Value)
			f := float64(u)
			if neg {
				f = -f
			}
			return Float, f, true
		case l.Kind == syntax.INT && t == Int:
			n, err := ParseInt(l.Value)
			if err != nil {
				return Invalid, nil, false // reported
			}
			if neg {
				n = -n
			}
			return Int, n, true
		case l.Kind == syntax.FLOAT && t == Float:
			f, _ := ParseFloat(l.Value)
			if neg {
				f = -f
			}
			return Float, f, true
		}
	}
	return bad()
}

// IsDefine reports whether e is a @define (in parentheses or not).
func IsDefine(e syntax.Expr) bool {
	a, ok := unparen(e).(*syntax.AtExpr)
	return ok && a.Name == "define"
}
