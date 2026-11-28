package appbuild

import (
	"os"
	"path/filepath"
	"testing"
)

// usesTestApp writes an app that calls platform/location's current() and
// platform/icloud's remove(), with a local function named delete, and a
// dependency (@test/sync) that calls location.requestAlways and
// icloud.readFile; it returns the app's directory.
func usesTestApp(t *testing.T) string {
	t.Helper()
	root := t.TempDir()
	app, sync := filepath.Join(root, "app"), filepath.Join(root, "sync")
	os.MkdirAll(app, 0o755)
	os.MkdirAll(sync, 0o755)
	write := func(path, s string) {
		if err := os.WriteFile(path, []byte(s), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	write(filepath.Join(app, "app.asuka"), `import platform/icloud
import platform/location
import platform/notify
import @test/sync

async fn clear() {
  let _ = try? await icloud.remove("k")
  let _ = try? await location.current()
  let n = notify.Notification(body: "hi").from(notify.Person(name: "Ann"), notify.Conversation())
  let m = notify.Notification(sender: notify.Person(name: "Bo"), interruption: .critical)
  let level = notify.Interruption.timeSensitive
  try? await notify.show(m)
  try? await notify.show(n)
}

fn delete(x: string) -> string = x

test "a test's calls aren't the app's" {
  let _ = try? await icloud.documents()
}

app {
  Column {
    Text(delete(sync.name()))
    Video("a.mp4") { pictureInPicture: true }
  }
}
`)
	write(filepath.Join(app, "asuka.toml"), "id = \"dev.asuka.journal\"\n\n[dependencies]\n\"@test/sync\" = \"file:../sync\"\n")
	write(filepath.Join(sync, "sync.asuka"), `import platform/icloud
import platform/location

pub fn name() -> string = "sync"

pub async fn pull() throws -> bytes {
  let _ = try await location.requestAlways()
  return try await icloud.readFile("notes.txt")
}
`)
	write(filepath.Join(sync, "asuka.toml"), "name = \"@test/sync\"\nversion = \"0.1.0\"\n")
	return app
}

// Uses is what the checker resolved, across the app and its
// dependencies: the members referred to from outside their packages, not
// every identifier (the app's own delete isn't platform/icloud's), and not a
// test block's.
func TestUses(t *testing.T) {
	p, err := Load(usesTestApp(t))
	if err != nil {
		t.Fatal(err)
	}
	u := p.Uses("")
	for _, imp := range []string{"platform/icloud", "platform/location", "@test/sync"} {
		if !u.Import(imp) {
			t.Errorf("no import %s: %v", imp, u.ImportList())
		}
	}
	for _, r := range [][2]string{{"platform/icloud", "remove"}, {"platform/location", "current"}, {"@test/sync", "name"},
		{"platform/location", "requestAlways"}, {"platform/icloud", "readFile"}, {"", "Text"},
		{"platform/notify", "Notification.from"}, {"platform/notify", "Person"}, {"platform/notify", "Notification.sender"},
		{"platform/notify", "Notification.body"}, {"platform/notify", "Person.name"},
		{"platform/notify", "Interruption.critical"}, {"platform/notify", "Interruption.timeSensitive"}, {"", "Video.pictureInPicture"}} {
		if !u.Ref(r[0], r[1]) {
			t.Errorf("no %s.%s in %v", r[0], r[1], u.Refs)
		}
	}
	for _, r := range [][2]string{{"platform/icloud", "delete"}, {"platform/icloud", "documents"}, {"", "delete"}, {"", "clear"},
		{"platform/notify", "Notification.title"}, {"platform/notify", "Person.bot"}, {"platform/notify", "Interruption.passive"}, {"", "Video.loop"}} {
		if u.Ref(r[0], r[1]) {
			t.Errorf("%s.%s in %v", r[0], r[1], u.Refs)
		}
	}
	// What std's packages call of each other counts as theirs: platform/icloud
	// calls platform/host.
	if !u.Ref("platform/host", "call") {
		t.Errorf("no platform/host.call in %v", u.Refs)
	}
	if std := p.Std(""); std["platform/icloud"] == "" || std["platform/host"] == "" {
		t.Errorf("std %v", std)
	}
}

// A Window that isn't an app block's main window is a window of the
// app's own (SecondaryWindow): an app block's first Window is its main
// window, which doesn't count; one a component opens does.
func TestSecondaryWindow(t *testing.T) {
	cases := []struct {
		src  string
		want bool
	}{
		{"app {\n  Window {\n    title: \"Notes\"\n    Text(\"hi\")\n  }\n}\n", false},
		{"app {\n  Text(\"hi\")\n}\n", false},
		{`state open = false

app {
  Window {
    title: "Notes"
    Inspector()
  }
}

component Inspector {
  Column {
    Button("Inspector") { on press { open = true } }
    Window {
      bind open: open
      stateKey: "inspector"
      Text("hi")
    }
  }
}
`, true},
	}
	for i, c := range cases {
		dir := t.TempDir()
		if err := os.WriteFile(filepath.Join(dir, "app.asuka"), []byte(c.src), 0o644); err != nil {
			t.Fatal(err)
		}
		os.WriteFile(filepath.Join(dir, "asuka.toml"), []byte("id = \"dev.asuka.windows\"\n"), 0o644)
		p, err := Load(dir)
		if err != nil {
			t.Fatal(err)
		}
		if got := p.Uses("").Ref("", SecondaryWindow); got != c.want {
			t.Errorf("%d: SecondaryWindow %v, want %v", i, got, c.want)
		}
	}
}
