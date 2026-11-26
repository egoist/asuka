package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"runtime"
	"strings"

	"github.com/egoist/asuka/internal/androidsdk"
	"github.com/egoist/asuka/internal/cbuild"
)

// asuka doctor says what this computer can build and run for each
// platform, and what to install for the rest, as flutter doctor and expo
// doctor do. It asks what the builds ask (the toolchains' lookups:
// newIOSToolchain, newAndroidToolchainOn, cbuild.ForTarget), so it never
// says a platform is ready that a build then finds isn't.

// doctorCheck is one thing a platform needs: found (detail says what),
// or not (fix says what to do). An optional one only adds to what the
// platform can do (an emulator to run on, a signing identity).
type doctorCheck struct {
	Name     string `json:"name"`
	OK       bool   `json:"ok"`
	Detail   string `json:"detail,omitempty"`
	Fix      string `json:"fix,omitempty"`
	Optional bool   `json:"optional,omitempty"`
}

// doctorPlatform is a platform's checks: ready when every check that
// isn't optional is.
type doctorPlatform struct {
	Platform string        `json:"platform"`
	Ready    bool          `json:"ready"`
	Checks   []doctorCheck `json:"checks"`
}

// doctorEnv is what the checks look up, so a test can give its own.
type doctorEnv struct {
	goos      string
	ios       func() (*iosToolchain, error)
	android   func() (*androidToolchain, error)
	toolchain func(cbuild.Target) (*cbuild.Toolchain, error)
	cc        func() (cbuild.Compiler, error)
	output    func(name string, args ...string) (string, error)
	look      func(name string) (string, error)
}

func localDoctorEnv() doctorEnv {
	return doctorEnv{
		goos:      runtime.GOOS,
		ios:       func() (*iosToolchain, error) { return newIOSToolchain(false) },
		android:   func() (*androidToolchain, error) { return newAndroidToolchainOn(androidsdk.Local(), false) },
		toolchain: cbuild.ForTarget,
		cc:        cbuild.Find,
		output:    output,
		look:      exec.LookPath,
	}
}

// doctorPlatforms is every platform doctor checks, in the order it says them.
var doctorPlatforms = []string{platformIOS, platformAndroid, platformMac, platformWindows, platformLinux, platformWeb}

func doctor(e doctorEnv, platforms []string) []doctorPlatform {
	var out []doctorPlatform
	for _, p := range platforms {
		var cs []doctorCheck
		switch p {
		case platformIOS:
			cs = e.doctorIOS()
		case platformAndroid:
			cs = e.doctorAndroid()
		case platformMac:
			cs = e.doctorMac()
		default:
			cs = e.doctorDesktopOrWeb(p)
		}
		ready := true
		for _, c := range cs {
			if !c.OK && !c.Optional {
				ready = false
			}
		}
		out = append(out, doctorPlatform{Platform: p, Ready: ready, Checks: cs})
	}
	return out
}

func (e doctorEnv) doctorIOS() []doctorCheck {
	if e.goos != "darwin" {
		return []doctorCheck{{Name: "a Mac", Fix: "iOS apps are built on a Mac with Xcode"}}
	}
	cs := []doctorCheck{}
	if _, err := e.ios(); err != nil {
		cs = append(cs, doctorCheck{Name: "Xcode", Fix: "install Xcode from the App Store, then open it once (" + firstLine(err.Error()) + ")"})
		return cs
	}
	v, _ := e.output("xcodebuild", "-version")
	cs = append(cs, doctorCheck{Name: "Xcode", OK: true, Detail: firstLine(v)})
	sims, err := e.output("xcrun", "simctl", "list", "devices", "available")
	n := strings.Count(sims, "iPhone")
	if err != nil || n == 0 {
		cs = append(cs, doctorCheck{Name: "a simulator", Optional: true, Fix: "add an iOS simulator runtime in Xcode › Settings › Components (asuka dev --platform ios runs on one)"})
	} else {
		cs = append(cs, doctorCheck{Name: "a simulator", OK: true, Detail: fmt.Sprintf("%d iPhone simulators", n), Optional: true})
	}
	ids, _ := e.output("security", "find-identity", "-v", "-p", "codesigning")
	if strings.Contains(ids, "Apple Development") || strings.Contains(ids, "Apple Distribution") || strings.Contains(ids, "iPhone") {
		cs = append(cs, doctorCheck{Name: "a signing identity", OK: true, Detail: "for iPhones and the App Store", Optional: true})
	} else {
		cs = append(cs, doctorCheck{Name: "a signing identity", Optional: true, Fix: "sign in with your Apple ID in Xcode › Settings › Accounts to run on an iPhone or build for the App Store"})
	}
	return cs
}

func (e doctorEnv) doctorAndroid() []doctorCheck {
	t, err := e.android()
	if err != nil {
		return []doctorCheck{{Name: "the Android SDK and NDK", Fix: firstLine(err.Error()) + " (Android Studio › SDK Manager: SDK Tools › NDK)"}}
	}
	cs := []doctorCheck{{Name: "the Android SDK and NDK", OK: true, Detail: t.sdk}}
	if p := androidsdk.JavaProblem(t.javaHome); p != "" {
		cs = append(cs, doctorCheck{Name: "Java", Fix: p})
	} else {
		cs = append(cs, doctorCheck{Name: "Java", OK: true, Detail: t.javaHome})
	}
	if avds, err := e.output(t.emulator, "-list-avds"); err == nil && strings.TrimSpace(avds) != "" {
		cs = append(cs, doctorCheck{Name: "an emulator", OK: true, Detail: strings.Join(strings.Fields(avds), ", "), Optional: true})
	} else {
		cs = append(cs, doctorCheck{Name: "an emulator", Optional: true, Fix: "make a virtual device in Android Studio › Device Manager, or plug in a phone with USB debugging on"})
	}
	return cs
}

