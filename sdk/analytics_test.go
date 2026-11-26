package sdk

import (
	"bytes"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"testing"

	"github.com/egoist/asuka/internal/cbuild"
	"github.com/egoist/asuka/internal/nativegen"
)

// mpRequest is a Measurement Protocol request as GA4 reads it: a field
// it doesn't know is an error (its validation server says so too).
type mpRequest struct {
	ClientID       string                         `json:"client_id"`
	UserID         string                         `json:"user_id"`
	UserProperties map[string]struct{ Value any } `json:"user_properties"`
	Consent        *struct {
		AdUserData        string `json:"ad_user_data"`
		AdPersonalization string `json:"ad_personalization"`
	} `json:"consent"`
	Events []struct {
		Name            string         `json:"name"`
		Params          map[string]any `json:"params"`
		TimestampMicros int64          `json:"timestamp_micros"`
	} `json:"events"`
}

// platform/analytics on Windows and Linux sends GA4's Measurement
// Protocol: its C test (desktop/analytics_test.c, also TestStdUnits')
// sends its requests to a server here, which reads them as GA does.
func TestAnalyticsPayload(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("built with the POSIX test host's flags")
	}
	core, err := cbuild.CoreDir()
	if err != nil {
		t.Skip(err)
	}
	var mu sync.Mutex
	var got []*http.Request
	var bodies [][]byte
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		b, _ := io.ReadAll(r.Body)
		mu.Lock()
		got = append(got, r)
		bodies = append(bodies, b)
		mu.Unlock()
		w.WriteHeader(http.StatusNoContent)
	}))
	defer srv.Close()

	dir, _ := filepath.Abs(filepath.Join("analytics", "desktop"))
	desk, _ := filepath.Abs(filepath.Join("..", "platform", "desktop"))
	srcs := []string{filepath.Join(dir, "analytics_test.c"), filepath.Join(desk, "test", "unit.c"), filepath.Join(desk, "src", "native.c"),
		filepath.Join(desk, "src", "json.c"), filepath.Join(desk, "src", "jsontree.c"), filepath.Join(desk, "src", "linux.c"),
		filepath.Join(desk, "src", "linux_syms.c")}
	for _, f := range nativegen.CSources(dir, "unit") {
		if !strings.HasSuffix(f, ".asuka.c") && !strings.HasSuffix(f, "_test.c") {
			srcs = append(srcs, filepath.Join(dir, f))
		}
	}
	flags := append(append([]string{}, cbuild.Flags...), "-I"+filepath.Join(desk, "include"), "-I"+dir, "-D_GNU_SOURCE")
	work := t.TempDir()
	exe, err := cbuild.Exe(core, srcs, flags, nil, true, work, "analytics")
	if err != nil {
		t.Fatal(err)
	}
	cmd := exec.Command(exe)
	cmd.Env = append(os.Environ(), "ASUKA_DATA="+work, "ASAN_OPTIONS=detect_leaks=0", "ANALYTICS_TEST_ENDPOINT="+srv.URL+"/mp/collect")
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("%v\n%s", err, out)
	}

	mu.Lock()
	defer mu.Unlock()
	if len(got) < 8 {
		t.Fatalf("%d requests", len(got))
	}
	names := map[string]bool{}
	for i, r := range got {
		if r.Method != "POST" || r.URL.Path != "/mp/collect" || r.Header.Get("Content-Type") != "application/json" {
			t.Errorf("request %d: %s %s %s", i, r.Method, r.URL, r.Header.Get("Content-Type"))
		}
		if q := r.URL.Query(); q.Get("measurement_id") != "G-TEST123" || q.Get("api_secret") != "s3cret" {
			t.Errorf("request %d's query: %s", i, r.URL.RawQuery)
		}
		var req mpRequest
		d := json.NewDecoder(bytes.NewReader(bodies[i]))
		d.DisallowUnknownFields()
		if err := d.Decode(&req); err != nil {
			t.Errorf("request %d: %v\n%s", i, err, bodies[i])
			continue
		}
		if req.ClientID == "" || len(req.Events) == 0 || len(req.Events) > 25 {
			t.Errorf("request %d: client %q, %d events", i, req.ClientID, len(req.Events))
		}
		for _, e := range req.Events {
			names[e.Name] = true
			if _, ok := e.Params["session_id"].(string); !ok || e.TimestampMicros == 0 {
				t.Errorf("request %d: %s without its session or time: %s", i, e.Name, bodies[i])
			}
			if ms, _ := e.Params["engagement_time_msec"].(float64); ms < 1 {
				t.Errorf("request %d: %s's engagement %v", i, e.Name, e.Params["engagement_time_msec"])
			}
			switch e.Name {
			case "level_end":
				if e.Params["level"] != 3.0 || e.Params["result"] != "won" || e.Params["score"] != 2.5 {
					t.Errorf("level_end's params: %v", e.Params)
				}
			case "page_view":
				if e.Params["page_title"] != "Settings Page" || e.Params["page_location"] != "app://dev.asuka.test/Settings%20Page" {
					t.Errorf("page_view's params: %v", e.Params)
				}
			case "purchase":
				items, _ := e.Params["items"].([]any)
				item, _ := items[0].(map[string]any)
				if len(items) != 1 || item["item_id"] != "SKU_1" || item["quantity"] != 2.0 || e.Params["level"] != 2.0 {
					t.Errorf("purchase's params: %v", e.Params)
				}
			case "after":
				if req.UserID != "u_1" || req.UserProperties["plan"].Value != "pro" || req.Consent == nil ||
					req.Consent.AdUserData != "DENIED" || req.Consent.AdPersonalization != "GRANTED" {
					t.Errorf("after's request: %s", bodies[i])
				}
			}
		}
	}
	for _, n := range []string{"level_end", "tick", "back", "page_view", "purchase", "before", "after", "first", "second", "new"} {
		if !names[n] {
			t.Errorf("no %s event sent", n)
		}
	}
	if names["screen_view"] {
		t.Error("a screen_view: the desktops send a screen as a page_view")
	}
}
