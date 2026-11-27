package main

import (
	"encoding/json"
	"os"
	"path/filepath"
	"runtime"
	"slices"
	"strings"
	"testing"

	iupdate "github.com/egoist/asuka/internal/update"
	"github.com/egoist/asuka/lang/target"
)

// An Apple Watch build's updates are its own: its fingerprint is of the
// watch's native side (its own host, with fewer optional parts), not
// the iPhone's or the Apple TV's, and its manifest is named for watchOS.
func TestOTAFingerprintWatchOS(t *testing.T) {
	pub, _ := testKeys(t)
	t.Setenv("ASUKA_UPDATE_PUBLIC_KEY", "")
	t.Setenv("ASUKA_UPDATE_URL", "")
	dir := t.TempDir()
	toml := "name = \"Watch\"\nid = \"dev.asuka.watch\"\nversion = \"1.0.0\"\n\n[updates]\npublicKey = \"" + pub + "\"\nurl = \"https://example.com/ota\"\n"
	writeFiles(t, dir, map[string]string{"asuka.toml": toml, "app.asuka": "app { Text(\"A\") }\n"})
	phone, _ := otaFingerprintOf(t, dir, platformIOS)
	tv, _ := otaFingerprintOf(t, dir, platformTVOS)
	watch, feed := otaFingerprintOf(t, dir, platformWatchOS)
	if watch == phone || watch == tv {
		t.Fatal("watchOS's fingerprint is iOS's or tvOS's")
	}
	if want := "https://example.com/ota/update-watchos-" + watch[:16] + ".json"; feed != want {
		t.Errorf("watchOS's feed %q, want %q", feed, want)
	}
	// A change of code: the same builds take it.
	writeFiles(t, dir, map[string]string{"app.asuka": "app { Text(\"B\") }\n"})
	if p, _ := otaFingerprintOf(t, dir, platformWatchOS); p != watch {
		t.Error("a change of code changed watchOS's fingerprint")
	}
	// The watch's host has no player or web view: a Video or a WebView (which
	// it draws nothing for) is no other native side.
	writeFiles(t, dir, map[string]string{"app.asuka": "app { Video(\"https://example.com/a.mp4\") }\n"})
	if p, _ := otaFingerprintOf(t, dir, platformIOS); p == phone {
		t.Error("a Video kept iOS's fingerprint")
	}
	if p, _ := otaFingerprintOf(t, dir, platformWatchOS); p != watch {
		t.Error("a Video changed watchOS's fingerprint, whose host has no player to add")
	}
	// What a watch build goes without (iPhone widgets, shortcuts, a share
	// target) is out of its fingerprint: the build drops it before it
	// asks, the update doesn't.
	writeFiles(t, dir, map[string]string{
		"asuka.toml": toml + "\n[[widgets]]\nkind = \"today\"\nsizes = [\"small\"]\n\n[[shortcuts]]\nid = \"new\"\ntitle = \"New\"\nurl = \"w://new\"\n",
		"app.asuka":  "app { Text(\"A\") }\n",
	})
	app, err := loadApp(dir, platformWatchOS)
	if err != nil {
		t.Fatal(err)
	}
	m, err := app.compile(target.IOS)
	if err != nil {
		t.Fatal(err)
	}
	before, err := app.otaFingerprint(platformWatchOS, m)
	if err != nil {
		t.Fatal(err)
	}
	if len(app.cfg.Widgets) == 0 || len(app.cfg.Shortcuts) == 0 {
		t.Fatal("the fingerprint dropped the app's own widgets or shortcuts")
	}
	app.cfg.dropWatchExtensions()
	after, err := app.otaFingerprint(platformWatchOS, m)
	if err != nil {
		t.Fatal(err)
	}
	if before != after || before != watch {
		t.Errorf("watchOS's fingerprint with widgets %s, without %s, want %s", before[:16], after[:16], watch[:16])
	}
	// The host's parts the watch has (notifications, pushes) are in it;
	// those it hasn't aren't.
	if got := watchFeatures([]string{"ASUKA_NOTIFY", "ASUKA_VIDEO", "ASUKA_PUSH", "ASUKA_WEBVIEW"}); !slices.Equal(got, []string{"ASUKA_NOTIFY", "ASUKA_PUSH"}) {
		t.Errorf("the watch's host parts: %v", got)
	}
	// A complication is the watch's: a widget with a watch size is in its
	// fingerprint (the build keeps it).
	writeFiles(t, dir, map[string]string{"asuka.toml": toml + "\n[[widgets]]\nkind = \"today\"\nsizes = [\"circular\"]\n"})
	if p, _ := otaFingerprintOf(t, dir, platformWatchOS); p == watch {
		t.Error("a complication kept watchOS's fingerprint")
	}
}

