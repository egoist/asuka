package windows_test

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// The controls the host draws or composes itself (testdata/controls.asuka):
// filter chips toggled, input chips removed by their remove button and by
// Delete, an action chip pressed; a field's suggestions shown as it's
// typed in, chosen with the arrows and Enter, and a combo box's list
// opened by its chevron and clicked; a date range chosen on its two
// pickers (the end following a start moved past it) and on a calendar
// (two clicks); a disclosure expanded; pictures tinted, prefetched and
// kept out of the caches, the caches cleared. ASUKA_CONTROLS_OUT: a
// directory the window's snapshots are copied to.
func TestControls(t *testing.T) {
	w := onWindows(t)
	local, guest := w.job(t)
	src, err := os.ReadFile(filepath.Join("testdata", "controls.asuka"))
	if err != nil {
		t.Fatal(err)
	}
	controlsAssets(t, local)
	buildApp(t, w, string(src), local)
	d := script(t, w, local, guest, "app.exe",
		"wait:500ms", "snap:start",
		"tap:Wi-Fi", "tap:Breakfast", "snap:chips",
		"remove:Bo", "focus:Cy", "key:delete", "tap:Add me", "snap:removed",
		"tap:Destination", "type:r", "snap:typed",
		"key:down", "key:down", "snap:highlighted", "key:enter", "snap:chose",
		"tap:Size", "dropdown:Size", "snap:combo", "tap:Large", "snap:sized",
		"pickdate:start,2026-12-10", "pickdate:end,2026-12-14", "snap:range",
		"pickdate:start,2026-12-20", "snap:moved",
		"calday:10", "snap:anchored", "calday:13", "snap:calendar",
		"tap:More options", "snap:expanded",
		"scroll:400", "snap:images", "tap:Clear the image cache", "wait:300ms")

	shows(t, d["start"], "the start", []string{"Wanted: false true false", "Guests: 3 presses: 0", "Collapsed", "Loaded: 2"}, "Hidden content")
	for _, s := range []string{`title="Wi-Fi" unselected`, `title="Breakfast" selected`, `title="Ana" removable icon=person`, `title="More options" collapsed`, "suggestions=5", "suggestions=3 dropdown"} {
		if !strings.Contains(d["start"], s) {
			t.Errorf("the start: no %s:\n%s", s, d["start"])
		}
	}
	shows(t, d["chips"], "filter chips toggled", []string{"Wanted: true false false"})
	if !strings.Contains(d["chips"], `title="Wi-Fi" selected`) {
		t.Errorf("Wi-Fi isn't shown selected:\n%s", d["chips"])
	}
	shows(t, d["removed"], "chips removed, one pressed", []string{"Guests: 1 presses: 1"})
	if strings.Contains(d["removed"], `title="Bo"`) || strings.Contains(d["removed"], `title="Cy"`) {
		t.Errorf("Bo and Cy are still there:\n%s", d["removed"])
	}
	if !strings.Contains(d["typed"], `list=["Amsterdam","Paris","Prague","Rome"]`) {
		t.Errorf("typing r doesn't show the cities with an r:\n%s", d["typed"])
	}
	if !strings.Contains(d["highlighted"], "highlighted=1") {
		t.Errorf("two Downs don't highlight Paris:\n%s", d["highlighted"])
	}
	shows(t, d["chose"], "a suggestion chosen", []string{"Chosen: Paris text: Paris"})
	if strings.Contains(d["chose"], "list=[") {
		t.Errorf("the list stayed after the choice:\n%s", d["chose"])
	}
	if !strings.Contains(d["combo"], `list=["Small","Medium","Large"]`) {
		t.Errorf("the chevron doesn't list the sizes:\n%s", d["combo"])
	}
	shows(t, d["sized"], "a size clicked", []string{"Sized: Large"})
	if !strings.Contains(d["range"], "range=2026-12-10..2026-12-14") || !strings.Contains(d["range"], "nights: 4") {
		t.Errorf("the range isn't 10 to 14 December:\n%s", d["range"])
	}
	if !strings.Contains(d["moved"], "range=2026-12-20..2026-12-20") || !strings.Contains(d["moved"], "nights: 0") {
		t.Errorf("a start past the end doesn't move it:\n%s", d["moved"])
	}
	if !strings.Contains(d["anchored"], "anchored") {
		t.Errorf("the calendar's first click doesn't start a range:\n%s", d["anchored"])
	}
	shows(t, d["anchored"], "one click", []string{"Calendar: 0 nights"})
	shows(t, d["calendar"], "two clicks", []string{"Calendar: 3 nights"})
	shows(t, d["expanded"], "the disclosure", []string{"Expanded", "Hidden content"}, "Collapsed")
	if !strings.Contains(d["expanded"], `title="More options" expanded`) {
		t.Errorf("the disclosure isn't shown expanded:\n%s", d["expanded"])
	}
	if out := os.Getenv("ASUKA_CONTROLS_OUT"); out != "" {
		os.MkdirAll(out, 0o755)
		for _, name := range []string{"start", "chips", "removed", "typed.list", "highlighted.list", "combo.list", "range", "calendar", "expanded", "images"} {
			if b, err := os.ReadFile(filepath.Join(local, name+".png")); err == nil {
				os.WriteFile(filepath.Join(out, name+".png"), b, 0o644)
			}
		}
	}
}

