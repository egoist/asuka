//go:build darwin

package mactest

import (
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
)

// webEvalButton is a Button that evaluates script in the page and logs
// its value, or its error.
func webEvalButton(title, script string) string {
	return fmt.Sprintf(`Button(%q) { on press { do { let v = try await page.eval(%q); log = log + "eval {v};" } catch e { log = log + "eval error {e.message};" } } }`, title, script)
}

// A WebView blocks what Block names from its first page on, reports a
// file it can't show as a Download and its state as it changes, runs
// script for its value, hears what the page posts, sends its user agent,
// runs its preload before the page's scripts, hands a page for a new
// window to the app, and the web views' cookies are the app's to read.
func TestVisualWebView(t *testing.T) {
	var mu sync.Mutex
	var paths, agents []string
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		mu.Lock()
		paths = append(paths, r.URL.Path)
		agents = append(agents, r.UserAgent())
		mu.Unlock()
		switch r.URL.Path {
		case "/":
			http.SetCookie(w, &http.Cookie{Name: "flavor", Value: "oat", Path: "/"})
			w.Header().Set("Content-Type", "text/html")
			w.Write([]byte(`<!doctype html><title>Home</title><script src="/ads/a.js"></script><script src="/ok.js"></script>` +
				`<a id="f" href="/file.zip">file</a><a id="p" href="/popup" target="_blank">popup</a>`))
		case "/file.zip":
			w.Header().Set("Content-Type", "application/zip")
			w.Write([]byte("PK"))
		default:
			w.Header().Set("Content-Type", "text/javascript")
			w.Write([]byte("window.loaded = (window.loaded || 0) + 1"))
		}
	}))
	defer srv.Close()
	a := Run(t, fmt.Sprintf(`
		import platform/webview

		component Web {
		  state log = ""
		  let page = Ref()
		  Column {
		    padding: 8; gap: 4; align: .start
		    WebView(%[1]q) {
		      ref: page
		      block: ["/ads/"]
		      userAgent: "AsukaTest/1"
		      preload: "window.pre = (window.loaded || 0) + 7"
		      width: 300; height: 200
		      on loaded(u) { log = log + "loaded {u};" }
		      on state(s) { if !s.loading { log = log + "state {s.title};" } }
		      on download(d) { log = log + "download {d.url} {d.mime};" }
		      on message(m) { log = log + "message {m};" }
		      on newWindow(u) { log = log + "newWindow {u};" }
		    }
		    %[2]s
		    %[3]s
		    %[4]s
		    Button("Post") { on press { page.run("window.asuka.postMessage('hi')") } }
		    Button("Download") { on press { page.run("document.getElementById('f').click()") } }
		    Button("Popup") { on press { page.run("document.getElementById('p').click()") } }
		    Button("Cookies") { on press { let c = (try? await webview.cookies(%[1]q)) ?? "none"; log = log + "cookies {c};" } }
		    Text("log:{log}")
		  }
		}
		app { Web() }
	`, srv.URL+"/", webEvalButton("Loaded", "window.loaded"), webEvalButton("Throw", "throw new Error('nope')"),
		webEvalButton("Preloaded", "window.pre")))
	l := &vlogger{a: a}
	l.until(t, func(s string) bool {
		return strings.Contains(s, "loaded "+srv.URL+"/;") && strings.Contains(s, "state Home;")
	})
	mu.Lock()
	got := strings.Join(paths, " ")
	ua := strings.Join(agents, " ")
	mu.Unlock()
	if strings.Contains(got, "/ads/") || !strings.Contains(got, "/ok.js") {
		t.Fatalf("requests %s: /ads/ should be blocked, /ok.js not", got)
	}
	if !strings.Contains(ua, "AsukaTest/1") {
		t.Errorf("user agents %q", ua)
	}
	web := a.Tree().Kind("WebView")[0]
	if w := vobj(web, "web"); vstr(w, "title") != "Home" || vbool(w, "loading") || vnum(w, "ruleLists") != 1 || vstr(w, "userAgent") != "AsukaTest/1" {
		t.Errorf("the view: %v", w)
	}

	// Script's value, as JSON: only ok.js ran; the preload ran before it.
	a.Do("tap Loaded")
	l.until(t, func(s string) bool { return strings.HasSuffix(s, "eval 1;") || strings.Contains(s, "eval error") })
	a.Do("tap Throw")
	s := l.until(t, func(s string) bool { return strings.Count(s, "eval") >= 2 })
	if !strings.Contains(s, "eval error") || !strings.Contains(s, "nope") {
		t.Errorf("a throw: %q", s)
	}
	a.Do("tap Preloaded")
	l.until(t, func(s string) bool { return strings.HasSuffix(s, "eval 7;") })

	a.Do("tap Post")
	l.until(t, func(s string) bool { return strings.HasSuffix(s, "message hi;") })

	a.Do("tap Download")
	s = l.until(t, func(s string) bool { return strings.Contains(s, "download ") })
	if !strings.Contains(s, "download "+srv.URL+"/file.zip application/zip;") {
		t.Errorf("the download: %q", s)
	}

	a.Do("tap Popup")
	l.until(t, func(s string) bool { return strings.Contains(s, "newWindow "+srv.URL+"/popup;") })

	a.Do("tap Cookies")
	l.until(t, func(s string) bool { return strings.HasSuffix(s, "cookies flavor=oat;") })
}

