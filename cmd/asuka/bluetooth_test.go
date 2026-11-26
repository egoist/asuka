package main

import (
	"os"
	"path/filepath"
	"slices"
	"strings"
	"testing"
)

// An app that imports platform/bluetooth gets its permissions, its purpose
// string, its module's uses-feature and the Flatpak's bus name;
// asuka.toml's bluetooth adds the background modes and the location from
// scans.
func TestBluetoothConfig(t *testing.T) {
	var c appConfig
	importStd(t, &c, "", "bluetooth")
	m := c.androidManifest(false)
	for _, want := range []string{
		`<uses-permission android:name="android.permission.BLUETOOTH_SCAN" android:usesPermissionFlags="neverForLocation" />`,
		`<uses-permission android:name="android.permission.BLUETOOTH_CONNECT" />`,
		`<uses-permission android:name="android.permission.BLUETOOTH_ADVERTISE" />`,
		// Before Android 12: the install's Bluetooth permissions, and the
		// location a scan needs there.
		`<uses-permission android:name="android.permission.BLUETOOTH" android:maxSdkVersion="30" />`,
		`<uses-permission android:name="android.permission.BLUETOOTH_ADMIN" android:maxSdkVersion="30" />`,
		`<uses-permission android:name="android.permission.ACCESS_FINE_LOCATION" android:maxSdkVersion="30" />`,
	} {
		if !strings.Contains(m, want) {
			t.Errorf("the manifest has no %s:\n%s", want, m)
		}
	}
	if mod := stdAndroidManifest(t, "bluetooth"); !strings.Contains(mod, `<uses-feature android:name="android.hardware.bluetooth_le" android:required="false" />`) {
		t.Errorf("platform/bluetooth's module's manifest:\n%s", mod)
	}
	if strings.Contains(m, "AsukaBluetoothScanReceiver") || strings.Count(m, "LOCATION") != 1 {
		t.Errorf("background scanning without asking for it:\n%s", m)
	}
	if c.IOS.Usage["NSBluetoothAlwaysUsageDescription"] == "" || len(c.plistArrays["UIBackgroundModes"]) != 0 {
		t.Errorf("iOS: %v %v", c.IOS.Usage, c.plistArrays)
	}
	if !slices.Contains(c.flatpakArgs, "--system-talk-name=org.bluez") {
		t.Errorf("flatpak: %v", c.flatpakArgs)
	}
	app := &asukaApp{cfg: &c, bundleID: "dev.asuka.ble", name: "ble"}
	if f := flatpakManifest(app, "ble"); !strings.Contains(f, "  - --device=dri\n  - --system-talk-name=org.bluez\nmodules:") {
		t.Errorf("the Flatpak's manifest:\n%s", f)
	}

	// With location (another package's too): the location permission on
	// every Android, and scans that may find it.
	dir := t.TempDir()
	os.WriteFile(filepath.Join(dir, "asuka.toml"), []byte("[bluetooth]\nbackground = [\"central\", \"peripheral\"]\nlocation = true"), 0o644)
	bg, err := loadConfig(dir)
	if err != nil {
		t.Fatal(err)
	}
	importStd(t, bg, "", "bluetooth", "location")
	m = bg.androidManifest(false)
	for _, want := range []string{
		`<uses-permission android:name="android.permission.BLUETOOTH_SCAN" />`,
		`<uses-permission android:name="android.permission.ACCESS_FINE_LOCATION" />`,
	} {
		if !strings.Contains(m, want) {
			t.Errorf("the manifest has no %s:\n%s", want, m)
		}
	}
	if strings.Contains(m, "AsukaBluetoothScanReceiver") {
		t.Errorf("the host's old scan receiver:\n%s", m)
	}
	if n := strings.Count(m, "ACCESS_FINE_LOCATION"); n != 1 {
		t.Errorf("ACCESS_FINE_LOCATION %d times:\n%s", n, m)
	}
	if got := bg.plistArrays["UIBackgroundModes"]; !slices.Equal(got, []string{"bluetooth-central", "bluetooth-peripheral"}) {
		t.Errorf("background modes: %v", got)
	}

	// A background mode asked for, without the package: nothing.
	var none appConfig
	none.Bluetooth.Background = []string{"central"}
	importStd(t, &none, "")
	if len(none.plistArrays["UIBackgroundModes"]) != 0 || len(none.androidApp) != 0 {
		t.Errorf("without the package: %v %v", none.plistArrays, none.androidApp)
	}

	os.WriteFile(filepath.Join(dir, "asuka.toml"), []byte(`bluetooth.background = ["beacon"]`), 0o644)
	if _, err := loadConfig(dir); err == nil || !strings.Contains(err.Error(), "beacon") {
		t.Errorf("a bad background role: %v", err)
	}
}

// Permissions merge by name: the widest level, the fewest flags.
func TestAddPermission(t *testing.T) {
	var c appConfig
	for _, p := range []string{"a:28", "a:30", "b", "b:30", "c|neverForLocation", "c", "d:30|x", "d:28|x"} {
		c.addPermission(p)
	}
	if want := []string{"a:30", "b", "c", "d:30|x"}; !slices.Equal(c.Android.Permissions, want) {
		t.Errorf("%v, want %v", c.Android.Permissions, want)
	}
}

// A package's Flatpak modules (platform/pdf's Poppler, which the GNOME
// runtime hasn't) are built before the app, each a list item of the
// manifest's modules.
func TestFlatpakModules(t *testing.T) {
	var c appConfig
	importStd(t, &c, "", "pdf")
	app := &asukaApp{cfg: &c, bundleID: "dev.asuka.reader", name: "reader"}
	f := flatpakManifest(app, "reader")
	poppler := strings.Index(f, "modules:\n  - name: poppler\n    buildsystem: cmake-ninja\n    config-opts:\n      - -DCMAKE_BUILD_TYPE=Release\n")
	own := strings.Index(f, "  - name: reader\n    buildsystem: simple\n")
	if poppler < 0 || own < poppler {
		t.Errorf("the Flatpak's manifest:\n%s", f)
	}
	if !strings.Contains(f, "      - type: archive\n        url: https://poppler.freedesktop.org/") {
		t.Errorf("Poppler's source:\n%s", f)
	}
}
