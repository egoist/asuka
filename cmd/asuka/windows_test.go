package main

import (
	"bytes"
	"debug/pe"
	"encoding/binary"
	"encoding/xml"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"slices"
	"strings"
	"testing"

	"github.com/egoist/asuka/internal/cbuild"
)

func TestWindowsArch(t *testing.T) {
	for in, want := range map[string]string{"": "amd64", "amd64": "amd64", "arm64": "arm64"} {
		if got, err := windowsArch(in); got != want || err != nil {
			t.Errorf("windowsArch(%q) = %q, %v", in, got, err)
		}
	}
	if _, err := windowsArch("386"); err == nil {
		t.Error("windowsArch(386): no error")
	}
}

// The resources' object is the build's machine's, with its relocations.
func TestWinresMachine(t *testing.T) {
	app := &asukaApp{name: "Notes", dir: t.TempDir()}
	for arch, machine := range map[string]uint16{"amd64": 0x8664, "arm64": 0xAA64} {
		b, err := winresSyso(app, arch)
		if err != nil {
			t.Fatal(err)
		}
		if m := binary.LittleEndian.Uint16(b); m != machine {
			t.Errorf("%s: machine %#x, want %#x", arch, m, machine)
		}
	}
}

// Without a certificate the .exe isn't signed; with one but no password
// set, or no file, it says so.
func TestSignWindows(t *testing.T) {
	exe := filepath.Join(t.TempDir(), "App.exe")
	os.WriteFile(exe, []byte("MZ"), 0o644)
	t.Setenv("ASUKA_WINDOWS_CERT", "")
	if signed, err := signWindows(exe, "App", false); signed || err != nil {
		t.Fatalf("no certificate: %v, %v", signed, err)
	}
	t.Setenv("ASUKA_WINDOWS_CERT", filepath.Join(t.TempDir(), "missing.pfx"))
	if _, err := signWindows(exe, "App", false); err == nil || !strings.Contains(err.Error(), "ASUKA_WINDOWS_CERT") {
		t.Fatalf("a missing certificate: %v", err)
	}
	pfx := filepath.Join(t.TempDir(), "cert.pfx")
	os.WriteFile(pfx, []byte("x"), 0o600)
	t.Setenv("ASUKA_WINDOWS_CERT", pfx)
	os.Unsetenv("ASUKA_WINDOWS_CERT_PASSWORD")
	if _, err := signWindows(exe, "App", false); err == nil || !strings.Contains(err.Error(), "ASUKA_WINDOWS_CERT_PASSWORD") {
		t.Fatalf("no password: %v", err)
	}
	t.Setenv("ASUKA_WINDOWS_CERT_PASSWORD", "secret")
	ts := defaultTimestamps[0]
	tool, args, err := windowsSigner(exe, "App", pfx, "secret", ts)
	if err != nil {
		t.Skipf("no signer here: %v", err)
	}
	joined := strings.Join(args, " ")
	if runtime.GOOS == "windows" {
		if !strings.Contains(joined, "/fd SHA256 /tr "+ts+" /td SHA256") {
			t.Errorf("%s %s", tool, joined)
		}
	} else if !strings.Contains(joined, "-h sha256") || !strings.Contains(joined, "-ts "+ts) {
		t.Errorf("%s %s", tool, joined)
	}
	if r := strings.Join(redact(args, "secret"), " "); strings.Contains(r, "secret") {
		t.Errorf("the password is printed: %s", r)
	}
}

func TestTimestampServers(t *testing.T) {
	if got := timestampServers(""); !slices.Equal(got, defaultTimestamps) {
		t.Errorf("default: %v", got)
	}
	if got := timestampServers("None"); got != nil {
		t.Errorf("none: %v", got)
	}
	if got := timestampServers(" http://a , http://b,"); !slices.Equal(got, []string{"http://a", "http://b"}) {
		t.Errorf("a list: %v", got)
	}
	if !timestampFailed("HTTP failure: Failed to get data from http://x\nRFC 3161 timestamping failed") ||
		!timestampFailed("SignTool Error: The specified timestamp server either could not be reached or returned an invalid response.") ||
		timestampFailed("Failed to read PKCS#12 file") {
		t.Error("timestampFailed")
	}
}