// On a page whose Content Security Policy blocks eval, a WebView's eval
// still answers with the last statement's value, as JSON; what a script
// threw, or a value WebKit can't return (a promise), is an error.
func TestVisualWebViewEvalCSP(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Security-Policy", "script-src 'self'")
		if r.URL.Path == "/ok.js" {
			w.Header().Set("Content-Type", "text/javascript")
			w.Write([]byte("window.loaded = 1"))
			return
		}
		w.Header().Set("Content-Type", "text/html")
		w.Write([]byte(`<!doctype html><title>Locked</title><script src="/ok.js"></script>`))
	}))
	defer srv.Close()
	a := Run(t, fmt.Sprintf(`
		component Web {
		  state log = ""
		  let page = Ref()
		  Column {
		    padding: 8; gap: 4; align: .start
		    WebView(%q) { ref: page; width: 300; height: 200; on loaded(u) { log = log + "loaded;" } }
		    %s
		    Text("log:{log}")
		  }
		}
		app { Web() }
	`, srv.URL+"/", webEvalButton("Title", "document.title")))
	l := &vlogger{a: a}
	l.until(t, func(s string) bool { return strings.Contains(s, "loaded;") })
	// Through the app's Ref.
	a.Do("tap Title")
	l.until(t, func(s string) bool { return strings.HasSuffix(s, `eval "Locked";`) })

	// Through the "web" module the Ref calls, script by script.
	web := vid(a.Tree().Kind("WebView")[0])
	eval := func(script string) string {
		t.Helper()
		i := len(vstrs(visualState(a.Tree()), "moduleResults"))
		a.Do("module web eval " + web + " " + script)
		var r string
		a.Until(func(tr *Tree) bool {
			rs := vstrs(visualState(tr), "moduleResults")
			if len(rs) > i && rs[i] != "" {
				r = rs[i]
				return true
			}
			return false
		})
		return r
	}
	for _, c := range []struct{ script, want string }{
		{"document.title", `"Locked"`},
		{"window.loaded", `1`},
		{"let a = 2; a * 3", `6`},
		{"({a: [1, 'x', null]})", `{"a":[1,"x",null]}`},
		{"new Date(0)", `"1970-01-01T00:00:00.000Z"`},
		{"undefined", `null`},
		{"var kept = 5", `null`},
		{"kept", `5`},
	} {
		if r := eval(c.script); r != "ok "+c.want {
			t.Errorf("%s: %q, want %s", c.script, r, c.want)
		}
	}
	if r := eval("eval('1')"); !strings.HasPrefix(r, "error ") {
		t.Errorf("the page doesn't block eval: %q", r)
	}
	if r := eval("throw new Error('nope')"); !strings.HasPrefix(r, "error ") || !strings.Contains(r, "nope") {
		t.Errorf("a throw: %q", r)
	}
	if r := eval("1 +"); !strings.HasPrefix(r, "error ") || !strings.Contains(r, "SyntaxError") {
		t.Errorf("a syntax error: %q", r)
	}
	if r := eval("Promise.resolve(1)"); !strings.HasPrefix(r, "error ") || !strings.Contains(r, "Content Security Policy") {
		t.Errorf("a promise: %q", r)
	}
}
