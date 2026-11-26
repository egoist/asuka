package main

import (
	"bytes"
	"errors"
	"strings"
	"testing"

	"github.com/egoist/asuka/internal/cbuild"
)

// A computer without Zig, off a Mac, without Android's SDK: doctor says
// what each platform lacks and how to get it, and an optional lack (an
// emulator) leaves a platform ready.
func TestDoctor(t *testing.T) {
	e := doctorEnv{
		goos:    "linux",
		ios:     func() (*iosToolchain, error) { return nil, errors.New("no Xcode") },
		android: func() (*androidToolchain, error) { return nil, errors.New("no Android SDK: install Android Studio or set ANDROID_HOME") },
		toolchain: func(t cbuild.Target) (*cbuild.Toolchain, error) {
			if t.OS == "linux" {
				return &cbuild.Toolchain{Target: t, CC: cbuild.Compiler{Path: "/usr/bin/cc"}}, nil
			}
			return nil, errors.New("cbuild: zig isn't installed (https://ziglang.org/download)")
		},
		cc:     func() (cbuild.Compiler, error) { return cbuild.Compiler{Path: "/usr/bin/cc"}, nil },
		output: func(name string, args ...string) (string, error) { return "", errors.New("no " + name) },
		look:   func(name string) (string, error) { return "", errors.New("no " + name) },
	}
	ps := doctor(e, doctorPlatforms)
	ready := map[string]bool{}
	for _, p := range ps {
		ready[p.Platform] = p.Ready
	}
	want := map[string]bool{platformIOS: false, platformAndroid: false, platformMac: false, platformWindows: false, platformLinux: true, platformWeb: false}
	for p, r := range want {
		if ready[p] != r {
			t.Errorf("%s ready %v, want %v", p, ready[p], r)
		}
	}
	var b bytes.Buffer
	writeDoctor(&b, ps)
	out := b.String()
	for _, s := range []string{
		"✗ iOS\n  ✗ a Mac: iOS apps are built on a Mac with Xcode",
		"✗ Android\n  ✗ the Android SDK and NDK: no Android SDK: install Android Studio or set ANDROID_HOME",
		"✗ Web\n  ✗ a C toolchain: install Zig (https://ziglang.org/download), which builds for the web from any system",
		"✗ Windows\n  ✗ a C toolchain: install Zig",
		"✓ Linux\n  ✓ a C toolchain: this computer's\n  ! GTK 4 and libadwaita: install",
	} {
		if !strings.Contains(out, s) {
			t.Errorf("the report has no %q:\n%s", s, out)
		}
	}
}
