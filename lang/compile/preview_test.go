package compile

import (
	"slices"
	"strings"
	"testing"
)

const previewSrc = `component Counter {
  state count = 0
  Text("Count: {count}")
}

app { Text("The app") }

preview "At zero" { Counter() }

preview "Dark, large text" (dark: true, fontScale: 1.5, width: 320, height: 200) {
  state n = 7
  Text("n is {n}")
}

test "counts" {
  let app = mount(Counter())
  app.expect("Count: 0")
}
`

func previewFunc(names []string) bool {
	return slices.ContainsFunc(names, func(s string) bool { return strings.Contains(s, "preview") })
}

// A build has no previews (nor what only they use); a preview's build
// has the preview as its app, and its settings in its init.
func TestPreviewBuilds(t *testing.T) {
	files := map[string][]byte{"app.asuka": []byte(previewSrc)}
	res := Files(files, &Options{NoTests: true, Debug: true})
	if err := res.Err(); err != nil {
		t.Fatal(err)
	}
	m := res.Module
	var names []string
	for _, f := range m.Funcs {
		names = append(names, f.Name)
	}
	if previewFunc(names) || slices.Contains(m.Builtins, "ui.preview") || slices.Contains(m.Strings, "n is ") {
		t.Errorf("a build has a preview: %v", names)
	}

	for _, ref := range []*PreviewRef{{File: "app.asuka", Name: "Dark, large text"}, {File: "app.asuka", Line: 11}} {
		res = Files(files, &Options{NoTests: true, Debug: true, Preview: ref})
		if err := res.Err(); err != nil {
			t.Fatal(err)
		}
		m = res.Module
		if m.App < 0 || !slices.Contains(m.Builtins, "ui.preview") || len(m.Tests) != 0 {
			t.Fatalf("%+v: app %d, builtins %v, tests %d", ref, m.App, m.Builtins, len(m.Tests))
		}
		if !slices.Contains(m.Floats, 1.5) || !slices.Contains(m.Floats, 320) {
			t.Errorf("%+v: its settings aren't in its init: %v", ref, m.Floats)
		}
		runner, err := Runner(false)
		if err != nil {
			t.Fatal(err)
		}
		out, err := Run(runner, m, false, "", "-app")
		if err != nil || !strings.Contains(out, "n is 7") || strings.Contains(out, "The app") {
			t.Errorf("%+v: the preview's build shows\n%s(%v)", ref, out, err)
		}
	}

	res = Files(files, &Options{NoTests: true, Preview: &PreviewRef{File: "app.asuka", Name: "Gone"}})
	if err := res.Err(); err == nil || !strings.Contains(err.Error(), `no preview "Gone"`) {
		t.Errorf("a preview that isn't there: %v", err)
	}
}

// asuka test --previews: each preview is a test of the test build.
func TestPreviewTests(t *testing.T) {
	files := map[string][]byte{"app.asuka": []byte(previewSrc)}
	res := Files(files, &Options{Debug: true})
	if err := res.Err(); err != nil {
		t.Fatal(err)
	}
	if n := len(res.Module.Tests); n != 1 {
		t.Errorf("a test build without --previews has %d tests", n)
	}
	res = Files(files, &Options{Debug: true, PreviewTests: true})
	if err := res.Err(); err != nil {
		t.Fatal(err)
	}
	var names []string
	for _, x := range res.Module.Tests {
		names = append(names, x.Name)
	}
	if !slices.Equal(names, []string{"counts", "preview At zero", "preview Dark, large text"}) {
		t.Errorf("tests %q", names)
	}
}
