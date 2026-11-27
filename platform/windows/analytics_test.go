package windows_test

import (
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"io"
	"net"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"testing"
)

// analyticsApp logs two events and a screen when Log is pressed.
const analyticsApp = `import platform/analytics

component Logger {
  state said = "ready"
  Column {
    Text(said)
    Button("Log") {
      on press {
        do {
          try analytics.log("tapped", ["n": 1, "where": "windows"])
          try analytics.log("tapped", ["n": 2])
          try analytics.screen("Home")
          said = "logged {analytics.available()}"
        } catch e {
          said = e.message
        }
      }
    }
  }
}

app { Logger() }
`

// platform/analytics' Measurement Protocol sender in a Windows app, its
// requests to a server of the test's (asuka.toml's analytics.endpoint):
// the events of a run that ended before their batch was due are kept in
// the app's data (%LOCALAPPDATA%\<name>\...) and sent, in one request,
// by the next run, under the same client ID.
func TestAnalytics(t *testing.T) {
	w := onWindows(t)
	local, guest := w.job(t)
	var mu sync.Mutex
	var bodies []string
	var queries []string
	ln, err := net.Listen("tcp", ":0")
	if err != nil {
		t.Fatal(err)
	}
	srv := &http.Server{Handler: http.HandlerFunc(func(rw http.ResponseWriter, r *http.Request) {
		b, _ := io.ReadAll(r.Body)
		mu.Lock()
		bodies = append(bodies, string(b))
		queries = append(queries, r.URL.Path+"?"+r.URL.RawQuery)
		mu.Unlock()
		rw.WriteHeader(http.StatusNoContent)
	})}
	go srv.Serve(ln)
	defer srv.Close()
	endpoint := "http://" + hostAddress(t, w) + ":" + strconv.Itoa(ln.Addr().(*net.TCPAddr).Port) + "/mp/collect"

	buildStdApp(t, w, analyticsApp, local, "analytics")
	b := make([]byte, 4)
	rand.Read(b)
	name := "AsukaAnalytics" + hex.EncodeToString(b)
	cfg, _ := json.Marshal(map[string]any{"name": name, "id": "dev.asuka.analytics", "settings": map[string]string{
		"analytics.measurementId": "G-TEST123", "analytics.apiSecret": "s3cret", "analytics.endpoint": endpoint,
	}})
	if err := os.WriteFile(filepath.Join(local, "asuka-app.json"), cfg, 0o644); err != nil {
		t.Fatal(err)
	}
	// The app's data where Windows keeps it, not the job's.
	defer exec.Command("swiftvm", "exec", w.vm, "--", "Remove-Item -Recurse -Force (Join-Path $env:LOCALAPPDATA '"+name+"') -ErrorAction SilentlyContinue").Run()

	// The first run logs, and ends before the batch is due (5 seconds).
	d := script(t, w, local, guest, "app.exe", "tap:Log", "wait:1500ms", "snap:logged")
	shows(t, d["logged"], "the first run", []string{"logged true"})
	mu.Lock()
	early := len(bodies)
	mu.Unlock()
	if early != 0 {
		t.Errorf("the batch went before it was due: %d requests", early)
	}
	if w.vm != "" {
		out, err := exec.Command("swiftvm", "exec", w.vm, "--", "Get-ChildItem -Recurse -File (Join-Path $env:LOCALAPPDATA '"+name+"') | ForEach-Object { $_.FullName }").CombinedOutput()
		if err != nil || !strings.Contains(string(out), `\asuka-analytics`) {
			t.Errorf("what the app keeps isn't in its data, %%LOCALAPPDATA%%\\%s: %v\n%s", name, err, out)
		} else {
			t.Logf("kept: %s", strings.TrimSpace(string(out)))
		}
	}

	// The next run sends them.
	script(t, w, local, guest, "app.exe", "wait:7s")
	mu.Lock()
	defer mu.Unlock()
	if len(bodies) != 1 {
		t.Fatalf("%d requests, want 1 (the first run's events, batched): %v", len(bodies), bodies)
	}
	if q := queries[0]; !strings.HasPrefix(q, "/mp/collect?") || !strings.Contains(q, "measurement_id=G-TEST123") || !strings.Contains(q, "api_secret=s3cret") {
		t.Errorf("the request's URL: %s", q)
	}
	var req struct {
		ClientID string `json:"client_id"`
		Events   []struct {
			Name   string         `json:"name"`
			Params map[string]any `json:"params"`
		} `json:"events"`
	}
	if err := json.Unmarshal([]byte(bodies[0]), &req); err != nil {
		t.Fatalf("%v\n%s", err, bodies[0])
	}
	var names []string
	for _, e := range req.Events {
		names = append(names, e.Name)
	}
	if req.ClientID == "" || strings.Join(names, ",") != "tapped,tapped,page_view" {
		t.Errorf("the batch: client %q, events %v\n%s", req.ClientID, names, bodies[0])
	}
	if len(req.Events) == 3 && (req.Events[0].Params["where"] != "windows" || req.Events[2].Params["page_location"] != "app://dev.asuka.analytics/Home") {
		t.Errorf("the events' params: %s", bodies[0])
	}
}