// With osslsigncode (and openssl, for a self-signed certificate), a PE is
// signed and the signature verifies against the certificate; with no
// timestamp server answering, signing fails, saying so.
func TestSignWindowsPE(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("signtool signs on Windows")
	}
	if _, err := exec.LookPath("osslsigncode"); err != nil {
		t.Skip("no osslsigncode")
	}
	if _, err := exec.LookPath("openssl"); err != nil {
		t.Skip("no openssl")
	}
	dir := t.TempDir()
	key, crt, pfx := filepath.Join(dir, "key.pem"), filepath.Join(dir, "cert.pem"), filepath.Join(dir, "cert.pfx")
	for _, args := range [][]string{
		{"req", "-x509", "-newkey", "rsa:2048", "-sha256", "-days", "2", "-nodes", "-keyout", key, "-out", crt,
			"-subj", "/CN=Asuka Test", "-addext", "keyUsage=critical,digitalSignature", "-addext", "extendedKeyUsage=codeSigning"},
		{"pkcs12", "-export", "-out", pfx, "-inkey", key, "-in", crt, "-passout", "pass:secret"},
	} {
		if b, err := exec.Command("openssl", args...).CombinedOutput(); err != nil {
			t.Skipf("openssl %s: %v\n%s", args[0], err, b)
		}
	}
	exe := filepath.Join(dir, "App.exe")
	if err := os.WriteFile(exe, tinyPE(), 0o644); err != nil {
		t.Fatal(err)
	}
	t.Setenv("ASUKA_WINDOWS_CERT", pfx)
	t.Setenv("ASUKA_WINDOWS_CERT_PASSWORD", "secret")

	// No server answers (nothing listens on port 1): a clear error, the
	// .exe as it was.
	t.Setenv("ASUKA_WINDOWS_TIMESTAMP", "http://127.0.0.1:1,http://127.0.0.1:2")
	if _, err := signWindows(exe, "App", false); err == nil || !strings.Contains(err.Error(), "no timestamp server answered (http://127.0.0.1:1, http://127.0.0.1:2)") ||
		!strings.Contains(err.Error(), "ASUKA_WINDOWS_TIMESTAMP") {
		t.Fatalf("no timestamp server: %v", err)
	}
	if b, _ := os.ReadFile(exe); !bytes.Equal(b, tinyPE()) {
		t.Fatal("the .exe changed though signing failed")
	}
	if _, err := os.Stat(exe + ".signed"); err == nil {
		t.Error("osslsigncode's output is left behind")
	}

	// Without a timestamp: signed.
	t.Setenv("ASUKA_WINDOWS_TIMESTAMP", "none")
	signed, err := signWindows(exe, "App", false)
	if err != nil || !signed {
		t.Fatalf("signing: %v, %v", signed, err)
	}
	out, err := exec.Command("osslsigncode", "verify", "-CAfile", crt, "-in", exe).CombinedOutput()
	if err != nil || !strings.Contains(string(out), "Signature verification: ok") || !strings.Contains(string(out), "Text description: App") {
		t.Fatalf("verify: %v\n%s", err, out)
	}
}

// tinyPE is the smallest PE32+ osslsigncode signs: headers and one
// section of code (ret).
func tinyPE() []byte {
	const fileAlign, sectAlign = 0x200, 0x1000
	b := make([]byte, 2*fileAlign)
	le := binary.LittleEndian
	copy(b, "MZ")
	le.PutUint32(b[0x3c:], 0x40) // e_lfanew
	copy(b[0x40:], "PE\x00\x00")
	coff := b[0x44:]
	le.PutUint16(coff[0:], 0x8664) // machine: amd64
	le.PutUint16(coff[2:], 1)      // sections
	le.PutUint16(coff[16:], 240)   // optional header's size
	le.PutUint16(coff[18:], 0x22)  // executable, large address aware
	opt := b[0x58:]
	le.PutUint16(opt[0:], 0x20b)        // PE32+
	le.PutUint32(opt[4:], fileAlign)    // size of code
	le.PutUint32(opt[16:], sectAlign)   // entry point
	le.PutUint32(opt[20:], sectAlign)   // base of code
	le.PutUint64(opt[24:], 0x140000000) // image base
	le.PutUint32(opt[32:], sectAlign)
	le.PutUint32(opt[36:], fileAlign)
	le.PutUint16(opt[40:], 6)           // OS version
	le.PutUint16(opt[48:], 6)           // subsystem version
	le.PutUint32(opt[56:], 2*sectAlign) // size of image
	le.PutUint32(opt[60:], fileAlign)   // size of headers
	le.PutUint16(opt[68:], 2)           // subsystem: GUI
	le.PutUint16(opt[70:], 0x8160)      // DLL characteristics: ASLR, DEP, high entropy, terminal server aware
	le.PutUint64(opt[72:], 0x100000)
	le.PutUint64(opt[80:], 0x1000)
	le.PutUint64(opt[88:], 0x100000)
	le.PutUint64(opt[96:], 0x1000)
	le.PutUint32(opt[108:], 16) // data directories
	sect := b[0x58+240:]
	copy(sect, ".text")
	le.PutUint32(sect[8:], 1)           // virtual size
	le.PutUint32(sect[12:], sectAlign)  // virtual address
	le.PutUint32(sect[16:], fileAlign)  // raw size
	le.PutUint32(sect[20:], fileAlign)  // raw offset
	le.PutUint32(sect[36:], 0x60000020) // code, execute, read
	b[fileAlign] = 0xc3                 // ret
	return b
}

