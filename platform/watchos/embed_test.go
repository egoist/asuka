package watchos

import (
	"bytes"
	"io/fs"
	"os"
	"strings"
	"testing"
)

// The Swift the watch's host shares with the iOS host is the same file in
// both: the ops decoder (go generate ./ops writes each copy of Ops.swift),
// its hand-written parts, and typed native modules' runtime.
func TestSwiftShared(t *testing.T) {
	for _, p := range []string{
		"Sources/AsukaHost/NativeModules.swift",
		"Sources/AsukaOps/Ops.swift",
		"Sources/AsukaOps/Actions.swift",
		"Sources/AsukaOps/AlertFields.swift",
		"Sources/AsukaOps/Keys.swift",
		"Sources/AsukaOps/Sidebar.swift",
	} {
		ios, err := os.ReadFile("../ios/" + p)
		if err != nil {
			t.Fatal(err)
		}
		watch, err := Files.ReadFile(p)
		if err != nil {
			t.Fatal(err)
		}
		if !bytes.Equal(ios, watch) {
			t.Errorf("platform/watchos/%s isn't the iOS host's platform/ios/%s: copy it", p, p)
		}
	}
}

// The host's copy of the core's header is the core's.
func TestHeader(t *testing.T) {
	core, err := os.ReadFile("../../core/include/asuka.h")
	if err != nil {
		t.Fatal(err)
	}
	b, err := Files.ReadFile("Sources/CAsuka/include/asuka.h")
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(core, b) {
		t.Error("platform/watchos/Sources/CAsuka/include/asuka.h isn't core/include/asuka.h: copy it")
	}
}

// SwiftUI is the watch's host's alone (AGENTS.md): no UIKit view, and no
// file of the iOS host's but those shared above.
func TestNoUIKitViews(t *testing.T) {
	fs.WalkDir(Files, "Sources/AsukaHost", func(p string, d fs.DirEntry, err error) error {
		if err != nil || d.IsDir() || p == "Sources/AsukaHost/NativeModules.swift" {
			return err
		}
		b, _ := Files.ReadFile(p)
		for _, s := range []string{"UIView", "UIViewController", "UIApplication", "WKInterface" + "Object"} {
			if strings.Contains(string(b), s) {
				t.Errorf("%s uses %s: the watch's host is SwiftUI's", p, s)
			}
		}
		return nil
	})
}
