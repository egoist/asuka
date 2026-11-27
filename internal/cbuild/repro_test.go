package cbuild

import (
	"bytes"
	"os"
	"path/filepath"
	"runtime"
	"sort"
	"testing"
	"time"
)

// Builds are reproducible (docs/shipping.md): the core built in one
// checkout with one cache, then in another checkout with another cache a
// while later, is the same bytes, for this computer (its own compiler)
// and for targets Zig cross-compiles to. The paths each compile ran with
// are mapped to fixed names (prefixMaps), archives have no dates (D), a
// link's output is named as it will be and a Windows executable is
// stamped with its contents' hash (reproLink).
func TestReproducible(t *testing.T) {
	if testing.Short() {
		t.Skip("builds the core four times over")
	}
	other := "amd64"
	if runtime.GOARCH == "amd64" {
		other = "arm64"
	}
	hostLib := map[string]string{"darwin": "libasuka.dylib", "windows": "asuka.dll"}[runtime.GOOS]
	if hostLib == "" {
		hostLib = "libasuka.so"
	}
	builds := []struct {
		target Target
		kind   LibKind
		name   string
		zig    bool
	}{
		{Host(), Shared, hostLib, false},
		{Host(), Static, "libasuka.a", false},
		{Target{"linux", other}, Shared, "libasuka.so", true},
		{Target{"windows", "amd64"}, Shared, "asuka.dll", true},
	}
	rootsOnce.Do(func() {})
	saved := roots
	defer func() { roots = saved }()
	src := os.DirFS(coreRoot(t))
	var first [][]byte
	for round := 0; round < 2; round++ {
		repo := filepath.Join(t.TempDir(), "checkout"+string(rune('a'+round)))
		core := filepath.Join(repo, "core")
		if err := os.CopyFS(core, src); err != nil {
			t.Fatal(err)
		}
		cache := freshCache(t)
		roots = []pathRoot{{repo, "$REPO", ""}, {cache, "$CACHE", "asuka-cache/"}}
		sort.SliceStable(roots, func(i, j int) bool { return len(roots[i].dir) > len(roots[j].dir) })
		if round == 1 {
			time.Sleep(1100 * time.Millisecond) // archives and links a second later
		}
		var got [][]byte
		for _, b := range builds {
			tc, err := ForTarget(b.target)
			if err != nil || b.zig != tc.CC.IsZig() {
				got = append(got, nil) // no Zig, or Zig is this computer's compiler: another build covers it
				continue
			}
			out := filepath.Join(t.TempDir(), b.name)
			if err := tc.Lib(core, b.kind, nil, false, out); err != nil {
				t.Fatalf("%s: %v", b.target, err)
			}
			data, err := os.ReadFile(out)
			if err != nil {
				t.Fatal(err)
			}
			for _, p := range []string{repo, cache} {
				if bytes.Contains(data, []byte(p)) {
					t.Errorf("%s's %s names %s", b.target, b.name, p)
				}
			}
			got = append(got, data)
		}
		if round == 0 {
			first = got
			continue
		}
		for i, b := range builds {
			if first[i] == nil || got[i] == nil {
				continue
			}
			if !bytes.Equal(first[i], got[i]) {
				t.Errorf("%s's %s: two builds differ (%d and %d bytes; %d differ)", b.target, b.name, len(first[i]), len(got[i]), differing(first[i], got[i]))
			}
		}
	}
}

// differing is how many bytes a and b differ in, the longer's extra
// counted.
func differing(a, b []byte) int {
	n := max(len(a), len(b)) - min(len(a), len(b))
	for i := range min(len(a), len(b)) {
		if a[i] != b[i] {
			n++
		}
	}
	return n
}
