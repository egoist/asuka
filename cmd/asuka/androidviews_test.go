package main

import (
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
	"testing"

	"github.com/egoist/asuka/std"
)

// TestStdAndroidViews runs the standard library's and the platform SDK's
// instrumented Android tests ({std,sdk}/<name>/android/src/androidTest: a
// view driven on a device, its package's tests/ its assets): an app importing those packages has
// the Android host's Gradle project written as asuka dev writes it, its
// packages' modules in it, and each module's connectedDebugAndroidTest
// run on the device adb has (none: skipped). Gradle fails when a test
// does, saying which and why.
func TestStdAndroidViews(t *testing.T) {
	if testing.Short() {
		t.Skip("builds with Gradle")
	}
	ps, err := std.Packages()
	if err != nil {
		t.Fatal(err)
	}
	var tests []string
	imports := map[string]string{} // a test's directory → its package's import path
	for path, d := range ps {
		if test := filepath.Join(d, "android", "src", "androidTest"); exists(test) {
			tests = append(tests, test)
			imports[test] = path
		}
	}
	sort.Strings(tests)
	if len(tests) == 0 {
		t.Fatal("no instrumented tests")
	}
	tc, err := newAndroidToolchain(false)
	if err != nil {
		t.Skip(err)
	}
	if out, _ := exec.Command(tc.adb, "devices").Output(); !strings.Contains(string(out), "\tdevice") {
		t.Skip("no Android device (adb devices)")
	}
	serial, err := tc.pickDevice("")
	if err != nil {
		t.Skip(err)
	}
	// The app: one that imports each package with tests.
	app := t.TempDir()
	var src strings.Builder
	pkgs := map[string]bool{}
	for _, test := range tests {
		name := filepath.Base(filepath.Dir(filepath.Dir(filepath.Dir(test))))
		pkgs[name] = true
		src.WriteString("import " + imports[test] + "\n")
	}
	src.WriteString("\napp { Text(\"tests\") }\n")
	os.WriteFile(filepath.Join(app, "app.asuka"), []byte(src.String()), 0o644)
	os.WriteFile(filepath.Join(app, "asuka.toml"), []byte("name = \"StdViews\"\nid = \"dev.asuka.stdviews\"\nversion = \"1.0.0\"\nbuild = 1\n"), 0o644)
	a, err := loadApp(app, platformAndroid)
	if err != nil {
		t.Fatal(err)
	}
	work, err := workDir(a, "android")
	if err != nil {
		t.Fatal(err)
	}
	unlock, err := lockShared(androidHostDir(work))
	if err != nil {
		t.Fatal(err)
	}
	defer unlock()
	host, _, err := prepareAndroidHost(a, work)
	if err != nil {
		t.Fatal(err)
	}
	for _, m := range a.cfg.nativeModules {
		name := filepath.Base(filepath.Dir(m[1]))
		if !pkgs[name] || !exists(filepath.Join(m[1], "src", "androidTest")) {
			continue
		}
		t.Run(name, func(t *testing.T) {
			args := append([]string{":" + m[0] + ":connectedDebugAndroidTest"}, a.cfg.gradleProps()...)
			if err := tc.gradleEnv(host, []string{"ANDROID_SERIAL=" + serial}, args...); err != nil {
				t.Fatal(err)
			}
		})
	}
}
