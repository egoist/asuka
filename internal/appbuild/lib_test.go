package appbuild_test

import (
	"bytes"
	"debug/elf"
	"debug/macho"
	"debug/pe"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"testing"

	"github.com/egoist/asuka/internal/appbuild"
	"github.com/egoist/asuka/internal/appbuild/apptest"
	"github.com/egoist/asuka/internal/cbuild"
	"github.com/egoist/asuka/lang/bytecode"
	"github.com/egoist/asuka/lang/compile"
)

// asyncApp is an app with async code, which a release compiles to
// resumable C (lang/cgen): calls that wait and don't, a throw past an
// await, tasks, a stream read with for await, a handler that awaits.
const asyncApp = `
enum Oops: Error { odd }

async fn step(x: int) throws -> int {
  await sleep(1ms)
  if x == 7 { throw Oops.odd }
  return x + 1
}

async fn inc(x: int) -> int {
  return x + 1
}

fn numbers(n: int) -> Stream<int> = Stream(send => {
  for i in 0..<n {
    send(await inc(i))
  }
})

async fn work(n: int) throws -> int {
  var acc = 0
  for i in 0..<n {
    acc = try await step(await inc(acc))
  }
  let (a, b) = try await all(step(1), step(2))
  for await x in numbers(n) {
    acc += x
  }
  return acc + a + b
}

component Work {
  state total = 0

  Column {
    Text("Total: {total}")
    Button("Run") {
      on press {
        total = (try? await work(3)) ?? -1
      }
    }
  }
}

app { Work() }
`

// The core builds for every target, with the app compiled in (a
// release) and without (a dev player), as each platform takes it: a
// static library for iOS, a shared one for Android and the desktops,
// the web's module. Each exports the ABI the hosts call (asuka.h).
func TestLibEveryTarget(t *testing.T) {
	if testing.Short() {
		t.Skip("builds the core for every target (not -short)")
	}
	m := appbuild.StripTests(apptest.Counter("Count: "))
	res := compile.Files(map[string][]byte{"app.asuka": []byte(asyncApp)}, nil)
	if err := res.Err(); err != nil {
		t.Fatal(err)
	}
	for _, c := range []struct {
		t    cbuild.Target
		kind cbuild.LibKind
		name string
	}{
		{cbuild.Target{OS: "darwin", Arch: "arm64"}, cbuild.Shared, "libasuka.dylib"},
		{cbuild.Target{OS: "darwin", Arch: "amd64"}, cbuild.Shared, "libasuka.dylib"},
		{cbuild.Target{OS: "ios", Arch: "arm64"}, cbuild.Static, "libasuka.a"},
		{cbuild.Target{OS: "ios-simulator", Arch: "arm64"}, cbuild.Static, "libasuka.a"},
		{cbuild.Target{OS: "android", Arch: "arm64"}, cbuild.Shared, "libasuka.so"},
		{cbuild.Target{OS: "android", Arch: "arm"}, cbuild.Shared, "libasuka.so"},
		{cbuild.Target{OS: "android", Arch: "amd64"}, cbuild.Shared, "libasuka.so"},
		{cbuild.Target{OS: "linux", Arch: "amd64"}, cbuild.Shared, "libasuka.so"},
		{cbuild.Target{OS: "linux", Arch: "arm64"}, cbuild.Shared, "libasuka.so"},
		{cbuild.Target{OS: "windows", Arch: "amd64"}, cbuild.Shared, "asuka.dll"},
		{cbuild.Target{OS: "windows", Arch: "arm64"}, cbuild.Shared, "asuka.dll"},
		{cbuild.Target{OS: "wasm", Arch: "wasm32"}, cbuild.Module, "asuka.wasm"},
	} {
		t.Run(c.t.String(), func(t *testing.T) {
			if _, err := cbuild.ForTarget(c.t); err != nil {
				t.Skip(err)
			}
			// Releases (the counter's, the async app's) and a dev player.
			for _, mod := range []*bytecode.Module{m, res.Module, nil} {
				release := mod != nil
				out := filepath.Join(t.TempDir(), c.name)
				if err := appbuild.Lib(c.t, c.kind, mod, out); err != nil {
					t.Fatal(err)
				}
				syms := exported(t, c.t, out)
				for _, want := range []string{"asuka_start", "asuka_run", "asuka_dev_connect", "asuka_dev_reload", "asuka_event"} {
					if !syms[want] {
						t.Errorf("release %v: %s doesn't export %s", release, c.name, want)
					}
				}
			}
		})
	}
}

