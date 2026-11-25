package pkgs

import (
	"os"
	"path/filepath"
	"testing"
	"time"
)

func TestSetDependency(t *testing.T) {
	src := "# My app\nname = \"Notes\"\n\n[dependencies]\n# charts\n\"@acme/charts\" = \"^1.0.0\"\n\n[ios]\nteam = \"X\"\n"
	got := string(setDependency([]byte(src), "colors", "^0.3.0"))
	want := "# My app\nname = \"Notes\"\n\n[dependencies]\n# charts\n\"@acme/charts\" = \"^1.0.0\"\ncolors = \"^0.3.0\"\n\n[ios]\nteam = \"X\"\n"
	if got != want {
		t.Fatalf("add:\n%s\nwant:\n%s", got, want)
	}
	got = string(setDependency([]byte(got), "@acme/charts", "^1.2.0"))
	if want2 := "\"@acme/charts\" = \"^1.2.0\""; !contains(got, want2) {
		t.Fatalf("replace:\n%s", got)
	}
	got = string(setDependency([]byte(got), "@acme/charts", ""))
	if contains(got, "charts\" =") {
		t.Fatalf("remove:\n%s", got)
	}
	got = string(setDependency([]byte("name = \"x\"\n"), "a", "1.0.0"))
	if got != "name = \"x\"\n\n[dependencies]\na = \"1.0.0\"\n" {
		t.Fatalf("new table:\n%q", got)
	}
}

func contains(s, sub string) bool {
	for i := 0; i+len(sub) <= len(s); i++ {
		if s[i:i+len(sub)] == sub {
			return true
		}
	}
	return false
}

func TestLockRoundTrip(t *testing.T) {
	c := registry(t, world)
	l, err := Resolve(c, map[string]string{"a": "^1.0.0", "b2": "npm:b@^2.0.0"}, ".")
	if err != nil {
		t.Fatal(err)
	}
	dir := t.TempDir()
	if err := WriteLock(dir, l); err != nil {
		t.Fatal(err)
	}
	back, err := ReadLock(dir)
	if err != nil {
		t.Fatal(err)
	}
	if versions(back) != versions(l) || back.Root["b2"] != "b@2" || back.Find("a").Deps["b"] != "b" {
		t.Fatalf("round trip: %+v", back)
	}
	m := &Manifest{Dependencies: map[string]string{"a": "^1.0.0", "b2": "npm:b@^2.0.0"}}
	if !back.Current(m) {
		t.Fatal("the lock should answer the manifest")
	}
	m.Dependencies["a"] = "^1.1.0"
	if back.Current(m) {
		t.Fatal("a changed range needs resolving again")
	}
}

// A lock written again with the same contents is left as it is, its time
// too: asuka dev's watcher reads a new time as the app changing. A lock
// that changed is written.
func TestWriteLockUnchanged(t *testing.T) {
	c := registry(t, world)
	l, err := Resolve(c, map[string]string{"a": "^1.0.0"}, ".")
	if err != nil {
		t.Fatal(err)
	}
	dir := t.TempDir()
	if err := WriteLock(dir, l); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(dir, LockFile)
	past := time.Now().Add(-time.Hour).Truncate(time.Second)
	if err := os.Chtimes(path, past, past); err != nil {
		t.Fatal(err)
	}
	if err := WriteLock(dir, l); err != nil {
		t.Fatal(err)
	}
	fi, err := os.Stat(path)
	if err != nil {
		t.Fatal(err)
	}
	if !fi.ModTime().Equal(past) {
		t.Fatalf("an unchanged lock was written again (its time %v, was %v)", fi.ModTime(), past)
	}
	if l, err = Resolve(c, map[string]string{"a": "^1.1.0"}, "."); err != nil {
		t.Fatal(err)
	}
	if err := WriteLock(dir, l); err != nil {
		t.Fatal(err)
	}
	back, err := ReadLock(dir)
	if err != nil || versions(back) != "a=1.1.0 b=1.3.0" {
		t.Fatalf("a changed lock wasn't written: %+v, %v", back, err)
	}
}

// std/ and platform/ are the standard library's and the platform SDK's:
// no package or dependency is named under them (nor asuka/, theirs once).
func TestReservedNames(t *testing.T) {
	for _, n := range []string{"std/charts", "platform/battery", "asuka/http"} {
		if CheckName(n) == nil {
			t.Errorf("%s: no error", n)
		}
		if _, err := ParseSpec(n, "^1.0.0"); err == nil {
			t.Errorf("a dependency %s: no error", n)
		}
	}
	for _, n := range []string{"@acme/charts", "charts", "@std/charts"} {
		if err := CheckName(n); err != nil {
			t.Errorf("%s: %v", n, err)
		}
	}
}
