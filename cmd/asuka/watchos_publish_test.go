package main

import (
	"archive/zip"
	"bytes"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// asuka publish --platform watchos: App Store Connect's flags, as iOS's;
// Google Play's and the desktops' refused.
func TestPublishWatchOSFlags(t *testing.T) {
	for _, c := range []struct {
		args []string
		err  string
	}{
		{[]string{"-p", "watchos"}, ""},
		{[]string{"-p", "watchos", "--groups", "Friends", "--notes", "New", "--review", "--release", "auto", "--profile", "Counter Watch"}, ""},
		{[]string{"-p", "watchos", "--file", "dist/watchos/Counter.ipa"}, ""},
		{[]string{"-p", "watchos", "--no-wait", "--build", "12"}, ""},
		{[]string{"-p", "watchos", "--file", "Counter.aab"}, "a .ipa"},
		{[]string{"-p", "watchos", "--track", "beta"}, "--track is for --platform android"},
		{[]string{"-p", "watchos", "--draft"}, "--draft is for --platform android"},
		{[]string{"-p", "watchos", "--sign", "Developer ID Application: X"}, "--sign is for --platform mac, windows or linux"},
	} {
		var o publishOptions
		fs := publishFlags(&o)
		if err := fs.parse(c.args); err != nil {
			t.Fatalf("%q: %v", c.args, err)
		}
		err := o.check(fs.set)
		if c.err == "" && err != nil || c.err != "" && (err == nil || !strings.Contains(err.Error(), c.err)) {
			t.Errorf("%q: %v, want %q", c.args, err, c.err)
		}
	}
	// A watch-only app is an iOS app's record on App Store Connect.
	if ascPlatform(platformWatchOS) != "IOS" || !appStorePlatform(platformWatchOS) {
		t.Errorf("watchOS's App Store Connect platform: %s", ascPlatform(platformWatchOS))
	}
}

// writeWatchIPA writes an .ipa of an iPhone app (stub's Info.plist) with
// a watch app (watch's; nil: none) in its Watch folder.
func writeWatchIPA(t *testing.T, path string, stub, watch map[string]any) {
	t.Helper()
	var buf bytes.Buffer
	zw := zip.NewWriter(&buf)
	w, _ := zw.Create("Payload/Counter.app/Counter")
	w.Write([]byte("stub"))
	w, _ = zw.Create("Payload/Counter.app/Info.plist")
	w.Write([]byte(plist(stub)))
	if watch != nil {
		w, _ = zw.Create("Payload/Counter.app/Watch/Counter.app/AsukaApp")
		w.Write([]byte("binary"))
		w, _ = zw.Create("Payload/Counter.app/Watch/Counter.app/Info.plist")
		w.Write([]byte(plist(watch)))
	}
	zw.Close()
	if err := os.WriteFile(path, buf.Bytes(), 0o644); err != nil {
		t.Fatal(err)
	}
}

// A watch-only app goes up as Xcode's export of one does: the iPhone
// stub's .ipa, with the watch app in it, as the record's iOS platform; its
// builds, versions, TestFlight groups and review the iOS platform's.
func TestPublishWatchOS(t *testing.T) {
	c, f := testASC(t)
	c.platform = ascPlatform(platformWatchOS)
	app := testApp(t)
	var built int
	build, _ := fakeBuild(t, 2500, ".ipa", &built)
	o := &publishOptions{groups: "friends", review: true}
	o.platform = platformWatchOS
	if err := publishIOS(c, app, o, map[string]string{"": "On the wrist."}, build); err != nil {
		t.Fatalf("%v\nrequests: %v", err, f.log)
	}
	if built != 9 {
		t.Fatalf("built %d, want 9", built)
	}
	if a := f.attrs("POST /v1/buildUploads", 0); a["platform"] != "IOS" || a["cfBundleVersion"] != "9" {
		t.Fatalf("buildUploads %v", a)
	}
	if a := f.attrs("POST /v1/buildUploadFiles", 0); a["uti"] != "com.apple.ipa" {
		t.Fatalf("buildUploadFiles %v", a)
	}
	if a := f.attrs("POST /v1/reviewSubmissions", 0); a["platform"] != "IOS" {
		t.Fatalf("review submission %v", a)
	}
	if f.count("POST /v1/betaAppReviewSubmissions") != 1 {
		t.Fatal("no beta review for the external group")
	}
	for _, q := range f.queries {
		if strings.Contains(q, "TV_OS") || strings.Contains(q, "WATCH") {
			t.Fatalf("a query for another platform: %s", q)
		}
	}
}

// --file: a watch-only app's .ipa for --platform watchos; an iPhone app,
// with a watch app in it or not, is refused there, and a watch-only app's
// stub is refused for --platform ios.
func TestPublishWatchOSFile(t *testing.T) {
	dir := t.TempDir()
	stub := map[string]any{"CFBundleIdentifier": "com.example.notes", "CFBundleShortVersionString": "1.2", "CFBundleVersion": "42",
		"DTPlatformName": "iphoneos", "ITSWatchOnlyContainer": true, "LSApplicationLaunchProhibited": true}
	phone := map[string]any{"CFBundleIdentifier": "com.example.notes", "CFBundleShortVersionString": "1.2", "CFBundleVersion": "42",
		"DTPlatformName": "iphoneos"}
	watch := map[string]any{"CFBundleIdentifier": "com.example.notes.watchkitapp", "CFBundleShortVersionString": "1.2", "CFBundleVersion": "42",
		"DTPlatformName": "watchos", "WKApplication": true}
	simWatch := map[string]any{"CFBundleIdentifier": "com.example.notes.watchkitapp", "DTPlatformName": "watchsimulator"}
	watchOnly, phoneIPA, companion, simulator := filepath.Join(dir, "w.ipa"), filepath.Join(dir, "p.ipa"), filepath.Join(dir, "c.ipa"), filepath.Join(dir, "s.ipa")
	writeWatchIPA(t, watchOnly, stub, watch)
	writeWatchIPA(t, phoneIPA, phone, nil)
	writeWatchIPA(t, companion, phone, watch)
	writeWatchIPA(t, simulator, stub, simWatch)
	info, err := readIPAInfo(watchOnly)
	if err != nil || info.sdk != "iphoneos" || info.watch != "watchos" || !info.watchOnly || info.id != "com.example.notes" {
		t.Fatalf("%+v %v", info, err)
	}
	if info, err := readIPAInfo(companion); err != nil || info.watch != "watchos" || info.watchOnly {
		t.Fatalf("an iPhone app with its watch app: %+v %v", info, err)
	}
	never := func(*asukaApp, *buildOptions) (string, error) { t.Fatal("built"); return "", nil }
	watchOpts := func(file string) *publishOptions {
		o := &publishOptions{file: file, noWait: true}
		o.platform = platformWatchOS
		return o
	}

	c, f := testASC(t)
	c.platform = ascPlatform(platformWatchOS)
	if err := publishIOS(c, testApp(t), watchOpts(watchOnly), nil, never); err != nil {
		t.Fatal(err)
	}
	if a := f.attrs("POST /v1/buildUploads", 0); a["platform"] != "IOS" || a["cfBundleVersion"] != "42" {
		t.Fatalf("%v", a)
	}
	for file, want := range map[string]string{
		phoneIPA:  "isn't a watch-only app",
		companion: "publish it with --platform ios",
		simulator: "watchsimulator's SDK",
	} {
		c, f := testASC(t)
		err := publishIOS(c, testApp(t), watchOpts(file), nil, never)
		if err == nil || !strings.Contains(err.Error(), want) {
			t.Errorf("%s for watchOS: %v, want %q", filepath.Base(file), err, want)
		}
		if f.count("POST /v1/buildUploads") != 0 {
			t.Errorf("%s: uploaded", filepath.Base(file))
		}
	}
	// A watch-only app isn't an iPhone app; an iPhone app that carries its
	// watch app is.
	c, _ = testASC(t)
	err = publishIOS(c, testApp(t), &publishOptions{file: watchOnly, noWait: true}, nil, never)
	if err == nil || !strings.Contains(err.Error(), "--platform watchos") {
		t.Fatalf("a watch-only app for iOS: %v", err)
	}
	c, _ = testASC(t)
	if err := publishIOS(c, testApp(t), &publishOptions{file: companion, noWait: true}, nil, never); err != nil {
		t.Fatalf("an iPhone app with its watch app: %v", err)
	}
}