// A Windows build carries asuka.toml's ID and scheme in the host's
// settings, which it registers and runs once by.
func TestWindowsHostConfig(t *testing.T) {
	app := &asukaApp{name: "notes", cfg: &appConfig{ID: "com.example.notes", Scheme: "notes", Version: "1.2", Build: 3}}
	c := app.hostConfig(platformWindows, "windows-amd64", true)
	if c.ID != "com.example.notes" || c.Scheme != "notes" || c.Version != "1.2" || c.Build != "3" || c.MultipleInstances {
		t.Errorf("%+v", c)
	}
	no := false
	app.cfg.SingleInstance = &no
	if c := app.hostConfig(platformWindows, "", false); !c.MultipleInstances {
		t.Errorf("singleInstance false: %+v", c)
	}
}

// A Windows build is a folder: one executable (the host, the core, the
// app and, as it imports std/sqlite, SQLite, its resources linked in), the
// app's settings. The executable needs only what Windows has: no MinGW
// runtime's DLLs.
func TestBuildWindows(t *testing.T) {
	needDesktopBuild(t, cbuild.Target{OS: "windows", Arch: "amd64"})
	t.Setenv("ASUKA_WINDOWS_CERT", "")
	useSQLiteCounter(t, "Count: ")
	app := sqliteTestApp(t)
	for arch, machine := range map[string]uint16{"amd64": pe.IMAGE_FILE_MACHINE_AMD64, "arm64": pe.IMAGE_FILE_MACHINE_ARM64} {
		out := t.TempDir()
		if err := buildWindows(app, &buildOptions{out: out, arch: arch}); err != nil {
			t.Fatal(err)
		}
		dir := filepath.Join(out, "windows-"+arch, sanitizeExe(app.displayName()))
		if _, err := os.Stat(filepath.Join(out, "windows-"+arch, "sqliteapp-"+app.cfg.Version+"-windows-"+arch+".zip")); err != nil {
			t.Error(err)
		}
		// Its installer, by default: a GUI program for the architecture,
		// the app in its resources.
		setup := filepath.Join(out, "windows-"+arch, "sqliteapp-"+app.cfg.Version+"-windows-"+arch+"-setup.exe")
		if f, err := pe.Open(setup); err != nil {
			t.Error(err)
		} else {
			if f.Machine != machine || subsystem(f) != pe.IMAGE_SUBSYSTEM_WINDOWS_GUI {
				t.Errorf("%s: the installer's machine %#x, subsystem %d", arch, f.Machine, subsystem(f))
			}
			if r := f.Section(".rsrc"); r == nil || r.Size < 100_000 {
				t.Errorf("%s: the installer hasn't the app in its resources", arch)
			}
			f.Close()
		}
		if _, err := os.Stat(filepath.Join(out, "windows-"+arch, "sqliteapp-"+app.cfg.Version+"-windows-"+arch+".msix")); err == nil {
			t.Errorf("%s: an MSIX nobody asked for", arch)
		}
		entries, _ := os.ReadDir(dir)
		var names []string
		for _, e := range entries {
			names = append(names, e.Name())
		}
		if want := []string{"asuka-app.json", "sqliteapp.exe"}; !slices.Equal(names, want) {
			t.Errorf("%s: %v, want %v", arch, names, want)
		}
		checkSQLiteLinked(t, filepath.Join(dir, "sqliteapp.exe"))
		for _, n := range []string{"sqliteapp.exe"} {
			f, err := pe.Open(filepath.Join(dir, n))
			if err != nil {
				t.Fatal(err)
			}
			if f.Machine != machine {
				t.Errorf("%s: %s's machine is %#x", arch, n, f.Machine)
			}
			// A GUI program: no console window opens with it.
			if sub := subsystem(f); sub != pe.IMAGE_SUBSYSTEM_WINDOWS_GUI {
				t.Errorf("%s: %s's subsystem is %d, not a GUI program's", arch, n, sub)
			}
			if n == "sqliteapp.exe" && f.Section(".rsrc") == nil {
				t.Errorf("%s: the executable has no resources (its icon, manifest, version)", arch)
			}
			if n == "sqliteapp.exe" {
				libs, _ := f.ImportedLibraries()
				for _, l := range libs {
					if l = strings.ToLower(l); strings.HasPrefix(l, "lib") || strings.Contains(l, "pthread") || strings.Contains(l, "gcc") {
						t.Errorf("%s: the executable imports %s", arch, l)
					}
				}
			}
			f.Close()
		}
	}
	if m, _ := filepath.Glob(filepath.Join(app.dir, "*.syso")); len(m) > 0 {
		t.Errorf("left in the app: %v", m)
	}
}

