package check

import (
	"fmt"
	"io/fs"
	"math"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"

	"github.com/egoist/asuka/lang/syntax"
)

// std/embed: files of the package compiled into the app, as Go's
// go:embed puts them in a program, read as it compiles. A path is a
// string literal, relative to the package's directory (where its
// asuka.toml is), inside it:
//
//	embed.text("README.md")            the file's text
//	embed.bytes("art/logo.png")        its bytes
//	embed.json<Config>("config.json")  read as a Config, checked as it
//	                                   compiles (a mismatch is an error
//	                                   here, not at run time)
//	embed.texts("posts/*.md")          [path: text] of what a glob names
//	embed.files("icons/**/*.svg")      [path: bytes]
//
// A glob is filepath.Match's, with ** for any directories; it never
// names a file or directory whose name starts with a dot.

// EmbedPath is a path a package embeds: a file's, or a glob's.
type EmbedPath struct {
	Dir     string // the package's directory
	Pattern string // relative to it, slashes: a file's path or a glob
}

// Match reports whether file (an absolute path) is one p embeds.
func (p EmbedPath) Match(file string) bool {
	rel, err := filepath.Rel(p.Dir, file)
	if err != nil {
		return false
	}
	rel = filepath.ToSlash(rel)
	if rel == ".." || strings.HasPrefix(rel, "../") {
		return false
	}
	for _, part := range strings.Split(rel, "/") {
		if strings.HasPrefix(part, ".") {
			return false
		}
	}
	re, err := embedGlobRegexp(p.Pattern)
	return err == nil && re.MatchString(rel)
}

// EmbeddedFile is a file embed.texts or files names: its path as the
// glob found it (slashes, relative to the package) and its contents.
type EmbeddedFile struct {
	Path string
	Data string
}

func (c *Checker) embedCall(e *syntax.CallExpr, b *Builtin, call *Call, want Type) Type {
	name := "embed." + b.name
	args := c.matchArgs(e, []*Param{{Name: "path", Type: String}}, name)
	call.Args = args
	result := func() Type {
		switch b.ID {
		case BEmbedText:
			return String
		case BEmbedBytes:
			return Bytes
		case BEmbedTexts:
			return &Map{Key: String, Elem: String}
		case BEmbedFiles:
			return &Map{Key: String, Elem: Bytes}
		}
		return nil
	}()
	if b.ID == BEmbedJSON {
		result = want
		if len(e.TypeArgs) == 1 {
			result = c.typeExpr(e.TypeArgs[0])
		} else if len(e.TypeArgs) > 1 {
			c.errorf(e, "embed.json takes one type: embed.json<T>(path)")
		}
		if result == nil {
			c.errorf(e, "embed.json needs to know what it reads: embed.json<T>(path), or say what's expected, let config: Config = embed.json(path)")
			result = Invalid
		} else if why := c.jsonable(result); why != "" && !IsInvalid(result) {
			c.errorf(e, "json can't read a %s: %s", typeString(result), why)
			return result
		}
	} else if len(e.TypeArgs) > 0 {
		c.errorf(e, "%s takes no type", name)
	}
	call.Result = result
	a := args[0]
	if a == nil {
		return result
	}
	c.assign(a, String, "the path")
	path, ok := constString(a)
	if !ok {
		c.errorf(a, "a file is embedded as the app compiles: its path is a string literal, without {…}")
		return result
	}
	if c.pkg == nil || c.pkg.Dir == "" {
		c.errorf(a, "%s: this package has no directory to read %s from", name, path)
		return result
	}
	slash := filepath.ToSlash(path)
	if path == "" || filepath.IsAbs(path) || strings.HasPrefix(slash, "/") || slash == ".." || strings.HasPrefix(slash, "../") || strings.Contains(slash, "/../") || strings.HasSuffix(slash, "/..") {
		c.errorf(a, "%s: %q isn't in the package: a path is relative to its directory (where its asuka.toml is), inside it", name, path)
		return result
	}
	switch b.ID {
	case BEmbedTexts, BEmbedFiles:
		files, err := embedGlob(c.pkg.Dir, slash)
		if err != nil {
			c.errorf(a, "%s(%q): %v", name, path, err)
			return result
		}
		if len(files) == 0 {
			c.errorf(a, "%s(%q): no file in the package matches it", name, path)
		}
		c.info.EmbedPaths[EmbedPath{c.pkg.Dir, slash}] = true
		c.info.EmbeddedFiles[e] = files
		return result
	}
	file := filepath.Join(c.pkg.Dir, filepath.FromSlash(slash))
	c.info.EmbedPaths[EmbedPath{c.pkg.Dir, regexpQuoteGlob(slash)}] = true
	data, err := os.ReadFile(file)
	if err != nil {
		if os.IsNotExist(err) {
			c.errorf(a, "%s: there's no %s in the package", name, slash)
		} else {
			c.errorf(a, "%s: %v", name, err)
		}
		return result
	}
	if b.ID == BEmbedJSON && !IsInvalid(result) {
		if err := checkJSON(string(data), result); err != nil {
			c.errorf(a, "%s doesn't fit a %s: %s:%v", name, typeString(result), slash, err)
		}
	}
	c.info.Embedded[e] = string(data)
	return result
}