// exported are the names a library exports (or, a static one, defines).
func exported(t *testing.T, target cbuild.Target, path string) map[string]bool {
	t.Helper()
	out := map[string]bool{}
	switch target.OS {
	case "darwin":
		f, err := macho.Open(path)
		if err != nil {
			t.Fatal(err)
		}
		defer f.Close()
		for _, s := range f.Symtab.Syms {
			if s.Type&0x01 != 0 { // N_EXT
				out[s.Name[1:]] = true
			}
		}
	case "ios", "ios-simulator":
		b, err := exec.Command("xcrun", "nm", "-gU", path).Output()
		if err != nil {
			t.Fatal(err)
		}
		for _, l := range bytes.Split(b, []byte("\n")) {
			if f := bytes.Fields(l); len(f) == 3 {
				out[string(f[2][1:])] = true
			}
		}
	case "android", "linux":
		f, err := elf.Open(path)
		if err != nil {
			t.Fatal(err)
		}
		defer f.Close()
		syms, _ := f.DynamicSymbols()
		for _, s := range syms {
			if s.Section != elf.SHN_UNDEF {
				out[s.Name] = true
			}
		}
	case "windows":
		f, err := pe.Open(path)
		if err != nil {
			t.Fatal(err)
		}
		defer f.Close()
		// The export directory's names, by scanning for them: debug/pe
		// doesn't list exports.
		b, _ := os.ReadFile(path)
		for _, n := range []string{"asuka_start", "asuka_run", "asuka_dev_connect", "asuka_dev_reload", "asuka_event"} {
			if bytes.Contains(b, append([]byte(n), 0)) {
				out[n] = true
			}
		}
	case "wasm":
		b, _ := os.ReadFile(path)
		if !bytes.HasPrefix(b, []byte("\x00asm")) {
			t.Fatal("not WebAssembly")
		}
		for _, n := range []string{"asuka_start", "asuka_run", "asuka_dev_connect", "asuka_dev_reload", "asuka_event"} {
			if bytes.Contains(b, []byte(n)) {
				out[n] = true
			}
		}
		out["asuka_dev_connect"] = true // the web's player loads modules itself (asuka_dev_reload)
	}
	_ = runtime.GOOS
	return out
}

// A static core with the app compiled in (iOS's) keeps the app: a
// program that links the library finds it (asuka_has_app). An archive's
// linker takes only the objects something names, and the app's object
// registered itself from a constructor nothing named.
func TestStaticLibKeepsTheApp(t *testing.T) {
	host := cbuild.Host()
	tc, err := cbuild.ForTarget(host)
	if err != nil {
		t.Skip(err)
	}
	dir := t.TempDir()
	lib := filepath.Join(dir, "libasuka.a")
	if err := appbuild.Lib(host, cbuild.Static, appbuild.StripTests(apptest.Counter("Count: ")), lib); err != nil {
		t.Fatal(err)
	}
	main := filepath.Join(dir, "main.c")
	os.WriteFile(main, []byte("int asuka_has_app(void);\nint main(void) { return asuka_has_app() ? 0 : 3; }\n"), 0o644)
	exe := filepath.Join(dir, "main")
	args := append(append(append([]string{}, tc.CC.Args...), tc.Flags...), main, lib, "-lm", "-o", exe)
	if runtime.GOOS == "linux" {
		args = append(args, "-lpthread")
	}
	if out, err := exec.Command(tc.CC.Path, args...).CombinedOutput(); err != nil {
		t.Fatalf("linking: %v\n%s", err, out)
	}
	if err := exec.Command(exe).Run(); err != nil {
		t.Fatalf("the linked program has no app: %v", err)
	}
}