// subsystem is an executable's subsystem.
func subsystem(f *pe.File) uint16 {
	switch h := f.OptionalHeader.(type) {
	case *pe.OptionalHeader64:
		return h.Subsystem
	case *pe.OptionalHeader32:
		return h.Subsystem
	}
	return 0
}

// ASUKA_WINDOWS_SIGN_COMMAND signs with a tool of the user's: %1 is the
// file, whose path may have spaces; a command without %1 is refused.
func TestSignWindowsCommand(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("sh's quoting")
	}
	dir := filepath.Join(t.TempDir(), "My App")
	os.MkdirAll(dir, 0o755)
	exe := filepath.Join(dir, "My App.exe")
	os.WriteFile(exe, []byte("MZ"), 0o644)
	t.Setenv("ASUKA_WINDOWS_CERT", "")
	t.Setenv("ASUKA_WINDOWS_SIGN_COMMAND", `printf signed >> %1`)
	if signed, err := signWindows(exe, "My App", false); !signed || err != nil {
		t.Fatalf("%v %v", signed, err)
	}
	if b, _ := os.ReadFile(exe); string(b) != "MZsigned" {
		t.Errorf("the command wrote %q", b)
	}
	t.Setenv("ASUKA_WINDOWS_SIGN_COMMAND", `false`)
	if _, err := signWindows(exe, "My App", false); err == nil || !strings.Contains(err.Error(), "%1") {
		t.Errorf("no %%1: %v", err)
	}
}

// A Windows app's manifest runs it as its user (asInvoker): one named
// like an installer would ask for an administrator otherwise.
func TestWindowsManifest(t *testing.T) {
	m := windowsManifest(&asukaApp{name: "update-probe", bundleID: "dev.asuka.updateprobe", cfg: &appConfig{Name: "Update Probe", Version: "1.2.3"}})
	for _, want := range []string{`<requestedExecutionLevel level="asInvoker" uiAccess="false"/>`, `Microsoft.Windows.Common-Controls`, `PerMonitorV2`, `version="1.2.3.1"`} {
		if !strings.Contains(m, want) {
			t.Errorf("no %s in:\n%s", want, m)
		}
	}
}

