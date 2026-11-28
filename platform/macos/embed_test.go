package macos

import (
	"bytes"
	"os"
	"testing"
)

// The Swift the iOS host and the Mac's share is one file each in both:
// typed native modules' runtime, iCloud's, background URLSessions'
// events (packages' sessions, by identifier), and the ops
// decoder (go generate ./ops writes both copies of Ops.swift).
func TestSwiftShared(t *testing.T) {
	for mac, ios := range map[string]string{
		"Sources/AsukaHost/NativeModules.swift":      "Sources/AsukaHost/NativeModules.swift",
		"Sources/AsukaHost/ICloud.swift":             "Sources/AsukaHost/ICloud.swift",
		"Sources/AsukaHost/BackgroundSessions.swift": "Sources/AsukaHost/BackgroundSessions.swift",
		"Sources/AsukaOps/Ops.swift":                 "Sources/AsukaOps/Ops.swift",
		"Sources/AsukaOps/Actions.swift":             "Sources/AsukaOps/Actions.swift",
		"Sources/AsukaOps/AlertFields.swift":         "Sources/AsukaOps/AlertFields.swift",
		"Sources/AsukaOps/Keys.swift":                "Sources/AsukaOps/Keys.swift",
		"Sources/AsukaOps/Sidebar.swift":             "Sources/AsukaOps/Sidebar.swift",
	} {
		a, err := os.ReadFile("../ios/" + ios)
		if err != nil {
			t.Fatal(err)
		}
		b, err := Files.ReadFile(mac)
		if err != nil {
			t.Fatal(err)
		}
		if !bytes.Equal(a, b) {
			t.Errorf("platform/macos/%s isn't the iOS host's platform/ios/%s: copy it", mac, ios)
		}
	}
}

// The hosts' copies of the core's header are the core's.
func TestHeader(t *testing.T) {
	core, err := os.ReadFile("../../core/include/asuka.h")
	if err != nil {
		t.Fatal(err)
	}
	for _, p := range []string{"Sources/CAsuka/include/asuka.h", "../ios/Sources/CAsuka/include/asuka.h"} {
		var b []byte
		if p[0] == '.' {
			b, err = os.ReadFile(p)
		} else {
			b, err = Files.ReadFile(p)
		}
		if err != nil {
			t.Fatal(err)
		}
		if !bytes.Equal(core, b) {
			t.Errorf("%s isn't core/include/asuka.h: copy it", p)
		}
	}
}

// The host's copy of the desktop hosts' shared header is theirs: the
// host links that C (the CLI's MacShared; shared_*.c in its tests).
func TestDesktopHeader(t *testing.T) {
	want, err := os.ReadFile("../desktop/include/desktop.h")
	if err != nil {
		t.Fatal(err)
	}
	got, err := Files.ReadFile("Sources/CAsuka/include/desktop.h")
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(want, got) {
		t.Error("platform/macos/Sources/CAsuka/include/desktop.h isn't platform/desktop/include/desktop.h: copy it")
	}
	for _, f := range []string{"jsontree.c", "files.c", "state.c", "flyout.c", "words.c", "words_gen.c"} {
		b, err := Files.ReadFile("Sources/CAsuka/shared_" + f)
		if err != nil || !bytes.Contains(b, []byte(`#include "../../../desktop/src/`+f+`"`)) {
			t.Errorf("Sources/CAsuka/shared_%s doesn't include platform/desktop's %s: %v", f, f, err)
		}
	}
}