// asuka build --platform watchos --update writes the watch's update, in
// dist/watchos/updates, for the watch's builds.
func TestBuildOTAUpdateWatchOS(t *testing.T) {
	pub, priv := testKeys(t)
	t.Setenv("ASUKA_UPDATE_PUBLIC_KEY", "")
	t.Setenv("ASUKA_UPDATE_URL", "")
	t.Setenv("ASUKA_UPDATE_PRIVATE_KEY", priv)
	dir := t.TempDir()
	writeFiles(t, dir, map[string]string{
		"asuka.toml": "name = \"Watch\"\nid = \"dev.asuka.watch\"\nversion = \"1.0.0\"\n\n[updates]\npublicKey = \"" + pub + "\"\nurl = \"https://example.com/ota\"\n",
		"app.asuka":  "app { Text(\"A\") }\n",
	})
	if err := cmdBuild([]string{"--platform", platformWatchOS, "--update", dir}); err != nil {
		t.Fatal(err)
	}
	fp, _ := otaFingerprintOf(t, dir, platformWatchOS)
	b, err := os.ReadFile(filepath.Join(dir, "dist", "watchos", "updates", "update-watchos-"+fp[:16]+".json"))
	if err != nil {
		t.Fatal(err)
	}
	var m iupdate.Manifest
	if err := json.Unmarshal(b, &m); err != nil || m.Fingerprint != fp || !strings.Contains(m.URL, "-watchos-"+fp[:16]+"-") {
		t.Errorf("manifest %+v (%v): want the fingerprint %s", m, err, fp)
	}
}

// asuka publish --platform watchos --update signs the watch's update and
// uploads it as iOS's is: the module, then its manifest,
// update-watchos-<fp>.json, to the version's release.
func TestPublishOTAUpdateWatchOS(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("a shell script as gh")
	}
	pub, priv := testKeys(t)
	t.Setenv("ASUKA_UPDATE_PUBLIC_KEY", "")
	t.Setenv("ASUKA_UPDATE_URL", "")
	t.Setenv("ASUKA_UPDATE_PRIVATE_KEY", priv)
	dir := t.TempDir()
	writeFiles(t, dir, map[string]string{
		"asuka.toml": "name = \"Watch\"\nid = \"dev.asuka.watch\"\nversion = \"1.2.0\"\n\n[updates]\npublicKey = \"" + pub + "\"\ngithub = \"me/watch\"\n",
		"app.asuka":  "app { Text(\"A\") }\n",
	})
	bin := t.TempDir()
	log := filepath.Join(bin, "log")
	script := "#!/bin/sh\necho \"$*\" >> " + log + "\ncase \"$1 $2\" in \"release view\") exit 1;; esac\n"
	if err := os.WriteFile(filepath.Join(bin, "gh"), []byte(script), 0o755); err != nil {
		t.Fatal(err)
	}
	defer func(c string) { ghCommand = c }(ghCommand)
	ghCommand = filepath.Join(bin, "gh")
	if err := cmdPublish([]string{"--platform", platformWatchOS, "--update", dir}); err != nil {
		t.Fatal(err)
	}
	fp, feed := otaFingerprintOf(t, dir, platformWatchOS)
	if want := "https://github.com/me/watch/releases/latest/download/update-watchos-" + fp[:16] + ".json"; feed != want {
		t.Errorf("the build's feed %q, want %q", feed, want)
	}
	updates := filepath.Join(dir, "dist", "watchos", "updates")
	modules, _ := filepath.Glob(filepath.Join(updates, "*-watchos-"+fp[:16]+"-*.asukab"))
	manifest := filepath.Join(updates, "update-watchos-"+fp[:16]+".json")
	if len(modules) != 1 {
		t.Fatalf("modules %q in %s", modules, updates)
	}
	b, _ := os.ReadFile(log)
	want := "release view v1.2.0 --repo me/watch\n" +
		"release create v1.2.0 --repo me/watch --draft --title v1.2.0 --notes \n" +
		"release upload v1.2.0 --repo me/watch --clobber " + modules[0] + "\n" +
		"release upload v1.2.0 --repo me/watch --clobber " + manifest + "\n" +
		"release edit v1.2.0 --repo me/watch --draft=false\n"
	if string(b) != want {
		t.Errorf("gh ran:\n%s\nwant\n%s", b, want)
	}
}
