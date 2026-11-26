package main

import (
	"os"
	"path/filepath"
	"slices"
	"strings"
	"testing"
)

// An Xcode team profile's iCloud: wildcards, CloudKit's, two containers.
func icloudProfile() map[string]any {
	return map[string]any{
		entKVStore:       "ABCDE12345.*",
		entServices:      "*",
		entContainers:    []any{"iCloud.dev.asuka.notes", "iCloud.dev.asuka.shared"},
		entUbiquity:      []any{"iCloud.dev.asuka.notes", "iCloud.dev.asuka.shared"},
		entDevContainers: []any{"iCloud.dev.asuka.notes"},
		entEnvironment:   "Production",
	}
}

// The profile's iCloud entitlements are signed as the app uses them: the
// store's identifier the team's and the app's, its containers, the
// documents' service; what it doesn't use (CloudKit's, the documents' for
// an app of the store alone, everything for an extension) goes.
func TestNarrowICloud(t *testing.T) {
	both := &appConfig{ID: "dev.asuka.notes"}
	both.addICloud(true, true)
	set, remove, err := narrowICloud(icloudProfile(), both, "ABCDE12345")
	if err != nil {
		t.Fatal(err)
	}
	if set[entKVStore] != "ABCDE12345.dev.asuka.notes" || !slices.Equal(set[entServices].([]string), []string{"CloudDocuments"}) ||
		!slices.Equal(set[entContainers].([]string), []string{"iCloud.dev.asuka.notes"}) || !slices.Equal(set[entUbiquity].([]string), []string{"iCloud.dev.asuka.notes"}) {
		t.Errorf("set %v", set)
	}
	slices.Sort(remove)
	if !slices.Equal(remove, []string{entDevContainers, entEnvironment}) {
		t.Errorf("removed %v", remove)
	}

	kv := &appConfig{ID: "dev.asuka.notes"}
	kv.addICloud(true, false)
	set, remove, _ = narrowICloud(icloudProfile(), kv, "ABCDE12345")
	if len(set) != 1 || len(remove) != 5 {
		t.Errorf("the store alone: set %v, removed %v", set, remove)
	}
	if set, remove, _ := narrowICloud(icloudProfile(), nil, "ABCDE12345"); len(set) != 0 || len(remove) != 6 {
		t.Errorf("an extension: set %v, removed %v", set, remove)
	}

	// A container the profile hasn't, a profile without iCloud, another
	// app's store.
	other := &appConfig{ID: "dev.asuka.notes"}
	other.ICloud.Containers = []string{"iCloud.dev.asuka.other"}
	other.addICloud(false, true)
	if _, _, err := narrowICloud(icloudProfile(), other, "ABCDE12345"); err == nil || !strings.Contains(err.Error(), "the iCloud container iCloud.dev.asuka.other") {
		t.Errorf("another container: %v", err)
	}
	if _, _, err := narrowICloud(map[string]any{}, both, "ABCDE12345"); err == nil || !strings.Contains(err.Error(), "key-value storage") || !strings.Contains(err.Error(), "iCloud Documents") {
		t.Errorf("no iCloud: %v", err)
	}
	if _, _, err := narrowICloud(map[string]any{entKVStore: "ABCDE12345.dev.asuka.else"}, kv, "ABCDE12345"); err == nil {
		t.Error("another app's store: no error")
	}
}

// The documents' first container shows in iCloud Drive, named after the
// app, or not at all; a simulator build's entitlements have the team's
// prefix (the fake one without ios.team).
func TestICloudPlistAndSimulator(t *testing.T) {
	c := &appConfig{ID: "dev.asuka.notes", displayName: "Notes"}
	if c.ubiquitousContainers() != nil {
		t.Error("containers without documents")
	}
	c.addICloud(true, true)
	u := c.ubiquitousContainers()["iCloud.dev.asuka.notes"].(map[string]any)
	if u["NSUbiquitousContainerIsDocumentScopePublic"] != true || u["NSUbiquitousContainerName"] != "Notes" {
		t.Errorf("containers %v", u)
	}
	if _, ok := c.plistExtras()["NSUbiquitousContainers"]; !ok {
		t.Error("not in Info.plist")
	}
	c.ICloud.DriveHidden, c.ICloud.DriveName = true, "My Notes"
	u = c.ubiquitousContainers()["iCloud.dev.asuka.notes"].(map[string]any)
	if u["NSUbiquitousContainerIsDocumentScopePublic"] != false || u["NSUbiquitousContainerName"] != "My Notes" {
		t.Errorf("hidden %v", u)
	}

	work := t.TempDir()
	if _, err := simulatorEntitlements(work, c); err != nil {
		t.Fatal(err)
	}
	b, _ := os.ReadFile(filepath.Join(work, "simulator.entitlements"))
	if !strings.Contains(string(b), "<string>"+simulatorTeam+".dev.asuka.notes</string>") || !strings.Contains(string(b), "<string>CloudDocuments</string>") {
		t.Errorf("simulator entitlements:\n%s", b)
	}
	c.IOS.Team = "ABCDE12345"
	simulatorEntitlements(work, c)
	if b, _ := os.ReadFile(filepath.Join(work, "simulator.entitlements")); !strings.Contains(string(b), "<string>ABCDE12345.dev.asuka.notes</string>") {
		t.Errorf("with a team:\n%s", b)
	}
}

