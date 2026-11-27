package cbuild

import (
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"

	"github.com/egoist/asuka/internal/ccache"
)

func writeFile(t *testing.T, p, s string) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(p, []byte(s), 0o644); err != nil {
		t.Fatal(err)
	}
}

// freshCache points the build cache at a new directory, its counts
// zero.
func freshCache(t *testing.T) string {
	d := t.TempDir()
	t.Setenv("ASUKA_CACHE", d)
	// What an earlier test counted, left out: never flushed into (and
	// trimming) the user's own cache.
	ccache.Flush()
	ccache.ResetCounts()
	return d
}

func counts() (hits, misses int64) {
	ccache.Flush()
	c := ccache.ReadCounts()
	return c.Hits["c"], c.Misses["c"]
}

func TestObjectCacheHeaders(t *testing.T) {
	freshCache(t)
	cc, err := Find()
	if err != nil {
		t.Skip(err)
	}
	dir := t.TempDir()
	src := filepath.Join(dir, "a.c")
	hdr := filepath.Join(dir, "inc", "a.h")
	writeFile(t, src, "#include \"a.h\"\nint f(void) { return A; }\n")
	writeFile(t, hdr, "#define A 1\n")
	flags := []string{"-I" + filepath.Dir(hdr), "-O1"}

	first, err := cc.CompileCached([]string{src}, flags)
	if err != nil {
		t.Fatal(err)
	}
	if h, m := counts(); h != 0 || m != 1 {
		t.Fatalf("first compile: %d hits, %d misses", h, m)
	}
	again, err := cc.CompileCached([]string{src}, flags)
	if err != nil {
		t.Fatal(err)
	}
	if again[0] != first[0] {
		t.Fatalf("the same source and header compiled to %v, then %v", first, again)
	}
	if h, m := counts(); h != 1 || m != 1 {
		t.Fatalf("second compile: %d hits, %d misses", h, m)
	}

	// The header changes: another object, though the source didn't.
	writeFile(t, hdr, "#define A 2\n")
	changed, err := cc.CompileCached([]string{src}, flags)
	if err != nil {
		t.Fatal(err)
	}
	if changed[0].Key == first[0].Key {
		t.Fatal("a header changed and the object's key didn't")
	}
	if h, m := counts(); h != 1 || m != 2 {
		t.Fatalf("after the header changed: %d hits, %d misses", h, m)
	}
	// And back: the first object again, from the manifest.
	writeFile(t, hdr, "#define A 1\n")
	back, err := cc.CompileCached([]string{src}, flags)
	if err != nil {
		t.Fatal(err)
	}
	if back[0] != first[0] {
		t.Fatal("the header's first version didn't find its object")
	}
	// A flag changes: another object.
	opt, err := cc.CompileCached([]string{src}, []string{"-I" + filepath.Dir(hdr), "-O2"})
	if err != nil {
		t.Fatal(err)
	}
	if opt[0].Key == first[0].Key {
		t.Fatal("a flag changed and the object's key didn't")
	}
}

// Two checkouts (worktrees) of the same sources share objects: paths
// under a checkout are relative to it in keys and manifests, so the
// second compiles nothing; and a header that differs between them still
// compiles again.
func TestObjectCacheSharedAcrossCheckouts(t *testing.T) {
	cache := freshCache(t)
	cc, err := Find()
	if err != nil {
		t.Skip(err)
	}
	rootsOnce.Do(func() {})
	saved := roots
	defer func() { roots = saved }()
	compile := func(repo string) Object {
		roots = []pathRoot{{repo, "$REPO", ""}, {cache, "$CACHE", "asuka-cache/"}}
		d := filepath.Join(repo, "core", "src")
		o, err := cc.CompileCached([]string{filepath.Join(d, "x.c")}, []string{"-I" + filepath.Join(repo, "core", "include")})
		if err != nil {
			t.Fatal(err)
		}
		return o[0]
	}
	var repos []string
	for i := 0; i < 3; i++ {
		r := t.TempDir()
		writeFile(t, filepath.Join(r, "core", "src", "x.c"), "#include \"x.h\"\nint x(void) { return X; }\n")
		writeFile(t, filepath.Join(r, "core", "include", "x.h"), "#define X 7\n")
		repos = append(repos, r)
	}
	writeFile(t, filepath.Join(repos[2], "core", "include", "x.h"), "#define X 8\n")
	a := compile(repos[0])
	b := compile(repos[1])
	if a != b {
		t.Fatalf("two checkouts' builds of the same sources made two objects: %v, %v", a, b)
	}
	if h, m := counts(); h != 1 || m != 1 {
		t.Fatalf("%d hits, %d misses; want the second checkout's a hit", h, m)
	}
	if c := compile(repos[2]); c.Key == a.Key {
		t.Fatal("a checkout whose header differs got the other's object")
	}
}

