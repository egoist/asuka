package desktop_test

import (
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"

	"github.com/egoist/asuka/internal/appbuild"
	"github.com/egoist/asuka/internal/cbuild"
	"github.com/egoist/asuka/internal/nativegen"
	"github.com/egoist/asuka/lang/compile"
	"github.com/egoist/asuka/lang/target"
)

// pantry is nativegen's golden package: its declarations, its generated
// C and its C (every kind of value, an async call, callbacks, objects,
// errors, a view).
var pantry = filepath.Join("..", "..", "internal", "nativegen", "testdata", "pantry")

// The registry and a package's C, run with no Go: an Asuka app compiled
// to bytecode, loaded by a test host of asuka_desktop.h (test/host.c) on
// the core, with the pantry's C behind its native interface and its view.
// What the app gets back it shows as its step; what the C gets, it says.
func TestPantry(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("the test host is POSIX's (pthreads)")
	}
	core, err := cbuild.CoreDir()
	if err != nil {
		t.Skip(err)
	}
	res := compileProbe(t)
	work := t.TempDir()
	mod := filepath.Join(work, "app.akb")
	os.WriteFile(mod, res.Module.Encode(), 0o644)

	desk, _ := filepath.Abs(".")
	pdesk, _ := filepath.Abs(filepath.Join(pantry, "desktop"))
	srcs := []string{filepath.Join(desk, "src", "native.c"), filepath.Join(desk, "src", "linux.c"), filepath.Join(desk, "src", "linux_syms.c"), filepath.Join(desk, "test", "host.c")}
	for _, f := range nativegen.CSources(pdesk, "linux") {
		srcs = append(srcs, filepath.Join(pdesk, f))
	}
	flags := append(append([]string{}, cbuild.Flags...), "-I"+filepath.Join(desk, "include"), "-I"+pdesk, "-D_GNU_SOURCE")
	exe, err := cbuild.Exe(core, srcs, flags, nil, true, work, "asuka-test-host")
	if err != nil {
		t.Fatal(err)
	}
	cmd := exec.Command(exe, mod)
	cmd.Env = append(os.Environ(), "ASUKA_DATA="+work, noLeaks)
	checkProbe(t, cmd)
}

// A release's executable, as the CLI builds one for Windows and Linux
// (appbuild.DesktopExe): a host (here the test host), the core, the
// registry, the packages' C and the app compiled to C, one program.
func TestDesktopExe(t *testing.T) {
	if runtime.GOOS == "windows" || testing.Short() {
		t.Skip("the test host is POSIX's (pthreads); builds the app's C (not -short)")
	}
	res := compileProbe(t)
	desk, _ := filepath.Abs(".")
	host := &appbuild.DesktopC{Sources: []string{filepath.Join(desk, "test", "host.c")}, Flags: []string{"-D_GNU_SOURCE"}}
	pkg, _ := filepath.Abs(pantry)
	natives, err := appbuild.DesktopNatives(cbuild.Host(), []string{pkg})
	if err != nil {
		t.Fatal(err)
	}
	work := t.TempDir()
	exe := filepath.Join(work, "probe")
	if err := appbuild.DesktopExe(cbuild.Host(), res.Module, host, natives, exe); err != nil {
		t.Fatal(err)
	}
	cmd := exec.Command(exe)
	cmd.Env = append(os.Environ(), "ASUKA_DATA="+work, noLeaks)
	out := checkProbe(t, cmd)
	if !strings.Contains(out, "host: the app compiled in") {
		t.Error("the app isn't the one compiled in")
	}
}

// noLeaks turns LeakSanitizer off: asuka_unload leaves the module's
// globals (the core's, not what these tests check).
const noLeaks = "ASAN_OPTIONS=detect_leaks=0"

func compileProbe(t *testing.T) *compile.Result {
	t.Helper()
	app := t.TempDir()
	pkg := filepath.Join(app, "pantry")
	os.MkdirAll(pkg, 0o755)
	for _, f := range []string{"pantry.asuka", "asuka.toml"} {
		b, err := os.ReadFile(filepath.Join(pantry, f))
		if err != nil {
			t.Fatal(err)
		}
		os.WriteFile(filepath.Join(pkg, f), b, 0o644)
	}
	os.WriteFile(filepath.Join(app, "app.asuka"), []byte(probe), 0o644)
	res, err := compile.Dir(app, &compile.Options{Platform: target.Host()})
	if err != nil {
		t.Fatal(err)
	}
	if err := res.Err(); err != nil {
		t.Fatal(err)
	}
	return res
}

