package i18n

import (
	"bytes"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"testing/fstest"
	"time"
)

// dateSets are the dates the date cases write, in UTC (the core's test
// runs with its zone UTC): an afternoon, a year's last second, noon, the
// epoch, a date before it.
func dateSets(string) [][]struct {
	c string
	v any
} {
	var out [][]struct {
		c string
		v any
	}
	for _, t := range []time.Time{
		time.Date(2026, 1, 5, 13, 7, 9, 0, time.UTC),
		time.Date(2025, 12, 31, 23, 59, 59, 0, time.UTC),
		time.Date(2026, 6, 15, 12, 0, 0, 0, time.UTC),
		time.Date(2026, 3, 1, 0, 30, 0, 0, time.UTC),
		time.Unix(0, 0).UTC(),
		time.Date(1969, 7, 20, 20, 17, 40, 0, time.UTC),
	} {
		out = append(out, []struct {
			c string
			v any
		}{{"i", t}})
	}
	return out
}

// The C core says messages from compiled catalogs (core/src/ui/i18n.c).
// TestCompileCases compiles a catalog of every kind of message into
// core/test/testdata/catalog.akc and writes what Format says for many
// locales and arguments into catalog_cases.txt, which the core's test
// says the same way: the two must agree, case by case. -update-c writes
// them.
func TestCompileCases(t *testing.T) {
	c, err := NewCatalog(fstest.MapFS{
		"en.json": {Data: []byte(`{
			"hello": "Hello",
			"files": "{n, plural, =0 {No files} one {# file} other {# files}}",
			"place": "{n, selectordinal, one {#st} two {#nd} few {#rd} other {#th}}",
			"guests": "{n, plural, offset:1 =0 {Nobody} =1 {{host}} one {{host} and # other} other {{host} and # others}}",
			"replied": "{gender, select, female {She} male {He} other {They}} replied.",
			"price": "{p, number} | {r, number, percent} | {q, number, ::.00} | {b, number, integer} | {g, number, ::group-off}",
			"quote": "'{'{name}'}' don't",
			"seen": "Last seen {when, date, medium} at {when, time, short}",
			"day": "{d, date, ::yMMMd} | {d, date, full} | {d, time, long} | {d, date, short} | {d, time} | {d, date, ::MMMMEEEEd} | {d, time, ::jm}",
			"stamp": "{t}",
			"asuka.ok": "Okay",
			"cost": "{p, number, ::currency/EUR} | {q, number, ::currency/JPY} | {r, number, ::currency/CHF} | {u, number, ::currency/USD} | {k, number, ::currency/KWD} | {x, number, ::currency/XYZ}"
		}`)},
		"de.json":      {Data: []byte(`{"hello": "Hallo", "files": "{n, plural, =0 {Keine Dateien} one {# Datei} other {# Dateien}}", "price": "{p, number} | {r, number, percent} | {q, number, ::.00} | {b, number, integer} | {g, number, ::group-off}", "cost": "{p, number, ::currency/EUR} | {q, number, ::currency/JPY} | {r, number, ::currency/CHF} | {u, number, ::currency/USD} | {k, number, ::currency/KWD} | {x, number, ::currency/XYZ}"}`)},
		"ar.json":      {Data: []byte(`{"hello": "مرحبا", "files": "{n, plural, zero {لا ملفات} one {ملف} two {ملفان} few {# ملفات} many {# ملفًا} other {# ملف}}", "replied": "{gender, select, female {ردت} other {رد}} Ali", "cost": "{p, number, ::currency/EUR} | {q, number, ::currency/JPY} | {r, number, ::currency/CHF} | {u, number, ::currency/USD} | {k, number, ::currency/KWD} | {x, number, ::currency/XYZ}"}`)},
		"ru.json":      {Data: []byte(`{"files": "{n, plural, one {# файл} few {# файла} many {# файлов} other {# файла}}"}`)},
		"fr.json":      {Data: []byte(`{"price": "{p, number} | {r, number, percent} | {q, number, ::.00} | {b, number, integer} | {g, number, ::group-off}", "cost": "{p, number, ::currency/EUR} | {q, number, ::currency/JPY} | {r, number, ::currency/CHF} | {u, number, ::currency/USD} | {k, number, ::currency/KWD} | {x, number, ::currency/XYZ}"}`)},
		"hi.json":      {Data: []byte(`{"price": "{p, number} | {r, number, percent} | {q, number, ::.00} | {b, number, integer} | {g, number, ::group-off}", "cost": "{p, number, ::currency/EUR} | {q, number, ::currency/JPY} | {r, number, ::currency/CHF} | {u, number, ::currency/USD} | {k, number, ::currency/KWD} | {x, number, ::currency/XYZ}"}`)},
		"fa.json":      {Data: []byte(`{"price": "{p, number} | {r, number, percent} | {q, number, ::.00} | {b, number, integer} | {g, number, ::group-off}", "cost": "{p, number, ::currency/EUR} | {q, number, ::currency/JPY} | {r, number, ::currency/CHF} | {u, number, ::currency/USD} | {k, number, ::currency/KWD} | {x, number, ::currency/XYZ}"}`)},
		"zh-Hant.json": {Data: []byte(`{"hello": "哈囉", "day": "{d, date, ::yMMMd} | {d, time, short} | {d, date, full}"}`)},
		"ko.json":      {Data: []byte(`{"seen": "마지막 접속 {when, date, medium} {when, time, short}"}`)},
		"en-GB.json":   {Data: []byte(`{"hello": "Hello, mate"}`)},
	}, "en")
	if err != nil {
		t.Fatal(err)
	}
	comp, err := c.CompileWith(CompileOptions{DateArgs: map[string][]string{"stamp": {"t"}}})
	if err != nil {
		t.Fatal(err)
	}
	type value = struct {
		c string // i, f or s (a date: i, its nanoseconds)
		v any
	}
	sets := map[string][][]value{
		"hello":   {{}},
		"files":   {{{"i", 0}}, {{"i", 1}}, {{"i", 2}}, {{"i", 3}}, {{"i", 5}}, {{"i", 11}}, {{"i", 21}}, {{"i", 100}}, {{"i", 1234567}}, {{"f", 1.5}}, {{"f", 2.0}}},
		"place":   {{{"i", 1}}, {{"i", 2}}, {{"i", 3}}, {{"i", 4}}, {{"i", 11}}, {{"i", 12}}, {{"i", 13}}, {{"i", 21}}, {{"i", 22}}, {{"i", 23}}, {{"i", 101}}},
		"guests":  {{{"i", 0}, {"s", "Ada"}}, {{"i", 1}, {"s", "Ada"}}, {{"i", 2}, {"s", "Ada"}}, {{"i", 3}, {"s", "Ada"}}},
		"replied": {{{"s", "female"}}, {{"s", "male"}}, {{"s", "x"}}},
		"price": {
			{{"f", 1234.5}, {"f", 0.256}, {"f", 3.14159}, {"f", 2.5}, {"i", 12345}},
			{{"i", -1234567}, {"f", 1.0}, {"i", 7}, {"f", 3.5}, {"i", 1000}},
			{{"f", 0.0005}, {"f", 0.125}, {"f", 1.005}, {"f", -2.5}, {"i", 0}},
		},
		"quote":    {{{"s", "x"}}},
		"seen":     dateSets("when"),
		"day":      dateSets("d"),
		"stamp":    dateSets("t"),
		"asuka.ok": {{}},
		"cost": {
			{{"f", 1234.5}, {"f", 1234.5}, {"i", 12}, {"f", -0.125}, {"f", 1.2345}, {"i", 5}},
			{{"i", -1234567}, {"i", 0}, {"f", -12.5}, {"i", 1000000}, {"i", -3}, {"f", 0.5}},
		},
	}
	locales := []string{"en", "de", "ar", "ru", "fr", "hi", "fa", "zh-Hant", "en-GB", "en-XA", "ar-XB", "zh-TW", "en-AU", "de-AT", "ja",
		"en-u-hc-h23", "de-u-hc-h12", "zh-Hant-u-hc-h23", "ko-u-hc-h23", "ar-u-hc-h23", "ko"}
	numbers := map[string]bool{"files": true, "place": true, "guests": true, "price": true, "seen": true, "day": true, "stamp": true, "cost": true}
	var b bytes.Buffer
	b.WriteString("# Code generated by go test ./i18n -run CompileCases -update-c. DO NOT EDIT.\n")
	b.WriteString("# locale\tkey\targuments (kind:value, by the key's Args)\twhat Format says\n")
	for _, loc := range locales {
		for i, key := range comp.Keys {
			// Numbers are written in the locale's conventions: the core
			// has the catalog's languages', and the pseudo locales'.
			inCatalog := false
			for _, l := range c.Languages() {
				inCatalog = inCatalog || l == stripExtensions(loc)
			}
			if numbers[key] && !inCatalog && !IsPseudo(loc) {
				continue
			}
			for _, set := range sets[key] {
				args := map[string]any{}
				var enc []string
				for j, a := range comp.Args[i] {
					if j < len(set) {
						args[a.Name] = set[j].v
						v := set[j].v
						if t, ok := v.(time.Time); ok {
							v = t.UnixNano() // the core takes a date as its nanoseconds
						}
						enc = append(enc, fmt.Sprintf("%s:%v", set[j].c, v))
					}
				}
				got := c.Format(loc, key, args)
				if strings.ContainsAny(got, "\t\n") {
					t.Fatalf("%s %s: %q", loc, key, got)
				}
				fmt.Fprintf(&b, "%s\t%s\t%s\t%s\n", loc, key, strings.Join(enc, ","), got)
			}
		}
	}
	// Relative times, in the catalog's languages: what FormatRelative
	// says of each time seen from now, in each style, numeric or not.
	var rel bytes.Buffer
	rel.WriteString("# Code generated by go test ./i18n -run CompileCases -update-c. DO NOT EDIT.\n")
	rel.WriteString("# locale\ttime (ns)\tnow (ns)\tstyle (0 long, 1 short, 2 narrow)\tnumeric (0, 1)\twhat FormatRelative says\n")
	now := time.Date(2026, 3, 15, 12, 0, 0, 0, time.UTC)
	var times []time.Time
	for _, d := range []time.Duration{0, 400 * time.Millisecond, -time.Second, -45 * time.Second, 90 * time.Second, -time.Hour, -2 * time.Hour,
		-3 * time.Hour, 5 * time.Hour, -11 * time.Hour, -21 * time.Hour, -24 * time.Hour, 24 * time.Hour, -48 * time.Hour, 48 * time.Hour,
		-5 * 24 * time.Hour, -7 * 24 * time.Hour, 14 * 24 * time.Hour, -22 * 24 * time.Hour} {
		times = append(times, now.Add(d))
	}
	times = append(times,
		time.Date(2026, 3, 14, 23, 0, 0, 0, time.UTC), time.Date(2026, 2, 15, 12, 0, 0, 0, time.UTC), time.Date(2026, 4, 20, 0, 0, 0, 0, time.UTC),
		time.Date(2025, 11, 1, 0, 0, 0, 0, time.UTC), time.Date(2025, 3, 15, 12, 0, 0, 0, time.UTC), time.Date(2028, 6, 1, 0, 0, 0, 0, time.UTC),
		time.Date(2005, 1, 1, 0, 0, 0, 0, time.UTC), time.Date(1969, 7, 20, 20, 17, 40, 0, time.UTC))
	for _, loc := range c.Languages() {
		for _, tm := range times {
			for style := RelativeLong; style <= RelativeNarrow; style++ {
				for _, numeric := range []bool{false, true} {
					got := FormatRelative(loc, tm, now, style, numeric)
					n := 0
					if numeric {
						n = 1
					}
					fmt.Fprintf(&rel, "%s\t%d\t%d\t%d\t%d\t%s\n", loc, tm.UnixNano(), now.UnixNano(), style, n, got)
				}
			}
		}
	}
	dir := filepath.Join("..", "core", "test", "testdata")
	if *updateC {
		if err := os.WriteFile(filepath.Join(dir, "catalog.akc"), comp.Blob, 0o644); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(dir, "catalog_cases.txt"), b.Bytes(), 0o644); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(dir, "relative_cases.txt"), rel.Bytes(), 0o644); err != nil {
			t.Fatal(err)
		}
		return
	}
	blob, err1 := os.ReadFile(filepath.Join(dir, "catalog.akc"))
	cases, err2 := os.ReadFile(filepath.Join(dir, "catalog_cases.txt"))
	relCases, err3 := os.ReadFile(filepath.Join(dir, "relative_cases.txt"))
	if err1 != nil || err2 != nil || err3 != nil || !bytes.Equal(blob, comp.Blob) || !bytes.Equal(cases, b.Bytes()) || !bytes.Equal(relCases, rel.Bytes()) {
		t.Fatal("core/test/testdata's catalog is stale: go test ./i18n -run CompileCases -update-c")
	}
}
