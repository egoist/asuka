package compile

import (
	"os"
	"path/filepath"
	"slices"
	"strings"
	"testing"

	bc "github.com/egoist/asuka/lang/bytecode"
	"github.com/egoist/asuka/lang/target"
)

// compileFor compiles files for platform p, failing on diagnostics.
func compileFor(t *testing.T, files map[string]string, p target.Platform) *Result {
	t.Helper()
	srcs := map[string][]byte{}
	for n, s := range files {
		srcs[n] = []byte(s)
	}
	res := Files(srcs, &Options{Platform: p})
	if err := res.Err(); err != nil {
		t.Fatalf("%s: %v", p, err)
	}
	if err := res.IR.Validate(); err != nil {
		t.Fatalf("%s: invalid IR: %v", p, err)
	}
	return res
}

// diagsFor is what compiling files for p reports.
func diagsFor(files map[string]string, p target.Platform) []string {
	srcs := map[string][]byte{}
	for n, s := range files {
		srcs[n] = []byte(s)
	}
	var out []string
	for _, d := range Files(srcs, &Options{Platform: p}).Diags {
		out = append(out, d.String())
	}
	return out
}

func hasString(m *bc.Module, s string) bool { return slices.Contains(m.Strings, s) }

func TestFileSuffix(t *testing.T) {
	for name, want := range map[string]string{
		"tray_linux.asuka":     "linux",
		"tray_macos.asuka":     "macos",
		"menu_desktop.asuka":   "macos windows linux",
		"haptics_mobile.asuka": "ios android",
		"a_b_web.asuka":        "web",
		"dir/x_ios.asuka":      "ios",
		"linux.asuka":          "",
		"_linux.asuka":         "",
		"__linux.asuka":        "",
		"tray_linuxes.asuka":   "",
		"tray_Linux.asuka":     "",
		"tray.asuka":           "",
		"tray_linux.go":        "",
	} {
		ps, _ := target.FileSuffix(name)
		var got []string
		for _, p := range ps {
			got = append(got, string(p))
		}
		if strings.Join(got, " ") != want {
			t.Errorf("%s: %q, want %q", name, got, want)
		}
	}
}

// A file whose name ends in a platform's suffix is part of its package
// only on that platform; _desktop and _mobile name groups of them.
func TestPlatformFiles(t *testing.T) {
	files := map[string]string{
		"main.asuka": `
test "greet" {
  print(greet())
  print(family())
}
`,
		"greet_ios.asuka":      `fn greet() -> string { return "hello from iOS" }`,
		"greet_android.asuka":  `fn greet() -> string { return "hello from Android" }`,
		"greet_desktop.asuka":  `fn greet() -> string { return "hello from a desktop" }`,
		"greet_web.asuka":      `fn greet() -> string { return "hello from the web" }`,
		"family_mobile.asuka":  `fn family() -> string { return "mobile" }`,
		"family_macos.asuka":   `fn family() -> string { return "apple desktop" }`,
		"family_windows.asuka": `fn family() -> string { return "pc" }`,
		"family_linux.asuka":   `fn family() -> string { return "pc" }`,
		"family_web.asuka":     `fn family() -> string { return "browser" }`,
	}
	greets := map[target.Platform]string{
		target.IOS: "hello from iOS", target.Android: "hello from Android", target.MacOS: "hello from a desktop",
		target.Windows: "hello from a desktop", target.Linux: "hello from a desktop", target.Web: "hello from the web",
	}
	for _, p := range target.All {
		m := compileFor(t, files, p).Module
		for q, g := range greets {
			if hasString(m, g) != (g == greets[p]) {
				t.Errorf("%s: has %q: %v (it's %s's)", p, g, hasString(m, g), q)
			}
		}
	}
	// On the VM and compiled to C.
	for p, want := range map[target.Platform]string{target.Linux: "hello from a desktop\npc", target.Android: "hello from Android\nmobile"} {
		out, err := runBoth(t, compileFor(t, files, p).Module)
		if err != nil {
			t.Fatalf("%s: %v\n%s", p, err, out)
		}
		if got := strings.TrimSpace(out); got != want+"\nok greet" {
			t.Errorf("%s: %q", p, got)
		}
	}
	// No platform: iOS's.
	if res := Files(map[string][]byte{"main.asuka": []byte(files["main.asuka"]), "greet_ios.asuka": []byte(greets[target.IOS]), "greet_linux.asuka": []byte("fn greet() -> string { return 1 }")}, nil); res.Platform != target.IOS || !res.PlatformFiles {
		t.Errorf("the default: %s, %v", res.Platform, res.PlatformFiles)
	}
}