func (e doctorEnv) doctorMac() []doctorCheck {
	if e.goos != "darwin" {
		return []doctorCheck{{Name: "a Mac", Fix: "Mac apps are built on a Mac with Xcode"}}
	}
	cs := []doctorCheck{}
	if _, err := e.toolchain(cbuild.Target{OS: "darwin", Arch: runtime.GOARCH}); err != nil {
		cs = append(cs, doctorCheck{Name: "a C compiler", Fix: "xcode-select --install (" + firstLine(err.Error()) + ")"})
	} else {
		cs = append(cs, doctorCheck{Name: "a C compiler", OK: true, Detail: "Xcode's"})
	}
	if _, err := e.output("xcodebuild", "-version"); err != nil {
		cs = append(cs, doctorCheck{Name: "Xcode", Fix: "install Xcode from the App Store (the host is Swift on AppKit)"})
	} else {
		cs = append(cs, doctorCheck{Name: "Xcode", OK: true})
	}
	return cs
}

// doctorDesktopOrWeb: Windows, Linux and the web are built with their
// target's C toolchain (this computer's for its own system, Zig's to
// cross-compile and for the web's WebAssembly).
func (e doctorEnv) doctorDesktopOrWeb(p string) []doctorCheck {
	t := cbuild.Target{OS: p, Arch: runtime.GOARCH}
	if p == platformWeb {
		t = cbuild.Target{OS: "wasm", Arch: "wasm32"}
	}
	if t.Arch == "arm64" && p == platformWindows {
		t.Arch = "amd64" // what a Windows build makes by default
	}
	tc, err := e.toolchain(t)
	if err != nil {
		fix := firstLine(err.Error())
		if strings.Contains(fix, "zig") || strings.Contains(fix, "Zig") {
			fix = "install Zig (https://ziglang.org/download), which builds for " + map[string]string{platformWeb: "the web", platformWindows: "Windows", platformLinux: "Linux"}[p] + " from any system"
		}
		return []doctorCheck{{Name: "a C toolchain", Fix: fix}}
	}
	detail := "this computer's"
	if tc.CC.IsZig() {
		detail = "Zig's"
	}
	cs := []doctorCheck{{Name: "a C toolchain", OK: true, Detail: detail}}
	if p == platformLinux && e.goos == "linux" {
		// Running here needs GTK 4 and libadwaita (opened at run time).
		if out, err := e.output("sh", "-c", "ldconfig -p 2>/dev/null | grep -E 'libgtk-4\\.so|libadwaita-1\\.so'"); err == nil && strings.Contains(out, "libgtk-4") && strings.Contains(out, "libadwaita") {
			cs = append(cs, doctorCheck{Name: "GTK 4 and libadwaita", OK: true, Optional: true})
		} else {
			cs = append(cs, doctorCheck{Name: "GTK 4 and libadwaita", Optional: true, Fix: "install libgtk-4 and libadwaita-1 (apt install libadwaita-1-0) to run Linux apps here"})
		}
	}
	return cs
}

func firstLine(s string) string {
	s, _, _ = strings.Cut(strings.TrimSpace(s), "\n")
	return strings.TrimPrefix(s, "cbuild: ")
}

// writeDoctor says the report: a line a platform, then its checks.
func writeDoctor(w io.Writer, ps []doctorPlatform) {
	names := map[string]string{platformIOS: "iOS", platformAndroid: "Android", platformMac: "macOS", platformWindows: "Windows", platformLinux: "Linux", platformWeb: "Web"}
	for _, p := range ps {
		mark := "✓"
		if !p.Ready {
			mark = "✗"
		}
		fmt.Fprintf(w, "%s %s\n", mark, names[p.Platform])
		for _, c := range p.Checks {
			m := "  ✓"
			if !c.OK {
				m = "  ✗"
				if c.Optional {
					m = "  !"
				}
			}
			line := m + " " + c.Name
			if c.OK && c.Detail != "" {
				line += ": " + c.Detail
			}
			if !c.OK && c.Fix != "" {
				line += ": " + c.Fix
			}
			fmt.Fprintln(w, line)
		}
	}
}

func cmdDoctor(args []string) error {
	var platform string
	var asJSON bool
	fs := newFlagSet("doctor", "")
	fs.about = "Says what this computer can build and run for each platform, and what to install for the rest."
	platformFlag(fs, &platform, doctorPlatforms, "check this `platform` alone; asuka doctor fails if it isn't ready (for CI)")
	fs.Bool(&asJSON, "json", 0, "the report as JSON")
	if err := fs.parse(args); err != nil {
		return err
	}
	ps := doctorPlatforms
	if platform != "" {
		ps = []string{platform}
	}
	report := doctor(localDoctorEnv(), ps)
	if asJSON {
		b, _ := json.MarshalIndent(report, "", "  ")
		fmt.Println(string(b))
	} else {
		writeDoctor(os.Stdout, report)
	}
	if platform != "" && !report[0].Ready {
		return errors.New("asuka: " + platform + " isn't ready to build on this computer")
	}
	return nil
}
