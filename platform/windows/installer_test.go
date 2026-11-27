package windows_test

import (
	"encoding/json"
	"os"
	"os/exec"
	"path/filepath"
	"slices"
	"strings"
	"testing"
	"time"
)

// ps runs a PowerShell script on Windows (in the VM: over its agent, not
// in the signed-in session) and returns its output.
func (w *windows) ps(t *testing.T, script string) string {
	t.Helper()
	var cmd *exec.Cmd
	if w.vm == "" {
		cmd = exec.Command("powershell", "-NoProfile", "-Command", script)
	} else {
		cmd = exec.Command("swiftvm", "exec", w.vm, "--", script)
	}
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("powershell: %v\n%s\n%s", err, script, out)
	}
	return strings.TrimSpace(string(out))
}

// installed is what Windows says of an app the installer installed: its
// folder's entries, its Installed apps entry, its Start menu shortcut.
type installed struct {
	Files     []string
	Version   string
	Location  string
	Uninstall string
	Quiet     string
	Shortcut  string
	AppID     string
}

func (w *windows) installed(t *testing.T, name, id string) installed {
	t.Helper()
	out := w.ps(t, `$dir = Join-Path $env:LOCALAPPDATA 'Programs\`+name+`'
$key = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\`+id+`'
$lnk = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\`+name+`.lnk'
$r = [ordered]@{Files = @(); Version = ''; Location = ''; Uninstall = ''; Quiet = ''; Shortcut = ''; AppID = ''}
if (Test-Path $dir) { $r.Files = @(Get-ChildItem -Force -Name $dir | Sort-Object | ForEach-Object { [string]$_ }) }
if (Test-Path $key) {
  $k = Get-ItemProperty $key
  $r.Version = $k.DisplayVersion; $r.Location = $k.InstallLocation; $r.Uninstall = $k.UninstallString; $r.Quiet = $k.QuietUninstallString
}
if (Test-Path $lnk) { $r.Shortcut = (New-Object -ComObject WScript.Shell).CreateShortcut($lnk).TargetPath }
$a = Get-StartApps | Where-Object { $_.Name -eq '`+name+`' } | Select-Object -First 1
if ($a) { $r.AppID = $a.AppID }
$r | ConvertTo-Json -Compress`)
	var r installed
	if err := json.Unmarshal([]byte(out[strings.Index(out, "{"):]), &r); err != nil {
		t.Fatalf("%v\n%s", err, out)
	}
	slices.Sort(r.Files)
	return r
}

// The installer asuka build makes (--package installer), as a user runs
// it, silently (/S, as electron-builder's): Counter 1.0.0 installed for
// the user, in %LOCALAPPDATA%\Programs\Counter, listed in Installed apps
// and the Start menu (its AppUserModelID the app's), and run from there;
// 1.0.1's installer updating it in place; its uninstaller taking it all
// away.
func TestInstaller(t *testing.T) {
	w := onWindows(t)
	zig(t)
	local, guest := w.job(t)
	root, err := filepath.Abs(filepath.Join("..", ".."))
	if err != nil {
		t.Fatal(err)
	}
	const name, id = "Counter", "dev.asuka.counter"
	if r := w.installed(t, name, id); r.Location != "" || len(r.Files) > 0 {
		t.Skipf("Counter is installed in the VM already (%s): uninstall it first", r.Location)
	}
	setup := map[string]string{}
	for _, v := range []string{"1.0.0", "1.0.1"} {
		out := filepath.Join(local, v)
		c := exec.Command("go", "run", "./cmd/asuka", "build", "--platform", "windows", "--arch", w.arch, "--version", v, "--package", "installer", "-o", out, "examples/counter")
		c.Dir, c.Env = root, append(os.Environ(), "ASUKA_WINDOWS_CERT=")
		if b, err := c.CombinedOutput(); err != nil {
			t.Fatalf("asuka build %s: %v\n%s", v, err, b)
		}
		setup[v] = guest + `\` + v + `\windows-` + w.arch + `\counter-` + v + `-windows-` + w.arch + `-setup.exe`
		if _, err := os.Stat(w.local(setup[v])); err != nil {
			t.Fatal(err)
		}
	}
	log := guest + `\setup.log`
	defer func() {
		// Whatever the test left.
		w.ps(t, `$u = Join-Path $env:LOCALAPPDATA 'Programs\Counter\Uninstall Counter.exe'; if (Test-Path $u) { Start-Process $u -ArgumentList '/S' -Wait }`)
		if t.Failed() {
			b, _ := os.ReadFile(w.local(log))
			t.Logf("the installer's log:\n%s", b)
		}
	}()

	if out, err := w.run(t, guest, setup["1.0.0"], nil, "/S", "/LOG="+log); err != nil {
		t.Fatalf("setup 1.0.0: %v\n%s", err, out)
	}
	// The Start menu lists the shortcut once the shell has seen it.
	var r installed
	for i := 0; i < 20; i++ {
		if r = w.installed(t, name, id); r.AppID != "" {
			break
		}
		time.Sleep(500 * time.Millisecond)
	}
	dir := r.Location
	if !strings.HasSuffix(strings.ToLower(dir), `\appdata\local\programs\counter`) || r.Version != "1.0.0" {
		t.Errorf("Installed apps says %+v", r)
	}
	if want := []string{".asuka-files", "Counter.exe", "Uninstall Counter.exe", "asuka-app.json"}; strings.Join(r.Files, ",") != strings.Join(want, ",") {
		t.Errorf("the folder has %v, want %v", r.Files, want)
	}
	if r.Uninstall != `"`+dir+`\Uninstall Counter.exe"` || r.Quiet != r.Uninstall+" /S" {
		t.Errorf("uninstall %q, quietly %q", r.Uninstall, r.Quiet)
	}
	if r.Shortcut != dir+`\Counter.exe` || r.AppID != id {
		t.Errorf("the Start menu's shortcut: %q, its app ID %q", r.Shortcut, r.AppID)
	}
	list := w.ps(t, `Get-Content (Join-Path $env:LOCALAPPDATA 'Programs\Counter\.asuka-files')`)
	if strings.Join(strings.Fields(list), ",") != "Counter.exe,asuka-app.json" {
		t.Errorf("the file list (the updater's) names %q", list)
	}

	// The app, from where it's installed.
	out, err := w.run(t, guest, dir+`\Counter.exe`, map[string]string{"ASUKA_SCRIPT": "tap:Add", "ASUKA_SNAPSHOT": guest + `\c.png`, "ASUKA_DUMP": "1"})
	dump, rerr := os.ReadFile(filepath.Join(local, "c.png.txt"))
	if err != nil || rerr != nil || !strings.Contains(string(dump), `"Count: 1"`) {
		t.Errorf("the installed app: %v %v\n%s\n%s", err, rerr, dump, out)
	}

	// 1.0.1 over it.
	if out, err := w.run(t, guest, setup["1.0.1"], nil, "/S", "/LOG="+log); err != nil {
		t.Fatalf("setup 1.0.1: %v\n%s", err, out)
	}
	if r := w.installed(t, name, id); r.Version != "1.0.1" || r.Location != dir || len(r.Files) != 4 {
		t.Errorf("after 1.0.1's setup: %+v", r)
	}
	if v := w.ps(t, `(Get-Content (Join-Path $env:LOCALAPPDATA 'Programs\Counter\asuka-app.json') | ConvertFrom-Json).version`); v != "1.0.1" {
		t.Errorf("the app's settings say %q", v)
	}

	// Its uninstaller, as Settings runs it quietly.
	if out, err := w.run(t, guest, dir+`\Uninstall Counter.exe`, nil, "/S", "/LOG="+log); err != nil {
		t.Fatalf("uninstall: %v\n%s", err, out)
	}
	// The copy of it that did the work removes it, and the folder, once
	// it has ended; the Start menu forgets the app a moment after.
	var after installed
	for i := 0; i < 20; i++ {
		if after = w.installed(t, name, id); len(after.Files) == 0 && after.AppID == "" {
			break
		}
		time.Sleep(500 * time.Millisecond)
	}
	if after.Location != "" || after.Shortcut != "" || after.AppID != "" || len(after.Files) > 0 {
		t.Errorf("after uninstalling: %+v", after)
	}
}
