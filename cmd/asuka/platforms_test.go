package main

import (
	"archive/zip"
	"bytes"
	"encoding/json"
	"io"
	"net"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"runtime"
	"slices"
	"strings"
	"testing"
	"time"

	"github.com/egoist/asuka/internal/appbuild"
	"github.com/egoist/asuka/internal/appbuild/apptest"
	"github.com/egoist/asuka/internal/cbuild"
	"github.com/egoist/asuka/lang/bytecode"
	"github.com/egoist/asuka/lang/check"
	"github.com/egoist/asuka/lang/syntax"
	"github.com/egoist/asuka/lang/target"
)

// useModule makes appbuild.Compile compile any app to the module m makes,
// until the test ends.
func useModule(t *testing.T, m func() *bytecode.Module) {
	t.Helper()
	old := appbuild.Compile
	appbuild.Compile = func(dir string, deps map[string]string, _ target.Platform, _ bool) (*bytecode.Module, *appbuild.Uses, []check.Diagnostic, error) {
		return m(), nil, nil, nil
	}
	t.Cleanup(func() { appbuild.Compile = old })
}

// asuka test runs an app's test blocks on the fake host: it fails with
// how many did; -run picks them; a compile that fails prints its
// diagnostics.
func TestCmdTest(t *testing.T) {
	if _, err := cbuild.Find(); err != nil {
		t.Skip(err)
	}
	dir := counterDir(t)
	useModule(t, func() *bytecode.Module {
		m := apptest.Counter("Count: ")
		apptest.Failing(m, "Count: ")
		return m
	})
	if err := cmdTest([]string{dir}); err == nil || err.Error() != "1 of 2 tests failed" {
		t.Errorf("err %v", err)
	}
	if err := cmdTest([]string{"--run", "Add", "-p", "android", dir}); err != nil {
		t.Errorf("-run Add: %v", err)
	}
	if err := cmdTest([]string{"--run", "nothing", dir}); err == nil || !strings.Contains(err.Error(), `no test's name contains "nothing"`) {
		t.Errorf("-run nothing: %v", err)
	}
	old := appbuild.Compile
	appbuild.Compile = func(dir string, deps map[string]string, _ target.Platform, _ bool) (*bytecode.Module, *appbuild.Uses, []check.Diagnostic, error) {
		pos := syntax.Position{Filename: filepath.Join(dir, "app.asuka"), Line: 1, Column: 7}
		return nil, nil, []check.Diagnostic{{Pos: pos, Code: "undefined-name", Msg: "unknown name Txt"}}, nil
	}
	defer func() { appbuild.Compile = old }()
	if err := cmdTest([]string{dir}); err == nil || err.Error() != "1 error" {
		t.Errorf("a compile error: %v", err)
	}
}

// asuka run runs one file: its tests, then its app's screen.
func TestCmdRun(t *testing.T) {
	if _, err := cbuild.Find(); err != nil {
		t.Skip(err)
	}
	useCounter(t, "Count: ")
	file := filepath.Join(counterDir(t), "app.asuka")
	if err := cmdRun([]string{file}); err != nil {
		t.Fatal(err)
	}
	if err := cmdRun([]string{filepath.Join(t.TempDir(), "none.txt")}); err == nil {
		t.Error("not a file")
	}
}

