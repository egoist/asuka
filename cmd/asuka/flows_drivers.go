package main

import (
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"time"
)

// asuka test --gui's drivers for Android (adb) and the desktops (the
// host's ASUKA_COMMANDS); the simulator's is simulatorFlows (ios.go), the
// web's webFlowDriver (flows.go).

// androidFlows is what the flows do on an Android device with adb: its
// screen, the app's runtime permissions, links the app declares, the
// system UI's demo mode and no animations.
func androidFlows(app *asukaApp, tc *androidToolchain, serial, pkg string, relaunch func() error) *flowDriver {
	adb := func(args ...string) (string, error) {
		out, err := exec.Command(tc.adb, append([]string{"-s", serial}, args...)...).CombinedOutput()
		s := strings.TrimSpace(string(out))
		if err != nil {
			return s, fmt.Errorf("adb %s: %v: %s", strings.Join(args, " "), err, s)
		}
		return s, nil
	}
	shell := func(args ...string) (string, error) { return adb(append([]string{"shell"}, args...)...) }
	sdk := 0
	if s, err := shell("getprop", "ro.build.version.sdk"); err == nil {
		sdk, _ = strconv.Atoi(s)
	}
	perms := func(name string) []string {
		p := "android.permission."
		switch name {
		case "camera":
			return []string{p + "CAMERA"}
		case "microphone":
			return []string{p + "RECORD_AUDIO"}
		case "location":
			return []string{p + "ACCESS_FINE_LOCATION", p + "ACCESS_COARSE_LOCATION"}
		case "location-always":
			return []string{p + "ACCESS_FINE_LOCATION", p + "ACCESS_COARSE_LOCATION", p + "ACCESS_BACKGROUND_LOCATION"}
		case "photos", "media-library":
			if sdk >= 33 {
				if name == "photos" {
					return []string{p + "READ_MEDIA_IMAGES", p + "READ_MEDIA_VIDEO"}
				}
				return []string{p + "READ_MEDIA_AUDIO"}
			}
			return []string{p + "READ_EXTERNAL_STORAGE"}
		case "contacts":
			return []string{p + "READ_CONTACTS", p + "WRITE_CONTACTS"}
		case "calendar", "reminders":
			return []string{p + "READ_CALENDAR", p + "WRITE_CALENDAR"}
		case "notifications":
			if sdk >= 33 {
				return []string{p + "POST_NOTIFICATIONS"}
			}
		case "motion":
			return []string{p + "ACTIVITY_RECOGNITION"}
		case "bluetooth":
			if sdk >= 31 {
				return []string{p + "BLUETOOTH_SCAN", p + "BLUETOOTH_CONNECT"}
			}
		}
		return nil
	}
	declared := func(perm string) bool {
		out, err := shell("dumpsys", "package", pkg)
		return err == nil && strings.Contains(out, perm)
	}
	return &flowDriver{
		screenshot: func(path string) error {
			out, err := exec.Command(tc.adb, "-s", serial, "exec-out", "screencap", "-p").Output()
			if err != nil {
				return fmt.Errorf("adb screencap: %v", err)
			}
			return os.WriteFile(path, out, 0o644)
		},
		relaunch:   relaunch,
		relaunches: true, // a permission revoked ends the app
		permission: func(name, setting string) (string, error) {
			ps := perms(name)
			if len(ps) == 0 {
				return fmt.Sprintf("Android %d doesn't ask for it", sdk), nil
			}
			n := 0
			for _, p := range ps {
				if !declared(p) {
					continue // a permission the app doesn't use (READ_CONTACTS without WRITE_CONTACTS)
				}
				n++
				// User-fixed: denied without asking again, as "Don't allow"
				// twice leaves it; cleared, it's asked for.
				var err error
				switch setting {
				case "allow":
					shell("pm", "clear-permission-flags", pkg, p, "user-set", "user-fixed")
					_, err = shell("pm", "grant", pkg, p)
				case "deny":
					shell("pm", "revoke", pkg, p)
					_, err = shell("pm", "set-permission-flags", pkg, p, "user-set", "user-fixed")
				case "ask":
					shell("pm", "revoke", pkg, p)
					_, err = shell("pm", "clear-permission-flags", pkg, p, "user-set", "user-fixed")
				}
				if err != nil {
					return "", err
				}
			}
			if n == 0 {
				return "", fmt.Errorf("the app doesn't ask for %s (its manifest has none of %s)", name, strings.Join(ps, ", "))
			}
			return "", nil
		},
		openURL: func(u string) (bool, error) {
			scheme, _, _ := strings.Cut(u, ":")
			if app.cfg == nil || app.cfg.Scheme == "" || !strings.EqualFold(scheme, app.cfg.Scheme) {
				return false, nil
			}
			_, err := shell("am", "start", "-W", "-a", "android.intent.action.VIEW", "-d", "'"+strings.ReplaceAll(u, "'", `'\''`)+"'", pkg)
			return true, err
		},
		prepare: func() func() {
			// No animations: what a step starts is over when it's taken.
			scales := []string{"window_animation_scale", "transition_animation_scale", "animator_duration_scale"}
			was := map[string]string{}
			for _, k := range scales {
				v, _ := shell("settings", "get", "global", k)
				was[k] = v
				shell("settings", "put", "global", k, "0")
			}
			// The status bar's demo mode: 9:41, full bars and battery, no
			// notifications' icons.
			allowed, _ := shell("settings", "get", "global", "sysui_demo_allowed")
			shell("settings", "put", "global", "sysui_demo_allowed", "1")
			demo := func(args ...string) {
				shell(append([]string{"am", "broadcast", "-a", "com.android.systemui.demo", "-e", "command"}, args...)...)
			}
			demo("enter")
			demo("clock", "-e", "hhmm", "0941")
			demo("battery", "-e", "level", "100", "-e", "plugged", "false")
			demo("network", "-e", "wifi", "show", "-e", "level", "4")
			demo("network", "-e", "mobile", "show", "-e", "datatype", "none", "-e", "level", "4")
			demo("notifications", "-e", "visible", "false")
			// Light, whatever the device was in.
			night, _ := shell("cmd", "uimode", "night")
			dark := strings.Contains(night, "yes")
			if dark {
				shell("cmd", "uimode", "night", "no")
			}
			return func() {
				demo("exit")
				if allowed == "" || allowed == "null" {
					shell("settings", "delete", "global", "sysui_demo_allowed")
				} else {
					shell("settings", "put", "global", "sysui_demo_allowed", allowed)
				}
				for _, k := range scales {
					if v := was[k]; v == "" || v == "null" {
						shell("settings", "delete", "global", k)
					} else {
						shell("settings", "put", "global", k, v)
					}
				}
				if dark {
					shell("cmd", "uimode", "night", "yes")
				}
			}
		},
	}
}

// desktopFlows is what the flows do on a desktop: the window as the
// host's automation writes it (ASUKA_COMMANDS' snap, in dir), the app's
// process started again. A desktop asks for no permissions ahead (macOS
// asks as the app first uses one, and only the user answers).
func desktopFlows(dir string, relaunch func() error) *flowDriver {
	return &flowDriver{
		relaunch: relaunch,
		screenshot: func(path string) error {
			done := filepath.Join(dir, "asuka-done.txt")
			os.Remove(done)
			os.Remove(path)
			cmds := filepath.Join(dir, "asuka-commands.txt")
			// Whole, or the host reads half of it.
			if err := os.WriteFile(cmds+".part", []byte("snap "+path+"\n"), 0o644); err != nil {
				return err
			}
			if err := os.Rename(cmds+".part", cmds); err != nil {
				return err
			}
			deadline := time.Now().Add(30 * time.Second)
			for time.Now().Before(deadline) {
				if _, err := os.Stat(done); err == nil {
					if _, err := os.Stat(path); err == nil {
						return nil
					}
					return errors.New("the window wrote no screenshot")
				}
				time.Sleep(50 * time.Millisecond)
			}
			return errors.New("the window didn't take the screenshot")
		},
	}
}