// A name is declared once on each platform: in a file every platform
// has, or a file for each platform. The same is reported on every
// platform whose files declare it twice.
func TestPlatformFilesTwice(t *testing.T) {
	// A shared file's and a platform's: on every platform, at the shared
	// file's, once.
	files := map[string]string{
		"tray.asuka":       "fn showTray() {}\ntest \"t\" { showTray() }",
		"tray_linux.asuka": "fn showTray() {}",
	}
	for _, p := range target.All {
		d := diagsFor(files, p)
		if len(d) != 1 || d[0] != "tray.asuka:1:4: showTray is declared twice for Linux: here and in tray_linux.asuka (a name is declared once on each platform: in a file every platform has, or in a file for each platform)" {
			t.Errorf("%s: %q", p, d)
		}
	}
	// A group's and one of its platforms': on the group's platforms.
	files = map[string]string{
		"main.asuka":         `test "t" { showTray() }`,
		"tray_desktop.asuka": "fn showTray() {}",
		"tray_linux.asuka":   "fn showTray() {}",
		"tray_mobile.asuka":  "fn showTray() {}",
		"tray_web.asuka":     "fn showTray() {}",
	}
	for _, p := range target.All {
		d := diagsFor(files, p)
		want := 0
		if p == target.MacOS || p == target.Windows || p == target.Linux {
			want = 1
		}
		if len(d) != want || want == 1 && !strings.HasPrefix(d[0], "tray_desktop.asuka:1:4: showTray is declared twice for Linux: here and in tray_linux.asuka") {
			t.Errorf("%s: %q", p, d)
		}
	}
	// Two files of one platform's.
	files = map[string]string{
		"main.asuka":      `test "t" { f() }`,
		"a_ios.asuka":     "fn f() {}",
		"b_ios.asuka":     "fn f() {}",
		"c_android.asuka": "fn f() {}\nfn f() {}",
	}
	if d := diagsFor(files, target.IOS); len(d) != 1 || !strings.HasPrefix(d[0], "b_ios.asuka:1:4: f is declared twice for iOS: here and in a_ios.asuka") {
		t.Errorf("iOS: %q", d)
	}
	if d := diagsFor(files, target.Android); len(d) != 1 || !strings.Contains(d[0], "f is declared twice in package main") {
		t.Errorf("Android (the checker's own): %q", d)
	}
	if d := diagsFor(files, target.Web); len(d) != 1 || !strings.Contains(d[0], "unknown name f") && !strings.Contains(d[0], "f") {
		t.Errorf("the web: %q", d)
	}
	// An app block for each platform, and one every platform has.
	files = map[string]string{
		"app_mobile.asuka":  `app { Text("phone") }`,
		"app_desktop.asuka": `app { Text("computer") }`,
		"app_web.asuka":     `app { Text("page") }`,
	}
	for _, p := range target.All {
		if d := diagsFor(files, p); len(d) != 0 {
			t.Errorf("%s: %q", p, d)
		}
	}
	files["app.asuka"] = `app { Text("all") }`
	for _, p := range target.All {
		// Each platform's file with the one every platform has.
		if d := diagsFor(files, p); len(d) != 3 || !strings.HasPrefix(d[0], "app.asuka:1:1: the app block is declared twice for macOS, Windows and Linux: here and in app_desktop.asuka") {
			t.Errorf("%s: %q", p, d)
		}
	}
}

// Every branch is checked, whichever platform the program is for.
func TestPlatformBranchesChecked(t *testing.T) {
	files := map[string]string{"main.asuka": `
fn tray() {
  if @platform == .linux {
    showTrya()
  }
}
let x = match @platform {
  .web => 1 + "one"
  _ => 2
}
test "t" { tray() }
`}
	for _, p := range []target.Platform{target.IOS, target.Linux} {
		d := diagsFor(files, p)
		if len(d) != 2 || !strings.Contains(d[0], "showTrya") || !strings.HasPrefix(d[1], "main.asuka:8:") {
			t.Errorf("%s: %q", p, d)
		}
	}
	if d := diagsFor(map[string]string{"main.asuka": "let p = @os\n"}, target.IOS); len(d) != 1 || !strings.Contains(d[0], "no compile-time constant @os: there's @platform") {
		t.Errorf("@os: %q", d)
	}
	if d := diagsFor(map[string]string{"main.asuka": "test \"t\" { @platform = .linux }\n"}, target.IOS); len(d) != 1 {
		t.Errorf("assigned: %q", d)
	}
	// A match of @platform covers it without .unknown; one of another
	// Platform doesn't.
	if d := diagsFor(map[string]string{"main.asuka": `
let a = match @platform { .iOS | .android => 1; .macOS | .windows | .linux => 2; .web => 3 }
fn b(p: Platform) -> int { return match p { .iOS | .android => 1; .macOS | .windows | .linux => 2; .web => 3 } }
`}, target.IOS); len(d) != 1 || !strings.Contains(d[0], "match doesn't cover .unknown") {
		t.Errorf("exhaustive: %q", d)
	}
}

