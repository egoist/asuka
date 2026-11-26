package main

import (
	"bytes"
	"errors"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"text/template"

	"github.com/egoist/asuka/internal/guide"
	"github.com/egoist/asuka/internal/toml"
	tpl "github.com/egoist/asuka/template"
)

// asukaCheckout returns the Asuka source tree this CLI was built from, when
// it runs from a checkout (go run ./cmd/asuka, or go install from one).
func asukaCheckout() string {
	_, file, _, ok := runtime.Caller(0)
	if !ok {
		return ""
	}
	root := filepath.Dir(filepath.Dir(filepath.Dir(file)))
	if data, err := os.ReadFile(filepath.Join(root, "go.mod")); err == nil && bytes.Contains(data, []byte("module github.com/egoist/asuka\n")) {
		return root
	}
	return ""
}

func cmdNew(args []string) error {
	flags := newFlagSet("new", "<dir>")
	flags.about = `Makes a project in dir: an app (the template's: asuka.toml, app.asuka with
a component and a test block), with --bin a program, a command-line tool
(asuka.toml, main.asuka with fn main and a test block), or with --lib a
library, an Asuka package other apps import (asuka.toml, <name>.asuka with
a function and a test block). A library with --native also declares a native module, which each
platform's own code implements: a Swift package for iOS and macOS
(Package.swift, swift/), a Gradle module for Android (android/), JavaScript
for the web (web/), and C for Windows and Linux (desktop/); asuka native
add adds one later, and asuka native generate (which dev and build run
too) connects them.`
	var lib, native, bin bool
	only := strings.Join(nativeProjects, ",")
	flags.Bool(&bin, "bin", 0, "make a program (a command-line tool), not an app")
	flags.Bool(&lib, "lib", 0, "make a library, not an app")
	flags.Bool(&native, "native", 0, "a library with a native module and each platform's code (implies --lib)")
	flags.String(&only, "projects", 0, only, "with --native: the platforms' projects to make")
	if err := flags.parse(args); err != nil {
		return err
	}
	if len(flags.Args()) != 1 {
		return errors.New("usage: asuka new [--bin | --lib [--native [--projects swift,android,web,desktop]]] <dir>")
	}
	dir := flags.Arg(0)
	if entries, err := os.ReadDir(dir); err == nil && len(entries) > 0 {
		return fmt.Errorf("%s is not empty", dir)
	}
	if bin && (lib || native) {
		return errors.New("asuka new: --bin makes a program, --lib a library: one or the other")
	}
	if bin {
		return newBin(dir)
	}
	if lib || native {
		return newLib(dir, native, strings.Split(only, ","))
	}
	abs, err := filepath.Abs(dir)
	if err != nil {
		return err
	}
	name := filepath.Base(abs)
	data := struct{ Name, ID string }{
		Name: name,
		ID:   "com.example." + strings.Trim(strings.ToLower(idPart.ReplaceAllString(name, "")), "-"),
	}
	err = fs.WalkDir(tpl.Files, "files", func(path string, d fs.DirEntry, err error) error {
		if err != nil || d.IsDir() {
			return err
		}
		src, _ := tpl.Files.ReadFile(path)
		rel := strings.TrimSuffix(strings.TrimPrefix(path, "files/"), ".tmpl")
		if rel == "gitignore" {
			rel = ".gitignore"
		}
		t, err := template.New(rel).Funcs(template.FuncMap{"quote": toml.Quote}).Parse(string(src))
		if err != nil {
			return err
		}
		var out bytes.Buffer
		if err := t.Execute(&out, data); err != nil {
			return err
		}
		dst := filepath.Join(abs, rel)
		os.MkdirAll(filepath.Dir(dst), 0o755)
		return os.WriteFile(dst, out.Bytes(), 0o644)
	})
	if err != nil {
		return err
	}
	if err := os.WriteFile(filepath.Join(abs, "AGENTS.md"), []byte(guide.AgentsMD(guide.App)), 0o644); err != nil {
		return err
	}
	fmt.Printf("asuka: made %s\n\n  cd %s\n  asuka dev --platform ios    # or android, web; on macOS, Windows or Linux, asuka dev runs it on this computer\n  asuka test                  # its test blocks\n", dir, dir)
	return nil
}

// newBin makes a program in dir (docs/programs.md): asuka.toml, and
// main.asuka, whose main greets its first argument, with a test block.
func newBin(dir string) error {
	name := filepath.Base(mustAbs(dir))
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return err
	}
	files := map[string]string{
		"asuka.toml": fmt.Sprintf("name = %s\nversion = \"0.1.0\"\n", toml.Quote(name)),
		".gitignore": "dist/\n",
		"AGENTS.md":  guide.AgentsMD(guide.Program),
		"main.asuka": `// A program: asuka run runs it (asuka run -- Ada), asuka build makes it
// an executable in dist/.
import std/os

fn main() {
  let name = os.args().first() ?? "world"
  print("Hello, {name}!")
}

test "greets whom it's given" {
  let o = os.fake(args: ["Ada"])
  expect(await o.run(main) == 0)
  expect(o.stdout() == "Hello, Ada!\n")
}
`,
	}
	for f, src := range files {
		if err := os.WriteFile(filepath.Join(dir, f), []byte(src), 0o644); err != nil {
			return err
		}
	}
	fmt.Printf("asuka: made %s\n\n  cd %s\n  asuka run -- Ada   # run it, with its arguments\n  asuka test         # its test blocks\n  asuka build        # an executable in dist/\n", dir, dir)
	return nil
}
