//go:build darwin && !ios

package darwin

import (
	"encoding/json"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"

	"github.com/ebitengine/purego"

	"github.com/egoist/asuka/ui"
)

// buildICloudSwift builds the iCloud module's Swift (ICloud.swift, the
// iOS host's, and its exports) as the CLI builds it into an app's library
// for asuka/sdk/icloud.
func buildICloudSwift(t *testing.T) string {
	t.Helper()
	if _, err := exec.LookPath("xcrun"); err != nil {
		t.Skip("no Xcode command line tools")
	}
	arch := map[string]string{"arm64": "arm64", "amd64": "x86_64"}[runtime.GOARCH]
	src := "../../platform/macos/Sources/AsukaMac/"
	lib := filepath.Join(t.TempDir(), "ICloud.dylib")
	out, err := exec.Command("xcrun", "swiftc", "-swift-version", "5", "-target", arch+"-apple-macos11", "-parse-as-library", "-Onone",
		"-D", "ASUKA_ICLOUD", "-module-name", "Asuka", "-emit-library", src+"ICloud.swift", src+"ICloudExports.swift", "-o", lib).CombinedOutput()
	if err != nil {
		t.Fatalf("swiftc: %v\n%s", err, out)
	}
	return lib
}

// The module through the Mac's Swift library: the key-value store's
// values (in a test's store), converted between JSON and property lists
// both ways; without iCloud's entitlements (the test's process has
// none), the account's and the documents' calls say why; events after
// stop.
func TestICloudSwift(t *testing.T) {
	lib := buildICloudSwift(t)
	h, _ := mount(t, func() ui.Node { return ui.StaticText("x") })
	onMain(func() {
		handle, err := purego.Dlopen(lib, purego.RTLD_NOW|purego.RTLD_LOCAL)
		if err != nil {
			fatal(t, err)
		}
		saved := swiftICloud
		defer func() { swiftICloud = saved }()
		loadSwiftICloud(handle)
		if !swiftICloud.ok {
			fatal(t, "the library's iCloud functions weren't found")
		}
		// The values in a UserDefaults suite: the test has no entitlement
		// for iCloud's store.
		var testStore func()
		purego.RegisterLibFunc(&testStore, handle, "asuka_icloud_test_store")
		testStore()
		call := func(m string, args string) (string, error) {
			b, err := await(h, "icloud", m, []byte(args))
			return string(b), err
		}

		set := `{"key":"asuka.test","value":{"n":1.5,"on":true,"s":"x","none":null,"list":[1,"two"],"in":{"deep":[]}}}`
		if _, err := call("kvSet", set); err != nil {
			fatal(t, "kvSet: ", err)
		}
		got, err := call("kvGet", "asuka.test")
		var v map[string]any
		if err != nil || json.Unmarshal([]byte(got), &v) != nil {
			fatalf(t, "kvGet: %s, %v", got, err)
		}
		if _, has := v["none"]; has || v["n"] != 1.5 || v["on"] != true || v["s"] != "x" || len(v["list"].([]any)) != 2 {
			t.Errorf("kvGet: %s", got)
		}
		if all, err := call("kvAll", ""); err != nil || !strings.Contains(all, `"asuka.test"`) {
			t.Errorf("kvAll: %s, %v", all, err)
		}
		if _, err := call("kvSet", `{"key":"asuka.bad","value":[null]}`); err == nil {
			t.Error("a null in an array was kept")
		}
		if _, err := call("kvDelete", "asuka.test"); err != nil {
			t.Error("kvDelete: ", err)
		}
		if got, err := call("kvGet", "asuka.test"); err != nil || got != "" {
			t.Errorf("deleted: %q, %v", got, err)
		}
		if _, err := call("kvSync", ""); err != nil {
			t.Error("kvSync: ", err)
		}

		// Unsigned, there's no iCloud (whatever the Mac is signed in to),
		// and the calls say why.
		if _, err := call("account", ""); err == nil || !strings.HasPrefix(err.Error(), "not available: the app isn't signed with iCloud") {
			t.Errorf("account unsigned: %v", err)
		}
		if _, err := call("files", `{"scope":"documents"}`); err == nil || !strings.HasPrefix(err.Error(), "not available: the app isn't signed with iCloud's documents") {
			t.Errorf("files unsigned: %v", err)
		}
		if _, err := call("read", "/tmp/x"); err == nil || !strings.HasPrefix(err.Error(), "not available") {
			t.Errorf("read unsigned: %v", err)
		}

		// "next" waits, and "stop" ends it.
		var stopped error
		answered := false
		h.module("icloud")["next"](nil, func(_ []byte, err error) { stopped, answered = err, true })
		if _, err := call("stop", ""); err != nil || !answered || stopped == nil || stopped.Error() != "stopped" {
			t.Errorf("stop: %v, next %v %v", err, answered, stopped)
		}
	})
}