// checkProbe runs the probe and checks what the app and the C said.
func checkProbe(t *testing.T, cmd *exec.Cmd) string {
	t.Helper()
	out, err := cmd.CombinedOutput()
	got := string(out)
	t.Logf("%s", got)
	if err != nil {
		t.Fatalf("%v", err)
	}
	for _, want := range []string{
		"host: native view @asuka/pantry#Dial",
		"pantry: dial made for view",
		"pantry: dial label Heat",
		"pantry: dial value 0.5",
		"pantry: dial item knob weight=1.5 level=0 tags=a,b notes=k:v next=none data=xyz took=250ms count=4 fresh=1",
		"host: measured @asuka/pantry#Dial 120x32",
		"host: name Larder",
		"host: items fig-1/1/high/dry+cool/top/raw/1500 jam/2/lid",
		"pantry: add fig-1 weight=2.5 level=1 tags=dry,cool notes=shelf:top next=none data=raw took=1500ms count=1 fresh=1",
		"pantry: add jam weight=2.5 level=1 tags=dry,cool notes=shelf:top next=lid data=raw took=1500ms count=2 fresh=1",
		"host: add empty: PantryError.empty",
		"host: cooked stew cooked/9 after 0.5,1",
		"host: burnt: the burnt burnt (oven 7)",
		"pantry: cook cancelled: 1",
		"host: forever: ",
		"host: asked ready?: true",
		"pantry: stack top 3 level=1",
		"pantry: close top",
		"host: locked: PantryError.busy",
		"pantry: show on the main thread: 1",
		"host: counts jars=12 tins=4",
		"pantry: shows a view 1",
		"pantry: shows none 0",
		"host: shows true false",
		"host: dial 0.75 picked knob@1",
		"host: done",
		"host: finished",
	} {
		if !strings.Contains(got, want) {
			t.Errorf("no %q", want)
		}
	}
	if !strings.Contains(got, "pantry: release shelf top") {
		t.Errorf("the shelf wasn't released")
	}
	return got
}

const probe = `import ./pantry

let stepView = Ref()

component Probe {
  state step = "start"
  state dial = 0.5
  state picked = ""
  state progress: [float] = []

  Column {
    Text("#{step}") {
      ref: stepView
    }
    pantry.Dial(dial) {
      label: "Heat"
      items: [pantry.Item(name: "knob", weight: 1.5, level: .low, tags: ["a", "b"], notes: ["k": "v"], next: none, data: "xyz".bytes(), took: 250ms, count: 4, fresh: true)]
      width: 120
      on changed(v) { dial = v }
      on picked(item, i) { picked = "{item.name}@{i}" }
    }
  }

  task {
    do {
      let p = try pantry.Pantry()
      step = "name {try p.name()}"
      await sleep(1ms)
      let items = try p.items("fig")
      let a = items[0]
      let b = items[1]
      step = "items {a.name}/{a.count}/{a.level}/{a.tags.joined(separator: "+")}/{a.notes["shelf"] ?? ""}/{a.data.string()}/{a.took.milliseconds()} {b.name}/{b.count}/{b.next?.name ?? "none"}"
      await sleep(1ms)
      try p.add(a)
      try p.add(b)
      do {
        try p.add(pantry.Item(name: "void", weight: 0, level: .low, tags: [], notes: [:], next: none, data: bytes(), took: 0ms, count: -1, fresh: false))
      } catch e {
        step = "add empty: {e.message}"
        await sleep(1ms)
      }
      let cooked = try await p.cook("stew", progress: f => progress.push(f))
      step = "cooked {cooked.name}/{cooked.count} after {progress.map(f => "{f}").joined(separator: ",")}"
      await sleep(1ms)
      do {
        let _ = try await p.cook("burnt", progress: f => {})
      } catch e {
        step = "burnt: {e.message}"
        await sleep(1ms)
      }
      do {
        try await withTimeout(5ms) {
          let _ = try await p.cook("forever", progress: f => {})
        }
      } catch e {
        step = "forever: {e.message}"
        await sleep(1ms)
      }
      let yes = try p.ask(question: q => q == "ready?")
      step = "asked ready?: {yes}"
      await sleep(1ms)
      let shelf = try p.open("top")
      try shelf.stack(3, .high)
      try shelf.close()
      do {
        let _ = try p.open("locked")
      } catch e {
        step = "locked: {e.message}"
        await sleep(1ms)
      }
      try p.show()
      let counts = try p.counts()
      step = "counts jars={counts["jars"] ?? 0} tins={counts["tins"] ?? 0}"
      await sleep(1ms)
      step = "shows {try p.shows(stepView)} {try p.shows(Ref())}"
      await sleep(1ms)
      while dial != 0.75 || picked == "" { await sleep(1ms) }
      step = "dial {dial} picked {picked}"
      await sleep(1ms)
      step = "done"
    } catch e {
      step = "fail {e.message}"
    }
  }
}

app { Probe() }
`

// The registry compiles for every desktop the C hosts run on (Zig
// cross-compiles: MinGW's headers for Windows).
func TestRegistryBuilds(t *testing.T) {
	zig, err := exec.LookPath("zig")
	if err != nil {
		t.Skip("no zig")
	}
	out := t.TempDir()
	for _, target := range []string{"x86_64-windows-gnu", "aarch64-windows-gnu", "x86_64-linux-gnu", "aarch64-linux-gnu"} {
		args := append([]string{"cc", "-target", target}, cbuild.Flags...)
		args = append(args, "-I", "include", "-I", "../../core/include", "-c", "src/native.c", "-o", filepath.Join(out, target+".o"))
		if b, err := exec.Command(zig, args...).CombinedOutput(); err != nil {
			t.Errorf("%s: %v\n%s", target, err, b)
		}
	}
}
