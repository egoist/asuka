//go:build ignore

// gen.go writes cases.txt, the official TOML test suite's TOML 1.0
// cases (github.com/toml-lang/toml-test, MIT: LICENSE) as one file that
// core/test/toml_test.c reads:
//
//	go run core/test/testdata/toml/gen.go <toml-test's tests directory>
//
// from the repository's root.
//
// Each case is a line "valid <name> <toml bytes> <json bytes>" or
// "invalid <name> <toml bytes>", then those bytes, then a newline. A
// valid case's JSON is the suite's, written canonically as the C test
// writes what it parsed: object keys sorted, each scalar
// {"type":…,"value":…} with its value normalized (an integer in
// decimal; a float as its IEEE 754 bits in decimal, any NaN "nan"; a
// date-time with T, Z and its fraction's trailing zeros dropped).
package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io/fs"
	"math"
	"os"
	"path"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
)

// TOML 1.0's cases: the suite's 1.1 cases aside, and the 1.0 ones 1.1
// changed (toml-test's own version table).
var exclude = []string{
	"valid/spec-1.1.0/*",
	"invalid/spec-1.1.0/*",
	"valid/string/escape-esc",
	"valid/string/hex-escape",
	"invalid/string/bad-hex-esc",
	"valid/datetime/no-seconds",
	"valid/inline-table/newline",
	"valid/inline-table/newline-comment",
}

func excluded(name string) bool {
	for _, e := range exclude {
		if ok, _ := path.Match(e, name); ok || e == name {
			return true
		}
	}
	return false
}

func main() {
	if len(os.Args) != 2 {
		fmt.Fprintln(os.Stderr, "usage: go run gen.go <toml-test/tests>")
		os.Exit(2)
	}
	root := os.Args[1]
	var names []string
	filepath.WalkDir(root, func(p string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if !d.IsDir() && strings.HasSuffix(p, ".toml") {
			rel, _ := filepath.Rel(root, p)
			rel = filepath.ToSlash(strings.TrimSuffix(rel, ".toml"))
			if !excluded(rel) && (strings.HasPrefix(rel, "valid/") || strings.HasPrefix(rel, "invalid/")) {
				names = append(names, rel)
			}
		}
		return nil
	})
	sort.Strings(names)
	var out bytes.Buffer
	for _, n := range names {
		src, err := os.ReadFile(filepath.Join(root, n+".toml"))
		if err != nil {
			panic(err)
		}
		if strings.HasPrefix(n, "invalid/") {
			fmt.Fprintf(&out, "invalid %s %d\n", n, len(src))
			out.Write(src)
			out.WriteByte('\n')
			continue
		}
		j, err := os.ReadFile(filepath.Join(root, n+".json"))
		if err != nil {
			panic(err)
		}
		d := json.NewDecoder(bytes.NewReader(j))
		d.UseNumber()
		var v any
		if err := d.Decode(&v); err != nil {
			panic(fmt.Sprint(n, ": ", err))
		}
		var c bytes.Buffer
		canon(&c, v)
		fmt.Fprintf(&out, "valid %s %d %d\n", n, len(src), c.Len())
		out.Write(src)
		out.Write(c.Bytes())
		out.WriteByte('\n')
	}
	// Run from the repository's root.
	if err := os.WriteFile(filepath.Join("core", "test", "testdata", "toml", "cases.txt"), out.Bytes(), 0o644); err != nil {
		panic(err)
	}
}

func str(b *bytes.Buffer, s string) {
	b.WriteByte('"')
	for i := 0; i < len(s); i++ {
		c := s[i]
		switch {
		case c == '"' || c == '\\':
			b.WriteByte('\\')
			b.WriteByte(c)
		case c < 0x20 || c == 0x7f:
			fmt.Fprintf(b, "\\u%04x", c)
		default:
			b.WriteByte(c)
		}
	}
	b.WriteByte('"')
}

func canon(b *bytes.Buffer, v any) {
	switch v := v.(type) {
	case []any:
		b.WriteByte('[')
		for i, e := range v {
			if i > 0 {
				b.WriteByte(',')
			}
			canon(b, e)
		}
		b.WriteByte(']')
	case map[string]any:
		if t, ok := v["type"].(string); ok && len(v) == 2 {
			if s, ok := v["value"].(string); ok {
				b.WriteString(`{"type":`)
				str(b, t)
				b.WriteString(`,"value":`)
				str(b, scalar(t, s))
				b.WriteByte('}')
				return
			}
		}
		keys := make([]string, 0, len(v))
		for k := range v {
			keys = append(keys, k)
		}
		sort.Strings(keys)
		b.WriteByte('{')
		for i, k := range keys {
			if i > 0 {
				b.WriteByte(',')
			}
			str(b, k)
			b.WriteByte(':')
			canon(b, v[k])
		}
		b.WriteByte('}')
	default:
		panic(fmt.Sprintf("not toml-test's JSON: %v", v))
	}
}

func scalar(t, s string) string {
	switch t {
	case "integer":
		n, err := strconv.ParseInt(s, 10, 64)
		if err != nil {
			panic(err)
		}
		return strconv.FormatInt(n, 10)
	case "float":
		switch strings.TrimPrefix(s, "+") {
		case "nan", "-nan":
			return "nan"
		case "inf":
			s = "+Inf"
		case "-inf":
			s = "-Inf"
		}
		f, err := strconv.ParseFloat(s, 64)
		if err != nil {
			panic(err)
		}
		if math.IsNaN(f) {
			return "nan"
		}
		return strconv.FormatUint(math.Float64bits(f), 10)
	case "datetime", "datetime-local", "date-local", "time-local":
		return datetime(s)
	}
	return s
}

// datetime is a date-time's text normalized: T and Z upper case, a space
// between the date and the time a T, the fraction's trailing zeros
// dropped.
func datetime(s string) string {
	s = strings.ToUpper(s)
	if len(s) > 10 && s[10] == ' ' {
		s = s[:10] + "T" + s[11:]
	}
	if i := strings.IndexByte(s, '.'); i >= 0 {
		j := i + 1
		for j < len(s) && s[j] >= '0' && s[j] <= '9' {
			j++
		}
		frac := strings.TrimRight(s[i+1:j], "0")
		if frac == "" {
			s = s[:i] + s[j:]
		} else {
			s = s[:i+1] + frac + s[j:]
		}
	}
	return s
}
