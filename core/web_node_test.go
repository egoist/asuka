package core_test

import (
	"io"
	"net"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/egoist/asuka/internal/appbuild"
	"github.com/egoist/asuka/internal/cbuild"
	"github.com/egoist/asuka/lang/compile"
	"github.com/egoist/asuka/lang/target"
)

// The player's core as WebAssembly (no app compiled in), built once for
// the tests that run apps on it under Node (testdata/web/run.mjs).
var (
	playerOnce sync.Once
	playerWasm string
	playerErr  error
)

func webPlayer(t *testing.T) string {
	t.Helper()
	tc, err := cbuild.ForTarget(cbuild.Target{OS: "wasm", Arch: "wasm32"})
	if err != nil {
		t.Skip(err)
	}
	playerOnce.Do(func() {
		dir, err := os.MkdirTemp("", "asuka-web-player")
		if err != nil {
			playerErr = err
			return
		}
		root, _ := filepath.Abs(".")
		playerWasm = filepath.Join(dir, "asuka.wasm")
		playerErr = tc.Lib(root, cbuild.Module, nil, false, playerWasm)
	})
	if playerErr != nil {
		t.Fatal(playerErr)
	}
	return playerWasm
}

// runWebNode compiles the Asuka file src (testdata/web) with opts and
// runs it on the web host's core under Node, returning what it printed.
func runWebNode(t *testing.T, src string, opts *compile.Options, extra ...string) string {
	t.Helper()
	if testing.Short() {
		t.Skip("WebAssembly under Node (not -short)")
	}
	node, err := exec.LookPath("node")
	if err != nil {
		t.Skip("no Node")
	}
	wasm := webPlayer(t)
	b, err := os.ReadFile(filepath.Join("testdata", "web", src))
	if err != nil {
		t.Fatal(err)
	}
	o := compile.Options{}
	if opts != nil {
		o = *opts
	}
	o.Platform = target.Web
	res := compile.Files(map[string][]byte{src: b}, &o)
	if err := res.Err(); err != nil {
		t.Fatal(err)
	}
	akb := filepath.Join(t.TempDir(), "app.akb")
	if err := os.WriteFile(akb, res.Module.Encode(), 0o644); err != nil {
		t.Fatal(err)
	}
	web, _ := filepath.Abs(filepath.Join("..", "platform", "web", "src"))
	cmd := exec.Command(node, append([]string{filepath.Join("testdata", "web", "run.mjs"), web, wasm, akb}, extra...)...)
	var stderr strings.Builder
	cmd.Stderr = &stderr
	out, err := cmd.Output()
	if err != nil {
		t.Fatalf("%v\n%s%s", err, out, stderr.String())
	}
	return string(out)
}

// spawn on the web runs on workers, each the same core with the same
// app: values and errors there and back, the app's files on both sides,
// several at once, a cancelled one.
func TestWebSpawn(t *testing.T) {
	got := runWebNode(t, "spawn.asuka", &compile.Options{Lang: "fr"})
	// The registry (not a value) is made once: by the page, not by the
	// workers too.
	want := `registry made
500500
d3 3 2 -1 true 0
path 3 5 2
0 3
caught no to 2
not found nowhere.txt
from the page
from a worker
changed
false
worker.txt 13
55 210 465 820
first -1
15
(15 jobs on workers)
done
`
	if strings.TrimSpace(got) != strings.TrimSpace(want) {
		t.Fatalf("got:\n%s\nwant:\n%s", got, want)
	}
}

// A native package's module on the web: the core makes it and calls it
// through the page's JavaScript (native.mjs's), now and async, and its
// callbacks answer, during a call and on their own; an interface its
// JavaScript doesn't make is unsupported.
func TestWebNative(t *testing.T) {
	mjs, _ := filepath.Abs(filepath.Join("testdata", "web", "native.mjs"))
	got := runWebNode(t, "native.asuka", nil, mjs)
	want := `HELLO
after!
6
ticks 6
missing: unsupported: no interface 1
(0 jobs on workers)
done
`
	if strings.TrimSpace(got) != strings.TrimSpace(want) {
		t.Fatalf("got:\n%s\nwant:\n%s", got, want)
	}
}

// asqliteWasm is std/sqlite's C for the web, its SQLite worker's module
// (appbuild.SQLiteWasm: built into the cache, with Zig).
func asqliteWasm(t *testing.T) string {
	t.Helper()
	p, err := appbuild.SQLiteWasm(false)
	if err != nil {
		t.Skip(err)
	}
	return p
}

// std/sqlite on the web: the core's calls go to the page's SQLite
// worker and come back decoded; open and migrate run before what follows
// them, and a failed migration fails the next statement; a live query
// follows the writes; spawn's workers open the app's database at init
// harmlessly.
func TestWebSQLite(t *testing.T) {
	got := runWebNode(t, "sqlite.asuka", nil, asqliteWasm(t))
	want := `1 milk false
2 eggs true
count 2
1 true 2 2.5
2 false 0 -1
caught sqlite: no such table: nowhere (1)
caught sqlite: migration 2: sqlite: near "this": syntax error (1)
t 1 2
live 1 milk
live 2 bread
/data/spawntest/notes.db
42
(1 jobs on workers)
done
`
	if strings.TrimSpace(got) != strings.TrimSpace(want) {
		t.Fatalf("got:\n%s\nwant:\n%s", got, want)
	}
}