// embedGlob is the files the glob pattern (slashes) names in dir, sorted
// by path: filepath.Match's syntax, with ** for any directories (none
// included); never a file or directory whose name starts with a dot.
func embedGlob(dir, pattern string) ([]EmbeddedFile, error) {
	re, err := embedGlobRegexp(pattern)
	if err != nil {
		return nil, err
	}
	var out []EmbeddedFile
	err = filepath.WalkDir(dir, func(p string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if p != dir && strings.HasPrefix(d.Name(), ".") {
			if d.IsDir() {
				return filepath.SkipDir
			}
			return nil
		}
		if d.IsDir() {
			return nil
		}
		rel, _ := filepath.Rel(dir, p)
		rel = filepath.ToSlash(rel)
		if !re.MatchString(rel) {
			return nil
		}
		b, err := os.ReadFile(p)
		if err != nil {
			return err
		}
		out = append(out, EmbeddedFile{Path: rel, Data: string(b)})
		return nil
	})
	if err != nil {
		return nil, err
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Path < out[j].Path })
	return out, nil
}

// regexpQuoteGlob is a file's path as a glob that names only it.
func regexpQuoteGlob(path string) string {
	var b strings.Builder
	for _, c := range path {
		if strings.ContainsRune(`*?[\`, c) {
			b.WriteByte('\\')
		}
		b.WriteRune(c)
	}
	return b.String()
}

// embedGlobRegexp is a glob as a regexp over slash paths: * and ? within
// a name, [...] a class, ** any directories.
func embedGlobRegexp(g string) (*regexp.Regexp, error) {
	var b strings.Builder
	b.WriteString("^")
	for i := 0; i < len(g); i++ {
		switch c := g[i]; {
		case strings.HasPrefix(g[i:], "**/"):
			b.WriteString("(?:[^/]+/)*")
			i += 2
		case strings.HasPrefix(g[i:], "**"):
			b.WriteString(".*")
			i++
		case c == '*':
			b.WriteString("[^/]*")
		case c == '?':
			b.WriteString("[^/]")
		case c == '[':
			j := strings.IndexByte(g[i:], ']')
			if j < 0 {
				return nil, fmt.Errorf("a [ without its ]")
			}
			class := g[i+1 : i+j]
			if strings.HasPrefix(class, "!") {
				class = "^" + class[1:]
			}
			b.WriteString("[" + strings.ReplaceAll(class, `\`, `\\`) + "]")
			i += j
		case c == '\\' && i+1 < len(g):
			i++
			b.WriteString(regexp.QuoteMeta(string(g[i])))
		default:
			b.WriteString(regexp.QuoteMeta(string(c)))
		}
	}
	b.WriteString("$")
	return regexp.Compile(b.String())
}

// ---- JSON, checked as it compiles ----
//
// checkJSON reports whether text reads as a value of t as the runtime's
// json.decode reads it (core/src/rt/json.c), so embed.json can't fail as
// the app runs: the same rules (a field the object doesn't have is its
// type's empty value, but a missing struct, enum with payloads or bytes
// is an error; unknown fields skipped; an int may be written 2.0), the
// same messages, at line:col of the file.

type jsonErr struct {
	line, col int
	msg       string
}

func (e *jsonErr) Error() string { return fmt.Sprintf("%d:%d: %s", e.line, e.col, e.msg) }

type jsonCheck struct {
	s   string
	i   int
	err *jsonErr
}

func checkJSON(text string, t Type) error {
	p := &jsonCheck{s: text}
	p.value(t)
	p.ws()
	if p.err == nil && p.i != len(p.s) {
		p.fail("more after the value")
	}
	if p.err != nil {
		return p.err
	}
	return nil
}

func (p *jsonCheck) fail(format string, args ...any) {
	if p.err != nil {
		return
	}
	line, col := 1, 1
	for _, c := range p.s[:min(p.i, len(p.s))] {
		if c == '\n' {
			line, col = line+1, 1
		} else {
			col++
		}
	}
	p.err = &jsonErr{line, col, fmt.Sprintf(format, args...)}
}

func (p *jsonCheck) failed() bool { return p.err != nil }

func (p *jsonCheck) ws() {
	for p.i < len(p.s) && (p.s[p.i] == ' ' || p.s[p.i] == '\t' || p.s[p.i] == '\n' || p.s[p.i] == '\r') {
		p.i++
	}
}

func (p *jsonCheck) lit(w string) bool {
	if strings.HasPrefix(p.s[p.i:], w) {
		p.i += len(w)
		return true
	}
	return false
}

func (p *jsonCheck) hex4() {
	if len(p.s)-p.i < 4 {
		p.fail(`a short \u escape`)
		return
	}
	for k := 0; k < 4; k++ {
		c := p.s[p.i+k]
		if !(c >= '0' && c <= '9' || c >= 'a' && c <= 'f' || c >= 'A' && c <= 'F') {
			p.fail(`a bad \u escape`)
			return
		}
	}
	p.i += 4
}

// str reads a string, its value.
func (p *jsonCheck) str() string {
	if p.i >= len(p.s) || p.s[p.i] != '"' {
		p.fail("expected a string")
		return ""
	}
	p.i++
	var b strings.Builder
	for p.i < len(p.s) && p.s[p.i] != '"' {
		c := p.s[p.i]
		p.i++
		if c != '\\' {
			b.WriteByte(c)
			continue
		}
		if p.i >= len(p.s) {
			break
		}
		c = p.s[p.i]
		p.i++
		switch c {
		case '"', '\\', '/':
			b.WriteByte(c)
		case 'b', 'f', 'n', 'r', 't':
			b.WriteByte(' ')
		case 'u':
			p.hex4()
			if p.i+6 <= len(p.s) && p.s[p.i] == '\\' && p.s[p.i+1] == 'u' {
				// A surrogate pair's second half (read as the runtime does
				// only after a high surrogate, which this doesn't need to
				// tell: either way it's four hex digits).
				u, _ := strconv.ParseUint(p.s[p.i-4:p.i], 16, 32)
				if u >= 0xd800 && u < 0xdc00 {
					p.i += 2
					p.hex4()
				}
			}
		default:
			p.fail("a bad escape")
		}
	}
	if p.i >= len(p.s) {
		p.fail("a string that doesn't end")
	} else {
		p.i++
	}
	return b.String()
}

// number reads a number as the runtime does: the run of characters a
// number may have, read as strtod reads its longest prefix that is one.
func (p *jsonCheck) number() (f float64, integral bool, ok bool) {
	p.ws()
	st := p.i
	if p.i < len(p.s) && p.s[p.i] == '-' {
		p.i++
	}
	for p.i < len(p.s) && strings.IndexByte("0123456789.eE+-", p.s[p.i]) >= 0 {
		p.i++
	}
	if p.i == st {
		p.fail("expected a number")
		return 0, false, false
	}
	run := p.s[st:p.i]
	integral = !strings.ContainsAny(run, ".eE")
	for n := len(run); n > 0; n-- {
		if v, err := strconv.ParseFloat(run[:n], 64); err == nil || errorsIsRange(err) {
			return v, integral, true
		}
	}
	return 0, integral, true
}

func errorsIsRange(err error) bool {
	ne, ok := err.(*strconv.NumError)
	return ok && ne.Err == strconv.ErrRange
}

func (p *jsonCheck) skip() bool {
	p.ws()
	if p.i >= len(p.s) {
		p.fail("expected a value")
		return false
	}
	c := p.s[p.i]
	if c == '"' {
		p.str()
		return !p.failed()
	}
	if c == '{' || c == '[' {
		closing := byte(']')
		if c == '{' {
			closing = '}'
		}
		p.i++
		p.ws()
		if p.i < len(p.s) && p.s[p.i] == closing {
			p.i++
			return true
		}
		for {
			if c == '{' {
				p.ws()
				p.str()
				p.ws()
				if !p.lit(":") {
					p.fail("expected ':'")
					return false
				}
			}
			if !p.skip() {
				return false
			}
			p.ws()
			if p.lit(",") {
				continue
			}
			if p.i < len(p.s) && p.s[p.i] == closing {
				p.i++
				return true
			}
			p.fail("expected ',' or a closing bracket")
			return false
		}
	}
	if p.lit("true") || p.lit("false") || p.lit("null") {
		return true
	}
	st := p.i
	for p.i < len(p.s) && strings.IndexByte("+-0123456789.eE", p.s[p.i]) >= 0 {
		p.i++
	}
	if p.i == st {
		p.fail("unexpected '%c'", p.s[st])
		return false
	}
	return true
}

// emptyOK reports whether a struct's field of type t may be missing (the
// runtime makes it empty), as json.c's object_struct decides.
func emptyOK(t Type) bool {
	switch x := t.(type) {
	case *Optional, *Array, *Map, *Set:
		return true
	case *Basic:
		return x != Bytes // a scalar is zero, a string ""; bytes are required
	case *Named:
		return x.Obj.Kind == EnumType && !x.Obj.RefEnum() // a case's index, 0
	}
	return false
}

func (p *jsonCheck) value(t Type) {
	p.ws()
	if p.i >= len(p.s) {
		p.fail("expected a value")
		return
	}
	switch x := t.(type) {
	case *Optional:
		if p.lit("null") {
			return
		}
		p.value(x.Elem)
		return
	case *Basic:
		switch x {
		case Int, Duration, Color, Length, Pointer:
			f, integral, ok := p.number()
			if ok && !integral && f != math.Floor(f) {
				p.fail("expected an integer")
			}
		case Float:
			p.number()
		case Bool:
			if !p.lit("true") && !p.lit("false") {
				p.fail("expected true or false")
			}
		case String, Bytes:
			p.str()
		default:
			p.fail("a bad type: %s", x.name)
		}
		return
	case *Array:
		p.elems(x.Elem)
		return
	case *Set:
		p.elems(x.Elem)
		return
	case *Map:
		if p.lit("null") { // an empty map, as Go's encoding/json writes a nil one
			return
		}
		if !p.lit("{") {
			p.fail("expected an object")
			return
		}
		p.ws()
		if p.lit("}") {
			return
		}
		for {
			p.ws()
			p.str()
			p.ws()
			if p.failed() || !p.lit(":") {
				p.fail("expected ':'")
				return
			}
			p.value(x.Elem)
			if p.failed() {
				return
			}
			p.ws()
			if p.lit(",") {
				continue
			}
			if p.lit("}") {
				return
			}
			p.fail("expected ',' or '}'")
			return
		}
	case *Named:
		s := x.Subst()
		switch {
		case x.Obj.Kind == StructType && !x.Obj.IsError:
			p.object(x, s)
			return
		case x.Obj.Kind == EnumType && !x.Obj.RefEnum():
			name := p.str()
			if p.failed() {
				return
			}
			for _, k := range x.Obj.Cases {
				if k.Name == name {
					return
				}
			}
			p.fail("not a case: %s", name)
			return
		case x.Obj.Kind == EnumType:
			p.payloadEnum(x, s)
			return
		}
	}
	p.fail("a bad type: %s", typeString(t))
}

func (p *jsonCheck) elems(elem Type) {
	if p.lit("null") { // none: an empty array or set, as Go writes a nil slice
		return
	}
	if !p.lit("[") {
		p.fail("expected an array")
		return
	}
	p.ws()
	if p.lit("]") {
		return
	}
	for {
		p.value(elem)
		if p.failed() {
			return
		}
		p.ws()
		if p.lit(",") {
			continue
		}
		if p.lit("]") {
			return
		}
		p.fail("expected ',' or ']'")
		return
	}
}

func (p *jsonCheck) object(x *Named, s Subst) {
	p.ws()
	if !p.lit("{") {
		p.fail("expected an object for %s", x.Obj.Name())
		return
	}
	set := map[int]bool{}
	p.ws()
	if !p.lit("}") {
		for {
			p.ws()
			k := p.str()
			if p.failed() {
				return
			}
			p.ws()
			if !p.lit(":") {
				p.fail("expected ':'")
				return
			}
			idx := -1
			for i, f := range x.Obj.Fields {
				if f.Name == k {
					idx = i
				}
			}
			if idx < 0 {
				if !p.skip() {
					return
				}
			} else {
				p.value(s.Apply(x.Obj.Fields[idx].Type))
				if p.failed() {
					return
				}
				set[idx] = true
			}
			p.ws()
			if p.lit(",") {
				continue
			}
			if p.lit("}") {
				break
			}
			p.fail("expected ',' or '}'")
			return
		}
	}
	for i, f := range x.Obj.Fields {
		if !set[i] && !emptyOK(s.Apply(f.Type)) {
			p.fail("a missing field %s", f.Name)
			return
		}
	}
}

func (p *jsonCheck) payloadEnum(x *Named, s Subst) {
	if p.i < len(p.s) && p.s[p.i] == '"' {
		name := p.str()
		for _, k := range x.Obj.Cases {
			if k.Name == name && len(k.Params) == 0 {
				return
			}
		}
		p.fail("not a case without a payload: %s", name)
		return
	}
	if !p.lit("{") {
		p.fail("expected a case of %s", x.Obj.Name())
		return
	}
	p.ws()
	name := p.str()
	p.ws()
	if p.failed() || !p.lit(":") {
		p.fail("expected ':'")
		return
	}
	var k *Case
	for _, c := range x.Obj.Cases {
		if c.Name == name {
			k = c
		}
	}
	if k == nil {
		p.fail("not a case: %s", name)
		return
	}
	p.ws()
	switch {
	case p.lit("["):
		for i, pt := range k.Params {
			if i > 0 {
				p.ws()
				if !p.lit(",") {
					p.fail("expected ','")
				}
			}
			p.value(s.Apply(pt))
			if p.failed() {
				return
			}
		}
		p.ws()
		if !p.lit("]") {
			p.fail("expected ']'")
			return
		}
	case p.lit("{"):
		for {
			p.ws()
			if p.lit("}") {
				break
			}
			key := p.str()
			p.ws()
			if p.failed() || !p.lit(":") {
				p.fail("expected ':'")
				return
			}
			fi := -1
			for i, l := range k.Labels {
				if l != "" && l == key {
					fi = i
				}
			}
			if fi < 0 {
				p.skip()
			} else {
				p.value(s.Apply(k.Params[fi]))
			}
			if p.failed() {
				return
			}
			p.ws()
			p.lit(",")
		}
	case len(k.Params) == 1:
		p.value(s.Apply(k.Params[0]))
		if p.failed() {
			return
		}
	default:
		p.fail("expected the case's payload")
		return
	}
	p.ws()
	if !p.lit("}") {
		p.fail("expected '}'")
	}
}