// usesApp writes an app (with a dependency, @test/sync, beside it) whose
// sources are app's and dep's, and loads it as asuka build does.
func usesApp(t *testing.T, app, dep string) *asukaApp {
	t.Helper()
	root := t.TempDir()
	dir, sync := filepath.Join(root, "app"), filepath.Join(root, "sync")
	os.MkdirAll(dir, 0o755)
	os.MkdirAll(sync, 0o755)
	os.WriteFile(filepath.Join(dir, "app.asuka"), []byte(app), 0o644)
	os.WriteFile(filepath.Join(dir, "asuka.toml"), []byte("id = \"dev.asuka.journal\"\n\n[dependencies]\n\"@test/sync\" = \"file:../sync\"\n"), 0o644)
	os.WriteFile(filepath.Join(sync, "sync.asuka"), []byte("pub fn name() -> string = \"sync\"\n"+dep), 0o644)
	os.WriteFile(filepath.Join(sync, "asuka.toml"), []byte("name = \"@test/sync\"\nversion = \"0.1.0\"\n"), 0o644)
	return projectApp(t, dir, nil)
}

// What the program refers to of platform/icloud says what the app's given,
// by the members its manifest names: the store's (remove), the
// documents' (writeFile, a dependency's readFile); not a local function
// that's named as one of them (delete), nor a mere import.
func TestUseICloud(t *testing.T) {
	const local = "\nfn delete(x: string) -> string = x\n\napp { Text(delete(sync.name())) }\n"
	for _, x := range []struct {
		name, app, dep string
		kv, docs       bool
	}{
		{"store", "import platform/icloud\nimport @test/sync\n\nasync fn clear() {\n  let _ = try? await icloud.remove(\"k\")\n}\n" + local, "", true, false},
		{"documents", "import platform/icloud\nimport @test/sync\n\nasync fn save() {\n  let _ = try? await icloud.writeFile(\"a\", \"b\".bytes())\n}\n" + local, "", false, true},
		{"a dependency's", "import @test/sync\n" + local, "import platform/icloud\n\npub async fn pull() throws -> bytes = try await icloud.readFile(\"notes.txt\")\n", false, true},
		{"a local delete", "import platform/icloud\nimport @test/sync\n\nfn account() -> string = \"me\"\n" + local, "", false, false},
	} {
		c := usesApp(t, x.app, x.dep).cfg
		if c.icloudKV != x.kv || c.icloudDocs != x.docs {
			t.Errorf("%s: kv %v, docs %v, %v", x.name, c.icloudKV, c.icloudDocs, c.entitlements)
		}
		if _, ok := c.entitlements[entKVStore]; ok != x.kv {
			t.Errorf("%s: the store's entitlement %v", x.name, c.entitlements)
		}
		if _, ok := c.entitlements[entContainers]; ok != x.docs {
			t.Errorf("%s: the containers' %v", x.name, c.entitlements)
		}
	}
}

// platform/location's background part is the apps' that call it: one that
// asks where the device is has none of it; one whose dependency asks for
// Always has its permission and purpose string.
func TestLocationByUse(t *testing.T) {
	const app = "import platform/location\nimport @test/sync\n\nasync fn here() {\n  let _ = try? await location.current()\n}\n\napp { Text(sync.name()) }\n"
	fg := usesApp(t, app, "").cfg
	man := fg.androidManifest(false)
	if !strings.Contains(man, "ACCESS_FINE_LOCATION") || strings.Contains(man, "BACKGROUND_LOCATION") || strings.Contains(man, "LocationService") ||
		fg.IOS.Usage["NSLocationAlwaysAndWhenInUseUsageDescription"] != "" || slices.Contains(fg.IOS.BackgroundModes, "location") {
		t.Errorf("current() alone: %v %v\n%s", fg.IOS.Usage, fg.IOS.BackgroundModes, man)
	}
	bg := usesApp(t, app, "import platform/location\n\npub async fn always() throws {\n  let _ = try await location.requestAlways()\n}\n").cfg
	if man := bg.androidManifest(false); !strings.Contains(man, "android.permission.ACCESS_BACKGROUND_LOCATION") ||
		bg.IOS.Usage["NSLocationAlwaysAndWhenInUseUsageDescription"] == "" {
		t.Errorf("a dependency's requestAlways(): %v\n%s", bg.IOS.Usage, man)
	}
}

