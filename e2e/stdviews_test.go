//go:build e2e

package e2e

import (
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// TestStdWebViews runs the standard library's and the platform SDK's web
// view tests: each {std,sdk}/<name>/tests/*_view_test.js, a module the page imports (served
// from the checkout, so it imports its package's web/ as an app does),
// in headless Chrome, read once the test wrote its result into
// #result: "PASS ..." or "FAIL" and why.
func TestStdWebViews(t *testing.T) {
	browser := chrome()
	if browser == "" {
		t.Skip("no Chrome (ASUKA_CHROME)")
	}
	r := root(t)
	tests, _ := filepath.Glob(filepath.Join(r, "std", "*", "tests", "*_view_test.js"))
	sdk, _ := filepath.Glob(filepath.Join(r, "sdk", "*", "tests", "*_view_test.js"))
	tests = append(tests, sdk...)
	if len(tests) == 0 {
		t.Fatal("no web view tests")
	}
	files := http.FileServer(http.Dir(r))
	for _, test := range tests {
		rel, _ := filepath.Rel(r, test)
		t.Run(strings.TrimSuffix(filepath.ToSlash(rel), ".js"), func(t *testing.T) {
			mux := http.NewServeMux()
			mux.HandleFunc("/test.html", func(w http.ResponseWriter, req *http.Request) {
				w.Header().Set("Content-Type", "text/html")
				w.Write([]byte(`<!doctype html><meta charset="utf-8"><body><pre id="result">RUNNING</pre>` +
					`<script type="module" src="/` + filepath.ToSlash(rel) + `"></script>`))
			})
			mux.Handle("/", files)
			srv := httptest.NewServer(mux)
			defer srv.Close()
			// A page that draws frames, as an app's does: a view's
			// ResizeObserver and requestAnimationFrame run.
			page := devtoolsPage(t, browser, srv.URL+"/test.html", "1")
			result := "RUNNING"
			for deadline := time.Now().Add(within); time.Now().Before(deadline); time.Sleep(100 * time.Millisecond) {
				var r struct{ Result struct{ Value string } }
				if err := page.call("Runtime.evaluate", map[string]any{
					"expression": `document.getElementById("result")?.textContent ?? "RUNNING"`, "returnByValue": true,
				}, &r); err != nil {
					t.Fatal(err)
				}
				if result = r.Result.Value; result != "RUNNING" {
					break
				}
			}
			t.Log(result)
			if !strings.HasPrefix(result, "PASS") {
				t.Fatal(result)
			}
		})
	}
}
