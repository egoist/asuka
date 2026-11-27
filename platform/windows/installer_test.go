package windows_test

import (
	"bytes"
	"encoding/json"
	"os"
	"os/exec"
	"path/filepath"
	"slices"
	"strings"
	"testing"
	"time"

	"github.com/egoist/asuka/internal/l10n"
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

// appNames are TestInstaller's app's names in its catalogs.
var appNames = map[string]string{"en": "Counter", "de": "Zähler", "ja": "カウンター", "ar": "العداد"}

// shownDialog is what a dialog of a program's showed: its title, its
// texts and buttons, whether it's laid out right to left, and the
// program's exit code once its cancel button was pressed.
type shownDialog struct {
	Title string
	Texts []string
	RTL   bool
	Code  int
}

// dialog runs exe with /LANG=lang (none: the user's language) in the
// signed-in session, reads the dialog it shows through UI Automation,
// saves a picture of it in dir (<name>-<lang>.png), and presses its
// button named cancel. With app, that runs first (and is ended after):
// the app the dialog is about, open. more are exe's other arguments.
func (w *windows) dialog(t *testing.T, dir, name, exe, app, lang, cancel string, more ...string) shownDialog {
	t.Helper()
	base := name + "-" + lang
	if lang == "" {
		base = name
	}
	open := "$app = $null"
	if app != "" {
		open = "$app = Start-Process -FilePath " + psQuote(app) + " -PassThru\n" +
			"for ($i = 0; $i -lt 80 -and $app.MainWindowHandle -eq 0; $i++) { Start-Sleep -Milliseconds 250; $app.Refresh() }"
	}
	if lang != "" {
		more = append([]string{"/LANG=" + lang}, more...)
	}
	args := ""
	if len(more) > 0 {
		quoted := make([]string, len(more))
		for i, a := range more {
			quoted[i] = psQuote(a)
		}
		args = " -ArgumentList " + strings.Join(quoted, ", ")
	}
	script := `$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes, System.Drawing
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class U {
  [DllImport("user32.dll")] public static extern int GetWindowLongW(IntPtr h, int i);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint x, uint y, uint d, UIntPtr e);
}
'@
[U]::SetProcessDPIAware() | Out-Null
$A = [System.Windows.Automation.AutomationElement]
$r = [ordered]@{Title = ''; Texts = @(); RTL = $false; Code = -1}
` + open + `
$p = Start-Process -FilePath ` + psQuote(exe) + args + ` -PassThru
try {
  $win = $null
  for ($i = 0; $i -lt 120 -and -not $win; $i++) {
    Start-Sleep -Milliseconds 250
    $win = $A::RootElement.FindFirst([System.Windows.Automation.TreeScope]::Children, (New-Object System.Windows.Automation.PropertyCondition($A::ProcessIdProperty, $p.Id)))
  }
  if (-not $win) { throw 'no dialog' }
  # Until its buttons are there.
  $cancel = $null
  for ($i = 0; $i -lt 80 -and -not $cancel; $i++) {
    Start-Sleep -Milliseconds 250
    $all = $win.FindAll([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
    $cancel = $all | Where-Object { $_.Current.Name -eq ` + psQuote(cancel) + ` -and $_.Current.ClassName -eq 'CCPushButton' } | Select-Object -First 1
  }
  $h = [IntPtr]$win.Current.NativeWindowHandle
  [U]::SetForegroundWindow($h) | Out-Null
  $r.Title = $win.Current.Name
  $r.RTL = ([U]::GetWindowLongW($h, -20) -band 0x400000) -ne 0
  $r.Texts = @($all | ForEach-Object { [string]$_.Current.Name } | Where-Object { $_ })
  if (-not $cancel) { throw 'no button ' + ` + psQuote(cancel) + ` }
  Start-Sleep -Milliseconds 500
  $b = $win.Current.BoundingRectangle
  $bmp = New-Object System.Drawing.Bitmap ([int]$b.Width), ([int]$b.Height)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen([int]$b.X, [int]$b.Y, 0, 0, $bmp.Size)
  $bmp.Save(` + psQuote(dir+`\`+base+`.png`) + `, [System.Drawing.Imaging.ImageFormat]::Png)
  $g.Dispose(); $bmp.Dispose()
  # Clicked, as the user would (the task dialog's buttons have no Invoke).
  $c = $cancel.Current.BoundingRectangle
  [U]::SetCursorPos([int]($c.X + $c.Width / 2), [int]($c.Y + $c.Height / 2)) | Out-Null
  [U]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero); [U]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)
  if (-not $p.WaitForExit(20000)) { throw 'it went on' }
  $r.Code = $p.ExitCode
} finally {
  if (-not $p.HasExited) { $p.Kill() }
  if ($app -and -not $app.HasExited) { $app.Kill() }
  $r | ConvertTo-Json -Compress | Set-Content -Encoding UTF8 ` + psQuote(dir+`\`+base+`.json`) + `
}
`
	// PowerShell 5 reads a script without a BOM as the system's code page.
	if err := os.WriteFile(w.local(dir+`\`+base+`.ps1`), append([]byte("\xef\xbb\xbf"), script...), 0o644); err != nil {
		t.Fatal(err)
	}
	out, err := w.run(t, dir, "powershell.exe", nil, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", dir+`\`+base+`.ps1`)
	b, rerr := os.ReadFile(w.local(dir + `\` + base + `.json`))
	if rerr != nil {
		t.Fatalf("the dialog of %s /LANG=%s: %v %v\n%s", exe, lang, err, rerr, out)
	}
	var d shownDialog
	if err := json.Unmarshal(bytes.TrimPrefix(b, []byte("\xef\xbb\xbf")), &d); err != nil {
		t.Fatalf("%v\n%s\n%s", err, b, out)
	}
	if err != nil {
		t.Errorf("the dialog of %s /LANG=%s: %v\n%s", exe, lang, err, out)
	}
	return d
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
	// 1.0.1 has catalogs, which name the app in German, Japanese and
	// Arabic (asuka.appName).
	translated := filepath.Join(local, "counter")
	copyTree(t, filepath.Join(root, "examples", "counter"), translated)
	for tag, n := range appNames {
		os.MkdirAll(filepath.Join(translated, "locales"), 0o755)
		if err := os.WriteFile(filepath.Join(translated, "locales", tag+".json"), []byte(`{"asuka.appName": "`+n+`"}`), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	setup := map[string]string{}
	for _, v := range []string{"1.0.0", "1.0.1"} {
		out := filepath.Join(local, v)
		src := "examples/counter"
		if v == "1.0.1" {
			src = translated
		}
		c := exec.Command("go", "run", "./cmd/asuka", "build", "--platform", "windows", "--arch", w.arch, "--version", v, "--package", "installer", "-o", out, src)
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

	// The uninstaller asks in the user's language (the VM's, English), or
	// /LANG='s (pt-BR: Asuka's pt), the app's name its catalogs' in it
	// (most have none: asuka.toml's), right to left in Arabic and Hebrew;
	// Cancel cancels.
	for _, tag := range []string{"", "de", "ja", "zh-Hans", "zh-Hant", "ko", "fr", "es", "pt-BR", "it", "ru", "ar", "he"} {
		app := appNames[tag]
		if app == "" {
			app = name
		}
		rtl := tag == "ar" || tag == "he"
		word := func(key string) string {
			// Isolates said as embeddings, as the installer says them
			// (the values here are left to right), and a path in
			// right-to-left text unbroken at its backslashes.
			folder := dir
			if rtl {
				folder = strings.ReplaceAll(dir, `\`, "\ufeff\\")
			}
			return strings.NewReplacer("\u2066", "\u202a", "\u2067", "\u202b", "\u2068", "\u202a", "\u2069", "\u202c").
				Replace(l10n.Catalog.Format(tag, key, map[string]any{"app": app, "folder": folder}))
		}
		d := w.dialog(t, guest, "uninstall", dir+`\Uninstall Counter.exe`, "", tag, word("cancel"))
		if d.Title != app || !slices.Contains(d.Texts, word("installerUninstallAsk")) || !slices.Contains(d.Texts, word("installerUninstallAskDetail")) ||
			!slices.Contains(d.Texts, word("installerUninstall")) {
			t.Errorf("%q: the uninstaller asks %+v, want %q, %q", tag, d, word("installerUninstallAsk"), word("installerUninstallAskDetail"))
		}
		if d.RTL != rtl {
			t.Errorf("%q: right to left %v", tag, d.RTL)
		}
		if d.Code != 2 {
			t.Errorf("%q: canceled, the uninstaller exits %d", tag, d.Code)
		}
		if r := w.installed(t, name, id); r.Version != "1.0.1" {
			t.Errorf("%q: canceling uninstalled it: %+v", tag, r)
		}
	}
	// The installer where the user can't write: it says what failed in
	// French, and why (the system's words).
	fr := func(key string) string {
		return l10n.Catalog.Format("fr", key, map[string]any{"app": name, "path": `C:\Windows\Counter`})
	}
	failed := w.dialog(t, guest, "setup-error", setup["1.0.1"], "", "fr", fr("close"), `/D=C:\Windows\Counter`)
	why := false
	for _, s := range failed.Texts {
		why = why || strings.HasPrefix(s, fr("installerCantWrite")+"\n") && len(s) > len(fr("installerCantWrite"))+1
	}
	if failed.Title != name || !slices.Contains(failed.Texts, fr("installerInstallFailed")) || !why || failed.Code != 1 {
		t.Errorf("setup into C:\\Windows: %+v, want %q, and %q and why", failed, fr("installerInstallFailed"), fr("installerCantWrite"))
	}

	// The installer, the app open: it asks to close it, in Japanese.
	ja := func(key string) string { return l10n.Catalog.Format("ja", key, map[string]any{"app": appNames["ja"]}) }
	d := w.dialog(t, guest, "setup", setup["1.0.1"], dir+`\Counter.exe`, "ja", ja("cancel"))
	if d.Title != appNames["ja"] || !slices.Contains(d.Texts, ja("installerAppOpen")) || !slices.Contains(d.Texts, ja("installerAppOpenDetail")) ||
		!slices.Contains(d.Texts, ja("installerCloseApp")) || d.Code != 2 {
		t.Errorf("setup, the app open: %+v, want %q, %q, %q and exit 2", d, ja("installerAppOpen"), ja("installerAppOpenDetail"), ja("installerCloseApp"))
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