// On the Mac, the profile's iCloud is signed as the app uses it.
func TestMacICloudEntitlements(t *testing.T) {
	cfg := &appConfig{ID: "dev.asuka.notes"}
	cfg.addICloud(true, true)
	if got := macProfileNeeds(cfg); len(got) != 2 {
		t.Errorf("needs: %v", got)
	}
	p, err := readMacProfile(writeTestProfile(t, "ABCDE12345.dev.asuka.*", ""))
	if err != nil {
		t.Fatal(err)
	}
	for k, v := range icloudProfile() {
		p.entitlements[k] = v
	}
	ents, err := macEntitlements(cfg, p, false)
	if err != nil {
		t.Fatal(err)
	}
	if ents[entKVStore] != "ABCDE12345.dev.asuka.notes" || !slices.Equal(ents[entServices].([]string), []string{"CloudDocuments"}) {
		t.Errorf("entitlements %v", ents)
	}
	if _, ok := ents[entEnvironment]; ok {
		t.Errorf("CloudKit's: %v", ents)
	}
	delete(p.entitlements, entKVStore)
	if _, err := macEntitlements(cfg, p, false); err == nil || !strings.Contains(err.Error(), "key-value storage") {
		t.Errorf("no store: %v", err)
	}
}

// Communication notifications are the apps' that make a message
// (notify's Notification.from, or a Notification made with its sender):
// not those that import platform/push, whose own Message has a notify.Person,
// nor those whose notifications have no sender.
func TestConversationsByUse(t *testing.T) {
	const tail = "\napp { Text(sync.name()) }\n"
	plain := usesApp(t, "import platform/push\nimport @test/sync\n\nasync fn reg() {\n  let _ = try? await push.register()\n}\n"+tail, "")
	if fl := usedFeatures(plain, plain.cfg); !slices.Contains(fl, "ASUKA_PUSH") || slices.Contains(fl, "ASUKA_CONVERSATIONS") ||
		plain.cfg.entitlements["com.apple.developer.usernotifications.communication"] != nil {
		t.Errorf("push alone: %v %v", fl, plain.cfg.entitlements)
	}
	msg := usesApp(t, "import platform/notify\nimport @test/sync\n\nasync fn say() {\n  let n = notify.Notification(body: \"hi\").from(notify.Person(name: \"Ann\"), notify.Conversation())\n  try? await notify.show(n)\n}\n"+tail, "")
	if fl := usedFeatures(msg, msg.cfg); !slices.Contains(fl, "ASUKA_CONVERSATIONS") || msg.cfg.entitlements["com.apple.developer.usernotifications.communication"] != true {
		t.Errorf("a message: %v %v", fl, msg.cfg.entitlements)
	}
	lit := usesApp(t, "import platform/notify\nimport @test/sync\n\nasync fn say() {\n  try? await notify.show(notify.Notification(body: \"hi\", sender: notify.Person(name: \"Ann\")))\n}\n"+tail, "")
	if fl := usedFeatures(lit, lit.cfg); !slices.Contains(fl, "ASUKA_CONVERSATIONS") {
		t.Errorf("a message made with its sender: %v", fl)
	}
	plainNote := usesApp(t, "import platform/notify\nimport @test/sync\n\nasync fn say() {\n  try? await notify.show(notify.Notification(title: \"hi\"))\n}\n"+tail, "")
	if fl := usedFeatures(plainNote, plainNote.cfg); slices.Contains(fl, "ASUKA_CONVERSATIONS") {
		t.Errorf("a notification without a sender: %v", fl)
	}
}

// A simulator build's app identifier is its team's ID and its bundle ID,
// as a signed app's: the team's a team ID's shape (ten characters, upper
// case and digits), or the system doesn't find the app by it (HealthKit
// names it by its executable).
func TestSimulatorAppIdentifier(t *testing.T) {
	if len(simulatorTeam) != 10 || strings.Trim(simulatorTeam, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") != "" {
		t.Fatalf("%q isn't a team ID's shape", simulatorTeam)
	}
	work := t.TempDir()
	if _, err := simulatorEntitlements(work, &appConfig{ID: "dev.asuka.health"}); err != nil {
		t.Fatal(err)
	}
	b, _ := os.ReadFile(filepath.Join(work, "simulator.entitlements"))
	if want := "<key>application-identifier</key><string>" + simulatorTeam + ".dev.asuka.health</string>"; !strings.Contains(strings.Join(strings.Fields(string(b)), ""), want) {
		t.Errorf("simulator entitlements:\n%s", b)
	}
}
