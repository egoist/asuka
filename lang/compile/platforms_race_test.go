package compile

import (
	"os"
	"path/filepath"
	"reflect"
	"testing"

	"github.com/egoist/asuka/lang/target"
)

// The platforms of a program with platforms' files compile at once
// (DirPlatforms): what they report is what compiling them one by one
// does, in the same order (go test -race: nothing they share races).
func TestDirPlatformsMatchesEachPlatformAlone(t *testing.T) {
	tmp := t.TempDir()
	files := map[string]string{
		"app.asuka":           "import std/strings\n\ncomponent Home {\n  Text(greeting())\n}\n\napp { Home() }\n\nfn shout(s: string) -> string = strings.toUpper(s)\n",
		"greet_ios.asuka":     "fn greeting() -> string = \"iPhone\"\n",
		"greet_android.asuka": "fn greeting() -> string = \"Android\"\n",
		"greet_desktop.asuka": "fn greeting() -> string = \"desktop\"\n\nfn unused() -> int = \"not an int\"\n",
		"greet_web.asuka":     "fn greeting() -> string = 1\n",
	}
	for name, src := range files {
		if err := os.WriteFile(filepath.Join(tmp, name), []byte(src), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	for _, d := range []string{tmp} {
		dir, _ := filepath.Abs(d)
		all, err := DirPlatforms(dir, nil, nil)
		if err != nil {
			t.Fatal(err)
		}
		if !all.First.PlatformFiles {
			t.Fatalf("%s: no platforms' files: pick a package with some", d)
		}
		for _, p := range target.All {
			one, err := Dir(dir, &Options{Platform: p})
			if err != nil {
				t.Fatal(err)
			}
			got := all.Results[p]
			if got == nil {
				t.Fatalf("%s: no result for %s", d, p)
			}
			if !reflect.DeepEqual(diagStrings(got), diagStrings(one)) {
				t.Errorf("%s on %s: %v, alone %v", d, p, diagStrings(got), diagStrings(one))
			}
			if (got.Module == nil) != (one.Module == nil) {
				t.Errorf("%s on %s: module %v, alone %v", d, p, got.Module != nil, one.Module != nil)
			}
		}
	}
}

func diagStrings(r *Result) []string {
	var out []string
	for _, d := range append(r.Diags, r.Warnings...) {
		out = append(out, d.String())
	}
	return out
}