// controlsAssets writes the app's assets: examples/trip's pin.png.
func controlsAssets(t *testing.T, local string) {
	t.Helper()
	pin, err := os.ReadFile(filepath.Join("..", "..", "examples", "trip", "assets", "pin.png"))
	if err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(filepath.Join(local, "assets"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(local, "assets", "pin.png"), pin, 0o644); err != nil {
		t.Fatal(err)
	}
}

// The controls right to left, dark: what each draws itself mirrored (a
// chip's glyph and remove button, a disclosure's chevron, a combo box's
// chevron and list), and each still working.
func TestControlsRTLDark(t *testing.T) {
	w := onWindows(t)
	local, guest := w.job(t)
	src, err := os.ReadFile(filepath.Join("testdata", "controls.asuka"))
	if err != nil {
		t.Fatal(err)
	}
	controlsAssets(t, local)
	app := strings.Replace(string(src), "Scroll {\n    grow: 1\n", "Scroll {\n    grow: 1\n    layoutDirection: .rightToLeft\n", 1)
	buildApp(t, w, app, local)
	steps := []string{"resize:800,860", "wait:800ms", "tap:Pool", "remove:Ana", "tap:More options", "tap:Size", "dropdown:Size", "key:down", "snap:rtl"}
	for i, s := range steps {
		if name, ok := strings.CutPrefix(s, "snap:"); ok {
			steps[i] = "snap:" + guest + `\` + name + ".png"
		}
	}
	out, err := w.run(t, guest, guest+`\app.exe`, map[string]string{"ASUKA_SCRIPT": strings.Join(steps, ";"), "ASUKA_DUMP": "1",
		"ASUKA_SNAPSHOT": guest + `\end.png`, "ASUKA_COLOR_SCHEME": "dark"})
	dump, rerr := os.ReadFile(filepath.Join(local, "rtl.png.txt"))
	if err != nil || rerr != nil {
		t.Fatalf("%v %v\n%s", err, rerr, out)
	}
	if strings.Contains(out, "automation: no") {
		t.Errorf("the script missed a step:\n%s", out)
	}
	shows(t, string(dump), "right to left", []string{"Wanted: false true true", "Guests: 2 presses: 0", "Expanded", "Hidden content", "Loaded: 2"})
	if !strings.Contains(string(dump), `list=["Small","Medium","Large"] highlighted=0`) {
		t.Errorf("the combo box's list isn't open, its first item highlighted:\n%s", dump)
	}
	if o := os.Getenv("ASUKA_CONTROLS_OUT"); o != "" {
		os.MkdirAll(o, 0o755)
		for _, name := range []string{"rtl", "rtl.list"} {
			if b, err := os.ReadFile(filepath.Join(local, name+".png")); err == nil {
				os.WriteFile(filepath.Join(o, name+"-dark.png"), b, 0o644)
			}
		}
	}
}

// The controls as screen readers find them (UI Automation, in the VM): a
// filter chip a button with the Toggle pattern, an input chip's remove
// button a button named with the word remove and the title (Invoke), an
// action chip a button that invokes, the disclosure a button that
// expands and collapses; each pattern changes the app as the mouse does.
func TestControlsAccessibility(t *testing.T) {
	w := onWindows(t)
	if w.vm == "" && os.Getenv("ASUKA_UIA") == "" {
		t.Skip("reads the desktop's UI Automation tree: in the VM, or with ASUKA_UIA set")
	}
	local, guest := w.job(t)
	src, err := os.ReadFile(filepath.Join("testdata", "controls.asuka"))
	if err != nil {
		t.Fatal(err)
	}
	controlsAssets(t, local)
	buildApp(t, w, string(src), local)
	if err := os.WriteFile(filepath.Join(local, "uia.ps1"), []byte(controlsUIAScript), 0o644); err != nil {
		t.Fatal(err)
	}
	out, err := w.run(t, guest, `C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe`, map[string]string{
		"ASUKA_SCRIPT":   "waitfor:" + guest + `\uia.done`,
		"ASUKA_SNAPSHOT": guest + `\end.png`,
		"ASUKA_DUMP":     "1",
	}, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", guest+`\uia.ps1`, guest)
	if err != nil {
		t.Fatalf("%v\n%s", err, out)
	}
	for _, want := range []string{
		"Button|Wi-Fi|toggle Off",
		"Button|Breakfast|toggle On",
		"Button|Remove Bo|invoke",
		"Button|Add me|invoke",
		"Button|More options|expand Collapsed",
		"toggled Wi-Fi", "expanded More options", "invoked Remove Bo",
	} {
		if !strings.Contains(out, want) {
			t.Errorf("no %q in the tree:\n%s", want, out)
		}
	}
	dump, _ := os.ReadFile(filepath.Join(local, "end.png.txt"))
	shows(t, string(dump), "after the patterns", []string{"Wanted: true true false", "Expanded", "Guests: 2 presses: 0"})
}

// controlsUIAScript starts the app, prints each element under its window
// (control type|name|its Toggle, ExpandCollapse and Invoke patterns),
// toggles Wi-Fi, expands More options and invokes Remove Bo, then lets the
// app end (uia.done) and waits for it.
const controlsUIAScript = `
param([string]$dir)
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
$p = Start-Process -FilePath "$dir\app.exe" -WorkingDirectory $dir -PassThru -RedirectStandardOutput "$dir\app.txt" -RedirectStandardError "$dir\app.err.txt" -WindowStyle Normal
$h = [IntPtr]::Zero
for ($i = 0; $i -lt 100 -and $h -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 200; $p.Refresh(); $h = $p.MainWindowHandle }
if ($h -eq [IntPtr]::Zero) { "no window"; Set-Content "$dir\uia.done" "x"; $p.WaitForExit(); exit 1 }
Start-Sleep -Milliseconds 1500
$root = [System.Windows.Automation.AutomationElement]::FromHandle($h)
$all = $root.FindAll([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
$acts = @()
foreach ($e in $all) {
  $c = $e.Current
  $type = $c.ControlType.ProgrammaticName -replace '^ControlType\.', ''
  $line = "$type|$($c.Name)|"
  $o = $null
  if ($e.TryGetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern, [ref]$o)) { $line += "toggle $($o.Current.ToggleState) " }
  if ($e.TryGetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern, [ref]$o)) { $line += "expand $($o.Current.ExpandCollapseState) " }
  if ($e.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$o)) { $line += "invoke " }
  $line
  if ($c.Name -eq 'Wi-Fi' -or $c.Name -eq 'More options' -or $c.Name -eq 'Remove Bo') { $acts += $e }
}
foreach ($e in $acts) {
  $n = $e.Current.Name
  $o = $null
  if ($n -eq 'Wi-Fi' -and $e.TryGetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern, [ref]$o)) { $o.Toggle(); "toggled $n" }
  if ($n -eq 'More options' -and $e.TryGetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern, [ref]$o)) { $o.Expand(); "expanded $n" }
  if ($n -eq 'Remove Bo' -and $e.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$o)) { $o.Invoke(); "invoked $n" }
  Start-Sleep -Milliseconds 400
}
Start-Sleep -Milliseconds 800
Set-Content "$dir\uia.done" "x"
$p.WaitForExit()
Get-Content "$dir\app.err.txt"
exit 0
`
