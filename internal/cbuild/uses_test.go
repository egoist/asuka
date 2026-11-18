package cbuild

import (
	"os"
	"path/filepath"
	"regexp"
	"slices"
	"strings"
	"testing"
)

// An app's builtins name the std packages' tables its link registers:
// one macro per name before the dot, sorted; none given, every table.
func TestUsesDefines(t *testing.T) {
	got := usesDefines([]string{"fs.join", "os.args", "fs.read", "compress.encode", "os.watchSignals", "time-zone.x", "nodot"})
	want := []string{"-DAK_USES_COMPRESS=1", "-DAK_USES_FS=1", "-DAK_USES_OS=1", "-DAK_USES_TIME_ZONE=1"}
	if !slices.Equal(got, want) {
		t.Errorf("got %v, want %v", got, want)
	}
	if got := usesDefines(nil); !slices.Equal(got, []string{"-DAK_USES_ALL=1"}) {
		t.Errorf("nil: %v", got)
	}
	if got := usesDefines([]string{}); len(got) != 0 {
		t.Errorf("none: %v", got)
	}
}

// core/link/link.c registers every table std/std.h declares, each under
// the macro of its builtins' prefix: a table it leaves out is one no app
// can call, one under another's macro one an app that calls it lacks.
func TestLinkRegistersEveryTable(t *testing.T) {
	core, err := CoreDir()
	if err != nil {
		t.Skip(err)
	}
	read := func(p string) string {
		b, err := os.ReadFile(filepath.Join(core, p))
		if err != nil {
			t.Fatal(err)
		}
		return string(b)
	}
	declared := regexp.MustCompile(`extern const ak_lib_entry (ak_std_\w+)\[\];`).FindAllStringSubmatch(read("src/std/std.h"), -1)
	link := read("link/link.c")
	registered := map[string]string{}
	for _, m := range regexp.MustCompile(`defined\(AK_USES_(\w+)\)\n\t(ak_std_\w+),`).FindAllStringSubmatch(link, -1) {
		registered[m[2]] = m[1]
	}
	// Each table's builtins, by their names' prefixes, from its C.
	srcs, _ := filepath.Glob(filepath.Join(core, "src", "std", "*.c"))
	prefixes := map[string][]string{}
	for _, s := range srcs {
		b, _ := os.ReadFile(s)
		for _, m := range regexp.MustCompile(`(?s)const ak_lib_entry (ak_std_\w+)\[\] = \{(.*?)\{NULL, NULL\}`).FindAllStringSubmatch(string(b), -1) {
			for _, e := range regexp.MustCompile(`\{"(\w+)\.`).FindAllStringSubmatch(m[2], -1) {
				if !slices.Contains(prefixes[m[1]], e[1]) {
					prefixes[m[1]] = append(prefixes[m[1]], e[1])
				}
			}
		}
	}
	if len(declared) < 10 {
		t.Fatalf("std.h declares %d tables", len(declared))
	}
	for _, d := range declared {
		name := d[1]
		macro, ok := registered[name]
		if !ok {
			t.Errorf("link.c doesn't register %s", name)
			continue
		}
		ps := prefixes[name]
		if len(ps) != 1 || strings.ToUpper(ps[0]) != macro {
			t.Errorf("%s's builtins are %v, registered under AK_USES_%s", name, ps, macro)
		}
	}
}
