package main

import (
	"encoding/json"
	"net/http/httptest"
	"net/url"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"testing"

	"github.com/egoist/asuka/internal/cbuild"
	"github.com/egoist/asuka/lang/target"
)

const previewsApp = `component Counter {
  state count = 0
  Column {
    Text("Count: {count}")
    Button("Add") {
      on press { count += 1 }
    }
  }
}

app { Counter() }

preview "Framed" (width: 300, height: 200, dark: true) {
  state label = "In a preview"
  Text(label)
  Counter()
}
`

// asuka dev's previews on this Mac's player: the inspector lists the
// app's, and showing one has the player load it in place of the app;
// a save reloads it keeping its state, and back to the app, the app's
// state is as it was.
func TestDevDesktopPreviews(t *testing.T) {
	needMacBuild(t)
	t.Setenv("ASUKA_DEV_REGISTRY", t.TempDir())
	dir := filepath.Join(t.TempDir(), "counter")
	os.MkdirAll(dir, 0o755)
	os.WriteFile(filepath.Join(dir, "asuka.toml"), []byte("name = \"Counter\"\nid = \"dev.asuka.devtest\"\n"), 0o644)
	src := filepath.Join(dir, "app.asuka")
	os.WriteFile(src, []byte(previewsApp), 0o644)
	app := projectApp(t, dir, nil)
	app.platform = target.MacOS
	work := t.TempDir()
	loop, err := newDevLoop(app, "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer loop.close()
	var printed strings.Builder
	loop.out = &printed
	exe, err := buildDesktopPlayer(app, platformMac, cbuild.Host(), work, false)
	if err != nil {
		t.Fatal(err)
	}
	snaps := t.TempDir()
	at := func(name string) string { return filepath.Join(snaps, name) }
	cmd := exec.Command(exe)
	cmd.Env = append(os.Environ(),
		"ASUKA_DEV=127.0.0.1:"+strconv.Itoa(loop.port()),
		"ASUKA_DATA="+filepath.Join(work, "data"),
		"ASUKA_SCRIPT=waitfor:"+at("loaded")+";tap:Add;snap:"+at("app.png")+";waitfor:"+at("shown")+";tap:Add;tap:Add;snap:"+at("preview.png")+
			";waitfor:"+at("edited")+";snap:"+at("edited.png")+";waitfor:"+at("back"),
		"ASUKA_SNAPSHOT="+at("last.png"), "ASUKA_DUMP=1")
	var out strings.Builder
	cmd.Stdout, cmd.Stderr = &out, &out
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	defer cmd.Process.Kill()
	if r := nextReload(t, loop); !r.first {
		t.Fatalf("the first report: %+v", r)
	}
	os.WriteFile(at("loaded"), nil, 0o644)
	if b := waitFile(t, at("app.png.txt")); !strings.Contains(string(b), `"Count: 1"`) || strings.Contains(string(b), "In a preview") {
		t.Fatalf("the app:\n%s", b)
	}

	// The inspector lists the previews; the terminal prints them.
	rec := httptest.NewRecorder()
	loop.inspector.ServeHTTP(rec, httptest.NewRequest("GET", "/previews", nil))
	var list previewsAnswer
	if err := json.Unmarshal(rec.Body.Bytes(), &list); err != nil || len(list.Previews) != 1 || list.Previews[0].Name != "Framed" || list.Previews[0].Line != 13 ||
		list.Previews[0].Path != "app.asuka" || list.Previews[0].Settings != "width: 300, height: 200, dark: true" {
		t.Fatalf("the previews: %v %s", err, rec.Body.String())
	}
	loop.printPreviews()
	if !strings.Contains(printed.String(), "1  Framed (width: 300, height: 200, dark: true)  app.asuka:13") {
		t.Errorf("the terminal printed:\n%s", printed.String())
	}

	// Shown, as the Previews tab asks (an editor's lens: by its line).
	q := url.Values{"file": {src}, "line": {"14"}}
	rec = httptest.NewRecorder()
	loop.inspector.ServeHTTP(rec, httptest.NewRequest("POST", "/preview?"+q.Encode(), nil))
	if rec.Code != 204 {
		t.Fatalf("showing it: %d %s", rec.Code, rec.Body.String())
	}
	if r := nextReload(t, loop); r.first || r.restarted {
		t.Fatalf("the preview's load: %+v", r)
	}
	os.WriteFile(at("shown"), nil, 0o644)
	b := waitFile(t, at("preview.png.txt"))
	if !strings.Contains(string(b), `"In a preview"`) || !strings.Contains(string(b), `"Count: 2"`) {
		t.Fatalf("the preview:\n%s", b)
	}
	inspects(t, loop, "macOS", `"text":"In a preview"`)
	rec = httptest.NewRecorder()
	loop.inspector.ServeHTTP(rec, httptest.NewRequest("GET", "/previews", nil))
	if !strings.Contains(rec.Body.String(), `"shown":{"`) || !strings.Contains(rec.Body.String(), `"name":"Framed"`) {
		t.Errorf("what the players show: %s", rec.Body.String())
	}

	// A save: the preview again, its state kept.
	os.WriteFile(src, []byte(strings.Replace(previewsApp, "Text(label)", `Text("Edited: {label}")`, 1)), 0o644)
	if !loop.rebuild() {
		t.Fatal("didn't compile")
	}
	if r := nextReload(t, loop); r.first || r.kept < 2 {
		t.Fatalf("the preview's reload: %+v", r)
	}
	os.WriteFile(at("edited"), nil, 0o644)
	if b := waitFile(t, at("edited.png.txt")); !strings.Contains(string(b), `"Edited: In a preview"`) || !strings.Contains(string(b), `"Count: 2"`) {
		t.Fatalf("after the save:\n%s", b)
	}

	// Back to the app: its count as it was.
	rec = httptest.NewRecorder()
	loop.inspector.ServeHTTP(rec, httptest.NewRequest("POST", "/preview", nil))
	if rec.Code != 204 {
		t.Fatalf("back to the app: %d %s", rec.Code, rec.Body.String())
	}
	if r := nextReload(t, loop); r.first || r.kept < 1 {
		t.Fatalf("the app's load: %+v", r)
	}
	os.WriteFile(at("back"), nil, 0o644)
	if err := cmd.Wait(); err != nil {
		t.Fatalf("%v\n%s", err, out.String())
	}
	if b := waitFile(t, at("last.png.txt")); !strings.Contains(string(b), `"Count: 1"`) || strings.Contains(string(b), "preview") {
		t.Fatalf("back to the app, the window shows:\n%s\n%s", b, out.String())
	}
}