// What @platform doesn't take is left out of the module: its code, its
// strings, the builtins only it calls.
func TestPlatformFolds(t *testing.T) {
	files := map[string]string{"main.asuka": `
let banner = if @platform == .linux { "penguin".upper() } else { "apple" }
let both = @platform == .linux && "and linux".count() > 0
let either = @platform != .linux || "or linux".isEmpty()

fn shout() -> string {
  if @platform == .windows {
    return "windows".lower()
  } else if @platform == .linux {
    return "{[3, 1, 2].sorted().count()}linux elif"
  }
  return match @platform {
    .web => "the web"
    _ => "elsewhere"
  }
}

component Tray {
  Column {
    if @platform == .linux {
      Text("Linux tray")
    } else {
      Text("Another tray")
    }
    match @platform {
      .macOS => Text("Menu bar extra")
      _ => Text("No menu bar")
    }
  }
}

app { Tray() }

test "t" {
  print(banner)
  print(both)
  print(either)
  print(shout())
}
`}
	ios := compileFor(t, files, target.IOS).Module
	linux := compileFor(t, files, target.Linux).Module
	for _, s := range []string{"penguin", "and linux", "or linux", "windows", "linux elif", "the web", "Linux tray", "Menu bar extra"} {
		if hasString(ios, s) {
			t.Errorf("iOS has %q", s)
		}
	}
	for _, s := range []string{"apple", "elsewhere", "Another tray", "No menu bar"} {
		if !hasString(ios, s) {
			t.Errorf("iOS hasn't %q", s)
		}
	}
	for _, s := range []string{"penguin", "and linux", "or linux", "linux elif", "Linux tray", "No menu bar"} {
		if !hasString(linux, s) {
			t.Errorf("Linux hasn't %q", s)
		}
	}
	for _, s := range []string{"apple", "windows", "the web", "Another tray", "Menu bar extra"} {
		if hasString(linux, s) {
			t.Errorf("Linux has %q", s)
		}
	}
	for _, b := range []string{"str.upper", "str.count", "str.lower", "arr.sorted", "ui.show", "ui.switch"} {
		if slices.Contains(ios.Builtins, b) {
			t.Errorf("iOS calls %s", b)
		}
	}
	for _, b := range []string{"str.upper", "arr.sorted"} {
		if !slices.Contains(linux.Builtins, b) {
			t.Errorf("Linux doesn't call %s", b)
		}
	}
	// What each prints, on the VM and compiled to C.
	for m, want := range map[*bc.Module]string{ios: "apple\nfalse\ntrue\nelsewhere", linux: "PENGUIN\ntrue\nfalse\n3linux elif"} {
		out, err := runBoth(t, m)
		if err != nil {
			t.Fatalf("%v\n%s", err, out)
		}
		if got := strings.TrimSpace(out); got != want+"\nok t" {
			t.Errorf("got %q, want %q", got, want)
		}
	}
}

// A package's platform files are chosen as Dir reads it, an import's
// (std's and dependencies' too) likewise; DirPlatforms checks every
// platform's, saying which a diagnostic is for when not every one's.
func TestDirPlatforms(t *testing.T) {
	dir := t.TempDir()
	write := func(name, src string) {
		p := filepath.Join(dir, filepath.FromSlash(name))
		os.MkdirAll(filepath.Dir(p), 0o755)
		if err := os.WriteFile(p, []byte(src), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	write("main.asuka", "import ./tray\ntest \"t\" { tray.show() }\n")
	write("tray/tray_desktop.asuka", "pub fn show() { print(\"tray\") }\n")
	write("tray/tray_mobile.asuka", "pub fn show() { print(\"none\") }\n")
	write("tray/tray_linux.asuka", "fn linuxOnly() { nope() }\n")
	write("tray/tray_web.asuka", "pub fn shown() {}\n")
	res, err := Dir(dir, &Options{Platform: target.Windows})
	if err != nil {
		t.Fatal(err)
	}
	if res.Err() != nil || !hasString(res.Module, "tray") || hasString(res.Module, "none") {
		t.Fatalf("Windows: %v", res.Err())
	}
	all, err := DirPlatforms(dir, nil, nil)
	if err != nil {
		t.Fatal(err)
	}
	var got []string
	for _, d := range all.Diags {
		r, _ := filepath.Rel(dir, d.Pos.Filename)
		d.Pos.Filename = filepath.ToSlash(r)
		got = append(got, d.String())
	}
	if len(got) != 2 || !strings.HasPrefix(got[0], "tray/tray_linux.asuka:1:18: ") || !strings.HasSuffix(got[0], " (for Linux)") ||
		!strings.HasPrefix(got[1], "main.asuka:2:") || !strings.HasSuffix(got[1], " (for the web)") {
		t.Errorf("%q", got)
	}
	if len(all.Results) != len(target.All) {
		t.Errorf("%d results", len(all.Results))
	}
	// Without platforms' files: one compile, every platform's.
	write2 := t.TempDir()
	os.WriteFile(filepath.Join(write2, "main.asuka"), []byte("let x: int = \"no\"\n"), 0o644)
	all, err = DirPlatforms(write2, nil, nil)
	if err != nil || len(all.Diags) != 1 || all.Diags[0].Platforms != nil || all.Results[target.Web] != all.First {
		t.Errorf("%v %+v", err, all)
	}
}