// A web build is a static site: the page, the core with the app compiled
// in (asuka.<digest>.wasm), the host's files; served, it shows the app.
func TestBuildWeb(t *testing.T) {
	if testing.Short() {
		t.Skip("builds the core as WebAssembly (not -short)")
	}
	if _, err := cbuild.ForTarget(cbuild.Target{OS: "wasm", Arch: "wasm32"}); err != nil {
		t.Skip(err)
	}
	useCounter(t, "Count: ")
	app := projectApp(t, counterDir(t), nil)
	out := t.TempDir()
	if err := buildWeb(app, &buildOptions{out: out}); err != nil {
		t.Fatal(err)
	}
	site := filepath.Join(out, "web")
	page, err := os.ReadFile(filepath.Join(site, "index.html"))
	if err != nil {
		t.Fatal(err)
	}
	core := regexp.MustCompile(`"core":"(asuka\.[0-9a-f]{12}\.wasm)"`).FindSubmatch(page)
	if core == nil {
		t.Fatalf("the page doesn't start the core:\n%s", page)
	}
	if b, err := os.ReadFile(filepath.Join(site, string(core[1]))); err != nil || !bytes.HasPrefix(b, []byte("\x00asm")) {
		t.Fatalf("%s: %v", core[1], err)
	}
	host, _ := filepath.Glob(filepath.Join(site, "asuka", "*", "asuka.js"))
	if len(host) != 1 {
		t.Fatalf("the host's files: %v", host)
	}
	for _, f := range []string{"manifest.webmanifest", "_headers", "_redirects", "version.json"} {
		if _, err := os.Stat(filepath.Join(site, f)); err != nil {
			t.Error(err)
		}
	}
	// The service worker, at the same name each build, lists the site's
	// files for offline: the page, the core, the host's.
	if !bytes.Contains(page, []byte(`"serviceWorker":"sw.js"`)) {
		t.Errorf("the page doesn't register sw.js:\n%s", page)
	}
	if sw, err := os.ReadFile(filepath.Join(site, "sw.js")); err != nil {
		t.Error(err)
	} else if !bytes.Contains(sw, []byte(`["`+string(core[1])+`",""]`)) || !regexp.MustCompile(`\["index.html","[0-9a-f]{16}"\]`).Match(sw) || !bytes.Contains(sw, []byte("asukaOffline")) {
		t.Errorf("sw.js lists no files to keep offline:\n%.2000s", sw)
	}
	browser := chromePath()
	if browser == "" {
		return
	}
	// The site's page, with what reports what it shows (or its errors)
	// before it: the rest is the site's files as built.
	results := make(chan string, 1)
	reporter := `<script>
const report = (m) => fetch("/__result", { method: "POST", body: m });
addEventListener("error", (e) => report("ERROR " + e.message));
addEventListener("unhandledrejection", (e) => report("ERROR " + String(e.reason && e.reason.stack || e.reason)));
const wait = setInterval(() => { if (document.body && document.body.innerText.includes("Count: 0")) { clearInterval(wait); report(document.body.innerText); } }, 20);
</script>`
	files := http.FileServer(http.Dir(site))
	mux := http.NewServeMux()
	mux.HandleFunc("/__result", func(w http.ResponseWriter, r *http.Request) {
		b, _ := io.ReadAll(r.Body)
		select {
		case results <- string(b):
		default:
		}
	})
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/" {
			files.ServeHTTP(w, r)
			return
		}
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		io.WriteString(w, strings.Replace(string(page), "<head>", "<head>"+reporter, 1))
	})
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	srv := &http.Server{Handler: mux}
	go srv.Serve(ln)
	defer srv.Close()
	cmd := exec.Command(browser, "--headless=new", "--no-first-run", "--no-default-browser-check", "--disable-gpu",
		"--user-data-dir="+t.TempDir(), "--window-size=430,932", "http://"+ln.Addr().String()+"/")
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	defer func() { cmd.Process.Kill(); cmd.Wait() }()
	select {
	case got := <-results:
		if strings.HasPrefix(got, "ERROR") || !strings.Contains(got, "Count: 0") {
			t.Fatalf("the page shows:\n%s", got)
		}
	case <-time.After(90 * time.Second):
		t.Fatal("the page never showed the app")
	}
}

// An iOS build for the simulator is an .app: the Swift host linked with
// the core, the app compiled in (asuka_run), its Info.plist; it installs
// and launches on a booted simulator.
func TestBuildIOSSimulator(t *testing.T) {
	if runtime.GOOS != "darwin" || testing.Short() {
		t.Skip("Xcode, on a Mac (not -short)")
	}
	if _, err := cbuild.ForTarget(cbuild.Target{OS: "ios-simulator", Arch: runtime.GOARCH}); err != nil {
		t.Skip(err)
	}
	useCounter(t, "Count: ")
	app := projectApp(t, counterDir(t), nil)
	out := t.TempDir()
	if err := buildIOS(app, &buildOptions{out: out, simulator: true}); err != nil {
		t.Fatal(err)
	}
	bundle := filepath.Join(out, "Counter.app")
	syms, err := exec.Command("xcrun", "nm", filepath.Join(bundle, appExe)).Output()
	if err != nil {
		t.Fatal(err)
	}
	// The core's API (its other symbols are hidden, and a release's
	// stripped), and the app compiled in: its text.
	for _, want := range []string{"_asuka_run", "_asuka_dev_connect"} {
		if !bytes.Contains(syms, []byte(" "+want+"\n")) {
			t.Errorf("the executable has no %s", want)
		}
	}
	if exe, err := os.ReadFile(filepath.Join(bundle, appExe)); err != nil || !bytes.Contains(exe, []byte("Count: ")) {
		t.Errorf("the executable hasn't the app compiled in (%v)", err)
	}
	info, err := os.ReadFile(filepath.Join(bundle, "Info.plist"))
	if err != nil || !bytes.Contains(info, []byte("dev.asuka.devtest")) {
		t.Fatalf("Info.plist: %v", err)
	}
	if sim := bootedSimulator(); sim != "" {
		if b, err := exec.Command("xcrun", "simctl", "install", sim, bundle).CombinedOutput(); err != nil {
			t.Fatalf("install: %v\n%s", err, b)
		}
		if b, err := exec.Command("xcrun", "simctl", "launch", "--terminate-running-process", sim, "dev.asuka.devtest").CombinedOutput(); err != nil {
			t.Fatalf("launch: %v\n%s", err, b)
		}
		exec.Command("xcrun", "simctl", "terminate", sim, "dev.asuka.devtest").Run()
	}
}