// platform/crash on the web: the core keeps the run's record in the app's
// files, and reads the reports in a spawn worker, told where the page's
// record is.
func TestWebCrash(t *testing.T) {
	got := runWebNode(t, "crash.asuka", nil)
	want := `nonfatal false card declined u1 1 Web
(1 jobs on workers)
done
`
	if strings.TrimSpace(got) != strings.TrimSpace(want) {
		t.Fatalf("got:\n%s\nwant:\n%s", got, want)
	}
}

// The same programs in headless Chrome, on the web host's page: spawn on
// real Web Workers, the app's files in IndexedDB, SQLite's worker over
// the origin private file system, the crash record.
func TestWebChrome(t *testing.T) {
	if testing.Short() {
		t.Skip("headless Chrome (not -short)")
	}
	browser := chromePath()
	if browser == "" {
		t.Skip("no Chrome (ASUKA_CHROME)")
	}
	wasm := webPlayer(t)
	asqlite := asqliteWasm(t)
	web, _ := filepath.Abs(filepath.Join("..", "platform", "web"))
	for _, c := range []struct{ src, want string }{
		{"spawn.asuka", "registry made\n500500\nd3 3 2 -1 true 0\npath 3 5 2\n0 3\ncaught no to 2\nnot found nowhere.txt\nfrom the page\nfrom a worker\nchanged\nfalse\nworker.txt 13\n55 210 465 820\nfirst -1\n15\ndone"},
		{"sqlite.asuka", "1 milk false\n2 eggs true\ncount 2\n1 true 2 2.5\n2 false 0 -1\ncaught sqlite: no such table: nowhere (1)\ncaught sqlite: migration 2: sqlite: near \"this\": syntax error (1)\nt 1 2\nlive 1 milk\nlive 2 bread\n/data/webtest/notes.db\n42\ndone"},
		{"crash.asuka", "nonfatal false card declined u1 1 Web\ndone"},
	} {
		t.Run(strings.TrimSuffix(c.src, ".asuka"), func(t *testing.T) {
			b, err := os.ReadFile(filepath.Join("testdata", "web", c.src))
			if err != nil {
				t.Fatal(err)
			}
			res := compile.Files(map[string][]byte{c.src: b}, &compile.Options{Platform: target.Web})
			if err := res.Err(); err != nil {
				t.Fatal(err)
			}
			akb := res.Module.Encode()
			page := `<!doctype html><meta charset="utf-8"><body><script type="module">
const lines = [];
const send = () => fetch("/result", { method: "POST", body: lines.join("\n") });
addEventListener("error", (e) => { lines.push("ERROR " + e.message); send(); });
const log = console.log;
console.log = (...a) => { const l = a.join(" "); lines.push(l); log(...a); if (l === "done") send(); };
import { start } from "/src/asuka.js";
await start({ core: "/asuka.wasm", module: "/app.akb", name: "webtest", sqlite: "/asqlite.wasm" });
</script>`
			results := make(chan string, 1)
			mux := http.NewServeMux()
			mux.Handle("/src/", http.FileServer(http.Dir(web)))
			wasmFile := func(path string) http.HandlerFunc {
				return func(w http.ResponseWriter, r *http.Request) {
					w.Header().Set("Content-Type", "application/wasm")
					http.ServeFile(w, r, path)
				}
			}
			mux.HandleFunc("/asuka.wasm", wasmFile(wasm))
			mux.HandleFunc("/asqlite.wasm", wasmFile(asqlite))
			mux.HandleFunc("/app.akb", func(w http.ResponseWriter, r *http.Request) { w.Write(akb) })
			mux.HandleFunc("/result", func(w http.ResponseWriter, r *http.Request) {
				b, _ := io.ReadAll(r.Body)
				select {
				case results <- string(b):
				default:
				}
			})
			mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) { io.WriteString(w, page) })
			l, err := net.Listen("tcp", "127.0.0.1:0")
			if err != nil {
				t.Fatal(err)
			}
			srv := &http.Server{Handler: mux}
			go srv.Serve(l)
			defer srv.Close()
			cmd := exec.Command(browser, "--headless=new", "--no-first-run", "--no-default-browser-check", "--disable-gpu",
				"--user-data-dir="+t.TempDir(), "http://"+l.Addr().String()+"/")
			if err := cmd.Start(); err != nil {
				t.Fatal(err)
			}
			defer func() { cmd.Process.Kill(); cmd.Wait() }()
			select {
			case got := <-results:
				if strings.TrimSpace(got) != c.want {
					t.Fatalf("the page printed:\n%s\nwant:\n%s", got, c.want)
				}
			case <-time.After(60 * time.Second):
				t.Fatal("the page never reported")
			}
		})
	}
}