// What the installer says of the app (its CONFIG), and the MSIX's
// manifest: the app's identity, its scheme and file types, its toasts'
// activator, as the host registers them when it isn't packaged.
func TestWindowsPackageManifests(t *testing.T) {
	cfg := &appConfig{Name: "My Notes", ID: "com.example.notes", Version: "1.4.2", Build: 7, Scheme: "notes",
		FileTypes: []fileTypeConfig{{Name: "Note", Extensions: []string{"note", "NOTE2"}}}}
	cfg.Windows.Publisher = "Example Inc."
	cfg.Windows.Homepage = "https://example.com"
	app := &asukaApp{name: "notes", bundleID: cfg.ID, cfg: cfg}
	clsid := toastActivatorCLSID("com.example.notes")
	if len(clsid) != 38 || clsid[0] != '{' || clsid[15] != '5' {
		t.Errorf("the activator's CLSID %s isn't a name-based (version 5) UUID", clsid)
	}
	conf := string(installerConfig(app, "My Notes.exe"))
	for _, want := range []string{"name=My Notes\n", "version=1.4.2\n", "publisher=Example Inc.\n", "id=com.example.notes\n", "aumid=com.example.notes\n",
		"exe=My Notes.exe\n", "folder=My Notes\n", "uninstaller=Uninstall My Notes.exe\n", "homepage=https://example.com\n", "scheme=notes\n",
		"progids=com.example.notes.note\n", "exts=.note;.note2\n", "activator=" + clsid + "\n", "desktop=0\n"} {
		if !strings.Contains(conf, want) {
			t.Errorf("no %q in the installer's config:\n%s", want, conf)
		}
	}
	m := msixManifest(app, "My Notes.exe", "arm64")
	for _, want := range []string{
		`<Identity Name="com.example.notes" Publisher="CN=Example Inc." Version="1.4.2.0" ProcessorArchitecture="arm64"/>`,
		`<PublisherDisplayName>Example Inc.</PublisherDisplayName>`,
		`<Application Id="App" Executable="My Notes.exe" EntryPoint="Windows.FullTrustApplication">`,
		`<uap:Protocol Name="notes">`,
		`<uap:FileTypeAssociation Name="note">`, `<uap:FileType>.note</uap:FileType>`, `<uap:FileType>.note2</uap:FileType>`,
		`<com:Class Id="` + strings.Trim(clsid, "{}") + `"`, `ToastActivatorCLSID="` + strings.Trim(clsid, "{}") + `"`,
		`<rescap:Capability Name="runFullTrust"/>`,
	} {
		if !strings.Contains(m, want) {
			t.Errorf("no %s in the MSIX's manifest:\n%s", want, m)
		}
	}
	var doc struct{ XMLName xml.Name }
	if err := xml.Unmarshal([]byte(m), &doc); err != nil || doc.XMLName.Local != "Package" {
		t.Errorf("the manifest isn't a Package: %v", err)
	}
	if !strings.Contains(m, `<Resource Language="en-us"/>`) {
		t.Errorf("an app without catalogs isn't in English:\n%s", m)
	}
	// The Store's identity, as Partner Center gives it.
	cfg.Windows.IdentityName, cfg.Windows.PublisherID = "12345Example.Notes", "CN=8A3C5E1B-0000-4000-8000-000000000000"
	if m := msixManifest(app, "My Notes.exe", "amd64"); !strings.Contains(m, `<Identity Name="12345Example.Notes" Publisher="CN=8A3C5E1B-0000-4000-8000-000000000000" Version="1.4.2.0" ProcessorArchitecture="x64"/>`) {
		t.Errorf("the Store's identity:\n%s", m)
	}
}

// An app with catalogs: the installer says its name in the user's
// language (asuka.appName), and the MSIX declares its languages, the
// source's first.
func TestWindowsPackageLanguages(t *testing.T) {
	dir := appWithCatalogs(t, "name = \"Notes\"\nid = \"com.example.notes\"\nversion = \"1.0.0\"", map[string]string{
		"locales/en.json":      `{"a": "A", "asuka.appName": "Notes"}`,
		"locales/de.json":      `{"a": "Ä", "asuka.appName": "Notizen"}`,
		"locales/zh-Hant.json": `{"a": "甲", "asuka.appName": "筆記"}`,
		"locales/ar.json":      `{"a": "أ"}`,
	})
	c, err := loadConfig(dir)
	if err != nil {
		t.Fatal(err)
	}
	app := &asukaApp{name: "notes", bundleID: c.ID, cfg: c}
	conf := string(installerConfig(app, "Notes.exe"))
	for _, want := range []string{"name=Notes\n", "name.en=Notes\n", "name.de=Notizen\n", "name.zh-Hant=筆記\n"} {
		if !strings.Contains(conf, want) {
			t.Errorf("no %q in the installer's config:\n%s", want, conf)
		}
	}
	if strings.Contains(conf, "name.ar=") {
		t.Errorf("a language that doesn't name the app names it:\n%s", conf)
	}
	m := msixManifest(app, "Notes.exe", "arm64")
	want := "<Resources>\n    <Resource Language=\"en\"/>\n    <Resource Language=\"ar\"/>\n    <Resource Language=\"de\"/>\n    <Resource Language=\"zh-Hant\"/>\n  </Resources>"
	if !strings.Contains(m, want) {
		t.Errorf("the MSIX's languages, want\n%s\nin:\n%s", want, m)
	}
}
