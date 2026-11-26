package main

import (
	"os/exec"
	"path/filepath"
	"testing"
)

func TestVersionLess(t *testing.T) {
	for _, c := range []struct {
		a, b string
		less bool
	}{
		{"11.0", "13.0", true}, {"13.0", "11.0", false}, {"13", "13.1", true}, {"13.1", "13", false},
		{"16.0", "16.0", false}, {"9.0", "10.0", true}, {"11.0", "", false},
	} {
		if got := versionLess(c.a, c.b); got != c.less {
			t.Errorf("versionLess(%q, %q) = %v", c.a, c.b, got)
		}
	}
}

// An app's least macOS is its native packages' highest: platform/camera
// needs macOS 13, above the host's; platform/review asks for less, which
// leaves the host's.
func TestSwiftMinimum(t *testing.T) {
	if _, err := exec.LookPath("swift"); err != nil {
		t.Skip("no swift")
	}
	sdk := filepath.Join("..", "..", "sdk")
	review := nativePackage{path: "platform/review", dir: filepath.Join(sdk, "review")}
	camera := nativePackage{path: "platform/camera", dir: filepath.Join(sdk, "camera")}
	if got, err := swiftMinimum([]nativePackage{review}, "macos"); err != nil || got != macMin {
		t.Errorf("with review: %q, %v; want the host's %s", got, err, macMin)
	}
	if got, err := swiftMinimum([]nativePackage{review, camera}, "macos"); err != nil || got != "13.0" {
		t.Errorf("with camera: %q, %v; want 13.0", got, err)
	}
	if got, err := swiftMinimum([]nativePackage{camera}, "ios"); err != nil || got != iosMin {
		t.Errorf("camera on iOS: %q, %v; want the host's %s", got, err, iosMin)
	}
}