// The core's objects compiled for one app are every other app's: a
// second build with another app compiles only that app.
func TestCoreObjectsSharedAcrossApps(t *testing.T) {
	freshCache(t)
	tc, err := ForTarget(Host())
	if err != nil {
		t.Skip(err)
	}
	core := coreRoot(t)
	dir := t.TempDir()
	app := func(name, body string) string {
		p := filepath.Join(dir, name+".c")
		writeFile(t, p, "typedef struct ak_module ak_module;\nak_module *asuka_app_module(void);\nak_module *asuka_app_module(void) { "+body+" }\n")
		return p
	}
	if err := tc.Lib(core, Static, []string{app("a", "return 0;")}, false, filepath.Join(dir, "a.a")); err != nil {
		t.Fatal(err)
	}
	_, m1 := counts()
	if err := tc.Lib(core, Static, []string{app("b", "return (ak_module *)0;")}, false, filepath.Join(dir, "b.a")); err != nil {
		t.Fatal(err)
	}
	h, m2 := counts()
	if m2-m1 != 1 {
		t.Fatalf("the second app's build compiled %d sources; want 1 (its own)", m2-m1)
	}
	if h < 10 {
		t.Fatalf("the second app's build hit %d objects", h)
	}
}

func TestParseDepfile(t *testing.T) {
	for _, c := range []struct {
		in   string
		want []string
	}{
		{"a.o: a.c a.h \\\n  /usr/x/b.h\n", []string{"a.c", "a.h", "/usr/x/b.h"}},
		{"C:\\x\\a.o: C:\\x\\a.c C:\\x\\my\\ dir\\a.h\r\n", []string{`C:\x\a.c`, `C:\x\my dir\a.h`}},
		{"/tmp/o.12.tmp.o: /src/core/a.c \\\n /src/core/include/asuka.h\n", []string{"/src/core/a.c", "/src/core/include/asuka.h"}},
	} {
		if got := parseDepfile([]byte(c.in)); !reflect.DeepEqual(got, c.want) {
			t.Errorf("parseDepfile(%q) = %q; want %q", c.in, got, c.want)
		}
	}
}

func TestKeyPaths(t *testing.T) {
	root := t.TempDir()
	t.Setenv("ASUKA_CACHE", root)
	rootsOnce.Do(func() {}) // computed below for this test's cache
	saved := roots
	defer func() { roots = saved }()
	roots = []pathRoot{{filepath.Join("/", "w", "repo"), "$REPO", ""}, {root, "$CACHE", "asuka-cache/"}}
	p := filepath.Join("/", "w", "repo", "core", "src", "a.h")
	if k := keyPath("-I" + filepath.Dir(p)); k != "-I$REPO/core/src" {
		t.Errorf("keyPath = %q", k)
	}
	if k := keyPath(p); realPath(k) != p || !strings.HasPrefix(k, "$REPO/") {
		t.Errorf("keyPath(%s) = %s, back %s", p, k, realPath(k))
	}
	if k := keyPath(filepath.Join("/", "w", "repo2", "a.h")); strings.Contains(k, "$REPO") {
		t.Errorf("another directory that starts the same is the repository's: %s", k)
	}
	// A checkout at /src has /src/core/src: the root is where the path
	// starts, not every directory of its name.
	roots = []pathRoot{{filepath.Join("/", "src"), "$REPO", ""}, {root, "$CACHE", "asuka-cache/"}}
	p = filepath.Join("/", "src", "core", "src", "base", "base.h")
	if k := keyPath(p); k != "$REPO/core/src/base/base.h" || realPath(k) != p {
		t.Errorf("keyPath(%s) = %s, back %s", p, k, realPath(k))
	}
	if k := keyPath("-I" + filepath.Join("/", "src", "core", "src")); k != "-I$REPO/core/src" {
		t.Errorf("keyPath = %q", k)
	}
}
