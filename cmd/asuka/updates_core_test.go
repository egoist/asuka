package main

import (
	"encoding/json"
	"fmt"
	"maps"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"runtime"
	"slices"
	"strconv"
	"strings"
	"testing"

	"github.com/egoist/asuka/internal/cbuild"
	iupdate "github.com/egoist/asuka/internal/update"
	"github.com/egoist/asuka/lang/compile"
)

// TestUpdatesVerifiedByCore checks that what the CLI makes, the app's
// updater takes: a key pair from asuka update keygen, two versions'
// update files from writeUpdates (the archive, the delta, the manifest,
// signed with that key, as asuka build and publish write them), read by
// platform/update on the fake host, its downloads and signatures checked and
// its deltas applied by the C core. The new version's tree it stages is
// the build's. With the archive alone (the installed app changed, so its
// delta fails) it's the same; with another key, nothing is installed.
func TestUpdatesVerifiedByCore(t *testing.T) {
	if _, err := cbuild.Find(); err != nil {
		t.Skip(err)
	}
	keys := t.TempDir()
	if err := cmdUpdate([]string{"keygen", "--out", keys}); err != nil {
		t.Fatal(err)
	}
	pubText, err := os.ReadFile(filepath.Join(keys, "asuka-update.pub"))
	if err != nil {
		t.Fatal(err)
	}
	pub := strings.TrimSpace(string(pubText))
	t.Setenv("ASUKA_UPDATE_PUBLIC_KEY", "")
	t.Setenv("ASUKA_UPDATE_URL", "")
	t.Setenv("ASUKA_UPDATE_PRIVATE_KEY", filepath.Join(keys, "asuka-update.key"))

	runner, err := compile.Runner(false)
	if err != nil {
		t.Fatal(err)
	}
	for _, kind := range []iupdate.Kind{iupdate.Folder, iupdate.Prefix, iupdate.Bundle} {
		kindName := map[iupdate.Kind]string{iupdate.Folder: "windows", iupdate.Prefix: "linux", iupdate.Bundle: "mac"}[kind]
		t.Run(kindName, func(t *testing.T) {
			published := t.TempDir()
			srv := httptest.NewServer(http.FileServer(http.Dir(published)))
			defer srv.Close()
			appDir := t.TempDir()
			writeFiles(t, appDir, map[string]string{"CHANGELOG.md": "# Changes\n\n## 1.0.1\n\n- Faster.\n\n## 1.0.0\n\nFirst.\n"})
			app := &asukaApp{dir: appDir, name: "notes", bundleID: "com.example.notes", cfg: &appConfig{
				Name: "Notes", Updates: &updatesConfig{PublicKey: pub, URL: srv.URL + "/notes"},
			}}
			target := "test-" + kindName
			build := func(version string) updateBuild {
				out := t.TempDir()
				app.cfg.Version = version
				b := updateBuild{target: target, kind: kind}
				switch kind {
				case iupdate.Folder:
					b.dir = filepath.Join(out, "Notes")
					b.root = b.dir
					writeFiles(t, b.root, map[string]string{"Notes.exe": code(version), "assets/a.txt": "a", "assets/" + version + ".txt": version})
					if err := writeFileList(b); err != nil {
						t.Fatal(err)
					}
					entries, _ := os.ReadDir(b.root)
					for _, e := range entries {
						b.entries = append(b.entries, e.Name())
					}
				case iupdate.Prefix:
					b.dir = filepath.Join(out, "linux")
					b.root = filepath.Join(b.dir, "notes")
					writeFiles(t, b.root, map[string]string{"bin/notes": code(version), "share/notes/assets/a.txt": "a", "share/notes/assets/" + version + ".txt": version, "share/applications/com.example.notes.desktop": "[Desktop Entry]"})
					if err := writeFileList(b); err != nil {
						t.Fatal(err)
					}
					b.tarball = filepath.Join(b.dir, "notes-"+version+"-linux-amd64.tar.gz")
					if err := tarGz(b.root, b.tarball); err != nil {
						t.Fatal(err)
					}
				case iupdate.Bundle:
					b.dir = out
					b.root = filepath.Join(out, "Notes.app")
					b.entries = []string{"Notes.app"}
					writeFiles(t, b.root, map[string]string{"Contents/MacOS/Notes": code(version), "Contents/Info.plist": "<plist/>", "Contents/Resources/" + version + ".txt": version})
				}
				files, err := writeUpdates(app, out, b, true)
				if err != nil {
					t.Fatal(err)
				}
				os.MkdirAll(filepath.Join(published, "notes"), 0o755)
				for _, f := range files {
					if err := linkOrCopy(f, filepath.Join(published, "notes", filepath.Base(f))); err != nil {
						t.Fatal(err)
					}
				}
				return b
			}
			old := build("1.0.0")
			nw := build("1.0.1")
			var m iupdate.Manifest
			data, _ := os.ReadFile(filepath.Join(published, "notes", iupdate.ManifestName(target, "")))
			if err := json.Unmarshal(data, &m); err != nil || m.Version != "1.0.1" || len(m.Deltas) != 1 {
				t.Fatalf("the manifest: %+v, %v", m, err)
			}

			// installAs runs platform/update in an app of version 1.0.0 installed in
			// a copy of the first build (changed, if asked), with key: it returns
			// what the program printed, and the tree it staged.
			installAs := func(t *testing.T, key string, changed bool) (string, string) {
				root := filepath.Join(t.TempDir(), filepath.Base(old.root))
				if err := os.CopyFS(root, os.DirFS(old.root)); err != nil {
					t.Fatal(err)
				}
				exe, work, kindText := filepath.Join(root, "Notes.exe"), root, "folder"
				switch kind {
				case iupdate.Prefix:
					exe, work, kindText = filepath.Join(root, "bin", "notes"), filepath.Join(root, "share", "notes"), "prefix"
				case iupdate.Bundle:
					exe, work, kindText = filepath.Join(root, "Contents", "MacOS", "Notes"), filepath.Dir(root), "bundle"
				}
				if changed {
					os.WriteFile(exe, []byte("changed since"), 0o755)
				}
				var serve strings.Builder
				entries, _ := os.ReadDir(filepath.Join(published, "notes"))
				for _, e := range entries {
					fmt.Fprintf(&serve, "  try await host.serve(update.Served(url: %s, file: %s))\n",
						quoteAsuka(srv.URL+"/notes/"+e.Name()), quoteAsuka(filepath.Join(published, "notes", e.Name())))
				}
				src := fmt.Sprintf(`import platform/update

		component Root {
		  Text("notes")
		}

		app { Root() }

		test "the CLI's update, installed" {
		  let app = mount(Root())
		  let host = try await update.fakeHost(update.FakeApp(version: "1.0.0", name: "Notes", target: %s, feed: %s,
		    key: %s, kind: %s, root: %s, exe: %s, work: %s))
		%s  let r = (try await update.check())!
		  print("release {r.version} {r.size}")
		  do {
		    try await r.install()
		  } catch e {
		    print("failed: {e.message}")
		  }
		  let log = try await host.log()
		  print("downloads {log.downloads.count()}")
		  for s in log.installed { print("staged {s}") }
		}
		`, quoteAsuka(target), quoteAsuka(app.cfg.Updates.feed(target)), quoteAsuka(key), quoteAsuka(kindText), quoteAsuka(root), quoteAsuka(exe), quoteAsuka(work), serve.String())
				res := compile.Files(map[string][]byte{"cli_update.asuka": []byte(src)}, &compile.Options{Platform: osPlatform(runtime.GOOS)})
				if err := res.Err(); err != nil {
					t.Fatalf("compile:\n%v\n%s", err, src)
				}
				out, err := compile.Run(runner, res.Module, true, "")
				if err != nil {
					t.Fatalf("%v\n%s", err, out)
				}
				staged := ""
				for _, line := range strings.Split(out, "\n") {
					if s, ok := strings.CutPrefix(line, "staged "); ok {
						staged = s
					}
				}
				return out, staged
			}
			want := readFiles(t, nw.root)

			t.Run("delta", func(t *testing.T) {
				out, staged := installAs(t, pub, false)
				if !strings.Contains(out, "release 1.0.1 "+strconv.FormatInt(m.Deltas[0].Size, 10)+"\n") || !strings.Contains(out, "downloads 2\n") || staged == "" {
					t.Fatalf("the delta's install:\n%s", out)
				}
				if got := readFiles(t, staged); !maps.Equal(got, want) {
					t.Errorf("staged %v, want %v", slices.Sorted(maps.Keys(got)), slices.Sorted(maps.Keys(want)))
				}
			})
			t.Run("archive", func(t *testing.T) {
				out, staged := installAs(t, pub, true)
				if !strings.Contains(out, "downloads 3\n") || staged == "" {
					t.Fatalf("the archive's install:\n%s", out)
				}
				if got := readFiles(t, staged); !maps.Equal(got, want) {
					t.Errorf("staged %v, want %v", slices.Sorted(maps.Keys(got)), slices.Sorted(maps.Keys(want)))
				}
			})
			t.Run("another key", func(t *testing.T) {
				other, _ := testKeys(t)
				out, staged := installAs(t, other, false)
				if staged != "" || !strings.Contains(out, "failed: update: the update is not signed with the app's key") {
					t.Fatalf("installed with another key:\n%s", out)
				}
			})
		})
	}
}

// quoteAsuka is an Asuka string literal of s.
func quoteAsuka(s string) string {
	r := strings.NewReplacer(`\`, `\\`, `"`, `\"`, "{", `\{`, "}", `\}`, "\n", `\n`)
	return `"` + r.Replace(s) + `"`
}