func bootedSimulator() string {
	out, err := exec.Command("xcrun", "simctl", "list", "devices", "booted", "-j").Output()
	if err != nil {
		return ""
	}
	var list struct {
		Devices map[string][]simDevice `json:"devices"`
	}
	json.Unmarshal(out, &list)
	for _, ds := range list.Devices {
		for _, d := range ds {
			if d.State == "Booted" {
				return d.UDID
			}
		}
	}
	return ""
}

// The dev loop on the simulator: the player (the app's .app with the core
// without an app) is built, installed and launched; it loads the module
// and the next one, keeping its state.
func TestDevIOSSimulator(t *testing.T) {
	if runtime.GOOS != "darwin" || testing.Short() {
		t.Skip("the simulator, on a Mac (not -short)")
	}
	sim := bootedSimulator()
	if sim == "" {
		t.Skip("no booted simulator")
	}
	tc, err := newIOSToolchain(false)
	if err != nil {
		t.Skip(err)
	}
	useCounter(t, "Count: ")
	app := projectApp(t, counterDir(t), nil)
	loop, err := newDevLoop(app, "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer loop.close()
	p := simulatorPlayer(app, tc, t.TempDir(), &simDevice{UDID: sim}, loop)
	if err := p.launch(); err != nil {
		t.Fatal(err)
	}
	defer p.stop()
	if r := nextReload(t, loop); !r.first {
		t.Fatalf("the first report: %+v", r)
	}
	useCounter(t, "Total: ")
	loop.compile()
	if r := nextReload(t, loop); r.first || r.kept < 1 {
		t.Fatalf("the reload: %+v", r)
	}
}

// The dev loop on an Android emulator or device: the shell app installed,
// the player's core pushed, connected through adb reverse; it loads the
// module and the next one, keeping its state.
func TestDevAndroid(t *testing.T) {
	if testing.Short() {
		t.Skip("an emulator (not -short)")
	}
	tc, err := newAndroidToolchain(false)
	if err != nil {
		t.Skip(err)
	}
	b, _ := exec.Command(tc.adb, "devices").Output()
	devs := string(b)
	serial := ""
	for _, l := range strings.Split(devs, "\n")[1:] {
		if f := strings.Fields(l); len(f) == 2 && f[1] == "device" {
			serial = f[0]
			break
		}
	}
	if serial == "" {
		t.Skip("no device or emulator (adb devices)")
	}
	useCounter(t, "Count: ")
	app := projectApp(t, counterDir(t), nil)
	loop, err := newDevLoop(app, "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer loop.close()
	p, err := androidPlayer(app, tc, t.TempDir(), serial, loop)
	if err != nil {
		t.Fatal(err)
	}
	if err := p.launch(); err != nil {
		t.Fatal(err)
	}
	defer p.stop()
	if r := nextReload(t, loop); !r.first {
		t.Fatalf("the first report: %+v", r)
	}
	useCounter(t, "Total: ")
	loop.compile()
	if r := nextReload(t, loop); r.first || r.kept < 1 {
		t.Fatalf("the reload: %+v", r)
	}
}

// An Android release is an .aab with the core (the app compiled in) for
// every ABI, signed with the upload key.
func TestBuildAndroid(t *testing.T) {
	if testing.Short() {
		t.Skip("Gradle (not -short)")
	}
	tc, err := newAndroidToolchain(false)
	if err != nil {
		t.Skip(err)
	}
	_ = tc
	if _, err := exec.LookPath("keytool"); err != nil {
		t.Skip("no keytool")
	}
	useCounter(t, "Count: ")
	dir := counterDir(t)
	ks := filepath.Join(dir, "upload.jks")
	if b, err := exec.Command("keytool", "-genkeypair", "-keystore", ks, "-alias", "upload", "-keyalg", "RSA", "-keysize", "2048",
		"-validity", "100", "-storepass", "secret1", "-keypass", "secret1", "-dname", "CN=Test").CombinedOutput(); err != nil {
		t.Fatalf("keytool: %v\n%s", err, b)
	}
	t.Setenv("ASUKA_KEYSTORE", ks)
	t.Setenv("ASUKA_KEY_ALIAS", "upload")
	t.Setenv("ASUKA_KEYSTORE_PASSWORD", "secret1")
	app := projectApp(t, dir, nil)
	out := t.TempDir()
	if err := buildAndroid(app, &buildOptions{out: out}); err != nil {
		t.Fatal(err)
	}
	aab, _ := filepath.Glob(filepath.Join(out, "*.aab"))
	if len(aab) != 1 {
		t.Fatalf("%v", aab)
	}
	z, err := zip.OpenReader(aab[0])
	if err != nil {
		t.Fatal(err)
	}
	defer z.Close()
	var libs []string
	for _, f := range z.File {
		if strings.HasSuffix(f.Name, "/libasuka.so") {
			libs = append(libs, f.Name)
		}
	}
	slices.Sort(libs)
	if want := []string{"base/lib/arm64-v8a/libasuka.so", "base/lib/armeabi-v7a/libasuka.so", "base/lib/x86_64/libasuka.so"}; !slices.Equal(libs, want) {
		t.Errorf("the .aab's cores: %v, want %v", libs, want)
	}
}
