package main

import (
	"net/http/httptest"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"testing"

	"github.com/egoist/asuka/internal/testserver"
	"github.com/egoist/asuka/std"
)

// TestStdAndroidUnits runs the standard library's and the platform SDK's
// Android unit tests ({std,sdk}/<name>/android/src/test: their Kotlin on
// this computer's JVM, Android's classes stubs), as TestStdAndroidViews
// runs the instrumented ones but with no device: each module's
// testDebugUnitTest, in the Android host's Gradle project an app
// importing those packages writes. internal/testserver serves them
// (std/http's bodies, std/websocket's echo), its URL the
// asuka.testserver property.
func TestStdAndroidUnits(t *testing.T) {
	if testing.Short() {
		t.Skip("builds with Gradle")
	}
	imports := map[string]string{} // a package's directory → its import path
	ps, err := std.Packages()
	if err != nil {
		t.Fatal(err)
	}
	for path, d := range ps {
		if exists(filepath.Join(d, "android", "src", "test")) {
			imports[filepath.Clean(d)] = path
		}
	}
	if len(imports) == 0 {
		t.Fatal("no unit tests")
	}
	tc, err := newAndroidToolchain(false)
	if err != nil {
		t.Skip(err)
	}
	var paths []string
	for _, p := range imports {
		paths = append(paths, p)
	}
	sort.Strings(paths)
	app := t.TempDir()
	var src strings.Builder
	for _, p := range paths {
		src.WriteString("import " + p + "\n")
	}
	src.WriteString("\napp { Text(\"tests\") }\n")
	os.WriteFile(filepath.Join(app, "app.asuka"), []byte(src.String()), 0o644)
	os.WriteFile(filepath.Join(app, "asuka.toml"), []byte("name = \"StdUnits\"\nid = \"dev.asuka.stdunits\"\nversion = \"1.0.0\"\nbuild = 1\n"), 0o644)
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
	server := httptest.NewServer(testserver.Handler())
	defer server.Close()
	for _, m := range a.cfg.nativeModules {
		pkg := filepath.Clean(filepath.Dir(m[1]))
		if imports[pkg] == "" {
			continue
		}
		t.Run(filepath.Base(pkg), func(t *testing.T) {
			args := append([]string{":" + m[0] + ":testDebugUnitTest", "-PasukaTestServer=" + server.URL}, a.cfg.gradleProps()...)
			if err := tc.gradle(host, args...); err != nil {
				t.Fatalf("%v (the report: %s)", err, filepath.Join(host, "build", "native", m[0]+"-*", "reports", "tests"))
			}
		})
	}
}
