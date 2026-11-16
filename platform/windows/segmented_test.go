package windows_test

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/egoist/asuka/internal/appbuild"
	"github.com/egoist/asuka/std"
)

// A native package's view in an app: examples/media's segmented control
// (its desktop/'s C, a tab control drawn as buttons), with the standard
// library's packages the example imports. A click on a button changes
// the app's state through the package's changed event.
func TestSegmented(t *testing.T) {
	w := onWindows(t)
	zig(t)
	local, guest := w.job(t)
	p, err := appbuild.Load(filepath.Join("..", "..", "examples", "media"))
	if err != nil {
		t.Fatal(err)
	}
	m, diags, err := p.Compile("windows")
	if err != nil || m == nil {
		t.Fatalf("media doesn't compile: %v\n%s", err, strings.Join(diags, "\n"))
	}
	c, err := appbuild.AppC(appbuild.StripTests(m))
	if err != nil {
		t.Fatal(err)
	}
	dirs, err := std.Glob("desktop")
	if err != nil {
		t.Fatal(err)
	}
	for i := range dirs {
		dirs[i] = filepath.Dir(dirs[i])
	}
	seg, _ := filepath.Abs(filepath.Join("..", "..", "examples", "media", "segmented"))
	natives, err := appbuild.DesktopNatives(target(w), append(dirs, seg))
	if err != nil {
		t.Fatal(err)
	}
	if err := appbuild.WindowsHost(target(w), appbuild.WindowsOptions{App: []string{c}, Native: natives, Console: true}, filepath.Join(local, "media.exe")); err != nil {
		t.Fatal(err)
	}
	// The app goes to its native view, then waits while a client in the
	// VM clicks the control's first button (Day), as the user does: its
	// window found through UI Automation, the click the mouse's (the
	// automation's tap reaches the core's views, not inside a package's
	// widget).
	if err := os.WriteFile(filepath.Join(local, "seg.ps1"), []byte(segmentedScript), 0o644); err != nil {
		t.Fatal(err)
	}
	out, err := w.run(t, guest, `C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe`, map[string]string{
		"ASUKA_SCRIPT":   "tap:Native view;wait:500ms;snap:" + guest + `\shown.png;waitfor:` + guest + `\seg.done;wait:300ms`,
		"ASUKA_SNAPSHOT": guest + `\end.png`,
		"ASUKA_DUMP":     "1",
	}, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", guest+`\seg.ps1`, guest)
	t.Logf("%s", out)
	if err != nil {
		t.Fatal(err)
	}
	shown, _ := os.ReadFile(filepath.Join(local, "shown.png.txt"))
	end, _ := os.ReadFile(filepath.Join(local, "end.png.txt"))
	shows(t, string(shown), "the native view", []string{"Selected: Week"})
	shows(t, string(end), "after a click on Day", []string{"Selected: Day"}, "Selected: Week")
	if !strings.Contains(out, "clicked Day") {
		t.Error("no tab control in the window's UI Automation tree")
	}
}

// segmentedScript starts the app (media.exe in the job, args[0]), waits
// for its native view's tab control and the app's snapshot of it, clicks
// its first button with the mouse, then lets the app end (seg.done) and waits for it.
const segmentedScript = `
param([string]$dir)
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
$p = Start-Process -FilePath "$dir\media.exe" -WorkingDirectory $dir -PassThru -RedirectStandardOutput "$dir\app.txt" -RedirectStandardError "$dir\app.err.txt" -WindowStyle Normal
$h = [IntPtr]::Zero
for ($i = 0; $i -lt 100 -and $h -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 200; $p.Refresh(); $h = $p.MainWindowHandle }
if ($h -eq [IntPtr]::Zero) { "no window"; Set-Content "$dir\seg.done" "x"; $p.WaitForExit(); exit 1 }
$root = [System.Windows.Automation.AutomationElement]::FromHandle($h)
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class Mouse {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint x, uint y, uint d, UIntPtr e);
}
"@
$class = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ClassNameProperty, "SysTabControl32")
$e = $null
for ($i = 0; $i -lt 50 -and $e -eq $null; $i++) { Start-Sleep -Milliseconds 200; $e = $root.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $class) }
# After the app's snapshot of its view as it opened.
for ($i = 0; $i -lt 100 -and -not (Test-Path "$dir\shown.png.txt"); $i++) { Start-Sleep -Milliseconds 100 }
if ($e -ne $null) {
  [Mouse]::SetForegroundWindow($h) | Out-Null
  Start-Sleep -Milliseconds 300
  $r = $e.Current.BoundingRectangle
  [Mouse]::SetCursorPos([int]($r.Left + 6), [int]($r.Top + $r.Height / 2)) | Out-Null
  [Mouse]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero)
  [Mouse]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)
  "clicked Day"
} else { "no control" }
Start-Sleep -Milliseconds 500
Set-Content "$dir\seg.done" "x"
$p.WaitForExit()
Get-Content "$dir\app.err.txt"
exit 0
`
