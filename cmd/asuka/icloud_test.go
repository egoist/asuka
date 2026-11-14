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
	if !strings.Contains(string(b), "<string>ASUKASIMULA.dev.asuka.notes</string>") || !strings.Contains(string(b), "<string>CloudDocuments</string>") {
		t.Errorf("simulator entitlements:\n%s", b)
	}
	c.IOS.Team = "ABCDE12345"
	simulatorEntitlements(work, c)
	if b, _ := os.ReadFile(filepath.Join(work, "simulator.entitlements")); !strings.Contains(string(b), "<string>ABCDE12345.dev.asuka.notes</string>") {
		t.Errorf("with a team:\n%s", b)
	}
}

// What an app calls says what it's given: examples/journal calls both
// parts; the player, which stands in for any app, has neither (an app of
// iCloud's is built, not run in the player).
func TestUseICloud(t *testing.T) {
	c := &appConfig{ID: "dev.asuka.journal"}
	c.useICloud("../../examples/journal", ".", []string{icloudPkg})
	if !c.icloudKV || !c.icloudDocs || c.entitlements[entKVStore] != teamPrefix+"dev.asuka.journal" {
		t.Errorf("examples/journal: kv %v, docs %v, %v", c.icloudKV, c.icloudDocs, c.entitlements)
	}
	none := &appConfig{ID: "dev.asuka.counter"}
	none.useICloud("../../examples/counter", ".", []string{"github.com/egoist/asuka/ui"})
	if none.icloudKV || none.icloudDocs || len(none.entitlements) != 0 {
		t.Errorf("examples/counter: %v", none.entitlements)
	}
	if p := playerConfig(); p.icloudKV || p.icloudDocs || p.entitlements[entKVStore] != nil {
		t.Errorf("the player: %v", p.entitlements)
	}
	for _, target := range []string{"ios", "ios-device"} {
		if b, _ := gapsOf(c, target, icloudPkg); !strings.Contains(b, "iCloud") {
			t.Errorf("%s: an app of iCloud's runs in the player (%q)", target, b)
		}
	}
	if b, _ := gapsOf(c, "android", icloudPkg); b != "" {
		t.Errorf("android: built for %q", b)
	}
	if !slices.Equal(featureMacSwift[icloudPkg], []string{"ASUKA_ICLOUD"}) {
		t.Error("no Mac Swift for iCloud")
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
